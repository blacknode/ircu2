/*
 * IRC - Internet Relay Chat, include/sasl.h
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
 * @brief SASL mechanisms: the register, and the exchange with one client.
 *
 * This file is the *shape* of an authentication, not the decision.  It
 * turns the AUTHENTICATE lines a client sends into a credential -- who
 * says they are whom, which of their accounts they want, and what they
 * offer as proof -- and stops there.  Whether the proof is good is the
 * identity module's business (see include/account.h and proposal 007):
 * the mechanism is the same on every server, and the answer is not.
 *
 * Nothing here knows about a @c struct @c Client or a socket.  The caller
 * copies the two things a mechanism may need about the connection -- that
 * it is TLS, and the certificate fingerprint -- into the session before
 * starting it, and ircd/m_authenticate.c does the talking.  That is what
 * lets the whole state machine be tested without a server (@c sasl_t),
 * the same split as migration.c against migration_run.c.
 *
 * The register is a run-time list, like the capabilities in capab.c and
 * the user and channel modes: the core brings PLAIN and EXTERNAL, and a
 * module adds SCRAM-SHA-256 or OAUTHBEARER with
 * module_add_sasl_mechanism() without the core learning their names.
 */
#ifndef INCLUDED_sasl_h
#define INCLUDED_sasl_h

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif
#ifndef INCLUDED_ircd_defs_h
#include "ircd_defs.h"
#endif
#ifndef INCLUDED_ircd_base64_h
#include "ircd_base64.h"
#endif

struct ModuleHandle;

/** Longest mechanism name, per RFC 4422 section 3.1. */
#define SASLMECHLEN 20
/** Longest decoded message the server will assemble, in bytes. */
#define SASL_MESSAGE_MAX 1024
/** Longest encoded message, which is what arrives on the wire. */
#define SASL_WIRE_MAX IRCD_BASE64_ENCLEN(SASL_MESSAGE_MAX)
/** Characters of base64 per AUTHENTICATE line; exactly this many means
 * the message continues on the next one.  Fixed by the IRCv3 SASL spec. */
#define SASL_CHUNKLEN 400
/** Longest authentication identity: an address, per RFC 5321. */
#define SASL_AUTHCID_MAX 254
/** Longest secret a mechanism will carry. */
#define SASL_SECRET_MAX 300

/** This mechanism sends a secret the connection does not protect.
 *
 * PLAIN puts the password on the wire as it is, so the server refuses it
 * without TLS unless an operator has deliberately said otherwise.
 */
#define SASL_MECH_NEEDS_TLS 0x0001

/** What one step of a mechanism decided. */
enum SaslStep {
  SASL_STEP_CHALLENGE,   /**< SaslSession::ss_out holds a challenge. */
  SASL_STEP_CREDENTIAL,  /**< The session now holds what must be checked. */
  SASL_STEP_FAIL         /**< Malformed; the exchange is over. */
};

/** What arrived on one AUTHENTICATE line. */
enum SaslResult {
  SASL_NEED_MORE,   /**< A full chunk; the message continues. */
  SASL_CHALLENGE,   /**< Send SaslSession::ss_out back to the client. */
  SASL_CREDENTIAL,  /**< Ready to be verified. */
  SASL_ABORTED,     /**< The client sent '*'. */
  SASL_TOO_LONG,    /**< More than #SASL_MESSAGE_MAX bytes. */
  SASL_BAD_INPUT    /**< Not base64, or the mechanism refused it. */
};

struct SaslSession;

/** One step of a mechanism.
 *
 * @param[in,out] ses Session, with the connection's details already in it.
 *   The step fills in SaslSession::ss_authcid, ss_authzid and ss_secret,
 *   or ss_out for a challenge.
 * @param[in] in Decoded bytes from the client; never NULL, possibly empty.
 * @param[in] inlen How many.
 * @return What it decided.
 */
typedef enum SaslStep (*SaslStepFn)(struct SaslSession* ses, const char* in,
                                    size_t inlen);

/** One registered mechanism.
 *
 * Public because a module walks the register through sasl_first();
 * changing it changes the module ABI.
 */
struct SaslMechanism {
  char                 sm_name[SASLMECHLEN + 1]; /**< Uppercase, on the wire. */
  unsigned int         sm_flags;      /**< SASL_MECH_* flags. */
  SaslStepFn           sm_step;       /**< One round of the exchange. */
  struct ModuleHandle* sm_owner;      /**< Module, or NULL for the core. */
  struct SaslMechanism* sm_next;      /**< Next, sorted by name. */
};

/** One client's exchange in progress.
 *
 * Holds a password in the clear for as long as it takes to hand it to the
 * provider, which is why sasl_session_clear() wipes rather than frees and
 * why every path out of the exchange goes through it.
 */
struct SaslSession {
  const struct SaslMechanism* ss_mech;  /**< Mechanism, or NULL if none. */

  /* --- about the connection; set by the caller before starting --- */
  int  ss_tls;                          /**< Non-zero if the link is TLS. */
  char ss_fingerprint[65];              /**< Certificate fingerprint, or "". */

  /* --- the message being assembled --- */
  char   ss_wire[SASL_WIRE_MAX + 1];    /**< Base64 seen so far. */
  size_t ss_wirelen;                    /**< Its length. */
  int    ss_steps;                      /**< Rounds completed. */

  /* --- what the mechanism produced --- */
  char   ss_authcid[SASL_AUTHCID_MAX + 1]; /**< Who they say they are. */
  char   ss_authzid[NICKLEN + 1];          /**< Which account; may be "". */
  char   ss_secret[SASL_SECRET_MAX + 1];   /**< The proof. */
  size_t ss_secretlen;                     /**< Its length; it may hold NULs. */

  char   ss_out[SASL_MESSAGE_MAX + 1];  /**< A challenge for the client. */
  size_t ss_outlen;                     /**< Its length. */
};

/*
 * The register.
 */

/** Populate the register with the core's mechanisms.  Called once. */
extern void sasl_init(void);
/** Release the register; main() only, at exit. */
extern void sasl_close(void);

/** First registered mechanism, for iteration; sorted by name. */
extern const struct SaslMechanism* sasl_first(void);
/** Find a mechanism by name, case-insensitively.  NULL if there is none. */
extern const struct SaslMechanism* sasl_find(const char* name);
/** Number of mechanisms currently registered. */
extern unsigned int sasl_count(void);

/** Register a mechanism.
 *
 * @param[in] mod Module registering it; NULL for the core.
 * @param[in] name Name as it goes on the wire; uppercased here.
 * @param[in] flags SASL_MECH_* flags.
 * @param[in] step The exchange.
 * @return Non-zero on success; zero if the name is malformed or taken.
 */
extern int sasl_register(struct ModuleHandle* mod, const char* name,
                         unsigned int flags, SaslStepFn step);

/** Remove a mechanism \a mod registered.
 *
 * A module cannot remove one of the core's, nor one another module
 * registered.
 * @return Non-zero if it was found and removed.
 */
extern int sasl_unregister(struct ModuleHandle* mod, const char* name);

/** Remove every mechanism \a mod registered.  Called when it unloads. */
extern void sasl_drop_module(struct ModuleHandle* mod);

/** Number of mechanisms \a mod currently has registered. */
extern unsigned int sasl_module_count(const struct ModuleHandle* mod);

/** Return non-zero if \a name is usable as a mechanism name.
 *
 * RFC 4422 section 3.1: 1 to 20 characters of A-Z, 0-9, '-' and '_'.
 */
extern int sasl_name_valid(const char* name);

/** Write the mechanism list into \a buf, comma-separated.
 *
 * This is the value the "sasl" capability is advertised with.
 * @return Characters written, not counting the terminator.  A mechanism
 *   that would not fit is left out rather than truncated.
 */
extern size_t sasl_mechanisms_str(char* buf, size_t len);

/*
 * One exchange.
 */

/** Start a session with nothing in it. */
extern void sasl_session_init(struct SaslSession* ses);

/** End a session, wiping the secret it held.
 *
 * Idempotent, and called on every way out -- success, failure, the client
 * disconnecting mid-exchange -- because what it wipes is a password.
 */
extern void sasl_session_clear(struct SaslSession* ses);

/** Choose the mechanism for \a ses.
 *
 * @param[in,out] ses Session, with its connection details already set.
 * @param[in] name Mechanism the client asked for.
 * @return Zero on success, -1 if there is no such mechanism, -2 if it
 *   needs TLS and this connection has none.
 */
extern int sasl_session_begin(struct SaslSession* ses, const char* name);

/** Feed one AUTHENTICATE parameter to \a ses.
 *
 * Handles the chunking itself: "+" is an empty message or the terminator
 * of one that ended on a chunk boundary, "*" aborts, and anything else is
 * base64 that continues while it is exactly #SASL_CHUNKLEN long.
 *
 * @param[in,out] ses Session, already begun.
 * @param[in] line The parameter, as it arrived.
 * @return What to do next, from #SaslResult.
 */
extern enum SaslResult sasl_session_input(struct SaslSession* ses,
                                          const char* line);

#endif /* INCLUDED_sasl_h */
