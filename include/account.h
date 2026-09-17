/*
 * IRC - Internet Relay Chat, include/account.h
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
 * @brief Accounts: asking whether a credential is good, and acting on it.
 *
 * An account @b is a nickname (proposal 007).  There is no separate account
 * name to keep in step with the nick, no mapping to maintain, and umode
 * @c +r keeps the literal meaning it has always had here: identified to the
 * nick in use.  What this file adds is a way for a user to earn it.
 *
 * The core does not know whether a password is right.  That depends on a
 * database it should not have to know about and on rules that differ by
 * deployment -- expiry, a second factor, single sign-on, suspensions -- so
 * one module registers as the @b provider and answers.  The shape is
 * db_register_driver()'s, for the same two reasons: modules are
 * @c RTLD_LOCAL and cannot resolve each other's symbols, so the core is the
 * meeting point; and holding the outstanding questions here is what lets
 * that module be unloaded with some still in flight.
 *
 * The file is in two halves, split the way migration.c is split from
 * migration_run.c.  This one -- ircd/account.c -- is the register, the
 * table of questions in flight and the guest-name generator: it never
 * dereferences a @c struct @c Client, which is what lets it be tested
 * without a server (@c account_t).  ircd/account_user.c is the other half,
 * what an answer does to a user: granting @c +r, renaming, logging out.
 */
#ifndef INCLUDED_account_h
#define INCLUDED_account_h

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif
#ifndef INCLUDED_ircd_defs_h
#include "ircd_defs.h"
#endif

struct Client;
struct ModuleHandle;

/** Handle for one question in flight.  Zero is never a valid one. */
typedef unsigned long account_id_t;

/** Longest address the core will carry, per RFC 5321. */
#define ACCOUNT_EMAIL_MAX 254

/** What the provider decided. */
enum AccountResult {
  ACCOUNT_OK,              /**< The credential is good. */
  ACCOUNT_ERR_CREDENTIAL,  /**< No such identity, or the secret is wrong. */
  ACCOUNT_ERR_NOSUCH,      /**< That identity has no such account. */
  ACCOUNT_ERR_SUSPENDED,   /**< The account exists and is suspended. */
  ACCOUNT_ERR_INUSE,       /**< Somebody else is using the account's nick. */
  ACCOUNT_ERR_EXISTS,      /**< That nickname is registered already. */
  ACCOUNT_ERR_LIMIT,       /**< The address already holds as many as it may. */
  ACCOUNT_ERR_UNAVAILABLE, /**< No provider, or it went away mid-question. */
  ACCOUNT_ERR_TIMEOUT,     /**< The provider took too long. */
  ACCOUNT_ERR_LAST         /**< Number of results. */
};

/** What a change is asking for. */
enum AccountWrite {
  ACCOUNT_WRITE_REGISTER,  /**< Take this nickname for this address. */
  ACCOUNT_WRITE_PASSWD,    /**< Replace this address's password. */
  ACCOUNT_WRITE_DROP,      /**< Give this nickname up. */
  /** Record that this address was read by whoever holds it.
   *
   * The odd one out, and deliberately: it carries no password, because
   * the proof is not a password.  It is a token the core signed and the
   * holder of the address sent back (mail.h), which is a proof of the
   * same kind and checked before the provider is asked at all.
   */
  ACCOUNT_WRITE_VERIFY
};

/** What is being asked about a nickname. */
enum AccountOwner {
  ACCOUNT_NICK_FREE,       /**< Nobody has registered it. */
  ACCOUNT_NICK_REGISTERED, /**< Somebody has; it must be proved or given up. */
  ACCOUNT_NICK_UNKNOWN     /**< Could not be established.  See below. */
};

/** One account in a listing.
 *
 * What ACCOUNT LIST shows: the nickname, and whether it is the address's
 * default.  Whether it is the one in use is the core's own knowledge, not
 * the provider's, so it is not here.
 */
struct AccountEntry {
  const char* ae_nick;    /**< The account; a nickname. */
  int         ae_default; /**< Non-zero if this is the address's default. */
};

/** One credential to check.
 *
 * The core's, and it does not outlive the ap_verify() call: a provider
 * copies what it needs.  The secret is wiped as soon as ap_verify()
 * returns, so a provider that keeps a pointer to it keeps a pointer to
 * zeroes -- which is the failure one notices, rather than a password
 * lingering in the heap, which is not.
 */
struct AccountRequest {
  const char* ar_mech;        /**< "PLAIN", "EXTERNAL", "ACCOUNT". */
  const char* ar_authcid;     /**< The address; "" for EXTERNAL. */
  const char* ar_authzid;     /**< Which account, or "" for the default. */
  const char* ar_secret;      /**< The proof; may hold NULs. */
  size_t      ar_secretlen;   /**< Its length. */
  const char* ar_fingerprint; /**< TLS certificate fingerprint, or "". */
  const char* ar_ip;          /**< Textual address the client came from. */
  int         ar_tls;         /**< Non-zero if the connection is TLS. */
};

/** One change to make, and the proof that it may be made.
 *
 * One request for all three kinds of write, the shape #DbQuery has, for
 * the same reason: a pointer per operation in #AccountProvider would be
 * three things to implement, three to document and three to get wrong,
 * where what differs between them is which fields are filled in.
 *
 * The core's, and it does not outlive the ap_change() call.  Every write
 * carries the current password, because every write is an act only the
 * holder of the address may perform and the provider is the only thing
 * that can tell whether this is the holder -- except for two: a
 * registration of an address nobody has yet, where the password given is
 * the one being set, and #ACCOUNT_WRITE_VERIFY, whose proof the core has
 * already checked itself.
 */
struct AccountChange {
  enum AccountWrite ach_what;      /**< Which change. */
  const char* ach_email;           /**< The address.  Always. */
  const char* ach_secret;          /**< The current password. */
  size_t      ach_secretlen;       /**< Its length. */
  const char* ach_nick;            /**< Account, for REGISTER and DROP. */
  const char* ach_new;             /**< New password, for PASSWD. */
  size_t      ach_newlen;          /**< Its length. */
  /** Accounts this address may hold, or 0 for no limit.
   *
   * Policy, and it travels with the request rather than living in the
   * provider, because the service that administers the accounts is what
   * decides it -- and because counting them and then inserting is a race
   * between two servers unless the count and the insert are the same
   * transaction, which is the provider's to arrange.
   */
  int         ach_max;
  const char* ach_ip;              /**< Where the client came from. */
  int         ach_tls;             /**< Non-zero if the link is TLS. */
};

/** What answers the questions.  A module registers one of these. */
struct AccountProvider {
  /** Short name, for logs and /STATS. */
  const char* ap_name;

  /** Check a credential.
   *
   * Copies what it needs from \a req -- which is the core's and does not
   * outlive the call -- and answers later with account_complete(\a id, ...)
   * in the main thread.  Never blocks: the password hash belongs on a
   * worker (see ircd_pwhash.h) and the query belongs in db.h.
   *
   * @param[in] id Handle to hand back to account_complete().
   * @param[in] req What to check.
   */
  void (*ap_verify)(account_id_t id, const struct AccountRequest* req);

  /** Find out whether a nickname is registered.
   *
   * Same contract: answer later with account_complete_owner().
   * @param[in] id Handle to hand back.
   * @param[in] nick Nickname to ask about.
   */
  void (*ap_lookup)(account_id_t id, const char* nick);

  /** List the accounts an address holds.
   *
   * Same contract: answer later with account_complete_list().
   * @param[in] id Handle to hand back.
   * @param[in] email Address to list, which the caller has authenticated.
   */
  void (*ap_list)(account_id_t id, const char* email);

  /** Make a change to the store.
   *
   * Same contract as ap_verify(): copies what it needs, never blocks, and
   * answers later with account_complete().  The secret is wiped as soon
   * as this returns.
   *
   * @param[in] id Handle to hand back.
   * @param[in] req What to change.
   */
  void (*ap_change)(account_id_t id, const struct AccountChange* req);

  /** Forget a question.  The core has stopped caring about the answer.
   * @param[in] id Handle it was given.
   */
  void (*ap_cancel)(account_id_t id);
};

/*
 * The register.
 */

/** Register the loaded module as the identity provider.
 *
 * One at a time: a second registration is refused rather than replacing
 * the first, so two identity modules in ircd.conf produce an error instead
 * of a coin flip.
 * @param[in] mod Handle passed to mi_init.
 * @param[in] provider Static description of the provider.
 * @return Non-zero on success.
 */
extern int account_register_provider(struct ModuleHandle* mod,
                                     const struct AccountProvider* provider);

/** Withdraw the provider.
 *
 * Every question in flight is failed with #ACCOUNT_ERR_UNAVAILABLE before
 * this returns, so no caller is left waiting for a module that has gone.
 * @param[in] mod Handle that registered it.
 */
extern void account_unregister_provider(struct ModuleHandle* mod);

/** Non-zero if a provider is registered and can be asked. */
extern int account_have_provider(void);

/** Name of the registered provider, or "none". */
extern const char* account_provider_name(void);

/*
 * Asking.
 */

/** Called when a question is answered, one way or another.
 *
 * @param[in] cptr Client it was about, or NULL if it has since left.
 * @param[in] result What the provider decided.
 * @param[in] nick Account nickname, or NULL.
 * @param[in] email Address of the identity, or NULL.
 * @param[in] reason Text to show the user, or NULL for the default.
 * @param[in] data Opaque pointer the caller passed in.
 */
typedef void (*AccountDoneFn)(struct Client* cptr, enum AccountResult result,
                              const char* nick, const char* email,
                              const char* reason, void* data);

/** Called when a nickname lookup is answered.
 *
 * @param[in] cptr Client it was about, or NULL if it has since left.
 * @param[in] owner What was established about the nickname.
 * @param[in] nick The nickname that was asked about.
 * @param[in] data Opaque pointer the caller passed in.
 */
typedef void (*AccountOwnerFn)(struct Client* cptr, enum AccountOwner owner,
                               const char* nick, void* data);

/** Called when a listing is answered.
 *
 * @param[in] cptr Client it was about, or NULL if it has since left.
 * @param[in] result #ACCOUNT_OK, or why there is no listing.
 * @param[in] entries The accounts, or NULL.  The provider's, and it does
 *   not outlive the call: a caller that keeps one keeps a dangling pointer.
 * @param[in] count How many.
 * @param[in] reason Text to show the user, or NULL for the default.
 * @param[in] data Opaque pointer the caller passed in.
 */
typedef void (*AccountListFn)(struct Client* cptr, enum AccountResult result,
                              const struct AccountEntry* entries,
                              unsigned int count, const char* reason,
                              void* data);

/** Ask whether a credential is good.
 *
 * @param[in] cptr Client it is about; the question is dropped if it leaves.
 * @param[in] req The credential.  Not kept.
 * @param[in] done Called with the answer, with the deadline, or when the
 *   provider goes away.  Never called before this returns.
 * @param[in] data Opaque pointer for \a done.
 * @return The handle, or 0 if there is no provider -- in which case \a done
 *   is not called and the caller reports #ACCOUNT_ERR_UNAVAILABLE itself.
 */
extern account_id_t account_verify(struct Client* cptr,
                                   const struct AccountRequest* req,
                                   AccountDoneFn done, void* data);

/** Ask who a nickname belongs to.
 *
 * The answer is #ACCOUNT_NICK_UNKNOWN when it could not be established --
 * no provider, the database unreachable, the deadline passed.  That is not
 * the same as free, and the caller must not treat it as such: the one
 * moment an impostor gets somebody else's nickname is the moment the
 * service protecting it is not answering, and that moment can be arranged.
 * See proposal 007 section 7.
 *
 * @param[in] cptr Client it is about, or NULL.
 * @param[in] nick Nickname to ask about.
 * @param[in] done Called with the answer.
 * @param[in] data Opaque pointer for \a done.
 * @return The handle, or 0 if there is no provider.
 */
extern account_id_t account_lookup(struct Client* cptr, const char* nick,
                                   AccountOwnerFn done, void* data);

/** Ask for a change to the store.
 *
 * @param[in] cptr Client it is about; the request is dropped if it leaves
 *   -- the change may still happen, because a write that has reached the
 *   database is not something the core can take back; what is dropped is
 *   the answer.
 * @param[in] req What to change.  Not kept.
 * @param[in] done Called with the answer.
 * @param[in] data Opaque pointer for \a done.
 * @return The handle, or 0 if there is no provider.
 */
extern account_id_t account_change(struct Client* cptr,
                                   const struct AccountChange* req,
                                   AccountDoneFn done, void* data);

/** Ask what accounts an address holds.
 *
 * The address is not a search key the caller may invent: it is the one the
 * client proved was its own by authenticating with it, which is why
 * ACCOUNT LIST refuses a client that has not.  Asking this about an
 * arbitrary address would be an account enumerator.
 *
 * @param[in] cptr Client it is about; the question is dropped if it leaves.
 * @param[in] email The address, as authenticated.
 * @param[in] done Called with the answer.
 * @param[in] data Opaque pointer for \a done.
 * @return The handle, or 0 if there is no provider.
 */
extern account_id_t account_list(struct Client* cptr, const char* email,
                                 AccountListFn done, void* data);

/** Answer a credential check.  Main thread only.
 *
 * Consumes the handle: \a id is invalid afterwards, and the provider must
 * call this exactly once for every ap_verify() it accepted.  A handle the
 * core no longer holds -- the client left, the deadline passed -- is not
 * an error; the call does nothing and says so.
 *
 * @param[in] id Handle from #AccountProvider::ap_verify.
 * @param[in] result What was decided.
 * @param[in] nick Account nickname on success.
 * @param[in] email Address of the identity on success.
 * @param[in] reason Text for the user, or NULL for the default.
 * @return Non-zero if the handle was outstanding.
 */
extern int account_complete(account_id_t id, enum AccountResult result,
                            const char* nick, const char* email,
                            const char* reason);

/** Answer a nickname lookup.  Main thread only.
 * @param[in] id Handle from #AccountProvider::ap_lookup.
 * @param[in] owner What was established.
 * @return Non-zero if the handle was outstanding.
 */
extern int account_complete_owner(account_id_t id, enum AccountOwner owner);

/** Answer a listing.  Main thread only.
 * @param[in] id Handle from #AccountProvider::ap_list.
 * @param[in] result #ACCOUNT_OK, or why there is no listing.
 * @param[in] entries The accounts; not kept past this call.
 * @param[in] count How many.
 * @param[in] reason Text for the user, or NULL for the default.
 * @return Non-zero if the handle was outstanding.
 */
extern int account_complete_list(account_id_t id, enum AccountResult result,
                                 const struct AccountEntry* entries,
                                 unsigned int count, const char* reason);

/** Drop every question about a client, without answering any.
 *
 * Called when it leaves: there is nobody left for the answer to be about,
 * and the callback must not run on a client that is being freed.
 */
extern void account_cancel_client(struct Client* cptr);

/** Fail every question whose deadline has passed.
 * @param[in] now Current time.
 * @return How many expired.
 */
extern int account_expire(time_t now);

/** Questions currently in flight. */
extern unsigned int account_pending_count(void);

/** Text for a result, for a log line or a default reason. */
extern const char* account_strerror(enum AccountResult result);

/** Release the register; main() only, at exit. */
extern void account_close(void);

/*
 * Guest names.
 */

/** Write a guest nickname into \a buf.
 *
 * FEAT_GUEST_PREFIX plus random base 62.  The random part is what makes
 * the collision that account_force_guest() answers with a KILL something
 * that does not happen rather than something that is handled.
 *
 * @param[out] buf Buffer for the nickname.
 * @param[in] len Its size, terminator included.
 * @return Non-zero on success; zero if the prefix leaves no room.
 */
extern int account_guest_nick(char* buf, size_t len);

/*
 * What an answer does to a user.  ircd/account_user.c.
 */

/** Grant \a cptr the account \a nick, renaming it if it is not already
 * called that.
 *
 * Granting +r and taking the nickname are one act: an account is a
 * nickname, so a client identified to an account it is not using is a
 * state the model does not define.  Either both happen or neither does.
 *
 * @param[in,out] cptr Client that authenticated; must be local.
 * @param[in] nick The account.
 * @param[in] email Address of the identity, or NULL.
 * @return Non-zero on success; zero if the nickname could not be taken, in
 *   which case nothing was changed.
 */
extern int account_login(struct Client* cptr, const char* nick,
                         const char* email);

/** Take the nickname an account owns, without granting +r.
 *
 * For a client that authenticated before it was registered: it is not a
 * user yet, so it cannot carry a user mode, but it can have the right
 * name -- which is what gets it introduced to the network as itself
 * instead of being renamed a moment after it arrives.  account_login()
 * grants the mode once registration is done.
 *
 * @param[in,out] cptr Client; must be local.
 * @param[in] nick The account.
 * @return Non-zero if the client now has that nickname.
 */
extern int account_claim_nick(struct Client* cptr, const char* nick);

/** Log \a cptr out: clear +r and the address, and rename it to a guest.
 *
 * The rename is not a flourish.  Leaving the account nickname on a client
 * that no longer holds +r is exactly the state an onlooker cannot tell
 * apart from an impostor, so the nickname goes back with the session.
 *
 * @param[in,out] cptr Client to log out; must be local.
 * @return Zero if it was not logged in, 1 if it is now logged out, and
 *   CPTR_KILLED if the guest nickname was somehow taken and the client is
 *   gone -- so a caller must test for that before touching it again.
 */
extern int account_logout(struct Client* cptr);

/** Rename \a cptr to a guest nickname, or kill it if that name is taken.
 *
 * Used wherever a client may not keep the nickname it is using: a failed
 * authentication, a logout, a grace period that ran out, a nickname whose
 * ownership could not be established.
 *
 * @param[in,out] cptr Client to rename; must be local.
 * @param[in] reason Why, for the KILL if it comes to that.
 * @return Zero if the client is still here, CPTR_KILLED if it is not.
 */
extern int account_force_guest(struct Client* cptr, const char* reason);

/** Rename every frozen local client to a guest name.
 *
 * The safeguard of proposal 007 section 6.  A frozen client is one whose
 * nickname nobody has proved is its own, and the provider is what would
 * have taken the proof; when it goes, nothing can ever lift the freeze.
 * The answer is not to unfreeze them where they stand -- that would leave
 * a possible impostor holding the name with nobody watching, which is
 * exactly what section 7 forbids -- but to take the name away.
 *
 * Called from account_unregister_provider(), which is in the other half of
 * the file and may not touch a client.
 */
extern void account_provider_gone(void);

/** Forget everything about a client that is leaving. */
extern void account_client_exiting(struct Client* cptr);

#endif /* INCLUDED_account_h */
