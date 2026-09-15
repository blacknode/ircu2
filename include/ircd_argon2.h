#ifndef INCLUDED_ircd_argon2_h
#define INCLUDED_ircd_argon2_h
/*
 * IRC - Internet Relay Chat, include/ircd_argon2.h
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
 * @brief Argon2id (RFC 9106) and the BLAKE2b it is built on.
 *
 * What a password is stored as.  Argon2 is deliberately expensive in both
 * time and memory, which is the point: an attacker with the stored hashes
 * and a warehouse of graphics cards is trying to guess billions of
 * passwords, and memory is the one cost that does not fall away when the
 * work moves off a general-purpose processor.
 *
 * **Never call this on the main thread.**  A sensible cost setting takes
 * between fifty and two hundred and fifty milliseconds and allocates tens
 * of megabytes, on purpose.  At a tenth of a second per check, ten people
 * logging in at once stop the server for a second; a hundred stop it for
 * ten.  It goes through worker_submit() -- see doc/readme.workers -- and
 * nothing here touches core state, which is what makes that safe.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

/** Length of a BLAKE2b-512 digest in bytes. */
#define BLAKE2B_DIGEST_LEN 64

/** BLAKE2b over \a len bytes of \a data.
 * @param[out] out \a outlen bytes; at most #BLAKE2B_DIGEST_LEN.
 * @param[in] outlen Digest length wanted.
 * @param[in] key Key, or NULL for an unkeyed hash.
 * @param[in] keylen Its length; at most 64.
 * @param[in] data What to hash.
 * @param[in] len Its length.
 */
extern void ircd_blake2b(unsigned char* out, size_t outlen,
                         const void* key, size_t keylen,
                         const void* data, size_t len);

/** Everything about an Argon2 hash other than the password and the salt. */
struct Argon2Params {
  /** A server-wide key mixed into every hash, or NULL.
   *
   * The pepper.  It is not stored with the hashes, so a stolen database on
   * its own is not enough to start guessing passwords against: the
   * attacker needs the server's configuration too.  It costs nothing and
   * it is the difference between one theft and two.
   */
  const void*  secret;
  size_t       secretlen;   /**< Its length. */

  /** Associated data mixed in, or NULL.  Rarely wanted; there for the
   * specification's sake and because the test vectors use it. */
  const void*  ad;
  size_t       adlen;       /**< Its length. */

  unsigned int t_cost;      /**< Passes over the memory; at least 1. */
  unsigned int m_cost;      /**< Memory in kibibytes. */
  unsigned int lanes;       /**< Degree of parallelism; at least 1. */
};

/** Argon2id.
 *
 * @param[out] out Where the tag goes.
 * @param[in] outlen How long a tag to produce; at least 4.
 * @param[in] pwd The password.
 * @param[in] pwdlen Its length.
 * @param[in] salt The salt; at least 8 bytes, and different every time.
 * @param[in] saltlen Its length.
 * @param[in] params Costs, and the optional secret and associated data.
 * @return Non-zero on success; zero if a parameter was out of range or
 *   the memory could not be had.
 */
extern int ircd_argon2id(unsigned char* out, size_t outlen,
                         const void* pwd, size_t pwdlen,
                         const void* salt, size_t saltlen,
                         const struct Argon2Params* params);

#endif /* INCLUDED_ircd_argon2_h */
