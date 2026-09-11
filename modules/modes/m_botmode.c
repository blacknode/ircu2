/*
 * IRC - Internet Relay Chat, modules/modes/m_botmode.c
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
 * @brief Adds user mode +B: this client is a bot run by the network.
 *
 * The mode is a label, not a policy: it says nothing about what the
 * client may do, only what it is, so that other code -- /WHOIS output,
 * a module that treats bots differently -- has one flag to look at.
 * modules/commands/m_bot.c sets it on every bot it creates by looking the letter
 * up with client_find_user_mode(), so the two modules share nothing but
 * the letter and this module loads first.
 *
 * What makes it a label worth trusting is that nobody can put it on
 * themselves.  The server registers module modes as settable by any local
 * user, exactly as asked, so the second half of this module is a
 * HOOK_CLIENT_PRE_UMODE hook that refuses every local attempt to set or
 * clear it.  Modules and the server itself write the flag directly and
 * never pass through that hook.
 */
#include "config.h"

#include "client.h"
#include "hooks.h"
#include "ircd_string.h"
#include "module.h"
#include "numeric.h"

#include <string.h>

/** The letter this module claims. */
#define BOT_MODE 'B'

/** Bit the server assigned to #BOT_MODE; zero until mi_init succeeds. */
static flag_t bot_flag;

/** Refuse any local MODE that mentions the bot letter.
 *
 * Both directions are refused: a user may no more shed the label than
 * claim it.  The hook sees the requested mode string before any of it is
 * applied, so nothing has to be undone.
 *
 * @param[in,out] ctx hc_client is the user, hc_arg the mode string.
 * @param[in] user Unused.
 * @return HOOK_DENY if the string contains the bot letter.
 */
static enum HookResult botmode_pre_umode(struct HookContext* ctx, void* user)
{
  (void) user;

  if (!bot_flag || !ctx->hc_arg || !strchr(ctx->hc_arg, BOT_MODE))
    return HOOK_CONTINUE;

  ctx->hc_numeric = ERR_NOPRIVILEGES;
  ircd_strncpy(ctx->hc_reason, "User mode +B is reserved for network bots",
               sizeof(ctx->hc_reason) - 1);
  ctx->hc_reason[sizeof(ctx->hc_reason) - 1] = '\0';

  return HOOK_DENY;
}

/** Claim the letter, then guard it.
 * @param[in] mod Handle for this module.
 * @return Zero on success, non-zero to refuse the load.
 */
static int botmode_init(struct ModuleHandle* mod)
{
  if (!module_add_user_mode(mod, BOT_MODE, &bot_flag))
    return -1;

  if (!module_add_hook(mod, HOOK_CLIENT_PRE_UMODE, botmode_pre_umode,
                       HOOK_PRIORITY_DEFAULT, NULL))
    return -1;

  return 0;
}

/** Forget the bit.
 *
 * The loader unregisters the mode and strips it from every user still
 * carrying it -- the bots included -- so there is nothing to undo here
 * beyond dropping a value a reload would hand out differently.
 *
 * @param[in] mod Handle for this module.
 */
static void botmode_fini(struct ModuleHandle* mod)
{
  (void) mod;
  bot_flag = 0;
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "m_botmode",
  "1.0.0",
  "ircu developers",
  "Adds user mode +B: marks a client as a bot run by the network",
  botmode_init,
  botmode_fini,
  NULL
};
