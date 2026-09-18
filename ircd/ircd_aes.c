/*
 * IRC - Internet Relay Chat, ircd/ircd_aes.c
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
 * @brief AES-256-GCM.  See ircd_aes.h.
 *
 * The substitution box is **computed**, not written down.  It is two
 * hundred and fifty-six constants that are entirely determined by the
 * definition -- the multiplicative inverse in GF(2^8) followed by an
 * affine map -- and a table that has to be transcribed is a table that
 * can be transcribed wrong, in a way that still compiles, still runs, and
 * still produces consistent-looking output.  Fifteen lines of arithmetic
 * cannot be wrong in that way.
 */
#include "config.h"

#include "ircd_aes.h"
#include "ircd_sha256.h"   /* ircd_crypto_equal(), ircd_crypto_wipe() */

#include <string.h>

/** The substitution box, filled in on first use. */
static unsigned char sbox[256];
/** Whether it has been. */
static int sbox_ready;

/** Multiply in GF(2^8) modulo the AES polynomial x^8+x^4+x^3+x+1. */
static unsigned char gmul(unsigned char a, unsigned char b)
{
  unsigned char r = 0;
  int i;

  for (i = 0; i < 8; i++) {
    if (b & 1)
      r ^= a;
    b >>= 1;
    a = (unsigned char) ((a << 1) ^ ((a & 0x80) ? 0x1b : 0x00));
  }

  return r;
}

/** Build the substitution box from its definition (FIPS 197, 5.1.1). */
static void sbox_build(void)
{
  unsigned char inv[256];
  int i;

  /* The multiplicative inverse of every non-zero element, found by
   * walking the field: a table of 255 products is cheaper to be sure of
   * than a table of 255 inverses copied from somewhere.
   */
  memset(inv, 0, sizeof(inv));
  for (i = 1; i < 256; i++) {
    int j;

    for (j = 1; j < 256; j++)
      if (gmul((unsigned char) i, (unsigned char) j) == 1) {
        inv[i] = (unsigned char) j;
        break;
      }
  }

  for (i = 0; i < 256; i++) {
    unsigned char x = inv[i];   /* zero maps to zero */
    unsigned char s = x;
    int k;

    for (k = 1; k <= 4; k++)
      s ^= (unsigned char) ((x << k) | (x >> (8 - k)));

    sbox[i] = (unsigned char) (s ^ 0x63);
  }

  sbox_ready = 1;
}

/** Substitute the four bytes of a word. */
static unsigned int subword(unsigned int w)
{
  return ((unsigned int) sbox[(w >> 24) & 0xff] << 24)
    | ((unsigned int) sbox[(w >> 16) & 0xff] << 16)
    | ((unsigned int) sbox[(w >> 8) & 0xff] << 8)
    | (unsigned int) sbox[w & 0xff];
}

/** Read a big-endian 32-bit word. */
static unsigned int load32(const unsigned char* p)
{
  return ((unsigned int) p[0] << 24) | ((unsigned int) p[1] << 16)
    | ((unsigned int) p[2] << 8) | (unsigned int) p[3];
}

/** Expand a key.  See ircd_aes.h. */
void ircd_aes256_expand(struct Aes256Key* out, const unsigned char* key)
{
  /* Round constants: x^(i-1) in GF(2^8).  AES-256 uses seven of them. */
  static const unsigned int rcon[7] = {
    0x01000000, 0x02000000, 0x04000000, 0x08000000,
    0x10000000, 0x20000000, 0x40000000
  };
  int i;

  if (!sbox_ready)
    sbox_build();

  for (i = 0; i < 8; i++)
    out->rk[i] = load32(key + i * 4);

  for (i = 8; i < 60; i++) {
    unsigned int t = out->rk[i - 1];

    if (i % 8 == 0)
      t = subword((t << 8) | (t >> 24)) ^ rcon[i / 8 - 1];
    else if (i % 8 == 4)
      t = subword(t);

    out->rk[i] = out->rk[i - 8] ^ t;
  }
}

/** Encrypt one block.  See ircd_aes.h. */
void ircd_aes256_encrypt_block(const struct Aes256Key* key,
                               const unsigned char* in, unsigned char* out)
{
  unsigned char s[16];
  int round;
  int i;

  memcpy(s, in, 16);

  /* AddRoundKey, round zero. */
  for (i = 0; i < 16; i++)
    s[i] ^= (unsigned char) (key->rk[i / 4] >> (24 - 8 * (i % 4)));

  for (round = 1; round <= 14; round++) {
    unsigned char t[16];

    /* SubBytes and ShiftRows together: row r moves left by r, and the
     * state is laid out column by column.
     */
    for (i = 0; i < 16; i++) {
      int row = i % 4;
      int col = i / 4;
      int from = ((col + row) % 4) * 4 + row;

      t[i] = sbox[s[from]];
    }

    if (round < 14) {
      /* MixColumns. */
      for (i = 0; i < 4; i++) {
        unsigned char* c = t + i * 4;
        unsigned char a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];

        c[0] = (unsigned char) (gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
        c[1] = (unsigned char) (a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
        c[2] = (unsigned char) (a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
        c[3] = (unsigned char) (gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
      }
    }

    for (i = 0; i < 16; i++)
      t[i] ^= (unsigned char) (key->rk[round * 4 + i / 4] >> (24 - 8 * (i % 4)));

    memcpy(s, t, 16);
    ircd_crypto_wipe(t, sizeof(t));
  }

  memcpy(out, s, 16);
  ircd_crypto_wipe(s, sizeof(s));
}

/** Multiply \a x by \a h in GF(2^128), the field GCM authenticates in.
 *
 * Bit by bit, with no precomputed table: a table indexed by secret data is
 * how GCM implementations leak their key to whoever can watch the cache,
 * and the server has no throughput problem that would justify one.
 */
static void ghash_mul(unsigned char* x, const unsigned char* h)
{
  unsigned char z[16];
  unsigned char v[16];
  int i;

  memset(z, 0, sizeof(z));
  memcpy(v, h, 16);

  for (i = 0; i < 128; i++) {
    if (x[i / 8] & (0x80 >> (i % 8))) {
      int j;

      for (j = 0; j < 16; j++)
        z[j] ^= v[j];
    }

    /* v <<= 1, and reduce by x^128 + x^7 + x^2 + x + 1 when it overflows. */
    {
      int lsb = v[15] & 1;
      int j;

      for (j = 15; j > 0; j--)
        v[j] = (unsigned char) ((v[j] >> 1) | ((v[j - 1] & 1) << 7));
      v[0] >>= 1;
      if (lsb)
        v[0] ^= 0xe1;
    }
  }

  memcpy(x, z, 16);
  ircd_crypto_wipe(z, sizeof(z));
  ircd_crypto_wipe(v, sizeof(v));
}

/** Absorb \a len bytes into the GHASH accumulator, zero-padded. */
static void ghash_update(unsigned char* y, const unsigned char* h,
                         const unsigned char* p, size_t len)
{
  while (len > 0) {
    size_t take = len < 16 ? len : 16;
    size_t i;

    for (i = 0; i < take; i++)
      y[i] ^= p[i];

    ghash_mul(y, h);
    p += take;
    len -= take;
  }
}

/** Write a 64-bit big-endian length in bits. */
static void store_bits(unsigned char* p, size_t bytes)
{
  unsigned long long bits = (unsigned long long) bytes * 8;
  int i;

  for (i = 0; i < 8; i++)
    p[7 - i] = (unsigned char) (bits >> (i * 8));
}

/** The GCM counter block for a 96-bit nonce and counter \a ctr. */
static void gcm_counter(unsigned char* out, const unsigned char* nonce,
                        unsigned int ctr)
{
  memcpy(out, nonce, AES_GCM_NONCE_LEN);
  out[12] = (unsigned char) (ctr >> 24);
  out[13] = (unsigned char) (ctr >> 16);
  out[14] = (unsigned char) (ctr >> 8);
  out[15] = (unsigned char) ctr;
}

/** The common half of sealing and opening: the keystream and the tag. */
static void gcm_core(const struct Aes256Key* key, const unsigned char* nonce,
                     const void* aad, size_t aadlen,
                     const unsigned char* in, size_t len,
                     unsigned char* out, unsigned char* tag)
{
  unsigned char h[16];
  unsigned char zero[16];
  unsigned char y[16];
  unsigned char block[16];
  unsigned char lengths[16];
  unsigned int ctr = 2;   /* one is spoken for by the tag */
  size_t done = 0;

  memset(zero, 0, sizeof(zero));
  ircd_aes256_encrypt_block(key, zero, h);

  /* Counter mode over the plaintext.  Note this is the ciphertext in both
   * directions: the keystream is the same, which is why opening can reuse
   * it and why a nonce must never be repeated.
   */
  while (done < len) {
    size_t take = (len - done) < 16 ? (len - done) : 16;
    size_t i;

    gcm_counter(block, nonce, ctr++);
    ircd_aes256_encrypt_block(key, block, block);

    for (i = 0; i < take; i++)
      out[done + i] = (unsigned char) (in[done + i] ^ block[i]);

    done += take;
  }

  /* The tag is over the additional data and the ciphertext, each padded to
   * a block, then the two lengths.
   */
  memset(y, 0, sizeof(y));
  if (aad && aadlen)
    ghash_update(y, h, (const unsigned char*) aad, aadlen);

  {
    /* Always the ciphertext, whichever direction we are going. */
    const unsigned char* ct = (out == in) ? out : (len ? out : out);

    if (len)
      ghash_update(y, h, ct, len);
  }

  store_bits(lengths, aadlen);
  store_bits(lengths + 8, len);
  ghash_update(y, h, lengths, 16);

  gcm_counter(block, nonce, 1);
  ircd_aes256_encrypt_block(key, block, block);

  {
    int i;

    for (i = 0; i < AES_GCM_TAG_LEN; i++)
      tag[i] = (unsigned char) (y[i] ^ block[i]);
  }

  ircd_crypto_wipe(h, sizeof(h));
  ircd_crypto_wipe(y, sizeof(y));
  ircd_crypto_wipe(block, sizeof(block));
}

/** Encrypt and authenticate.  See ircd_aes.h. */
void ircd_aes256_gcm_seal(const struct Aes256Key* key,
                          const unsigned char* nonce,
                          const void* aad, size_t aadlen,
                          const void* in, size_t len,
                          unsigned char* out, unsigned char* tag)
{
  gcm_core(key, nonce, aad, aadlen, (const unsigned char*) in, len, out, tag);
}

/** Check the tag and decrypt.  See ircd_aes.h. */
int ircd_aes256_gcm_open(const struct Aes256Key* key,
                         const unsigned char* nonce,
                         const void* aad, size_t aadlen,
                         const void* in, size_t len,
                         const unsigned char* tag, unsigned char* out)
{
  unsigned char h[16];
  unsigned char zero[16];
  unsigned char y[16];
  unsigned char block[16];
  unsigned char lengths[16];
  unsigned char want[AES_GCM_TAG_LEN];
  const unsigned char* ct = (const unsigned char*) in;
  unsigned int ctr = 2;
  size_t done = 0;

  memset(zero, 0, sizeof(zero));
  ircd_aes256_encrypt_block(key, zero, h);

  /* The tag first, over the ciphertext as it arrived: decrypting before
   * checking would hand the caller whatever an attacker chose, and a
   * caller that forgot to look at the return value would use it.
   */
  memset(y, 0, sizeof(y));
  if (aad && aadlen)
    ghash_update(y, h, (const unsigned char*) aad, aadlen);
  if (len)
    ghash_update(y, h, ct, len);

  store_bits(lengths, aadlen);
  store_bits(lengths + 8, len);
  ghash_update(y, h, lengths, 16);

  gcm_counter(block, nonce, 1);
  ircd_aes256_encrypt_block(key, block, block);

  {
    int i;

    for (i = 0; i < AES_GCM_TAG_LEN; i++)
      want[i] = (unsigned char) (y[i] ^ block[i]);
  }

  if (!ircd_crypto_equal(want, tag, AES_GCM_TAG_LEN)) {
    ircd_crypto_wipe(h, sizeof(h));
    ircd_crypto_wipe(y, sizeof(y));
    ircd_crypto_wipe(block, sizeof(block));
    ircd_crypto_wipe(want, sizeof(want));
    return 0;
  }

  while (done < len) {
    size_t take = (len - done) < 16 ? (len - done) : 16;
    size_t i;

    gcm_counter(block, nonce, ctr++);
    ircd_aes256_encrypt_block(key, block, block);

    for (i = 0; i < take; i++)
      out[done + i] = (unsigned char) (ct[done + i] ^ block[i]);

    done += take;
  }

  ircd_crypto_wipe(h, sizeof(h));
  ircd_crypto_wipe(y, sizeof(y));
  ircd_crypto_wipe(block, sizeof(block));
  ircd_crypto_wipe(want, sizeof(want));

  return 1;
}
