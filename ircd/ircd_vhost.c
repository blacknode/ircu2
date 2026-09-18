/*
 * IRC - Internet Relay Chat, ircd/ircd_vhost.c
 * Copyright (C) 2026 ircu2 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
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
 * @brief Virtual host generation.
 * @version $Id$
 *
 * The scheme follows the one IRC-Hispano's ircd has used since 2002: the
 * real address goes through TEA under a key derived from twelve base64
 * characters, and the two output words are printed in the same base64
 * alphabet that P10 numerics use.  Keeping the derivation identical means
 * an operator can reproduce a user's host from its IP with an external tool
 * and the same key.
 *
 * Two things the original leaves to chance are pinned down here.  The
 * base64 alphabet ends in '[' and ']', which are not hostname characters;
 * the IPv4 path retries with a counter folded into the input block until
 * neither appears, and the IPv6 path -- which has no spare input bits --
 * retries with the counter as the block's initialisation vector instead.
 * The first attempt of either is byte-for-byte what the original produces,
 * so only hosts the original could not have used differ.
 */
#include "config.h"

#include "ircd_vhost.h"
#include "ircd_string.h"
#include "numnicks.h"
#include "res.h"

#include <string.h>

/** The key as the operator wrote it, for /STATS and the like. */
static char vhost_key_str[VHOST_KEY_LEN + 1];
/** The key as TEA wants it. */
static unsigned int vhost_k[2];
/** Non-zero once a valid key has been loaded. */
static int vhost_key_loaded;

/** Key the configuration pass in progress has offered, if any. */
static char vhost_pending[VHOST_KEY_LEN + 1];
/** Non-zero if the pass in progress mentioned a Security block. */
static int vhost_pending_seen;

/** Number of base64 characters per output word. */
#define VHOST_WORD_LEN 6
/** Number of attempts before giving up on a bracket-free host. */
#define VHOST_MAX_TRIES 65535

/** Encrypt one 64-bit block with TEA.
 *
 * The block is XORed with \a x before encryption so that chaining is a
 * matter of feeding the previous output back in; with \a x zero this is
 * plain TEA.  Only the lower 64 bits of the 128-bit TEA key are used.
 *
 * @param[in] v Plaintext block.
 * @param[in] k Key, two 32-bit words.
 * @param[in,out] x Initialisation vector on entry, ciphertext on exit.
 */
void vhost_tea(const unsigned int v[2], const unsigned int k[2],
               unsigned int x[2])
{
  unsigned int y = v[0] ^ x[0], z = v[1] ^ x[1];
  unsigned int sum = 0, delta = 0x9E3779B9;
  unsigned int a = k[0], b = k[1], c = 0, d = 0;
  unsigned int n = 32;

  while (n-- > 0) {
    sum += delta;
    y += ((z << 4) + a) ^ ((z + sum) ^ ((z >> 5) + b));
    z += ((y << 4) + c) ^ ((y + sum) ^ ((y >> 5) + d));
  }

  x[0] = y;
  x[1] = z;
}

/** Is \a c a character of the P10 base64 alphabet? */
static int vhost_is_base64(int c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
    || (c >= '0' && c <= '9') || c == '[' || c == ']';
}

/** Check that \a key is a well-formed virtual host key.
 * @param[in] key Candidate key.
 * @return Non-zero if \a key is exactly ::VHOST_KEY_LEN base64 characters.
 */
int vhost_key_valid(const char* key)
{
  int i;

  if (!key)
    return 0;
  for (i = 0; i < VHOST_KEY_LEN; ++i)
    if (!vhost_is_base64((unsigned char)key[i]))
      return 0;
  return key[VHOST_KEY_LEN] == '\0';
}

/** Turn a key string into the two words TEA takes. */
static void vhost_derive(const char* key, unsigned int k[2])
{
  char half[VHOST_WORD_LEN + 1];

  memcpy(half, key, VHOST_WORD_LEN);
  half[VHOST_WORD_LEN] = '\0';
  k[0] = base64toint(half);
  memcpy(half, key + VHOST_WORD_LEN, VHOST_WORD_LEN);
  k[1] = base64toint(half);
}

/** Install \a key as the key every host is generated with.
 * @param[in] key Key to use.
 * @return Non-zero on success, zero if \a key is malformed (the previous
 * key, if any, stays in force).
 */
int vhost_set_key(const char* key)
{
  if (!vhost_key_valid(key))
    return 0;
  ircd_strncpy(vhost_key_str, key, VHOST_KEY_LEN);
  vhost_derive(vhost_key_str, vhost_k);
  vhost_key_loaded = 1;
  return 1;
}

/** Has a key been installed? */
int vhost_have_key(void)
{
  return vhost_key_loaded;
}

/** The installed key, or an empty string. */
const char* vhost_key(void)
{
  return vhost_key_str;
}

/** Render two words as "xxxxxx.yyyyyy" in \a buf and report whether the
 * result is fit for a hostname.
 */
static int vhost_render(char* buf, unsigned int x0, unsigned int x1)
{
  inttobase64(buf, x0, VHOST_WORD_LEN);
  buf[VHOST_WORD_LEN] = '.';
  inttobase64(buf + VHOST_WORD_LEN + 1, x1, VHOST_WORD_LEN);
  return !strchr(buf, '[') && !strchr(buf, ']');
}

/** Generate the virtual host for \a ip.
 *
 * @param[out] buf Where to write the host.
 * @param[in] len Size of \a buf; ::VHOST_MAX_LEN + 1 is always enough.
 * @param[in] ip Address to hide.
 * @return Non-zero on success; zero if no key is loaded or \a buf is too
 * small, in which case \a buf holds an empty string.
 */
int vhost_make(char* buf, size_t len, const struct irc_in_addr* ip)
{
  char body[2 * VHOST_WORD_LEN + 2];
  unsigned int v[2], x[2];
  unsigned int ts;
  const char* suffix;

  if (len > 0)
    buf[0] = '\0';
  if (!vhost_key_loaded || len < VHOST_MAX_LEN + 1)
    return 0;

  if (irc_in_addr_is_ipv4(ip)) {
    /* The upper half of the first word is taken from the key and the lower
     * half is the retry counter; the second word is the address itself.
     */
    suffix = ".v4";
    for (ts = 0; ; ++ts) {
      x[0] = x[1] = 0;
      v[0] = (vhost_k[0] & 0xffff0000) + ts;
      v[1] = ((unsigned int)ntohs(ip->in6_16[6]) << 16)
        | ntohs(ip->in6_16[7]);
      vhost_tea(v, vhost_k, x);
      if (vhost_render(body, x[0], x[1]))
        break;
      if (ts == VHOST_MAX_TRIES)
        return 0;
    }
  } else {
    /* The first 64 bits of the address are the whole block, so a retry
     * has nowhere to go but the IV.
     */
    suffix = ".v6";
    v[0] = ((unsigned int)ntohs(ip->in6_16[0]) << 16) | ntohs(ip->in6_16[1]);
    v[1] = ((unsigned int)ntohs(ip->in6_16[2]) << 16) | ntohs(ip->in6_16[3]);
    for (ts = 0; ; ++ts) {
      x[0] = ts;
      x[1] = 0;
      vhost_tea(v, vhost_k, x);
      if (vhost_render(body, x[0], x[1]))
        break;
      if (ts == VHOST_MAX_TRIES)
        return 0;
    }
  }

  memcpy(buf, body, 2 * VHOST_WORD_LEN + 1);
  strcpy(buf + 2 * VHOST_WORD_LEN + 1, suffix);
  return 1;
}

/*
 * Configuration.  The parser hands over whatever the Security block says;
 * the sweep at the end of the pass decides what to do with it.
 */

/** Forget the key offered by any earlier pass. */
void vhost_conf_unmark(void)
{
  vhost_pending[0] = '\0';
  vhost_pending_seen = 0;
}

/** Record the key the Security block names.
 * @param[in] key Value of virtual_host_key.
 * @return Non-zero if \a key is well-formed.
 */
int vhost_conf_set_key(const char* key)
{
  vhost_pending_seen = 1;
  if (!vhost_key_valid(key))
    return 0;
  ircd_strncpy(vhost_pending, key, VHOST_KEY_LEN);
  return 1;
}

/** Finish a configuration pass.
 *
 * A well-formed key replaces the one in force.  A pass with no Security
 * block, or one whose key was rejected, leaves the previous key in place
 * so that a rehash cannot strip a running server of the ability to hide
 * hosts; the caller reports the omission and, at start-up, refuses to run.
 *
 * @return Non-zero if a key is in force after the pass.
 */
int vhost_conf_sweep(void)
{
  if (vhost_pending[0])
    vhost_set_key(vhost_pending);
  vhost_pending[0] = '\0';
  vhost_pending_seen = 0;
  return vhost_key_loaded;
}
