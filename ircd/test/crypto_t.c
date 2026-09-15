/* crypto_t.c - Test the server's own hash and cipher against the vectors
 * the standards publish.
 *
 * These are not a formality.  Both files contain constants that are fully
 * determined by their definition -- the SHA-256 round constants, the AES
 * substitution box -- and a wrong digit in any of them produces something
 * that still compiles, still runs, and still gives consistent-looking
 * output.  The published vectors are the only thing that can tell the
 * difference, which is why the AES box is computed rather than typed and
 * why every one of these is here.
 *
 * Sources: FIPS 180-4 (SHA-256), RFC 4231 (HMAC-SHA-256), FIPS 197
 * appendix C.3 (AES-256), and the GCM specification's own test cases.
 */

#include "ircd_aes.h"
#include "ircd_argon2.h"
#include "ircd_pwhash.h"
#include "ircd_sha256.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Render \a len bytes of \a p as hex into \a out. */
static const char* hex(const unsigned char* p, size_t len)
{
  static char out[256];
  size_t i;

  for (i = 0; i < len && i * 2 + 2 < sizeof(out); i++)
    sprintf(out + i * 2, "%02x", p[i]);

  return out;
}

/** Parse hex into \a out, returning how many bytes. */
static size_t unhex(const char* s, unsigned char* out)
{
  size_t n = 0;

  while (s[0] && s[1]) {
    unsigned int b;

    sscanf(s, "%2x", &b);
    out[n++] = (unsigned char) b;
    s += 2;
  }

  return n;
}

static void check(const char* what, const unsigned char* got, size_t len,
                  const char* want)
{
  if (strcmp(hex(got, len), want)) {
    printf("FAIL %s\n  got  %s\n  want %s\n", what, hex(got, len), want);
    assert(0);
  }
}

/* ------------------------------------------------------------------ */

/** SHA-256, the three vectors of FIPS 180-4. */
static void test_sha256(void)
{
  unsigned char d[SHA256_DIGEST_LEN];

  ircd_sha256("", 0, d);
  check("sha256(\"\")", d, sizeof(d),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  ircd_sha256("abc", 3, d);
  check("sha256(\"abc\")", d, sizeof(d),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  ircd_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
  check("sha256(two-block)", d, sizeof(d),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  printf("ok - SHA-256 (FIPS 180-4)\n");
}

/** The same digest arrived at one byte at a time.
 *
 * The buffering in update() is where a hash implementation actually goes
 * wrong: the block boundary is only exercised by an input that crosses it
 * awkwardly.
 */
static void test_sha256_streaming(void)
{
  static const char* text =
    "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
    "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
  unsigned char whole[SHA256_DIGEST_LEN];
  unsigned char piece[SHA256_DIGEST_LEN];
  struct Sha256Ctx ctx;
  size_t len = strlen(text);
  size_t i;

  ircd_sha256(text, len, whole);
  check("sha256(112 bytes)", whole, sizeof(whole),
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

  ircd_sha256_init(&ctx);
  for (i = 0; i < len; i++)
    ircd_sha256_update(&ctx, text + i, 1);
  ircd_sha256_final(&ctx, piece);

  assert(ircd_crypto_equal(whole, piece, sizeof(whole)));

  /* And in awkward chunks that straddle the block boundary. */
  {
    size_t chunk;

    for (chunk = 1; chunk <= 70; chunk++) {
      size_t off = 0;

      ircd_sha256_init(&ctx);
      while (off < len) {
        size_t take = (len - off) < chunk ? (len - off) : chunk;

        ircd_sha256_update(&ctx, text + off, take);
        off += take;
      }
      ircd_sha256_final(&ctx, piece);
      assert(ircd_crypto_equal(whole, piece, sizeof(whole)));
    }
  }

  printf("ok - SHA-256 gives the same answer however it is fed\n");
}

/** HMAC-SHA-256, the first vectors of RFC 4231. */
static void test_hmac(void)
{
  unsigned char key[131];
  unsigned char mac[SHA256_DIGEST_LEN];

  memset(key, 0x0b, 20);
  ircd_hmac_sha256(key, 20, "Hi There", 8, mac);
  check("hmac case 1", mac, sizeof(mac),
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

  ircd_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, mac);
  check("hmac case 2", mac, sizeof(mac),
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

  /* A key longer than the block: it is replaced by its own digest, which
   * is the branch nothing else reaches.
   */
  memset(key, 0xaa, 131);
  ircd_hmac_sha256(key, 131,
                   "Test Using Larger Than Block-Size Key - Hash Key First",
                   54, mac);
  check("hmac case 6", mac, sizeof(mac),
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

  printf("ok - HMAC-SHA-256 (RFC 4231)\n");
}

/** AES-256, the vector of FIPS 197 appendix C.3.
 *
 * This one checks the computed substitution box on its own, before GCM is
 * involved: if the box or the key schedule were wrong, nothing else here
 * would be meaningful.
 */
static void test_aes_block(void)
{
  unsigned char key[32];
  unsigned char in[16];
  unsigned char out[16];
  struct Aes256Key ks;

  unhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key);
  unhex("00112233445566778899aabbccddeeff", in);

  ircd_aes256_expand(&ks, key);
  ircd_aes256_encrypt_block(&ks, in, out);

  check("aes-256 block", out, sizeof(out), "8ea2b7ca516745bfeafc49904b496089");

  printf("ok - AES-256 (FIPS 197 C.3)\n");
}

/** AES-256-GCM, the specification's own test cases. */
static void test_gcm(void)
{
  unsigned char key[32];
  unsigned char nonce[AES_GCM_NONCE_LEN];
  unsigned char pt[16];
  unsigned char ct[16];
  unsigned char tag[AES_GCM_TAG_LEN];
  struct Aes256Key ks;

  memset(key, 0, sizeof(key));
  memset(nonce, 0, sizeof(nonce));
  ircd_aes256_expand(&ks, key);

  /* Nothing at all: the tag still has to be right. */
  ircd_aes256_gcm_seal(&ks, nonce, 0, 0, 0, 0, ct, tag);
  check("gcm case 13 tag", tag, sizeof(tag),
        "530f8afbc74536b9a963b4f1c4cb738b");

  /* One block of zeroes. */
  memset(pt, 0, sizeof(pt));
  ircd_aes256_gcm_seal(&ks, nonce, 0, 0, pt, sizeof(pt), ct, tag);
  check("gcm case 14 ct", ct, sizeof(ct), "cea7403d4d606b6e074ec5d3baf39d18");
  check("gcm case 14 tag", tag, sizeof(tag),
        "d0d1c8a799996bf0265b98b5d48ab919");

  printf("ok - AES-256-GCM (specification test cases)\n");
}

/** What is sealed comes back, and what is tampered with does not. */
static void test_gcm_roundtrip(void)
{
  unsigned char key[32];
  unsigned char nonce[AES_GCM_NONCE_LEN];
  unsigned char tag[AES_GCM_TAG_LEN];
  unsigned char ct[64];
  unsigned char back[64];
  struct Aes256Key ks;
  const char* msg = "a token the server will have to recognise again";
  const char* aad = "not secret, but it must not change either";
  size_t len = strlen(msg);
  size_t i;

  for (i = 0; i < sizeof(key); i++)
    key[i] = (unsigned char) (i * 7 + 1);
  for (i = 0; i < sizeof(nonce); i++)
    nonce[i] = (unsigned char) (i + 3);

  ircd_aes256_expand(&ks, key);
  ircd_aes256_gcm_seal(&ks, nonce, aad, strlen(aad), msg, len, ct, tag);

  assert(memcmp(ct, msg, len) != 0 && "it did not encrypt anything");

  assert(ircd_aes256_gcm_open(&ks, nonce, aad, strlen(aad), ct, len, tag, back));
  assert(0 == memcmp(back, msg, len));

  /* Every way of changing it must be caught. */
  ct[0] ^= 1;
  assert(!ircd_aes256_gcm_open(&ks, nonce, aad, strlen(aad), ct, len, tag, back));
  ct[0] ^= 1;

  tag[AES_GCM_TAG_LEN - 1] ^= 1;
  assert(!ircd_aes256_gcm_open(&ks, nonce, aad, strlen(aad), ct, len, tag, back));
  tag[AES_GCM_TAG_LEN - 1] ^= 1;

  /* Including changing the part that was authenticated but not encrypted:
   * that is the whole reason for passing it in.
   */
  assert(!ircd_aes256_gcm_open(&ks, nonce, "something else", 14, ct, len, tag,
                               back));

  /* And a different nonce is a different message. */
  nonce[0] ^= 1;
  assert(!ircd_aes256_gcm_open(&ks, nonce, aad, strlen(aad), ct, len, tag, back));
  nonce[0] ^= 1;

  /* Sealing in place is allowed, and the answer is the same. */
  memcpy(back, msg, len);
  ircd_aes256_gcm_seal(&ks, nonce, aad, strlen(aad), back, len, back, tag);
  assert(0 == memcmp(back, ct, len));
  assert(ircd_aes256_gcm_open(&ks, nonce, aad, strlen(aad), back, len, tag,
                              back));
  assert(0 == memcmp(back, msg, len));

  printf("ok - GCM round trip, and every tampering caught\n");
}

/** Lengths that are not a whole number of blocks. */
static void test_gcm_lengths(void)
{
  unsigned char key[32];
  unsigned char nonce[AES_GCM_NONCE_LEN];
  unsigned char tag[AES_GCM_TAG_LEN];
  unsigned char pt[70];
  unsigned char ct[70];
  unsigned char back[70];
  struct Aes256Key ks;
  size_t len;

  memset(key, 0x42, sizeof(key));
  memset(nonce, 0x24, sizeof(nonce));
  ircd_aes256_expand(&ks, key);

  for (len = 0; len < sizeof(pt); len++) {
    size_t i;

    for (i = 0; i < len; i++)
      pt[i] = (unsigned char) (i * 3);

    ircd_aes256_gcm_seal(&ks, nonce, 0, 0, pt, len, ct, tag);
    assert(ircd_aes256_gcm_open(&ks, nonce, 0, 0, ct, len, tag, back));
    assert(0 == memcmp(back, pt, len));
  }

  printf("ok - every length from nothing to four and a half blocks\n");
}

/** The comparison that must not say where it stopped. */
static void test_equal(void)
{
  unsigned char a[16];
  unsigned char b[16];

  memset(a, 0x5a, sizeof(a));
  memcpy(b, a, sizeof(b));

  assert(ircd_crypto_equal(a, b, sizeof(a)));

  b[0] ^= 1;
  assert(!ircd_crypto_equal(a, b, sizeof(a)));
  b[0] ^= 1;

  b[sizeof(b) - 1] ^= 0x80;
  assert(!ircd_crypto_equal(a, b, sizeof(a)));

  assert(ircd_crypto_equal(a, b, 0) && "nothing equals nothing");

  printf("ok - constant-time comparison\n");
}

/** BLAKE2b, the vectors of RFC 7693 and the reference implementation. */
static void test_blake2b(void)
{
  unsigned char d[BLAKE2B_DIGEST_LEN];

  ircd_blake2b(d, 64, 0, 0, "", 0);
  check("blake2b(\"\")", d, sizeof(d),
        "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419"
        "d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce");

  ircd_blake2b(d, 64, 0, 0, "abc", 3);
  check("blake2b(\"abc\")", d, sizeof(d),
        "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
        "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");

  printf("ok - BLAKE2b (RFC 7693)\n");
}

/** Argon2id, the test vector of RFC 9106. */
static void test_argon2id(void)
{
  unsigned char pwd[32];
  unsigned char salt[16];
  unsigned char secret[8];
  unsigned char ad[12];
  unsigned char tag[32];
  struct Argon2Params p;

  memset(pwd, 0x01, sizeof(pwd));
  memset(salt, 0x02, sizeof(salt));
  memset(secret, 0x03, sizeof(secret));
  memset(ad, 0x04, sizeof(ad));

  memset(&p, 0, sizeof(p));
  p.secret = secret; p.secretlen = sizeof(secret);
  p.ad = ad;         p.adlen = sizeof(ad);
  p.t_cost = 3; p.m_cost = 32; p.lanes = 4;

  assert(ircd_argon2id(tag, sizeof(tag), pwd, sizeof(pwd), salt, sizeof(salt),
                       &p));
  check("argon2id RFC 9106", tag, sizeof(tag),
        "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659");

  printf("ok - Argon2id (RFC 9106)\n");
}

/** What the hash is actually for: the same password twice, and a wrong
 * one never.
 */
static void test_argon2_behaviour(void)
{
  unsigned char a[32], b[32], c[32];
  const char* salt = "a salt long enough";
  struct Argon2Params q;

  memset(&q, 0, sizeof(q));
  q.t_cost = 2; q.m_cost = 64; q.lanes = 1;

  assert(ircd_argon2id(a, sizeof(a), "correct horse", 13, salt, strlen(salt), &q));
  assert(ircd_argon2id(b, sizeof(b), "correct horse", 13, salt, strlen(salt), &q));
  assert(ircd_crypto_equal(a, b, sizeof(a)) && "not deterministic");

  assert(ircd_argon2id(c, sizeof(c), "correct horsf", 13, salt, strlen(salt), &q));
  assert(!ircd_crypto_equal(a, c, sizeof(a)));

  /* A different salt is a different hash: two people with one password do
   * not get one entry an attacker can crack once.
   */
  assert(ircd_argon2id(c, sizeof(c), "correct horse", 13, "another salt!", 13,
                       &q));
  assert(!ircd_crypto_equal(a, c, sizeof(a)));

  /* And so is a different cost, so a hash cannot be replayed as a cheaper
   * one.
   */
  q.t_cost = 3;
  assert(ircd_argon2id(c, sizeof(c), "correct horse", 13, salt, strlen(salt), &q));
  assert(!ircd_crypto_equal(a, c, sizeof(a)));
  q.t_cost = 2; q.m_cost = 128;
  assert(ircd_argon2id(c, sizeof(c), "correct horse", 13, salt, strlen(salt), &q));
  assert(!ircd_crypto_equal(a, c, sizeof(a)));
  q.m_cost = 64;

  /* The pepper changes the answer, which is the whole of what it is for:
   * the same password and salt hash differently on a server that has one.
   */
  q.secret = "a server-wide key";
  q.secretlen = 17;
  assert(ircd_argon2id(c, sizeof(c), "correct horse", 13, salt, strlen(salt), &q));
  assert(!ircd_crypto_equal(a, c, sizeof(a)));
  q.secret = 0; q.secretlen = 0;

  /* Parameters that would weaken it are refused rather than adjusted. */
  assert(!ircd_argon2id(a, sizeof(a), "x", 1, "short", 5, &q));
  q.t_cost = 0;
  assert(!ircd_argon2id(a, sizeof(a), "x", 1, salt, strlen(salt), &q));
  q.t_cost = 2; q.lanes = 0;
  assert(!ircd_argon2id(a, sizeof(a), "x", 1, salt, strlen(salt), &q));
  q.lanes = 1;
  assert(!ircd_argon2id(a, 2, "x", 1, salt, strlen(salt), &q));
  assert(!ircd_argon2id(a, sizeof(a), "x", 1, salt, strlen(salt), 0));

  printf("ok - Argon2id behaves like a password hash\n");
}

/** A stored password: what it looks like, and what it does. */
static void test_pwhash(void)
{
  char stored[PWHASH_MAX + 1];
  char other[PWHASH_MAX + 1];
  const char* salt = "sixteen bytes!!!";
  const char* pepper = "a server-wide key";

  /* Cheap costs: this is testing the wrapping, not the hash, and the
   * defaults take a fifth of a second each on purpose.
   */
  assert(ircd_pwhash_make(stored, "hunter2", 7, salt, strlen(salt), 0, 0,
                          64, 2));

  assert(0 == strncmp(stored, "$argon2id$v=19$m=64,t=2,p=1$", 28));
  assert(strlen(stored) <= PWHASH_MAX);

  assert(ircd_pwhash_verify(stored, "hunter2", 7, 0, 0));
  assert(!ircd_pwhash_verify(stored, "hunter3", 7, 0, 0));
  assert(!ircd_pwhash_verify(stored, "hunter2", 6, 0, 0));
  assert(!ircd_pwhash_verify(stored, "", 0, 0, 0));

  /* Two people with one password do not get one entry to crack. */
  assert(ircd_pwhash_make(other, "hunter2", 7, "another salt!!!!", 16, 0, 0,
                          64, 2));
  assert(strcmp(stored, other) != 0);
  assert(ircd_pwhash_verify(other, "hunter2", 7, 0, 0));

  /* The pepper has to match too: a stolen database without the server's
   * configuration is not enough to start guessing against.
   */
  assert(ircd_pwhash_make(stored, "hunter2", 7, salt, strlen(salt),
                          pepper, strlen(pepper), 64, 2));
  assert(ircd_pwhash_verify(stored, "hunter2", 7, pepper, strlen(pepper)));
  assert(!ircd_pwhash_verify(stored, "hunter2", 7, 0, 0));
  assert(!ircd_pwhash_verify(stored, "hunter2", 7, "wrong key", 9));

  printf("ok - a stored password: %.44s...\n", stored);
}

/** A stored hash this server cannot read must never match.
 *
 * The failure that matters: a parser that gave up and said yes would let
 * anybody in with any password, and it would look like it was working.
 */
static void test_pwhash_refuses_nonsense(void)
{
  static const char* nonsense[] = {
    "",
    "not a hash at all",
    "$argon2id$",
    "$argon2i$v=19$m=64,t=2,p=1$c2FsdHNhbHQ$aGFzaA",      /* wrong variant */
    "$argon2id$v=16$m=64,t=2,p=1$c2FsdHNhbHQ$aGFzaA",     /* wrong version */
    "$argon2id$v=19$m=0,t=2,p=1$c2FsdHNhbHQ$aGFzaA",      /* no cost */
    "$argon2id$v=19$m=64,t=0,p=1$c2FsdHNhbHQ$aGFzaA",
    "$argon2id$v=19$m=64,t=2,p=0$c2FsdHNhbHQ$aGFzaA",
    "$argon2id$v=19$m=64,t=2,p=1$c2hvcnQ$aGFzaA",         /* salt too short */
    "$argon2id$v=19$m=64,t=2,p=1$c2FsdHNhbHQ$short",      /* tag wrong size */
    "$argon2id$v=19$m=64,t=2,p=1$!!!notbase64!!!$aGFzaA",
    0
  };
  int i;

  for (i = 0; nonsense[i]; i++)
    assert(!ircd_pwhash_verify(nonsense[i], "hunter2", 7, 0, 0));

  assert(!ircd_pwhash_verify(0, "hunter2", 7, 0, 0));

  /* Truncating a real one must not help either. */
  {
    char stored[PWHASH_MAX + 1];
    size_t len;

    assert(ircd_pwhash_make(stored, "hunter2", 7, "sixteen bytes!!!", 16,
                            0, 0, 64, 2));
    for (len = strlen(stored); len > 0; len--) {
      stored[len - 1] = '\0';
      assert(!ircd_pwhash_verify(stored, "hunter2", 7, 0, 0));
    }
  }

  printf("ok - a hash that cannot be read never matches\n");
}

/** Costs can be raised, and old passwords keep working. */
static void test_pwhash_costs(void)
{
  char stored[PWHASH_MAX + 1];

  assert(ircd_pwhash_make(stored, "hunter2", 7, "sixteen bytes!!!", 16,
                          0, 0, 64, 2));

  /* Verifying uses the costs in the hash, not today's: raising them does
   * not lock everybody out.
   */
  assert(ircd_pwhash_verify(stored, "hunter2", 7, 0, 0));

  assert(ircd_pwhash_outdated(stored, 128, 2));
  assert(ircd_pwhash_outdated(stored, 64, 3));
  assert(!ircd_pwhash_outdated(stored, 64, 2));
  assert(!ircd_pwhash_outdated(stored, 32, 1));

  /* And something unreadable is worth replacing whatever the costs. */
  assert(ircd_pwhash_outdated("rubbish", 64, 2));

  printf("ok - costs travel with the hash and can be raised\n");
}

int main(void)
{
  test_sha256();
  test_sha256_streaming();
  test_hmac();
  test_aes_block();
  test_gcm();
  test_gcm_roundtrip();
  test_gcm_lengths();
  test_equal();
  test_blake2b();
  test_argon2id();
  test_argon2_behaviour();
  test_pwhash();
  test_pwhash_refuses_nonsense();
  test_pwhash_costs();

  printf("ok - crypto_t\n");
  return 0;
}
