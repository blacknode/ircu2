/* Test fixture: registers command hooks and leaves them for the loader. */
#include "hooks.h"
#include "module.h"

/** Set by the hooks when they run, so the test can tell they are live. */
extern int mod_cmdhook_pre_calls;
extern int mod_cmdhook_post_calls;

static enum HookResult cmdhook_pre(struct HookContext* ctx, void* user)
{
  (void) ctx; (void) user;
  mod_cmdhook_pre_calls++;
  return HOOK_CONTINUE;
}

static enum HookResult cmdhook_post(struct HookContext* ctx, void* user)
{
  (void) ctx; (void) user;
  mod_cmdhook_post_calls++;
  return HOOK_CONTINUE;
}

static int cmdhook_init(struct ModuleHandle* mod)
{
  if (!module_add_command_hook(mod, HOOK_COMMAND_PRE, "KICK", cmdhook_pre,
                               HOOK_PRIORITY_DEFAULT, 0, 0))
    return -1;

  /* The same callback on a second command: the name is part of what
   * identifies a registration, so this is not a duplicate.
   */
  if (!module_add_command_hook(mod, HOOK_COMMAND_PRE, "KILL", cmdhook_pre,
                               HOOK_PRIORITY_DEFAULT, 0, 0))
    return -1;

  /* Deliberately NOT removed in mi_fini: the loader must revert it. */
  if (!module_add_command_hook(mod, HOOK_COMMAND_POST, 0, cmdhook_post,
                               HOOK_PRIORITY_DEFAULT, 0, 0))
    return -1;

  /* A lifecycle hook point is not a command hook point. */
  if (module_add_command_hook(mod, HOOK_CLIENT_REGISTERED, "KICK",
                              cmdhook_pre, HOOK_PRIORITY_DEFAULT, 0, 0))
    return -1;

  return 0;
}

struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI, "mod_cmdhook", "1.0", "test",
  "registers command hooks", cmdhook_init, 0, 0
};
