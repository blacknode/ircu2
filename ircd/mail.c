/*
 * IRC - Internet Relay Chat, ircd/mail.c
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
 * @brief The mail provider register, the messages in flight, and the token.
 *
 * See include/mail.h for why the proof is the core's and the delivery is
 * a module's.  Nothing here touches a @c struct @c Client.
 */
#include "config.h"

#include "mail.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_base64.h"
#include "ircd_events.h"
#include "ircd_log.h"
#include "ircd_sha256.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "ircd_vhost.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <stdlib.h>
#include <string.h>

/** Default per-message deadline, in seconds. */
#define MAIL_TIMEOUT_DEFAULT 30
/** Default validity of a verification token, in seconds. */
#define MAIL_WINDOW_DEFAULT (24 * 60 * 60)
/** Default gap between two messages to one client, in seconds. */
#define MAIL_RESEND_DEFAULT 300

/** Ceiling on messages in flight, so a provider that accepts and never
 * answers cannot grow the list without bound. */
#define MAIL_PENDING_MAX 1024

/** What the token's key is derived for.
 *
 * Domain separation: the Security{} key already ciphers hostnames, and a
 * value that can be used as two things is a value one of whose uses will
 * be surprised by the other.
 */
#define MAIL_KEY_LABEL "ircu-mail-verify-v1"

/** Bytes of the HMAC a token carries.
 *
 * Sixteen, not thirty-two: a token is pasted back by a person, and 128
 * bits is far past what forging one is worth -- the whole grant is "this
 * address was reachable".
 */
#define MAIL_TAG_LEN 16

/** One message the provider has not answered yet. */
struct MailCall {
  struct MailCall*     mc_next;     /**< Next message, in no order. */
  mail_id_t            mc_id;       /**< What the provider holds. */
  MailDoneFn           mc_fn;       /**< Callback, or NULL. */
  void*                mc_user;     /**< The caller's opaque pointer. */
  struct ModuleHandle* mc_owner;    /**< Module that asked, or NULL. */
  time_t               mc_deadline; /**< When it is given up on. */
};

/** The registered provider, or NULL. */
static const struct MailProvider* mail_provider;
/** Module that registered it. */
static struct ModuleHandle* mail_provider_owner;

/** Messages accepted and not yet answered. */
static struct MailCall* mail_calls;
/** Handle for the next message; never zero, never reused while in flight. */
static mail_id_t mail_last_id;

/** The published @c Mail{} block, or NULL. */
static struct MailConf* mail_config;
/** The block being read, between mail_conf_clear() and commit. */
static struct MailConf mail_pending;
/** Non-zero once mail_conf_clear() has run in this configuration pass. */
static int mail_pending_seen;

/** Why the last mail_send() refused. */
static enum MailError mail_error;

/** Statistics. */
static unsigned int mail_stat_pending;
static unsigned int mail_stat_total;   /**< @copydoc mail_stat_pending */
static unsigned int mail_stat_failed;  /**< @copydoc mail_stat_pending */

/** Text for each #MailError, indexed by the enum. */
static const char* mail_error_text[MAIL_ERR_LAST] = {
  "no error",
  "no mail provider is loaded",
  "the Mail{} block is missing or unusable",
  "that is not an address this server will send to",
  "the message is too large",
  "the mail provider did not answer in time",
  "the mail provider could not send it"
};

const char* mail_strerror(enum MailError err)
{
  if (err < 0 || err >= MAIL_ERR_LAST)
    return "unknown mail error";

  return mail_error_text[err];
}

/* ------------------------------------------------------------------- *
 * Messages in flight.                                                 *
 * ------------------------------------------------------------------- */

/** Timer over the earliest deadline, armed only while something is out.
 *
 * Its own timer, for the reason cache.c and account.c have one: a deadline
 * enforced by a pass that is scheduled minutes ahead is not a deadline.
 */
static struct Timer mail_timer;

/** Whether #mail_timer is on the queue. */
static int mail_timer_armed;

/** Whether the timer struct has been through timer_init().
 *
 * Once, and never again: timer_init() zeroes GEN_MARKED, which is the one
 * thing telling timer_add() it is re-arming a timer timer_run() still
 * holds.  See the same comment in cache.c.
 */
static int mail_timer_ready;

static void mail_timeout(struct Event* ev);

/** Find a message by handle. */
static struct MailCall* mail_find(mail_id_t id)
{
  struct MailCall* call;

  for (call = mail_calls; call; call = call->mc_next)
    if (call->mc_id == id)
      return call;

  return NULL;
}

/** Earliest deadline of anything in flight, or 0. */
static time_t mail_deadline(void)
{
  struct MailCall* call;
  time_t earliest = 0;

  for (call = mail_calls; call; call = call->mc_next)
    if (!earliest || call->mc_deadline < earliest)
      earliest = call->mc_deadline;

  return earliest;
}

/** Make sure the timer will fire by the earliest deadline. */
static void mail_arm(void)
{
  time_t deadline;

  if (mail_timer_armed)
    return;

  if (!(deadline = mail_deadline()))
    return;

  if (!mail_timer_ready) {
    timer_init(&mail_timer);
    mail_timer_ready = 1;
  }

  timer_add(&mail_timer, mail_timeout, 0, TT_ABSOLUTE, deadline);
  mail_timer_armed = 1;
}

/** Fail whatever has waited too long, and set the timer for the rest. */
static void mail_timeout(struct Event* ev)
{
  if (ev_type(ev) != ET_EXPIRE) {
    if (ev_type(ev) == ET_DESTROY)
      mail_timer_armed = 0;
    return;
  }

  mail_timer_armed = 0;

  mail_expire(CurrentTime);
  mail_arm();
}

/** Hand out a handle that is neither zero nor already in flight. */
static mail_id_t mail_new_id(void)
{
  do {
    ++mail_last_id;
  } while (mail_last_id == 0 || mail_find(mail_last_id));

  return mail_last_id;
}

/** Unlink and free one message.  Does not call anything back. */
static void mail_free(struct MailCall* call)
{
  struct MailCall** call_p;

  for (call_p = &mail_calls; *call_p; call_p = &(*call_p)->mc_next) {
    if (*call_p == call) {
      *call_p = call->mc_next;
      if (mail_stat_pending)
        mail_stat_pending--;
      break;
    }
  }

  MyFree(call);
}

/** Take a message off the list and answer it.
 *
 * Off the list before the callback runs: a callback that sends another
 * message must not find this entry still linked.
 */
static void mail_answer(struct MailCall* call, enum MailError err,
                        const char* detail)
{
  MailDoneFn fn = call->mc_fn;
  void* user = call->mc_user;

  mail_free(call);

  if (err != MAIL_OK)
    mail_stat_failed++;

  if (fn)
    (*fn)(err, detail, user);
}

/* ------------------------------------------------------------------- *
 * The register.                                                       *
 * ------------------------------------------------------------------- */

int mail_register_provider(struct ModuleHandle* mod,
                           const struct MailProvider* provider)
{
  if (!provider || !provider->mp_name || !provider->mp_send) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Refusing a mail provider that is missing a name or a send");
    return 0;
  }

  if (mail_provider) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "Refusing mail provider %s: %s is already registered",
              provider->mp_name, mail_provider->mp_name);
    return 0;
  }

  mail_provider = provider;
  mail_provider_owner = mod;

  log_write(LS_SYSTEM, L_INFO, 0, "Mail provider %s registered",
            provider->mp_name);

  return 1;
}

void mail_unregister_provider(struct ModuleHandle* mod)
{
  const char* name;

  if (!mail_provider || mail_provider_owner != mod)
    return;

  name = mail_provider->mp_name;

  /* The register goes first, so a callback that tries to send something
   * on its way out is refused rather than reaching code being unmapped.
   */
  mail_provider = NULL;
  mail_provider_owner = NULL;

  while (mail_calls)
    mail_answer(mail_calls, MAIL_ERR_NOPROVIDER, NULL);

  log_write(LS_SYSTEM, L_INFO, 0, "Mail provider %s withdrawn", name);
}

int mail_available(void)
{
  return mail_provider != NULL && mail_config != NULL
         && !EmptyString(mail_config->mconf_from);
}

const char* mail_provider_name(void)
{
  return mail_provider ? mail_provider->mp_name : "none";
}

void mail_cancel_module(struct ModuleHandle* mod)
{
  struct MailCall* call;
  struct MailCall* next;

  if (!mod)
    return;

  for (call = mail_calls; call; call = next) {
    next = call->mc_next;

    if (call->mc_owner != mod)
      continue;

    if (mail_provider && mail_provider->mp_cancel)
      (*mail_provider->mp_cancel)(call->mc_id);

    mail_free(call);
  }

  if (mail_provider_owner == mod)
    mail_unregister_provider(mod);
}

/* ------------------------------------------------------------------- *
 * Sending.                                                            *
 * ------------------------------------------------------------------- */

/** Non-zero if \a text has no control character and no space.
 *
 * What makes an address safe to put in a header: a CR or an LF in one
 * would let whoever typed it write headers of their own, and the address
 * comes from a client.
 */
static int mail_header_safe(const char* text)
{
  const unsigned char* p;

  for (p = (const unsigned char*) text; *p; p++)
    if (*p < 0x21 || *p > 0x7e)
      return 0;

  return 1;
}

/** Non-zero if \a email is an address this will send to.
 *
 * Deliberately not an attempt at RFC 5322: what matters here is that it
 * is one address, that it cannot smuggle a header, and that it is short
 * enough to store.  Whether it exists is what the message is for.
 */
int mail_address_valid(const char* email)
{
  const char* at;

  if (EmptyString(email) || strlen(email) > MAIL_ADDRESS_MAX)
    return 0;

  if (!mail_header_safe(email))
    return 0;

  if (!(at = strchr(email, '@')) || at == email || !at[1])
    return 0;

  /* One address, not a list, and not a route. */
  if (strchr(at + 1, '@') || strchr(email, ',') || strchr(email, ';')
      || strchr(email, ':') || strchr(email, '<') || strchr(email, '>'))
    return 0;

  /* Something that looks like a domain: a dot with text on both sides. */
  return strchr(at + 1, '.') != NULL && at[1] != '.'
         && email[strlen(email) - 1] != '.';
}

enum MailError mail_last_error(void)
{
  return mail_error;
}

mail_id_t mail_send(struct ModuleHandle* mod, const char* to,
                    const char* subject, const char* body,
                    MailDoneFn fn, void* user)
{
  struct MailCall* call;
  struct MailMessage msg;
  enum MailError err;

  mail_error = MAIL_OK;

  if (!mail_provider) {
    mail_error = MAIL_ERR_NOPROVIDER;
    return 0;
  }

  if (!mail_config || EmptyString(mail_config->mconf_from)) {
    mail_error = MAIL_ERR_NOCONFIG;
    return 0;
  }

  if (!mail_address_valid(to)) {
    mail_error = MAIL_ERR_ADDRESS;
    return 0;
  }

  if (EmptyString(subject) || strlen(subject) > MAIL_SUBJECT_MAX
      || EmptyString(body) || strlen(body) > MAIL_BODY_MAX) {
    mail_error = MAIL_ERR_TOOLONG;
    return 0;
  }

  /* A subject is one header line.  The body may hold newlines -- it is
   * the message -- but the subject may not, for the reason the address
   * may not.
   */
  if (strchr(subject, '\r') || strchr(subject, '\n')) {
    mail_error = MAIL_ERR_TOOLONG;
    return 0;
  }

  if (mail_stat_pending >= MAIL_PENDING_MAX) {
    mail_error = MAIL_ERR_FAILED;
    return 0;
  }

  call = (struct MailCall*) MyCalloc(1, sizeof(struct MailCall));
  call->mc_id = mail_new_id();
  call->mc_fn = fn;
  call->mc_user = user;
  call->mc_owner = mod;
  call->mc_deadline = CurrentTime + mail_config->mconf_timeout;

  call->mc_next = mail_calls;
  mail_calls = call;
  mail_stat_pending++;
  mail_stat_total++;

  memset(&msg, 0, sizeof(msg));
  msg.mm_to = to;
  msg.mm_from = mail_config->mconf_from;
  msg.mm_subject = subject;
  msg.mm_body = body;

  err = (*mail_provider->mp_send)(call->mc_id, &msg);

  if (err != MAIL_OK) {
    /* Refused on the spot.  The caller is told through its callback, the
     * way it would be for any other failure, so that there is one path
     * out of a send and not two.
     */
    mail_error = err;

    if (mail_find(call->mc_id))
      mail_answer(call, err, NULL);

    return 0;
  }

  mail_arm();

  return call->mc_id;
}

int mail_complete(mail_id_t id, enum MailError err, const char* detail)
{
  struct MailCall* call = mail_find(id);

  if (!call)
    return 0;

  if (err != MAIL_OK)
    log_write(LS_SYSTEM, L_WARNING, 0, "Mail to a user was not sent: %s%s%s",
              mail_strerror(err), detail ? ": " : "", detail ? detail : "");

  mail_answer(call, err, detail);

  return 1;
}

int mail_expire(time_t now)
{
  struct MailCall* call;
  struct MailCall* next;
  int expired = 0;

  for (call = mail_calls; call; call = next) {
    next = call->mc_next;

    if (call->mc_deadline > now)
      continue;

    if (mail_provider && mail_provider->mp_cancel)
      (*mail_provider->mp_cancel)(call->mc_id);

    mail_answer(call, MAIL_ERR_TIMEOUT, NULL);
    expired++;
  }

  return expired;
}

unsigned int mail_pending_count(void)
{
  return mail_stat_pending;
}

/* ------------------------------------------------------------------- *
 * The token.                                                          *
 * ------------------------------------------------------------------- */

/** Derive the signing key from the network's Security{} key.
 *
 * @param[out] out #SHA256_DIGEST_LEN bytes.
 * @return Non-zero when there is a key to derive from.
 */
static int mail_token_key(unsigned char* out)
{
  const char* key = vhost_key();

  if (EmptyString(key))
    return 0;

  ircd_hmac_sha256(key, strlen(key), MAIL_KEY_LABEL,
                   strlen(MAIL_KEY_LABEL), out);

  return 1;
}

/** Sign \a payload.
 * @param[in] payload Text being signed.
 * @param[out] tag #MAIL_TAG_LEN bytes.
 * @return Non-zero when there is a key.
 */
static int mail_token_sign(const char* payload, unsigned char* tag)
{
  unsigned char key[SHA256_DIGEST_LEN];
  unsigned char full[SHA256_DIGEST_LEN];

  if (!mail_token_key(key))
    return 0;

  ircd_hmac_sha256(key, sizeof(key), payload, strlen(payload), full);
  memcpy(tag, full, MAIL_TAG_LEN);

  memset(key, 0, sizeof(key));
  memset(full, 0, sizeof(full));

  return 1;
}

int mail_token_make(char* buf, size_t len, const char* email, time_t now,
                    int window)
{
  char payload[MAIL_ADDRESS_MAX + 32];
  char payload64[IRCD_BASE64_ENCLEN(sizeof(payload)) + 1];
  char tag64[IRCD_BASE64_ENCLEN(MAIL_TAG_LEN) + 1];
  unsigned char tag[MAIL_TAG_LEN];
  unsigned int wrote;

  assert(0 != buf);

  if (EmptyString(email) || strlen(email) > MAIL_ADDRESS_MAX)
    return 0;

  if (window <= 0)
    window = MAIL_WINDOW_DEFAULT;

  /* What the token asserts, in the clear: it is not a secret, it is a
   * claim with a signature on it.  Whoever reads the mail already knows
   * their own address.
   */
  ircd_snprintf(0, payload, sizeof(payload), "%lu:%s",
                (unsigned long) (now + window), email);

  if (!mail_token_sign(payload, tag))
    return 0;

  if (ircd_base64_encode(payload, strlen(payload), payload64,
                         sizeof(payload64)) < 0)
    return 0;

  if (ircd_base64_encode(tag, sizeof(tag), tag64, sizeof(tag64)) < 0)
    return 0;

  wrote = ircd_snprintf(0, buf, len, "1.%s.%s", payload64, tag64);

  return wrote > 0 && wrote < len;
}

enum MailToken mail_token_check(const char* token, char* email, size_t len,
                                time_t now)
{
  char payload[MAIL_ADDRESS_MAX + 32];
  unsigned char tag[MAIL_TAG_LEN];
  unsigned char want[MAIL_TAG_LEN];
  const char* dot;
  const char* sig;
  char* colon;
  int decoded;
  unsigned long expiry;

  assert(0 != email);

  if (EmptyString(token) || token[0] != '1' || token[1] != '.')
    return MAIL_TOKEN_MALFORMED;

  if (!(dot = strchr(token + 2, '.')))
    return MAIL_TOKEN_MALFORMED;

  sig = dot + 1;

  decoded = ircd_base64_decode(token + 2, (size_t) (dot - (token + 2)),
                               payload, sizeof(payload) - 1);
  if (decoded <= 0)
    return MAIL_TOKEN_MALFORMED;

  payload[decoded] = '\0';

  /* A payload with a NUL inside it would be two different strings to two
   * different readers, which is how a signature ends up covering less
   * than what is used.
   */
  if ((size_t) decoded != strlen(payload))
    return MAIL_TOKEN_MALFORMED;

  if (ircd_base64_decode(sig, 0, tag, sizeof(tag)) != (int) sizeof(tag))
    return MAIL_TOKEN_MALFORMED;

  if (!mail_token_sign(payload, want))
    return MAIL_TOKEN_NOKEY;

  /* The signature before the contents: what an unsigned token says is not
   * worth parsing, and comparing in constant time is what keeps a guess
   * from being told how close it was.
   */
  if (!ircd_crypto_equal(tag, want, sizeof(tag)))
    return MAIL_TOKEN_BAD;

  if (!(colon = strchr(payload, ':')) || colon == payload || !colon[1])
    return MAIL_TOKEN_MALFORMED;

  *colon = '\0';
  expiry = strtoul(payload, NULL, 10);

  if (!expiry || (time_t) expiry <= now)
    return MAIL_TOKEN_EXPIRED;

  if (strlen(colon + 1) >= len)
    return MAIL_TOKEN_MALFORMED;

  ircd_strncpy(email, colon + 1, len - 1);

  return MAIL_TOKEN_OK;
}

/* ------------------------------------------------------------------- *
 * The Mail{} block.                                                   *
 * ------------------------------------------------------------------- */

/** Free what a configuration holds. */
static void mail_conf_release(struct MailConf* conf)
{
  MyFree(conf->mconf_from);
  MyFree(conf->mconf_program);
  MyFree(conf->mconf_verify_url);
  memset(conf, 0, sizeof(*conf));
}

const struct MailConf* mail_conf(void)
{
  return mail_config;
}

void mail_conf_clear(void)
{
  mail_conf_release(&mail_pending);
  mail_pending.mconf_timeout = MAIL_TIMEOUT_DEFAULT;
  mail_pending.mconf_window = MAIL_WINDOW_DEFAULT;
  mail_pending.mconf_resend = MAIL_RESEND_DEFAULT;
  mail_pending_seen = 1;
}

void mail_conf_set_from(char* from)
{
  MyFree(mail_pending.mconf_from);
  mail_pending.mconf_from = from;
}

void mail_conf_set_program(char* program)
{
  MyFree(mail_pending.mconf_program);
  mail_pending.mconf_program = program;
}

void mail_conf_set_verify_url(char* url)
{
  MyFree(mail_pending.mconf_verify_url);
  mail_pending.mconf_verify_url = url;
}

void mail_conf_set_timeout(int seconds)
{
  mail_pending.mconf_timeout = seconds;
}

void mail_conf_set_window(int seconds)
{
  mail_pending.mconf_window = seconds;
}

void mail_conf_set_resend(int seconds)
{
  mail_pending.mconf_resend = seconds;
}

int mail_conf_commit(const char** errstr)
{
  static unsigned int generation;

  assert(0 != errstr);

  /* Without a sender there is nothing to put in the From: header, and a
   * message with no sender is one most mail servers will not take.
   */
  if (!mail_address_valid(mail_pending.mconf_from)) {
    *errstr = "Mail: from must be one ordinary address";
    mail_conf_release(&mail_pending);
    return 0;
  }

  /* A link template that does not say where the token goes would send
   * everybody the same link.
   */
  if (!EmptyString(mail_pending.mconf_verify_url)
      && !strstr(mail_pending.mconf_verify_url, "%s")) {
    *errstr = "Mail: verify_url must contain %s, where the token goes";
    mail_conf_release(&mail_pending);
    return 0;
  }

  if (mail_pending.mconf_timeout <= 0)
    mail_pending.mconf_timeout = MAIL_TIMEOUT_DEFAULT;
  if (mail_pending.mconf_timeout > MAIL_TIMEOUT_MAX) {
    log_write(LS_CONFIG, L_WARNING, 0,
              "Mail: timeout of %ds exceeds the %ds maximum; using %ds",
              mail_pending.mconf_timeout, MAIL_TIMEOUT_MAX,
              MAIL_TIMEOUT_MAX);
    mail_pending.mconf_timeout = MAIL_TIMEOUT_MAX;
  }

  if (mail_pending.mconf_window <= 0)
    mail_pending.mconf_window = MAIL_WINDOW_DEFAULT;
  if (mail_pending.mconf_resend < 0)
    mail_pending.mconf_resend = MAIL_RESEND_DEFAULT;

  if (!mail_config)
    mail_config = (struct MailConf*) MyCalloc(1, sizeof(struct MailConf));
  else
    mail_conf_release(mail_config);

  *mail_config = mail_pending;
  memset(&mail_pending, 0, sizeof(mail_pending));

  mail_config->mconf_generation = ++generation;

  return 1;
}

void mail_conf_unmark(void)
{
  mail_pending_seen = 0;
}

void mail_conf_sweep(void)
{
  mail_conf_release(&mail_pending);

  if (mail_pending_seen || !mail_config)
    return;

  log_write(LS_CONFIG, L_INFO, 0,
            "The Mail{} block is gone; no mail will be sent");

  mail_conf_release(mail_config);
  MyFree(mail_config);
  mail_config = NULL;
}

void mail_close(void)
{
  while (mail_calls)
    mail_answer(mail_calls, MAIL_ERR_NOPROVIDER, NULL);

  if (mail_timer_armed) {
    timer_del(&mail_timer);
    mail_timer_armed = 0;
  }

  mail_conf_release(&mail_pending);

  if (mail_config) {
    mail_conf_release(mail_config);
    MyFree(mail_config);
    mail_config = NULL;
  }

  mail_provider = NULL;
  mail_provider_owner = NULL;

  mail_stat_pending = 0;
  mail_stat_total = 0;
  mail_stat_failed = 0;
}
