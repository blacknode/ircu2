/* ircd_env_t.c - Test configuration from the environment.
 *
 * Covers what ircd_env.c promises: the placeholder forms behave like the
 * shell ones, a variable's value is never rescanned, malformed references
 * fail instead of silently expanding to nothing, and the limits hold.
 */

#include "ircd_alloc.h"
#include "ircd_env.h"
#include "ircd_log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Expand \a src and require it to succeed with \a want. */
static void expect_ok(const char* src, const char* want, unsigned int flags)
{
  struct EnvError err;
  char* got = env_expand(src, flags, &err);

  if (!got) {
    printf("FAIL: %s -> error \"%s\", wanted \"%s\"\n", src, err.text, want);
    assert(got != NULL);
  }
  if (strcmp(got, want)) {
    printf("FAIL: %s -> \"%s\", wanted \"%s\"\n", src, got, want);
    assert(0 == strcmp(got, want));
  }
  assert(err.code == ENV_OK);
  MyFree(got);
}

/** Expand \a src and require it to fail with \a code. */
static void expect_err(const char* src, enum EnvResult code,
                       unsigned int flags)
{
  struct EnvError err;
  char* got = env_expand(src, flags, &err);

  if (got) {
    printf("FAIL: %s -> \"%s\", wanted an error\n", src, got);
    assert(got == NULL);
  }
  if (err.code != code) {
    printf("FAIL: %s -> error %d (\"%s\"), wanted error %d\n", src,
           (int) err.code, err.text, (int) code);
    assert(err.code == code);
  }
  assert(err.text[0] != '\0');
}

static void test_plain(void)
{
  setenv("IRCU_T_NAME", "irc.example.net", 1);
  setenv("IRCU_T_EMPTY", "", 1);
  unsetenv("IRCU_T_UNSET");

  expect_ok("${IRCU_T_NAME}", "irc.example.net", 0);
  expect_ok("server ${IRCU_T_NAME} here", "server irc.example.net here", 0);
  expect_ok("${IRCU_T_NAME}${IRCU_T_NAME}",
            "irc.example.netirc.example.net", 0);
  expect_ok("no references at all", "no references at all", 0);
  expect_ok("", "", 0);

  /* An empty variable is set: ${X} gives "", it is not an error. */
  expect_ok("${IRCU_T_EMPTY}", "", 0);

  printf("Passed: plain substitution\n");
}

static void test_unset(void)
{
  unsetenv("IRCU_T_UNSET");

  /* Strict by default: a missing variable stops the config being read. */
  expect_err("${IRCU_T_UNSET}", ENV_ERR_UNSET, 0);
  expect_ok("${IRCU_T_UNSET}", "", ENV_LENIENT);
  expect_ok("a${IRCU_T_UNSET}b", "ab", ENV_LENIENT);

  printf("Passed: an unset variable is an error unless allowed\n");
}

static void test_defaults(void)
{
  setenv("IRCU_T_NAME", "irc.example.net", 1);
  setenv("IRCU_T_EMPTY", "", 1);
  unsetenv("IRCU_T_UNSET");

  /* ":" also treats an empty value as unset; without it, only unset is. */
  expect_ok("${IRCU_T_NAME:-fallback}", "irc.example.net", 0);
  expect_ok("${IRCU_T_EMPTY:-fallback}", "fallback", 0);
  expect_ok("${IRCU_T_EMPTY-fallback}", "", 0);
  expect_ok("${IRCU_T_UNSET:-fallback}", "fallback", 0);
  expect_ok("${IRCU_T_UNSET-fallback}", "fallback", 0);
  expect_ok("${IRCU_T_UNSET:-}", "", 0);

  expect_ok("${IRCU_T_NAME:+yes}", "yes", 0);
  expect_ok("${IRCU_T_EMPTY:+yes}", "", 0);
  expect_ok("${IRCU_T_EMPTY+yes}", "yes", 0);
  expect_ok("${IRCU_T_UNSET:+yes}", "", 0);
  expect_ok("${IRCU_T_UNSET+yes}", "", 0);

  printf("Passed: defaults and alternates\n");
}

static void test_required(void)
{
  struct EnvError err;

  setenv("IRCU_T_NAME", "irc.example.net", 1);
  setenv("IRCU_T_EMPTY", "", 1);
  unsetenv("IRCU_T_UNSET");

  expect_ok("${IRCU_T_NAME:?needed}", "irc.example.net", 0);
  expect_err("${IRCU_T_EMPTY:?needed}", ENV_ERR_UNSET, 0);
  expect_ok("${IRCU_T_EMPTY?needed}", "", 0);

  /* ENV_LENIENT does not soften an explicit requirement. */
  expect_err("${IRCU_T_UNSET:?needed}", ENV_ERR_UNSET, ENV_LENIENT);

  /* The operator's own words end up in the message, the value never does. */
  assert(NULL == env_expand("${IRCU_T_UNSET:?set this to the link password}",
                            0, &err));
  assert(NULL != strstr(err.text, "set this to the link password"));
  assert(NULL != strstr(err.text, "IRCU_T_UNSET"));

  printf("Passed: required variables\n");
}

static void test_nesting(void)
{
  setenv("IRCU_T_NAME", "irc.example.net", 1);
  unsetenv("IRCU_T_UNSET");
  unsetenv("IRCU_T_ALSO_UNSET");

  expect_ok("${IRCU_T_UNSET:-${IRCU_T_NAME}}", "irc.example.net", 0);
  expect_ok("${IRCU_T_UNSET:-${IRCU_T_ALSO_UNSET:-last}}", "last", 0);
  expect_err("${IRCU_T_UNSET:-${IRCU_T_ALSO_UNSET}}", ENV_ERR_UNSET, 0);

  /* Nine levels: one more than ENV_MAX_DEPTH. */
  expect_err("${IRCU_T_UNSET:-${IRCU_T_UNSET:-${IRCU_T_UNSET:-"
             "${IRCU_T_UNSET:-${IRCU_T_UNSET:-${IRCU_T_UNSET:-"
             "${IRCU_T_UNSET:-${IRCU_T_UNSET:-${IRCU_T_UNSET:-x"
             "}}}}}}}}}", ENV_ERR_DEPTH, 0);

  printf("Passed: nested defaults\n");
}

static void test_values_are_not_rescanned(void)
{
  /* The rule that keeps the environment from smuggling references: what a
   * variable holds is copied in and never looked at again.
   */
  setenv("IRCU_T_SNEAKY", "${IRCU_T_NAME}", 1);
  setenv("IRCU_T_NAME", "irc.example.net", 1);

  expect_ok("${IRCU_T_SNEAKY}", "${IRCU_T_NAME}", 0);

  printf("Passed: values are not rescanned\n");
}

static void test_literals(void)
{
  setenv("IRCU_T_NAME", "irc.example.net", 1);

  expect_ok("$$", "$", 0);
  expect_ok("$${IRCU_T_NAME}", "${IRCU_T_NAME}", 0);
  expect_ok("costs $5", "costs $5", 0);
  expect_ok("trailing $", "trailing $", 0);
  expect_ok("a$b", "a$b", 0);
  expect_ok("}{", "}{", 0);

  printf("Passed: literal dollars and braces\n");
}

static void test_syntax_errors(void)
{
  expect_err("${", ENV_ERR_SYNTAX, 0);
  expect_err("${IRCU_T_NAME", ENV_ERR_SYNTAX, 0);
  expect_err("${IRCU_T_NAME:-unclosed", ENV_ERR_SYNTAX, 0);
  expect_err("${IRCU_T_NAME:*x}", ENV_ERR_SYNTAX, 0);
  expect_err("${IRCU_T_NAME!}", ENV_ERR_SYNTAX, 0);
  expect_err("${}", ENV_ERR_NAME, 0);
  expect_err("${1ABC}", ENV_ERR_NAME, 0);
  expect_err("${A B}", ENV_ERR_SYNTAX, 0);

  /* Still errors when unset variables are allowed: these are not variables. */
  expect_err("${}", ENV_ERR_NAME, ENV_LENIENT);

  printf("Passed: malformed references are errors\n");
}

static void test_limits(void)
{
  char big[4096];
  char name[ENV_NAME_MAX + 32];
  char ref[ENV_NAME_MAX + 64];

  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  setenv("IRCU_T_BIG", big, 1);

  /* Two of those fit; three do not. */
  expect_err("${IRCU_T_BIG}${IRCU_T_BIG}${IRCU_T_BIG}", ENV_ERR_TOOLONG, 0);

  memset(name, 'A', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  sprintf(ref, "${%s}", name);
  expect_err(ref, ENV_ERR_NAME, 0);

  printf("Passed: length limits\n");
}

static void test_expand_buf(void)
{
  struct EnvError err;
  char dst[32];

  setenv("IRCU_T_NAME", "irc.example.net", 1);

  assert(15 == env_expand_buf(dst, sizeof(dst), "${IRCU_T_NAME}", 0, &err));
  assert(0 == strcmp(dst, "irc.example.net"));

  assert(-1 == env_expand_buf(dst, 8, "${IRCU_T_NAME}", 0, &err));
  assert(err.code == ENV_ERR_TOOLONG);
  assert(dst[0] == '\0');

  printf("Passed: expansion into a fixed buffer\n");
}

static void test_expand_number(void)
{
  struct EnvError err;
  int num;

  setenv("IRCU_T_NUM", "4400", 1);
  setenv("IRCU_T_NAME", "irc.example.net", 1);
  unsetenv("IRCU_T_UNSET");

  num = 0;
  assert(env_expand_number("${IRCU_T_NUM}", 0, &num, &err));
  assert(num == 4400);

  num = 0;
  assert(env_expand_number("${IRCU_T_UNSET:-6667}", 0, &num, &err));
  assert(num == 6667);

  /* A value that is not a number must not become some other token. */
  assert(!env_expand_number("${IRCU_T_NAME}", 0, &num, &err));
  assert(err.code == ENV_ERR_NOTNUM);
  assert(!env_expand_number("${IRCU_T_UNSET}", 0, &num, &err));
  assert(err.code == ENV_ERR_UNSET);

  printf("Passed: numeric references\n");
}

static void test_accessors(void)
{
  setenv("IRCU_T_NAME", "irc.example.net", 1);
  setenv("IRCU_T_EMPTY", "", 1);
  setenv("IRCU_T_NUM", "4400", 1);
  setenv("IRCU_T_YES", "Yes", 1);
  setenv("IRCU_T_NO", "off", 1);
  setenv("IRCU_T_JUNK", "banana", 1);
  unsetenv("IRCU_T_UNSET");

  assert(NULL != env_get("IRCU_T_NAME"));
  assert(NULL == env_get("IRCU_T_UNSET"));
  assert(0 == strcmp(env_get("IRCU_T_EMPTY"), ""));

  assert(0 == strcmp(env_str("IRCU_T_NAME", "def"), "irc.example.net"));
  assert(0 == strcmp(env_str("IRCU_T_EMPTY", "def"), "def"));
  assert(0 == strcmp(env_str("IRCU_T_UNSET", "def"), "def"));

  assert(4400 == env_int("IRCU_T_NUM", 1, 1, 65535));
  assert(1 == env_int("IRCU_T_NUM", 1, 1, 4399));   /* out of range */
  assert(7 == env_int("IRCU_T_JUNK", 7, 0, 100));   /* not a number */
  assert(7 == env_int("IRCU_T_UNSET", 7, 0, 100));
  assert(7 == env_int("IRCU_T_EMPTY", 7, 0, 100));

  assert(1 == env_bool("IRCU_T_YES", 0));
  assert(0 == env_bool("IRCU_T_NO", 1));
  assert(1 == env_bool("IRCU_T_JUNK", 1));          /* not a boolean */
  assert(0 == env_bool("IRCU_T_UNSET", 0));

  assert(env_has_ref("${A}"));
  assert(env_has_ref("$"));
  assert(!env_has_ref("plain"));

  printf("Passed: typed accessors\n");
}

int main(int argc, char* argv[])
{
  (void) argc;
  (void) argv;

  test_plain();
  test_unset();
  test_defaults();
  test_required();
  test_nesting();
  test_values_are_not_rescanned();
  test_literals();
  test_syntax_errors();
  test_limits();
  test_expand_buf();
  test_expand_number();
  test_accessors();

  printf("Done.\n");
  return 0;
}
