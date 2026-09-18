/*
 * IRC - Internet Relay Chat, modules/hooks/cmdaudit.c
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
 * @brief Records what one user does to another, and shows a command veto.
 *
 * The reference module for the command hooks.  It watches the commands
 * that are one user acting on another -- KICK, KILL, INVITE, MODE, GLINE,
 * SLINE, JUPE -- and writes a line to the log for each, using the subject
 * the server resolved rather than parsing parv for itself.  That is what
 * the hooks are for: a module that wanted this before would have had to
 * patch every m_*.c, and would have had to know that a KICK carries a nick
 * from a client and a numnick from a server.
 *
 * It also refuses one thing, so the veto path is exercised by something
 * real and not only by a test: a KICK aimed at a network service (+k) or
 * at an operator.  The core already protects the first; the second is a
 * policy several networks want and none of them can express without
 * patching m_kick.c, which is the point.  What matters here is the shape
 * of the veto, not the policy.
 */
#include "config.h"

#include "channel.h"
#include "client.h"
#include "hooks.h"
#include "ircd_log.h"
#include "ircd_string.h"
#include "module.h"
#include "numeric.h"

#include <string.h>

/** Commands this module watches.
 *
 * One registration each rather than one hook for everything: a hook that
 * named no command would be handed every line the server parses, including
 * all of the server-to-server traffic, to throw almost all of it away.
 */
static const char* const audit_commands[] = {
  "KICK", "KILL", "INVITE", "MODE", "GLINE", "SLINE", "JUPE", NULL
};

/** Refuse a KICK aimed at a protected user.
 * @param[in,out] ctx What the hook is judging.
 * @param[in] user Unused.
 * @return #HOOK_DENY for a protected victim, #HOOK_CONTINUE otherwise.
 */
static enum HookResult audit_pre(struct HookContext* ctx, void* user)
{
  (void) user;

  /* hc_client is the victim, already resolved: on the client path from a
   * nick and on the server path from a numnick, which is a distinction
   * this module never has to make.
   */
  if (!ctx->hc_client)
    return HOOK_CONTINUE;

  if (IsChannelService(ctx->hc_client)) {
    ctx->hc_numeric = ERR_ISCHANSERVICE;
    ircd_strncpy(ctx->hc_reason, "that user is a network service",
                 sizeof(ctx->hc_reason) - 1);
    return HOOK_DENY;
  }

  if (IsAnOper(ctx->hc_client)) {
    ctx->hc_numeric = ERR_CHANOPRIVSNEEDED;
    ircd_strncpy(ctx->hc_reason, "that user is an operator",
                 sizeof(ctx->hc_reason) - 1);
    return HOOK_DENY;
  }

  return HOOK_CONTINUE;
}

/** Write one line to the log for a command that ran.
 * @param[in] ctx What the hook is being told about.
 * @param[in] user Unused.
 * @return #HOOK_CONTINUE always; a notification cannot decide anything.
 */
static enum HookResult audit_post(struct HookContext* ctx, void* user)
{
  const struct HookCommand* cmd = ctx->hc_command;

  (void) user;

  log_write(LS_USER, L_INFO, 0, "%s: %s%s%s%s%s%s%s",
            cmd->hcc_cmd,
            ctx->hc_source ? cli_name(ctx->hc_source) : "?",
            ctx->hc_client ? " -> " : "",
            ctx->hc_client ? cli_name(ctx->hc_client) : "",
            ctx->hc_channel ? " on " : "",
            ctx->hc_channel ? ctx->hc_channel->chname : "",
            ctx->hc_arg ? ": " : "",
            ctx->hc_arg ? ctx->hc_arg : "");

  return HOOK_CONTINUE;
}

/** Register the hooks.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int cmdaudit_init(struct ModuleHandle* mod)
{
  int i;

  for (i = 0; audit_commands[i]; i++)
    if (!module_add_command_hook(mod, HOOK_COMMAND_POST, audit_commands[i],
                                 audit_post, HOOK_PRIORITY_DEFAULT, 0, 0))
      return -1;

  if (!module_add_command_hook(mod, HOOK_COMMAND_PRE, "KICK", audit_pre,
                               HOOK_PRIORITY_DEFAULT, 0, 0))
    return -1;

  return 0;
}

/** Module description. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "cmdaudit",
  "1.0",
  "ircu2",
  "logs what one user does to another, and refuses a KICK on a +k user",
  cmdaudit_init,
  0,
  0
};
