#ifndef INCLUDED_worker_h
#define INCLUDED_worker_h
/*
 * IRC - Internet Relay Chat, include/worker.h
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
 * @brief Worker threads: blocking work, off the main thread.
 *
 * The IRC core stays single-threaded.  Everything in this file exists so
 * that work which would otherwise stall it -- a database round trip, a
 * password hash, an outbound HTTP request -- can happen somewhere else.
 *
 * There are two shapes:
 *
 *   - A @b task (#WorkTask) is a one-shot unit of work handed to a pool of
 *     interchangeable threads.  Submit it with worker_submit(); its
 *     #WorkTask::wt_work runs in whichever pool thread picks it up, and its
 *     #WorkTask::wt_done runs afterwards in the main thread.
 *
 *   - A @b dedicated @b worker (#Worker) is a thread of its own with its own
 *     loop, for something that is not a series of short tasks: a listening
 *     socket, a subscription to a message bus.  Start it with
 *     worker_spawn(); it hands results back with worker_post(), which puts a
 *     task straight onto the reply queue so its wt_done runs in the main
 *     thread just the same.
 *
 * Both shapes are woken up on the main thread through a self-pipe that the
 * event engine watches like any other descriptor, so no event engine needed
 * changing to support this.
 *
 * @section worker_rule The rule
 *
 * @b A @b worker @b thread @b never @b touches @b core @b state.
 *
 * Not @c struct @c Client, not @c struct @c Channel, not the hash tables,
 * not @c CurrentTime, not a global of any kind.  Not @c MyMalloc() and not
 * @c MyFree(), because the free lists behind them are unsynchronised.  Not
 * @c log_write(), not @c Debug(), not @c send_reply() or any of the
 * @c sendcmdto_* family.
 *
 * What a worker thread may call is short enough to list: worker_alloc(),
 * worker_free(), worker_log(), worker_stopping(), worker_stop_fd(),
 * worker_post(), worker_submit(), the system allocator, the system's own
 * thread-safe library functions, and pure functions that work on a context
 * the caller supplied (SHA1Init() and friends qualify; anything returning a
 * pointer to a static buffer does not).
 *
 * Everything a worker needs, it receives copied into the task.  Everything
 * it produces, it returns copied in the task.  That is the whole boundary,
 * and it is the only part of the server that has to be audited for thread
 * safety.
 *
 * See doc/readme.workers.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif
#ifndef INCLUDED_stddef_h
#include <stddef.h>     /* size_t */
#define INCLUDED_stddef_h
#endif
#ifndef INCLUDED_time_h
#include <time.h>       /* time_t */
#define INCLUDED_time_h
#endif

struct Client;
struct ModuleHandle;
struct WorkTask;
struct Worker;

/** Length of the numnick a task remembers a client by, without the NUL. */
#define WORKER_NUMNICKLEN 5

/** Largest value #FEAT_WORKER_THREADS accepts.
 *
 * Not a resource limit so much as a sanity one: a typo of 100000 in the
 * configuration should be refused rather than obeyed.
 */
#define WORKER_MAX_THREADS 64

/** Longest name a dedicated worker may have, without the NUL. */
#define WORKER_NAMELEN 31

/** The work itself.  Runs in a worker thread.
 *
 * Reads #WorkTask::wt_in, writes #WorkTask::wt_out and
 * #WorkTask::wt_status.  Must obey @ref worker_rule.
 * @param[in,out] task The task being worked on.
 */
typedef void (*WorkFn)(struct WorkTask* task);

/** What to do with the result.  Runs in the main thread.
 *
 * By the time this runs the work is finished and the server is between
 * commands, so it may touch core state freely -- including submitting
 * another task.  It may not assume the client that asked for the work is
 * still connected; see worker_task_client().
 * @param[in,out] task The task that finished.
 */
typedef void (*WorkDoneFn)(struct WorkTask* task);

/** Release a task's payload.  Runs in the main thread.
 *
 * Optional.  A task that leaves it NULL gets the default, which is
 * worker_free() on #WorkTask::wt_in and #WorkTask::wt_out -- correct for
 * any task that allocated them with worker_alloc().  Supply one when the
 * payload owns something else: a file descriptor, a nested allocation, a
 * handle from a third-party library.
 *
 * It runs on every path, including when the task is cancelled without ever
 * having been worked on.
 * @param[in,out] task The task being released.
 */
typedef void (*WorkFreeFn)(struct WorkTask* task);

/** The body of a dedicated worker.  Runs in its own thread.
 *
 * Returning ends the thread.  A worker that means to stay alive loops until
 * worker_stopping() is true, and waits on worker_stop_fd() rather than
 * sleeping, so that worker_stop() does not have to wait out a sleep.
 * @param[in] worker The worker this is the body of.
 * @param[in] arg The pointer passed to worker_spawn().
 */
typedef void (*WorkerMainFn)(struct Worker* worker, void* arg);

/** One unit of work, and its result.
 *
 * The submitter fills in the first group before calling worker_submit(),
 * the worker thread fills in the second, and the third belongs to the
 * server: a module that writes to it will be surprised at what happens.
 *
 * Ownership passes to the server on a successful worker_submit() or
 * worker_post(), and the server frees the task once wt_done has run.  A
 * submit that fails leaves the task with the caller, who is expected to
 * worker_task_free() it.
 */
struct WorkTask {
  /* --- set by the submitter, in the main thread --- */
  WorkFn      wt_work;      /**< The work.  Required, except for a post. */
  WorkDoneFn  wt_done;      /**< The result.  May be NULL. */
  WorkFreeFn  wt_free;      /**< Payload release.  May be NULL. */
  void*       wt_arg;       /**< Opaque to the server; the submitter's own. */

  void*       wt_in;        /**< Input, read by wt_work. */
  size_t      wt_in_len;    /**< Length of #wt_in. */

  /* --- set by wt_work, in a worker thread; read by wt_done --- */
  void*       wt_out;       /**< Output, read by wt_done. */
  size_t      wt_out_len;   /**< Length of #wt_out. */
  int         wt_status;    /**< Zero for success; the task's own meaning. */

  /* --- private to worker.c --- */
  struct WorkTask*     wt_next;    /**< Next on whichever queue it is on. */
  struct ModuleHandle* wt_owner;   /**< Module that submitted it, or NULL. */
  char        wt_client[WORKER_NUMNICKLEN + 1]; /**< Numnick, not a pointer. */
  time_t      wt_client_born;      /**< cli_firsttime() of that client. */
};

/*
 * Lifecycle.  The server calls these; modules do not.
 */

/** Bring the worker subsystem up, if the configuration asks for it.
 *
 * Called once from main() after the configuration file has been read, so
 * that #FEAT_WORKER_THREADS has its final value and a module loaded from a
 * Module{} block can have spawned a dedicated worker already: those spawns
 * are held and started here.
 *
 * With FEAT_WORKER_THREADS at zero this creates no threads, no queues and
 * no pipe, and the server behaves exactly as it did before workers existed.
 */
extern void worker_init(void);

/** Stop every worker and release the subsystem.
 *
 * Waits for work in flight.  Results that arrive during the wait are
 * discarded rather than delivered: the core is going away, and a wt_done
 * running against a half-torn-down server is worse than a lost result.
 */
extern void worker_shutdown(void);

/** Resize the pool to \a nthreads, starting or stopping threads.
 *
 * Zero is the master switch: it stops the pool @em and every dedicated
 * worker, and makes worker_submit() and worker_spawn() fail until it is
 * non-zero again.  Raising it later does not bring dedicated workers back;
 * the module that wanted one asks again.
 *
 * Shrinking waits for the threads it is removing to finish what they are
 * doing.
 * @param[in] nthreads Desired pool size, clamped to #WORKER_MAX_THREADS.
 */
extern void worker_set_threads(int nthreads);

/** Apply a change to #FEAT_WORKER_THREADS or #FEAT_WORKER_QUEUE_MAX.
 *
 * The features' notify callback.  Does nothing until worker_init() has run,
 * because until then the configuration file is still being read and the
 * value it will settle on is not known yet.
 */
extern void worker_feature_notify(void);

/** Deliver everything the workers have finished.
 *
 * The main thread calls this from the wake-up callback.  It runs each
 * finished task's wt_done and then frees the task.  Exposed because the
 * shutdown path and the unit test drive it directly.
 * @return Number of tasks delivered.
 */
extern unsigned int worker_drain(void);

/*
 * Submitting work.
 */

/** Allocate a zeroed task.
 *
 * @param[in] work Runs in a worker thread.  Required.
 * @param[in] done Runs in the main thread afterwards, or NULL.
 * @return A task the caller owns until worker_submit() accepts it, or NULL
 *   if there was no memory.
 */
extern struct WorkTask* worker_task_new(WorkFn work, WorkDoneFn done);

/** Release a task the server does not own.
 *
 * Only for a task worker_submit() refused; once submitted, the server frees
 * it.  Runs wt_free (or the default payload release) first.
 * @param[in] task Task to release.
 */
extern void worker_task_free(struct WorkTask* task);

/** Remember which client asked for this work.  Main thread only.
 *
 * Stores the client's numnick and the time it connected, never a pointer:
 * a @c struct @c Client is returned to a free list when the user quits and
 * the memory is handed to somebody else, so a pointer held across a task is
 * a use-after-free waiting for a slow query.
 * @param[in,out] task Task to attach the client to.
 * @param[in] cptr Client, or NULL to detach.
 */
extern void worker_task_set_client(struct WorkTask* task,
                                   const struct Client* cptr);

/** The client this work was for, if it is still here.  Main thread only.
 *
 * @param[in] task Task to look up.
 * @return The client, or NULL if it quit, or if the numnick has since been
 *   handed to somebody else, or if no client was ever attached.
 */
extern struct Client* worker_task_client(const struct WorkTask* task);

/** Hand a task to the pool.
 *
 * Safe to call from a worker thread as well as from the main thread.
 *
 * @param[in] task Task to run.  On success the server owns it.
 * @return Non-zero on success.  Zero if the pool is off
 *   (#FEAT_WORKER_THREADS is zero) or the queue is at
 *   #FEAT_WORKER_QUEUE_MAX, in which case the task is still the caller's.
 */
extern int worker_submit(struct WorkTask* task);

/*
 * Dedicated workers.
 */

/** Start a thread with a loop of its own.
 *
 * @param[in] name Short name for logs and /STATS M.
 * @param[in] fn The thread body.
 * @param[in] arg Passed through to \a fn.
 * @return The worker, or NULL if the pool is off or the thread could not be
 *   created.  The server owns it; stop it with worker_stop().
 */
extern struct Worker* worker_spawn(const char* name, WorkerMainFn fn,
                                   void* arg);

/** Ask a dedicated worker to stop, and wait for it.  Main thread only.
 *
 * Sets the flag worker_stopping() reads, makes worker_stop_fd() readable,
 * and joins the thread.  Results the worker posted but the main thread has
 * not delivered yet are delivered on the next worker_drain().
 *
 * A worker the server has already stopped -- on worker_cancel_module(),
 * or on worker_set_threads(0) -- is gone, and stopping it again does
 * nothing: the pointer is checked against the list of live workers before
 * it is read, so a handle that outlived its worker is harmless here.
 * @param[in] worker Worker to stop.  Invalid once this returns.
 */
extern void worker_stop(struct Worker* worker);

/** Non-zero once somebody has asked this worker to stop.
 *
 * The thread body polls this.  Safe to call from the worker thread.
 * @param[in] worker Worker to query.
 */
extern int worker_stopping(const struct Worker* worker);

/** A descriptor that becomes readable when the worker should stop.
 *
 * Put it in the worker's own select()/poll() set so that a stop does not
 * have to wait for a timeout to expire.  Never read from it; the byte is
 * there to make it readable, and worker_stopping() is the actual answer.
 * @param[in] worker Worker to query.
 * @return A readable descriptor, or -1.
 */
extern int worker_stop_fd(const struct Worker* worker);

/** Name a dedicated worker was given.
 * @param[in] worker Worker to query.
 */
extern const char* worker_name(const struct Worker* worker);

/** Module that spawned a dedicated worker, or NULL if the core did.
 * @param[in] worker Worker to query.
 */
extern struct ModuleHandle* worker_owner(const struct Worker* worker);

/** Hand a result to the main thread.  Called from a worker thread.
 *
 * The task's wt_work is never called; the worker has already done the work.
 * Its wt_done runs in the main thread on the next drain.
 * @param[in] worker The posting worker.
 * @param[in] task Task carrying the result.  On success the server owns it.
 * @return Non-zero on success; zero if the queue is full, leaving the task
 *   with the caller.
 */
extern int worker_post(struct Worker* worker, struct WorkTask* task);

/*
 * Things a worker thread may call.
 */

/** Allocate memory a worker thread may touch.
 *
 * The system allocator, deliberately: the core's MyMalloc() draws on free
 * lists with no locking, and calling it from two threads corrupts them
 * quietly.  Memory from here is freed with worker_free().
 * @param[in] size Bytes to allocate.
 * @return Zeroed memory, or NULL.
 */
extern void* worker_alloc(size_t size);

/** Release memory from worker_alloc().  NULL is accepted.
 * @param[in] ptr Memory to release.
 */
extern void worker_free(void* ptr);

/** Write a line to the server log, from a worker thread.
 *
 * log_write() is not thread-safe, so this does not call it: the line is
 * formatted here and queued, and the main thread logs it on the next drain.
 * Which means it appears slightly late, and is dropped rather than blocking
 * if the queue is full.
 * @param[in] fmt printf-style format, followed by its arguments.
 */
extern void worker_log(const char* fmt, ...);

/*
 * Instrumentation, for /STATS M.
 */

/** Non-zero when workers are available.
 *
 * Before worker_init() -- which is when a module loaded from a Module{}
 * block asks, since its mi_init runs while the configuration file is still
 * being read -- this answers what the configuration says the pool will be,
 * because the pool has not started yet and "no" would be wrong.
 */
extern int worker_enabled(void);
/** Threads currently in the pool. */
extern unsigned int worker_thread_count(void);
/** Dedicated workers currently running. */
extern unsigned int worker_dedicated_count(void);
/** Tasks submitted and not yet delivered: queued + running + undelivered. */
extern unsigned int worker_outstanding(void);
/** Tasks waiting for a pool thread to pick them up. */
extern unsigned int worker_queued(void);
/** Tasks a pool thread is working on right now. */
extern unsigned int worker_running(void);
/** Finished tasks waiting for the main thread to deliver them. */
extern unsigned int worker_undelivered(void);
/** Tasks accepted since the server started. */
extern unsigned int worker_submitted(void);
/** Tasks delivered since the server started. */
extern unsigned int worker_completed(void);
/** Submissions refused for a full queue since the server started. */
extern unsigned int worker_rejected(void);

/*
 * Ownership.  Used by module.c so that unloading a module does not leave a
 * worker thread executing code that is about to be unmapped.  Not for
 * modules, which reach these through module_submit_work() and friends.
 */
extern int worker_submit_owned(struct ModuleHandle* mod,
                               struct WorkTask* task);
extern struct Worker* worker_spawn_owned(struct ModuleHandle* mod,
                                         const char* name, WorkerMainFn fn,
                                         void* arg);
extern int worker_stop_owned(struct ModuleHandle* mod, struct Worker* worker);
extern void worker_cancel_module(struct ModuleHandle* mod);
extern unsigned int worker_module_tasks(const struct ModuleHandle* mod);
extern unsigned int worker_module_workers(const struct ModuleHandle* mod);

#endif /* INCLUDED_worker_h */
