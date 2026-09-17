/*
 * IRC - Internet Relay Chat, modules/commands/isolated_demo.c
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
 * @brief The reference module for isolation = "process".
 *
 * There is nothing in this file about isolation.  That is the point: the
 * same shared object runs inside the server or inside a host of its own,
 * and which it is is a line in ircd.conf, not a line here.
 *
 * @code
 *   Module { name = "isolated_demo"; isolation = "process"; };
 * @endcode
 *
 * It registers a command and a hook and stays inside the profile
 * ircd/modhost/modhost_api.c implements -- commands, hooks, the log, the
 * allocator, the string helpers, the features, finding a client and
 * sending to one.  A module that reaches past that profile does not load
 * isolated: dlopen() is RTLD_NOW and the host fails at load, naming the
 * symbol.  See doc/readme.isolation.
 *
 * @code
 *   /DEMO                -- who am I, and where is this running
 *   /DEMO CRASH          -- dereference NULL on purpose
 *   /DEMO SPIN           -- loop for ever on purpose
 * @endcode
 *
 * The last two are there because a reference module for isolation that
 * could not demonstrate the isolation would be demonstrating nothing.
 * Loaded natively they take the server down, which is the honest way to
 * show what the configuration line buys.
 */
#include "config.h"

#include "client.h"
#include "hooks.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "module.h"
#include "msg.h"
#include "numeric.h"
#include "send.h"

#include <string.h>

/** This module's handle. */
static struct ModuleHandle* demo_mod;

/** How many nicknames have gone past, to show a hook running. */
static unsigned int demo_nicks;

/** Answer /DEMO.
 * @param[in] cptr Client that sent it.
 * @param[in] sptr Original source.
 * @param[in] parc Parameter count.
 * @param[in] parv Parameters.
 */
static int demo_m_demo(struct Client* cptr, struct Client* sptr, int parc,
                       char* parv[])
{
  const char* what = parc > 1 ? parv[1] : "";

  (void) cptr;

  if (!ircd_strcmp(what, "CRASH")) {
    struct Client* nobody = 0;

    log_write(LS_SYSTEM, L_WARNING, 0,
              "isolated_demo: %s asked for a crash; obliging",
              cli_name(sptr));

    /* Deliberate, and the whole demonstration: in a host this takes the
     * host, the server says so and carries on.  Loaded natively it takes
     * the server. */
    return cli_name(nobody)[0];
  }

  if (!ircd_strcmp(what, "SPIN")) {
    volatile unsigned long i = 0;

    log_write(LS_SYSTEM, L_WARNING, 0,
              "isolated_demo: %s asked for a spin; obliging",
              cli_name(sptr));

    for (;;)
      i++;
  }

  sendcmdto_one(&me, MSG_NOTICE, TOK_NOTICE, sptr,
                "%C :isolated_demo: you are %s (%s@%s), account \"%s\"",
                sptr, cli_name(sptr),
                cli_user(sptr) ? cli_user(sptr)->username : "?",
                cli_user(sptr) ? cli_user(sptr)->host : "?",
                (cli_user(sptr) && IsAccount(sptr)) ? cli_user(sptr)->account
                                                    : "");

  sendcmdto_one(&me, MSG_NOTICE, TOK_NOTICE, sptr,
                "%C :isolated_demo: oper=%d local=%d secure=%d, "
                "NICKLEN=%d, %u nicks seen", sptr,
                IsAnOper(sptr) ? 1 : 0, MyConnect(sptr) ? 1 : 0,
                IsTLS(sptr) ? 1 : 0, feature_int(FEAT_NICKLEN), demo_nicks);

  return 0;
}

/** Count nicknames, to show a hook arriving from the other side. */
static enum HookResult demo_nick(struct HookContext* ctx, void* user)
{
  (void) user;

  demo_nicks++;

  log_write(LS_SYSTEM, L_INFO, 0, "isolated_demo: nick %s (%u so far)",
            ctx->hc_arg ? ctx->hc_arg : "?", demo_nicks);

  return HOOK_CONTINUE;
}

/** Refuse a nickname, to show a veto crossing a process boundary.
 *
 * Registration is the one hook point the server can suspend, which is
 * what makes a veto from another process possible at all: the server
 * holds the registration, asks, and resumes with the answer.  See
 * proposal 006 §5.5 and §7.7.
 */
static enum HookResult demo_register(struct HookContext* ctx, void* user)
{
  (void) user;

  if (ctx->hc_client && cli_name(ctx->hc_client)
      && !ircd_strncmp(cli_name(ctx->hc_client), "demo-no", 7)) {
    ircd_strncpy(ctx->hc_reason, "isolated_demo says no", TOPICLEN);
    return HOOK_DENY;
  }

  return HOOK_CONTINUE;
}

/** Set up on load.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int demo_init(struct ModuleHandle* mod)
{
  MessageHandler handlers[LAST_HANDLER_TYPE];
  int i;

  demo_mod = mod;

  for (i = 0; i < LAST_HANDLER_TYPE; i++)
    handlers[i] = 0;

  handlers[CLIENT_HANDLER] = demo_m_demo;
  handlers[OPER_HANDLER] = demo_m_demo;

  if (!module_add_command(mod, "DEMO", "DEMO", 2, 0, handlers))
    return -1;

  if (!module_add_hook(mod, HOOK_CLIENT_NICK_CHANGED, demo_nick,
                       HOOK_PRIORITY_DEFAULT, NULL))
    return -1;

  if (!module_add_hook(mod, HOOK_CLIENT_PRE_REGISTER, demo_register,
                       HOOK_PRIORITY_DEFAULT, NULL))
    return -1;

  log_write(LS_SYSTEM, L_INFO, 0, "isolated_demo: ready");

  return 0;
}

/** Tear down on unload.
 * @param[in] mod Handle for this module.
 */
static void demo_fini(struct ModuleHandle* mod)
{
  (void) mod;
  demo_mod = 0;
}

/** Re-read the configuration.
 * @param[in] mod Handle for this module.
 */
static void demo_rehash(struct ModuleHandle* mod)
{
  (void) mod;
  log_write(LS_SYSTEM, L_INFO, 0, "isolated_demo: rehashed");
}

/** Module description. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "isolated_demo",
  "1.0",
  "ircu2",
  "Reference module for isolation = \"process\"",
  demo_init,
  demo_fini,
  demo_rehash
};
