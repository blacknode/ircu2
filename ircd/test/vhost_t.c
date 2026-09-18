/* vhost_t.c - Test file for virtual host generation */

#include "ircd_log.h"
#include "ircd_string.h"
#include "ircd_vhost.h"
#include "res.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Every expected host below was produced by IRC-Hispano's ipvirtual tool
 * (ircd/crypt/tools/ipvirtual.c, branch u2_10_H_10_L) fed the same key and
 * address, with ".virtual" read as ".v4".  The 127.0.0.1 entry is one the
 * tool had to retry (its first output carried a bracket), so it also pins
 * the retry counter's behaviour.
 */
struct vhost_test {
  const char* key;
  const char* ip;
  const char* expected;
};

static const struct vhost_test vectors[] = {
  { "AbCdEfGhIjKl", "127.0.0.1",              "BfmWwf.DHt5v6.v4" },
  { "AbCdEfGhIjKl", "192.168.1.10",           "C81sBB.AItioL.v4" },
  { "AbCdEfGhIjKl", "10.0.0.1",               "DMlC1Z.Ck1Rft.v4" },
  { "AbCdEfGhIjKl", "8.8.8.8",                "BlMCxj.DPlkbR.v4" },
  { "AbCdEfGhIjKl", "1.2.3.4",                "BbVudu.AS71C1.v4" },
  { "AbCdEfGhIjKl", "200.100.50.25",          "AW5TKX.BYjnCL.v4" },
  { "AbCdEfGhIjKl", "::1",                    "CvsjqN.CHduq1.v6" },
  { "AbCdEfGhIjKl", "2001:db8::1",            "BQCwtO.CncH0Z.v6" },
  { "AbCdEfGhIjKl", "fe80::1",                "DP9nEI.DiMq3F.v6" },
  { "AbCdEfGhIjKl", "2a02:1234:5678:9abc::1", "DVtpej.CaL20W.v6" },
  { "[]09azAZ[]09", "127.0.0.1",              "AQ5y68.B4a1CE.v4" },
  { "[]09azAZ[]09", "::1",                    "BpKjRa.DOifQJ.v6" },
  { 0, 0, 0 }
};

static int failures;

static void check(int cond, const char* what)
{
  if (!cond) {
    fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

static void parse(const char* text, struct irc_in_addr* ip)
{
  unsigned char bits;

  if (!ipmask_parse(text, ip, &bits)) {
    fprintf(stderr, "cannot parse %s\n", text);
    exit(1);
  }
}

static void test_key_validation(void)
{
  check(vhost_key_valid("AbCdEfGhIjKl"), "12 letters are a valid key");
  check(vhost_key_valid("[]09azAZ[]09"), "brackets and digits are valid");
  check(!vhost_key_valid(""), "empty key rejected");
  check(!vhost_key_valid("AbCdEfGhIjK"), "11 characters rejected");
  check(!vhost_key_valid("AbCdEfGhIjKlM"), "13 characters rejected");
  check(!vhost_key_valid("AbCdEfGhIjK-"), "'-' is not base64");
  check(!vhost_key_valid("AbCdEf GhIjK"), "space is not base64");
  check(!vhost_key_valid(NULL), "NULL rejected");

  check(!vhost_set_key("bad"), "vhost_set_key refuses a bad key");
  check(!vhost_have_key(), "no key loaded after a refused one");
}

static void test_no_key(void)
{
  struct irc_in_addr ip;
  char buf[VHOST_MAX_LEN + 1];

  parse("1.2.3.4", &ip);
  memset(buf, 'x', sizeof(buf));
  check(!vhost_make(buf, sizeof(buf), &ip), "vhost_make fails without a key");
  check(buf[0] == '\0', "buffer emptied when there is no key");
}

static void test_vectors(void)
{
  const struct vhost_test* t;
  struct irc_in_addr ip;
  char buf[VHOST_MAX_LEN + 1];
  char msg[128];

  for (t = vectors; t->key; ++t) {
    check(vhost_set_key(t->key), "vhost_set_key accepts vector key");
    check(vhost_have_key(), "key reported as loaded");
    check(!strcmp(vhost_key(), t->key), "vhost_key() returns the key");
    parse(t->ip, &ip);
    check(vhost_make(buf, sizeof(buf), &ip), "vhost_make succeeds");
    snprintf(msg, sizeof(msg), "%s %s -> %s (got %s)", t->key, t->ip,
             t->expected, buf);
    check(!strcmp(buf, t->expected), msg);
    check(strlen(buf) <= VHOST_MAX_LEN, "host fits VHOST_MAX_LEN");
  }
}

static void test_ipv6_retry(void)
{
  /* The reference tool prints "A1Vdp].BgknuH.v6" for this pair: a bracket,
   * which no hostname may carry.  We retry instead.
   */
  struct irc_in_addr ip;
  char buf[VHOST_MAX_LEN + 1], again[VHOST_MAX_LEN + 1];

  check(vhost_set_key("[]09azAZ[]09"), "key loads");
  parse("2001:db8::1", &ip);
  check(vhost_make(buf, sizeof(buf), &ip), "vhost_make succeeds on retry");
  check(strcmp(buf, "A1Vdp].BgknuH.v6") != 0, "bracketed host not returned");
  check(!strchr(buf, '[') && !strchr(buf, ']'), "retry output has no brackets");
  /* Pinned so that the retry rule (IV = attempt number) cannot drift:
   * tests/vhost.py, the Python port the integration tests predict hosts
   * with, must agree with this.
   */
  check(!strcmp(buf, "DCbNB3.DrsmBh.v6"), "retry output is the IV=1 block");
  check(!strcmp(buf + 13, ".v6"), "retry output keeps the .v6 suffix");
  check(vhost_make(again, sizeof(again), &ip), "second call succeeds");
  check(!strcmp(buf, again), "retry is deterministic");
}

static void test_shape(void)
{
  struct irc_in_addr ip;
  char buf[VHOST_MAX_LEN + 1];
  char small[VHOST_MAX_LEN];
  size_t i;

  check(vhost_set_key("AbCdEfGhIjKl"), "key loads");

  parse("203.0.113.7", &ip);
  check(vhost_make(buf, sizeof(buf), &ip), "v4 host made");
  check(strlen(buf) == VHOST_MAX_LEN, "v4 host is exactly VHOST_MAX_LEN");
  check(buf[6] == '.' && buf[13] == '.', "dots where expected");
  check(!strcmp(buf + 13, ".v4"), "IPv4 gets .v4");
  for (i = 0; i < 13; ++i)
    if (i != 6)
      check(IsAlnum(buf[i]), "body is alphanumeric");

  parse("2001:db8:1234:5678::9", &ip);
  check(vhost_make(buf, sizeof(buf), &ip), "v6 host made");
  check(!strcmp(buf + 13, ".v6"), "IPv6 gets .v6");

  /* Only the /64 prefix goes in, so two hosts on one prefix hide alike. */
  {
    char other[VHOST_MAX_LEN + 1];
    parse("2001:db8:1234:5678:abcd::ff", &ip);
    check(vhost_make(other, sizeof(other), &ip), "second v6 host made");
    check(!strcmp(buf, other), "same /64 prefix gives the same host");
    parse("2001:db8:1234:5679::9", &ip);
    check(vhost_make(other, sizeof(other), &ip), "third v6 host made");
    check(strcmp(buf, other) != 0, "different prefix gives a different host");
  }

  check(!vhost_make(small, sizeof(small), &ip), "short buffer refused");
  check(small[0] == '\0', "short buffer emptied");

  /* ::ffff:1.2.3.4 and 1.2.3.4 are the same address to the server. */
  {
    char a[VHOST_MAX_LEN + 1], b[VHOST_MAX_LEN + 1];
    parse("1.2.3.4", &ip);
    vhost_make(a, sizeof(a), &ip);
    parse("::ffff:1.2.3.4", &ip);
    vhost_make(b, sizeof(b), &ip);
    check(!strcmp(a, b), "v4-mapped address hides like plain v4");
  }
}

static void test_conf_glue(void)
{
  vhost_conf_unmark();
  check(vhost_conf_set_key("AbCdEfGhIjKl"), "conf accepts a good key");
  check(vhost_conf_sweep(), "sweep installs it");
  check(!strcmp(vhost_key(), "AbCdEfGhIjKl"), "installed key is the offered one");

  /* A pass that names a bad key keeps the old one. */
  vhost_conf_unmark();
  check(!vhost_conf_set_key("nope"), "conf rejects a bad key");
  check(vhost_conf_sweep(), "a key is still in force");
  check(!strcmp(vhost_key(), "AbCdEfGhIjKl"), "old key survives a bad pass");

  /* A pass with no block at all keeps the old one too. */
  vhost_conf_unmark();
  check(vhost_conf_sweep(), "a key is still in force after an empty pass");

  /* A new good key replaces it. */
  vhost_conf_unmark();
  check(vhost_conf_set_key("[]09azAZ[]09"), "conf accepts another key");
  check(vhost_conf_sweep(), "sweep installs the new key");
  check(!strcmp(vhost_key(), "[]09azAZ[]09"), "new key in force");
}

int main(int argc, char* argv[])
{
  test_key_validation();
  test_no_key();
  test_vectors();
  test_ipv6_retry();
  test_shape();
  test_conf_glue();

  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  printf("vhost_t: all checks passed\n");
  return 0;
}
