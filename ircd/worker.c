/*
 * IRC - Internet Relay Chat, ircd/worker.c
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
 * @brief Worker threads and the queues that connect them to the main thread.
 *
 * Three queues, all guarded by one mutex:
 *
 *   - @c wi_pending  tasks waiting for a pool thread to pick them up.
 *   - @c wi_running  tasks a pool thread is working on right now.  Kept as a
 *                    list rather than a counter because unloading a module
 *                    has to wait for exactly its own tasks.
 *   - @c wi_ready    tasks that are finished and waiting for the main thread.
 *
 * One mutex for all three is not a bottleneck: the critical sections are a
 * pointer swap each, and the interesting work happens outside them.  The
 * main thread never blocks on the mutex for longer than a list splice --
 * except when it is deliberately waiting, which is only ever on a module
 * unload or a shutdown.
 *
 * The main thread learns that @c wi_ready is non-empty through a self-pipe
 * registered as an ordinary Socket, the same trick ircd_events.c already
 * uses for signals.  The engines needed no changes.
 *
 * @section worker_c_rule What runs where
 *
 * Functions in this file are labelled in their doc comment with the thread
 * they run in.  The ones a worker thread may call touch nothing but this
 * file's own state and the system allocator; see include/worker.h.
 */
#include "config.h"

#include "worker.h"

#include "client.h"
#include "ircd_events.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_osdep.h"
#include "ircd_string.h"
#include "numnicks.h"
#include "s_debug.h"
#include "struct.h"

/* #include's that are the point of the file, not an accident: */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** How long to wait between complaints while blocked on a stuck worker. */
#define WORKER_STALL_WARN 2

/** A thread in the pool. */
struct PoolThread {
  pthread_t     pt_thread;   /**< The thread itself. */
  unsigned int  pt_index;    /**< Its slot; it exits when this is >= target. */
  int           pt_live;     /**< Non-zero between create and join. */
};

/** A thread with a loop of its own. */
struct Worker {
  struct Worker*       w_next;     /**< Next in #wi_workers. */
  char                 w_name[WORKER_NAMELEN + 1]; /**< For logs and stats. */
  pthread_t            w_thread;   /**< The thread itself. */
  WorkerMainFn         w_main;     /**< Its body. */
  void*                w_arg;      /**< Handed to w_main. */
  struct ModuleHandle* w_owner;    /**< Module that spawned it, or NULL. */
  int                  w_stop;     /**< Stop requested.  Under the mutex. */
  int                  w_stop_r;   /**< Read end of the stop pipe. */
  int                  w_stop_w;   /**< Write end of the stop pipe. */
  int                  w_started;  /**< Thread created (vs. still pending). */
};

/** Everything the worker subsystem owns. */
static struct {
  int               wi_up;         /**< Queues and pipe exist. */
  int               wi_configured; /**< worker_init() has run. */
  int               wi_enabled;    /**< Cached: FEAT_WORKER_THREADS > 0. */
  unsigned int      wi_queue_max;  /**< Cached: FEAT_WORKER_QUEUE_MAX. */

  pthread_mutex_t   wi_mutex;      /**< Guards everything below. */
  pthread_cond_t    wi_work;       /**< Pool threads wait for a task. */
  pthread_cond_t    wi_idle;       /**< Main thread waits for tasks to end. */

  struct WorkTask*  wi_pending;    /**< Head of the pending queue. */
  struct WorkTask** wi_pending_tail; /**< Where the next one is appended. */
  unsigned int      wi_pending_n;  /**< Length of the pending queue. */

  struct WorkTask*  wi_running;    /**< Tasks in a worker thread now. */
  unsigned int      wi_running_n;  /**< How many of those. */

  struct WorkTask*  wi_ready;      /**< Head of the reply queue. */
  struct WorkTask** wi_ready_tail; /**< Where the next one is appended. */
  unsigned int      wi_ready_n;    /**< Length of the reply queue. */

  unsigned int      wi_outstanding; /**< pending + running + ready. */

  struct PoolThread wi_pool[WORKER_MAX_THREADS]; /**< The pool. */
  unsigned int      wi_nthreads;   /**< Threads alive in the pool. */
  unsigned int      wi_target;     /**< Threads there should be. */

  struct Worker*    wi_workers;    /**< Dedicated workers. */
  unsigned int      wi_nworkers;   /**< How many of them. */

  int               wi_wake_r;     /**< Read end of the wake-up pipe. */
  int               wi_wake_w;     /**< Write end of the wake-up pipe. */
  int               wi_wake_armed; /**< A byte is already in the pipe. */
  struct Socket     wi_wake_sock;  /**< Read end, as the engine sees it. */

  unsigned int      wi_submitted;  /**< Tasks accepted, ever. */
  unsigned int      wi_completed;  /**< Tasks delivered, ever. */
  unsigned int      wi_rejected;   /**< Submissions refused, ever. */
} wInfo;

static void* worker_pool_main(void* arg);
static void* worker_dedicated_main(void* arg);
static void worker_start_pending(void);

/** One-time setup of the mutex, the condition variables and the queue tails.
 *
 * Done through pthread_once() rather than from worker_init() because the
 * order is not ours to choose: a module loaded from a Module{} block runs
 * its mi_init while the configuration file is still being read, which is
 * before worker_init(), and it may already be asking for a worker.
 *
 * None of this creates a thread, a queue or a descriptor, so it does not
 * cost a server that never turns workers on anything but the memory the
 * structure already occupies.
 */
static pthread_once_t wi_once = PTHREAD_ONCE_INIT;

static void worker_once(void)
{
  pthread_mutex_init(&wInfo.wi_mutex, 0);
  pthread_cond_init(&wInfo.wi_work, 0);
  pthread_cond_init(&wInfo.wi_idle, 0);

  wInfo.wi_pending_tail = &wInfo.wi_pending;
  wInfo.wi_ready_tail = &wInfo.wi_ready;
  wInfo.wi_wake_r = wInfo.wi_wake_w = -1;
}

/** Make sure worker_once() has run.  Any thread, any number of times. */
#define worker_prepare()  pthread_once(&wi_once, worker_once)

/* ------------------------------------------------------------------------
 * Memory.  Workers use the system allocator; see include/worker.h.
 * ------------------------------------------------------------------------ */

/** Allocate zeroed memory.  Any thread. */
void* worker_alloc(size_t size)
{
  return calloc(1, size ? size : 1);
}

/** Release memory from worker_alloc().  Any thread. */
void worker_free(void* ptr)
{
  free(ptr);
}

/* ------------------------------------------------------------------------
 * Queue plumbing.  Every function here is called with wi_mutex held.
 * ------------------------------------------------------------------------ */

/** Append \a task to a singly-linked queue.  Mutex held. */
static void queue_push(struct WorkTask*** tail_p, struct WorkTask* task)
{
  task->wt_next = 0;
  **tail_p = task;
  *tail_p = &task->wt_next;
}

/** Take the head off the pending queue, or NULL.  Mutex held. */
static struct WorkTask* pending_pop(void)
{
  struct WorkTask* task = wInfo.wi_pending;

  if (!task)
    return 0;

  wInfo.wi_pending = task->wt_next;
  if (!wInfo.wi_pending)
    wInfo.wi_pending_tail = &wInfo.wi_pending;
  wInfo.wi_pending_n--;
  task->wt_next = 0;

  return task;
}

/** Make the main thread's engine notice the reply queue.  Mutex held.
 *
 * Coalesced: one byte is enough to wake the engine however many tasks are
 * waiting, and writing more only risks filling the pipe.  The byte is
 * cleared by the drain, not here.
 */
static void wake_main(void)
{
  unsigned char c = 0;

  if (wInfo.wi_wake_armed || wInfo.wi_wake_w < 0)
    return;

  /* A short write means the pipe is full, which means the main thread has
   * a wake-up coming already.  Either way there is nothing to recover.
   */
  if (write(wInfo.wi_wake_w, &c, 1) == 1)
    wInfo.wi_wake_armed = 1;
}

/** Put a finished task on the reply queue and wake the main thread.
 * Mutex held.  Called from a worker thread.
 */
static void ready_push(struct WorkTask* task)
{
  queue_push(&wInfo.wi_ready_tail, task);
  wInfo.wi_ready_n++;
  wake_main();
}

/** Unlink \a task from the running list.  Mutex held. */
static void running_unlink(struct WorkTask* task)
{
  struct WorkTask** p;

  for (p = &wInfo.wi_running; *p; p = &(*p)->wt_next) {
    if (*p == task) {
      *p = task->wt_next;
      task->wt_next = 0;
      wInfo.wi_running_n--;
      return;
    }
  }
}

/* ------------------------------------------------------------------------
 * Tasks
 * ------------------------------------------------------------------------ */

/** Allocate a zeroed task.  Main thread, or a worker submitting more work. */
struct WorkTask* worker_task_new(WorkFn work, WorkDoneFn done)
{
  struct WorkTask* task = (struct WorkTask*) worker_alloc(sizeof(*task));

  if (!task)
    return 0;

  task->wt_work = work;
  task->wt_done = done;

  return task;
}

/** Release a task's payload and then the task.  Main thread.
 *
 * The default release is worker_free() on both buffers, which is right for
 * anything worker_alloc() produced; wt_free replaces it entirely, so a task
 * that supplies one is responsible for both buffers.
 */
static void task_destroy(struct WorkTask* task)
{
  if (!task)
    return;

  if (task->wt_free)
    (*task->wt_free)(task);
  else {
    worker_free(task->wt_in);
    worker_free(task->wt_out);
  }

  worker_free(task);
}

/** Release a task the server does not own.  Main thread. */
void worker_task_free(struct WorkTask* task)
{
  task_destroy(task);
}

/** Remember which client asked for this work.  Main thread. */
void worker_task_set_client(struct WorkTask* task, const struct Client* cptr)
{
  size_t len;

  assert(0 != task);

  if (!cptr || !cli_user(cptr)) {
    task->wt_client[0] = '\0';
    task->wt_client_born = 0;
    return;
  }

  /* A numnick is the user's server numeric followed by the user's own, two
   * NUL-terminated fields of at most two and three characters.  Assembled by
   * hand rather than with ircd_snprintf(), which would be the whole
   * formatting engine for a concatenation of two short strings.
   */
  ircd_strncpy(task->wt_client, cli_yxx(cli_user(cptr)->server),
               WORKER_NUMNICKLEN);
  len = strlen(task->wt_client);
  ircd_strncpy(task->wt_client + len, cli_yxx(cptr),
               WORKER_NUMNICKLEN - len);

  task->wt_client_born = cli_firsttime(cptr);
}

/** The client this work was for, if it is still here.  Main thread.
 *
 * Two checks, because one is not enough.  findNUser() answers "who holds
 * this numnick now", and numnicks are reused: SetLocalNumNick() walks the
 * whole 2^18 space before it comes back round, but "eventually" is not
 * "never".  Comparing cli_firsttime() as well means the answer is the
 * client that was asked about or nothing at all.
 */
struct Client* worker_task_client(const struct WorkTask* task)
{
  struct Client* cptr;

  assert(0 != task);

  if (!task->wt_client[0])
    return 0;

  if (!(cptr = findNUser(task->wt_client)))
    return 0;

  if (cli_firsttime(cptr) != task->wt_client_born)
    return 0;

  return cptr;
}

/** Put a task on a queue, if there is room.  Mutex held.
 * @return Non-zero if it was accepted.
 */
static int task_accept(struct WorkTask* task)
{
  if (wInfo.wi_outstanding >= wInfo.wi_queue_max) {
    wInfo.wi_rejected++;
    return 0;
  }

  wInfo.wi_outstanding++;
  wInfo.wi_submitted++;

  return 1;
}

/** Hand a task to the pool, recording who owns it.  Any thread. */
int worker_submit_owned(struct ModuleHandle* mod, struct WorkTask* task)
{
  int accepted;

  assert(0 != task);
  assert(0 != task->wt_work);

  worker_prepare();

  pthread_mutex_lock(&wInfo.wi_mutex);

  /* Read inside the lock: a worker thread may be submitting at the same
   * moment the main thread is turning the pool off.
   */
  accepted = wInfo.wi_up && wInfo.wi_enabled && task_accept(task);

  if (accepted) {
    task->wt_owner = mod;
    queue_push(&wInfo.wi_pending_tail, task);
    wInfo.wi_pending_n++;
    pthread_cond_signal(&wInfo.wi_work);
  }

  pthread_mutex_unlock(&wInfo.wi_mutex);

  return accepted;
}

/** Hand a task to the pool.  Any thread. */
int worker_submit(struct WorkTask* task)
{
  return worker_submit_owned(0, task);
}

/** Hand a result to the main thread.  Worker thread. */
int worker_post(struct Worker* worker, struct WorkTask* task)
{
  int accepted;

  assert(0 != worker);
  assert(0 != task);

  worker_prepare();

  pthread_mutex_lock(&wInfo.wi_mutex);

  accepted = wInfo.wi_up && task_accept(task);

  if (accepted) {
    task->wt_owner = worker->w_owner;
    ready_push(task);
  }

  pthread_mutex_unlock(&wInfo.wi_mutex);

  return accepted;
}

/* ------------------------------------------------------------------------
 * Logging from a worker thread
 * ------------------------------------------------------------------------ */

/** Write the queued line to the log.  Main thread. */
static void worker_log_done(struct WorkTask* task)
{
  log_write(LS_SYSTEM, L_INFO, 0, "worker: %s", (const char*) task->wt_in);
}

/** Queue a log line from a worker thread.  Worker thread.
 *
 * log_write() writes to shared state and formats into shared buffers, so it
 * cannot be called from here.  The line is formatted with the system's
 * vsnprintf(), which is thread-safe, and handed to the main thread like any
 * other result.
 */
void worker_log(const char* fmt, ...)
{
  char buf[512];
  struct WorkTask* task;
  va_list args;
  size_t len;

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  worker_prepare();

  if (!(task = worker_task_new(0, worker_log_done)))
    return;

  len = strlen(buf) + 1;
  if (!(task->wt_in = worker_alloc(len))) {
    worker_free(task);
    return;
  }
  memcpy(task->wt_in, buf, len);
  task->wt_in_len = len;

  pthread_mutex_lock(&wInfo.wi_mutex);
  if (wInfo.wi_up && task_accept(task)) {
    ready_push(task);
    task = 0;
  }
  pthread_mutex_unlock(&wInfo.wi_mutex);

  /* Dropped rather than blocking: a worker in a loop must not be able to
   * push the server into swap through the log.
   */
  if (task)
    task_destroy(task);
}

/* ------------------------------------------------------------------------
 * The pool
 * ------------------------------------------------------------------------ */

/** Body of a pool thread.  Worker thread.
 *
 * Waits for a task, runs it, puts it on the reply queue, repeats.  Exits
 * when its slot is above the target size, which is how the pool shrinks.
 */
static void* worker_pool_main(void* arg)
{
  struct PoolThread* self = (struct PoolThread*) arg;
  struct WorkTask* task;

  pthread_mutex_lock(&wInfo.wi_mutex);

  for (;;) {
    while (!wInfo.wi_pending && self->pt_index < wInfo.wi_target)
      pthread_cond_wait(&wInfo.wi_work, &wInfo.wi_mutex);

    if (self->pt_index >= wInfo.wi_target)
      break;

    if (!(task = pending_pop()))
      continue;

    /* On the running list before the mutex is dropped, so that a module
     * unload that starts now sees this task and waits for it.
     */
    task->wt_next = wInfo.wi_running;
    wInfo.wi_running = task;
    wInfo.wi_running_n++;

    pthread_mutex_unlock(&wInfo.wi_mutex);

    (*task->wt_work)(task);

    pthread_mutex_lock(&wInfo.wi_mutex);

    running_unlink(task);
    ready_push(task);

    /* Somebody may be waiting for exactly this task to be over. */
    pthread_cond_broadcast(&wInfo.wi_idle);
  }

  pthread_mutex_unlock(&wInfo.wi_mutex);

  return 0;
}

/** Grow or shrink the pool.  Main thread.
 *
 * Growing creates threads.  Shrinking lowers the target, wakes everybody so
 * the surplus threads notice, and then joins them -- which waits for
 * whatever they were in the middle of, because the alternative is killing a
 * thread holding the mutex.
 */
static void pool_resize(unsigned int target)
{
  unsigned int i, old;

  if (target > WORKER_MAX_THREADS)
    target = WORKER_MAX_THREADS;

  pthread_mutex_lock(&wInfo.wi_mutex);
  old = wInfo.wi_nthreads;
  wInfo.wi_target = target;
  pthread_cond_broadcast(&wInfo.wi_work);
  pthread_mutex_unlock(&wInfo.wi_mutex);

  /* Shrink: join the slots that are now above the target. */
  for (i = target; i < old; i++) {
    if (!wInfo.wi_pool[i].pt_live)
      continue;
    pthread_join(wInfo.wi_pool[i].pt_thread, 0);
    wInfo.wi_pool[i].pt_live = 0;
  }

  /* Grow: create the slots that are now below it. */
  for (i = old; i < target; i++) {
    wInfo.wi_pool[i].pt_index = i;

    if (pthread_create(&wInfo.wi_pool[i].pt_thread, 0, worker_pool_main,
                       &wInfo.wi_pool[i])) {
      log_write(LS_SYSTEM, L_ERROR, 0,
                "worker: could not create pool thread %u: %s", i,
                strerror(errno));
      /* Settle for the threads that did start rather than pretending. */
      pthread_mutex_lock(&wInfo.wi_mutex);
      wInfo.wi_target = i;
      pthread_mutex_unlock(&wInfo.wi_mutex);
      target = i;
      break;
    }

    wInfo.wi_pool[i].pt_live = 1;
  }

  wInfo.wi_nthreads = target;
}

/* ------------------------------------------------------------------------
 * Dedicated workers
 * ------------------------------------------------------------------------ */

/** Trampoline into the worker's own loop.  Worker thread. */
static void* worker_dedicated_main(void* arg)
{
  struct Worker* worker = (struct Worker*) arg;

  (*worker->w_main)(worker, worker->w_arg);

  return 0;
}

/** Non-zero once somebody has asked this worker to stop.  Any thread. */
int worker_stopping(const struct Worker* worker)
{
  int stop;

  assert(0 != worker);

  pthread_mutex_lock(&wInfo.wi_mutex);
  stop = worker->w_stop;
  pthread_mutex_unlock(&wInfo.wi_mutex);

  return stop;
}

/** A descriptor that becomes readable when the worker should stop. */
int worker_stop_fd(const struct Worker* worker)
{
  assert(0 != worker);
  return worker->w_stop_r;
}

/** Name a dedicated worker was given. */
const char* worker_name(const struct Worker* worker)
{
  assert(0 != worker);
  return worker->w_name;
}

/** Module that spawned a dedicated worker, or NULL if the core did. */
struct ModuleHandle* worker_owner(const struct Worker* worker)
{
  assert(0 != worker);
  return worker->w_owner;
}

/** Start a thread with a loop of its own, recording who owns it.  Main thread.
 *
 * A module loaded from a Module{} block runs its mi_init while the
 * configuration file is still being read, which is before worker_init() has
 * seen the final FEAT_WORKER_THREADS.  Rather than make that a race the
 * module author has to know about, a spawn from before the subsystem is up
 * is recorded and started by worker_init().
 */
struct Worker* worker_spawn_owned(struct ModuleHandle* mod, const char* name,
                                  WorkerMainFn fn, void* arg)
{
  struct Worker* worker;
  int p[2];

  assert(0 != fn);

  worker_prepare();

  /* The feature is the master switch, and worker_enabled() knows how to
   * answer both before and after worker_init().  Refusing here rather than
   * handing back a thread that will never run is what lets a module treat
   * NULL as "workers are off" and say so.
   */
  if (!worker_enabled())
    return 0;

  if (!(worker = (struct Worker*) worker_alloc(sizeof(*worker))))
    return 0;

  if (pipe(p)) {
    log_write(LS_SYSTEM, L_ERROR, 0, "worker: no stop pipe for %s: %s",
              name ? name : "?", strerror(errno));
    worker_free(worker);
    return 0;
  }

  worker->w_stop_r = p[0];
  worker->w_stop_w = p[1];
  worker->w_main = fn;
  worker->w_arg = arg;
  worker->w_owner = mod;
  ircd_strncpy(worker->w_name, name ? name : "worker", WORKER_NAMELEN);

  worker->w_next = wInfo.wi_workers;
  wInfo.wi_workers = worker;
  wInfo.wi_nworkers++;

  if (wInfo.wi_up) {
    if (pthread_create(&worker->w_thread, 0, worker_dedicated_main, worker)) {
      log_write(LS_SYSTEM, L_ERROR, 0, "worker: could not start %s: %s",
                worker->w_name, strerror(errno));
      wInfo.wi_workers = worker->w_next;
      wInfo.wi_nworkers--;
      close(worker->w_stop_r);
      close(worker->w_stop_w);
      worker_free(worker);
      return 0;
    }
    worker->w_started = 1;
  }

  return worker;
}

/** Start a thread with a loop of its own.  Main thread. */
struct Worker* worker_spawn(const char* name, WorkerMainFn fn, void* arg)
{
  return worker_spawn_owned(0, name, fn, arg);
}

/** Start the workers that were spawned before the subsystem came up. */
static void worker_start_pending(void)
{
  struct Worker** p = &wInfo.wi_workers;
  struct Worker* worker;

  while ((worker = *p)) {
    if (worker->w_started) {
      p = &worker->w_next;
      continue;
    }

    if (pthread_create(&worker->w_thread, 0, worker_dedicated_main, worker)) {
      log_write(LS_SYSTEM, L_ERROR, 0, "worker: could not start %s: %s",
                worker->w_name, strerror(errno));
      *p = worker->w_next;
      wInfo.wi_nworkers--;
      close(worker->w_stop_r);
      close(worker->w_stop_w);
      worker_free(worker);
      continue;
    }

    worker->w_started = 1;
    p = &worker->w_next;
  }
}

/** Non-zero if \a worker is on the list of dedicated workers.  Main thread.
 *
 * Compares addresses and never looks inside, because the whole point is
 * that the pointer may be to memory that has been freed.  A module keeps
 * the handle worker_spawn_owned() gave it, and the server frees the worker
 * behind that handle on its own schedule: worker_cancel_module() runs
 * before mi_fini, and worker_set_threads(0) runs whenever an operator says
 * so.  So a stop that arrives from outside this file may well be the
 * second one, and the only thing it is safe to do with its argument is to
 * look for it here.
 */
static int worker_live(const struct Worker* worker)
{
  const struct Worker* w;

  for (w = wInfo.wi_workers; w; w = w->w_next)
    if (w == worker)
      return 1;

  return 0;
}

/** Ask a dedicated worker to stop, and wait for it.  Main thread. */
void worker_stop(struct Worker* worker)
{
  struct Worker** p;
  unsigned char c = 0;

  assert(0 != worker);

  worker_prepare();

  /* Already stopped and freed: nothing to do, and nothing that can be
   * read from the pointer to say so.
   */
  if (!worker_live(worker)) {
    Debug((DEBUG_ERROR, "worker: stop of a worker that is already gone"));
    return;
  }

  pthread_mutex_lock(&wInfo.wi_mutex);
  worker->w_stop = 1;
  pthread_mutex_unlock(&wInfo.wi_mutex);

  /* Both halves matter: the flag for a worker that polls it, the byte for
   * one parked in select() on the descriptor.
   */
  if (worker->w_stop_w >= 0 && write(worker->w_stop_w, &c, 1) != 1)
    Debug((DEBUG_ERROR, "worker: stop pipe write for %s failed",
           worker->w_name));

  if (worker->w_started)
    pthread_join(worker->w_thread, 0);

  for (p = &wInfo.wi_workers; *p; p = &(*p)->w_next) {
    if (*p == worker) {
      *p = worker->w_next;
      wInfo.wi_nworkers--;
      break;
    }
  }

  close(worker->w_stop_r);
  close(worker->w_stop_w);
  worker_free(worker);
}

/** Stop a dedicated worker, if \a mod owns it.  Main thread.
 *
 * The entry point behind module_stop_worker(), which cannot check
 * ownership itself: the handle a module passes may be stale, and reading
 * w_owner out of it is exactly the use-after-free this exists to prevent.
 * The liveness check comes first for that reason.
 * @param[in] mod Module claiming the worker.
 * @param[in] worker Worker to stop, or NULL.
 * @return Non-zero if the worker was \a mod's and has now been stopped;
 *   zero if it was somebody else's, or is already gone.
 */
int worker_stop_owned(struct ModuleHandle* mod, struct Worker* worker)
{
  worker_prepare();

  if (!worker || !worker_live(worker) || worker->w_owner != mod)
    return 0;

  worker_stop(worker);

  return 1;
}

/* ------------------------------------------------------------------------
 * Delivering results
 * ------------------------------------------------------------------------ */

/** Deliver everything the workers have finished.  Main thread.
 *
 * The reply queue is detached under the mutex and then walked without it,
 * because a wt_done is allowed to submit more work -- and would deadlock on
 * a mutex this function was still holding.
 */
unsigned int worker_drain(void)
{
  struct WorkTask* list;
  struct WorkTask* task;
  unsigned int delivered = 0;

  if (!wInfo.wi_up)
    return 0;

  pthread_mutex_lock(&wInfo.wi_mutex);
  list = wInfo.wi_ready;
  wInfo.wi_ready = 0;
  wInfo.wi_ready_tail = &wInfo.wi_ready;
  wInfo.wi_ready_n = 0;
  pthread_mutex_unlock(&wInfo.wi_mutex);

  while ((task = list)) {
    list = task->wt_next;
    task->wt_next = 0;

    if (task->wt_done)
      (*task->wt_done)(task);

    task_destroy(task);
    delivered++;
  }

  if (delivered) {
    pthread_mutex_lock(&wInfo.wi_mutex);
    wInfo.wi_outstanding -= delivered;
    wInfo.wi_completed += delivered;
    pthread_mutex_unlock(&wInfo.wi_mutex);
  }

  return delivered;
}

/** Wake-up pipe became readable.  Main thread. */
static void worker_wake_callback(struct Event* event)
{
  unsigned char buf[64];
  int n;

  /* socket_del() during shutdown comes back through here. */
  if (event->ev_type == ET_DESTROY) {
    close(s_fd(event->ev_gen.gen_socket));
    return;
  }

  assert(event->ev_type == ET_READ);

  /* Empty the pipe before clearing the armed flag, so that a byte written
   * between the two is not lost.
   */
  do {
    n = read(s_fd(event->ev_gen.gen_socket), buf, sizeof(buf));
  } while (n == (int) sizeof(buf));

  pthread_mutex_lock(&wInfo.wi_mutex);
  wInfo.wi_wake_armed = 0;
  pthread_mutex_unlock(&wInfo.wi_mutex);

  worker_drain();
}

/* ------------------------------------------------------------------------
 * Ownership: keeping a module's code alive while its work runs
 * ------------------------------------------------------------------------ */

/** Unlink every task \a mod owns from a queue, onto \a dropped.  Mutex held.
 * @param[in,out] head_p Head of the queue to filter.
 * @param[in,out] tail_p Tail pointer of that queue.
 * @param[in,out] count_p Length of that queue.
 * @param[in] mod Owner to remove.
 * @param[in,out] dropped List the removed tasks are pushed onto.
 * @return How many were removed.
 */
static unsigned int queue_filter_owner(struct WorkTask** head_p,
                                       struct WorkTask*** tail_p,
                                       unsigned int* count_p,
                                       const struct ModuleHandle* mod,
                                       struct WorkTask** dropped)
{
  struct WorkTask** p = head_p;
  struct WorkTask* task;
  unsigned int removed = 0;

  while ((task = *p)) {
    if (task->wt_owner != mod) {
      p = &task->wt_next;
      continue;
    }

    if (!(*p = task->wt_next))
      *tail_p = p;

    (*count_p)--;
    wInfo.wi_outstanding--;

    task->wt_next = *dropped;
    *dropped = task;
    removed++;
  }

  return removed;
}

/** Non-zero if any task belonging to \a mod is in a worker right now.
 * Mutex held.
 */
static int module_has_running(const struct ModuleHandle* mod)
{
  const struct WorkTask* task;

  for (task = wInfo.wi_running; task; task = task->wt_next)
    if (task->wt_owner == mod)
      return 1;

  return 0;
}

/** Drop everything a module owns, so it can be unloaded.  Main thread.
 *
 * Unloading a module unmaps its code.  A task of that module's still
 * waiting to run is easy -- it is taken off the queue and thrown away.  A
 * task of that module's that a worker is executing right now is not: the
 * only correct thing to do is wait, because the alternative is calling
 * dlclose() on the function on a live stack.
 *
 * So the main thread blocks here, and the whole server with it.  That is
 * the price of unloading a module with slow work in flight, and it is
 * logged so an operator can see why the server went quiet.
 *
 * Results are then discarded rather than delivered: wt_done belongs to the
 * module too.
 */
void worker_cancel_module(struct ModuleHandle* mod)
{
  struct WorkTask* dropped = 0;
  struct WorkTask* task;
  struct Worker* worker;
  struct Worker* next;
  unsigned int cancelled = 0;
  time_t complained = 0;

  worker_prepare();

  /* Its threads first, and joined before anything else happens: a dedicated
   * worker still running could worker_post() a result whose wt_done is this
   * module's code, and a queue emptied before that thread stopped would fill
   * up again behind us.  Reached even when the subsystem never came up,
   * because a module can have a spawn still held from before worker_init().
   */
  for (worker = wInfo.wi_workers; worker; worker = next) {
    next = worker->w_next;
    if (worker->w_owner == mod)
      worker_stop(worker);
  }

  if (!wInfo.wi_up)
    return;

  pthread_mutex_lock(&wInfo.wi_mutex);

  /* Never started: unlink and collect for release below. */
  cancelled += queue_filter_owner(&wInfo.wi_pending, &wInfo.wi_pending_tail,
                                  &wInfo.wi_pending_n, mod, &dropped);

  /* Running: nothing to do but wait for the thread to come back. */
  while (module_has_running(mod)) {
    struct timespec deadline;

    if (!complained) {
      complained = time(0);
      log_write(LS_SYSTEM, L_WARNING, 0,
                "worker: waiting for work in flight before unloading a module");
    }

    deadline.tv_sec = time(0) + WORKER_STALL_WARN;
    deadline.tv_nsec = 0;

    if (pthread_cond_timedwait(&wInfo.wi_idle, &wInfo.wi_mutex, &deadline)
        == ETIMEDOUT && module_has_running(mod))
      log_write(LS_SYSTEM, L_WARNING, 0,
                "worker: still waiting after %ld seconds; the server is "
                "blocked until a worker returns",
                (long) (time(0) - complained));
  }

  /* Pending again: a task that was running is allowed to submit more work,
   * and one of them may have done so after the first sweep.  The mutex has
   * been held continuously since that wait ended, so this is the last word.
   */
  cancelled += queue_filter_owner(&wInfo.wi_pending, &wInfo.wi_pending_tail,
                                  &wInfo.wi_pending_n, mod, &dropped);

  /* Finished, or finished while we waited: the result goes nowhere, because
   * wt_done belongs to the module too.  Taken off the queue rather than
   * flagged, so that nothing downstream has to remember to ignore it.
   */
  cancelled += queue_filter_owner(&wInfo.wi_ready, &wInfo.wi_ready_tail,
                                  &wInfo.wi_ready_n, mod, &dropped);

  pthread_mutex_unlock(&wInfo.wi_mutex);

  /* Outside the mutex: wt_free is the module's code and may do anything. */
  while ((task = dropped)) {
    dropped = task->wt_next;
    task->wt_next = 0;
    task_destroy(task);
  }

  if (cancelled)
    log_write(LS_SYSTEM, L_INFO, 0,
              "worker: discarded %u task%s belonging to an unloaded module",
              cancelled, cancelled == 1 ? "" : "s");
}

/** Tasks a module has outstanding.  Main thread. */
unsigned int worker_module_tasks(const struct ModuleHandle* mod)
{
  const struct WorkTask* task;
  unsigned int count = 0;

  worker_prepare();

  pthread_mutex_lock(&wInfo.wi_mutex);

  for (task = wInfo.wi_pending; task; task = task->wt_next)
    if (task->wt_owner == mod)
      count++;
  for (task = wInfo.wi_running; task; task = task->wt_next)
    if (task->wt_owner == mod)
      count++;
  for (task = wInfo.wi_ready; task; task = task->wt_next)
    if (task->wt_owner == mod)
      count++;

  pthread_mutex_unlock(&wInfo.wi_mutex);

  return count;
}

/** Dedicated workers a module has running.  Main thread. */
unsigned int worker_module_workers(const struct ModuleHandle* mod)
{
  const struct Worker* worker;
  unsigned int count = 0;

  for (worker = wInfo.wi_workers; worker; worker = worker->w_next)
    if (worker->w_owner == mod)
      count++;

  return count;
}

/* ------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

/** Create the queues and the wake-up pipe.  Main thread.
 * @return Non-zero on success.
 */
static int worker_bring_up(void)
{
  int p[2];

  worker_prepare();

  if (wInfo.wi_up)
    return 1;

  if (pipe(p)) {
    log_write(LS_SYSTEM, L_ERROR, 0, "worker: could not open wake pipe: %s",
              strerror(errno));
    return 0;
  }

  wInfo.wi_wake_r = p[0];
  wInfo.wi_wake_w = p[1];

  /* Non-blocking, because the write happens under the mutex from a worker
   * thread: blocking there would stall every other worker as well.
   */
  os_set_nonblocking(wInfo.wi_wake_w);

  if (!socket_add(&wInfo.wi_wake_sock, worker_wake_callback, 0, SS_NOTSOCK,
                  SOCK_EVENT_READABLE, wInfo.wi_wake_r)) {
    log_write(LS_SYSTEM, L_ERROR, 0, "worker: could not watch the wake pipe");
    close(p[0]);
    close(p[1]);
    wInfo.wi_wake_r = wInfo.wi_wake_w = -1;
    return 0;
  }

  /* Published under the lock: from here on a worker thread may read it. */
  pthread_mutex_lock(&wInfo.wi_mutex);
  wInfo.wi_up = 1;
  pthread_mutex_unlock(&wInfo.wi_mutex);

  return 1;
}

/** Resize the pool, bringing the subsystem up or down with it.  Main thread. */
void worker_set_threads(int nthreads)
{
  struct Worker* worker;
  struct Worker* next;

  worker_prepare();

  if (nthreads < 0)
    nthreads = 0;
  if (nthreads > WORKER_MAX_THREADS)
    nthreads = WORKER_MAX_THREADS;

  pthread_mutex_lock(&wInfo.wi_mutex);
  wInfo.wi_queue_max = (unsigned int) feature_int(FEAT_WORKER_QUEUE_MAX);
  pthread_mutex_unlock(&wInfo.wi_mutex);

  if (nthreads == 0) {
    /* Zero is the master switch, so the dedicated workers go too -- including
     * any that were spawned before worker_init() and are still waiting to
     * start.  They are not resurrected if the feature comes back: the module
     * that wanted one asks again, on its own terms.
     */
    pthread_mutex_lock(&wInfo.wi_mutex);
    wInfo.wi_enabled = 0;
    pthread_mutex_unlock(&wInfo.wi_mutex);

    /* Only the threads that are actually running.  A worker still held from
     * before worker_init() has no thread to stop, and freeing it would
     * leave the module that asked for it holding a pointer to nothing; it
     * stays on the list until the module goes away, or until the feature
     * comes back and worker_start_pending() picks it up.
     */
    for (worker = wInfo.wi_workers; worker; worker = next) {
      next = worker->w_next;
      if (worker->w_started)
        worker_stop(worker);
    }

    if (!wInfo.wi_up)
      return;

    pool_resize(0);
    worker_drain();

    log_write(LS_SYSTEM, L_INFO, 0, "worker: pool stopped");
    return;
  }

  if (!worker_bring_up())
    return;

  pthread_mutex_lock(&wInfo.wi_mutex);
  wInfo.wi_enabled = 1;
  pthread_mutex_unlock(&wInfo.wi_mutex);

  pool_resize((unsigned int) nthreads);
  worker_start_pending();

  log_write(LS_SYSTEM, L_INFO, 0, "worker: pool running with %u thread%s",
            wInfo.wi_nthreads, wInfo.wi_nthreads == 1 ? "" : "s");
}

/** Bring the worker subsystem up, if the configuration asks for it. */
void worker_init(void)
{
  worker_prepare();
  wInfo.wi_configured = 1;

  worker_set_threads(feature_int(FEAT_WORKER_THREADS));
}

/** Called when FEAT_WORKER_THREADS or FEAT_WORKER_QUEUE_MAX changes. */
void worker_feature_notify(void)
{
  /* The first time round, the configuration file is still being read and
   * the value is not final; worker_init() applies whatever it settles on.
   * After that, every /SET and every /REHASH arrives here.
   */
  if (!wInfo.wi_configured)
    return;

  worker_set_threads(feature_int(FEAT_WORKER_THREADS));
}

/** Stop every worker and release the subsystem.  Main thread. */
void worker_shutdown(void)
{
  struct WorkTask* task;

  if (!wInfo.wi_up)
    return;

  pthread_mutex_lock(&wInfo.wi_mutex);
  wInfo.wi_enabled = 0;
  pthread_mutex_unlock(&wInfo.wi_mutex);

  while (wInfo.wi_workers)
    worker_stop(wInfo.wi_workers);

  pool_resize(0);

  /* Whatever came back is dropped: the core this would run against is
   * already being taken apart.
   */
  pthread_mutex_lock(&wInfo.wi_mutex);
  task = wInfo.wi_ready;
  wInfo.wi_ready = 0;
  wInfo.wi_ready_tail = &wInfo.wi_ready;
  wInfo.wi_ready_n = 0;
  while (wInfo.wi_pending) {
    struct WorkTask* pending = pending_pop();
    pending->wt_next = task;
    task = pending;
  }
  wInfo.wi_outstanding = 0;
  wInfo.wi_up = 0;
  pthread_mutex_unlock(&wInfo.wi_mutex);

  while (task) {
    struct WorkTask* next = task->wt_next;
    task->wt_next = 0;
    task_destroy(task);
    task = next;
  }

  /* socket_del() runs the callback with ET_DESTROY, which closes the read
   * end; the write end is ours to close.
   */
  socket_del(&wInfo.wi_wake_sock);
  close(wInfo.wi_wake_w);
  wInfo.wi_wake_r = wInfo.wi_wake_w = -1;

  /* The mutex and the condition variables stay: they cost nothing, nothing
   * is waiting on them by now, and destroying them would make a later
   * worker_set_threads() -- which the unit test does -- undefined.
   * wi_configured stays set too, so a spawn after this is refused rather
   * than held for a worker_init() that is not coming.
   */
}

/* ------------------------------------------------------------------------
 * Instrumentation
 * ------------------------------------------------------------------------ */

/** Read one counter under the lock.
 *
 * Every counter below is written by worker threads, so reading it is not a
 * plain load however harmless it looks: a torn or stale value read without
 * the mutex is a data race, and the whole design rests on there being none.
 * These are called from /STATS, so the cost does not matter.
 */
static unsigned int worker_read(const unsigned int* field)
{
  unsigned int value;

  worker_prepare();

  pthread_mutex_lock(&wInfo.wi_mutex);
  value = *field;
  pthread_mutex_unlock(&wInfo.wi_mutex);

  return value;
}

/** Non-zero when workers are available. */
int worker_enabled(void)
{
  int enabled;

  worker_prepare();

  /* Before worker_init() the pool has not started yet, and answering "no"
   * would be misleading: a module loaded from a Module{} block asks from
   * its mi_init, which runs while the configuration file is still being
   * read.  What the configuration says is the best answer available, and
   * it is the one that turns out to be true.
   */
  if (!wInfo.wi_configured)
    return feature_int(FEAT_WORKER_THREADS) > 0;

  pthread_mutex_lock(&wInfo.wi_mutex);
  enabled = wInfo.wi_up && wInfo.wi_enabled;
  pthread_mutex_unlock(&wInfo.wi_mutex);

  return enabled;
}

/** Threads currently in the pool. */
unsigned int worker_thread_count(void)
{
  return wInfo.wi_nthreads;
}

/** Dedicated workers currently running. */
unsigned int worker_dedicated_count(void)
{
  return wInfo.wi_nworkers;
}

/** Tasks submitted and not yet delivered. */
unsigned int worker_outstanding(void)
{
  return worker_read(&wInfo.wi_outstanding);
}

/** Tasks waiting for a pool thread to pick them up. */
unsigned int worker_queued(void)
{
  return worker_read(&wInfo.wi_pending_n);
}

/** Tasks a pool thread is working on right now. */
unsigned int worker_running(void)
{
  return worker_read(&wInfo.wi_running_n);
}

/** Finished tasks waiting for the main thread to deliver them. */
unsigned int worker_undelivered(void)
{
  return worker_read(&wInfo.wi_ready_n);
}

/** Tasks accepted since the server started. */
unsigned int worker_submitted(void)
{
  return worker_read(&wInfo.wi_submitted);
}

/** Tasks delivered since the server started. */
unsigned int worker_completed(void)
{
  return worker_read(&wInfo.wi_completed);
}

/** Submissions refused for a full queue since the server started. */
unsigned int worker_rejected(void)
{
  return worker_read(&wInfo.wi_rejected);
}
