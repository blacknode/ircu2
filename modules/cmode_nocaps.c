/*
 * IRC - Internet Relay Chat, modules/cmode_nocaps.c
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
 * @brief Adds channel mode +G: no shouting in this channel.
 *
 * The reference for registering a channel mode from a module, and the
 * counterpart of modules/umode_nopm.c.  A mode is two halves that have to
 * be written together: the registration, which makes the letter something
 * /MODE will accept and the server will carry across the network, and a
 * hook that gives the letter a meaning.  A mode without the hook is a flag
 * nothing reads; a hook without the mode has no way for anyone to ask for
 * it -- which is the difference between this module and modules/nocaps.c,
 * whose rule applies to every channel because there is no letter to turn
 * it on with.
 *
 * The bit is not handed out here.  It follows from the letter, so 'G' is
 * the same bit on every server that loads this module, and two servers
 * built from the same sources never have to agree on anything.
 */
#include "config.h"

#include "channel.h"
#include "client.h"
#include "hooks.h"
#include "ircd_string.h"
#include "module.h"
#include "numeric.h"

#include <ctype.h>
#include <string.h>

/** The letter this module claims. */
#define NOCAPS_MODE 'G'

/** Messages shorter than this are never judged: "OK" is not shouting. */
#define NOCAPS_MIN_LENGTH 10

/** Percentage of letters that must be capitals to count as shouting. */
#define NOCAPS_THRESHOLD 70

/** Bit #NOCAPS_MODE maps to.
 *
 * Zero until mi_init succeeds, which is why the hook checks it: a stray
 * call with no bit would test mask zero and match every channel.
 */
static chanmode_t nocaps_flag;

/** Decide whether a line counts as shouting.
 * @param[in] text Message body.
 * @return Non-zero if it is mostly capitals.
 */
static int is_shouting(const char* text)
{
  size_t upper = 0;
  size_t alpha = 0;
  const char* p;

  if (strlen(text) < NOCAPS_MIN_LENGTH)
    return 0;

  for (p = text; *p; p++) {
    if (!isalpha((unsigned char) *p))
      continue;
    alpha++;
    if (isupper((unsigned char) *p))
      upper++;
  }

  /* A line with almost no letters cannot be shouting. */
  if (alpha < NOCAPS_MIN_LENGTH)
    return 0;

  return (upper * 100 / alpha) > NOCAPS_THRESHOLD;
}

/** Refuse a shouted message to a channel that asked for the mode.
 *
 * The hook runs on the sender's server, so it binds this server only; a
 * user connected elsewhere can still shout unless that server has the
 * module loaded too.  That is true of every veto hook, and is why a rule
 * meant to hold network-wide belongs in every server's configuration.
 *
 * @param[in,out] ctx hc_channel is the channel, hc_arg the message body.
 * @param[in] user Unused.
 * @return HOOK_DENY when the channel is +G and the line is shouting.
 */
static enum HookResult nocaps_channel(struct HookContext* ctx, void* user)
{
  (void) user;

  if (!nocaps_flag || !ctx->hc_channel || !ctx->hc_arg)
    return HOOK_CONTINUE;

  /* HasCFlag() is how a module reads its own mode off a channel; the bit
   * lives in the same mask as the core's modes.
   */
  if (!HasCFlag(ctx->hc_channel, nocaps_flag))
    return HOOK_CONTINUE;

  if (!is_shouting(ctx->hc_arg))
    return HOOK_CONTINUE;

  ctx->hc_numeric = ERR_CANNOTSENDTOCHAN;
  ircd_strncpy(ctx->hc_reason, "Turn the volume down",
               sizeof(ctx->hc_reason) - 1);
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
static int cmode_nocaps_init(struct ModuleHandle* mod)
{
  /* Fails if the letter is already taken -- by the core, or by another
   * module that got there first.  There is nothing sensible to do but
   * refuse the load: the module cannot do its job under a letter its
   * users were not told about.
   */
  if (!module_add_chan_mode(mod, NOCAPS_MODE, &nocaps_flag))
    return -1;

  if (!module_add_hook(mod, HOOK_MESSAGE_PRE_CHANNEL, nocaps_channel,
                       HOOK_PRIORITY_DEFAULT, NULL))
    return -1;

  return 0;
}

/** Forget the bit.
 *
 * The loader unregisters the mode and takes it off every channel that had
 * it, so there is nothing to undo here.  What is worth doing is dropping
 * our copy of the bit: a stale value in a static is the kind of thing that
 * only misbehaves later.
 *
 * @param[in] mod Handle for this module.
 */
static void cmode_nocaps_fini(struct ModuleHandle* mod)
{
  (void) mod;
  nocaps_flag = 0;
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "cmode_nocaps",
  "1.0.0",
  "ircu developers",
  "Adds channel mode +G: refuses shouted messages to the channel",
  cmode_nocaps_init,
  cmode_nocaps_fini,
  NULL
};
