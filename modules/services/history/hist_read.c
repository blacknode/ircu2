/*
 * IRC - Internet Relay Chat, modules/services/history/hist_read.c
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
 * @brief CHATHISTORY: what a client is allowed to read, and how it is sent.
 *
 * The IRCv3 @c draft/chathistory command.  This file decides three things
 * and hands everything else to hist_store.c:
 *
 *   - what the client asked for, out of six shapes and two ways of naming
 *     a point in a conversation;
 *   - whether it may see it, which for a channel is membership and for a
 *     conversation is being one of the two people in it;
 *   - how the answer is put on the wire, which is a batch of the messages
 *     as they were sent, carrying the name and the time they had.
 *
 * The answer is not synchronous.  The store is a database, the query goes
 * out through db.h and comes back a round trip later, which has one
 * visible consequence: a client that labelled the command gets its
 * @c ACK straight away and the batch afterwards, outside the label.  That
 * is the honest shape of an asynchronous answer, and the batch identifies
 * itself well enough to be matched without one.
 */
#include "config.h"

#include "batch.h"
#include "channel.h"
#include "client.h"
#include "hash.h"
#include "history.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_alloc.h"
#include "ircd_i18n.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "msg.h"
#include "msg_tag.h"
#include "numeric.h"
#include "numnicks.h"
#include "s_user.h"
#include "send.h"
#include "struct.h"

#include <stdlib.h>
#include <string.h>

/** The batch type the messages come back in. */
#define HIST_BATCH_TYPE "chathistory"

/** The batch type a listing of conversations comes back in. */
#define HIST_TARGETS_BATCH_TYPE "draft/chathistory-targets"

/** A time far enough in the past to mean "from the beginning". */
#define HIST_TIME_DAWN "1970-01-01T00:00:00.000Z"

/** And far enough ahead to mean "until now". */
#define HIST_TIME_DUSK "9999-12-31T23:59:59.999Z"

/** One request in flight.
 *
 * The client is held by numnick rather than by pointer: the answer comes
 * back from a database, and a connection can be gone by then.  db.c drops
 * the callback of a module being unloaded, but it does not know when a
 * client leaves, and a pointer to a freed one is the kind of mistake that
 * only shows up under load.
 */
struct HistAsk {
  char ha_numnick[10];             /**< Who asked. */
  time_t ha_born;                  /**< When they connected. */
  int  ha_targets;                 /**< It was a TARGETS request. */
  char ha_target[CHANNELLEN + 1];  /**< What they asked about. */
};

/** The client that made a request, or NULL if it is not there any more. */
static struct Client* hist_asker(const struct HistAsk* ask)
{
  struct Client* cptr = findNUser(ask->ha_numnick);

  /* The numnick is reused when a client leaves and another arrives, so
   * the connection time is checked too: it is what makes a numnick name
   * one connection rather than one slot.
   */
  if (!cptr || !MyUser(cptr) || cli_firsttime(cptr) != ask->ha_born)
    return 0;

  return cptr;
}

/** Refuse a request.
 * @param[in] cptr Who asked.
 * @param[in] code Machine-readable code from the specification.
 * @param[in] context One word of context, or NULL.
 * @param[in] text Untranslated description.
 */
static void hist_fail(struct Client* cptr, const char* code,
                      const char* context, const char* text)
{
  send_fail(cptr, MSG_CHATHISTORY, code, context, _(cptr, text));
}

/* ------------------------------------------------------------------- *
 * Parsing                                                             *
 * ------------------------------------------------------------------- */

/** Read one selector: @c timestamp=..., @c msgid=... or @c *.
 * @param[in] text What the client wrote.
 * @param[out] point Where to put it.
 * @return Non-zero if it was understood.
 */
static int hist_point_parse(const char* text, struct HistPoint* point)
{
  memset(point, 0, sizeof(*point));

  if (EmptyString(text))
    return 0;

  if (!strcmp(text, "*"))
    return 1;                   /* no point at all: from the end */

  if (!ircd_strncmp(text, "timestamp=", 10)) {
    ircd_strncpy(point->hp_time, text + 10, sizeof(point->hp_time) - 1);
    point->hp_time[sizeof(point->hp_time) - 1] = '\0';
    return point->hp_time[0] ? 1 : 0;
  }

  if (!ircd_strncmp(text, "msgid=", 6)) {
    ircd_strncpy(point->hp_msgid, text + 6, sizeof(point->hp_msgid) - 1);
    point->hp_msgid[sizeof(point->hp_msgid) - 1] = '\0';
    return point->hp_msgid[0] ? 1 : 0;
  }

  return 0;
}

/** Turn a subcommand into a shape.
 * @param[in] word What the client wrote.
 * @param[out] shape Where to put it.
 * @return Non-zero if it is one of the six.
 */
static int hist_shape_parse(const char* word, enum HistShape* shape)
{
  if (!ircd_strcmp(word, "LATEST"))  { *shape = HIST_LATEST;  return 1; }
  if (!ircd_strcmp(word, "BEFORE"))  { *shape = HIST_BEFORE;  return 1; }
  if (!ircd_strcmp(word, "AFTER"))   { *shape = HIST_AFTER;   return 1; }
  if (!ircd_strcmp(word, "AROUND"))  { *shape = HIST_AROUND;  return 1; }
  if (!ircd_strcmp(word, "BETWEEN")) { *shape = HIST_BETWEEN; return 1; }
  if (!ircd_strcmp(word, "TARGETS")) { *shape = HIST_TARGETS; return 1; }

  return 0;
}

/** Clamp what the client asked for to what the server will give.
 *
 * A client asking for more than #FEAT_HISTORY_MAX_LIMIT gets the maximum
 * rather than an error: the limit is the server's, and a client that has
 * to discover it by being refused would only ask again with a smaller
 * number.
 */
static unsigned int hist_limit_parse(const char* word)
{
  int max = feature_int(FEAT_HISTORY_MAX_LIMIT);
  int n;

  if (max <= 0)
    max = 1;

  n = EmptyString(word) ? 0 : atoi(word);

  if (n <= 0)
    return (unsigned int) max;

  return (unsigned int) (n > max ? max : n);
}

/* ------------------------------------------------------------------- *
 * Sending the answer                                                  *
 * ------------------------------------------------------------------- */

/** Render the client tags a stored message goes back out with.
 *
 * A reply carries what it replies to; a reaction carries that and the
 * reaction itself, which is where a TAGMSG keeps what it came to say.
 * Everything else carries nothing: the tags a message had that were not
 * part of it -- a typing indicator, a label -- were not stored and are
 * not invented here.
 */
static void hist_replay_tags(char* buf, size_t buflen,
                             const struct HistRow* row)
{
  buf[0] = '\0';

  if (row->hr_kind == HIST_TAGMSG && row->hr_body && row->hr_body[0])
    ircd_snprintf(0, buf, buflen, "%s=%s;%s=%s", HIST_TAG_REACT,
                  row->hr_body, HIST_TAG_REPLY,
                  (row->hr_reply && row->hr_reply[0]) ? row->hr_reply : "");
  else if (row->hr_reply && row->hr_reply[0])
    ircd_snprintf(0, buf, buflen, "%s=%s", HIST_TAG_REPLY, row->hr_reply);
}

/** Send the messages a read came back with.
 * @param[in] cptr Who asked.
 * @param[in] ask What they asked.
 * @param[in] rows The messages, oldest first.
 * @param[in] count How many.
 */
static void hist_send_messages(struct Client* cptr, const struct HistAsk* ask,
                               const struct HistRow* rows, unsigned int count)
{
  unsigned int i;

  if (!batch_out_open(cptr, HIST_BATCH_TYPE, ask->ha_target))
    return;

  for (i = 0; i < count; i++) {
    const char* cmd;
    const char* tok;
    char tags[512];

    switch (rows[i].hr_kind) {
    case HIST_NOTICE: cmd = MSG_NOTICE;  tok = TOK_NOTICE;  break;
    case HIST_TAGMSG: cmd = MSG_TAGMSG;  tok = TOK_TAGMSG;  break;
    default:          cmd = MSG_PRIVATE; tok = TOK_PRIVATE; break;
    }

    hist_replay_tags(tags, sizeof(tags), &rows[i]);

    /* The message goes back out under the name and the time it already
     * had: that is what makes it the same message everybody else saw,
     * and what lets a client put it in order against what it has.
     */
    msg_tag_line_replay(tok, rows[i].hr_msgid, rows[i].hr_time, tags);

    /* The prefix is the one stored with the message, not one built from
     * whoever holds that nickname now, so the tags have to be rendered by
     * a send that takes a prefix rather than a client. */
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

/** Send a listing of conversations.
 * @param[in] cptr Who asked.
 * @param[in] rows The targets, oldest first.
 * @param[in] count How many.
 */
static void hist_send_targets(struct Client* cptr, const struct HistRow* rows,
                              unsigned int count)
{
  unsigned int i;

  if (!batch_out_open(cptr, HIST_TARGETS_BATCH_TYPE, 0))
    return;

  for (i = 0; i < count; i++)
    sendcmdto_one(&me, MSG_CHATHISTORY, TOK_CHATHISTORY, cptr,
                  "TARGETS %s %s", rows[i].hr_target, rows[i].hr_time);

  batch_out_close(cptr);
}

/** What the store came back with.
 * @param[in] ok Non-zero if it answered at all.
 * @param[in] rows The messages.
 * @param[in] count How many.
 * @param[in] user The #HistAsk.
 */
static void hist_answer(int ok, const struct HistRow* rows,
                        unsigned int count, void* user)
{
  struct HistAsk* ask = (struct HistAsk*) user;
  struct Client* cptr = hist_asker(ask);

  if (!cptr) {
    MyFree(ask);
    return;
  }

  if (!ok) {
    hist_fail(cptr, "MESSAGE_ERROR", ask->ha_targets ? 0 : ask->ha_target,
              N_("The history is not available right now"));
    MyFree(ask);
    return;
  }

  /* An empty answer is still an answer, and it is sent as an empty batch
   * rather than as nothing: a client waiting for the close line would
   * otherwise wait for ever, and "there is nothing there" is a different
   * thing from "the server ignored you".
   */
  if (ask->ha_targets)
    hist_send_targets(cptr, rows, count);
  else
    hist_send_messages(cptr, ask, rows, count);

  MyFree(ask);
}

/* ------------------------------------------------------------------- *
 * The command                                                         *
 * ------------------------------------------------------------------- */

/** Work out who the two ends of a conversation are.
 *
 * An account is a nickname, so the other end is the nickname the client
 * named; there is nothing to look up and the person does not have to be
 * online for their side of the conversation to be readable.
 *
 * @param[in] sptr Who asked.
 * @param[in] target The nickname they named.
 * @param[out] q Where to put the two accounts.
 * @return Zero if the client may not read this, having been told why.
 */
static int hist_conversation(struct Client* sptr, const char* target,
                             struct HistQuery* q)
{
  /* Only somebody who has proved a nickname has conversations: a direct
   * message is stored under the two accounts in it, and a client with no
   * account is not one of them.  This is also why there is nothing here
   * to leak -- there is no row it could match.
   */
  if (!IsAccount(sptr) || !cli_user(sptr)->account[0]) {
    hist_fail(sptr, "INVALID_TARGET", target,
              N_("You have to be identified to read a conversation"));
    return 0;
  }

  hist_canon(q->hq_self, sizeof(q->hq_self), cli_user(sptr)->account);
  hist_canon(q->hq_peer, sizeof(q->hq_peer), target);

  return 1;
}

/** Work out whether a client may read a channel's history.
 *
 * Membership, and nothing cleverer.  A channel's history is exactly as
 * private as the channel, and somebody who is not in it was not there for
 * the conversation either.
 *
 * @param[in] sptr Who asked.
 * @param[in] target The channel they named.
 * @param[out] q Where to put the channel.
 * @return Zero if the client may not read this, having been told why.
 */
static int hist_channel(struct Client* sptr, const char* target,
                        struct HistQuery* q)
{
  struct Channel* chptr = FindChannel(target);

  if (!chptr || !find_channel_member(sptr, chptr)) {
    hist_fail(sptr, "INVALID_TARGET", target,
              N_("You are not on that channel"));
    return 0;
  }

  hist_canon(q->hq_canon, sizeof(q->hq_canon), chptr->chname);

  return 1;
}

/** Start a request.
 * @param[in] sptr Who asked.
 * @param[in] q What they asked for.
 * @param[in] target What to put on the batch line.
 * @param[in] targets Non-zero for a TARGETS request.
 */
static void hist_begin(struct Client* sptr, struct HistQuery* q,
                       const char* target, int targets)
{
  struct HistAsk* ask;

  ask = (struct HistAsk*) MyCalloc(1, sizeof(*ask));
  ircd_snprintf(0, ask->ha_numnick, sizeof(ask->ha_numnick), "%s%s",
                NumNick(sptr));
  ask->ha_born = cli_firsttime(sptr);
  ask->ha_targets = targets;
  ircd_strncpy(ask->ha_target, target ? target : "*",
               sizeof(ask->ha_target) - 1);

  if (!hist_store_read(q, hist_answer, ask)) {
    hist_fail(sptr, "MESSAGE_ERROR", targets ? 0 : ask->ha_target,
              N_("The history is not available right now"));
    MyFree(ask);
  }
}

/** CHATHISTORY from a client.
 * @param[in] cptr Connection it arrived on.
 * @param[in] sptr Who sent it.
 * @param[in] parc Parameter count.
 * @param[in] parv Parameters.
 * @return Zero.
 */
int hist_m_chathistory(struct Client* cptr, struct Client* sptr, int parc,
                       char* parv[])
{
  struct HistQuery q;
  enum HistShape shape;
  const char* target;

  if (parc < 2 || EmptyString(parv[1]))
    return need_more_params(sptr, MSG_CHATHISTORY);

  /* The answer is a batch, and a client that cannot read one would be
   * sent a pile of messages it could not tell from the ones happening
   * now.  Worse than refusing, so refuse.
   */
  if (!CapActive(sptr, CAP_BATCH)) {
    hist_fail(sptr, "INVALID_PARAMS", parv[1],
              N_("CHATHISTORY needs the batch capability"));
    return 0;
  }

  if (!hist_shape_parse(parv[1], &shape)) {
    hist_fail(sptr, "UNKNOWN_COMMAND", parv[1],
              N_("Unknown CHATHISTORY subcommand"));
    return 0;
  }

  memset(&q, 0, sizeof(q));
  q.hq_shape = shape;

  if (shape == HIST_TARGETS) {
    /* CHATHISTORY TARGETS <after> <before> <limit> */
    if (parc < 5)
      return need_more_params(sptr, MSG_CHATHISTORY);

    if (!hist_point_parse(parv[2], &q.hq_a)
        || !hist_point_parse(parv[3], &q.hq_b)) {
      hist_fail(sptr, "INVALID_PARAMS", parv[1],
                N_("A selector is timestamp=<time>, msgid=<id> or *"));
      return 0;
    }

    /* The window is by time only: naming it by message would mean the two
     * ends of it were in conversations the client is asking to be told
     * about, which it cannot know yet.
     */
    if (!q.hq_a.hp_time[0])
      ircd_strncpy(q.hq_a.hp_time, HIST_TIME_DAWN, sizeof(q.hq_a.hp_time) - 1);
    if (!q.hq_b.hp_time[0])
      ircd_strncpy(q.hq_b.hp_time, HIST_TIME_DUSK, sizeof(q.hq_b.hp_time) - 1);

    q.hq_a.hp_msgid[0] = '\0';
    q.hq_b.hp_msgid[0] = '\0';
    q.hq_limit = hist_limit_parse(parv[4]);

    if (!IsAccount(sptr) || !cli_user(sptr)->account[0]) {
      /* Nothing is stored for a client with no account, so the honest
       * answer is an empty listing rather than a refusal. */
      hist_send_targets(sptr, 0, 0);
      return 0;
    }

    hist_canon(q.hq_self, sizeof(q.hq_self), cli_user(sptr)->account);
    hist_begin(sptr, &q, 0, 1);
    return 0;
  }

  if (parc < 5)
    return need_more_params(sptr, MSG_CHATHISTORY);

  target = parv[2];

  if (EmptyString(target)) {
    hist_fail(sptr, "INVALID_TARGET", "*", N_("No target given"));
    return 0;
  }

  q.hq_channel = IsChannelName(target) ? 1 : 0;

  if (q.hq_channel) {
    if (!hist_channel(sptr, target, &q))
      return 0;
  } else {
    if (!hist_conversation(sptr, target, &q))
      return 0;
  }

  if (!hist_point_parse(parv[3], &q.hq_a)) {
    hist_fail(sptr, "INVALID_PARAMS", parv[1],
              N_("A selector is timestamp=<time>, msgid=<id> or *"));
    return 0;
  }

  if (shape == HIST_BETWEEN) {
    /* CHATHISTORY BETWEEN <target> <a> <b> <limit> */
    if (parc < 6)
      return need_more_params(sptr, MSG_CHATHISTORY);

    if (!hist_point_parse(parv[4], &q.hq_b)) {
      hist_fail(sptr, "INVALID_PARAMS", parv[1],
                N_("A selector is timestamp=<time>, msgid=<id> or *"));
      return 0;
    }

    if (!q.hq_a.hp_time[0] && !q.hq_a.hp_msgid[0])
      ircd_strncpy(q.hq_a.hp_time, HIST_TIME_DAWN, sizeof(q.hq_a.hp_time) - 1);
    if (!q.hq_b.hp_time[0] && !q.hq_b.hp_msgid[0])
      ircd_strncpy(q.hq_b.hp_time, HIST_TIME_DUSK, sizeof(q.hq_b.hp_time) - 1);

    q.hq_limit = hist_limit_parse(parv[5]);
  } else {
    q.hq_limit = hist_limit_parse(parv[4]);

    /* Everything but LATEST needs somewhere to start from; LATEST is the
     * one that means "from the end", which is what * is for. */
    if (!q.hq_a.hp_time[0] && !q.hq_a.hp_msgid[0] && shape != HIST_LATEST) {
      hist_fail(sptr, "INVALID_PARAMS", parv[1],
                N_("That subcommand needs a point to start from"));
      return 0;
    }
  }

  hist_begin(sptr, &q, target, 0);

  return 0;
}
