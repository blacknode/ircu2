/*
 * IRC - Internet Relay Chat, modules/workers/http/http_module.c
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
 * @brief The HTTP provider: what the server sees of it.
 *
 * The implementation behind http.h.  It registers itself as the provider,
 * and from then on every route any module claims -- without any of them
 * owning a socket or knowing that this module exists -- is served from
 * here.
 *
 * @code
 *   F:HTTP_PORT:8080
 *   F:HTTP_BIND:127.0.0.1
 *   F:HTTP_MAX_CLIENTS:64
 *
 *   Module { name = "http"; };
 * @endcode
 *
 * and set @c WORKER_THREADS to at least one, because the listener is a
 * thread.  @c HTTP_PORT is 0 by default and at 0 nothing listens: a
 * server does not open a second port because a module was loaded.
 *
 * @section httpd_threads Which thread is which
 *
 * The socket belongs to the worker and the routes belong to the main
 * thread, and neither ever reaches across.  A request goes up through
 * worker_post(), which runs httpd_dispatch_done() in the main thread,
 * where http_dispatch() finds the handler.  An answer comes back down
 * through this module's own queue -- a mutex, a list and a self-pipe --
 * because the worker is asleep in poll() on its sockets and a condition
 * variable would be no use to it.
 *
 * Both directions carry a @c struct @c HttpXfer and nothing else: bytes,
 * no pointer into anything either side might free, and a connection named
 * by an #http_req_t rather than by its address.  That is the same rule
 * worker_task_set_client() follows for clients, for the same reason.
 *
 * @section httpd_port The listener follows the configuration
 *
 * The thread is started from @c HOOK_CONFIG_LOADED, not from @c mi_init,
 * because @c mi_init runs in the middle of the parse and the features are
 * not final yet.  A rehash that changes the port, the address or the
 * limit restarts it; one that does not, does not -- restarting a listener
 * for nothing would drop every connection in flight.
 */
#include "config.h"

#include "http_priv.h"

#include "client.h"
#include "hooks.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_string.h"
#include "module.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/** Most answers that may be waiting for the worker at once.
 *
 * A bound rather than a policy: if the worker is not draining this, it is
 * wedged, and the right thing to do with the next answer is drop it and
 * let the connection time out -- not grow a list nobody is reading.
 */
#define HTTPD_QUEUE_MAX 256

struct ModuleHandle* httpd_mod;
struct HttpQueue httpd_queue;

/** The listening thread, or NULL. */
static struct Worker* httpd_worker;

/** What it was started against, so a rehash knows whether to restart it. */
static int httpd_port;
static char httpd_bind[64];
static int httpd_max;

/* ------------------------------------------------------------------------
 * The queue.  The one thing in this module both threads touch.
 * ------------------------------------------------------------------------ */

/** Put a descriptor in non-blocking mode. */
static void httpd_nb(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);

  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/** Set up an empty queue, with its wake pipe. */
static void httpd_queue_init(struct HttpQueue* q)
{
  memset(q, 0, sizeof(*q));
  pthread_mutex_init(&q->hq_lock, 0);

  if (pipe(q->hq_wake) < 0) {
    q->hq_wake[0] = q->hq_wake[1] = -1;
    return;
  }

  /* Both ends: the writer is the main thread, which may never block, and
   * the reader is the worker, which drains until it would. */
  httpd_nb(q->hq_wake[0]);
  httpd_nb(q->hq_wake[1]);
}

/** Throw away everything still queued. */
static void httpd_queue_flush(struct HttpQueue* q)
{
  struct HttpXferNode* node;
  struct HttpXferNode* next;

  pthread_mutex_lock(&q->hq_lock);
  node = q->hq_head;
  q->hq_head = q->hq_tail = 0;
  pthread_mutex_unlock(&q->hq_lock);

  while (node) {
    next = node->hxn_next;
    worker_free(node->hxn_xfer);
    worker_free(node);
    node = next;
  }
}

/** Tear one down.  Every thread that used it must have stopped. */
static void httpd_queue_destroy(struct HttpQueue* q)
{
  httpd_queue_flush(q);

  if (q->hq_wake[0] >= 0)
    close(q->hq_wake[0]);
  if (q->hq_wake[1] >= 0)
    close(q->hq_wake[1]);

  q->hq_wake[0] = q->hq_wake[1] = -1;

  pthread_mutex_destroy(&q->hq_lock);
}

int httpd_queue_push(struct HttpQueue* q, struct HttpXfer* xfer)
{
  struct HttpXferNode* node;
  unsigned int depth = 0;
  struct HttpXferNode* walk;

  if (!(node = (struct HttpXferNode*) worker_alloc(sizeof(*node))))
    return 0;

  node->hxn_next = 0;
  node->hxn_xfer = xfer;

  pthread_mutex_lock(&q->hq_lock);

  for (walk = q->hq_head; walk && depth < HTTPD_QUEUE_MAX; walk = walk->hxn_next)
    depth++;

  if (depth >= HTTPD_QUEUE_MAX) {
    pthread_mutex_unlock(&q->hq_lock);
    worker_free(node);
    return 0;
  }

  if (q->hq_tail)
    q->hq_tail->hxn_next = node;
  else
    q->hq_head = node;

  q->hq_tail = node;

  pthread_mutex_unlock(&q->hq_lock);

  /* One byte, and what happens to it does not matter: a full pipe means
   * the worker has wakeups it has not read yet, which is exactly what
   * this was for. */
  if (q->hq_wake[1] >= 0) {
    ssize_t ignored = write(q->hq_wake[1], "!", 1);
    (void) ignored;
  }

  return 1;
}

struct HttpXfer* httpd_queue_pop(struct HttpQueue* q)
{
  struct HttpXferNode* node;
  struct HttpXfer* xfer;

  pthread_mutex_lock(&q->hq_lock);

  if ((node = q->hq_head)) {
    q->hq_head = node->hxn_next;
    if (!q->hq_head)
      q->hq_tail = 0;
  }

  pthread_mutex_unlock(&q->hq_lock);

  if (!node)
    return 0;

  xfer = node->hxn_xfer;
  worker_free(node);

  return xfer;
}

/* ------------------------------------------------------------------------
 * Going up: a request the worker parsed, handed to whoever claimed it.
 * ------------------------------------------------------------------------ */

/** Send an answer this module wrote itself down to the worker.
 *
 * Used for the two answers the core never produces: nothing claimed the
 * route, and the request was abandoned.
 */
static void httpd_answer_now(http_req_t id, int status, const char* text)
{
  struct HttpXfer* out;

  if (!(out = (struct HttpXfer*) worker_alloc(sizeof(*out))))
    return;

  memset(out, 0, sizeof(*out));
  out->hx_id = id;
  out->hx_status = status;
  strcpy(out->hx_type, "text/plain");

  ircd_strncpy(out->hx_body, text, sizeof(out->hx_body) - 1);
  out->hx_replylen = strlen(out->hx_body);

  if (!httpd_queue_push(&httpd_queue, out))
    worker_free(out);
}

void httpd_dispatch_done(struct WorkTask* task)
{
  struct HttpXfer* xfer = (struct HttpXfer*) task->wt_in;
  struct HttpRequest req;

  if (!xfer)
    return;

  memset(&req, 0, sizeof(req));
  req.hreq_method = xfer->hx_method;
  req.hreq_path = xfer->hx_path;
  req.hreq_query = xfer->hx_query;
  req.hreq_remote = xfer->hx_remote;
  req.hreq_tls = 0;
  req.hreq_nheaders = xfer->hx_nheaders;
  req.hreq_headers = xfer->hx_headers;
  req.hreq_body = xfer->hx_body;
  req.hreq_bodylen = xfer->hx_bodylen;

  /* Negative means nothing claimed the route and the core is holding
   * nothing: the 404 is this module's to send, and if it did not send one
   * the connection would sit there until it was reaped. */
  if (http_dispatch(&req, xfer->hx_id) < 0)
    httpd_answer_now(xfer->hx_id, 404, "404 Not Found\n");

  /* xfer itself is the task's payload; the server frees it. */
}

/* ------------------------------------------------------------------------
 * Coming down: the provider.
 * ------------------------------------------------------------------------ */

/** Send an answer for a request the core dispatched.  Main thread. */
static void httpd_provider_respond(http_req_t id,
                                   const struct HttpResponse* res)
{
  struct HttpXfer* out;
  unsigned int i;
  size_t len;

  if (!(out = (struct HttpXfer*) worker_alloc(sizeof(*out))))
    return;

  memset(out, 0, sizeof(*out));
  out->hx_id = id;
  out->hx_status = res->hres_status;

  ircd_strncpy(out->hx_type, res->hres_type[0] ? res->hres_type
                                               : "application/octet-stream",
               sizeof(out->hx_type) - 1);

  for (i = 0; i < res->hres_nheaders && i < HTTP_HEADERS_MAX; i++)
    out->hx_reply[out->hx_nreply++] = res->hres_headers[i];

  len = res->hres_bodylen;
  if (len > HTTPD_BODY_MAX)
    len = HTTPD_BODY_MAX;

  if (len && res->hres_body)
    memcpy(out->hx_body, res->hres_body, len);

  out->hx_replylen = len;

  if (!httpd_queue_push(&httpd_queue, out))
    worker_free(out);
}

/** Forget a request that will never be answered.  Main thread.
 *
 * The connection is not this thread's to close, so what goes down is an
 * answer: 503, which is what the peer is actually looking at -- the
 * module that was going to answer is gone.
 */
static void httpd_provider_cancel(http_req_t id)
{
  httpd_answer_now(id, 503, "503 Service Unavailable\n");
}

static const struct HttpProvider httpd_provider = {
  "http", httpd_provider_respond, httpd_provider_cancel
};

/* ------------------------------------------------------------------------
 * The listener.
 * ------------------------------------------------------------------------ */

/** Stop the listening thread, if there is one. */
static void httpd_stop(void)
{
  if (httpd_worker) {
    module_stop_worker(httpd_mod, httpd_worker);
    httpd_worker = 0;
  }

  /* The thread is joined, so nobody else is looking at the queue and what
   * is in it is an answer for a socket that has just been closed. */
  httpd_queue_flush(&httpd_queue);

  httpd_port = 0;
  httpd_bind[0] = '\0';
  httpd_max = 0;
}

/** Start or restart the listening thread against the current features. */
static void httpd_start(void)
{
  int port = feature_int(FEAT_HTTP_PORT);
  const char* bind = feature_str(FEAT_HTTP_BIND);
  int max = feature_int(FEAT_HTTP_MAX_CLIENTS);
  void* args;

  if (!bind)
    bind = "";

  if (port <= 0 || port > 65535) {
    if (httpd_worker) {
      log_write(LS_SYSTEM, L_INFO, 0, "http: HTTP_PORT is 0; nothing listens");
      httpd_stop();
    }
    return;
  }

  /* A rehash that did not touch any of this must not drop the listener:
   * every connection on it would go with it, for nothing. */
  if (httpd_worker && port == httpd_port && max == httpd_max
      && !strcmp(bind, httpd_bind))
    return;

  httpd_stop();

  if (!(args = httpd_args_new(port, bind, max)))
    return;

  httpd_worker = module_spawn_worker(httpd_mod, "http", httpd_worker_main,
                                     args);

  if (!httpd_worker) {
    worker_free(args);
    log_write(LS_SYSTEM, L_WARNING, 0,
              "http: no listening thread; is WORKER_THREADS zero?");
    return;
  }

  httpd_port = port;
  httpd_max = max;
  ircd_strncpy(httpd_bind, bind, sizeof(httpd_bind) - 1);
}

/** The configuration was read in full: the features are final now. */
static enum HookResult httpd_on_config(struct HookContext* ctx, void* user)
{
  (void) ctx;
  (void) user;
  httpd_start();
  return HOOK_CONTINUE;
}

/* ------------------------------------------------------------------------
 * The module.
 * ------------------------------------------------------------------------ */

/** Set up on load.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int httpd_init(struct ModuleHandle* mod)
{
  httpd_mod = mod;

  httpd_queue_init(&httpd_queue);

  if (!module_add_http_provider(mod, &httpd_provider)) {
    httpd_queue_destroy(&httpd_queue);
    httpd_mod = 0;
    return -1;
  }

  if (!module_add_hook(mod, HOOK_CONFIG_LOADED, httpd_on_config,
                       HOOK_PRIORITY_DEFAULT, NULL)) {
    module_del_http_provider(mod);
    httpd_queue_destroy(&httpd_queue);
    httpd_mod = 0;
    return -1;
  }

  /* Loaded from a Module{} block this runs mid-parse, with HTTP_PORT not
   * necessarily read yet; HOOK_CONFIG_LOADED will come.  Loaded by an
   * operator the configuration is complete and nothing else will call. */
  if (module_loaded_by(mod))
    httpd_start();

  return 0;
}

/** Tear down on unload.
 * @param[in] mod Handle for this module.
 */
static void httpd_fini(struct ModuleHandle* mod)
{
  /* The provider goes first: every request in flight is failed while
   * there is still a thread to hear about it. */
  module_del_http_provider(mod);

  httpd_stop();
  httpd_queue_destroy(&httpd_queue);

  httpd_mod = 0;
}

/** Re-read the configuration.
 *
 * Nothing to do here: mi_rehash runs mid-parse like mi_init, and
 * HOOK_CONFIG_LOADED comes after, which is where the listener is
 * reconciled against what the file turned out to say.
 */
static void httpd_rehash(struct ModuleHandle* mod)
{
  (void) mod;
}

/** Module description. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "http",
  "1.0",
  "ircu2",
  "HTTP provider (listener on a thread of its own)",
  httpd_init,
  httpd_fini,
  httpd_rehash
};
