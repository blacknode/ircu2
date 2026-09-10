/*
 * IRC - Internet Relay Chat, modules/trace_events.c
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
 * @brief Logs every notification hook, as a reference and a debugging aid.
 *
 * This module vetoes nothing.  It attaches to each notification hook and
 * writes a line to the server log, which makes it useful both as a worked
 * example of hook registration and for checking that a hook point fires
 * with the data one expects.
 *
 * It is noisy by design.  Do not run it on a busy server.
 */
#include "config.h"

#include "channel.h"
#include "client.h"
#include "hooks.h"
#include "ircd_log.h"
#include "module.h"
#include "struct.h"

/** Log one event.
 *
 * The same callback serves every hook: the hook type arrives through the
 * @a user pointer that was supplied at registration.
 *
 * @param[in] ctx What happened.
 * @param[in] user The HookType this registration was for.
 * @return Always HOOK_CONTINUE; this module never decides anything.
 */
static enum HookResult trace_event(struct HookContext* ctx, void* user)
{
  enum HookType type = (enum HookType) (long) user;

  log_write(LS_SYSTEM, L_INFO, 0,
            "hook %s: client=%s source=%s channel=%s arg=%s",
            hook_type_name(type),
            ctx->hc_client ? cli_name(ctx->hc_client) : "-",
            ctx->hc_source ? cli_name(ctx->hc_source) : "-",
            ctx->hc_channel ? ctx->hc_channel->chname : "-",
            ctx->hc_arg ? ctx->hc_arg : "-");

  return HOOK_CONTINUE;
}

/** Every notification hook this module attaches to. */
static const enum HookType traced[] = {
  HOOK_CLIENT_REGISTERED,
  HOOK_CLIENT_NICK_CHANGED,
  HOOK_CLIENT_EXITING,
  HOOK_CHANNEL_JOINED,
  HOOK_CHANNEL_PARTED,
  HOOK_SERVER_LINKED
};

/** Attach to each traced hook.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int trace_init(struct ModuleHandle* mod)
{
  unsigned int i;

  for (i = 0; i < sizeof(traced) / sizeof(traced[0]); i++) {
    if (!module_add_hook(mod, traced[i], trace_event,
                         HOOK_PRIORITY_DEFAULT, (void*) (long) traced[i]))
      return -1;
  }

  return 0;
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "trace_events",
  "1.0.0",
  "ircu developers",
  "Logs every notification hook; noisy, for debugging",
  trace_init,
  NULL,           /* the loader detaches the hooks for us */
  NULL
};
