/*
 * IRC - Internet Relay Chat, ircd/msgid.c
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
 * @brief The message identifier generator.
 *
 * See msgid.h for what an identifier is for.  This file is only how one is
 * made, and it is deliberately the whole of that: no clients, no sending,
 * no configuration, so that the one property that matters -- that an
 * identifier is never handed out twice -- can be tested on its own.
 */
#include "config.h"

#include "msgid.h"
#include "ircd_log.h"
#include "ircd_string.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <stdint.h>
#include <string.h>

/** Digits of the counter's base.  Letters and digits only: an identifier
 * travels as a tag value, and a character set that needs no escaping is
 * one fewer thing that can go wrong on the wire. */
static const char msgid_digits[] =
  "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/** Number of digits above. */
#define MSGID_BASE 62

/** Bits of the counter given to the sequence within one second. */
#define MSGID_SEQ_BITS 20

/** This server's numeric, which separates its identifiers from every other
 * server's without anything being agreed at run time. */
static char msgid_prefix[16];

/** The counter.  Seeded from the clock so that a restart resumes above
 * every identifier the previous run handed out, rather than walking back
 * over them.
 */
static uint64_t msgid_counter;

/** Identifiers generated since start-up. */
static unsigned long msgid_generated;

/** Start the generator.
 * @param[in] prefix The server's P10 numeric, or NULL.
 * @param[in] now Current time.
 */
void msgid_init(const char* prefix, time_t now)
{
  msgid_prefix[0] = '\0';
  if (prefix && *prefix) {
    ircd_strncpy(msgid_prefix, prefix, sizeof(msgid_prefix) - 1);
    msgid_prefix[sizeof(msgid_prefix) - 1] = '\0';
  }

  /* The clock in the high bits, the sequence in the low ones.  A server
   * that restarts inside the same second still resumes above where it
   * left off, because the sequence only ever counts up: at more than
   * 2^20 messages in one second it simply borrows from the next second's
   * range, which costs nothing -- what matters is that the counter never
   * goes backwards, not that it tracks the clock.
   */
  msgid_counter = ((uint64_t) now) << MSGID_SEQ_BITS;
  msgid_generated = 0;
}

/** Generate the next identifier.
 * @return A pointer to a static buffer, valid until the next call.
 */
const char* msgid_new(void)
{
  static char buf[MSGIDLEN + 1];
  char digits[24];
  uint64_t n = ++msgid_counter;
  size_t len = 0;
  size_t i;
  size_t pos;

  msgid_generated++;

  /* Base 62, least significant digit first, then reversed: the leading
   * zeroes a fixed-width rendering would need carry no information and
   * would make every identifier longer for as long as the server runs.
   */
  do {
    digits[len++] = msgid_digits[n % MSGID_BASE];
    n /= MSGID_BASE;
  } while (n > 0 && len < sizeof(digits));

  pos = 0;
  for (i = 0; msgid_prefix[i] && pos < sizeof(buf) - 1; i++)
    buf[pos++] = msgid_prefix[i];

  while (len > 0 && pos < sizeof(buf) - 1)
    buf[pos++] = digits[--len];

  buf[pos] = '\0';

  return buf;
}

/** Return non-zero if \a id is acceptable as an identifier from a peer.
 * @param[in] id Candidate identifier.
 */
int msgid_valid(const char* id)
{
  size_t len = 0;

  if (EmptyString(id))
    return 0;

  for (; id[len]; len++) {
    unsigned char c = (unsigned char) id[len];

    if (len >= MSGIDLEN)
      return 0;
    /* Printable ASCII, and none of the characters that delimit a tag: an
     * identifier that needed escaping would come back different.
     */
    if (c <= 0x20 || c >= 0x7f)
      return 0;
    if (c == ';' || c == '=' || c == ',' || c == '\\')
      return 0;
  }

  return 1;
}

/** Identifiers generated since start-up. */
unsigned long msgid_count(void)
{
  return msgid_generated;
}
