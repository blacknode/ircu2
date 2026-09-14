/* capab_t.c - Test the run-time client capability register.
 *
 * Covers what capab.c promises to the modules that register capabilities:
 * the core's are seeded once and land on exactly the positions enum Capab
 * names for them, a module is handed a free position and never one that is
 * taken, names are validated the way IRCv3 spells them, only a module's
 * own capability can be removed by that module, removing one announces it,
 * and unloading a module takes every capability it registered with it.
 *
 * The register is the half of the capability code that knows nothing about
 * clients, which is what lets it be tested without the server: the half
 * that does -- CAP LS, CAP REQ, CAP NEW, CAP DEL -- is in m_cap.c, and
 * capab_stub.c stands in for it.
 */

#include "capab.h"
#include "client.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Defined by capab_stub.c. */
extern int stub_cap_new;
extern int stub_cap_del;
extern int stub_cap_new_calls;
extern int stub_cap_del_calls;
extern enum Feature stub_feature_off;

/** Two module handles.  The register only ever compares these pointers,
 * never dereferences them, so a distinct address is a whole module as far
 * as this test is concerned.
 */
static struct ModuleHandle* const mod_a = (struct ModuleHandle*) 0x1;
static struct ModuleHandle* const mod_b = (struct ModuleHandle*) 0x2;

/** Reset what capab_stub.c recorded. */
static void reset_stub(void)
{
  stub_cap_new = CAP_NONE;
  stub_cap_del = CAP_NONE;
  stub_cap_new_calls = 0;
  stub_cap_del_calls = 0;
}

/** Render the registered names in list order, space separated. */
static const char* cap_names(void)
{
  static char buf[2048];
  const struct Capability* cap;
  size_t len = 0;

  buf[0] = '\0';
  for (cap = cap_first(); cap; cap = cap->cap_next)
    len += (size_t) snprintf(buf + len, sizeof(buf) - len, "%s%s",
                             len ? " " : "", cap->cap_name);

  return buf;
}

/* ------------------------------------------------------------------ */

/** The core's capabilities are seeded on the positions enum Capab names.
 *
 * This is the property the rest of the tree depends on: every
 * CapHas(cli_active(x), CAP_ECHOMESSAGE) in the server is testing a bit
 * position, and if the seeding ever put echo-message somewhere else, all
 * of them would be testing the wrong one silently.
 */
static void test_core_seeding(void)
{
  const struct Capability* cap;

  assert(cap_count() == CAP_LAST_CORE_CAP);

  cap = cap_find("echo-message");
  assert(cap != NULL);
  assert(cap->cap_index == CAP_ECHOMESSAGE);
  assert(cap->cap_owner == NULL);

  cap = cap_find("away-notify");
  assert(cap != NULL && cap->cap_index == CAP_AWAYNOTIFY);

  cap = cap_find("draft/languages");
  assert(cap != NULL && cap->cap_index == CAP_LANGUAGES);

  cap = cap_find("cap-notify");
  assert(cap != NULL && cap->cap_index == CAP_CAPNOTIFY);
  assert(cap->cap_flags & CAPFL_STICKY_302);

  /* Both directions of the mapping agree. */
  assert(cap_find_index(CAP_ECHOMESSAGE) == cap_find("echo-message"));
  assert(cap_find_index(CAP_NONE) == NULL);
  assert(cap_find_index(CAP_MAX) == NULL);

  /* Lookup is case-insensitive, the way CAP REQ is. */
  assert(cap_find("ECHO-MESSAGE") == cap_find("echo-message"));
  assert(cap_find("nonesuch") == NULL);
  assert(cap_find("") == NULL);
  assert(cap_find(NULL) == NULL);

  printf("ok - core capabilities seeded on their own positions (%u)\n",
         cap_count());
}

/** The list comes out sorted by name, whenever a module was loaded. */
static void test_sorted(void)
{
  const struct Capability* cap;
  const char* prev = NULL;

  for (cap = cap_first(); cap; cap = cap->cap_next) {
    if (prev)
      assert(strcmp(prev, cap->cap_name) < 0);
    prev = cap->cap_name;
  }

  printf("ok - register sorted by name: %s\n", cap_names());
}

/** Name validation, as IRCv3 spells capability names. */
static void test_names(void)
{
  char toolong[CAPNAMELEN + 3];

  assert(cap_name_valid("react"));
  assert(cap_name_valid("draft/react"));
  assert(cap_name_valid("example.org/some-cap"));
  assert(cap_name_valid("a_b.c-d"));
  assert(cap_name_valid("sasl3"));

  assert(!cap_name_valid(NULL));
  assert(!cap_name_valid(""));
  assert(!cap_name_valid("/leading"));
  assert(!cap_name_valid("trailing/"));
  assert(!cap_name_valid("two/slashes/here"));
  assert(!cap_name_valid("has space"));
  assert(!cap_name_valid("has=equals"));
  assert(!cap_name_valid("has,comma"));

  memset(toolong, 'a', sizeof(toolong) - 1);
  toolong[sizeof(toolong) - 1] = '\0';
  assert(!cap_name_valid(toolong));

  printf("ok - capability names validated\n");
}

/** A module gets a free position, keeps it, and is announced. */
static void test_module_register(void)
{
  const struct Capability* cap;
  int first = CAP_NONE;
  int second = CAP_NONE;

  reset_stub();

  assert(cap_register(mod_a, "draft/react", 0, 0, &first));
  assert(first >= CAP_LAST_CORE_CAP);   /* never on one of the core's */
  assert(first < CAP_MAX);
  assert(cap_count() == CAP_LAST_CORE_CAP + 1);

  /* Registering on a running server announces the capability. */
  assert(stub_cap_new_calls == 1);
  assert(stub_cap_new == first);

  cap = cap_find("draft/react");
  assert(cap != NULL);
  assert(cap->cap_index == first);
  assert(cap->cap_owner == mod_a);
  assert(cap->cap_config == 0);
  assert(cap->cap_value[0] == '\0');
  assert(cap_module_count(mod_a) == 1);
  assert(cap_module_count(mod_b) == 0);

  /* The same name twice is refused, whoever asks. */
  assert(!cap_register(mod_a, "draft/react", 0, 0, &second));
  assert(second == CAP_NONE);
  assert(!cap_register(mod_b, "DRAFT/REACT", 0, 0, &second));
  assert(second == CAP_NONE);
  /* So is one of the core's. */
  assert(!cap_register(mod_b, "echo-message", 0, 0, &second));
  assert(second == CAP_NONE);
  /* And a malformed one. */
  assert(!cap_register(mod_b, "not a name", 0, 0, &second));
  assert(second == CAP_NONE);
  assert(cap_count() == CAP_LAST_CORE_CAP + 1);

  /* A second module gets a different position. */
  assert(cap_register(mod_b, "draft/reply", 0, 0, &second));
  assert(second != first);
  assert(cap_module_count(mod_b) == 1);

  printf("ok - module capabilities registered at %d and %d\n", first, second);
}

/** Values and availability, and what each does to the announcements. */
static void test_value_and_availability(void)
{
  const struct Capability* cap = cap_find("draft/react");
  int index = cap->cap_index;

  cap_set_value(index, "a,b,c");
  assert(!strcmp(cap->cap_value, "a,b,c"));
  cap_set_value(index, "");
  assert(cap->cap_value[0] == '\0');
  cap_set_value(CAP_NONE, "ignored");   /* must not crash */

  assert(cap_is_available(cap));
  assert(!cap_is_available(NULL));

  reset_stub();

  /* Taking it out of service announces it once, and again on the way
   * back; asking for the state it is already in announces nothing. */
  cap_update_availability(index, 0);
  assert(stub_cap_del_calls == 1 && stub_cap_del == index);
  assert(!cap_is_available(cap));

  cap_update_availability(index, 0);
  assert(stub_cap_del_calls == 1);

  cap_update_availability(index, 1);
  assert(stub_cap_new_calls == 1 && stub_cap_new == index);
  assert(cap_is_available(cap));

  /* A capability gated by a feature is unavailable while it is off. */
  stub_feature_off = FEAT_CAP_ECHOMESSAGE;
  assert(!cap_is_available(cap_find("echo-message")));
  assert(cap_is_available(cap_find("cap-notify")));  /* gated by nothing */
  stub_feature_off = FEAT_LAST_F;
  assert(cap_is_available(cap_find("echo-message")));

  printf("ok - capability value and availability\n");
}

/** Only a module's own capability can be removed, and by it alone. */
static void test_unregister(void)
{
  int index = cap_find("draft/react")->cap_index;

  reset_stub();

  /* Not another module's, and not one of the core's. */
  assert(!cap_unregister(mod_b, "draft/react"));
  assert(!cap_unregister(mod_a, "echo-message"));
  assert(!cap_unregister(mod_a, "nonesuch"));
  assert(stub_cap_del_calls == 0);

  assert(cap_unregister(mod_a, "draft/react"));
  assert(stub_cap_del_calls == 1 && stub_cap_del == index);
  assert(cap_find("draft/react") == NULL);
  assert(cap_find_index(index) == NULL);
  assert(cap_module_count(mod_a) == 0);

  /* The position it held is the next one handed out. */
  {
    int again = CAP_NONE;
    assert(cap_register(mod_a, "draft/typing", 0, 0, &again));
    assert(again == index);
    assert(cap_unregister(mod_a, "draft/typing"));
  }

  printf("ok - only a module's own capability can be removed\n");
}

/** Unloading a module takes every capability it registered with it. */
static void test_drop_module(void)
{
  assert(cap_register(mod_a, "draft/one", 0, 0, NULL));
  assert(cap_register(mod_a, "draft/two", 0, 0, NULL));
  assert(cap_module_count(mod_a) == 2);
  assert(cap_module_count(mod_b) == 1);   /* draft/reply, from earlier */

  reset_stub();
  cap_drop_module(mod_a);

  assert(stub_cap_del_calls == 2);
  assert(cap_module_count(mod_a) == 0);
  assert(cap_find("draft/one") == NULL);
  assert(cap_find("draft/two") == NULL);

  /* The other module is untouched. */
  assert(cap_module_count(mod_b) == 1);
  assert(cap_find("draft/reply") != NULL);

  cap_drop_module(mod_b);
  assert(cap_count() == CAP_LAST_CORE_CAP);

  printf("ok - unloading a module drops its capabilities\n");
}

/** The register fills up rather than handing out a position twice. */
static void test_exhaustion(void)
{
  char name[32];
  int index = CAP_NONE;
  int i;
  int registered = 0;

  for (i = 0; i < CAP_MAX; i++) {
    snprintf(name, sizeof(name), "draft/fill-%d", i);
    if (!cap_register(mod_a, name, 0, 0, &index))
      break;
    assert(index >= CAP_LAST_CORE_CAP && index < CAP_MAX);
    registered++;
  }

  assert(registered == CAP_MAX - CAP_LAST_CORE_CAP);
  assert(cap_count() == CAP_MAX);

  /* One more is refused, and says so rather than overwriting anything. */
  index = 0;
  assert(!cap_register(mod_a, "draft/one-too-many", 0, 0, &index));
  assert(index == CAP_NONE);
  assert(cap_count() == CAP_MAX);

  cap_drop_module(mod_a);
  assert(cap_count() == CAP_LAST_CORE_CAP);

  printf("ok - register fills to %d and refuses the next\n", CAP_MAX);
}

/** The sets themselves: a bitset wide enough for every position. */
static void test_sets(void)
{
  capset_t cs;

  CapClrAll(&cs);
  assert(cap_set_empty(&cs));

  CapSet(&cs, CAP_ECHOMESSAGE);
  assert(!cap_set_empty(&cs));
  assert(CapHas(&cs, CAP_ECHOMESSAGE));
  assert(!CapHas(&cs, CAP_AWAYNOTIFY));

  /* The last position is reachable: a set that silently dropped the high
   * bits would leave a module's capability permanently off.
   */
  CapSet(&cs, CAP_MAX - 1);
  assert(CapHas(&cs, CAP_MAX - 1));
  assert(CapHas(&cs, CAP_ECHOMESSAGE));

  CapClr(&cs, CAP_ECHOMESSAGE);
  assert(!CapHas(&cs, CAP_ECHOMESSAGE));
  assert(CapHas(&cs, CAP_MAX - 1));

  CapClrAll(&cs);
  assert(cap_set_empty(&cs));

  printf("ok - capability sets hold all %d positions\n", CAP_MAX);
}

int main(void)
{
  cap_init();

  test_core_seeding();
  test_names();
  test_module_register();
  test_sorted();
  test_value_and_availability();
  test_unregister();
  test_drop_module();
  test_exhaustion();
  test_sets();

  cap_close();
  assert(cap_count() == 0);
  assert(cap_first() == NULL);

  printf("ok - capab_t\n");
  return 0;
}
