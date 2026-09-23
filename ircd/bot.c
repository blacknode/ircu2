/*
 * IRC - Internet Relay Chat, ircd/bot.c
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
 * @brief Virtual clients the server introduces on its own behalf.
 *
 * See include/bot.h for what a bot is and why it works.  This file keeps
 * the list of them, introduces and removes them, and is the one place a
 * message for a service bot is turned into a hook call.
 */
#include "config.h"

#include "bot.h"
#include "channel.h"
#include "client.h"
#include "hash.h"
#include "hooks.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_chattr.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "list.h"
#include "module.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "querycmds.h"
#include "s_debug.h"
#include "s_misc.h"
#include "s_user.h"
#include "send.h"
#include "struct.h"
#include "sys.h"

#include <string.h>

/** Bots that exist, oldest first. */
static struct Bot* bot_list;

/** Number of entries in #bot_list. */
static unsigned int bot_total;

/** Number of entries in #bot_list created with #BOT_SERVICE. */
static unsigned int bot_services;

/** Find the list link that points at the record for \a cptr.
 * @param[in] cptr Any client.
 * @return Address of the pointer to its record, or NULL if it is not a bot.
 */
static struct Bot** bot_slot(const struct Client* cptr)
{
  struct Bot** pp;

  for (pp = &bot_list; *pp; pp = &(*pp)->b_next)
    if ((*pp)->b_client == cptr)
      return pp;

  return NULL;
}

/** Take a bot off the list.
 * @param[in] cptr Client to forget; nothing happens if it is not a bot.
 * @return Non-zero if it was a bot.
 */
static int bot_unlink(const struct Client* cptr)
{
  struct Bot** pp = bot_slot(cptr);
  struct Bot* b;

  if (!pp)
    return 0;

  b = *pp;
  *pp = b->b_next;
  if (b->b_flags & BOT_SERVICE)
    bot_services--;
  MyFree(b);
  bot_total--;

  return 1;
}

struct Bot* bot_find(const struct Client* cptr)
{
  struct Bot** pp = bot_slot(cptr);

  return pp ? *pp : NULL;
}

struct Bot* bot_first(void)
{
  return bot_list;
}

unsigned int bot_count(void)
{
  return bot_total;
}

unsigned int bot_service_count(void)
{
  return bot_services;
}

int bot_check_nick(const char* nick, const struct Client* self)
{
  const struct Client* acptr;
  const char* p;
  size_t len = strlen(nick);

  if (len == 0 || len > (size_t) IRCD_MIN(NICKLEN, feature_int(FEAT_NICKLEN)))
    return ERR_ERRONEUSNICKNAME;

  if (*nick == '-' || IsDigit(*nick))
    return ERR_ERRONEUSNICKNAME;

  for (p = nick; *p; p++)
    if (!IsNickChar(*p))
      return ERR_ERRONEUSNICKNAME;

  if (isNickJuped(nick))
    return ERR_NICKNAMEINUSE;

  /* FindClient() rather than FindUser(): a server name is taken too. */
  if ((acptr = FindClient(nick)) && acptr != self)
    return ERR_NICKNAMEINUSE;

  return 0;
}

int bot_check_channel(const char* name)
{
  if (!IsChannelName(name) || !strIsIrcCh(name))
    return ERR_NOSUCHCHANNEL;

  if (IsLocalChannel(name) && !feature_bool(FEAT_LOCAL_CHANNELS))
    return ERR_NOSUCHCHANNEL;

  /* An existing channel is fine at any length; only creating one is
   * bounded, which is the rule m_join follows.
   */
  if (!FindChannel(name)
      && strlen(name) > (size_t) IRCD_MIN(CHANNELLEN,
                                           feature_int(FEAT_CHANNELLEN)))
    return ERR_NOSUCHCHANNEL;

  return 0;
}

/** Copy \a src into \a dst in lower case, at most \a len characters.
 * @param[out] dst Buffer of at least \a len + 1 bytes.
 * @param[in] src String to copy.
 * @param[in] len Maximum number of characters to copy.
 */
static void bot_lowercase(char* dst, const char* src, size_t len)
{
  size_t i;

  for (i = 0; src[i] && i < len; i++)
    dst[i] = ToLower(src[i]);
  dst[i] = '\0';
}

/** Bring a bot into existence and introduce it to the network.
 *
 * The steps are those of set_nick_name() introducing a remote user,
 * with &me as the introducing server: allocate, name, number, hash,
 * count, then register_user() sends the NICK and fires the hooks.
 */
struct Client* bot_create(struct ModuleHandle* owner, const char* nick,
                          const char* username, const char* host,
                          const char* info, unsigned int flags)
{
  struct Client* bot;
  struct Bot* b;
  struct Bot** pp;
  char lnick[NICKLEN + 1];

  assert(0 != nick);

  bot = make_client(&me, STAT_UNKNOWN);
  cli_hopcount(bot) = 0;
  cli_lastnick(bot) = TStime();
  cli_firsttime(bot) = CurrentTime;
  ircd_strncpy(cli_name(bot), nick, NICKLEN);
  cli_user(bot) = make_user(bot);
  cli_user(bot)->server = &me;

  /* Reserve the numeric first: it is the one step that can fail, and
   * before the client is listed or hashed there is nothing to unwind but
   * the two allocations.
   */
  if (!SetLocalNumNick(bot)) {
    free_user(cli_user(bot));
    cli_user(bot) = NULL;
    free_client(bot);
    return NULL;
  }

  bot_lowercase(lnick, nick, NICKLEN);

  /* The identity is derived unless the owner chose one: <nick> as the
   * user name and <nick>.<BOT_HOSTNAME> as the host, both in lower case,
   * so that a bot's mask can be read off its nick.
   */
  if (username && *username) {
    ircd_strncpy(cli_username(bot), username, USERLEN);
    ircd_strncpy(cli_user(bot)->username, username, USERLEN);
  } else {
    ircd_strncpy(cli_username(bot), lnick, USERLEN);
    ircd_strncpy(cli_user(bot)->username, lnick, USERLEN);
  }
  if (host && *host)
    ircd_strncpy(cli_user(bot)->host, host, HOSTLEN);
  else
    ircd_snprintf(0, cli_user(bot)->host, HOSTLEN + 1, "%s.%s", lnick,
                  feature_str(FEAT_BOT_HOSTNAME));
  ircd_strncpy(cli_user(bot)->realhost, cli_user(bot)->host, HOSTLEN);
  if (info && *info)
    ircd_strncpy(cli_info(bot), info, REALLEN);
  else
    ircd_snprintf(0, cli_info(bot), REALLEN + 1, "%s bot",
                  feature_str(FEAT_NETWORK));

  /* Set before register_user() so the NICK that introduces the bot
   * already carries the modes, rather than a MODE following it.  A
   * service is an operator as well: register_user() counts it as one,
   * and everything that exempts an operator -- target limits, +R, +c --
   * exempts it.
   */
  SetBot(bot);
  if (flags & BOT_SERVICE) {
    SetServiceBot(bot);
    SetChannelService(bot);
    SetOper(bot);
  }

  add_client_to_list(bot);
  hAddClient(bot);
  Count_newremoteclient(UserStats, &me);

  /* Listed before it is registered, so that if registration were ever to
   * exit the client, exit_one_client() would find the record and drop
   * it, and the pointer below would be known to be stale.
   */
  b = (struct Bot*) MyMalloc(sizeof(*b));
  b->b_client = bot;
  b->b_owner = owner;
  b->b_flags = flags;
  b->b_next = NULL;
  for (pp = &bot_list; *pp; pp = &(*pp)->b_next)
    ;
  *pp = b;
  bot_total++;
  if (flags & BOT_SERVICE)
    bot_services++;

  register_user(&me, bot);

  if (!bot_slot(bot))
    return NULL;

  log_write(LS_SYSTEM, L_INFO, 0, "Introduced %s %s (%s@%s) for %s",
            (flags & BOT_SERVICE) ? "service bot" : "bot", cli_name(bot),
            cli_user(bot)->username, cli_user(bot)->host,
            owner ? module_name(owner) : "the server");

  return bot;
}

void bot_destroy(struct Client* bot, struct Client* sptr, const char* reason)
{
  assert(0 != bot);
  assert(0 != bot_find(bot));

  /* exit_one_client() drops the record on the way out, with the bot
   * still listed for HOOK_CLIENT_EXITING.
   */
  exit_client(&me, bot, sptr ? sptr : &me, reason ? reason : "Bot destroyed");
}

void bot_rename(struct Client* bot, const char* newnick)
{
  char oldnick[NICKLEN + 1];
  char* nickv[3];

  assert(0 != bot);
  assert(0 != newnick);

  ircd_strncpy(oldnick, cli_name(bot), NICKLEN);
  oldnick[NICKLEN] = '\0';

  /* The nick-change path of set_nick_name(), called the way a client's
   * own NICK reaches it.  With cptr == sptr it stamps the nick with the
   * current time and reads no further argument.
   */
  nickv[0] = oldnick;
  nickv[1] = (char*) newnick;
  nickv[2] = NULL;
  set_nick_name(bot, bot, newnick, 2, nickv);
}

void bot_join(struct Client* bot, const char* name)
{
  struct Channel* chptr;
  struct JoinBuf jbuf;
  struct ModeBuf mbuf;
  char chname[CHANNELLEN + 1];

  assert(0 != bot);
  assert(0 != name);

  /* get_channel() and the JoinBuf take a writable name. */
  ircd_strncpy(chname, name, CHANNELLEN);
  chname[CHANNELLEN] = '\0';

  if ((chptr = FindChannel(chname))) {
    joinbuf_init(&jbuf, bot, &me, JOINBUF_TYPE_JOIN, NULL, 0);
    joinbuf_join(&jbuf, chptr, CHFL_CHANOP);
    joinbuf_flush(&jbuf);

    /* The JOIN told the network about the membership, not about the op;
     * the local members already heard it from joinbuf_join().  The mode
     * comes from the server, as m_join does it, so no server bounces it.
     */
    modebuf_init(&mbuf, &me, &me, chptr, MODEBUF_DEST_SERVER);
    modebuf_mode_client(&mbuf, MODE_ADD | MODE_CHANOP, bot,
                        chptr->mode.apass[0] ? 1 : MAXOPLEVEL);
    modebuf_flush(&mbuf);
  } else {
    time_t ts = TStime();

    joinbuf_init(&jbuf, bot, &me, JOINBUF_TYPE_CREATE, NULL, ts);
    chptr = get_channel(bot, chname, CGT_CREATE);
    /* get_channel() stamps a creation time only for MyUser() clients;
     * a CREATE with TS 0 would lose every timestamp comparison.
     */
    chptr->creationtime = ts;
    joinbuf_join(&jbuf, chptr, CHFL_CHANOP | CHFL_CHANNEL_MANAGER);
    joinbuf_flush(&jbuf);
  }
}

void bot_part(struct Client* bot, struct Channel* chptr)
{
  struct JoinBuf jbuf;

  assert(0 != bot);
  assert(0 != chptr);

  joinbuf_init(&jbuf, bot, &me, JOINBUF_TYPE_PART, NULL, 0);
  joinbuf_join(&jbuf, chptr, 0);
  joinbuf_flush(&jbuf);
}

void bot_send_user(struct Client* bot, struct Client* to, int notice,
                   const char* text)
{
  assert(0 != bot);
  assert(0 != to);
  assert(0 != text);

  /* CMD_* expands to two arguments, so it cannot sit in a conditional. */
  if (notice)
    sendcmdto_one(bot, CMD_NOTICE, to, "%C :%s", to, text);
  else
    sendcmdto_one(bot, CMD_PRIVATE, to, "%C :%s", to, text);
}

void bot_send_channel(struct Client* bot, struct Channel* chptr, int notice,
                      const char* text)
{
  assert(0 != bot);
  assert(0 != chptr);
  assert(0 != text);

  if (notice)
    sendcmdto_channel_butone(bot, CMD_NOTICE, chptr, NULL,
                             SKIP_DEAF | SKIP_BURST, "%H :%s", chptr, text);
  else
    sendcmdto_channel_butone(bot, CMD_PRIVATE, chptr, NULL,
                             SKIP_DEAF | SKIP_BURST, "%H :%s", chptr, text);
}

int bot_set_user_mode(struct Client* bot, struct Client* target,
                      const char* modes)
{
  char modebuf[BUFSIZE];
  char* parv[4];

  assert(0 != bot);
  assert(0 != target);
  assert(0 != modes);
  assert(0 != bot_find(bot));

  if (!IsUser(target) || EmptyString(modes))
    return 0;

  /* The mode parser writes into its arguments (a snomask parameter is
   * consumed in place), so it gets a copy.
   */
  ircd_strncpy(modebuf, modes, sizeof(modebuf) - 1);
  parv[0] = cli_name(bot);
  parv[1] = cli_name(target);
  parv[2] = modebuf;
  parv[3] = NULL;

  /* The bot is its own link: it is local, and it is what the restrictions
   * on a locally made change apply to.
   */
  if (target == bot) {
    set_user_mode(bot, bot, 3, parv, ALLOWMODES_ANY);
    return 1;
  }
  if (!IsServiceBot(bot) /*|| IsAnOper(target)*/)
    return 0;
  set_user_mode_on(bot, bot, target, 3, parv);
  return 1;
}

/** Run HOOK_MESSAGE_RECEIVED for one message to one bot.
 * @param[in] sptr Sender.
 * @param[in] bot Recipient.
 * @param[in] chptr Channel the message went to, or NULL for a private one.
 * @param[in] notice Non-zero for a NOTICE.
 * @param[in] text The message.
 */
static void bot_run_received(struct Client* sptr, struct Client* bot,
                             struct Channel* chptr, int notice,
                             const char* text)
{
  struct HookContext hc;

  hook_context_init(&hc);
  hc.hc_client = bot;
  hc.hc_source = sptr;
  hc.hc_channel = chptr;
  hc.hc_arg = text;
  hc.hc_notice = notice;

  hook_run(HOOK_MESSAGE_RECEIVED, &hc);
}

void bot_deliver_private(struct Client* sptr, struct Client* bot, int notice,
                         const char* text)
{
  assert(0 != sptr);
  assert(0 != bot);
  assert(0 != text);

  /* A server can address a user, and a service bot could be one of them;
   * but a service answers users, not servers, and a hook that had to
   * check cli_user() on every call would be a hook waiting to crash.
   */
  if (!IsUser(sptr) || !hook_is_active(HOOK_MESSAGE_RECEIVED))
    return;

  bot_run_received(sptr, bot, NULL, notice, text);
}

void bot_deliver_channel(struct Client* sptr, struct Channel* chptr,
                         int notice, const char* text)
{
  struct Bot* b;

  assert(0 != sptr);
  assert(0 != chptr);
  assert(0 != text);

  if (!bot_services || !IsUser(sptr)
      || !hook_is_active(HOOK_MESSAGE_RECEIVED))
    return;

  /* Walk the bots and ask each whether it is on the channel, not the
   * other way round: a service bot is on a handful of channels, while a
   * channel can have thousands of members.
   */
  for (b = bot_list; b; b = b->b_next)
    if ((b->b_flags & BOT_SERVICE) && find_channel_member(b->b_client, chptr))
      bot_run_received(sptr, b->b_client, chptr, notice, text);
}

void bot_client_exiting(struct Client* cptr)
{
  if (cptr && bot_unlink(cptr))
    log_write(LS_SYSTEM, L_INFO, 0, "Bot %s is gone", cli_name(cptr));
}

void bot_drop_module(struct ModuleHandle* mod)
{
  struct Bot* b;
  unsigned int n = 0;

  assert(0 != mod);

  /* exit_client() unlinks the record, so the walk restarts from the head
   * after each one rather than following a freed b_next.
   */
  for (;;) {
    for (b = bot_list; b && b->b_owner != mod; b = b->b_next)
      ;
    if (!b)
      break;

    exit_client(&me, b->b_client, &me, "Module unloaded");
    n++;
  }

  if (n)
    log_write(LS_SYSTEM, L_INFO, 0, "Destroyed %u bot%s of module %s on unload",
              n, n == 1 ? "" : "s", module_name(mod));
}
