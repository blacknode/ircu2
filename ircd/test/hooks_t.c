/* hooks_t.c - Test the hook dispatch engine.
 *
 * Covers what the engine promises: hooks run in priority order, the chain
 * stops at the first hook that decides, a DENY cannot be overturned, a hook
 * can remove itself while it is running, and unloading a module takes its
 * hooks with it.
 *
 * And, for the command hooks, the five rules in hooks.h: a hook sees only
 * the command it named, a service bot's commands are hidden unless asked
 * for, a veto counts only for a client of this server, the chain is
 * bounded against a hook that dispatches commands of its own, and the two
 * registration calls refuse each other's hook types.
 */

#include "hooks.h"
#include "client.h"
#include "ircd_log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Stub for hook_deny_reply(), the only part of hooks.c that reaches into
 * the send layer.  Answering a client is not what this test is about, and
 * linking send.c would drag in most of the server.
 */
int send_reply(struct Client* to, int reply, ...)
{
  (void) to;
  (void) reply;
  return 0;
}

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

/* ------------------------------------------------------------------
 * Command hooks
 * ------------------------------------------------------------------ */

/** A local client and a remote one, and a service bot.
 *
 * MyConnect() is "the connection points back at me", so a local client
 * needs a Connection whose con_client is itself; a remote one has a
 * connection belonging to the server it came from.  That is the whole of
 * what hooks.c asks about a client, along with the +S flag.
 */
static struct Client cli_local;
static struct Client cli_remote;
static struct Client cli_service;
static struct Client cli_uplink;
static struct Connection con_local;
static struct Connection con_uplink;
static struct Connection con_service;

static void clients_init(void)
{
  memset(&cli_local, 0, sizeof(cli_local));
  memset(&cli_remote, 0, sizeof(cli_remote));
  memset(&cli_service, 0, sizeof(cli_service));
  memset(&cli_uplink, 0, sizeof(cli_uplink));
  memset(&con_local, 0, sizeof(con_local));
  memset(&con_uplink, 0, sizeof(con_uplink));
  memset(&con_service, 0, sizeof(con_service));

  con_local.con_client = &cli_local;
  cli_local.cli_connect = &con_local;
  strcpy(cli_local.cli_name, "local");

  /* Remote: its connection is the uplink's, so MyConnect() is false. */
  con_uplink.con_client = &cli_uplink;
  cli_uplink.cli_connect = &con_uplink;
  cli_remote.cli_connect = &con_uplink;
  strcpy(cli_remote.cli_name, "remote");

  con_service.con_client = &cli_service;
  cli_service.cli_connect = &con_service;
  cli_service.cli_uflags = FLAG_SERVBOT;
  strcpy(cli_service.cli_name, "NickServ");
}

/** The command most of these run. */
static struct HookCommand hcc_kick;

/** Build a context for a command from \a source. */
static void command_ctx(struct HookContext* ctx, struct Client* source,
                        const char* cmd)
{
  memset(&hcc_kick, 0, sizeof(hcc_kick));
  hcc_kick.hcc_cmd = cmd;
  hcc_kick.hcc_tok = "K";
  hcc_kick.hcc_handler = CLIENT_HANDLER;
  hcc_kick.hcc_parc = 3;

  hook_context_init(ctx);
  ctx->hc_source = source;
  ctx->hc_command = &hcc_kick;
}

static enum HookResult cmd_log_a(struct HookContext* ctx, void* user)
{
  (void) ctx; (void) user;
  log_call("a");
  return HOOK_CONTINUE;
}

static enum HookResult cmd_log_b(struct HookContext* ctx, void* user)
{
  (void) ctx; (void) user;
  log_call("b");
  return HOOK_CONTINUE;
}

static enum HookResult cmd_deny(struct HookContext* ctx, void* user)
{
  (void) user;
  log_call("d");
  ctx->hc_numeric = 482;
  strcpy(ctx->hc_reason, "no");
  return HOOK_DENY;
}

/** Only the command a hook named reaches it; NULL means every command. */
static void test_command_filter(void)
{
  struct HookContext ctx;

  assert(!hook_command_active(HOOK_COMMAND_PRE));
  assert(!hook_command_active(HOOK_CLIENT_REGISTERED));

  assert(hook_add_command(MOD_A, "a", HOOK_COMMAND_PRE, "KICK", cmd_log_a,
                          HOOK_PRIORITY_DEFAULT, 0, 0));
  assert(hook_add_command(MOD_B, "b", HOOK_COMMAND_PRE, 0, cmd_log_b,
                          HOOK_PRIORITY_DEFAULT, 0, 0));
  assert(hook_command_active(HOOK_COMMAND_PRE));
  assert(!hook_command_active(HOOK_COMMAND_POST));

  /* The named command reaches both. */
  log_reset();
  command_ctx(&ctx, &cli_local, "KICK");
  assert(hook_run_command(HOOK_COMMAND_PRE, &ctx) == HOOK_CONTINUE);
  assert(0 == strcmp(call_log, "ab"));

  /* Another command reaches only the one that named none. */
  log_reset();
  command_ctx(&ctx, &cli_local, "PRIVMSG");
  assert(hook_run_command(HOOK_COMMAND_PRE, &ctx) == HOOK_CONTINUE);
  assert(0 == strcmp(call_log, "b"));

  /* The name is matched the way IRC compares names. */
  log_reset();
  command_ctx(&ctx, &cli_local, "kick");
  assert(hook_run_command(HOOK_COMMAND_PRE, &ctx) == HOOK_CONTINUE);
  assert(0 == strcmp(call_log, "ab"));

  /* And the P10 token is not a command name: a module names the command. */
  log_reset();
  command_ctx(&ctx, &cli_local, "K");
  assert(hook_run_command(HOOK_COMMAND_PRE, &ctx) == HOOK_CONTINUE);
  assert(0 == strcmp(call_log, "b"));

  /* Removing distinguishes by the command as well as the callback. */
  assert(!hook_del_command(MOD_A, HOOK_COMMAND_PRE, "KILL", cmd_log_a));
  assert(!hook_del_command(MOD_A, HOOK_COMMAND_PRE, 0, cmd_log_a));
  assert(hook_del_command(MOD_A, HOOK_COMMAND_PRE, "KICK", cmd_log_a));
  assert(hook_del_command(MOD_B, HOOK_COMMAND_PRE, 0, cmd_log_b));
  assert(!hook_command_active(HOOK_COMMAND_PRE));

  printf("Passed: a command hook sees the command it named\n");
}

/** The two registration calls refuse each other's hook types. */
static void test_command_registration_types(void)
{
  assert(!hook_add(MOD_A, "a", HOOK_COMMAND_PRE, cmd_log_a,
                   HOOK_PRIORITY_DEFAULT, 0));
  assert(!hook_add(MOD_A, "a", HOOK_COMMAND_POST, cmd_log_a,
                   HOOK_PRIORITY_DEFAULT, 0));
  assert(!hook_add_command(MOD_A, "a", HOOK_CLIENT_REGISTERED, "KICK",
                           cmd_log_a, HOOK_PRIORITY_DEFAULT, 0, 0));
  assert(hook_count(HOOK_COMMAND_PRE) == 0);
  assert(hook_count(HOOK_CLIENT_REGISTERED) == 0);

  /* An empty command name means the same as none at all, rather than a
   * command nothing is ever called.
   */
  assert(hook_add_command(MOD_A, "a", HOOK_COMMAND_PRE, "", cmd_log_a,
                          HOOK_PRIORITY_DEFAULT, 0, 0));
  {
    struct HookContext ctx;
    log_reset();
    command_ctx(&ctx, &cli_local, "WHATEVER");
    hook_run_command(HOOK_COMMAND_PRE, &ctx);
    assert(0 == strcmp(call_log, "a"));
  }
  assert(hook_del_command(MOD_A, HOOK_COMMAND_PRE, "", cmd_log_a));

  printf("Passed: command hooks and lifecycle hooks do not mix\n");
}

/** A service bot's commands are hidden unless a hook asks for them. */
static void test_command_services(void)
{
  struct HookContext ctx;

  assert(hook_add_command(MOD_A, "a", HOOK_COMMAND_PRE, "KICK", cmd_log_a,
                          HOOK_PRIORITY_DEFAULT, 0, 0));
  assert(hook_add_command(MOD_B, "b", HOOK_COMMAND_PRE, "KICK", cmd_log_b,
                          HOOK_PRIORITY_DEFAULT, 0,
                          HOOK_CMD_INCLUDE_SERVICES));

  /* An ordinary client reaches both. */
  log_reset();
  command_ctx(&ctx, &cli_local, "KICK");
  hook_run_command(HOOK_COMMAND_PRE, &ctx);
  assert(0 == strcmp(call_log, "ab"));

  /* A service bot reaches only the hook that asked for it. */
  log_reset();
  command_ctx(&ctx, &cli_service, "KICK");
  hook_run_command(HOOK_COMMAND_PRE, &ctx);
  assert(0 == strcmp(call_log, "b"));

  assert(hook_del_command(MOD_A, HOOK_COMMAND_PRE, "KICK", cmd_log_a));
  assert(hook_del_command(MOD_B, HOOK_COMMAND_PRE, "KICK", cmd_log_b));

  printf("Passed: a service bot's commands are opt-in\n");
}

/** A veto counts for a client of this server, and nowhere else.
 *
 * Refusing a command that another server has already applied would leave
 * this one disagreeing with the network, which is worse than whatever the
 * module was trying to prevent.
 */
static void test_command_veto_is_local(void)
{
  struct HookContext ctx;

  assert(hook_add_command(MOD_A, "a", HOOK_COMMAND_PRE, "KICK", cmd_deny,
                          HOOK_PRIORITY_DEFAULT, 0, 0));
  assert(hook_add_command(MOD_A, "a", HOOK_COMMAND_POST, "KICK", cmd_deny,
                          HOOK_PRIORITY_DEFAULT, 0, 0));

  /* Local: honoured, with the numeric and reason the module set. */
  command_ctx(&ctx, &cli_local, "KICK");
  assert(hook_run_command(HOOK_COMMAND_PRE, &ctx) == HOOK_DENY);
  assert(ctx.hc_numeric == 482);
  assert(0 == strcmp(ctx.hc_reason, "no"));

  /* Remote: the hook still runs, the veto does not count. */
  log_reset();
  command_ctx(&ctx, &cli_remote, "KICK");
  assert(hook_run_command(HOOK_COMMAND_PRE, &ctx) == HOOK_CONTINUE);
  assert(0 == strcmp(call_log, "d"));

  /* No source at all is not a client of this server either. */
  command_ctx(&ctx, 0, "KICK");
  assert(hook_run_command(HOOK_COMMAND_PRE, &ctx) == HOOK_CONTINUE);

  /* POST is a notification: it cannot refuse anything, local or not. */
  command_ctx(&ctx, &cli_local, "KICK");
  assert(hook_run_command(HOOK_COMMAND_POST, &ctx) == HOOK_CONTINUE);

  assert(hook_del_command(MOD_A, HOOK_COMMAND_PRE, "KICK", cmd_deny));
  assert(hook_del_command(MOD_A, HOOK_COMMAND_POST, "KICK", cmd_deny));

  printf("Passed: a command veto counts only for a local client\n");
}

/** How deep the recursive hook got. */
static int recursion_depth;
/** How many times it was actually entered. */
static int recursion_calls;

static enum HookResult cmd_recurse(struct HookContext* ctx, void* user)
{
  struct HookContext inner;

  (void) user;
  recursion_calls++;

  if (recursion_depth++ < 100) {
    command_ctx(&inner, ctx->hc_source, "KICK");
    hook_run_command(HOOK_COMMAND_PRE, &inner);
  }

  return HOOK_CONTINUE;
}

/** A hook that dispatches commands of its own is bounded. */
static void test_command_recursion(void)
{
  struct HookContext ctx;

  assert(hook_add_command(MOD_A, "a", HOOK_COMMAND_PRE, "KICK", cmd_recurse,
                          HOOK_PRIORITY_DEFAULT, 0, 0));

  recursion_depth = 0;
  recursion_calls = 0;
  command_ctx(&ctx, &cli_local, "KICK");
  assert(hook_run_command(HOOK_COMMAND_PRE, &ctx) == HOOK_CONTINUE);

  /* It stopped on its own rather than running out of stack. */
  assert(recursion_calls > 1);
  assert(recursion_calls < 100);

  assert(hook_del_command(MOD_A, HOOK_COMMAND_PRE, "KICK", cmd_recurse));

  printf("Passed: command hook recursion stops after %d calls\n",
         recursion_calls);
}

/** Unloading a module takes its command hooks with it. */
static void test_command_teardown(void)
{
  struct HookContext ctx;

  assert(hook_add_command(MOD_A, "a", HOOK_COMMAND_PRE, "KICK", cmd_log_a,
                          HOOK_PRIORITY_DEFAULT, 0, 0));
  assert(hook_add_command(MOD_A, "a", HOOK_COMMAND_POST, 0, cmd_log_a,
                          HOOK_PRIORITY_DEFAULT, 0, 0));
  assert(hook_add_command(MOD_B, "b", HOOK_COMMAND_PRE, "KICK", cmd_log_b,
                          HOOK_PRIORITY_DEFAULT, 0, 0));

  hook_del_module(MOD_A);

  assert(hook_count(HOOK_COMMAND_POST) == 0);
  assert(hook_count(HOOK_COMMAND_PRE) == 1);

  log_reset();
  command_ctx(&ctx, &cli_local, "KICK");
  hook_run_command(HOOK_COMMAND_PRE, &ctx);
  assert(0 == strcmp(call_log, "b"));

  hook_del_module(MOD_B);
  assert(!hook_command_active(HOOK_COMMAND_PRE));

  printf("Passed: command hooks go away with their module\n");
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

  clients_init();
  test_command_filter();
  test_command_registration_types();
  test_command_services();
  test_command_veto_is_local();
  test_command_recursion();
  test_command_teardown();

  printf("Done.\n");
  return 0;
}
