/*
 * IRC - Internet Relay Chat, modules/nickpolicy.c
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
 * @brief Refuses nicknames that match a policy, as a worked veto example.
 *
 * The policy here is deliberately simple so the mechanism is what stands
 * out: a nick may not be all digits, and may not contain a substring from
 * a small blocklist.  Replace check_nick() with whatever your network
 * needs.
 *
 * This is the shape every veto hook takes: look at the context, return
 * HOOK_CONTINUE to abstain, or fill in hc_numeric and hc_reason and return
 * HOOK_DENY to refuse.
 */
#include "config.h"

#include "client.h"
#include "hooks.h"
#include "ircd_log.h"
#include "ircd_string.h"
#include "module.h"
#include "numeric.h"

#include <ctype.h>
#include <string.h>

/** Substrings no nickname may contain, compared case-insensitively. */
static const char* const blocked[] = {
  "admin",
  "operator",
  "services",
  NULL
};

/** Decide whether a nickname is acceptable.
 * @param[in] nick Nickname the client is asking for.
 * @return NULL if it is fine, otherwise the reason to refuse it.
 */
static const char* check_nick(const char* nick)
{
  const char* p;
  int i;

  /* All digits: too easily confused with a numeric. */
  for (p = nick; *p; p++)
    if (!isdigit((unsigned char) *p))
      break;
  if (*p == '\0' && p != nick)
    return "Nicknames may not be all digits";

  for (i = 0; blocked[i]; i++) {
    const char* n;

    /* A case-insensitive substring search; ircd_strcmp is whole-string. */
    for (n = nick; *n; n++) {
      if (0 == ircd_strncmp(n, blocked[i], strlen(blocked[i])))
        return "That nickname is reserved on this network";
    }
  }

  return NULL;
}

/** Veto hook for nickname changes.
 * @param[in,out] ctx hc_arg holds the nickname being asked for.
 * @param[in] user Unused.
 * @return HOOK_DENY to refuse the nick, HOOK_CONTINUE to abstain.
 */
static enum HookResult nickpolicy_pre_nick(struct HookContext* ctx, void* user)
{
  const char* reason;

  (void) user;

  if (!ctx->hc_arg)
    return HOOK_CONTINUE;

  reason = check_nick(ctx->hc_arg);
  if (!reason)
    return HOOK_CONTINUE;

  /* Both fields are optional: leaving hc_numeric at zero lets the server
   * pick its own, and an empty hc_reason falls back to the nickname.
   */
  ctx->hc_numeric = ERR_ERRONEUSNICKNAME;
  ircd_strncpy(ctx->hc_reason, reason, sizeof(ctx->hc_reason) - 1);
  ctx->hc_reason[sizeof(ctx->hc_reason) - 1] = '\0';

  return HOOK_DENY;
}

/** Attach the hook.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int nickpolicy_init(struct ModuleHandle* mod)
{
  if (!module_add_hook(mod, HOOK_CLIENT_PRE_NICK, nickpolicy_pre_nick,
                       HOOK_PRIORITY_DEFAULT, NULL))
    return -1;

  return 0;
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "nickpolicy",
  "1.0.0",
  "ircu developers",
  "Refuses nicknames that break a local policy",
  nickpolicy_init,
  NULL,           /* the loader detaches the hook for us */
  NULL
};
