/*
 * IRC - Internet Relay Chat, modules/services/history/hist_search.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/** @file
 * @brief SEARCH: finding something that was said.
 *
 * The index has been there since v1 of the schema -- GIN over
 * @c to_tsvector('simple', body) -- and this is what uses it.  There is
 * no IRCv3 specification for searching, so the command and its capability
 * are vendored (@c blacknode/search): what comes back is an ordinary
 * batch of ordinary messages, replayed exactly the way CHATHISTORY
 * replays them, so a client that can read one can read this.
 *
 * Three things decide the shape of it.
 *
 * **What you may search is what you may read, asked when you ask it.**
 * The scope is the channels this client is on *now* and the conversations
 * it is in -- the same two answers hist_read.c gives, arrived at the same
 * way.  There is no scope that means "the network", and a client with
 * neither channels nor an account is told there is nowhere to look rather
 * than being given an empty answer that looks like "nothing matched".
 *
 * **The text goes first in the WHERE clause.**  It is the only condition
 * the index can answer; narrowing by channel first would read every
 * message that channel has ever had and test each one, which is the
 * difference between a search and a table scan.
 *
 * **The query is @c websearch_to_tsquery and not @c to_tsquery.**  What
 * arrives is what a person typed, and to_tsquery raises an error on
 * anything that is not already an expression -- an apostrophe, a stray
 * bracket, two words with no operator between them.  websearch_ takes
 * prose, quoted phrases, @c or and a leading @c - for "not", and cannot
 * be made to fail, which is what a field somebody types into needs.
 */
#include "config.h"

#include "batch.h"
#include "capab.h"
#include "channel.h"
#include "client.h"
#include "hash.h"
#include "history.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_i18n.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "msg.h"
#include "msg_tag.h"
#include "numnicks.h"
#include "s_user.h"
#include "send.h"
#include "struct.h"

#include <stdlib.h>
#include <string.h>

/** Room for the channel list a search is scoped to.
 *
 * A client can be on at most MAXCHANNELSPERUSER channels, so this is
 * bounded by the configuration rather than by a number invented here; a
 * client that somehow has more than fits is told to name one, which is
 * an answer, rather than being searched over a truncated list, which is
 * a wrong answer.
 */
#define HIST_SCOPE_MAX 4096

/** One search waiting on the store. */
struct HistSeek {
  char hs_numnick[10];            /**< Who asked. */
  time_t hs_born;                 /**< When they connected. */
  char hs_target[CHANNELLEN + 1]; /**< What they searched, for the batch. */
};

/** The client that asked, or NULL if it is not there any more. */
static struct Client* hist_seeker(const struct HistSeek* sk)
{
  struct Client* cptr = findNUser(sk->hs_numnick);

  if (!cptr || !MyUser(cptr) || cli_firsttime(cptr) != sk->hs_born)
    return 0;

  return cptr;
}

/** Refuse, in the terms the standard replies use. */
static void hist_search_fail(struct Client* cptr, const char* code,
                             const char* context, const char* text)
{
  send_fail(cptr, MSG_SEARCH, code, context, _(cptr, text));
}

/** Add one channel to a PostgreSQL text array literal.
 *
 * Quoted, with @c " and @c \ escaped.  A channel name may hold either --
 * @c \ is the upper case of @c | in IRC -- and an array literal built by
 * pasting names together would end up meaning something else for exactly
 * those names.  The value is a parameter, so nothing here can reach the
 * statement; what is being protected is the literal's own syntax.
 *
 * @return Non-zero if it fitted.
 */
static int hist_scope_add(char* buf, size_t buflen, size_t* at,
                          const char* name)
{
  size_t i;
  size_t n = *at;

  if (n + 3 >= buflen)
    return 0;

  if (n > 1)
    buf[n++] = ',';

  buf[n++] = '"';

  for (i = 0; name[i]; i++) {
    if (n + 3 >= buflen)
      return 0;

    if (name[i] == '"' || name[i] == '\\')
      buf[n++] = '\\';

    buf[n++] = name[i];
  }

  buf[n++] = '"';
  buf[n] = '\0';
  *at = n;

  return 1;
}

/** Build the array of channels this client may search.
 *
 * @param[in] sptr Who is asking.
 * @param[in] one One channel only, or NULL for every one they are on.
 * @param[out] buf Where to write the literal.
 * @param[in] buflen Size of \a buf.
 * @return How many channels went in, or -1 if they did not fit.
 */
static int hist_scope_channels(struct Client* sptr, struct Channel* one,
                               char* buf, size_t buflen)
{
  struct Membership* chan;
  char canon[CHANNELLEN + 1];
  size_t at = 1;
  int count = 0;

  buf[0] = '{';
  buf[1] = '\0';

  for (chan = cli_user(sptr)->channel; chan; chan = chan->next_channel) {
    if (one && chan->channel != one)
      continue;

    hist_canon(canon, sizeof(canon), chan->channel->chname);

    if (!hist_scope_add(buf, buflen - 1, &at, canon))
      return -1;

    count++;
  }

  buf[at++] = '}';
  buf[at] = '\0';

  return count;
}

/** Send what a search came back with.
 *
 * The messages as they were sent, under the names and the times they had,
 * inside a batch of this command's own type.  The target of each one is
 * the one it was sent to and not the one that was searched: a search over
 * everything answers with messages from several places, and a client has
 * to be able to tell which is which.
 */
static void hist_search_send(struct Client* cptr, const struct HistSeek* sk,
                             const struct HistRow* rows, unsigned int count)
{
  unsigned int i;

  if (!batch_out_open(cptr, HIST_SEARCH_BATCH, sk->hs_target))
    return;

  for (i = 0; i < count; i++) {
    const char* cmd;
    const char* tok;

    switch (rows[i].hr_kind) {
    case HIST_NOTICE: cmd = MSG_NOTICE;  tok = TOK_NOTICE;  break;
    case HIST_TAGMSG: cmd = MSG_TAGMSG;  tok = TOK_TAGMSG;  break;
    default:          cmd = MSG_PRIVATE; tok = TOK_PRIVATE; break;
    }

    hist_replay_begin(tok, &rows[i]);

    if (rows[i].hr_kind == HIST_TAGMSG)
      sendrawto_one_tagged(cptr, tok, ":%s %s %s", rows[i].hr_prefix, cmd,
                           rows[i].hr_target);
    else
      sendrawto_one_tagged(cptr, tok, ":%s %s %s :%s", rows[i].hr_prefix, cmd,
                           rows[i].hr_target, rows[i].hr_body);

    msg_tag_line_replay_end();
  }

  batch_out_close(cptr);
}

/** What the store came back with. */
static void hist_search_answer(int ok, const struct HistRow* rows,
                               unsigned int count, void* user)
{
  struct HistSeek* sk = (struct HistSeek*) user;
  struct Client* cptr = hist_seeker(sk);

  if (!cptr) {
    MyFree(sk);
    return;
  }

  if (!ok) {
    hist_search_fail(cptr, "MESSAGE_ERROR", sk->hs_target,
                     N_("The history is not available right now"));
    MyFree(sk);
    return;
  }

  /* An empty batch and not silence: "nothing matched" is a different
   * thing from "the server ignored you", and a client waiting for the
   * closing line would wait for ever. */
  hist_search_send(cptr, sk, rows, count);

  MyFree(sk);
}

/** Read one @c key=value argument into \a s.
 * @return Zero if the key is not one of them.
 */
static int hist_search_option(struct HistSearch* s, const char* word)
{
  if (!ircd_strncmp(word, "from=", 5)) {
    hist_canon(s->hs_from, sizeof(s->hs_from), word + 5);
    return 1;
  }

  if (!ircd_strncmp(word, "after=", 6)) {
    ircd_strncpy(s->hs_after, word + 6, sizeof(s->hs_after) - 1);
    return 1;
  }

  if (!ircd_strncmp(word, "before=", 7)) {
    ircd_strncpy(s->hs_before, word + 7, sizeof(s->hs_before) - 1);
    return 1;
  }

  if (!ircd_strncmp(word, "limit=", 6)) {
    int max = feature_int(FEAT_HISTORY_MAX_LIMIT);
    int n = atoi(word + 6);

    if (max <= 0)
      max = 1;

    /* Clamped rather than refused, for hist_read.c's reason: the limit is
     * the server's, and a client that had to discover it by being told no
     * would only ask again with a smaller number. */
    s->hs_limit = (unsigned int) ((n <= 0 || n > max) ? max : n);
    return 1;
  }

  return 0;
}

/** SEARCH from a client.
 *
 * @verbatim
 *   SEARCH <target|*> [from=<nick>] [after=<time>] [before=<time>]
 *          [limit=<n>] :<what to look for>
 * @endverbatim
 *
 * What is being looked for is the **last** parameter and not a
 * @c key=value like the rest, because it is the one thing here that is a
 * sentence: as the trailing parameter it may hold spaces, quotes and
 * anything else somebody types, and IRC has put the one free-form
 * argument last since PRIVMSG.
 *
 * @param[in] cptr Connection it arrived on.
 * @param[in] sptr Who sent it.
 * @param[in] parc Parameter count.
 * @param[in] parv Parameters.
 * @return Zero.
 */
int hist_m_search(struct Client* cptr, struct Client* sptr, int parc,
                  char* parv[])
{
  struct HistSearch s;
  struct HistSeek* sk;
  char channels[HIST_SCOPE_MAX];
  const char* target;
  int i;
  int found;

  if (parc < 3 || EmptyString(parv[1]) || EmptyString(parv[2]))
    return need_more_params(sptr, MSG_SEARCH);

  /* The answer is a batch of messages that look exactly like the ones
   * arriving now; a client that cannot tell the two apart is better
   * refused than confused. */
  if (!CapActive(sptr, CAP_BATCH)) {
    hist_search_fail(sptr, "INVALID_PARAMS", parv[1],
                     N_("SEARCH needs the batch capability"));
    return 0;
  }

  memset(&s, 0, sizeof(s));
  s.hs_limit = (unsigned int) feature_int(FEAT_HISTORY_MAX_LIMIT);

  if (s.hs_limit < 1)
    s.hs_limit = 1;

  /* Everything between the target and the last parameter is an option;
   * the last one is what to look for. */
  for (i = 2; i < parc - 1; i++) {
    if (EmptyString(parv[i]))
      continue;

    if (!hist_search_option(&s, parv[i])) {
      hist_search_fail(sptr, "INVALID_PARAMS", parv[i],
                       N_("A search takes from=, before=, after= and "
                          "limit=, and then what to look for"));
      return 0;
    }
  }

  ircd_strncpy(s.hs_text, parv[parc - 1], sizeof(s.hs_text) - 1);

  if (!s.hs_text[0]) {
    hist_search_fail(sptr, "INVALID_PARAMS", parv[1],
                     N_("A search needs something to look for"));
    return 0;
  }

  target = parv[1];

  if (IsAccount(sptr) && cli_user(sptr)->account[0])
    hist_canon(s.hs_self, sizeof(s.hs_self), cli_user(sptr)->account);

  if (!strcmp(target, "*")) {
    /* Everywhere this client may look: the channels it is on and its own
     * conversations.  Both, because a person searching for something they
     * said does not remember which of the two it was in. */
    found = hist_scope_channels(sptr, 0, channels, sizeof(channels));

    if (found < 0) {
      hist_search_fail(sptr, "INVALID_TARGET", target,
                       N_("You are on too many channels to search them all; "
                          "name one"));
      return 0;
    }

    if (found > 0)
      s.hs_channels = channels;

    if (!found && !s.hs_self[0]) {
      hist_search_fail(sptr, "INVALID_TARGET", target,
                       N_("There is nowhere for you to search: join a "
                          "channel or identify"));
      return 0;
    }
  } else if (IsChannelName(target)) {
    struct Channel* chptr = FindChannel(target);

    /* Membership, and nothing cleverer: a channel's history is exactly as
     * private as the channel, which is what hist_read.c decides too. */
    if (!chptr || !find_channel_member(sptr, chptr)) {
      hist_search_fail(sptr, "INVALID_TARGET", target,
                       N_("You are not on that channel"));
      return 0;
    }

    if (hist_scope_channels(sptr, chptr, channels, sizeof(channels)) != 1) {
      hist_search_fail(sptr, "INVALID_TARGET", target,
                       N_("That channel cannot be searched"));
      return 0;
    }

    s.hs_channels = channels;
    s.hs_self[0] = '\0';        /* one channel means only that channel */
  } else {
    if (!s.hs_self[0]) {
      hist_search_fail(sptr, "INVALID_TARGET", target,
                       N_("You have to be identified to search a "
                          "conversation"));
      return 0;
    }

    hist_canon(s.hs_peer, sizeof(s.hs_peer), target);
  }

  sk = (struct HistSeek*) MyCalloc(1, sizeof(*sk));
  ircd_snprintf(0, sk->hs_numnick, sizeof(sk->hs_numnick), "%s%s",
                NumNick(sptr));
  sk->hs_born = cli_firsttime(sptr);
  ircd_strncpy(sk->hs_target, target, sizeof(sk->hs_target) - 1);

  if (!hist_store_search(&s, hist_search_answer, sk)) {
    hist_search_fail(sptr, "MESSAGE_ERROR", target,
                     N_("The history is not available right now"));
    MyFree(sk);
  }

  return 0;
}
