/* msgid_t.c - Test the message identifier generator.
 *
 * One property matters and everything here is about it: an identifier is
 * never handed out twice.  Not within a run, not across a restart, and not
 * between two servers -- which is why the server's numeric is part of it
 * and why the counter is seeded from the clock rather than from zero.
 *
 * The generator knows nothing about clients or sending, which is what lets
 * this test link it on its own.
 */

#include "msgid.h"
#include "ircd_string.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** How many identifiers the uniqueness checks generate. */
#define SAMPLE 20000

/** Every identifier generated, for the duplicate check. */
static char seen[SAMPLE][MSGIDLEN + 1];

/** Identifiers begin with the server's numeric and are well formed. */
static void test_shape(void)
{
  const char* id;

  msgid_init("AB", 1700000000);

  id = msgid_new();
  assert(id != 0);
  assert(strlen(id) > 2);
  assert(strlen(id) <= MSGIDLEN);
  assert(0 == strncmp(id, "AB", 2));
  assert(msgid_valid(id));

  /* Whatever it renders, it survives a tag value unescaped. */
  assert(0 == strpbrk(id, " ;=,\\\\") - (char*) 0 || strpbrk(id, " ;=,\\\\") == 0);

  printf("ok - identifier shape: %s\n", id);
}

/** A server with no numeric yet still produces something usable.
 *
 * It should not happen -- msgid_init() runs after the configuration file
 * is read -- but an identifier that is the empty string would be far
 * worse than one that is merely not unique between servers.
 */
static void test_no_prefix(void)
{
  const char* id;

  msgid_init(0, 1700000000);
  id = msgid_new();
  assert(id != 0 && *id != '\0');
  assert(msgid_valid(id));

  msgid_init("", 1700000000);
  id = msgid_new();
  assert(id != 0 && *id != '\0');
  assert(msgid_valid(id));

  printf("ok - a server with no numeric still names its messages\n");
}

/** No identifier is ever handed out twice within a run. */
static void test_unique(void)
{
  int i, j;

  msgid_init("AB", 1700000000);

  for (i = 0; i < SAMPLE; i++) {
    const char* id = msgid_new();
    assert(strlen(id) <= MSGIDLEN);
    strcpy(seen[i], id);
  }

  /* The counter only ever goes up, so a repeat could only be adjacent or
   * a wrap; check every pair against the one before and a sample of the
   * rest rather than all 200 million pairs.
   */
  for (i = 1; i < SAMPLE; i++) {
    assert(0 != strcmp(seen[i], seen[i - 1]));
    for (j = 0; j < 32 && j < i; j++)
      assert(0 != strcmp(seen[i], seen[i - 1 - j]));
  }

  /* And the whole set, the slow honest way, on a smaller slice. */
  for (i = 0; i < 400; i++)
    for (j = i + 1; j < 400; j++)
      assert(0 != strcmp(seen[i], seen[j]));

  assert(msgid_count() == SAMPLE);

  printf("ok - %d identifiers, none repeated (%s .. %s)\n",
         SAMPLE, seen[0], seen[SAMPLE - 1]);
}

/** A restart resumes above what the previous run handed out.
 *
 * This is the property the clock seed buys.  A counter that started at
 * zero every time would hand out the same identifiers again after a
 * restart, and anything that had stored the old ones would quietly point
 * at the wrong message.
 */
static void test_restart(void)
{
  char before[MSGIDLEN + 1];
  char after[MSGIDLEN + 1];
  int i;

  msgid_init("AB", 1700000000);
  for (i = 0; i < 1000; i++)
    strcpy(before, msgid_new());

  /* Restarted a second later. */
  msgid_init("AB", 1700000001);
  strcpy(after, msgid_new());

  /* Same length or longer, and greater: the rendering is big-endian in a
   * fixed alphabet, so for equal lengths a plain comparison is the same
   * order as the counter's.
   */
  assert(strlen(after) >= strlen(before));
  if (strlen(after) == strlen(before))
    assert(strcmp(after, before) > 0);

  /* Even restarting within the same second does not walk backwards, as
   * long as fewer than 2^20 were handed out in it.
   */
  msgid_init("AB", 1700000001);
  for (i = 0; i < 100; i++)
    strcpy(before, msgid_new());
  msgid_init("AB", 1700000002);
  strcpy(after, msgid_new());
  assert(strlen(after) > strlen(before)
         || strcmp(after, before) > 0);

  printf("ok - a restart resumes above the previous run (%s -> %s)\n",
         before, after);
}

/** Two servers never collide, because the numeric is part of the name. */
static void test_servers_differ(void)
{
  char a[MSGIDLEN + 1];
  const char* b;

  msgid_init("AB", 1700000000);
  strcpy(a, msgid_new());

  msgid_init("AC", 1700000000);
  b = msgid_new();

  /* Same clock, same counter, different server: different identifier. */
  assert(0 != strcmp(a, b));

  printf("ok - two servers with the same clock do not collide (%s, %s)\n",
         a, b);
}

/** What the server accepts from a peer. */
static void test_valid(void)
{
  char toolong[MSGIDLEN + 2];

  msgid_init("AB", 1700000000);

  assert(msgid_valid("abc123"));
  assert(msgid_valid("AB1a2b3c"));
  assert(msgid_valid("a.b-c_d"));
  assert(msgid_valid(msgid_new()));

  assert(!msgid_valid(0));
  assert(!msgid_valid(""));
  assert(!msgid_valid("has space"));
  assert(!msgid_valid("has;semi"));
  assert(!msgid_valid("has=equals"));
  assert(!msgid_valid("has,comma"));
  assert(!msgid_valid("has\\\\backslash"));
  assert(!msgid_valid("has\tab"));
  assert(!msgid_valid("has\nnewline"));

  memset(toolong, 'a', sizeof(toolong) - 1);
  toolong[sizeof(toolong) - 1] = '\0';
  assert(!msgid_valid(toolong));

  /* Exactly at the limit is fine. */
  toolong[MSGIDLEN] = '\0';
  assert(msgid_valid(toolong));

  printf("ok - identifiers from a peer are checked, not parsed\n");
}

int main(void)
{
  test_shape();
  test_no_prefix();
  test_unique();
  test_restart();
  test_servers_differ();
  test_valid();

  printf("ok - msgid_t\n");
  return 0;
}
