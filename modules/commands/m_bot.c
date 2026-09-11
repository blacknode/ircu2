/*
 * IRC - Internet Relay Chat, modules/commands/m_bot.c
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
 * @brief Virtual bots created on demand by network operators.
 *
 *   /BOT CREATE  <nick> [channel]        create a bot, optionally on a channel
 *   /BOT RENAME  <nick> <newnick>        change its nick
 *   /BOT DESTROY <nick>                  make it quit and forget it
 *   /BOT JOIN    <nick> <channel>        join a channel with ops, creating it
 *   /BOT PART    <nick> <channel>        leave a channel
 *   /BOT SAY     <nick> <target> <text>  send a PRIVMSG to a channel or a user
 *   /BOT LIST                            what bots exist, and how many
 *
 * A bot is a struct Client the server introduces on its own behalf: it is
 * made with make_client(&me, ...), so it shares the server's own
 * Connection the way a remote user shares its uplink's.  That one choice
 * does most of the work.  cli_from() of a bot is &me, which has no
 * descriptor, so every message addressed to it -- a PRIVMSG, a numeric,
 * a channel it is on -- is dropped in can_send() before anything is
 * queued.  Its server is &me, so it gets a local numnick and the rest of
 * the network sees an ordinary user of this server.  And it is not
 * MyConnect(), so exit_client() never tries to close a socket that does
 * not exist.  register_user(), set_nick_name(), the JoinBuf and the
 * ModeBuf then handle it like any other client, which is how it comes to
 * do the P10 talking without this file knowing the protocol.
 *
 * Two things follow from sharing &me's Connection.  Nothing here writes
 * a per-connection field of a bot -- cli_handler(), cli_snomask(),
 * cli_privs() and their kind belong to &me -- and every message a bot
 * sends goes straight to send.c rather than through the relay layer,
 * whose flood limits assert on a client that is not local.
 *
 * Every bot carries user mode +B if modules/modes/m_botmode.c has registered
 * the letter; it is looked up when a bot is created rather than assumed,
 * so this module loads without it, just less usefully.
 *
 * A bot the network takes away -- a /KILL, a nick collision -- is dropped
 * from the list through HOOK_CLIENT_EXITING, so the list never holds a
 * client the server has freed.  Unloading the module destroys every bot
 * it has.
 */
#include "config.h"

#include "channel.h"
#include "client.h"
#include "handlers.h"
#include "hash.h"
#include "hooks.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_chattr.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "list.h"
#include "module.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "querycmds.h"
#include "s_misc.h"
#include "s_user.h"
#include "send.h"
#include "sys.h"

#include <string.h>

/** The user mode that marks a bot; modules/modes/m_botmode.c registers it. */
#define BOT_UMODE 'B'

/** One bot this module owns. */
struct Bot {
  struct Client* b_client;   /**< The client the server introduced. */
  struct Bot*    b_next;     /**< Next bot, in creation order. */
};

/** Bots that exist, oldest first. */
static struct Bot* bot_list;

/** Number of entries in #bot_list. */
static unsigned int bot_count;

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
  MyFree(b);
  bot_count--;

  return 1;
}

/** Look a bot up by nick, telling the operator when there is none.
 * @param[in] sptr Operator to answer.
 * @param[in] nick Nick to look for.
 * @return The bot, or NULL after a reply has been sent.
 */
static struct Client* bot_lookup(struct Client* sptr, const char* nick)
{
  struct Client* acptr = FindUser(nick);

  if (!acptr) {
    send_reply(sptr, ERR_NOSUCHNICK, nick);
    return NULL;
  }

  if (!bot_slot(acptr)) {
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :%s is not a bot of this server",
                  sptr, cli_name(acptr));
    return NULL;
  }

  return acptr;
}

/** Check that \a nick is a nickname a bot may have.
 *
 * The rules m_nick applies to a local client, minus the hooks: length,
 * characters, jupes, and that nobody else has it.
 *
 * @param[in] nick Proposed nick.
 * @param[in] self Client that may already own the nick (a bot changing
 *   the case of its own), or NULL.
 * @return Zero if it is acceptable, otherwise the numeric to answer with.
 */
static int bot_check_nick(const char* nick, const struct Client* self)
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

/** Check that \a name is a channel a bot may be put on.
 * @param[in] name Proposed channel name.
 * @return Zero if it is acceptable, otherwise the numeric to answer with.
 */
static int bot_check_channel(const char* name)
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

/** Bring a bot into existence and introduce it to the network.
 *
 * The steps are those of set_nick_name() introducing a remote user,
 * with &me as the introducing server: allocate, name, number, hash,
 * count, then register_user() sends the NICK and fires the hooks.
 *
 * @param[in] nick Nick the bot gets; already checked.
 * @return The new client, or NULL if no numeric nick was free.
 */
static struct Client* bot_create(const char* nick)
{
  struct Client* bot;
  struct Bot* b;
  struct Bot** pp;
  const struct UserMode* bmode;
  char lnick[NICKLEN + 1];
  size_t i;

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

  for (i = 0; nick[i] && i < NICKLEN; i++)
    lnick[i] = ToLower(nick[i]);
  lnick[i] = '\0';

  /* The identity is derived, not chosen: <nick> as the user name and
   * <nick>.<BOT_HOSTNAME> as the host, both in lower case, so that a
   * bot's mask can be read off its nick.
   */
  ircd_strncpy(cli_username(bot), lnick, USERLEN);
  ircd_strncpy(cli_user(bot)->username, lnick, USERLEN);
  ircd_snprintf(0, cli_user(bot)->host, HOSTLEN + 1, "%s.%s", lnick,
                feature_str(FEAT_BOT_HOSTNAME));
  ircd_strncpy(cli_user(bot)->realhost, cli_user(bot)->host, HOSTLEN);
  ircd_snprintf(0, cli_info(bot), REALLEN + 1, "%s bot",
                feature_str(FEAT_NETWORK));

  /* Set before register_user() so the NICK that introduces the bot
   * already carries the mode, rather than a MODE following it.
   */
  if ((bmode = client_find_user_mode(BOT_UMODE)))
    SetUFlag(bot, bmode->flag);

  add_client_to_list(bot);
  hAddClient(bot);
  Count_newremoteclient(UserStats, &me);

  /* Listed before it is registered, so that if registration were ever to
   * exit the client, HOOK_CLIENT_EXITING would find the record and drop
   * it, and the pointer below would be known to be stale.
   */
  b = (struct Bot*) MyMalloc(sizeof(*b));
  b->b_client = bot;
  b->b_next = NULL;
  for (pp = &bot_list; *pp; pp = &(*pp)->b_next)
    ;
  *pp = b;
  bot_count++;

  register_user(&me, bot);

  return bot_slot(bot) ? bot : NULL;
}

/** Put a bot on a channel with ops, creating the channel if it must.
 * @param[in] bot The bot.
 * @param[in] name Channel name; already checked, and the bot not on it.
 */
static void bot_join(struct Client* bot, char* name)
{
  struct Channel* chptr;
  struct JoinBuf jbuf;
  struct ModeBuf mbuf;

  if ((chptr = FindChannel(name))) {
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
    chptr = get_channel(bot, name, CGT_CREATE);
    /* get_channel() stamps a creation time only for MyUser() clients;
     * a CREATE with TS 0 would lose every timestamp comparison.
     */
    chptr->creationtime = ts;
    joinbuf_join(&jbuf, chptr, CHFL_CHANOP | CHFL_CHANNEL_MANAGER);
    joinbuf_flush(&jbuf);
  }
}

/** Take a bot off a channel.
 * @param[in] bot The bot.
 * @param[in] chptr Channel it is on.
 */
static void bot_part(struct Client* bot, struct Channel* chptr)
{
  struct JoinBuf jbuf;

  joinbuf_init(&jbuf, bot, &me, JOINBUF_TYPE_PART, NULL, 0);
  joinbuf_join(&jbuf, chptr, 0);
  joinbuf_flush(&jbuf);
}

/** Send a PRIVMSG as a bot.
 *
 * Straight to the channel or the user, with none of the checks the relay
 * layer applies to a local client's message: a bot speaks for an
 * operator, so +n, +m and bans do not apply to it, any more than they
 * apply to a service.
 *
 * @param[in] bot The bot.
 * @param[in] target Channel name or nick.
 * @param[in] text What to say.
 * @return Zero if sent, otherwise the numeric to answer with.
 */
static int bot_say(struct Client* bot, const char* target, const char* text)
{
  if (IsChannelName(target)) {
    struct Channel* chptr = FindChannel(target);

    if (!chptr)
      return ERR_NOSUCHCHANNEL;

    sendcmdto_channel_butone(bot, CMD_PRIVATE, chptr, NULL,
                             SKIP_DEAF | SKIP_BURST, "%H :%s", chptr, text);
  } else {
    struct Client* acptr = FindUser(target);

    if (!acptr)
      return ERR_NOSUCHNICK;

    sendcmdto_one(bot, CMD_PRIVATE, acptr, "%C :%s", acptr, text);
  }

  return 0;
}

/*
 * The subcommands.  Each takes the operator and the arguments the parser
 * split off, and answers the operator itself.
 */

static int bot_cmd_create(struct Client* sptr, const char* nick, char* chan)
{
  struct Client* bot;
  int err;

  if ((err = bot_check_nick(nick, NULL)))
    return send_reply(sptr, err, nick);

  if (chan && (err = bot_check_channel(chan)))
    return send_reply(sptr, err, chan);

  if (!(bot = bot_create(nick))) {
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  "%C :Cannot create bot %s: no numeric nick is free",
                  sptr, nick);
    return 0;
  }

  sendto_opmask_butone(0, SNO_OLDSNO, "%s created bot %s (%s@%s)",
                       cli_name(sptr), cli_name(bot),
                       cli_user(bot)->username, cli_user(bot)->host);
  log_write(LS_SYSTEM, L_INFO, 0, "%#C created bot %s (%s@%s)", sptr,
            cli_name(bot), cli_user(bot)->username, cli_user(bot)->host);

  sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Bot %s (%s@%s) created", sptr,
                cli_name(bot), cli_user(bot)->username, cli_user(bot)->host);

  if (!client_find_user_mode(BOT_UMODE))
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  "%C :User mode +%c is not registered (load m_botmode "
                  "first); %s was created without it",
                  sptr, BOT_UMODE, cli_name(bot));

  if (chan) {
    bot_join(bot, chan);
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Bot %s joined %s", sptr,
                  cli_name(bot), chan);
  }

  return 0;
}

static int bot_cmd_destroy(struct Client* cptr, struct Client* sptr,
                           const char* nick)
{
  struct Client* bot;
  char name[NICKLEN + 1];

  if (!(bot = bot_lookup(sptr, nick)))
    return 0;

  /* exit_client() frees the client; keep the name for the replies. */
  ircd_strncpy(name, cli_name(bot), NICKLEN);
  name[NICKLEN] = '\0';

  sendto_opmask_butone(0, SNO_OLDSNO, "%s destroyed bot %s", cli_name(sptr),
                       name);
  log_write(LS_SYSTEM, L_INFO, 0, "%#C destroyed bot %s", sptr, name);

  /* The QUIT reaches its channels and the network; HOOK_CLIENT_EXITING
   * takes it off the list on the way out.
   */
  exit_client(cptr, bot, sptr, "Bot destroyed");

  sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Bot %s destroyed", sptr, name);

  return 0;
}

static int bot_cmd_rename(struct Client* sptr, const char* nick,
                          char* newnick)
{
  struct Client* bot;
  char oldnick[NICKLEN + 1];
  char* nickv[3];
  int err;

  if (!(bot = bot_lookup(sptr, nick)))
    return 0;

  if ((err = bot_check_nick(newnick, bot)))
    return send_reply(sptr, err, newnick);

  if (0 == strcmp(cli_name(bot), newnick)) {
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Bot %s already has that nick",
                  sptr, cli_name(bot));
    return 0;
  }

  ircd_strncpy(oldnick, cli_name(bot), NICKLEN);
  oldnick[NICKLEN] = '\0';

  /* The nick-change path of set_nick_name(), called the way a client's
   * own NICK reaches it: it tells the bot's channels, updates WHOWAS,
   * sends the NICK to every server, rehashes the client and runs
   * HOOK_CLIENT_NICK_CHANGED.  With cptr == sptr it stamps the nick with
   * the current time and reads no further argument.
   */
  nickv[0] = oldnick;
  nickv[1] = newnick;
  nickv[2] = NULL;
  set_nick_name(bot, bot, newnick, 2, nickv);

  sendto_opmask_butone(0, SNO_OLDSNO, "%s renamed bot %s to %s",
                       cli_name(sptr), oldnick, cli_name(bot));
  log_write(LS_SYSTEM, L_INFO, 0, "%#C renamed bot %s to %s", sptr, oldnick,
            cli_name(bot));

  sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Bot %s renamed to %s", sptr,
                oldnick, cli_name(bot));

  return 0;
}

static int bot_cmd_join(struct Client* sptr, const char* nick, char* chan)
{
  struct Client* bot;
  struct Channel* chptr;
  int err;

  if (!(bot = bot_lookup(sptr, nick)))
    return 0;

  if ((err = bot_check_channel(chan)))
    return send_reply(sptr, err, chan);

  if ((chptr = FindChannel(chan)) && find_member_link(chptr, bot))
    return send_reply(sptr, ERR_USERONCHANNEL, cli_name(bot), chptr->chname);

  bot_join(bot, chan);

  sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Bot %s joined %s", sptr,
                cli_name(bot), chan);

  return 0;
}

static int bot_cmd_part(struct Client* sptr, const char* nick,
                        const char* chan)
{
  struct Client* bot;
  struct Channel* chptr;

  if (!(bot = bot_lookup(sptr, nick)))
    return 0;

  if (!(chptr = FindChannel(chan)))
    return send_reply(sptr, ERR_NOSUCHCHANNEL, chan);

  if (!find_member_link(chptr, bot))
    return send_reply(sptr, ERR_USERNOTINCHANNEL, cli_name(bot),
                      chptr->chname);

  bot_part(bot, chptr);

  sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Bot %s left %s", sptr,
                cli_name(bot), chan);

  return 0;
}

static int bot_cmd_say(struct Client* sptr, const char* nick,
                       const char* target, const char* text)
{
  struct Client* bot;
  int err;

  if (!(bot = bot_lookup(sptr, nick)))
    return 0;

  if ((err = bot_say(bot, target, text)))
    return send_reply(sptr, err, target);

  return 0;
}

static int bot_cmd_list(struct Client* sptr)
{
  struct Bot* b;

  for (b = bot_list; b; b = b->b_next) {
    struct Client* bot = b->b_client;
    struct Membership* member;
    char chans[BUFSIZE];
    int len = 0;

    chans[0] = '\0';
    for (member = cli_user(bot)->channel; member;
         member = member->next_channel) {
      const char* name = member->channel->chname;

      /* Leave room for the rest of the line, and say when it ran out. */
      if (len + strlen(name) + 5 > 400) {
        len += ircd_snprintf(0, chans + len, sizeof(chans) - len, " ...");
        break;
      }
      len += ircd_snprintf(0, chans + len, sizeof(chans) - len, " %s", name);
    }

    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Bot %s (%s@%s) on %u channel%s%s%s",
                  sptr, cli_name(bot), cli_user(bot)->username,
                  cli_user(bot)->host, cli_user(bot)->joined,
                  cli_user(bot)->joined == 1 ? "" : "s",
                  chans[0] ? ":" : "", chans);
  }

  sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :End of bot list: %u bot%s", sptr,
                bot_count, bot_count == 1 ? "" : "s");

  return 0;
}

/** Handle BOT from an operator.
 *
 * parv[1] is the subcommand, parv[2] the bot's nick, parv[3] a channel,
 * a target or a new nick, and parv[4] the rest of the line.  Only global
 * operators may use it: the OPER_HANDLER also admits local operators,
 * and a bot is seen by the whole network.
 *
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 * @return Zero.
 */
static int mo_bot(struct Client* cptr, struct Client* sptr, int parc,
                  char* parv[])
{
  const char* sub;

  if (!IsOper(sptr))
    return send_reply(sptr, ERR_NOPRIVILEGES);

  if (parc < 2 || EmptyString(parv[1]))
    return need_more_params(sptr, "BOT");

  sub = parv[1];

  if (0 == ircd_strcmp(sub, "LIST"))
    return bot_cmd_list(sptr);

  if (parc < 3 || EmptyString(parv[2]))
    return need_more_params(sptr, "BOT");

  if (0 == ircd_strcmp(sub, "CREATE"))
    return bot_cmd_create(sptr, parv[2],
                          parc > 3 && !EmptyString(parv[3]) ? parv[3] : NULL);

  if (0 == ircd_strcmp(sub, "DESTROY"))
    return bot_cmd_destroy(cptr, sptr, parv[2]);

  /* Everything else names a second thing. */
  if (parc < 4 || EmptyString(parv[3]))
    return need_more_params(sptr, "BOT");

  if (0 == ircd_strcmp(sub, "RENAME"))
    return bot_cmd_rename(sptr, parv[2], parv[3]);

  if (0 == ircd_strcmp(sub, "JOIN"))
    return bot_cmd_join(sptr, parv[2], parv[3]);

  if (0 == ircd_strcmp(sub, "PART"))
    return bot_cmd_part(sptr, parv[2], parv[3]);

  if (0 == ircd_strcmp(sub, "SAY")) {
    if (parc < 5 || EmptyString(parv[4]))
      return need_more_params(sptr, "BOT");
    return bot_cmd_say(sptr, parv[2], parv[3], parv[4]);
  }

  sendcmdto_one(&me, CMD_NOTICE, sptr,
                "%C :Unknown BOT subcommand %s; use CREATE, RENAME, DESTROY, "
                "JOIN, PART, SAY or LIST", sptr, sub);

  return 0;
}

/** Forget a bot the server is removing.
 *
 * Fires for every client that exits, ours or not; the list is short and
 * the lookup is by pointer.  It is what keeps a /KILL or a nick
 * collision from leaving a freed client on the list.
 *
 * @param[in,out] ctx hc_client is the client leaving, hc_arg the reason.
 * @param[in] user Unused.
 * @return HOOK_CONTINUE always; a notification has no veto.
 */
static enum HookResult bot_exiting(struct HookContext* ctx, void* user)
{
  (void) user;

  if (ctx->hc_client && bot_unlink(ctx->hc_client))
    log_write(LS_SYSTEM, L_INFO, 0, "Bot %s is gone: %s",
              cli_name(ctx->hc_client), ctx->hc_arg ? ctx->hc_arg : "");

  return HOOK_CONTINUE;
}

/** Register the command and the hook.
 * @param[in] mod Handle for this module.
 * @return Zero on success, non-zero to refuse the load.
 */
static int bot_init(struct ModuleHandle* mod)
{
  MessageHandler handlers[LAST_HANDLER_TYPE];

  handlers[UNREGISTERED_HANDLER] = NULL;
  handlers[CLIENT_HANDLER]       = m_not_oper;
  handlers[SERVER_HANDLER]       = NULL;
  handlers[OPER_HANDLER]         = mo_bot;
  handlers[SERVICE_HANDLER]      = NULL;

  /* The hook first: a command that could create a bot before the hook
   * that tracks its exit is in place would be a window, however brief.
   */
  if (!module_add_hook(mod, HOOK_CLIENT_EXITING, bot_exiting,
                       HOOK_PRIORITY_DEFAULT, NULL))
    return -1;

  /* Four parameters: subcommand, nick, target, and the message for SAY
   * as one trailing parameter with its spaces intact.
   */
  if (!module_add_command(mod, "BOT", NULL, 4, MFLG_SLOW, handlers))
    return -1;

  return 0;
}

/** Destroy every bot.
 *
 * Runs while the hook is still attached, so each exit_client() would
 * unlink its record anyway; unlinking first keeps this correct if that
 * order ever changes, and means the loop cannot see a record twice.
 *
 * @param[in] mod Handle for this module.
 */
static void bot_fini(struct ModuleHandle* mod)
{
  unsigned int n = bot_count;

  (void) mod;

  while (bot_list) {
    struct Client* bot = bot_list->b_client;

    bot_unlink(bot);
    exit_client(&me, bot, &me, "Bot module unloaded");
  }

  if (n)
    log_write(LS_SYSTEM, L_INFO, 0, "Destroyed %u bot%s on unload", n,
              n == 1 ? "" : "s");
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "m_bot",
  "1.0.0",
  "ircu developers",
  "Adds /BOT: virtual bots created on demand by network operators",
  bot_init,
  bot_fini,
  NULL
};
