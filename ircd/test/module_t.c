/* module_t.c - Test the module loader.
 *
 * Exercises the acceptance criteria for the loader: a well-formed module
 * loads and unloads, a malformed one is rejected without leaving anything
 * behind, and repeated load/unload cycles neither leak nor corrupt the
 * module list.
 *
 * The test modules live in modules/ next to this file and are built as
 * MODULE libraries by CMake into a tree shaped like the server's module
 * directory: a type directory holding <name>.so or <name>/<name>.so.
 * module.c is compiled with IRCU_MODULE_DIR pointing at that build
 * directory, so the loader resolves the fixtures by name just as the
 * server resolves real modules against MOD_PATH.
 */

#include "migration.h"
#include "module.h"
#include "capab.h"
#include "sasl.h"
#include "hooks.h"
#include "channel.h"
#include "client.h"
#include "s_conf.h"
#include "ircd_log.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Touched by mod_good so the test can prove the module ran and that it
 * resolved a symbol out of this binary's dynamic symbol table.
 */
int module_test_marker;

/** Written by mod_umode with the bits the server assigned it, so the test
 * can check they are real and distinct without knowing which ones they are.
 */
flag_t mod_umode_flag_one;
flag_t mod_umode_flag_two;

/** Written by mod_cmode with the bits its letters map to, so the test can
 * check they are the letters' own bits and not something handed out.
 */
chanmode_t mod_cmode_flag_one;
chanmode_t mod_cmode_flag_two;

/** Written by mod_cap with the positions the server assigned it, so the
 * test can check they are real and distinct without knowing which ones.
 */
int mod_cap_index_one;
int mod_cap_index_two;

/** Bumped by mod_cmdhook's callbacks, so the test can see them run. */
int mod_cmdhook_pre_calls;
int mod_cmdhook_post_calls;

/* The isolated-module half of the loader is ircd/modhost.c, which is a
 * process, a socket and the whole of the server's state; none of that
 * belongs in a test of the register.  What module.c calls into is these
 * three, and here they say "there is no host", which is the answer for
 * every module this test loads.
 */
int modhost_start(struct ModuleHandle *mod, const char **errstr) {
  (void) mod;
  if (errstr)
    *errstr = "this build has no module host";
  return 0;
}

void modhost_stop(struct ModuleHandle *mod) {
  (void) mod;
}

int modhost_pid(const struct ModuleHandle *mod) {
  (void) mod;
  return 0;
}

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
   * path the loader built from it ends in that name, under the type
   * directory it found the module in.
   */
  assert(module_find_file("mod_good") == mod);
  assert(0 == strcmp(module_file(mod), "mod_good"));
  assert(0 != strstr(module_path(mod), "/test/mod_good.so"));
  assert(0 == strcmp(module_relpath(mod), "test/mod_good.so"));

  /* The directory is the path without the file name, and is absolute. */
  assert(module_dir(mod)[0] == '/');
  assert(strlen(module_dir(mod)) + strlen("/mod_good.so")
         == strlen(module_path(mod)));
  assert(0 == strncmp(module_dir(mod), module_path(mod),
                      strlen(module_dir(mod))));
  assert(0 == strcmp(module_dir(mod) + strlen(module_dir(mod)) - 5, "/test"));

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

/** A name that matches nothing in any type directory is refused. */
static void test_reject_missing_file(void)
{
  const char* err = 0;

  assert(module_load("no_such_module_here", 0, &err) == 0);
  assert(err != 0);
  assert(strstr(err, "no_such_module_here") != 0);
  assert(module_count() == 0);

  printf("Passed: missing file rejected\n");
}

/** A shared object directly in the module directory is not a module: the
 * loader only looks inside the type directories.
 */
static void test_reject_flat(void)
{
  const char* err = 0;

  assert(module_load("mod_flat", 0, &err) == 0);
  assert(err != 0);
  assert(module_count() == 0);

  printf("Passed: a shared object outside any type directory is not found\n");
}

/** The same name under two type directories is refused rather than
 * resolved to whichever readdir() happened to return first.
 */
static void test_reject_ambiguous(void)
{
  const char* err = 0;

  assert(module_load("mod_dup", 0, &err) == 0);
  assert(err != 0);
  assert(strstr(err, "ambiguous") != 0);
  assert(strstr(err, "alpha/mod_dup.so") != 0);
  assert(strstr(err, "beta/mod_dup.so") != 0);
  assert(module_count() == 0);

  printf("Passed: a name found under two types is rejected\n");
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

  /* This fixture is built in the directory shape, <name>/<name>.so, and
   * the loader found it there by the same bare name.
   */
  assert(0 == strcmp(module_relpath(mod), "test/mod_cmd/mod_cmd.so"));
  assert(0 == strcmp(module_dir(mod) + strlen(module_dir(mod)) - 13,
                     "/test/mod_cmd"));

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

/* --- user modes ------------------------------------------------------- */


/** Number of modes currently registered with the core. */
static unsigned int core_mode_count(void)
{
  const struct UserMode* um;
  unsigned int n = 0;

  for (um = client_user_modes(); um; um = um->next)
    n++;

  return n;
}

/** Find a registered mode by letter. */
static const struct UserMode* core_mode_find(char c)
{
  return client_find_user_mode(c);
}

/** A module's user modes reach the core registry and are reverted on
 * unload, whether or not the module removed them itself.
 */
static void test_user_mode_registration(void)
{
  struct ModuleHandle* mod;
  unsigned int before = core_mode_count();

  mod = module_load("mod_umode", 0, 0);
  assert(mod != 0);

  assert(module_user_mode_count(mod) == 2);
  assert(core_mode_count() == before + 2);

  /* The server assigned the bits; the module did not choose them, and no
   * two modes share one.
   */
  assert(mod_umode_flag_one != 0);
  assert(mod_umode_flag_two != 0);
  assert(mod_umode_flag_one != mod_umode_flag_two);
  assert((mod_umode_flag_one & mod_umode_flag_two) == 0);
  assert((mod_umode_flag_one & FLAG_OPER) == 0 && "a core bit was reused");

  assert(core_mode_find('Y') != 0);
  assert(core_mode_find('Y')->flag == mod_umode_flag_one);
  assert(core_mode_find('Z')->flag == mod_umode_flag_two);

  assert(module_unload(mod) != 0);

  assert(core_mode_count() == before);
  assert(core_mode_find('Y') == 0);
  assert(core_mode_find('Z') == 0);

  printf("Passed: module user modes are reverted on unload\n");
}

/** A module can remove its own mode, and cannot remove anyone else's. */
static void test_user_mode_explicit_removal(void)
{
  struct ModuleHandle* mod;
  unsigned int before = core_mode_count();

  mod = module_load("mod_umode", 0, 0);
  assert(mod != 0);
  assert(module_user_mode_count(mod) == 2);

  assert(module_del_user_mode(mod, 'Y') != 0);
  assert(module_user_mode_count(mod) == 1);
  assert(core_mode_find('Y') == 0);

  /* Removing it again finds nothing, and neither does reaching for a mode
   * the core owns.
   */
  assert(module_del_user_mode(mod, 'Y') == 0);
  assert(module_del_user_mode(mod, 'o') == 0);
  assert(core_mode_find('o') != 0 && "a core mode survived the attempt");

  assert(module_unload(mod) != 0);
  assert(core_mode_count() == before);

  printf("Passed: explicit user mode removal\n");
}

/** Repeated load/unload neither leaks mode slots nor runs out of bits. */
static void test_user_mode_cycles(void)
{
  struct ModuleHandle* mod;
  unsigned int before = core_mode_count();
  flag_t first_run;
  int i;

  mod = module_load("mod_umode", 0, 0);
  assert(mod != 0);
  first_run = mod_umode_flag_one;
  assert(module_unload(mod) != 0);

  for (i = 0; i < 50; i++) {
    mod = module_load("mod_umode", 0, 0);
    assert(mod != 0);
    assert(core_mode_count() == before + 2);
    assert(module_unload(mod) != 0);
    assert(core_mode_count() == before);
  }

  /* A freed bit is handed out again rather than the pool draining. */
  mod = module_load("mod_umode", 0, 0);
  assert(mod != 0);
  assert(mod_umode_flag_one == first_run);
  assert(module_unload(mod) != 0);

  printf("Passed: 50 cycles of a mode-registering module\n");
}

/** RPL_MYINFO advertises whatever is registered, module modes included. */
static void test_user_mode_advertised(void)
{
  struct ModuleHandle* mod;

  assert(strchr(client_user_mode_chars(), 'Y') == 0);

  mod = module_load("mod_umode", 0, 0);
  assert(mod != 0);

  assert(strchr(client_user_mode_chars(), 'Y') != 0);
  assert(strchr(client_user_mode_chars(), 'Z') != 0);
  assert(strchr(client_user_mode_chars(), 'o') != 0 && "core modes are still there");

  assert(module_unload(mod) != 0);
  assert(strchr(client_user_mode_chars(), 'Y') == 0);

  printf("Passed: module user modes are advertised in RPL_MYINFO\n");
}

/** Number of channel modes currently registered. */
static unsigned int core_chan_mode_count(void)
{
  const struct ChanMode* cm;
  unsigned int n = 0;

  for (cm = channel_chan_modes(); cm; cm = cm->next)
    n++;

  return n;
}

/** A module's channel modes reach the register and are reverted on
 * unload, whether or not the module removed them itself.
 */
static void test_chan_mode_registration(void)
{
  struct ModuleHandle* mod;
  unsigned int before = core_chan_mode_count();

  mod = module_load("mod_cmode", 0, 0);
  assert(mod != 0);

  assert(module_chan_mode_count(mod) == 2);
  assert(core_chan_mode_count() == before + 2);

  /* The bit follows from the letter and from nothing else, so the module
   * gets the same bit on every server that loads it.
   */
  assert(mod_cmode_flag_one == channel_chan_mode_flag('W'));
  assert(mod_cmode_flag_two == channel_chan_mode_flag('X'));
  assert((mod_cmode_flag_one & mod_cmode_flag_two) == 0);
  assert((mod_cmode_flag_one & MODE_CHANOP) == 0 && "a core bit was reused");
  assert((mod_cmode_flag_one & CHANMODE_RESERVED) == 0);

  assert(channel_find_chan_mode('W') != 0);
  assert(channel_find_chan_mode('W')->flag == mod_cmode_flag_one);
  assert(channel_find_chan_mode('X')->flag == mod_cmode_flag_two);

  assert(module_unload(mod) != 0);

  assert(core_chan_mode_count() == before);
  assert(channel_find_chan_mode('W') == 0);
  assert(channel_find_chan_mode('X') == 0);

  printf("Passed: module channel modes are reverted on unload\n");
}

/** A module can remove its own mode, and cannot remove anyone else's. */
static void test_chan_mode_explicit_removal(void)
{
  struct ModuleHandle* mod;
  unsigned int before = core_chan_mode_count();

  mod = module_load("mod_cmode", 0, 0);
  assert(mod != 0);
  assert(module_chan_mode_count(mod) == 2);

  assert(module_del_chan_mode(mod, 'W') != 0);
  assert(module_chan_mode_count(mod) == 1);
  assert(channel_find_chan_mode('W') == 0);

  /* Removing it again finds nothing, and neither does reaching for a mode
   * the core owns.
   */
  assert(module_del_chan_mode(mod, 'W') == 0);
  assert(module_del_chan_mode(mod, 'o') == 0);
  assert(channel_find_chan_mode('o') != 0 && "a core mode survived the attempt");

  assert(module_unload(mod) != 0);
  assert(core_chan_mode_count() == before);

  printf("Passed: explicit channel mode removal\n");
}

/** A module's capabilities reach the register and are reverted on unload,
 * whether or not the module removed them itself.
 */
static void test_cap_registration(void)
{
  struct ModuleHandle* mod;
  unsigned int before = cap_count();

  mod = module_load("mod_cap", 0, 0);
  assert(mod != 0);

  assert(module_cap_count(mod) == 2);
  assert(cap_count() == before + 2);

  /* Unlike a channel mode, the position does not follow from the name:
   * the server hands out whatever is free, so all the module may assume
   * is that it got something real, distinct, and not one of the core's.
   */
  assert(mod_cap_index_one != CAP_NONE);
  assert(mod_cap_index_two != CAP_NONE);
  assert(mod_cap_index_one != mod_cap_index_two);
  assert(mod_cap_index_one >= CAP_LAST_CORE_CAP && "a core position was reused");
  assert(mod_cap_index_two >= CAP_LAST_CORE_CAP);

  assert(cap_find("example.org/one") != 0);
  assert(cap_find("example.org/one")->cap_index == mod_cap_index_one);
  assert(cap_find("example.org/one")->cap_owner == mod);
  assert(cap_find("example.org/two")->cap_index == mod_cap_index_two);
  assert(cap_find("example.org/two")->cap_flags & CAPFL_STICKY);
  assert(cap_find_index(mod_cap_index_one) == cap_find("example.org/one"));

  assert(module_unload(mod) != 0);

  assert(cap_count() == before);
  assert(cap_find("example.org/one") == 0);
  assert(cap_find("example.org/two") == 0);
  assert(cap_find_index(mod_cap_index_one) == 0);
  /* The core's are untouched. */
  assert(cap_find("echo-message") != 0);

  printf("Passed: module capabilities are reverted on unload\n");
}

/** A module's SASL mechanisms reach the register and go away with it. */
static void test_sasl_registration(void)
{
  struct ModuleHandle* mod;
  struct SaslSession ses;
  unsigned int before = sasl_count();

  mod = module_load("mod_sasl", 0, 0);
  assert(mod != 0);

  assert(sasl_module_count(mod) == 2);
  assert(sasl_count() == before + 2);

  assert(sasl_find("X-TEST") != 0);
  assert(sasl_find("X-TEST")->sm_owner == mod);
  assert(sasl_find("X-OTHER")->sm_flags & SASL_MECH_NEEDS_TLS);

  /* The registration is not just a name: the exchange runs the module's
   * code.  "YUBj" is base64 for "abc".
   */
  sasl_session_init(&ses);
  assert(sasl_session_begin(&ses, "X-TEST") == 0);
  assert(sasl_session_input(&ses, "YWJj") == SASL_CREDENTIAL);
  assert(0 == strcmp(ses.ss_authcid, "abc"));
  sasl_session_clear(&ses);

  assert(module_unload(mod) != 0);

  assert(sasl_count() == before);
  assert(sasl_find("X-TEST") == 0);
  assert(sasl_find("X-OTHER") == 0);
  /* The core's are untouched. */
  assert(sasl_find("PLAIN") != 0);
  assert(sasl_find("EXTERNAL") != 0);

  printf("Passed: module SASL mechanisms are reverted on unload\n");
}

/** A module can remove its own mechanism, and nobody else's. */
static void test_sasl_explicit_removal(void)
{
  struct ModuleHandle* mod;
  unsigned int before = sasl_count();

  mod = module_load("mod_sasl", 0, 0);
  assert(mod != 0);
  assert(sasl_module_count(mod) == 2);

  assert(module_del_sasl_mechanism(mod, "X-TEST") != 0);
  assert(sasl_module_count(mod) == 1);
  assert(sasl_find("X-TEST") == 0);

  /* Not the core's, and not one it never registered. */
  assert(module_del_sasl_mechanism(mod, "PLAIN") == 0);
  assert(module_del_sasl_mechanism(mod, "NOSUCH") == 0);
  assert(sasl_find("PLAIN") != 0);

  assert(module_unload(mod) != 0);
  assert(sasl_count() == before);

  printf("Passed: explicit SASL mechanism removal\n");
}

/** A module can remove its own capability, and nobody else's. */
static void test_cap_explicit_removal(void)
{
  struct ModuleHandle* mod;
  unsigned int before = cap_count();

  mod = module_load("mod_cap", 0, 0);
  assert(mod != 0);
  assert(module_cap_count(mod) == 2);

  assert(module_del_cap(mod, "example.org/one") != 0);
  assert(module_cap_count(mod) == 1);
  assert(cap_find("example.org/one") == 0);

  /* Removing it again finds nothing, and neither does reaching for one
   * the core owns.
   */
  assert(module_del_cap(mod, "example.org/one") == 0);
  assert(module_del_cap(mod, "echo-message") == 0);
  assert(cap_find("echo-message") != 0 && "a core capability survived the attempt");
  assert(module_del_cap(mod, "") == 0);
  assert(module_del_cap(mod, 0) == 0);

  assert(module_unload(mod) != 0);
  assert(cap_count() == before);

  printf("Passed: explicit capability removal\n");
}

/** A module's command hooks reach the chains and are reverted on unload.
 *
 * The same callback on two commands is two registrations, not one: the
 * command is part of what identifies a command hook, which is what lets a
 * module watch KICK and KILL with one function and give either back on
 * its own.
 */
static void test_command_hook_registration(void)
{
  struct ModuleHandle* mod;

  assert(hook_count(HOOK_COMMAND_PRE) == 0);
  assert(hook_count(HOOK_COMMAND_POST) == 0);

  mod = module_load("mod_cmdhook", 0, 0);
  assert(mod != 0);

  assert(hook_count(HOOK_COMMAND_PRE) == 2);
  assert(hook_count(HOOK_COMMAND_POST) == 1);
  /* The lifecycle point it tried to register as a command hook was
   * refused, and the module said so by loading anyway.
   */
  assert(hook_count(HOOK_CLIENT_REGISTERED) == 0);

  /* It can hand one back while it stays loaded, and only its own. */
  assert(module_del_command_hook(mod, HOOK_COMMAND_PRE, "KILL", 0) == 0);
  assert(hook_count(HOOK_COMMAND_PRE) == 2);

  assert(module_unload(mod) != 0);

  assert(hook_count(HOOK_COMMAND_PRE) == 0);
  assert(hook_count(HOOK_COMMAND_POST) == 0);

  printf("Passed: module command hooks are reverted on unload\n");
}

/** Repeated load/unload neither leaks positions nor strands them. */
static void test_cap_cycles(void)
{
  struct ModuleHandle* mod;
  unsigned int before = cap_count();
  int first_run;
  int i;

  mod = module_load("mod_cap", 0, 0);
  assert(mod != 0);
  first_run = mod_cap_index_one;
  assert(module_unload(mod) != 0);

  for (i = 0; i < 50; i++) {
    mod = module_load("mod_cap", 0, 0);
    assert(mod != 0);
    assert(cap_count() == before + 2);
    assert(module_unload(mod) != 0);
    assert(cap_count() == before);
  }

  /* A freed position is handed straight back out, so fifty cycles do not
   * walk the register up towards CAP_MAX.
   */
  assert(mod_cap_index_one == first_run);

  printf("Passed: capability load/unload cycles reuse the position\n");
}

/** Repeated load/unload neither leaks slots nor changes the bit. */
static void test_chan_mode_cycles(void)
{
  struct ModuleHandle* mod;
  unsigned int before = core_chan_mode_count();
  chanmode_t first_run;
  int i;

  mod = module_load("mod_cmode", 0, 0);
  assert(mod != 0);
  first_run = mod_cmode_flag_one;
  assert(module_unload(mod) != 0);

  for (i = 0; i < 50; i++) {
    mod = module_load("mod_cmode", 0, 0);
    assert(mod != 0);
    assert(core_chan_mode_count() == before + 2);
    assert(module_unload(mod) != 0);
    assert(core_chan_mode_count() == before);
  }

  /* The letter still maps to the same bit; nothing is a pool that drains. */
  mod = module_load("mod_cmode", 0, 0);
  assert(mod != 0);
  assert(mod_cmode_flag_one == first_run);
  assert(module_unload(mod) != 0);

  printf("Passed: 50 cycles of a channel-mode-registering module\n");
}

/** What the server advertises follows the register, module modes included. */
static void test_chan_mode_advertised(void)
{
  struct ModuleHandle* mod;

  assert(strchr(channel_chan_mode_chars(), 'W') == 0);

  mod = module_load("mod_cmode", 0, 0);
  assert(mod != 0);

  assert(strchr(channel_chan_mode_chars(), 'W') != 0);
  assert(strchr(channel_chan_mode_chars(), 'X') != 0);
  assert(strchr(channel_chan_mode_chars(), 'o') != 0 && "core modes are still there");

  /* A module's mode takes no argument, so it is advertised in the last
   * CHANMODES group and not among the ones that do.
   */
  assert(strchr(channel_chan_mode_param_chars(), 'W') == 0);
  assert(strchr(strrchr(channel_chanmodes_supported(), ','), 'W') != 0);

  assert(module_unload(mod) != 0);
  assert(strchr(channel_chan_mode_chars(), 'W') == 0);

  printf("Passed: module channel modes are advertised\n");
}

/* ------------------------------------------------------------------------
 * Migrations
 * ------------------------------------------------------------------------ */

static void test_migrations_loaded(void)
{
  const struct MigrationSet* set;
  struct ModuleHandle* mod;
  const char* err = 0;

  mod = module_load("mod_mig", 0, &err);
  assert(mod != 0);
  assert(!err);

  /* The .sql files were compiled into the shared object; nothing was copied
   * beside it, and the loader read them from the module itself.
   */
  set = module_migrations(mod);
  assert(set != 0);
  assert(set->ms_count == 2);
  assert(!strcmp(set->ms_module, "mod_mig"));
  assert(set->ms_list[0].mg_version == 1);
  assert(!strcmp(set->ms_list[0].mg_name, "create_thing"));
  assert(strstr(set->ms_list[0].mg_up, "CREATE TABLE") != 0);
  assert(strstr(set->ms_list[0].mg_down, "DROP TABLE") != 0);
  assert(set->ms_list[1].mg_version == 2);
  assert(!strcmp(set->ms_list[1].mg_name, "add_label"));

  assert(module_unload(mod) != 0);

  printf("Passed: a module's migrations are embedded and read at load\n");
}

static void test_module_without_migrations(void)
{
  struct ModuleHandle* mod = module_load("mod_good", 0, 0);

  assert(mod != 0);
  assert(module_migrations(mod) == 0);
  assert(module_unload(mod) != 0);

  printf("Passed: a module with no migrations has no set\n");
}

static void test_reject_bad_migrations(void)
{
  const char* err = 0;

  /* An up with no down.  The module is otherwise perfectly good, and it
   * still must not load: a migration nobody can revert is not one an
   * operator can safely apply.
   */
  assert(module_load("mod_migbad", 0, &err) == 0);
  assert(err != 0);
  assert(strstr(err, "v1_lonely") != 0);
  assert(strstr(err, "down") != 0);
  assert(module_find_file("mod_migbad") == 0);

  printf("Passed: a module with an unpaired migration is refused (%s)\n",
         err);
}

static void test_reject_reserved_name(void)
{
  const char* err = 0;

  /* "core" is the migrations table's name for the server itself. */
  assert(module_load("mod_core", 0, &err) == 0);
  assert(err != 0);
  assert(strstr(err, "core") != 0);
  assert(module_find("core") == 0);

  printf("Passed: a module calling itself \"core\" is refused (%s)\n", err);
}

int main(void)
{
  channel_init_chan_modes();
  cap_init();
  sasl_init();
  module_init();
  client_init_user_modes();

  test_load_good();
  test_reject_bad_abi();
  test_reject_no_symbol();
  test_reject_failed_init();
  test_reject_missing_file();
  test_reject_flat();
  test_reject_ambiguous();
  test_loaded_by();
  test_reject_path();
  test_reject_duplicate();
  test_load_unload_cycles();
  test_shutdown_unloads_all();
  test_iteration();
  test_command_registration();
  test_command_explicit_removal();
  test_command_cycles();
  test_user_mode_registration();
  test_user_mode_explicit_removal();
  test_user_mode_cycles();
  test_user_mode_advertised();
  test_chan_mode_registration();
  test_chan_mode_explicit_removal();
  test_chan_mode_cycles();
  test_chan_mode_advertised();
  test_cap_registration();
  test_cap_explicit_removal();
  test_cap_cycles();
  test_sasl_registration();
  test_sasl_explicit_removal();
  test_command_hook_registration();
  test_migrations_loaded();
  test_module_without_migrations();
  test_reject_bad_migrations();
  test_reject_reserved_name();

  printf("Done.\n");
  return 0;
}
