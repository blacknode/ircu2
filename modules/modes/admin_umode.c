#include "account.h"
#include "bot.h"
#include "client.h"
#include "hooks.h"
#include "ircd_log.h"
#include "module.h"

#define FLAG_ADMIN 'a'

static flag_t admin_mode;

int admin_mode_init(struct ModuleHandle *);
void admin_mode_finish(struct ModuleHandle *);
enum HookResult admin_mode_handle(struct HookContext *, void *);
void clear_admin_umode(char *buf);

void clear_admin_umode(char *buf) {
  assert(0 != buf);
  char *m = buf;
  char *b = buf;
  while (*m) {
    if (*m != FLAG_ADMIN)
      *b++ = *m;
    ++m;
  }
  *b = '\0';
}

enum HookResult admin_mode_handle(struct HookContext *ctx, void *sptr) {
  if (!ctx->hc_client || !ctx->hc_source)
    return HOOK_DENY;

  const char *mstr = (char *)ctx->hc_arg;
  short mode_add = 0;
  short mode_admin = 0;
  while (*mstr) {
    if (*mstr == '+')
      mode_add = 1;
    if (*mstr == '-')
      mode_add = 0;
    if (*mstr == FLAG_ADMIN)
      mode_admin = 1;
    ++mstr;
  }

  if (!mode_admin)
    return HOOK_CONTINUE;

  /* Only strip +a from a normal user's attempt to change its own modes.
   * Changes on another client have already passed the server/service-bot
   * dispatch path; a non-local target may also be in a burst. */
  if (ctx->hc_source == ctx->hc_client &&
      !(IsServer(ctx->hc_source) || IsLocalServiceBot(ctx->hc_source)))
    clear_admin_umode((char *)ctx->hc_arg);

  if (HasUFlag(ctx->hc_client, admin_mode) && mode_add)
    clear_admin_umode((char *)ctx->hc_arg);

  return HOOK_CONTINUE;
}

int admin_mode_init(struct ModuleHandle *mod) {
  if (!module_add_user_mode(mod, FLAG_ADMIN, &admin_mode))
    return -1;

  if (!module_add_hook(mod, HOOK_CLIENT_PRE_UMODE, admin_mode_handle,
                       HOOK_PRIORITY_DEFAULT, NULL)) {
    return -2;
  }

  return 0;
}

void admin_mode_finish(struct ModuleHandle *mod) {
  admin_mode = 0;
  module_del_user_mode(mod, FLAG_ADMIN);
}

struct ModuleInfo ircu_module = {
    IRCU_MODULE_ABI,
    "admin_umode",
    "1.0.0",
    "ircu developers",
    "Adds user mode +a: identify user as admin operator",
    admin_mode_init,
    admin_mode_finish,
    NULL,
};
