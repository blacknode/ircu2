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
 * The bots themselves are the server's: include/bot.h introduces them,
 * lists them, and forgets one the network takes away.  This module is the
 * operator's handle on that API, and nothing more.
 *
 * A service bot -- one a module such as irc_services created with
 * BOT_SERVICE, the NickServ of the network -- shows up in /BOT LIST and
 * can be sent places and made to speak, but is neither renamed nor
 * destroyed from here: it answers to its module, and the module answers
 * to the configuration file.
 *
 * Unloading the module destroys the bots it created, and only those.
 */
#include "config.h"

#include "bot.h"
#include "channel.h"
#include "client.h"
#include "handlers.h"
#include "hash.h"
#include "ircd.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "module.h"
#include "msg.h"
#include "numeric.h"
#include "s_user.h"
#include "send.h"
#include "sys.h"

#include <string.h>

/** Our handle, so that the bots we create are owned by us. */
static struct ModuleHandle* bot_mod;

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

  if (!bot_find(acptr)) {
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :%s is not a bot of this server",
                  sptr, cli_name(acptr));
    return NULL;
  }

  return acptr;
}

/** Refuse an operation a service bot is protected from.
 * @param[in] sptr Operator to answer.
 * @param[in] bot The bot.
 * @param[in] what What was attempted, for the reply.
 * @return Non-zero, with a reply sent, if \a bot is a service bot.
 */
static int bot_refuse_service(struct Client* sptr, struct Client* bot,
                              const char* what)
{
  const struct Bot* b = bot_find(bot);

  if (!b || !(b->b_flags & BOT_SERVICE))
    return 0;

  sendcmdto_one(&me, CMD_NOTICE, sptr,
                "%C :%s is a service of the network and cannot be %s; it "
                "belongs to %s", sptr, cli_name(bot), what,
                b->b_owner ? module_name(b->b_owner) : "the server");
  return 1;
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

  if (!(bot = bot_create(bot_mod, nick, NULL, NULL, NULL, 0))) {
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

  if (chan) {
    bot_join(bot, chan);
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Bot %s joined %s", sptr,
                  cli_name(bot), chan);
  }

  return 0;
}

static int bot_cmd_destroy(struct Client* sptr, const char* nick)
{
  struct Client* bot;
  char name[NICKLEN + 1];

  if (!(bot = bot_lookup(sptr, nick)))
    return 0;

  if (bot_refuse_service(sptr, bot, "destroyed"))
    return 0;

  /* bot_destroy() frees the client; keep the name for the replies. */
  ircd_strncpy(name, cli_name(bot), NICKLEN);
  name[NICKLEN] = '\0';

  sendto_opmask_butone(0, SNO_OLDSNO, "%s destroyed bot %s", cli_name(sptr),
                       name);
  log_write(LS_SYSTEM, L_INFO, 0, "%#C destroyed bot %s", sptr, name);

  bot_destroy(bot, sptr, "Bot destroyed");

  sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Bot %s destroyed", sptr, name);

  return 0;
}

static int bot_cmd_rename(struct Client* sptr, const char* nick,
                          char* newnick)
{
  struct Client* bot;
  char oldnick[NICKLEN + 1];
  int err;

  if (!(bot = bot_lookup(sptr, nick)))
    return 0;

  if (bot_refuse_service(sptr, bot, "renamed"))
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

  bot_rename(bot, newnick);

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

  if (!(bot = bot_lookup(sptr, nick)))
    return 0;

  if (IsChannelName(target)) {
    struct Channel* chptr = FindChannel(target);

    if (!chptr)
      return send_reply(sptr, ERR_NOSUCHCHANNEL, target);
    bot_send_channel(bot, chptr, 0, text);
  } else {
    struct Client* acptr = FindUser(target);

    if (!acptr)
      return send_reply(sptr, ERR_NOSUCHNICK, target);
    bot_send_user(bot, acptr, 0, text);
  }

  return 0;
}

static int bot_cmd_list(struct Client* sptr)
{
  const struct Bot* b;
  unsigned int n = bot_count();

  for (b = bot_first(); b; b = b->b_next) {
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

    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  "%C :%s %s (%s@%s) of %s on %u channel%s%s%s", sptr,
                  (b->b_flags & BOT_SERVICE) ? "Service" : "Bot",
                  cli_name(bot), cli_user(bot)->username,
                  cli_user(bot)->host,
                  b->b_owner ? module_name(b->b_owner) : "the server",
                  cli_user(bot)->joined,
                  cli_user(bot)->joined == 1 ? "" : "s",
                  chans[0] ? ":" : "", chans);
  }

  sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :End of bot list: %u bot%s", sptr,
                n, n == 1 ? "" : "s");

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

  (void) cptr;

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
    return bot_cmd_destroy(sptr, parv[2]);

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

/** Register the command.
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

  bot_mod = mod;

  /* Four parameters: subcommand, nick, target, and the message for SAY
   * as one trailing parameter with its spaces intact.
   */
  if (!module_add_command(mod, "BOT", NULL, 4, MFLG_SLOW, handlers))
    return -1;

  return 0;
}

/** Destroy the bots this module created.
 *
 * The server would do it on unload anyway; doing it here means the quit
 * reason says why, and the count is logged.
 *
 * @param[in] mod Handle for this module.
 */
static void bot_fini(struct ModuleHandle* mod)
{
  const struct Bot* b;
  unsigned int n = 0;

  /* bot_destroy() unlinks the record, so start over after each one. */
  for (;;) {
    for (b = bot_first(); b && b->b_owner != mod; b = b->b_next)
      ;
    if (!b)
      break;
    bot_destroy(b->b_client, &me, "Bot module unloaded");
    n++;
  }

  if (n)
    log_write(LS_SYSTEM, L_INFO, 0, "Destroyed %u bot%s on unload", n,
              n == 1 ? "" : "s");

  bot_mod = NULL;
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "m_bot",
  "1.1.0",
  "ircu developers",
  "Adds /BOT: virtual bots created on demand by network operators",
  bot_init,
  bot_fini,
  NULL
};
