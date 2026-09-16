/*
 * IRC - Internet Relay Chat, modules/services/identity/identity.h
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
 * @brief Private declarations for the identity module.
 *
 * Not installed and not visible to anything else: the module is reached
 * through the provider it registers (include/account.h) and through
 * nothing else at all.
 */
#ifndef INCLUDED_identity_h
#define INCLUDED_identity_h

#include "account.h"
#include "cache.h"
#include "ircd_pwhash.h"
#include "sasl.h"

#include <sys/types.h>

struct ModuleHandle;
struct WorkTask;
struct DbResult;
struct json_t;

/** Environment variable the password pepper is read from.
 *
 * The environment and not the configuration file: a pepper is the one
 * secret whose whole purpose is to not be wherever the hashes are, and a
 * configuration file gets copied, diffed and pasted into a bug report.
 * It must be the same on every server, like the virtual host key.
 */
#define IDENT_PEPPER_ENV "IRCU_PASSWORD_PEPPER"

/** Longest pepper that is read.  Anything longer is a key, not a pepper. */
#define IDENT_PEPPER_MAX 256

/** Bytes of salt for a new hash. */
#define IDENT_SALT_LEN 16

/** What kind of question is in flight. */
enum IdentKind {
  IDENT_VERIFY,   /**< ap_verify(): is this credential good? */
  IDENT_LOOKUP,   /**< ap_lookup(): whose nickname is this? */
  IDENT_LIST,     /**< ap_list(): what does this address hold? */
  IDENT_CHANGE    /**< ap_change(): make it so. */
};

/** What crosses to a worker thread and back.
 *
 * Everything Argon2 needs and nothing else: no client, no request, no
 * pointer into anything the main thread might free.  Allocated with
 * worker_alloc() because it is freed on the far side of a queue.
 */
struct IdentPwWork {
  char   pw_hash[PWHASH_MAX + 1];        /**< The stored hash. */
  char   pw_newhash[PWHASH_MAX + 1];     /**< What a re-hash produced. */
  char   pw_secret[SASL_SECRET_MAX + 1]; /**< The password offered. */
  size_t pw_secretlen;                   /**< Its length. */
  char   pw_pepper[IDENT_PEPPER_MAX + 1];/**< Server-wide pepper, or "". */
  size_t pw_pepperlen;                   /**< Its length. */
  unsigned char pw_salt[IDENT_SALT_LEN]; /**< For a re-hash; from the core. */
  int    pw_rehash;                      /**< Make a hash instead of checking. */
  int    pw_ok;                          /**< Set by the worker. */
  int    pw_outdated;                    /**< Set by the worker. */
  long long pw_identity_id;              /**< Row a re-hash belongs to. */
  unsigned long pw_serial;               /**< Request, or 0 for a re-hash. */
};

/** One question this module has accepted and not yet answered. */
struct IdentRequest {
  struct IdentRequest* ir_next;    /**< Next, in no order. */
  unsigned long  ir_serial;        /**< This module's own handle. */
  account_id_t   ir_id;            /**< What the core holds. */
  enum IdentKind ir_kind;          /**< Which question. */
  int            ir_cancelled;     /**< The core has stopped caring. */

  /* --- what was asked --- */
  char ir_email[ACCOUNT_EMAIL_MAX + 1];     /**< Address, lower-cased. */
  char ir_authzid[NICKLEN + 1];             /**< Account asked for, or "". */
  char ir_mech[SASLMECHLEN + 1];            /**< "PLAIN", "EXTERNAL", ... */
  char ir_nick[NICKLEN + 1];                /**< Nickname, as asked about. */
  char ir_canon[NICKLEN + 1];               /**< Its canonical form. */
  char ir_fingerprint[80];                  /**< TLS fingerprint, for EXTERNAL. */

  /* --- what came back --- */
  long long ir_identity_id;                 /**< identity.id, or 0. */
  char ir_account[NICKLEN + 1];             /**< The account resolved. */
  int  ir_has_account;                      /**< A row named one. */
  int  ir_suspended;                        /**< And it is suspended. */

  /** The password, until it has been handed to a worker.  Allocated with
   * worker_alloc() at the start, so that the only copy this module ever
   * makes is the one that crosses the queue. */
  struct IdentPwWork* ir_work;

  /* --- a change --- */
  enum AccountWrite ir_what;   /**< Which write. */
  int  ir_max;                 /**< Accounts the address may hold. */
  /** The password being set, waiting for a worker to hash it.  A second
   * payload rather than a field, for ir_work's reason: it is going to
   * cross a queue, so it is allocated where it has to end up. */
  struct IdentPwWork* ir_make;
};

/** This module's handle; db_query() and cache_get() both want it. */
extern struct ModuleHandle* ident_mod;

/*
 * Requests.  identity.c.
 */

extern struct IdentRequest* ident_begin(account_id_t id, enum IdentKind kind);
extern struct IdentRequest* ident_find(unsigned long serial);
extern void ident_free(struct IdentRequest* req);
extern void ident_requests_clear(void);

/** Answer a request with a failure and release it. */
extern void ident_fail(struct IdentRequest* req, enum AccountResult result,
                       const char* reason);

/*
 * Helpers.  identity.c.
 */

/** Non-zero when there is a database to ask.  Logged once per failure. */
extern int ident_db_ready(const char* what);

/** Canonicalise a nickname the way the ircd compares them. */
extern void ident_nick_canon(const char* in, char* out, size_t len);

/** Lower-case an address, ASCII only. */
extern void ident_email_canon(const char* in, char* out, size_t len);

/** The server-wide pepper, or "" when there is none. */
extern const char* ident_pepper(size_t* len);

/** Fill in a worker payload's pepper from ident_pepper(). */
extern void ident_pw_set_pepper(struct IdentPwWork* work);

/** Release a worker payload, wiping the password first. */
extern void ident_pw_free(struct WorkTask* task);

/* Row accessors over what db.h hands back.  NULL and a missing column are
 * the same thing, which is what every caller here wants. */
extern unsigned int ident_rows(const struct DbResult* res);
extern const char* ident_row_str(const struct DbResult* res, unsigned int row,
                                 const char* column);
extern long long ident_row_int(const struct DbResult* res, unsigned int row,
                               const char* column);
extern int ident_row_bool(const struct DbResult* res, unsigned int row,
                          const char* column);
extern int ident_row_null(const struct DbResult* res, unsigned int row,
                          const char* column);

/*
 * The three questions.
 */

extern void ident_verify(account_id_t id, const struct AccountRequest* areq);
extern void ident_change(account_id_t id, const struct AccountChange* req);

/** Forget what the cache knows about a nickname.  Called after a write. */
extern void ident_forget_nick(const char* canon);

/** Fill in a worker payload with a password, ready to cross the queue.
 * @return The payload, or NULL if there is no memory.
 */
extern struct IdentPwWork* ident_pw_new(const char* secret, size_t len);
extern void ident_lookup(account_id_t id, const char* nick);
extern void ident_list(account_id_t id, const char* email);
extern void ident_cancel(account_id_t id);

#endif /* INCLUDED_identity_h */
