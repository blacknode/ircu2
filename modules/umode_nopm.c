/*
 * IRC - Internet Relay Chat, modules/umode_nopm.c
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
 * @brief Adds user mode +P: only operators may send you a private message.
 *
 * The reference for registering a user mode from a module.  A mode is two
 * halves that have to be written together: the registration, which makes
 * the letter something /MODE will accept and the server will carry across
 * the network, and a hook that gives the letter a meaning.  A mode without
 * the hook is a flag nothing reads; a hook without the mode has no way for
 * a user to ask for it.
 *
 * The server assigns the bit.  A module that picked its own would collide
 * with the next module to pick the same one, and neither author would see
 * it happen -- the two modes would simply be the same flag.
 */
#include "config.h"

#include "client.h"
#include "hooks.h"
#include "ircd_string.h"
#include "module.h"

/** The letter this module claims. */
#define NOPM_MODE 'P'

/** Bit the server assigned to #NOPM_MODE.
 *
 * Zero until mi_init succeeds, which is why the hook checks it: a stray
 * call with no bit assigned would test flag zero and match every client.
 */
static flag_t nopm_flag;

/** Refuse a private message to a user who has the mode set.
 *
 * The hook runs on the sender's server, so it binds this server only; a
 * user connected elsewhere can still send the message unless that server
 * also has the module loaded.  That is true of every veto hook and is why
 * a rule meant to hold network-wide belongs in every server's
 * configuration.
 *
 * @param[in,out] ctx hc_client is the recipient, hc_source the sender.
 * @param[in] user Unused.
 * @return HOOK_DENY when the recipient is not accepting this message.
 */
static enum HookResult nopm_private(struct HookContext* ctx, void* user)
{
  (void) user;

  if (!nopm_flag || !ctx->hc_client || !ctx->hc_source)
    return HOOK_CONTINUE;

  /* HasUFlag() is how a module reads its own mode off a client; the bit
   * lives in the same word as the core's modes.
   */
  if (!HasUFlag(ctx->hc_client, nopm_flag))
    return HOOK_CONTINUE;

  /* Opers still get through, and so does a note to oneself. */
  if (IsAnOper(ctx->hc_source) || ctx->hc_source == ctx->hc_client)
    return HOOK_CONTINUE;

  /* No hc_numeric: the message path already has one that fits, and the
   * server uses it when a hook does not choose.
   */
  ircd_strncpy(ctx->hc_reason, "Is only accepting private messages from IRC "
               "operators", sizeof(ctx->hc_reason) - 1);
  ctx->hc_reason[sizeof(ctx->hc_reason) - 1] = '\0';

  return HOOK_DENY;
}

/** Claim the mode letter, then give it a meaning.
 *
 * Order matters only in that failing after a successful registration would
 * leave the mode registered; returning non-zero unloads the module, and
 * the loader takes the mode with it, so either way the server is left
 * consistent.
 *
 * @param[in] mod Handle for this module.
 * @return Zero on success, non-zero to refuse the load.
 */
static int umode_nopm_init(struct ModuleHandle* mod)
{
  /* Fails if the letter is already taken -- by the core, or by another
   * module that got there first.  There is nothing sensible to do but
   * refuse the load: the module cannot do its job under a letter its
   * users were not told about.
   */
  if (!module_add_user_mode(mod, NOPM_MODE, &nopm_flag))
    return -1;

  if (!module_add_hook(mod, HOOK_MESSAGE_PRE_PRIVATE, nopm_private,
                       HOOK_PRIORITY_DEFAULT, NULL))
    return -1;

  return 0;
}

/** Forget the bit the server handed out.
 *
 * The loader unregisters the mode and takes it off every user that had it,
 * so there is nothing to undo here.  What is worth doing is dropping our
 * copy of the bit: a reload would hand out a different one, and a stale
 * value in a static is the kind of thing that only misbehaves later.
 *
 * @param[in] mod Handle for this module.
 */
static void umode_nopm_fini(struct ModuleHandle* mod)
{
  (void) mod;
  nopm_flag = 0;
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "umode_nopm",
  "1.0.0",
  "ircu developers",
  "Adds user mode +P: only operators may send you a private message",
  umode_nopm_init,
  umode_nopm_fini,
  NULL
};
