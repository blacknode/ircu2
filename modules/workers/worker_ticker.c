/*
 * IRC - Internet Relay Chat, modules/workers/worker_ticker.c
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
 * @brief A thread with a loop of its own; the reference for worker_spawn().
 *
 * The other shape a worker can take.  Where worker_demo.c submits a task
 * and gets an answer, this module starts a thread that stays up for as long
 * as the module is loaded, does its own waiting, and pushes results at the
 * main thread when it has them.
 *
 * What it actually does is trivial -- it counts seconds -- because the
 * interesting part is the shape, and the shape is the same one an embedded
 * HTTP server or a message-bus subscriber would have:
 *
 *   - it waits on a descriptor rather than sleeping, so that a stop is
 *     immediate instead of taking up to a tick;
 *   - the descriptor worker_stop_fd() hands it goes in the same select()
 *     set a real worker would put its listening socket in;
 *   - it never touches core state: what it wants to say, it says with
 *     worker_log(), and what it wants the server to do, it posts as a task
 *     whose wt_done runs in the main thread.
 *
 * Operators see a server notice every WORKER_TICKER_PERIOD seconds.  Load
 * it, watch it tick, /MODULE UNLOAD worker_ticker, and watch the thread go
 * away with it.
 */
#include "config.h"

#include "client.h"
#include "ircd.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "module.h"
#include "s_debug.h"
#include "send.h"
#include "worker.h"

#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/** Seconds between ticks. */
#define WORKER_TICKER_PERIOD 10

/** The thread, while it is running. */
static struct Worker* ticker_worker;

static void ticker_report(struct WorkTask* task);

/** What one tick carries across the boundary. */
struct TickerReport {
  unsigned long tr_tick;    /**< Which tick this is. */
  long          tr_uptime;  /**< Seconds since the thread started. */
};

/* ------------------------------------------------------------------------
 * The worker thread
 * ------------------------------------------------------------------------ */

/** Wait up to \a seconds, or until the worker is asked to stop.
 * @param[in] worker Worker doing the waiting.
 * @param[in] seconds How long to wait for.
 * @return Non-zero if the wait ran its course, zero if a stop interrupted it.
 */
static int ticker_wait(struct Worker* worker, int seconds)
{
  struct timeval tv;
  fd_set readers;
  int fd = worker_stop_fd(worker);

  /* A real worker would add its own sockets here.  select() rather than
   * poll() only because it is the one every supported system has without a
   * configure check; the module is free to use whatever it likes.
   */
  FD_ZERO(&readers);
  FD_SET(fd, &readers);

  tv.tv_sec = seconds;
  tv.tv_usec = 0;

  if (select(fd + 1, &readers, NULL, NULL, &tv) > 0)
    return 0;   /* the stop descriptor woke us */

  return !worker_stopping(worker);
}

/** The thread body.  Runs in its own thread until told to stop.
 * @param[in] worker This worker.
 * @param[in] arg Unused.
 */
static void ticker_main(struct Worker* worker, void* arg)
{
  time_t started = time(NULL);
  unsigned long tick = 0;

  (void) arg;

  /* time(NULL), not CurrentTime: the latter is a core global that the main
   * thread writes after every pass through the event engine.
   */
  worker_log("ticker: started");

  while (ticker_wait(worker, WORKER_TICKER_PERIOD)) {
    struct TickerReport* report;
    struct WorkTask* task;

    if (!(task = worker_task_new(NULL, ticker_report)))
      continue;

    if (!(report = (struct TickerReport*) worker_alloc(sizeof(*report)))) {
      worker_task_free(task);
      continue;
    }

    report->tr_tick = ++tick;
    report->tr_uptime = (long) (time(NULL) - started);

    task->wt_out = report;
    task->wt_out_len = sizeof(*report);

    /* Posting can fail if the main thread is behind and the queue is full.
     * Dropping a tick is the right answer; blocking here would only make
     * the backlog worse.
     */
    if (!worker_post(worker, task)) {
      worker_task_free(task);
      worker_log("ticker: dropped tick %lu, the reply queue is full", tick);
    }
  }

  worker_log("ticker: stopping after %lu tick%s", tick, tick == 1 ? "" : "s");
}

/* ------------------------------------------------------------------------
 * The main thread
 * ------------------------------------------------------------------------ */

/** Announce a tick.  Runs in the main thread, so this may talk to clients.
 * @param[in] task The posted task.
 */
static void ticker_report(struct WorkTask* task)
{
  const struct TickerReport* report =
    (const struct TickerReport*) task->wt_out;

  sendto_opmask_butone(0, SNO_OLDSNO,
                       "worker_ticker: tick %lu, thread up %ld seconds",
                       report->tr_tick, report->tr_uptime);
}

/** Start the thread.
 * @param[in] mod Handle for this module.
 * @return Zero on success, non-zero to refuse to load.
 */
static int ticker_init(struct ModuleHandle* mod)
{
  /* Called from mi_init, which may be running while the configuration file
   * is still being read -- before the server knows what WORKER_THREADS will
   * be.  module_spawn_worker() holds the request in that case and starts
   * the thread once the answer is known, so this needs no special handling.
   */
  ticker_worker = module_spawn_worker(mod, "ticker", ticker_main, NULL);

  if (!ticker_worker) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "worker_ticker: could not start a worker; is WORKER_THREADS 0?");
    return -1;
  }

  return 0;
}

/** Stop the thread.
 *
 * Not needed on unload: the server stops a module's workers before it
 * calls mi_fini, precisely so that nothing this function frees can still
 * be in use by a thread.  So by the time this runs the worker is already
 * gone and module_stop_worker() returns zero without doing anything --
 * which is why it is safe to keep the call, and why a module that stops
 * its thread at some other time (a rehash, say) uses the same one.
 *
 * @param[in] mod Handle for this module.
 */
static void ticker_fini(struct ModuleHandle* mod)
{
  if (ticker_worker) {
    module_stop_worker(mod, ticker_worker);
    ticker_worker = NULL;
  }
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "worker_ticker",
  "1.0.0",
  "ircu developers",
  "A dedicated worker thread that ticks; reference for worker_spawn()",
  ticker_init,
  ticker_fini,
  NULL
};
