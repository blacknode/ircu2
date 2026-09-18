#ifndef INCLUDED_ircd_sha256_h
#define INCLUDED_ircd_sha256_h
/*
 * IRC - Internet Relay Chat, include/ircd_sha256.h
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
 * @brief SHA-256 and HMAC-SHA-256 (FIPS 180-4, RFC 2104).
 *
 * The server's own, not the TLS backend's.  @c IRCU_TLS can be @c none,
 * and can be GnuTLS or libtls rather than OpenSSL, so anything that needs
 * a hash cannot reach for whichever library happens to be linked: it
 * would work on one build and not compile on the next.
 *
 * Used for signing what the server hands out and has to recognise again --
 * an upload token, a TURN credential, a session it issued -- where the
 * point is that nobody else can produce the same bytes.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

/** Length of a SHA-256 digest in bytes. */
#define SHA256_DIGEST_LEN 32
/** Length of a SHA-256 input block in bytes. */
#define SHA256_BLOCK_LEN 64

/** State of a SHA-256 computation in progress. */
struct Sha256Ctx {
  unsigned int  h[8];                    /**< Chaining value. */
  unsigned char block[SHA256_BLOCK_LEN]; /**< Partial block. */
  size_t        used;                    /**< Bytes held in #block. */
  unsigned long long length;             /**< Bytes hashed so far. */
};

/** Begin a digest. */
extern void ircd_sha256_init(struct Sha256Ctx* ctx);
/** Add \a len bytes of \a data to it. */
extern void ircd_sha256_update(struct Sha256Ctx* ctx, const void* data,
                               size_t len);
/** Finish it, writing #SHA256_DIGEST_LEN bytes to \a out.
 *
 * The context is wiped: a digest of a secret leaves its tail in the
 * partial block, and a context on the stack outlives the call that made
 * it.
 */
extern void ircd_sha256_final(struct Sha256Ctx* ctx, unsigned char* out);

/** Digest \a len bytes of \a data in one call. */
extern void ircd_sha256(const void* data, size_t len, unsigned char* out);

/** HMAC-SHA-256 of \a len bytes of \a data under \a key.
 * @param[in] key Key.
 * @param[in] keylen Its length; any length is accepted.
 * @param[in] data What to authenticate.
 * @param[in] len Its length.
 * @param[out] out #SHA256_DIGEST_LEN bytes.
 */
extern void ircd_hmac_sha256(const void* key, size_t keylen,
                             const void* data, size_t len,
                             unsigned char* out);

/** Compare \a len bytes without letting the time taken say where they
 * first differ.
 *
 * For anything the server checks against a value somebody else supplied:
 * a memcmp() that returns early tells whoever is guessing how much of the
 * guess was right, one byte at a time.
 * @return Non-zero if the two are equal.
 */
extern int ircd_crypto_equal(const void* a, const void* b, size_t len);

/** Overwrite \a len bytes of \a p, in a way the compiler may not remove.
 *
 * memset() on a buffer that is about to go out of scope is dead code as
 * far as the optimiser is concerned, and it does remove it.
 */
extern void ircd_crypto_wipe(void* p, size_t len);

#endif /* INCLUDED_ircd_sha256_h */
