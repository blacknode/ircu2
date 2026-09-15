/*
 * IRC - Internet Relay Chat, ircd/ircd_sha256.c
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
 * @brief SHA-256 and HMAC-SHA-256.  See ircd_sha256.h.
 *
 * A plain transcription of FIPS 180-4, with no table anyone had to type
 * twice: the round constants are the only literals and the test vectors
 * in sha256_t would catch a wrong digit in any of them.
 */
#include "config.h"

#include "ircd_sha256.h"

#include <string.h>

/** Round constants: the first thirty-two bits of the fractional parts of
 * the cube roots of the first sixty-four primes (FIPS 180-4, 4.2.2). */
static const unsigned int K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
  0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
  0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
  0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
  0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
  0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
  0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
  0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

/** Rotate a 32-bit word right. */
#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/** Read a big-endian 32-bit word. */
static unsigned int load32(const unsigned char* p)
{
  return ((unsigned int) p[0] << 24) | ((unsigned int) p[1] << 16)
    | ((unsigned int) p[2] << 8) | (unsigned int) p[3];
}

/** Write a big-endian 32-bit word. */
static void store32(unsigned char* p, unsigned int v)
{
  p[0] = (unsigned char) (v >> 24);
  p[1] = (unsigned char) (v >> 16);
  p[2] = (unsigned char) (v >> 8);
  p[3] = (unsigned char) v;
}

/** Absorb one 64-byte block. */
static void sha256_block(struct Sha256Ctx* ctx, const unsigned char* p)
{
  unsigned int w[64];
  unsigned int a, b, c, d, e, f, g, h;
  int i;

  for (i = 0; i < 16; i++)
    w[i] = load32(p + i * 4);
  for (; i < 64; i++) {
    unsigned int s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    unsigned int s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);

    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
  e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];

  for (i = 0; i < 64; i++) {
    unsigned int s1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
    unsigned int ch = (e & f) ^ ((~e) & g);
    unsigned int t1 = h + s1 + ch + K[i] + w[i];
    unsigned int s0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
    unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
    unsigned int t2 = s0 + maj;

    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }

  ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d;
  ctx->h[4] += e; ctx->h[5] += f; ctx->h[6] += g; ctx->h[7] += h;

  memset(w, 0, sizeof(w));
}

/** Begin a digest.
 * @param[out] ctx Context to initialise.
 */
void ircd_sha256_init(struct Sha256Ctx* ctx)
{
  /* The first thirty-two bits of the fractional parts of the square roots
   * of the first eight primes (FIPS 180-4, 5.3.3).
   */
  ctx->h[0] = 0x6a09e667; ctx->h[1] = 0xbb67ae85;
  ctx->h[2] = 0x3c6ef372; ctx->h[3] = 0xa54ff53a;
  ctx->h[4] = 0x510e527f; ctx->h[5] = 0x9b05688c;
  ctx->h[6] = 0x1f83d9ab; ctx->h[7] = 0x5be0cd19;
  ctx->used = 0;
  ctx->length = 0;
  memset(ctx->block, 0, sizeof(ctx->block));
}

/** Add data to a digest.
 * @param[in,out] ctx Context.
 * @param[in] data Bytes to add.
 * @param[in] len How many.
 */
void ircd_sha256_update(struct Sha256Ctx* ctx, const void* data, size_t len)
{
  const unsigned char* p = (const unsigned char*) data;

  ctx->length += len;

  if (ctx->used) {
    size_t take = SHA256_BLOCK_LEN - ctx->used;

    if (take > len)
      take = len;
    memcpy(ctx->block + ctx->used, p, take);
    ctx->used += take;
    p += take;
    len -= take;

    if (ctx->used == SHA256_BLOCK_LEN) {
      sha256_block(ctx, ctx->block);
      ctx->used = 0;
    }
  }

  while (len >= SHA256_BLOCK_LEN) {
    sha256_block(ctx, p);
    p += SHA256_BLOCK_LEN;
    len -= SHA256_BLOCK_LEN;
  }

  if (len) {
    memcpy(ctx->block, p, len);
    ctx->used = len;
  }
}

/** Finish a digest.
 * @param[in,out] ctx Context; wiped before returning.
 * @param[out] out #SHA256_DIGEST_LEN bytes.
 */
void ircd_sha256_final(struct Sha256Ctx* ctx, unsigned char* out)
{
  unsigned long long bits = ctx->length * 8;
  int i;

  ctx->block[ctx->used++] = 0x80;

  if (ctx->used > SHA256_BLOCK_LEN - 8) {
    memset(ctx->block + ctx->used, 0, SHA256_BLOCK_LEN - ctx->used);
    sha256_block(ctx, ctx->block);
    ctx->used = 0;
  }

  memset(ctx->block + ctx->used, 0, SHA256_BLOCK_LEN - 8 - ctx->used);
  for (i = 0; i < 8; i++)
    ctx->block[SHA256_BLOCK_LEN - 1 - i] = (unsigned char) (bits >> (i * 8));

  sha256_block(ctx, ctx->block);

  for (i = 0; i < 8; i++)
    store32(out + i * 4, ctx->h[i]);

  /* The tail of whatever was hashed is still in the partial block. */
  ircd_crypto_wipe(ctx, sizeof(*ctx));
}

/** Digest in one call.
 * @param[in] data Bytes to digest.
 * @param[in] len How many.
 * @param[out] out #SHA256_DIGEST_LEN bytes.
 */
void ircd_sha256(const void* data, size_t len, unsigned char* out)
{
  struct Sha256Ctx ctx;

  ircd_sha256_init(&ctx);
  ircd_sha256_update(&ctx, data, len);
  ircd_sha256_final(&ctx, out);
}

/** HMAC-SHA-256.  See ircd_sha256.h. */
void ircd_hmac_sha256(const void* key, size_t keylen, const void* data,
                      size_t len, unsigned char* out)
{
  unsigned char k[SHA256_BLOCK_LEN];
  unsigned char pad[SHA256_BLOCK_LEN];
  unsigned char inner[SHA256_DIGEST_LEN];
  struct Sha256Ctx ctx;
  size_t i;

  memset(k, 0, sizeof(k));

  /* A key longer than the block is replaced by its digest; a shorter one
   * is padded with zeroes (RFC 2104).
   */
  if (keylen > SHA256_BLOCK_LEN)
    ircd_sha256(key, keylen, k);
  else
    memcpy(k, key, keylen);

  for (i = 0; i < SHA256_BLOCK_LEN; i++)
    pad[i] = k[i] ^ 0x36;

  ircd_sha256_init(&ctx);
  ircd_sha256_update(&ctx, pad, sizeof(pad));
  ircd_sha256_update(&ctx, data, len);
  ircd_sha256_final(&ctx, inner);

  for (i = 0; i < SHA256_BLOCK_LEN; i++)
    pad[i] = k[i] ^ 0x5c;

  ircd_sha256_init(&ctx);
  ircd_sha256_update(&ctx, pad, sizeof(pad));
  ircd_sha256_update(&ctx, inner, sizeof(inner));
  ircd_sha256_final(&ctx, out);

  ircd_crypto_wipe(k, sizeof(k));
  ircd_crypto_wipe(pad, sizeof(pad));
  ircd_crypto_wipe(inner, sizeof(inner));
}

/** Compare without leaking where the difference is.  See ircd_sha256.h. */
int ircd_crypto_equal(const void* a, const void* b, size_t len)
{
  const unsigned char* x = (const unsigned char*) a;
  const unsigned char* y = (const unsigned char*) b;
  unsigned char diff = 0;
  size_t i;

  /* Every byte, every time: the loop must not stop early, which is the
   * whole difference from memcmp().
   */
  for (i = 0; i < len; i++)
    diff |= (unsigned char) (x[i] ^ y[i]);

  return diff == 0;
}

/** Overwrite memory in a way the optimiser may not remove.
 *
 * Through a volatile pointer: a plain memset() on a buffer that is about
 * to go out of scope is dead code, and compilers do delete it.
 */
void ircd_crypto_wipe(void* p, size_t len)
{
  volatile unsigned char* q = (volatile unsigned char*) p;

  while (len--)
    *q++ = 0;
}
