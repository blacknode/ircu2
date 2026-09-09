/* module_t.c - Test the module loader.
 *
 * Exercises the acceptance criteria for the loader: a well-formed module
 * loads and unloads, a malformed one is rejected without leaving anything
 * behind, and repeated load/unload cycles neither leak nor corrupt the
 * module list.
 *
 * The test modules live in modules/ next to this file and are built as
 * MODULE libraries by CMake.  module.c is compiled with IRCU_MODULE_DIR
 * pointing at that build directory, so the loader resolves the fixtures by
 * name just as the server resolves real modules against MOD_PATH.
 */

#include "module.h"
#include "ircd_log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Touched by mod_good so the test can prove the module ran and that it
 * resolved a symbol out of this binary's dynamic symbol table.
 */
int module_test_marker;

/* Defined by module_stub.c. */
extern int stub_commands_live;
extern int stub_commands_added;


/** A well-formed module loads, runs mi_init, and is findable. */
static void test_load_good(void)
{
  struct ModuleHandle* mod;
  const char* err = "unset";

  module_test_marker = 0;

  mod = module_load("mod_good", 0, &err);
  assert(mod != 0);
  assert(err == 0);

  /* mi_init ran, and it reached a symbol in this executable. */
  assert(module_test_marker == 1);

  assert(module_count() == 1);
  assert(module_find("mod_good") == mod);
  assert(0 == strcmp(module_name(mod), "mod_good"));

  /* Loaded by name; the name is what a later load or unload names, and the
   * path the loader built from it ends in that name.
   */
  assert(module_find_file("mod_good") == mod);
  assert(0 == strcmp(module_file(mod), "mod_good"));
  assert(0 != strstr(module_path(mod), "/mod_good.so"));

  /* Name lookup is case-insensitive, like the rest of ircu. */
  assert(module_find("MOD_GOOD") == mod);

  /* mi_rehash is optional and only fires when asked. */
  module_rehash_notify(mod);
  assert(module_test_marker == 101);

  assert(module_unload(mod) != 0);
  assert(module_test_marker == 111);   /* mi_fini ran */
  assert(module_count() == 0);
  assert(module_find("mod_good") == 0);

  printf("Passed: load/unload of a well-formed module\n");
}

/** A module built against another ABI is refused. */
static void test_reject_bad_abi(void)
{
  const char* err = 0;

  assert(module_load("mod_badabi", 0, &err) == 0);
  assert(err != 0);
  assert(strstr(err, "ABI") != 0);
  assert(module_count() == 0);

  printf("Passed: ABI mismatch rejected\n");
}

/** A shared object without the ircu_module symbol is refused. */
static void test_reject_no_symbol(void)
{
  const char* err = 0;

  assert(module_load("mod_nosym", 0, &err) == 0);
  assert(err != 0);
  assert(module_count() == 0);

  printf("Passed: missing ircu_module symbol rejected\n");
}

/** A module whose mi_init fails does not stay loaded. */
static void test_reject_failed_init(void)
{
  const char* err = 0;

  assert(module_load("mod_failinit", 0, &err) == 0);
  assert(err != 0);
  assert(module_count() == 0);
  assert(module_find("mod_failinit") == 0);

  printf("Passed: failed mi_init unwinds the load\n");
}

/** A path that is not a shared object at all is refused. */
static void test_reject_missing_file(void)
{
  const char* err = 0;

  assert(module_load("no_such_module_here", 0, &err) == 0);
  assert(err != 0);
  assert(module_count() == 0);

  printf("Passed: missing file rejected\n");
}

/** The nick that loaded a module is recorded, and outlives the client. */
static void test_loaded_by(void)
{
  struct ModuleHandle* mod;
  char nick[16];

  strcpy(nick, "SomeOper");

  mod = module_load("mod_good", nick, 0);
  assert(mod != 0);
  assert(module_loaded_by(mod) != 0);
  assert(0 == strcmp(module_loaded_by(mod), "SomeOper"));

  /* It is a copy: whatever the client does next cannot reach it. */
  memset(nick, 0, sizeof(nick));
  assert(0 == strcmp(module_loaded_by(mod), "SomeOper"));

  assert(module_unload(mod) != 0);

  /* A module the configuration loaded has no nick behind it. */
  mod = module_load("mod_good", 0, 0);
  assert(mod != 0);
  assert(module_loaded_by(mod) == 0);
  assert(module_unload(mod) != 0);

  printf("Passed: the loading operator's nick is recorded\n");
}

/** A module is named, not pathed: anything with a directory in it is
 * refused before dlopen() is reached, so a Module{} block cannot load a
 * shared object from outside the module directory.
 */
static void test_reject_path(void)
{
  const char* err = 0;

  assert(module_load("/tmp/mod_good", 0, &err) == 0);
  assert(err != 0);
  assert(module_count() == 0);

  err = 0;
  assert(module_load("../modules/mod_good", 0, &err) == 0);
  assert(err != 0);

  err = 0;
  assert(module_load("", 0, &err) == 0);
  assert(err != 0);

  printf("Passed: a path is not a module name\n");
}

/** The same module cannot be loaded twice. */
static void test_reject_duplicate(void)
{
  struct ModuleHandle* mod;
  const char* err = 0;

  mod = module_load("mod_good", 0, &err);
  assert(mod != 0);

  assert(module_load("mod_good", 0, &err) == 0);
  assert(err != 0);
  assert(module_count() == 1);

  assert(module_unload(mod) != 0);
  assert(module_count() == 0);

  printf("Passed: duplicate load rejected\n");
}

/** Repeated cycles leave the list consistent; run under valgrind to also
 * catch leaks.
 */
static void test_load_unload_cycles(void)
{
  struct ModuleHandle* mod;
  int i;

  for (i = 0; i < 50; i++) {
    mod = module_load("mod_good", 0, 0);
    assert(mod != 0);
    assert(module_count() == 1);
    assert(module_unload(mod) != 0);
    assert(module_count() == 0);
  }

  printf("Passed: 50 load/unload cycles\n");
}

/** module_shutdown() unloads everything that is still loaded. */
static void test_shutdown_unloads_all(void)
{
  assert(module_load("mod_good", 0, 0) != 0);
  assert(module_count() == 1);

  /* module_shutdown() empties the loader but leaves it usable; only
   * module_close(), which the server calls once at exit, ends it.
   */
  module_shutdown();
  assert(module_count() == 0);

  printf("Passed: shutdown unloads remaining modules\n");
}

/** Iteration visits every loaded module exactly once. */
static void test_iteration(void)
{
  struct ModuleHandle* mod;
  unsigned int seen = 0;

  assert(module_load("mod_good", 0, 0) != 0);

  for (mod = module_next(0); mod; mod = module_next(mod))
    seen++;

  assert(seen == module_count());
  assert(seen == 1);

  module_shutdown();

  printf("Passed: iteration over loaded modules\n");
}

/** Commands a module registers are reverted when it is unloaded, even the
 * ones the module itself never removes.
 */
static void test_command_registration(void)
{
  struct ModuleHandle* mod;

  stub_commands_live = 0;
  stub_commands_added = 0;

  mod = module_load("mod_cmd", 0, 0);
  assert(mod != 0);

  /* mod_cmd registers two and removes neither. */
  assert(stub_commands_added == 2);
  assert(stub_commands_live == 2);
  assert(module_command_count(mod) == 2);

  assert(module_unload(mod) != 0);

  /* The loader reverted both; module_stub asserts each is removed once. */
  assert(stub_commands_live == 0);

  printf("Passed: module commands are reverted on unload\n");
}

/** A module can remove its own command, and the loader does not then try to
 * remove it a second time.
 */
static void test_command_explicit_removal(void)
{
  struct ModuleHandle* mod;

  stub_commands_live = 0;

  mod = module_load("mod_cmd", 0, 0);
  assert(mod != 0);
  assert(stub_commands_live == 2);

  assert(module_del_command(mod, "TESTCMD") != 0);
  assert(stub_commands_live == 1);
  assert(module_command_count(mod) == 1);

  /* Removing it again finds nothing. */
  assert(module_del_command(mod, "TESTCMD") == 0);

  assert(module_unload(mod) != 0);
  assert(stub_commands_live == 0);

  printf("Passed: explicit command removal\n");
}

/** Repeated load/unload of a command-registering module stays balanced. */
static void test_command_cycles(void)
{
  struct ModuleHandle* mod;
  int i;

  stub_commands_live = 0;

  for (i = 0; i < 50; i++) {
    mod = module_load("mod_cmd", 0, 0);
    assert(mod != 0);
    assert(stub_commands_live == 2);
    assert(module_unload(mod) != 0);
    assert(stub_commands_live == 0);
  }

  printf("Passed: 50 cycles of a command-registering module\n");
}

int main(void)
{
  module_init();

  test_load_good();
  test_reject_bad_abi();
  test_reject_no_symbol();
  test_reject_failed_init();
  test_reject_missing_file();
  test_loaded_by();
  test_reject_path();
  test_reject_duplicate();
  test_load_unload_cycles();
  test_shutdown_unloads_all();
  test_iteration();
  test_command_registration();
  test_command_explicit_removal();
  test_command_cycles();

  printf("Done.\n");
  return 0;
}
