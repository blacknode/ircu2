/*
 * IRC - Internet Relay Chat, modules/nocaps.c
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
 * @brief Deals with shouting, showing both ways a message hook can act.
 *
 * A channel message that is mostly capitals is refused outright.  A
 * private message that is mostly capitals is lowercased instead of
 * refused, which demonstrates rewriting.
 *
 * Two different responses to the same condition, on purpose: refusing and
 * rewriting are the two things a message hook can do, and having one of
 * each in the reference module means both paths are exercised.
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

/** Messages shorter than this are never judged: "OK" is not shouting. */
#define NOCAPS_MIN_LENGTH 10

/** Percentage of letters that must be capitals to count as shouting. */
#define NOCAPS_THRESHOLD 70

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

/** Refuse a shouted channel message.
 * @param[in,out] ctx hc_arg holds the message body.
 * @param[in] user Unused.
 * @return HOOK_DENY when the message is shouting.
 */
static enum HookResult nocaps_channel(struct HookContext* ctx, void* user)
{
  (void) user;

  if (!ctx->hc_arg || !is_shouting(ctx->hc_arg))
    return HOOK_CONTINUE;

  ctx->hc_numeric = ERR_CANNOTSENDTOCHAN;
  ircd_strncpy(ctx->hc_reason, "Turn the volume down",
               sizeof(ctx->hc_reason) - 1);
  ctx->hc_reason[sizeof(ctx->hc_reason) - 1] = '\0';

  return HOOK_DENY;
}

/** Lowercase a shouted private message rather than refusing it.
 * @param[in,out] ctx hc_arg holds the body; hc_rewrite is where a new one
 *   goes.
 * @param[in] user Unused.
 * @return Always HOOK_CONTINUE; rewriting is not a veto.
 */
static enum HookResult nocaps_private(struct HookContext* ctx, void* user)
{
  size_t i;

  (void) user;

  if (!ctx->hc_arg || !is_shouting(ctx->hc_arg))
    return HOOK_CONTINUE;

  /* The server owns the buffer and tells us how big it is.  Writing our
   * own pointer here instead would leave the server reading memory this
   * module may unmap.
   */
  if (!ctx->hc_rewrite || ctx->hc_rewrite_len == 0)
    return HOOK_CONTINUE;

  for (i = 0; i + 1 < ctx->hc_rewrite_len && ctx->hc_arg[i]; i++)
    ctx->hc_rewrite[i] = tolower((unsigned char) ctx->hc_arg[i]);
  ctx->hc_rewrite[i] = '\0';

  ctx->hc_rewritten = 1;

  return HOOK_CONTINUE;
}

/** Attach both hooks.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int nocaps_init(struct ModuleHandle* mod)
{
  if (!module_add_hook(mod, HOOK_MESSAGE_PRE_CHANNEL, nocaps_channel,
                       HOOK_PRIORITY_DEFAULT, NULL))
    return -1;

  if (!module_add_hook(mod, HOOK_MESSAGE_PRE_PRIVATE, nocaps_private,
                       HOOK_PRIORITY_DEFAULT, NULL))
    return -1;

  return 0;
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "nocaps",
  "1.0.0",
  "ircu developers",
  "Refuses shouted channel messages and lowercases shouted private ones",
  nocaps_init,
  NULL,           /* the loader detaches the hooks for us */
  NULL
};
