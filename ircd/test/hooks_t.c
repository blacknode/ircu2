/* hooks_t.c - Test the hook dispatch engine.
 *
 * Covers what the engine promises: hooks run in priority order, the chain
 * stops at the first hook that decides, a DENY cannot be overturned, a hook
 * can remove itself while it is running, and unloading a module takes its
 * hooks with it.
 */

#include "hooks.h"
#include "ircd_log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Order in which hooks were called this run. */
static char call_log[64];

static void log_reset(void)
{
  call_log[0] = '\0';
}

static void log_call(const char* what)
{
  strncat(call_log, what, sizeof(call_log) - strlen(call_log) - 1);
}

/* Two module handles are enough to test per-module teardown.  The engine
 * only ever compares these pointers, so fake ones do.
 */
static struct ModuleHandle* const MOD_A = (struct ModuleHandle*) 0x1;
static struct ModuleHandle* const MOD_B = (struct ModuleHandle*) 0x2;

static enum HookResult hook_a(struct HookContext* ctx, void* user)
{
  (void) ctx; (void) user;
  log_call("a");
  return HOOK_CONTINUE;
}

static enum HookResult hook_b(struct HookContext* ctx, void* user)
{
  (void) ctx; (void) user;
  log_call("b");
  return HOOK_CONTINUE;
}

static enum HookResult hook_c(struct HookContext* ctx, void* user)
{
  (void) ctx; (void) user;
  log_call("c");
  return HOOK_CONTINUE;
}

static enum HookResult hook_deny(struct HookContext* ctx, void* user)
{
  (void) user;
  log_call("D");
  ctx->hc_numeric = 404;
  strcpy(ctx->hc_reason, "denied by test");
  return HOOK_DENY;
}

static enum HookResult hook_allow(struct HookContext* ctx, void* user)
{
  (void) ctx; (void) user;
  log_call("A");
  return HOOK_ALLOW;
}

static enum HookResult hook_pending(struct HookContext* ctx, void* user)
{
  (void) ctx; (void) user;
  log_call("P");
  return HOOK_PENDING;
}

/** A hook that unregisters itself from inside its own call. */
static enum HookResult hook_self_removing(struct HookContext* ctx, void* user)
{
  (void) ctx; (void) user;
  log_call("s");
  hook_del(MOD_A, HOOK_CLIENT_PRE_NICK, hook_self_removing);
  return HOOK_CONTINUE;
}

/** Registration and counting. */
static void test_add_remove(void)
{
  hooks_init();

  assert(hook_count(HOOK_CLIENT_PRE_NICK) == 0);
  assert(!hook_is_active(HOOK_CLIENT_PRE_NICK));

  assert(hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_a,
                  HOOK_PRIORITY_DEFAULT, 0) != 0);
  assert(hook_count(HOOK_CLIENT_PRE_NICK) == 1);
  assert(hook_is_active(HOOK_CLIENT_PRE_NICK));

  /* Removing something that was never added finds nothing. */
  assert(hook_del(MOD_A, HOOK_CLIENT_PRE_NICK, hook_b) == 0);
  /* Nor does removing another module's hook. */
  assert(hook_del(MOD_B, HOOK_CLIENT_PRE_NICK, hook_a) == 0);

  assert(hook_del(MOD_A, HOOK_CLIENT_PRE_NICK, hook_a) != 0);
  assert(hook_count(HOOK_CLIENT_PRE_NICK) == 0);

  /* An unknown hook type is refused, not stored. */
  assert(hook_add(MOD_A, "mod_a", HOOK_LAST, hook_a, 0, 0) == 0);
  assert(hook_add(MOD_A, "mod_a", (enum HookType) -1, hook_a, 0, 0) == 0);

  printf("Passed: hook registration and counting\n");
}

/** Lower priority runs first; equal priority keeps insertion order. */
static void test_priority_order(void)
{
  struct HookContext ctx;

  hooks_init();

  /* Registered out of order on purpose. */
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_c, 30, 0);
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_a, 10, 0);
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_b, 20, 0);

  log_reset();
  hook_context_init(&ctx);
  assert(hook_run(HOOK_CLIENT_PRE_NICK, &ctx) == HOOK_CONTINUE);
  assert(0 == strcmp(call_log, "abc"));

  hooks_init();

  /* Same priority: first registered runs first. */
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_a, 50, 0);
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_b, 50, 0);
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_c, 50, 0);

  log_reset();
  hook_context_init(&ctx);
  hook_run(HOOK_CLIENT_PRE_NICK, &ctx);
  assert(0 == strcmp(call_log, "abc"));

  printf("Passed: priority ordering\n");
}

/** The chain stops at the first hook that decides. */
static void test_chain_stops(void)
{
  struct HookContext ctx;

  hooks_init();

  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_a, 10, 0);
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_deny, 20, 0);
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_c, 30, 0);

  log_reset();
  hook_context_init(&ctx);
  assert(hook_run(HOOK_CLIENT_PRE_NICK, &ctx) == HOOK_DENY);

  /* hook_c never ran. */
  assert(0 == strcmp(call_log, "aD"));

  /* The denying hook's explanation survives. */
  assert(ctx.hc_numeric == 404);
  assert(0 == strcmp(ctx.hc_reason, "denied by test"));

  printf("Passed: chain stops at the first decision\n");
}

/** A DENY cannot be overturned by a hook further down the chain. */
static void test_deny_is_final(void)
{
  struct HookContext ctx;

  hooks_init();

  /* An ALLOW registered after a DENY never gets the chance to run. */
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_deny, 10, 0);
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_allow, 20, 0);

  log_reset();
  hook_context_init(&ctx);
  assert(hook_run(HOOK_CLIENT_PRE_NICK, &ctx) == HOOK_DENY);
  assert(0 == strcmp(call_log, "D"));

  /* And the reverse: an ALLOW first means the DENY never runs, so load
   * order decides only which hook gets to speak, never whether a spoken
   * DENY holds.
   */
  hooks_init();
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_allow, 10, 0);
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_deny, 20, 0);

  log_reset();
  hook_context_init(&ctx);
  assert(hook_run(HOOK_CLIENT_PRE_NICK, &ctx) == HOOK_ALLOW);
  assert(0 == strcmp(call_log, "A"));

  printf("Passed: a DENY is final\n");
}

/** HOOK_PENDING is reserved but not implemented; it behaves as CONTINUE. */
static void test_pending_is_reserved(void)
{
  struct HookContext ctx;

  hooks_init();

  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_pending, 10, 0);
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_b, 20, 0);

  log_reset();
  hook_context_init(&ctx);

  /* The chain carries on rather than stalling an operation the server has
   * no way to resume.
   */
  assert(hook_run(HOOK_CLIENT_PRE_NICK, &ctx) == HOOK_CONTINUE);
  assert(0 == strcmp(call_log, "Pb"));

  printf("Passed: HOOK_PENDING falls back to HOOK_CONTINUE\n");
}

/** A hook may remove itself from inside its own call. */
static void test_self_removal(void)
{
  struct HookContext ctx;

  hooks_init();

  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_self_removing, 10, 0);
  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_b, 20, 0);
  assert(hook_count(HOOK_CLIENT_PRE_NICK) == 2);

  log_reset();
  hook_context_init(&ctx);
  hook_run(HOOK_CLIENT_PRE_NICK, &ctx);

  /* It ran, and the rest of the chain still ran after it. */
  assert(0 == strcmp(call_log, "sb"));
  assert(hook_count(HOOK_CLIENT_PRE_NICK) == 1);

  /* The next run does not call it again, and does not touch freed memory. */
  log_reset();
  hook_context_init(&ctx);
  hook_run(HOOK_CLIENT_PRE_NICK, &ctx);
  assert(0 == strcmp(call_log, "b"));

  printf("Passed: a hook can remove itself while running\n");
}

/** Unloading a module removes its hooks and leaves the others alone. */
static void test_module_teardown(void)
{
  struct HookContext ctx;

  hooks_init();

  hook_add(MOD_A, "mod_a", HOOK_CLIENT_PRE_NICK, hook_a, 10, 0);
  hook_add(MOD_B, "mod_b", HOOK_CLIENT_PRE_NICK, hook_b, 20, 0);
  hook_add(MOD_A, "mod_a", HOOK_CHANNEL_PRE_JOIN, hook_c, 10, 0);

  assert(hook_count(HOOK_CLIENT_PRE_NICK) == 2);
  assert(hook_count(HOOK_CHANNEL_PRE_JOIN) == 1);

  hook_del_module(MOD_A);

  /* Only MOD_B's hook is left, on either chain. */
  assert(hook_count(HOOK_CLIENT_PRE_NICK) == 1);
  assert(hook_count(HOOK_CHANNEL_PRE_JOIN) == 0);

  log_reset();
  hook_context_init(&ctx);
  hook_run(HOOK_CLIENT_PRE_NICK, &ctx);
  assert(0 == strcmp(call_log, "b"));

  hook_del_module(MOD_B);
  assert(hook_count(HOOK_CLIENT_PRE_NICK) == 0);

  printf("Passed: unloading a module removes its hooks\n");
}

/** Chains are independent, and an empty chain is a cheap no-op. */
static void test_chains_are_independent(void)
{
  struct HookContext ctx;

  hooks_init();

  hook_add(MOD_A, "mod_a", HOOK_MESSAGE_PRE_CHANNEL, hook_deny, 10, 0);

  /* A different hook type sees nothing. */
  hook_context_init(&ctx);
  assert(hook_run(HOOK_MESSAGE_PRE_PRIVATE, &ctx) == HOOK_CONTINUE);
  assert(!hook_is_active(HOOK_MESSAGE_PRE_PRIVATE));

  hook_context_init(&ctx);
  assert(hook_run(HOOK_MESSAGE_PRE_CHANNEL, &ctx) == HOOK_DENY);

  printf("Passed: hook chains are independent\n");
}

/** Call counters track dispatch, for /STATS. */
static void test_call_counters(void)
{
  struct HookContext ctx;
  int i;

  hooks_init();

  assert(hook_calls(HOOK_CLIENT_REGISTERED) == 0);

  for (i = 0; i < 5; i++) {
    hook_context_init(&ctx);
    hook_run(HOOK_CLIENT_REGISTERED, &ctx);
  }

  /* Counted even with nothing registered, so the numbers show which points
   * are hot regardless of what is loaded.
   */
  assert(hook_calls(HOOK_CLIENT_REGISTERED) == 5);
  assert(hook_calls(HOOK_CLIENT_PRE_NICK) == 0);

  printf("Passed: call counters\n");
}

/** Every hook type has a name, and none is left as "?". */
static void test_type_names(void)
{
  int i;

  for (i = 0; i < HOOK_LAST; i++) {
    const char* name = hook_type_name((enum HookType) i);
    assert(name != 0);
    assert(name[0] != '\0');
    assert(0 != strcmp(name, "?"));
  }

  assert(0 == strcmp(hook_type_name(HOOK_LAST), "?"));
  assert(0 == strcmp(hook_type_name((enum HookType) -1), "?"));

  printf("Passed: every hook type is named\n");
}

int main(int argc, char* argv[])
{
  (void) argc;
  (void) argv;

  test_add_remove();
  test_priority_order();
  test_chain_stops();
  test_deny_is_final();
  test_pending_is_reserved();
  test_self_removal();
  test_module_teardown();
  test_chains_are_independent();
  test_call_counters();
  test_type_names();

  printf("Done.\n");
  return 0;
}
