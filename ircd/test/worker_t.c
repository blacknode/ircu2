/* worker_t.c - Test the worker pool and the boundary around it.
 *
 * The point of the worker subsystem is that the main thread and the worker
 * threads share exactly one thing: the queues in worker.c.  So that is what
 * this exercises -- that work crosses in both directions, that the queue
 * bound is enforced, that a task remembers a client safely rather than by
 * pointer, and that everything an owner had is gone once the owner is.
 *
 * The tests drive worker_drain() directly instead of running an event loop;
 * that is precisely what the wake-up callback does once the engine has
 * noticed the self-pipe.
 */

#include "client.h"
#include "ircd_features.h"
#include "struct.h"
#include "worker.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Recorded by worker_stub.c; the event system is not linked in. */
extern int stub_socket_live;
extern struct Client* stub_numnick_client;
extern char stub_numnick[8];

/* The feature system is not linked in either, so this test is it.  Both
 * values are what worker.c reads when it is asked to resize.
 */
int stub_worker_threads = 0;
int stub_worker_queue_max = 1024;

int feature_int(enum Feature feat)
{
  switch (feat) {
  case FEAT_WORKER_THREADS:   return stub_worker_threads;
  case FEAT_WORKER_QUEUE_MAX: return stub_worker_queue_max;
  default:                    return 0;
  }
}

/** How long any single wait in this test may take before it is a failure. */
#define TEST_TIMEOUT 10

/* ---------------------------------------------------------------------- */

/** Tasks whose work function has run.  Touched from several threads. */
static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static int work_ran;

/** Tasks whose done function has run.  Main thread only. */
static int done_ran;
/** Sum of the results delivered, so an interleaving cannot hide a loss. */
static long done_total;

static void counters_reset(void)
{
  pthread_mutex_lock(&counter_lock);
  work_ran = 0;
  pthread_mutex_unlock(&counter_lock);

  done_ran = 0;
  done_total = 0;
}

static int work_count(void)
{
  int n;

  pthread_mutex_lock(&counter_lock);
  n = work_ran;
  pthread_mutex_unlock(&counter_lock);

  return n;
}

/** Bring the pool up with \a n threads. */
static void pool_start(int n)
{
  stub_worker_threads = n;
  worker_set_threads(n);
}

/** Drain until \a want tasks have been delivered, or give up and fail. */
static void drain_until(int want)
{
  time_t deadline = time(0) + TEST_TIMEOUT;

  while (done_ran < want) {
    worker_drain();

    if (done_ran >= want)
      break;

    assert(time(0) < deadline && "timed out waiting for workers");
    usleep(1000);
  }
}

/* ---------------------------------------------------------------------- */

/** Double the number in wt_in.  Runs in a worker thread. */
static void work_double(struct WorkTask* task)
{
  int value = *(int*) task->wt_in;
  int* out = (int*) worker_alloc(sizeof(int));

  assert(0 != out);
  *out = value * 2;

  task->wt_out = out;
  task->wt_out_len = sizeof(int);
  task->wt_status = 0;

  pthread_mutex_lock(&counter_lock);
  work_ran++;
  pthread_mutex_unlock(&counter_lock);
}

/** Collect the result.  Runs in the main thread. */
static void done_collect(struct WorkTask* task)
{
  assert(0 == task->wt_status);
  assert(0 != task->wt_out);
  assert(sizeof(int) == task->wt_out_len);

  done_total += *(int*) task->wt_out;
  done_ran++;
}

/** Build a task carrying \a value. */
static struct WorkTask* task_for(int value)
{
  struct WorkTask* task = worker_task_new(work_double, done_collect);
  int* in;

  assert(0 != task);

  in = (int*) worker_alloc(sizeof(int));
  assert(0 != in);
  *in = value;

  task->wt_in = in;
  task->wt_in_len = sizeof(int);

  return task;
}

/* ---------------------------------------------------------------------- */

/** With the feature at zero there is no pool, and nothing pretends there is.
 *
 * This is the acceptance criterion for the whole design: a server that has
 * not asked for workers must behave exactly as it did before they existed.
 */
static void test_disabled_by_default(void)
{
  struct WorkTask* task = task_for(21);

  assert(!worker_enabled());
  assert(0 == worker_submit(task));
  assert(0 == worker_thread_count());
  assert(!stub_socket_live && "no pipe should have been opened");

  /* A refused submit leaves the task with the caller. */
  worker_task_free(task);

  printf("Passed: nothing exists until WORKER_THREADS says so\n");
}

/** Work crosses to a worker and the result comes back. */
static void test_round_trip(void)
{
  int i;

  counters_reset();
  pool_start(4);

  assert(worker_enabled());
  assert(4 == worker_thread_count());
  assert(stub_socket_live && "the wake pipe should be watched");

  for (i = 1; i <= 50; i++)
    assert(worker_submit(task_for(i)));

  drain_until(50);

  assert(50 == done_ran);
  assert(50 == work_count());
  /* 2 * (1 + ... + 50) */
  assert(2550 == done_total);
  assert(0 == worker_outstanding());
  assert(50 == worker_completed());

  printf("Passed: 50 tasks crossed the boundary and came back\n");
}

/** The queue has a bound and says no rather than growing without limit. */
static void test_queue_bound(void)
{
  struct WorkTask* task;
  unsigned int before;
  int accepted = 0;
  int i;

  counters_reset();

  /* One thread and a queue of four, so submissions outrun the pool. */
  stub_worker_queue_max = 4;
  pool_start(1);

  before = worker_rejected();

  for (i = 0; i < 200; i++) {
    task = task_for(i);
    if (worker_submit(task))
      accepted++;
    else
      worker_task_free(task);
  }

  assert(worker_rejected() > before && "the bound was never reached");
  assert(accepted < 200);

  drain_until(accepted);
  assert(accepted == done_ran);
  assert(0 == worker_outstanding());

  stub_worker_queue_max = 1024;

  printf("Passed: a full queue refuses work instead of growing\n");
}

/* ---------------------------------------------------------------------- */

/** A client identity for the numnick test. */
static struct Client test_client;
static struct User test_user;
static struct Client test_server;

/** A task remembers a client by numnick, and notices when it is gone. */
static void test_client_reference(void)
{
  struct WorkTask* task;

  counters_reset();
  pool_start(1);

  strcpy(test_server.cli_yxx, "AB");
  strcpy(test_client.cli_yxx, "AAA");
  test_client.cli_user = &test_user;
  test_user.server = &test_server;
  test_client.cli_firsttime = 1000;

  task = task_for(1);
  worker_task_set_client(task, &test_client);

  /* The client is here: the numnick resolves and the connect time agrees. */
  strcpy(stub_numnick, "ABAAA");
  stub_numnick_client = &test_client;
  assert(&test_client == worker_task_client(task));

  /* The client quit: nothing holds that numnick. */
  stub_numnick_client = 0;
  assert(0 == worker_task_client(task));

  /* Somebody else was handed the numnick.  This is the case a stored
   * struct Client* would get wrong, and get wrong silently.
   */
  stub_numnick_client = &test_client;
  test_client.cli_firsttime = 2000;
  assert(0 == worker_task_client(task));

  test_client.cli_firsttime = 1000;
  worker_task_set_client(task, 0);
  assert(0 == worker_task_client(task));

  stub_numnick_client = 0;
  worker_task_free(task);

  printf("Passed: a task holds a numnick, not a pointer\n");
}

/* ---------------------------------------------------------------------- */

/** A fake owner; worker.c only ever compares the pointer. */
static struct ModuleHandle* const OWNER = (struct ModuleHandle*) 0x1;
static struct ModuleHandle* const OTHER = (struct ModuleHandle*) 0x2;

/** Block until released, so a task can be caught mid-flight. */
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int gate_open;
static int gate_entered;

static void work_wait_at_gate(struct WorkTask* task)
{
  (void) task;

  pthread_mutex_lock(&gate_lock);
  gate_entered++;
  pthread_cond_broadcast(&gate_cond);
  while (!gate_open)
    pthread_cond_wait(&gate_cond, &gate_lock);
  pthread_mutex_unlock(&gate_lock);
}

/** Must never run: this task's owner went away. */
static void done_must_not_run(struct WorkTask* task)
{
  (void) task;
  assert(0 && "a cancelled task's done callback ran");
}

/** Unloading an owner takes its queued, running and finished work with it. */
static void test_owner_cancel(void)
{
  struct WorkTask* task;
  int i;

  counters_reset();
  pool_start(1);

  pthread_mutex_lock(&gate_lock);
  gate_open = 0;
  gate_entered = 0;
  pthread_mutex_unlock(&gate_lock);

  /* One task to occupy the single pool thread ... */
  task = worker_task_new(work_wait_at_gate, done_must_not_run);
  assert(worker_submit_owned(OWNER, task));

  /* ... so that the rest queue up behind it. */
  for (i = 0; i < 5; i++) {
    task = worker_task_new(work_wait_at_gate, done_must_not_run);
    assert(worker_submit_owned(OWNER, task));
  }

  /* And one from somebody else, which must survive all of this. */
  assert(worker_submit_owned(OTHER, task_for(7)));

  /* Wait until the first task really is inside a worker. */
  pthread_mutex_lock(&gate_lock);
  while (!gate_entered)
    pthread_cond_wait(&gate_cond, &gate_lock);
  /* Release it so worker_cancel_module() has something to wait for and then
   * gets it; a task that never returns would block the server, by design.
   */
  gate_open = 1;
  pthread_cond_broadcast(&gate_cond);
  pthread_mutex_unlock(&gate_lock);

  worker_cancel_module(OWNER);

  assert(0 == worker_module_tasks(OWNER));

  /* The other owner's task is untouched and still delivered. */
  drain_until(1);
  assert(1 == done_ran);
  assert(14 == done_total);

  printf("Passed: unloading an owner discards exactly its own work\n");
}

/* ---------------------------------------------------------------------- */

/** What the dedicated worker posts back. */
static int ticks_seen;

static void done_tick(struct WorkTask* task)
{
  (void) task;
  ticks_seen++;
}

/** A dedicated worker: posts once, then waits to be told to stop. */
static void ticker_main(struct Worker* worker, void* arg)
{
  struct WorkTask* task;
  char buf[1];

  assert(0 == strcmp((const char*) arg, "hello"));
  assert(0 == strcmp("ticker", worker_name(worker)));

  task = worker_task_new(0, done_tick);
  assert(0 != task);
  assert(worker_post(worker, task));

  /* Parked on the stop descriptor, which is the shape a real worker with a
   * socket of its own has: it never polls, it waits.
   */
  while (!worker_stopping(worker)) {
    if (read(worker_stop_fd(worker), buf, sizeof(buf)) <= 0)
      break;
  }
}

/** A dedicated worker runs, posts to the main thread, and stops on request. */
static void test_dedicated_worker(void)
{
  struct Worker* worker;
  time_t deadline;

  counters_reset();
  ticks_seen = 0;
  pool_start(2);

  worker = worker_spawn("ticker", ticker_main, (void*) "hello");
  assert(0 != worker);
  assert(1 == worker_dedicated_count());

  deadline = time(0) + TEST_TIMEOUT;
  while (!ticks_seen) {
    worker_drain();
    assert(time(0) < deadline && "the dedicated worker never posted");
    usleep(1000);
  }

  /* Joins the thread; if the stop descriptor did not work this hangs, and
   * ctest's own timeout is what catches it.
   */
  worker_stop(worker);
  assert(0 == worker_dedicated_count());

  printf("Passed: a dedicated worker posts back and stops when asked\n");
}

/** Stopping a worker the server already stopped is a no-op, not a crash.
 *
 * This is what every module that stops its own thread from mi_fini does:
 * worker_cancel_module() runs first and frees the worker, and mi_fini then
 * hands back the handle it kept.  Used to be a double free -- the
 * ownership check read w_owner out of the freed block, found it intact,
 * and stopped the worker again.
 */
static void test_stale_stop(void)
{
  struct Worker* worker;

  counters_reset();
  ticks_seen = 0;
  pool_start(1);

  worker = worker_spawn_owned(OWNER, "ticker", ticker_main, (void*) "hello");
  assert(0 != worker);
  assert(1 == worker_module_workers(OWNER));

  /* What module_unload_internal() does before mi_fini. */
  worker_cancel_module(OWNER);
  assert(0 == worker_module_workers(OWNER));
  assert(0 == worker_dedicated_count());

  /* What mi_fini does next, with the handle the module still holds.  The
   * pointer is compared against the list and never read through.
   */
  assert(0 == worker_stop_owned(OWNER, worker));
  worker_stop(worker);
  assert(0 == worker_dedicated_count());

  /* And what it may do at any time, regardless of state. */
  assert(0 == worker_stop_owned(OWNER, 0));

  printf("Passed: stopping a worker that is already gone does nothing\n");
}

/** With the pool off, a spawn is refused rather than quietly ignored. */
static void test_spawn_refused_when_off(void)
{
  pool_start(0);

  assert(!worker_enabled());
  assert(0 == worker_spawn("nope", ticker_main, (void*) "hello"));

  printf("Passed: WORKER_THREADS=0 refuses dedicated workers too\n");
}

/** Turning the feature off stops every thread and every dedicated worker. */
static void test_switch_off(void)
{
  struct Worker* worker;

  counters_reset();
  pool_start(3);
  assert(3 == worker_thread_count());

  worker = worker_spawn("ticker", ticker_main, (void*) "hello");
  assert(0 != worker);

  pool_start(0);

  assert(0 == worker_thread_count());
  assert(0 == worker_dedicated_count());
  assert(!worker_enabled());

  /* The switch freed the worker too, and the module that spawned it does
   * not know that; its stop must land harmlessly.
   */
  worker_stop(worker);
  assert(0 == worker_dedicated_count());

  printf("Passed: WORKER_THREADS=0 is a real off switch\n");
}

/** Resizing the pool up and down keeps working. */
static void test_resize(void)
{
  int i;

  counters_reset();
  pool_start(1);
  assert(1 == worker_thread_count());

  pool_start(8);
  assert(8 == worker_thread_count());

  for (i = 0; i < 20; i++)
    assert(worker_submit(task_for(1)));

  pool_start(2);
  assert(2 == worker_thread_count());

  drain_until(20);
  assert(20 == done_ran);
  assert(40 == done_total);

  printf("Passed: the pool resizes without losing work\n");
}

/* ---------------------------------------------------------------------- */

int main(int argc, char* argv[])
{
  (void) argc;
  (void) argv;

  /* The real entry point, with the stub standing in for the feature: from
   * here on the subsystem knows the configuration has been read, which is
   * what makes a spawn get refused rather than held.
   */
  stub_worker_threads = 0;
  worker_init();

  test_disabled_by_default();
  test_spawn_refused_when_off();
  test_round_trip();
  test_queue_bound();
  test_client_reference();
  test_owner_cancel();
  test_dedicated_worker();
  test_stale_stop();
  test_resize();
  test_switch_off();

  worker_shutdown();

  printf("Done.\n");
  return 0;
}
