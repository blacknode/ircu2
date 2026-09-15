#ifndef INCLUDED_ircd_aes_h
#define INCLUDED_ircd_aes_h
/*
 * IRC - Internet Relay Chat, include/ircd_aes.h
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
 * @brief AES-256 in GCM (FIPS 197, NIST SP 800-38D).
 *
 * The server's own, for the same reason as the hash: @c IRCU_TLS can be
 * @c none and can be a library other than OpenSSL, so anything that needs
 * a cipher cannot reach for whichever one happens to be linked.
 *
 * **GCM and not raw AES on purpose.**  There is no interface here for
 * encrypting a block without authenticating it, because every use this
 * server has -- sealing a token it will read back, keeping a second-factor
 * secret at rest -- is one where an attacker who can change the ciphertext
 * without being noticed has broken the thing entirely.  A cipher without a
 * tag is a cipher whose output anybody can edit.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

/** AES-256 key length in bytes. */
#define AES256_KEY_LEN 32
/** AES block length in bytes. */
#define AES_BLOCK_LEN 16
/** Nonce length this implementation takes, in bytes.
 *
 * Ninety-six bits: the size GCM is defined around, the only one that needs
 * no extra hashing step, and the one NIST recommends.  A different length
 * is refused rather than quietly hashed into shape.
 */
#define AES_GCM_NONCE_LEN 12
/** Authentication tag length in bytes. */
#define AES_GCM_TAG_LEN 16

/** An expanded AES-256 key.  Encryption direction only: GCM never needs
 * the other one. */
struct Aes256Key {
  unsigned int rk[60];   /**< Round keys. */
};

/** Expand \a key into \a out.
 * @param[out] out Expanded key.
 * @param[in] key #AES256_KEY_LEN bytes.
 */
extern void ircd_aes256_expand(struct Aes256Key* out, const unsigned char* key);

/** Encrypt and authenticate.
 *
 * @param[in] key Expanded key.
 * @param[in] nonce #AES_GCM_NONCE_LEN bytes.  **Never reuse one with the
 *   same key**: two messages under one nonce hand an attacker the
 *   difference of the plaintexts and, worse, the authentication key.
 * @param[in] aad Data to authenticate but not encrypt, or NULL.
 * @param[in] aadlen Its length.
 * @param[in] in Plaintext.
 * @param[in] len Its length.
 * @param[out] out Ciphertext, \a len bytes.  May be \a in.
 * @param[out] tag #AES_GCM_TAG_LEN bytes.
 */
extern void ircd_aes256_gcm_seal(const struct Aes256Key* key,
                                 const unsigned char* nonce,
                                 const void* aad, size_t aadlen,
                                 const void* in, size_t len,
                                 unsigned char* out, unsigned char* tag);

/** Check the tag and decrypt.
 *
 * Nothing is written to \a out unless the tag is right: a caller that
 * forgot to check the return value would otherwise be handed whatever an
 * attacker chose.
 *
 * @param[in] key Expanded key.
 * @param[in] nonce #AES_GCM_NONCE_LEN bytes.
 * @param[in] aad Data that was authenticated but not encrypted, or NULL.
 * @param[in] aadlen Its length.
 * @param[in] in Ciphertext.
 * @param[in] len Its length.
 * @param[in] tag #AES_GCM_TAG_LEN bytes.
 * @param[out] out Plaintext, \a len bytes.  May be \a in.
 * @return Non-zero if the tag was right and \a out was written.
 */
extern int ircd_aes256_gcm_open(const struct Aes256Key* key,
                                const unsigned char* nonce,
                                const void* aad, size_t aadlen,
                                const void* in, size_t len,
                                const unsigned char* tag, unsigned char* out);

/** Encrypt one block with a bare key.  For GHASH and for test vectors;
 * not a way to encrypt a message.
 * @param[in] key Expanded key.
 * @param[in] in #AES_BLOCK_LEN bytes.
 * @param[out] out #AES_BLOCK_LEN bytes.
 */
extern void ircd_aes256_encrypt_block(const struct Aes256Key* key,
                                      const unsigned char* in,
                                      unsigned char* out);

#endif /* INCLUDED_ircd_aes_h */
