/*
 * IRC - Internet Relay Chat, ircd/ircd_token.c
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
 * @brief Signing something the server hands out.  See include/ircd_token.h.
 */
#include "config.h"

#include "ircd_token.h"
#include "ircd_base64.h"
#include "ircd_log.h"
#include "ircd_sha256.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "ircd_vhost.h"

#include <stdlib.h>
#include <string.h>

/** Bytes of the HMAC a token carries. */
#define TOKEN_TAG_LEN 16

/** Derive the signing key for \a label from the network's key.
 *
 * @param[in] label What the token is for.
 * @param[out] out #SHA256_DIGEST_LEN bytes.
 * @return Non-zero when there is a key to derive from.
 */
static int token_key(const char* label, unsigned char* out)
{
  const char* key = vhost_key();

  if (EmptyString(key) || EmptyString(label))
    return 0;

  /* The label is the whole of the separation between one use and
   * another: same key material, different keys, and neither of them is
   * the key itself. */
  ircd_hmac_sha256(key, strlen(key), label, strlen(label), out);

  return 1;
}

/** Sign \a payload under \a label.
 * @param[out] tag #TOKEN_TAG_LEN bytes.
 * @return Non-zero when there is a key.
 */
static int token_sign(const char* label, const char* payload,
                      unsigned char* tag)
{
  unsigned char key[SHA256_DIGEST_LEN];
  unsigned char full[SHA256_DIGEST_LEN];

  if (!token_key(label, key))
    return 0;

  ircd_hmac_sha256(key, sizeof(key), payload, strlen(payload), full);
  memcpy(tag, full, TOKEN_TAG_LEN);

  memset(key, 0, sizeof(key));
  memset(full, 0, sizeof(full));

  return 1;
}

int ircd_token_make(char* buf, size_t len, const char* label,
                    const char* payload, time_t now, int window)
{
  char signed_payload[TOKEN_PAYLOAD_MAX + 32];
  char payload64[IRCD_BASE64_ENCLEN(sizeof(signed_payload)) + 1];
  char tag64[IRCD_BASE64_ENCLEN(TOKEN_TAG_LEN) + 1];
  unsigned char tag[TOKEN_TAG_LEN];
  unsigned int wrote;

  assert(0 != buf);

  if (EmptyString(payload) || strlen(payload) > TOKEN_PAYLOAD_MAX)
    return 0;

  /* A newline would be a second line in whatever this is pasted into,
   * and a NUL would make the signature cover less than what is read. */
  if (strchr(payload, '\n') || strchr(payload, '\r'))
    return 0;

  if (window <= 0)
    return 0;

  /* The expiry first, and in the signed part: a token whose lifetime the
   * holder could edit would have no lifetime. */
  ircd_snprintf(0, signed_payload, sizeof(signed_payload), "%lu:%s",
                (unsigned long) (now + window), payload);

  if (!token_sign(label, signed_payload, tag))
    return 0;

  if (ircd_base64_encode(signed_payload, strlen(signed_payload), payload64,
                         sizeof(payload64)) < 0)
    return 0;

  if (ircd_base64_encode(tag, sizeof(tag), tag64, sizeof(tag64)) < 0)
    return 0;

  wrote = ircd_snprintf(0, buf, len, "1.%s.%s", payload64, tag64);

  return wrote > 0 && wrote < len;
}

enum TokenResult ircd_token_check(const char* token, const char* label,
                                  char* payload, size_t len, time_t now)
{
  char signed_payload[TOKEN_PAYLOAD_MAX + 32];
  unsigned char tag[TOKEN_TAG_LEN];
  unsigned char want[TOKEN_TAG_LEN];
  const char* dot;
  char* colon;
  int decoded;
  unsigned long expiry;

  assert(0 != payload);

  if (EmptyString(token) || token[0] != '1' || token[1] != '.')
    return TOKEN_MALFORMED;

  if (!(dot = strchr(token + 2, '.')))
    return TOKEN_MALFORMED;

  decoded = ircd_base64_decode(token + 2, (size_t) (dot - (token + 2)),
                               signed_payload, sizeof(signed_payload) - 1);
  if (decoded <= 0)
    return TOKEN_MALFORMED;

  signed_payload[decoded] = '\0';

  /* A payload with a NUL inside it would be two different strings to two
   * different readers, which is how a signature ends up covering less
   * than what is used. */
  if ((size_t) decoded != strlen(signed_payload))
    return TOKEN_MALFORMED;

  if (ircd_base64_decode(dot + 1, 0, tag, sizeof(tag)) != (int) sizeof(tag))
    return TOKEN_MALFORMED;

  if (!token_sign(label, signed_payload, want))
    return TOKEN_NOKEY;

  /* The signature before the contents: what an unsigned token says is not
   * worth parsing, and comparing in constant time is what keeps a guess
   * from being told how close it was. */
  if (!ircd_crypto_equal(tag, want, sizeof(tag)))
    return TOKEN_BAD;

  if (!(colon = strchr(signed_payload, ':')) || colon == signed_payload
      || !colon[1])
    return TOKEN_MALFORMED;

  *colon = '\0';
  expiry = strtoul(signed_payload, NULL, 10);

  if (!expiry || (time_t) expiry <= now)
    return TOKEN_EXPIRED;

  if (strlen(colon + 1) >= len)
    return TOKEN_MALFORMED;

  ircd_strncpy(payload, colon + 1, len - 1);

  return TOKEN_OK;
}
