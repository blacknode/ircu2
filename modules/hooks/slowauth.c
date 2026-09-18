/*
 * IRC - Internet Relay Chat, modules/hooks/slowauth.c
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
 * @brief Holds a client's registration while it asks something slow.
 *
 * The reference module for suspending a hook.  It answers one question --
 * may this nickname connect? -- and it answers it the way a real module
 * would have to: not in the hook, but on a worker thread, with the client
 * held in the meantime.
 *
 * The question here is a strcmp against a list compiled in, and the work
 * is a sleep.  That is the whole point: a lookup in a database, an HTTP
 * call to an identity provider and an Argon2 verification are all "a
 * blocking call that takes milliseconds", and milliseconds on the main
 * thread are milliseconds in which nobody else on the server is served.
 * The sleep stands in for them so the shape of the thing is visible
 * without a database to set up.
 *
 * The shape is four steps:
 *
 *   1. The hook reads HookContext::hc_token.  Non-zero means this call
 *      site can wait; zero means it cannot, and the module must decide now
 *      or not at all.
 *   2. It copies what the worker needs -- never a struct Client, never a
 *      pointer into the context, both of which may be gone by the time the
 *      worker runs -- and returns #HOOK_PENDING.
 *   3. The worker does the slow thing, touching nothing of the core's.
 *   4. The completion callback, back in the main thread, calls
 *      module_hook_resume() with the token.
 *
 * What happens if step 4 never comes is not this module's problem, which
 * is the other half of the design: the server refuses the operation when
 * FEAT_HOOK_TIMEOUT passes, and again if the module is unloaded while it
 * owes an answer.
 */
#include "config.h"

#include "client.h"
#include "hooks.h"
#include "ircd_log.h"
#include "ircd_string.h"
#include "module.h"
#include "worker.h"

#include <string.h>
#include <time.h>

/** How long the imaginary lookup takes, in milliseconds. */
#define SLOWAUTH_DELAY_MS 250

/** Nicknames this module refuses.  Stands in for a query. */
static const char* const slowauth_denied[] = {
  "root", "admin", "operator", NULL
};

/** This module's handle, kept because module_hook_resume() asks for it and
 * the completion callback has nowhere else to get it from.
 */
static struct ModuleHandle* slowauth_mod;

/** What crosses to the worker thread and back.
 *
 * A numnick would be the right way to name the client (see
 * worker_task_set_client()); this module does not need the client at all,
 * because the answer travels back by token, and the token is the server's
 * to resolve.
 */
struct SlowAuthWork {
  hook_token_t sw_token;              /**< Answer this. */
  char         sw_nick[NICKLEN + 1];  /**< Copy of the nick, not a pointer. */
  int          sw_deny;               /**< Set by the worker. */
};

/** The slow part.  Runs in a worker thread.
 *
 * Obeys the one rule: no core state.  Everything it reads is in the task,
 * everything it writes is in the task, and it does not log, allocate with
 * MyMalloc(), or look at CurrentTime.
 *
 * @param[in,out] task The work, with SlowAuthWork in WorkTask::wt_in.
 */
static void slowauth_work(struct WorkTask* task)
{
  struct SlowAuthWork* work = (struct SlowAuthWork*) task->wt_in;
  struct timespec delay;
  int i;

  delay.tv_sec = SLOWAUTH_DELAY_MS / 1000;
  delay.tv_nsec = (SLOWAUTH_DELAY_MS % 1000) * 1000000L;
  nanosleep(&delay, 0);

  for (i = 0; slowauth_denied[i]; i++) {
    if (0 == strcasecmp(work->sw_nick, slowauth_denied[i])) {
      work->sw_deny = 1;
      break;
    }
  }
}

/** The answer.  Runs in the main thread once the worker is done.
 * @param[in] task The finished work.
 */
static void slowauth_done(struct WorkTask* task)
{
  struct SlowAuthWork* work = (struct SlowAuthWork*) task->wt_in;

  /* Resuming a token the server no longer holds -- because the client
   * left, or because the deadline passed while the worker was busy -- is
   * not an error: it returns zero and does nothing.
   */
  module_hook_resume(slowauth_mod, work->sw_token,
                     work->sw_deny ? HOOK_DENY : HOOK_ALLOW,
                     work->sw_deny ? "That nickname is reserved" : 0);
}

/** Ask, and hold the client while the answer is found.
 * @param[in,out] ctx What the hook is judging.
 * @param[in] user Unused.
 * @return #HOOK_PENDING if the client is held, #HOOK_CONTINUE otherwise.
 */
static enum HookResult slowauth_pre_register(struct HookContext* ctx,
                                             void* user)
{
  struct SlowAuthWork* work;
  struct WorkTask* task;

  (void) user;

  /* Zero means this hook point cannot wait.  A module that returned
   * HOOK_PENDING anyway would be logged and read as HOOK_CONTINUE, so
   * saying so here is the difference between a policy that is not applied
   * and a policy that is not applied *quietly*.
   */
  if (!ctx->hc_token) {
    log_write(LS_SYSTEM, L_WARNING, 0,
              "slowauth: CLIENT_PRE_REGISTER cannot be suspended here; "
              "letting %s in unchecked",
              ctx->hc_client ? cli_name(ctx->hc_client) : "?");
    return HOOK_CONTINUE;
  }

  task = worker_task_new(slowauth_work, slowauth_done);
  if (!task)
    return HOOK_CONTINUE;

  work = (struct SlowAuthWork*) worker_alloc(sizeof(*work));
  if (!work) {
    worker_task_free(task);
    return HOOK_CONTINUE;
  }

  work->sw_token = ctx->hc_token;
  ircd_strncpy(work->sw_nick, ctx->hc_client ? cli_name(ctx->hc_client) : "",
               NICKLEN);
  task->wt_in = work;

  /* Worker threads are off by default, and a module that assumes otherwise
   * is a module that holds every registration on a server that will never
   * answer.  Letting the client in is the right failure here: this is a
   * policy module, and the server is not configured to run its policy.
   */
  if (!module_submit_work(slowauth_mod, task)) {
    log_write(LS_SYSTEM, L_WARNING, 0,
              "slowauth: no worker threads (FEAT_WORKER_THREADS is zero); "
              "letting %s in unchecked", work->sw_nick);
    worker_task_free(task);
    return HOOK_CONTINUE;
  }

  return HOOK_PENDING;
}

/** Register the hook.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int slowauth_init(struct ModuleHandle* mod)
{
  slowauth_mod = mod;

  if (!module_add_hook(mod, HOOK_CLIENT_PRE_REGISTER, slowauth_pre_register,
                       HOOK_PRIORITY_DEFAULT, 0))
    return -1;

  return 0;
}

/** Forget the handle.
 *
 * Whatever this module was still holding has already been refused by then:
 * unloading it takes its hooks, its queued work and its outstanding
 * answers with it, in that order, before this runs.
 *
 * @param[in] mod Handle for this module.
 */
static void slowauth_fini(struct ModuleHandle* mod)
{
  (void) mod;
  slowauth_mod = 0;
}

/** Module description. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "slowauth",
  "1.0",
  "ircu2",
  "holds a registration while a worker thread answers for it",
  slowauth_init,
  slowauth_fini,
  0
};
