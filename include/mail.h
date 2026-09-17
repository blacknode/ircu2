/*
 * IRC - Internet Relay Chat, include/mail.h
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
 * @brief Sending mail, and the token that proves an address was read.
 *
 * Two things that belong together and are not the same thing.
 *
 * **Verifying an address is the core's**, because it is a proof, like a
 * password is a proof: what it decides is whether a client may claim an
 * identity, and that decision is the same on every server of the network.
 * It is done without storing anything -- the token the server hands out
 * carries what it asserts and is signed, so any server can recognise it
 * again (see mail_token_make()).  That is what ircd_hmac_sha256() is for.
 *
 * **Delivering the mail is a module's.**  SMTP is a protocol with a
 * server at the other end, a configuration, a queue and a set of failure
 * modes that have nothing to do with IRC, and there is more than one way
 * to do it: hand it to the local MTA, speak SMTP to a relay, call somebody's
 * HTTP API.  So the core holds the @c Mail{} block and the messages in
 * flight and dispatches to one registered @b provider -- the arrangement
 * db.h and cache.h have, for the same two reasons: modules are
 * @c RTLD_LOCAL and cannot resolve each other's symbols, and holding the
 * callbacks here is what lets a provider be unloaded with messages
 * outstanding.  @c modules/workers/sendmail/ is the one that ships.
 *
 * Nothing here dereferences a @c struct @c Client, which is what lets the
 * whole file be tested without a server (@c mail_t).  Composing the
 * message a user is sent -- which needs their language and their address
 * -- is ircd/m_account.c's, the way m_authenticate.c owns what sasl.c
 * refuses to know.
 *
 * **Mail is never on the fast path.**  Nothing waits for it: a client that
 * asks to be verified is told the message was accepted for delivery, not
 * that it arrived, because the second answer takes a mail server and a
 * person reading their inbox.  A provider that is missing, misconfigured
 * or down costs the message and nothing else.
 */
#ifndef INCLUDED_mail_h
#define INCLUDED_mail_h

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

struct Client;
struct ModuleHandle;

/** Handle for one message in flight.  Zero is never a valid one. */
typedef unsigned long mail_id_t;

/** Longest address the core will carry, per RFC 5321. */
#define MAIL_ADDRESS_MAX 254
/** Longest subject line. */
#define MAIL_SUBJECT_MAX 200
/** Longest body.  A verification message is a paragraph and a token. */
#define MAIL_BODY_MAX 4096
/** Longest token mail_token_make() will produce. */
#define MAIL_TOKEN_MAX 512

/** Ceiling on the per-message deadline, in seconds.
 *
 * A provider that takes longer than this has not failed to send the
 * message -- it has failed to say whether it did, which is the same thing
 * to everybody waiting.
 */
#define MAIL_TIMEOUT_MAX 120

/** What went wrong, if anything. */
enum MailError {
  MAIL_OK,               /**< Accepted for delivery. */
  MAIL_ERR_NOPROVIDER,   /**< No provider is loaded. */
  MAIL_ERR_NOCONFIG,     /**< The Mail{} block is missing or unusable. */
  MAIL_ERR_ADDRESS,      /**< The address is not one this will send to. */
  MAIL_ERR_TOOLONG,      /**< Subject or body is too large. */
  MAIL_ERR_TIMEOUT,      /**< The provider did not answer in time. */
  MAIL_ERR_FAILED,       /**< The provider tried and could not. */
  MAIL_ERR_LAST          /**< Number of errors. */
};

/** Why a token was refused. */
enum MailToken {
  MAIL_TOKEN_OK,         /**< Signed by this network, and still valid. */
  MAIL_TOKEN_MALFORMED,  /**< Not a token this server ever issued. */
  MAIL_TOKEN_BAD,        /**< Well formed, wrong signature. */
  MAIL_TOKEN_EXPIRED,    /**< Signed by this network, too old. */
  MAIL_TOKEN_NOKEY       /**< No Security{} key, so nothing can be signed. */
};

/** One message to send.
 *
 * The core's, and it does not outlive the mp_send() call: a provider
 * copies what it needs.  The sender is not in it -- the core applies
 * @c Mail{from} itself, once, so that a provider cannot forget to and two
 * providers cannot disagree about it.
 */
struct MailMessage {
  const char* mm_to;      /**< Recipient address. */
  const char* mm_from;    /**< Sender, from the Mail{} block. */
  const char* mm_subject; /**< Subject line; no line breaks. */
  const char* mm_body;    /**< Body, plain text, LF-separated. */
};

/** What delivers the mail.  A module registers one of these. */
struct MailProvider {
  /** Short name, for logs and /STATS. */
  const char* mp_name;

  /** Send a message.
   *
   * Copies what it needs from \a msg -- which is the core's and does not
   * outlive the call -- and answers later with mail_complete(\a id, ...)
   * in the main thread.  Never blocks: talking to a mail server, or to a
   * program, is exactly the kind of work worker.h exists for.
   *
   * @param[in] id Handle to hand back to mail_complete().
   * @param[in] msg What to send.
   * @return #MAIL_OK when the message was queued.
   */
  enum MailError (*mp_send)(mail_id_t id, const struct MailMessage* msg);

  /** Forget a message.  The core has stopped caring about the answer.
   * @param[in] id Handle it was given.
   */
  void (*mp_cancel)(mail_id_t id);
};

/*
 * The register.
 */

/** Register the loaded module as the mail provider.
 *
 * One at a time: a second registration is refused rather than replacing
 * the first, so two mail modules in ircd.conf produce an error instead of
 * a coin flip.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] provider Static description of the provider.
 * @return Non-zero on success.
 */
extern int mail_register_provider(struct ModuleHandle* mod,
                                  const struct MailProvider* provider);

/** Withdraw the provider.
 *
 * Every message in flight is failed with #MAIL_ERR_NOPROVIDER before this
 * returns, so no caller is left waiting for a module that has gone.
 * @param[in] mod Handle that registered it.
 */
extern void mail_unregister_provider(struct ModuleHandle* mod);

/** Non-zero if a provider is registered and the Mail{} block is usable.
 *
 * What a caller asks before offering to send anything: an offer to mail
 * somebody on a server that cannot is worse than no offer.
 */
extern int mail_available(void);

/** Name of the registered provider, or "none". */
extern const char* mail_provider_name(void);

/*
 * Sending.
 */

/** Called when a message is answered, one way or another.
 *
 * @param[in] err #MAIL_OK, or why it did not go.
 * @param[in] detail What the provider said, or NULL.
 * @param[in] user Opaque pointer the caller passed in.
 */
typedef void (*MailDoneFn)(enum MailError err, const char* detail,
                           void* user);

/** Send a message.
 *
 * @param[in] mod Module asking, or NULL for the core.  Its messages are
 *   failed if it is unloaded before they are answered.
 * @param[in] to Recipient address.
 * @param[in] subject Subject line.
 * @param[in] body Body text.
 * @param[in] fn Called with the answer, or NULL to fire and forget.
 * @param[in] user Opaque pointer for \a fn.
 * @return The handle, or 0 when the message was refused outright -- no
 *   provider, no configuration, a bad address -- in which case \a fn is
 *   not called and mail_last_error() says why.
 */
extern mail_id_t mail_send(struct ModuleHandle* mod, const char* to,
                           const char* subject, const char* body,
                           MailDoneFn fn, void* user);

/** Why the last mail_send() returned zero. */
extern enum MailError mail_last_error(void);

/** Answer a message.  Main thread only.
 *
 * Consumes the handle: \a id is invalid afterwards, and the provider must
 * call this exactly once for every mp_send() it accepted.  A handle the
 * core no longer holds -- the deadline passed, the caller went away -- is
 * not an error; the call does nothing and says so.
 *
 * @param[in] id Handle from #MailProvider::mp_send.
 * @param[in] err What happened.
 * @param[in] detail Text for the log, or NULL.
 * @return Non-zero if the handle was outstanding.
 */
extern int mail_complete(mail_id_t id, enum MailError err,
                         const char* detail);

/** Drop every message a module is waiting on, without answering any. */
extern void mail_cancel_module(struct ModuleHandle* mod);

/** Fail every message whose deadline has passed.
 * @param[in] now Current time.
 * @return How many expired.
 */
extern int mail_expire(time_t now);

/** Messages currently in flight. */
extern unsigned int mail_pending_count(void);

/** Text for an error, for a log line or a reason. */
extern const char* mail_strerror(enum MailError err);

/** Non-zero if \a email is an address this server will send to.
 *
 * Not an attempt at RFC 5322: one address, no header smuggling, short
 * enough to store.  Whether it exists is what the message is for.
 */
extern int mail_address_valid(const char* email);

/*
 * The token.
 *
 * Signed, not stored.  A token says "this address was reachable, and I
 * said so at this time", and the signature is what makes it recognisable
 * again on any server of the network -- because the key is derived from
 * the Security{} key, which is the same on every one of them by
 * construction (doc/readme.accounting).  Nothing is written down: there
 * is no table of outstanding tokens to keep, to expire, or to leak.
 *
 * The cost of not storing them is that a token cannot be revoked before
 * it expires, which is why the window is short and why what it grants is
 * one thing only: the address is marked verified.
 */

/** Mint a token for \a email.
 *
 * @param[out] buf Where to write it.
 * @param[in] len Size of \a buf; #MAIL_TOKEN_MAX is always enough.
 * @param[in] email Address the token is about.
 * @param[in] now Current time.
 * @param[in] window How long it is good for, in seconds.
 * @return Non-zero on success, zero when there is no key or no room.
 */
extern int mail_token_make(char* buf, size_t len, const char* email,
                           time_t now, int window);

/** Check a token and say what address it is about.
 *
 * @param[in] token The token, as the user pasted it back.
 * @param[out] email Where to write the address; #MAIL_ADDRESS_MAX + 1
 *   bytes are always enough.  Untouched unless #MAIL_TOKEN_OK.
 * @param[in] len Size of \a email.
 * @param[in] now Current time.
 * @return #MAIL_TOKEN_OK, or why not.
 */
extern enum MailToken mail_token_check(const char* token, char* email,
                                       size_t len, time_t now);

/** Ask for a verification message, or hand one's token back.
 *
 * The client-facing half, and it lives in ircd/m_account.c with the rest
 * of what needs a client: composing the message (in the user's language),
 * rationing how often one is sent, and telling them what happened.  It is
 * declared here because it is the other end of the token above, and
 * because a service that offers the same thing in its own words -- as
 * nickserv does -- has to come back through it, so that there is one path
 * from a token to a verified address.
 *
 * @param[in] cptr Local client asking.
 * @param[in] token What they pasted back, or NULL to have one sent to the
 *   address they authenticated with.
 * @return Zero.
 */
extern int account_verify_request(struct Client* cptr, const char* token);

/*
 * Configuration.  The Mail{} block, held by the core and read by the
 * provider, exactly as the Database{} and Redis{} blocks are.
 */

/** What ircd.conf says about mail. */
struct MailConf {
  char* mconf_from;        /**< Sender address on every message. */
  char* mconf_program;     /**< Local program to hand a message to. */
  char* mconf_verify_url;  /**< Link template, or NULL for a bare token. */
  int   mconf_timeout;     /**< Per-message deadline, seconds, clamped. */
  int   mconf_window;      /**< How long a token is good for, seconds. */
  int   mconf_resend;      /**< Shortest gap between two messages to one
                                client, in seconds. */
  unsigned int mconf_generation; /**< Bumped every time this changes. */
};

/** The configuration, or NULL if ircd.conf has no @c Mail{} block. */
extern const struct MailConf* mail_conf(void);

/** Reset the configuration; the parser calls this for each @c Mail{}. */
extern void mail_conf_clear(void);
/** Set the sender address. */
extern void mail_conf_set_from(char* from);
/** Set the local program. */
extern void mail_conf_set_program(char* program);
/** Set the verification link template. */
extern void mail_conf_set_verify_url(char* url);
/** Set the per-message deadline, in seconds; clamped on commit. */
extern void mail_conf_set_timeout(int seconds);
/** Set how long a token is good for, in seconds. */
extern void mail_conf_set_window(int seconds);
/** Set the shortest gap between two messages to one client. */
extern void mail_conf_set_resend(int seconds);
/** Forget that a block was seen; the parser calls this before a rehash. */
extern void mail_conf_unmark(void);
/** Drop the configuration if the rehash did not bring a block back. */
extern void mail_conf_sweep(void);

/** Publish the block being read.
 * @param[out] error Set to why it was refused.
 * @return Non-zero if the block is usable.
 */
extern int mail_conf_commit(const char** error);

/** Release the register and the configuration; main() only, at exit. */
extern void mail_close(void);

#endif /* INCLUDED_mail_h */
