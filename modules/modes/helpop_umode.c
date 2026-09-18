#include "account.h"
#include "bot.h"
#include "client.h"
#include "hooks.h"
#include "ircd_log.h"
#include "module.h"

#define FLAG_HELPOP 'h'

static flag_t helpop_mode;

int helpop_mode_init(struct ModuleHandle *);
void helpop_mode_finish(struct ModuleHandle *);
enum HookResult helpop_mode_handle(struct HookContext *, void *);
void clear_helpop_umode(char *buf);

void clear_helpop_umode(char *buf) {
  assert(0 != buf);
  char *m = buf;
  char *b = buf;
  while (*m) {
    if (*m != FLAG_HELPOP)
      *b++ = *m;
    ++m;
  }
  *b = '\0';
}

enum HookResult helpop_mode_handle(struct HookContext *ctx, void *sptr) {
  if (!ctx->hc_client || !ctx->hc_source)
    return HOOK_DENY;

  const char *mstr = (char *)ctx->hc_arg;
  short mode_add = 0;
  short mode_helpop = 0;
  while (*mstr) {
    if (*mstr == '+')
      mode_add = 1;
    if (*mstr == '-')
      mode_add = 0;
    if (*mstr == FLAG_HELPOP)
      mode_helpop = 1;
    ++mstr;
  }

  if (!mode_helpop)
    return HOOK_CONTINUE;

  /* Only strip +h from a normal user's attempt to change its own modes.
   * Changes on another client have already passed the server/service-bot
   * dispatch path; a non-local target may also be in a burst. */
  if (ctx->hc_source == ctx->hc_client &&
      !(IsServer(ctx->hc_source) || IsLocalServiceBot(ctx->hc_source)))
    clear_helpop_umode((char *)ctx->hc_arg);

  if (HasUFlag(ctx->hc_client, helpop_mode) && mode_add)
    clear_helpop_umode((char *)ctx->hc_arg);

  return HOOK_CONTINUE;
}

int helpop_mode_init(struct ModuleHandle *mod) {
  if (!module_add_user_mode(mod, FLAG_HELPOP, &helpop_mode))
    return -1;

  if (!module_add_hook(mod, HOOK_CLIENT_PRE_UMODE, helpop_mode_handle,
                       HOOK_PRIORITY_DEFAULT, NULL)) {
    return -2;
  }

  return 0;
}

void helpop_mode_finish(struct ModuleHandle *mod) {
  helpop_mode = 0;
  module_del_user_mode(mod, FLAG_HELPOP);
}

struct ModuleInfo ircu_module = {
    IRCU_MODULE_ABI,
    "helpop_umode",
    "1.0.0",
    "ircu developers",
    "Adds user mode +h: identify user as help operator",
    helpop_mode_init,
    helpop_mode_finish,
    NULL,
};
