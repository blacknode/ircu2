/*
 * IRC - Internet Relay Chat, ircd/http_server.c
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
 * @brief The HTTP listener: Mongoose, on a worker thread.
 *
 * The only file in the tree that knows Mongoose exists.  Everything else
 * sees include/http.h -- routes, a request, a response -- and could not
 * tell what is underneath.
 *
 * @section httpd_why Why a library and not our own
 *
 * This replaced a hand-written parser and poll() loop that worked.  The
 * argument for replacing it is not that it was broken; it is that HTTP is
 * a protocol where being *nearly* right is a security bug, and the list
 * of ways to get framing wrong -- chunked encoding, folded headers,
 * pipelining, Content-Length against Transfer-Encoding -- is a list
 * somebody else has already walked, with fuzzers, for twenty years.
 * Mongoose is also where TLS, WebSockets and SSE come from without any of
 * them being written here.
 *
 * The rule that follows: ircd/mongoose/ is upstream's, never edited.  A
 * local patch would give away the only thing this is for.
 *
 * @section httpd_threads Which thread is which
 *
 * Mongoose has an event loop of its own, and the server has one already.
 * Rather than drive @c mg_mgr_poll() from a timer -- which would either
 * busy-poll or add latency, and would put a whole HTTP stack on the
 * thread that must never block -- it runs on a dedicated worker.  That is
 * what include/worker.h is for.
 *
 *   - **up**: @c MG_EV_HTTP_MSG copies the request into a @c struct
 *     @c HttpXfer and worker_post()s it.  The task's @c wt_done runs
 *     http_dispatch() in the main thread.
 *   - **down**: the main thread hands the answer to mg_wakeup(), which
 *     Mongoose documents as safe from any thread and which wakes the
 *     poll.  That is why there is no queue and no self-pipe here: the
 *     library already has one.
 *
 * Across that line travels a struct of **bytes** and a connection id --
 * @c mg_connection::id, an integer Mongoose guarantees unique -- never a
 * pointer.  A connection that went away while its answer was being worked
 * out simply is not found, which is the ordinary case for a client that
 * pressed stop.
 *
 * Nothing on the worker touches core state: no @c struct @c Client, no
 * @c CurrentTime, no @c MyMalloc(), no @c log_write(), no @c sendto_*.
 * Mongoose's own logging is compiled out for the same reason.
 */
#include "config.h"

#include "http_server.h"

#include "http.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "worker.h"

#include "mongoose/mongoose.h"

#include <string.h>

/** How long a poll waits, in milliseconds.
 *
 * The upper bound on noticing worker_stopping(); short enough that a
 * rehash is not visibly slow, long enough that an idle server is not
 * spinning.  Answers do not wait for it -- mg_wakeup() interrupts the
 * poll.
 */
#define HTTPD_POLL_MS 200

/** Longest Content-Type or header value carried, and the longest bind
 * address and file path this reads from the configuration. */
#define HTTPD_PATHLEN 255

/* ------------------------------------------------------------------- *
 * What crosses between the threads                                    *
 * ------------------------------------------------------------------- */

/** A request on its way up, or an answer on its way down.
 *
 * worker_alloc()ed, and holding only bytes: no pointer into anything
 * either side might free, and the connection named by its Mongoose id.
 */
struct HttpXfer {
  unsigned long hx_conn;                  /**< mg_connection::id. */

  /* Going up. */
  char   hx_method[HTTP_METHOD_MAX + 1];
  char   hx_path[HTTP_PATH_MAX + 1];
  char   hx_query[HTTP_QUERY_MAX + 1];
  char   hx_remote[64];
  int    hx_tls;
  unsigned int hx_nheaders;
  struct HttpHeader hx_headers[HTTP_HEADERS_MAX];
  size_t hx_bodylen;

  /* Coming down. */
  int    hx_status;
  char   hx_type[HTTP_HVALUE_MAX + 1];
  unsigned int hx_nreply;
  struct HttpHeader hx_reply[HTTP_HEADERS_MAX];
  size_t hx_replylen;

  char   hx_body[HTTP_BODY_MAX + 1];      /**< Request, then reply. */
};

/** What the thread is started with.  Read in the main thread, where the
 * features live, and handed over: a worker never reads a feature. */
struct HttpdArgs {
  char ha_url[HTTPD_PATHLEN + 1];         /**< "http://0.0.0.0:8080". */
  char ha_cert[HTTPD_PATHLEN + 1];        /**< PEM path, or "". */
  char ha_key[HTTPD_PATHLEN + 1];
  int  ha_max;                            /**< Connections at once. */
  int  ha_tls;                            /**< The URL says https. */
};

/** What the worker tells the main thread once it has tried to listen. */
struct HttpdReady {
  int  hr_ok;
  char hr_error[256];
  char hr_url[HTTPD_PATHLEN + 1];
};

/* ------------------------------------------------------------------- *
 * State                                                               *
 * ------------------------------------------------------------------- */

/** The manager.  Written by the worker before it reports ready, read by
 * mg_wakeup() from the main thread, and freed by the worker after it has
 * been joined -- so the main thread only ever touches it while the
 * transport is installed, which is strictly inside that window. */
static struct mg_mgr httpd_mgr;

/** The thread, or NULL.  Main thread's; the worker uses #httpd_self. */
static struct Worker* httpd_worker;

/** The same thread, as the worker knows it.
 *
 * Not #httpd_worker: the main thread forgets that one before it asks the
 * thread to stop, and a worker_post() to a NULL worker while the thread
 * is still winding down would be a crash at shutdown.  This one is
 * written once by the thread and read only by it.
 */
static struct Worker* httpd_self;

/** What it was started against, so a rehash knows whether to restart. */
static struct HttpdArgs httpd_current;

/** Non-zero once mg_wakeup() may be called. */
static int httpd_up;

/** Worker-side: how many connections are open, and the cap. */
static int httpd_conns;
static int httpd_conn_max;

/** Worker-side: the TLS material, read once at start-up. */
static struct mg_str httpd_cert;
static struct mg_str httpd_key;

static void http_server_deliver(struct WorkTask* task);
static void http_server_ready(struct WorkTask* task);
static void http_server_respond(http_req_t id, const struct HttpResponse* res);
static void http_server_cancel(http_req_t id);

/** The transport the core answers through.  Defined below. */
static const struct HttpTransport httpd_transport;

/* ------------------------------------------------------------------- *
 * The worker                                                          *
 * ------------------------------------------------------------------- */

/** Copy an mg_str into a fixed buffer, truncating and NUL-terminating. */
static void httpd_copy(char* dst, size_t dstlen, struct mg_str s)
{
  size_t n = s.len < dstlen - 1 ? s.len : dstlen - 1;

  if (n && s.buf)
    memcpy(dst, s.buf, n);

  dst[n] = '\0';
}

/** Decode the request path into \a dst, or refuse it.
 *
 * Mongoose does the framing; what it deliberately does not do is decide
 * what a path means, and this is that decision.  Two things are refused
 * outright rather than decoded:
 *
 *   - an encoded separator (@c %2F, @c %5C).  Routing happens on the
 *     decoded path, so it would turn what the client wrote as data into a
 *     separator and with it reach a prefix route it is not under.
 *   - an encoded NUL (@c %00), which shortens the path for whoever reads
 *     it with str*() next.
 *
 * And after decoding, a path holding @c ".." or a control byte is
 * refused.  Nothing in this file opens a file -- but the path is handed
 * to modules and one of them will, and the check belongs where the path
 * is first believed rather than in each of them.
 *
 * @return Non-zero if \a dst holds a path this server will route on.
 */
static int httpd_path(char* dst, size_t dstlen, struct mg_str uri)
{
  const char* p;
  int n;
  size_t i;

  for (i = 0; uri.buf && i + 2 < uri.len; i++) {
    if (uri.buf[i] != '%')
      continue;

    if (!strncasecmp(uri.buf + i + 1, "2f", 2)
        || !strncasecmp(uri.buf + i + 1, "5c", 2)
        || !strncmp(uri.buf + i + 1, "00", 2))
      return 0;
  }

  n = mg_url_decode(uri.buf, uri.len, dst, dstlen, 0);

  if (n < 0)
    return 0;

  dst[n] = '\0';

  if (dst[0] != '/')
    return 0;

  for (p = dst; *p; p++) {
    if ((unsigned char) *p < 0x20)
      return 0;
    if (p[0] == '.' && p[1] == '.')
      return 0;
  }

  return 1;
}

/** Send a status-only answer from the worker itself.  Worker thread. */
static void httpd_status(struct mg_connection* c, int status,
                         const char* text)
{
  mg_http_reply(c, status, "Content-Type: text/plain\r\n", "%s\n", text);
}

/** Turn a parsed request into a transfer and post it.  Worker thread.
 * @return Non-zero if it went up.
 */
static int httpd_post_request(struct mg_connection* c,
                              struct mg_http_message* hm)
{
  struct WorkTask* task;
  struct HttpXfer* xfer;
  size_t i;

  if (hm->body.len > HTTP_BODY_MAX) {
    httpd_status(c, 413, "413 Payload Too Large");
    return 1;
  }

  if (!(xfer = (struct HttpXfer*) worker_alloc(sizeof(*xfer))))
    return 0;

  memset(xfer, 0, sizeof(*xfer));
  xfer->hx_conn = c->id;
  xfer->hx_tls = c->is_tls;

  httpd_copy(xfer->hx_method, sizeof(xfer->hx_method), hm->method);
  httpd_copy(xfer->hx_query, sizeof(xfer->hx_query), hm->query);

  /* The query is carried raw: "+" means a space there and nowhere else,
   * and a server that decided would be deciding for every route at once. */

  if (!httpd_path(xfer->hx_path, sizeof(xfer->hx_path), hm->uri)) {
    worker_free(xfer);
    httpd_status(c, 400, "400 Bad Request");
    return 1;
  }

  mg_snprintf(xfer->hx_remote, sizeof(xfer->hx_remote), "%M",
              mg_print_ip, &c->rem);

  for (i = 0; i < MG_MAX_HTTP_HEADERS; i++) {
    if (!hm->headers[i].name.len)
      break;
    if (xfer->hx_nheaders >= HTTP_HEADERS_MAX)
      break;

    httpd_copy(xfer->hx_headers[xfer->hx_nheaders].hh_name,
               sizeof(xfer->hx_headers[0].hh_name), hm->headers[i].name);
    httpd_copy(xfer->hx_headers[xfer->hx_nheaders].hh_value,
               sizeof(xfer->hx_headers[0].hh_value), hm->headers[i].value);
    xfer->hx_nheaders++;
  }

  if (hm->body.len) {
    memcpy(xfer->hx_body, hm->body.buf, hm->body.len);
    xfer->hx_bodylen = hm->body.len;
  }
  xfer->hx_body[xfer->hx_bodylen] = '\0';

  if (!(task = worker_task_new(0, http_server_deliver))) {
    worker_free(xfer);
    return 0;
  }

  task->wt_in = xfer;

  if (!worker_post(httpd_self, task)) {
    /* The main thread is not draining.  Answering 503 here beats blocking
     * this thread, which is the one thing it may not do. */
    task->wt_in = 0;
    worker_task_free(task);
    worker_free(xfer);
    return 0;
  }

  /* Mongoose would otherwise consider the request finished and, on a
   * keep-alive connection, start reading the next one into a buffer
   * nobody is watching.  is_resp says the answer is still being made. */
  c->is_resp = 1;

  return 1;
}

/** Write an answer that came back from the main thread.  Worker thread. */
static void httpd_send_answer(struct mg_connection* c,
                              const struct HttpXfer* xfer)
{
  char headers[HTTP_HEADERS_MAX * (HTTP_HNAME_MAX + HTTP_HVALUE_MAX + 4)
               + HTTP_HVALUE_MAX + 32];
  size_t n = 0;
  size_t wrote;
  unsigned int i;

  /* mg_snprintf() returns what it *would* have written, the way snprintf
   * does, so every step is checked against the room left rather than
   * added to a cursor that could walk off the end. */
  wrote = mg_snprintf(headers, sizeof(headers), "Content-Type: %s\r\n",
                      xfer->hx_type[0] ? xfer->hx_type : "text/plain");
  n = wrote < sizeof(headers) ? wrote : sizeof(headers) - 1;

  for (i = 0; i < xfer->hx_nreply; i++) {
    /* A newline in a header a module wrote is how one response becomes
     * two, and the second one is whatever the module was handed.  The
     * header is dropped; the answer still goes. */
    if (strpbrk(xfer->hx_reply[i].hh_name, "\r\n:")
        || strpbrk(xfer->hx_reply[i].hh_value, "\r\n"))
      continue;

    wrote = mg_snprintf(headers + n, sizeof(headers) - n, "%s: %s\r\n",
                        xfer->hx_reply[i].hh_name,
                        xfer->hx_reply[i].hh_value);

    if (wrote >= sizeof(headers) - n) {
      headers[n] = '\0';       /* no room: this one and the rest go */
      break;
    }

    n += wrote;
  }

  mg_http_reply(c, xfer->hx_status ? xfer->hx_status : 200, headers, "%.*s",
                (int) xfer->hx_replylen, xfer->hx_body);

  c->is_resp = 0;
}

/** Mongoose's event handler.  Worker thread, for every connection. */
static void httpd_event(struct mg_connection* c, int ev, void* ev_data)
{
  switch (ev) {
  case MG_EV_ACCEPT:
    httpd_conns++;

    /* Over the limit the connection is closed rather than queued.  A
     * queue of connections nobody is reading is a polite way of running
     * the server out of descriptors. */
    if (httpd_conn_max > 0 && httpd_conns > httpd_conn_max) {
      c->is_closing = 1;
      break;
    }

    if (c->is_tls) {
      struct mg_tls_opts opts;

      memset(&opts, 0, sizeof(opts));
      opts.cert = httpd_cert;
      opts.key = httpd_key;
      mg_tls_init(c, &opts);
    }
    break;

  case MG_EV_CLOSE:
    if (!c->is_listening)
      httpd_conns--;
    break;

  case MG_EV_HTTP_MSG:
    if (!httpd_post_request(c, (struct mg_http_message*) ev_data))
      httpd_status(c, 503, "503 Service Unavailable");
    break;

  case MG_EV_WAKEUP: {
    /* mg_wakeup() copies the bytes it is given, so what is given is a
     * pointer: the answer itself stays where the main thread put it and
     * is freed here, once. */
    struct mg_str* m = (struct mg_str*) ev_data;
    struct HttpXfer* xfer;

    if (m->len != sizeof(xfer))
      break;

    memcpy(&xfer, m->buf, sizeof(xfer));

    if (xfer) {
      if (xfer->hx_status)
        httpd_send_answer(c, xfer);
      else
        c->is_draining = 1;     /* nobody will answer; let it go */

      worker_free(xfer);
    }
    break;
  }

  default:
    break;
  }
}

/** Tell the main thread how the listen went.  Worker thread. */
static void httpd_report(const char* error, const char* url)
{
  struct WorkTask* task;
  struct HttpdReady* ready;

  if (!(ready = (struct HttpdReady*) worker_alloc(sizeof(*ready))))
    return;

  memset(ready, 0, sizeof(*ready));
  ready->hr_ok = error ? 0 : 1;

  if (error) {
    strncpy(ready->hr_error, error, sizeof(ready->hr_error) - 1);
    ready->hr_error[sizeof(ready->hr_error) - 1] = '\0';
  }

  strncpy(ready->hr_url, url, sizeof(ready->hr_url) - 1);
  ready->hr_url[sizeof(ready->hr_url) - 1] = '\0';

  if (!(task = worker_task_new(0, http_server_ready))) {
    worker_free(ready);
    return;
  }

  task->wt_in = ready;

  if (!worker_post(httpd_self, task)) {
    task->wt_in = 0;
    worker_task_free(task);
    worker_free(ready);
  }
}

/** The thread.  Listen, poll, and take the manager down after. */
static void httpd_main(struct Worker* worker, void* arg)
{
  struct HttpdArgs* args = (struct HttpdArgs*) arg;
  struct mg_connection* listener;

  httpd_self = worker;
  httpd_conns = 0;
  httpd_conn_max = args->ha_max;
  memset(&httpd_cert, 0, sizeof(httpd_cert));
  memset(&httpd_key, 0, sizeof(httpd_key));

  mg_mgr_init(&httpd_mgr);

  if (!mg_wakeup_init(&httpd_mgr)) {
    httpd_report("could not create the wakeup pipe", args->ha_url);
    mg_mgr_free(&httpd_mgr);
    worker_free(args);
    return;
  }

  if (args->ha_tls) {
    httpd_cert = mg_file_read(&mg_fs_posix, args->ha_cert);
    httpd_key = mg_file_read(&mg_fs_posix, args->ha_key);

    if (!httpd_cert.buf || !httpd_key.buf) {
      httpd_report("could not read HTTP_TLS_CERT or HTTP_TLS_KEY",
                   args->ha_url);
      mg_free(httpd_cert.buf);
      mg_free(httpd_key.buf);
      mg_mgr_free(&httpd_mgr);
      worker_free(args);
      return;
    }
  }

  listener = mg_http_listen(&httpd_mgr, args->ha_url, httpd_event, NULL);

  if (!listener) {
    httpd_report("could not listen (address in use, or not permitted)",
                 args->ha_url);
    mg_free(httpd_cert.buf);
    mg_free(httpd_key.buf);
    mg_mgr_free(&httpd_mgr);
    worker_free(args);
    return;
  }

  httpd_report(NULL, args->ha_url);
  worker_free(args);

  while (!worker_stopping(worker))
    mg_mgr_poll(&httpd_mgr, HTTPD_POLL_MS);

  /* The main thread cleared the transport before asking this thread to
   * stop and waits for it here, so no mg_wakeup() can be in flight. */
  mg_mgr_free(&httpd_mgr);
  mg_free(httpd_cert.buf);
  mg_free(httpd_key.buf);
  memset(&httpd_cert, 0, sizeof(httpd_cert));
  memset(&httpd_key, 0, sizeof(httpd_key));
  httpd_self = 0;
}

/* ------------------------------------------------------------------- *
 * The main thread's side                                              *
 * ------------------------------------------------------------------- */

static void http_server_deliver(struct WorkTask* task)
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
  req.hreq_tls = xfer->hx_tls;
  req.hreq_nheaders = xfer->hx_nheaders;
  req.hreq_headers = xfer->hx_headers;
  req.hreq_body = xfer->hx_body;
  req.hreq_bodylen = xfer->hx_bodylen;

  /* Negative means nothing claimed the route and the core is holding
   * nothing: the 404 is this file's to send, or the connection would sit
   * there until the client gave up. */
  if (http_dispatch(&req, xfer->hx_conn) < 0) {
    static char body[] = "404 Not Found\n";
    struct HttpResponse res;

    /* Not http_response_set(): that copies into a buffer the core owns,
     * and this response is a local with none.  The body is a literal
     * nothing writes to. */
    memset(&res, 0, sizeof(res));
    res.hres_status = 404;
    ircd_strncpy(res.hres_type, "text/plain", sizeof(res.hres_type) - 1);
    res.hres_body = body;
    res.hres_bodylen = sizeof(body) - 1;

    http_server_respond(xfer->hx_conn, &res);
  }

  /* xfer is the task's payload; the server frees it. */
}

static void http_server_ready(struct WorkTask* task)
{
  struct HttpdReady* ready = (struct HttpdReady*) task->wt_in;

  if (!ready)
    return;

  if (!ready->hr_ok) {
    log_write(LS_SYSTEM, L_ERROR, 0, "http: %s: %s", ready->hr_url,
              ready->hr_error);

    /* The thread has already returned.  Forget the handle so that a
     * rehash tries again rather than believing a listener is up. */
    httpd_worker = 0;
    httpd_up = 0;
    memset(&httpd_current, 0, sizeof(httpd_current));
    return;
  }

  httpd_up = 1;
  http_set_transport(&httpd_transport);

  log_write(LS_SYSTEM, L_INFO, 0, "http: listening on %s", ready->hr_url);
}

/** Hand an answer down to the worker.  Main thread. */
static void http_server_respond(http_req_t id, const struct HttpResponse* res)
{
  struct HttpXfer* xfer;
  unsigned int i;
  size_t len;

  if (!httpd_up)
    return;

  if (!(xfer = (struct HttpXfer*) worker_alloc(sizeof(*xfer))))
    return;

  memset(xfer, 0, sizeof(*xfer));
  xfer->hx_conn = (unsigned long) id;
  xfer->hx_status = res->hres_status ? res->hres_status : 200;

  ircd_strncpy(xfer->hx_type,
               res->hres_type[0] ? res->hres_type
                                 : "application/octet-stream",
               sizeof(xfer->hx_type) - 1);

  for (i = 0; i < res->hres_nheaders && i < HTTP_HEADERS_MAX; i++)
    xfer->hx_reply[xfer->hx_nreply++] = res->hres_headers[i];

  len = res->hres_bodylen;
  if (len > HTTP_BODY_MAX)
    len = HTTP_BODY_MAX;

  if (len && res->hres_body)
    memcpy(xfer->hx_body, res->hres_body, len);

  xfer->hx_replylen = len;

  if (!mg_wakeup(&httpd_mgr, (unsigned long) id, &xfer, sizeof(xfer)))
    worker_free(xfer);
}

/** Let a connection go that will never be answered.  Main thread. */
static void http_server_cancel(http_req_t id)
{
  struct HttpXfer* xfer;

  if (!httpd_up)
    return;

  /* A status of zero is how the worker is told to drain and close rather
   * than write an answer: the module that was going to produce one is
   * gone, and inventing a body on its behalf would be inventing content. */
  if (!(xfer = (struct HttpXfer*) worker_alloc(sizeof(*xfer))))
    return;

  memset(xfer, 0, sizeof(*xfer));
  xfer->hx_conn = (unsigned long) id;

  if (!mg_wakeup(&httpd_mgr, (unsigned long) id, &xfer, sizeof(xfer)))
    worker_free(xfer);
}

static const struct HttpTransport httpd_transport = {
  "mongoose " MG_VERSION, http_server_respond, http_server_cancel
};

/** Build the argument block from the features.  Main thread. */
static void httpd_args_from_features(struct HttpdArgs* args)
{
  const char* bind = feature_str(FEAT_HTTP_BIND);
  const char* cert = feature_str(FEAT_HTTP_TLS_CERT);
  const char* key = feature_str(FEAT_HTTP_TLS_KEY);
  int port = feature_int(FEAT_HTTP_PORT);

  memset(args, 0, sizeof(*args));

  args->ha_max = feature_int(FEAT_HTTP_MAX_CLIENTS);
  args->ha_tls = (cert && *cert && key && *key) ? 1 : 0;

  if (args->ha_tls) {
    ircd_strncpy(args->ha_cert, cert, sizeof(args->ha_cert) - 1);
    ircd_strncpy(args->ha_key, key, sizeof(args->ha_key) - 1);
  }

  ircd_snprintf(0, args->ha_url, sizeof(args->ha_url), "%s://%s:%d",
                args->ha_tls ? "https" : "http",
                bind && *bind ? bind : "0.0.0.0", port);
}

int http_server_running(void)
{
  return httpd_worker != 0;
}

void http_server_stop(void)
{
  struct Worker* worker = httpd_worker;

  if (!worker)
    return;

  /* In this order, and the order is the whole of the thread safety here:
   * the transport goes first, so no further mg_wakeup() can be issued,
   * and worker_stop() then waits for the thread, which frees the manager
   * as the last thing it does. */
  http_clear_transport();
  httpd_up = 0;
  httpd_worker = 0;

  worker_stop(worker);

  memset(&httpd_current, 0, sizeof(httpd_current));
}

void http_server_reconfigure(void)
{
  struct HttpdArgs want;
  struct HttpdArgs* args;
  int port = feature_int(FEAT_HTTP_PORT);

  if (port <= 0 || port > 65535) {
    if (httpd_worker) {
      log_write(LS_SYSTEM, L_INFO, 0,
                "http: HTTP_PORT is 0; the listener is going down");
      http_server_stop();
    }
    return;
  }

  httpd_args_from_features(&want);

  /* A rehash that did not touch any of this must not drop the listener:
   * every connection on it would go with it, for nothing. */
  if (httpd_worker && !memcmp(&want, &httpd_current, sizeof(want)))
    return;

  http_server_stop();

  if (!(args = (struct HttpdArgs*) worker_alloc(sizeof(*args))))
    return;

  *args = want;

  httpd_worker = worker_spawn("http", httpd_main, args);

  if (!httpd_worker) {
    worker_free(args);
    log_write(LS_SYSTEM, L_WARNING, 0,
              "http: no listener thread; is WORKER_THREADS zero?");
    return;
  }

  httpd_current = want;
}
