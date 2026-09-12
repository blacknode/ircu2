#ifndef INCLUDED_bot_h
#define INCLUDED_bot_h
/*
 * IRC - Internet Relay Chat, include/bot.h
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
 * A bot is a struct Client with no connection behind it.  It is made with
 * make_client(&me, ...), so it shares the server's own Connection the way
 * a remote user shares its uplink's.  That one choice does most of the
 * work: cli_from() of a bot is &me, which has no descriptor, so anything
 * addressed to it -- a PRIVMSG, a numeric, a channel it is on -- is
 * dropped in can_send() before it is queued; its server is &me, so it
 * takes a numnick from this server's own range and the network sees an
 * ordinary user of this server; and it is not MyConnect(), so
 * exit_client() never tries to close a socket that does not exist.
 * register_user(), set_nick_name(), the JoinBuf and the ModeBuf then
 * handle it like any other client, which is how it comes to speak P10
 * without this file knowing the protocol.
 *
 * Two things follow from sharing &me's Connection.  Nothing here writes a
 * per-connection field of a bot -- cli_handler(), cli_snomask(),
 * cli_privs() and their kind belong to &me -- and everything a bot says
 * goes straight to send.c rather than through the relay layer, whose flood
 * limits assert on a client that is not local.
 *
 * Two kinds of bot come out of bot_create().  A plain bot carries user
 * mode +B, a bot of the network, and is whatever its owner makes of it;
 * /BOT (modules/commands/m_bot.c) creates those on an operator's request.
 * A @b service bot (#BOT_SERVICE) is a service of the network -- NickServ,
 * ChanServ -- and carries +S, +k and +o as well: +k for the protections
 * the rest of the server already grants a channel service (no KICK, no
 * deop, no local KILL, no target limits), +o because a service acts with
 * an operator's standing and is exempt from what exempts one, and +S so
 * that a PRIVMSG or NOTICE sent to it is not merely dropped but handed
 * to the module that owns it through HOOK_MESSAGE_RECEIVED, and so that
 * /BOT will neither rename nor destroy it.  Only the server sets +B and
 * +S: a user may neither claim them nor shed them, and nor may an
 * operator.
 *
 * Every bot has an owner: the module that created it, or NULL for the
 * core.  Unloading a module destroys the bots it owns, so no bot outlives
 * the code that answers for it.  The server may also take a bot away
 * without asking -- a /KILL from another server, a nick collision -- and
 * an owner that wants to know attaches to HOOK_CLIENT_EXITING, where
 * bot_find() still answers for the client on its way out.
 *
 * All of this runs in the main thread; see doc/readme.services.
 */

struct Client;
struct Channel;
struct ModuleHandle;

/*
 * Flags for bot_create().
 */
/** A service of the network: +S +k +o, and protected from /BOT. */
#define BOT_SERVICE  0x0001

/** One bot the server has introduced. */
struct Bot {
  struct Client*       b_client;  /**< The client the server introduced. */
  struct ModuleHandle* b_owner;   /**< Module that created it; NULL for the core. */
  unsigned int         b_flags;   /**< Bitwise combination of BOT_* flags. */
  struct Bot*          b_next;    /**< Next bot, in creation order. */
};

/*
 * Checks a caller makes before creating or renaming.
 */

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
extern int bot_check_nick(const char* nick, const struct Client* self);

/** Check that \a name is a channel a bot may be put on.
 * @param[in] name Proposed channel name.
 * @return Zero if it is acceptable, otherwise the numeric to answer with.
 */
extern int bot_check_channel(const char* name);

/*
 * Creating and destroying.
 */

/** Bring a bot into existence and introduce it to the network.
 *
 * @param[in] owner Module creating it, or NULL for the core.
 * @param[in] nick Nick the bot gets; already checked with bot_check_nick().
 * @param[in] username Ident, or NULL for the nick in lower case.
 * @param[in] host Host, or NULL for <nick>.<BOT_HOSTNAME>, in lower case.
 * @param[in] info Real name, or NULL for "<NETWORK> bot".
 * @param[in] flags Bitwise combination of BOT_* flags.
 * @return The new client, or NULL if no numeric nick was free.
 */
extern struct Client* bot_create(struct ModuleHandle* owner, const char* nick,
                                 const char* username, const char* host,
                                 const char* info, unsigned int flags);

/** Make a bot quit and forget it.
 *
 * The QUIT reaches its channels and the network, HOOK_CLIENT_EXITING runs
 * with the bot still listed, and the client is freed: the pointer is
 * invalid once this returns.
 *
 * @param[in] bot The bot.
 * @param[in] sptr Who is destroying it, for the QUIT; &me if the server is.
 * @param[in] reason Quit reason.
 */
extern void bot_destroy(struct Client* bot, struct Client* sptr,
                        const char* reason);

/** Change a bot's nick.
 *
 * Runs the nick-change path of set_nick_name(): the channels are told,
 * WHOWAS is updated, the NICK goes to every server and
 * HOOK_CLIENT_NICK_CHANGED fires.
 *
 * @param[in] bot The bot.
 * @param[in] newnick New nick; already checked with bot_check_nick().
 */
extern void bot_rename(struct Client* bot, const char* newnick);

/*
 * Channels.
 */

/** Put a bot on a channel with ops, creating the channel if it must.
 * @param[in] bot The bot.
 * @param[in] name Channel name; already checked, and the bot not on it.
 */
extern void bot_join(struct Client* bot, const char* name);

/** Take a bot off a channel.
 * @param[in] bot The bot.
 * @param[in] chptr Channel it is on.
 */
extern void bot_part(struct Client* bot, struct Channel* chptr);

/*
 * Speaking.  Straight to send.c, with none of the checks the relay layer
 * applies to a local client's message: a bot speaks for the server, so
 * +n, +m and bans do not apply to it, any more than they apply to a
 * service.
 */

/** Send a PRIVMSG or NOTICE from a bot to a user.
 * @param[in] bot The bot.
 * @param[in] to Recipient, local or remote.
 * @param[in] notice Non-zero for a NOTICE, zero for a PRIVMSG.
 * @param[in] text What to say.
 */
extern void bot_send_user(struct Client* bot, struct Client* to, int notice,
                          const char* text);

/** Send a PRIVMSG or NOTICE from a bot to a channel.
 * @param[in] bot The bot.
 * @param[in] chptr The channel; the bot need not be on it.
 * @param[in] notice Non-zero for a NOTICE, zero for a PRIVMSG.
 * @param[in] text What to say.
 */
extern void bot_send_channel(struct Client* bot, struct Channel* chptr,
                             int notice, const char* text);

/** Change a user's modes on behalf of a bot.
 *
 * A service bot (+S) may change the modes of any user but an operator;
 * +r and -r -- identified to the nick in use, or no longer -- are the
 * ones it exists to set, and the rest are what a user may set on itself
 * (doc/readme.accounting).  Any bot may change its own modes within the
 * same limits.  The change is announced to the network and to the user
 * as coming from the bot.
 *
 * @param[in] bot The bot.
 * @param[in] target User whose modes change; the bot itself is allowed.
 * @param[in] modes Mode string, e.g. "+r" or "-r".
 * @return Non-zero if the bot was entitled to make the change, zero if
 * it was refused outright (target is an operator, bot is not +S).  A
 * non-zero return does not promise every letter took: modes only a
 * server may set are dropped silently.
 */
extern int bot_set_user_mode(struct Client* bot, struct Client* target,
                             const char* modes);

/*
 * Looking up.
 */

/** The record for a client, if it is a bot.
 * @param[in] cptr Any client.
 * @return Its record, or NULL if it is not a bot of this server.
 */
extern struct Bot* bot_find(const struct Client* cptr);

/** First bot in creation order; walk b_next for the rest. */
extern struct Bot* bot_first(void);

/** Number of bots. */
extern unsigned int bot_count(void);

/** Number of bots created with #BOT_SERVICE. */
extern unsigned int bot_service_count(void);

/*
 * Called by the server, never by modules.
 */

/** Hand a private message for a service bot to its owner.
 *
 * Runs HOOK_MESSAGE_RECEIVED with the bot as hc_client, the sender as
 * hc_source and the text as hc_arg.  Called from the relay layer once
 * every check the sender is subject to has passed; the message is not
 * delivered anywhere else, since a bot has nothing to deliver it to.
 *
 * @param[in] sptr Sender; a user, local or remote.
 * @param[in] bot Recipient; a service bot of this server.
 * @param[in] notice Non-zero for a NOTICE.
 * @param[in] text The message.
 */
extern void bot_deliver_private(struct Client* sptr, struct Client* bot,
                                int notice, const char* text);

/** Hand a channel message to every service bot of this server on the
 * channel, one HOOK_MESSAGE_RECEIVED per bot with hc_channel set.
 *
 * Cheap when there are no service bots or nobody is listening; otherwise
 * costs a walk of each service bot's channel list, which is short.
 *
 * @param[in] sptr Sender; a user, local or remote.
 * @param[in] chptr The channel.
 * @param[in] notice Non-zero for a NOTICE.
 * @param[in] text The message.
 */
extern void bot_deliver_channel(struct Client* sptr, struct Channel* chptr,
                                int notice, const char* text);

/** Forget a client the server is removing; from exit_one_client(). */
extern void bot_client_exiting(struct Client* cptr);

/** Destroy every bot a module owns; from module unload. */
extern void bot_drop_module(struct ModuleHandle* mod);

#endif /* INCLUDED_bot_h */
