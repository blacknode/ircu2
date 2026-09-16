/*
 * IRC - Internet Relay Chat, ircd/sasl.c
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
 * @brief The SASL mechanism register and the exchange with one client.
 */
#include "config.h"

#include "sasl.h"
#include "ircd_alloc.h"
#include "ircd_base64.h"
#include "ircd_chattr.h"
#include "ircd_log.h"
#include "ircd_sha256.h"
#include "ircd_string.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

/** The register, sorted by name.
 *
 * Sorted so that the "sasl" capability advertises the same list in the
 * same order on every server, whatever order the modules loaded in: a
 * value that depends on load order is one that differs between two
 * servers of the same network for no reason anybody can see.
 */
static struct SaslMechanism* sasl_list;

/** How many are on it. */
static unsigned int sasl_num;

/** Return non-zero if \a name is usable as a mechanism name.
 * @param[in] name Name to check.
 */
int sasl_name_valid(const char* name)
{
  size_t len;
  size_t i;

  if (!name)
    return 0;

  len = strlen(name);
  if (len < 1 || len > SASLMECHLEN)
    return 0;

  /* RFC 4422 section 3.1.  Deliberately not accepting lowercase: the name
   * is uppercased on the way in, so a module that registers "plain" and
   * one that registers "PLAIN" collide instead of both appearing.
   */
  for (i = 0; i < len; i++) {
    char c = name[i];

    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
          || c == '-' || c == '_'))
      return 0;
  }

  return 1;
}

/** First registered mechanism, for iteration. */
const struct SaslMechanism* sasl_first(void)
{
  return sasl_list;
}

/** Number of mechanisms currently registered. */
unsigned int sasl_count(void)
{
  return sasl_num;
}

/** Find a mechanism by name, case-insensitively.
 * @param[in] name Name to look for.
 * @return The mechanism, or NULL.
 */
const struct SaslMechanism* sasl_find(const char* name)
{
  struct SaslMechanism* m;

  if (!name || !*name)
    return NULL;

  for (m = sasl_list; m; m = m->sm_next)
    if (!ircd_strcmp(m->sm_name, name))
      return m;

  return NULL;
}

/** Register a mechanism.
 * @param[in] mod Module registering it, or NULL for the core.
 * @param[in] name Name as it goes on the wire.
 * @param[in] flags SASL_MECH_* flags.
 * @param[in] step The exchange.
 * @return Non-zero on success.
 */
int sasl_register(struct ModuleHandle* mod, const char* name,
                  unsigned int flags, SaslStepFn step)
{
  char upper[SASLMECHLEN + 1];
  struct SaslMechanism** m_p;
  struct SaslMechanism* m;
  size_t i;

  if (!name || !step)
    return 0;

  if (strlen(name) > SASLMECHLEN)
    return 0;

  for (i = 0; name[i]; i++)
    upper[i] = ToUpper(name[i]);
  upper[i] = '\0';

  if (!sasl_name_valid(upper)) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Refusing SASL mechanism \"%s\": not a valid mechanism name",
              name);
    return 0;
  }

  if (sasl_find(upper)) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Refusing SASL mechanism %s: already registered", upper);
    return 0;
  }

  m = (struct SaslMechanism*) MyCalloc(1, sizeof(struct SaslMechanism));
  strcpy(m->sm_name, upper);
  m->sm_flags = flags;
  m->sm_step = step;
  m->sm_owner = mod;

  for (m_p = &sasl_list; *m_p; m_p = &(*m_p)->sm_next)
    if (strcmp((*m_p)->sm_name, upper) > 0)
      break;

  m->sm_next = *m_p;
  *m_p = m;
  sasl_num++;

  sasl_advertise();

  return 1;
}

/** Remove a mechanism \a mod registered.
 * @param[in] mod Module that owns it; NULL is the core's.
 * @param[in] name Mechanism to remove.
 * @return Non-zero if it was found and removed.
 */
int sasl_unregister(struct ModuleHandle* mod, const char* name)
{
  struct SaslMechanism** m_p;
  struct SaslMechanism* m;

  if (!name)
    return 0;

  for (m_p = &sasl_list; (m = *m_p); m_p = &m->sm_next) {
    if (ircd_strcmp(m->sm_name, name))
      continue;

    /* Whoever registered it removes it.  A module that could withdraw
     * PLAIN could lock the network out of its own accounts.
     */
    if (m->sm_owner != mod)
      return 0;

    *m_p = m->sm_next;
    sasl_num--;
    MyFree(m);

    sasl_advertise();

    return 1;
  }

  return 0;
}

/** Remove every mechanism \a mod registered.
 * @param[in] mod Module being torn down.
 */
void sasl_drop_module(struct ModuleHandle* mod)
{
  struct SaslMechanism** m_p;
  struct SaslMechanism* m;

  if (!mod)
    return;

  for (m_p = &sasl_list; (m = *m_p); ) {
    if (m->sm_owner != mod) {
      m_p = &m->sm_next;
      continue;
    }

    *m_p = m->sm_next;
    sasl_num--;
    MyFree(m);
  }

  sasl_advertise();
}

/** Number of mechanisms \a mod currently has registered.
 * @param[in] mod Module to count for.
 */
unsigned int sasl_module_count(const struct ModuleHandle* mod)
{
  struct SaslMechanism* m;
  unsigned int n = 0;

  for (m = sasl_list; m; m = m->sm_next)
    if (m->sm_owner == mod)
      n++;

  return n;
}

/** Write the mechanism list into \a buf, comma-separated.
 * @param[out] buf Buffer to write into.
 * @param[in] len Its size, terminator included.
 * @return Characters written.
 */
size_t sasl_mechanisms_str(char* buf, size_t len)
{
  struct SaslMechanism* m;
  size_t o = 0;

  if (!buf || !len)
    return 0;

  buf[0] = '\0';

  for (m = sasl_list; m; m = m->sm_next) {
    size_t need = strlen(m->sm_name) + (o ? 1 : 0);

    /* A name that does not fit is left out, not cut in half: half a
     * mechanism name in a CAP LS is a mechanism a client will ask for and
     * nobody has.
     */
    if (o + need + 1 > len)
      continue;

    if (o)
      buf[o++] = ',';

    memcpy(buf + o, m->sm_name, strlen(m->sm_name));
    o += strlen(m->sm_name);
    buf[o] = '\0';
  }

  return o;
}

/* ------------------------------------------------------------------- *
 * The core's mechanisms.                                              *
 * ------------------------------------------------------------------- */

/** Copy a NUL-terminated field out of a SASL message.
 *
 * The message is a blob with NULs inside it, so strlen() is the wrong
 * tool twice over: it would stop at the separator the caller is trying to
 * find, and it would run off the end of a message that has none.
 *
 * @param[in] in The message.
 * @param[in] inlen Its length.
 * @param[in,out] pos Offset to read from; advanced past the separator.
 * @param[out] out Buffer for the field.
 * @param[in] outlen Its size, terminator included.
 * @return Non-zero on success; zero if there is no separator or the field
 *   does not fit.
 */
static int sasl_field(const char* in, size_t inlen, size_t* pos,
                      char* out, size_t outlen)
{
  const char* start = in + *pos;
  const char* nul;
  size_t len;

  if (*pos > inlen)
    return 0;

  nul = (const char*) memchr(start, '\0', inlen - *pos);
  if (!nul)
    return 0;

  len = (size_t) (nul - start);
  if (len + 1 > outlen)
    return 0;

  memcpy(out, start, len);
  out[len] = '\0';
  *pos += len + 1;

  return 1;
}

/** PLAIN, RFC 4616: authzid NUL authcid NUL password.
 *
 * Here the authcid is the email and the authzid names which of that
 * email's accounts to use -- empty for the default one.  That is what the
 * authorization identity is for, so no syntax of our own is invented for
 * it (proposal 007 section 4.1).
 *
 * @param[in,out] ses Session to fill in.
 * @param[in] in Decoded message.
 * @param[in] inlen Its length.
 * @return What it decided.
 */
static enum SaslStep sasl_step_plain(struct SaslSession* ses, const char* in,
                                     size_t inlen)
{
  size_t pos = 0;

  if (!sasl_field(in, inlen, &pos, ses->ss_authzid, sizeof(ses->ss_authzid)))
    return SASL_STEP_FAIL;

  if (!sasl_field(in, inlen, &pos, ses->ss_authcid, sizeof(ses->ss_authcid)))
    return SASL_STEP_FAIL;

  /* The rest is the password, and it is the rest: a password may contain
   * anything, NUL included, so it is taken by length and not by scanning.
   */
  ses->ss_secretlen = inlen - pos;
  if (ses->ss_secretlen + 1 > sizeof(ses->ss_secret))
    return SASL_STEP_FAIL;

  memcpy(ses->ss_secret, in + pos, ses->ss_secretlen);
  ses->ss_secret[ses->ss_secretlen] = '\0';

  if (!ses->ss_authcid[0] || !ses->ss_secretlen)
    return SASL_STEP_FAIL;

  return SASL_STEP_CREDENTIAL;
}

/** EXTERNAL, RFC 4422 appendix A: the message is the authzid, or empty.
 *
 * The credential is the client certificate, which the server has already
 * seen; what crosses here is only which account to use.  The fingerprint
 * is the proof and it is in the session already, so a message with a
 * password in it would be a message this mechanism has no use for.
 *
 * @param[in,out] ses Session to fill in.
 * @param[in] in Decoded message.
 * @param[in] inlen Its length.
 * @return What it decided.
 */
static enum SaslStep sasl_step_external(struct SaslSession* ses,
                                        const char* in, size_t inlen)
{
  if (inlen + 1 > sizeof(ses->ss_authzid))
    return SASL_STEP_FAIL;

  if (memchr(in, '\0', inlen))
    return SASL_STEP_FAIL;

  memcpy(ses->ss_authzid, in, inlen);
  ses->ss_authzid[inlen] = '\0';

  /* No certificate, no credential.  Failing here rather than letting the
   * provider decide keeps "authenticated as nobody" from being a state
   * that exists even for an instant.
   */
  if (!ses->ss_fingerprint[0])
    return SASL_STEP_FAIL;

  ses->ss_authcid[0] = '\0';
  ses->ss_secretlen = 0;

  return SASL_STEP_CREDENTIAL;
}

/** Populate the register with the core's mechanisms. */
void sasl_init(void)
{
  sasl_close();

  sasl_register(NULL, "PLAIN", SASL_MECH_NEEDS_TLS, sasl_step_plain);
  sasl_register(NULL, "EXTERNAL", 0, sasl_step_external);
}

/** Release the register. */
void sasl_close(void)
{
  while (sasl_list) {
    struct SaslMechanism* m = sasl_list;

    sasl_list = m->sm_next;
    MyFree(m);
  }

  sasl_num = 0;
}

/* ------------------------------------------------------------------- *
 * One exchange.                                                       *
 * ------------------------------------------------------------------- */

/** Start a session with nothing in it.
 * @param[out] ses Session to initialise.
 */
void sasl_session_init(struct SaslSession* ses)
{
  assert(0 != ses);
  memset(ses, 0, sizeof(*ses));
}

/** End a session, wiping the secret it held.
 * @param[in,out] ses Session to clear.
 */
void sasl_session_clear(struct SaslSession* ses)
{
  if (!ses)
    return;

  /* Not memset(): a compiler is entitled to drop a write to storage
   * nothing reads again, and what is being dropped here is the erasure of
   * a password.  See ircd_sha256.h.
   */
  ircd_crypto_wipe(ses, sizeof(*ses));
}

/** Choose the mechanism for \a ses.
 * @param[in,out] ses Session, with its connection details already set.
 * @param[in] name Mechanism the client asked for.
 * @return Zero on success, -1 if unknown, -2 if it needs TLS.
 */
int sasl_session_begin(struct SaslSession* ses, const char* name)
{
  const struct SaslMechanism* m;

  assert(0 != ses);

  m = sasl_find(name);
  if (!m)
    return -1;

  if ((m->sm_flags & SASL_MECH_NEEDS_TLS) && !ses->ss_tls)
    return -2;

  /* Starting again is allowed -- a client may change its mind after a
   * failure -- and it must not leave the previous attempt's secret, or
   * half of its message, lying in the session.
   */
  ses->ss_wirelen = 0;
  ses->ss_wire[0] = '\0';
  ses->ss_steps = 0;
  ses->ss_outlen = 0;
  ircd_crypto_wipe(ses->ss_secret, sizeof(ses->ss_secret));
  ses->ss_secretlen = 0;
  ses->ss_authcid[0] = '\0';
  ses->ss_authzid[0] = '\0';

  ses->ss_mech = m;

  return 0;
}

/** Feed one AUTHENTICATE parameter to \a ses.
 * @param[in,out] ses Session, already begun.
 * @param[in] line The parameter, as it arrived.
 * @return What to do next.
 */
enum SaslResult sasl_session_input(struct SaslSession* ses, const char* line)
{
  /* Sized from the wire cap, not from SASL_MESSAGE_MAX: the largest
   * message SASL_WIRE_MAX characters can carry is a little longer than
   * SASL_MESSAGE_MAX, and decoding into a buffer that cannot hold it
   * would report "malformed" for something that is merely too long. */
  char decoded[IRCD_BASE64_DECLEN(SASL_WIRE_MAX) + 1];
  enum SaslStep step;
  size_t linelen;
  int declen;

  assert(0 != ses);

  if (!line)
    return SASL_BAD_INPUT;

  if (!strcmp(line, "*"))
    return SASL_ABORTED;

  if (!ses->ss_mech)
    return SASL_BAD_INPUT;

  if (!strcmp(line, "+")) {
    /* Either an empty message, or the terminator of one whose length was
     * an exact multiple of the chunk size.  Both end the message, so both
     * fall through to the decode with whatever has been gathered.
     */
    linelen = 0;
  } else {
    linelen = strlen(line);

    if (linelen > SASL_CHUNKLEN)
      return SASL_BAD_INPUT;

    if (ses->ss_wirelen + linelen > SASL_WIRE_MAX)
      return SASL_TOO_LONG;

    memcpy(ses->ss_wire + ses->ss_wirelen, line, linelen);
    ses->ss_wirelen += linelen;
    ses->ss_wire[ses->ss_wirelen] = '\0';

    if (linelen == SASL_CHUNKLEN)
      return SASL_NEED_MORE;
  }

  declen = ircd_base64_decode(ses->ss_wire, ses->ss_wirelen, decoded,
                              sizeof(decoded) - 1);

  ses->ss_wirelen = 0;
  ses->ss_wire[0] = '\0';

  if (declen < 0)
    return SASL_BAD_INPUT;

  if (declen > SASL_MESSAGE_MAX) {
    ircd_crypto_wipe(decoded, sizeof(decoded));
    return SASL_TOO_LONG;
  }

  decoded[declen] = '\0';

  /* The message is consumed whatever the mechanism makes of it -- it was
   * cleared above, before any of the ways out -- because leaving it would
   * prepend it to the next one. */
  ses->ss_steps++;

  step = (*ses->ss_mech->sm_step)(ses, decoded, (size_t) declen);

  ircd_crypto_wipe(decoded, sizeof(decoded));

  switch (step) {
  case SASL_STEP_CHALLENGE:
    return SASL_CHALLENGE;
  case SASL_STEP_CREDENTIAL:
    return SASL_CREDENTIAL;
  default:
    return SASL_BAD_INPUT;
  }
}
