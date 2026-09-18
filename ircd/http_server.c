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

#include <dirent.h>

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

  /* A body too big to carry: the worker wrote it here instead, and what
   * crosses is the path and the size.  Empty for every ordinary request,
   * which is nearly all of them. */
  char   hx_file[HTTPD_PATHLEN + 1];
  size_t hx_filelen;

  /* And the same the other way: a file to send instead of a body, with
   * the conditional headers of the request it answers. */
  char   hx_sendfile[HTTPD_PATHLEN + 1];
  char   hx_range[HTTP_HVALUE_MAX + 1];
  char   hx_inm[HTTP_HVALUE_MAX + 1];

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
  char ha_spool[HTTPD_PATHLEN + 1];       /**< Where a big body is
                                               written, or "". */
  unsigned int ha_upload_max;             /**< Largest such body, or 0. */
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

/** Worker-side: where a body too big to carry is written, and how big it
 * may be.  Empty and zero when this server takes no uploads. */
static char httpd_spool[HTTPD_PATHLEN + 1];
static unsigned int httpd_upload_max;

/** One request whose body is being written to disk.  Worker thread only.
 *
 * The headers arrive first and the body afterwards, so what the request
 * *is* has to be kept somewhere while Mongoose streams the rest of it.
 * Not in @c mg_connection::data -- the upload helper owns that -- so a
 * small list of its own, which is also where the spool file is remembered
 * until the answer goes out and it can be deleted.
 */
struct HttpdUpload {
  struct HttpdUpload* hu_next;
  unsigned long       hu_conn;             /**< mg_connection::id. */
  struct HttpXfer*    hu_xfer;             /**< Headers, already copied. */
  char                hu_path[HTTPD_PATHLEN + 1]; /**< The spool file. */
};

/** Uploads in progress, and answers whose file is not deleted yet. */
static struct HttpdUpload* httpd_uploads;

/** The listening connection, kept for what an upload borrows from it.
 *
 * mg_http_start_upload() takes the connection's handlers over while the
 * body is arriving, and they have to be given back afterwards; the HTTP
 * protocol handler is Mongoose's own and therefore not a symbol this file
 * has.  An accepted connection inherits it from the listener, which is
 * where it is read back from. */
static struct mg_connection* httpd_listener;

static void httpd_event(struct mg_connection* c, int ev, void* ev_data);
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

/** Copy everything but the body of a parsed request into a transfer.
 *
 * Split out because the body arrives later for an upload: what a request
 * *is* -- its method, path, query, headers and who sent it -- is all
 * known when the headers are, and has to be kept while Mongoose streams
 * the rest of it to disk.  Worker thread.
 *
 * @return The transfer, or NULL; on a malformed path the answer has been
 *   sent and \a *answered is set.
 */
static struct HttpXfer* httpd_xfer_from(struct mg_connection* c,
                                        struct mg_http_message* hm,
                                        int* answered)
{
  struct HttpXfer* xfer;
  size_t i;

  *answered = 0;

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
    *answered = 1;
    return 0;
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

  return xfer;
}

/** Hand a transfer to the main thread.  Worker thread.
 * @return Non-zero if it went up.
 */
static int httpd_post_xfer(struct mg_connection* c, struct HttpXfer* xfer)
{
  struct WorkTask* task;

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

/* ------------------------------------------------------------------- *
 * Uploads: a body too big to carry                                    *
 * ------------------------------------------------------------------- */

/** Delete what a previous run left in the spool.  Worker thread.
 *
 * A server that died with an upload in progress left a file nobody will
 * ever ask for.  Only files this server named are touched -- the prefix
 * is the whole of the safety here, because the directory is configured
 * and an operator who points it somewhere surprising should not lose
 * what is in it.
 */
static void httpd_sweep_spool(void)
{
  DIR* dir;
  struct dirent* ent;
  char path[HTTPD_PATHLEN + 1];

  if (!httpd_spool[0])
    return;

  if (!(dir = opendir(httpd_spool)))
    return;

  while ((ent = readdir(dir)) != NULL) {
    if (strncmp(ent->d_name, "ircu-upload-", 12) != 0)
      continue;

    mg_snprintf(path, sizeof(path), "%s/%s", httpd_spool, ent->d_name);
    remove(path);
  }

  closedir(dir);
}

/** Find the upload on a connection, or NULL.  Worker thread. */
static struct HttpdUpload* httpd_upload_find(unsigned long conn)
{
  struct HttpdUpload* up;

  for (up = httpd_uploads; up; up = up->hu_next)
    if (up->hu_conn == conn)
      return up;

  return 0;
}

/** Forget an upload, deleting what it wrote.  Worker thread.
 *
 * The spool file is the worker's from the moment it is created until the
 * request has been answered: a handler that wanted the bytes has already
 * moved them, and this unlink then finds nothing, which is not an error.
 */
static void httpd_upload_done(unsigned long conn)
{
  struct HttpdUpload** up_p;
  struct HttpdUpload* up;

  for (up_p = &httpd_uploads; (up = *up_p); up_p = &up->hu_next) {
    if (up->hu_conn != conn)
      continue;

    *up_p = up->hu_next;

    if (up->hu_path[0])
      remove(up->hu_path);

    if (up->hu_xfer)
      worker_free(up->hu_xfer);

    worker_free(up);
    return;
  }
}

/** Mongoose has finished writing the body.  Worker thread.
 *
 * @param[in] c The connection.
 * @param[in] err NULL on success, or what went wrong.
 */
static void httpd_upload_finished(struct mg_connection* c, const char* err)
{
  struct HttpdUpload* up = httpd_upload_find(c->id);
  struct HttpXfer* xfer;
  size_t size = 0;

  /* Give the connection back.  mg_http_start_upload() pointed both
   * handlers at its own while the body was arriving, and it does not put
   * them back: leaving them there would send the MG_EV_WAKEUP carrying
   * the answer -- and the MG_EV_CLOSE that ends the connection -- to a
   * handler that has nothing left to do, which is a request that is never
   * answered.  The protocol handler is Mongoose's own, so it is taken
   * from the listener, which is where this connection got it. */
  c->fn = httpd_event;
  c->fn_data = 0;

  if (httpd_listener)
    c->pfn = httpd_listener->pfn;

  if (!up)
    return;

  if (err) {
    /* Not the client's fault as far as this can tell: the disk filled,
     * or the connection went away mid-body.  Either way there is nothing
     * to hand up. */
    httpd_status(c, 500, "500 Internal Server Error");
    httpd_upload_done(c->id);
    return;
  }

  if (!mg_fs_posix.st(up->hu_path, &size, NULL)) {
    httpd_status(c, 500, "500 Internal Server Error");
    httpd_upload_done(c->id);
    return;
  }

  xfer = up->hu_xfer;
  up->hu_xfer = 0;            /* the task owns it now */

  ircd_strncpy(xfer->hx_file, up->hu_path, sizeof(xfer->hx_file) - 1);
  xfer->hx_filelen = size;

  if (!httpd_post_xfer(c, xfer)) {
    httpd_status(c, 503, "503 Service Unavailable");
    httpd_upload_done(c->id);
  }
}

/** Refuse a request at the headers, before its body has arrived.
 *
 * Answering and returning is not enough on its own: the body is still on
 * its way, and Mongoose would deliver it as a request of its own and have
 * it answered a second time.  @c is_resp stops the parsing, and
 * @c is_draining closes the connection once the answer has gone -- which
 * is the honest end for a request whose body this server is not reading.
 */
static void httpd_refuse(struct mg_connection* c, int status, const char* text)
{
  httpd_status(c, status, text);

  c->is_resp = 1;
  c->is_draining = 1;
}

/** Take a request whose body is about to arrive, if it is an upload.
 *
 * Called when the headers have been read and before any of the body has.
 * That is the only moment a body can be sent somewhere other than into
 * memory, which is the whole point: a hundred megabytes buffered here
 * would be a hundred megabytes the event loop is holding while every
 * other client waits.
 *
 * @return Non-zero if this request has been taken over.
 */
static int httpd_upload_begin(struct mg_connection* c,
                              struct mg_http_message* hm)
{
  struct HttpdUpload* up;
  struct HttpXfer* xfer;
  struct mg_str* te;
  char name[64];
  int answered;

  if (!httpd_upload_max || !httpd_spool[0])
    return 0;

  /* Only where a body is expected at all.  A GET with a body is not an
   * upload, it is a client with an opinion. */
  if (mg_strcasecmp(hm->method, mg_str("POST")) != 0
      && mg_strcasecmp(hm->method, mg_str("PUT")) != 0)
    return 0;

  /* Small enough to carry: the ordinary path handles it, and a handler
   * that wanted bytes in memory still gets them. */
  if (hm->body.len <= HTTP_BODY_MAX)
    return 0;

  /* Chunked has no length until it ends, and this needs one: what is
   * being decided here is whether to accept the body at all, before any
   * of it has been written anywhere.  Saying so is better than taking it
   * and finding out. */
  if ((te = mg_http_get_header(hm, "Transfer-Encoding")) != NULL
      && mg_strcasecmp(*te, mg_str("chunked")) == 0) {
    httpd_refuse(c, 411, "411 Length Required");
    return 1;
  }

  if (hm->body.len > httpd_upload_max) {
    httpd_refuse(c, 413, "413 Payload Too Large");
    return 1;
  }

  if (httpd_upload_find(c->id)) {
    /* One at a time on one connection; a second while the first is still
     * arriving is not something a client does by accident. */
    httpd_refuse(c, 400, "400 Bad Request");
    return 1;
  }

  if (!(xfer = httpd_xfer_from(c, hm, &answered))) {
    if (!answered)
      return 0;

    /* It answered for us -- a path this server will not hand to a module
     * -- but the body is still coming, so the connection has to be closed
     * the way httpd_refuse() closes it. */
    c->is_resp = 1;
    c->is_draining = 1;

    return 1;
  }

  if (!(up = (struct HttpdUpload*) worker_alloc(sizeof(*up)))) {
    worker_free(xfer);
    return 0;
  }

  memset(up, 0, sizeof(*up));
  up->hu_conn = c->id;
  up->hu_xfer = xfer;

  /* The name is this server's to choose and nobody else's: it is built
   * from the connection and the clock, never from anything the client
   * sent, so a path cannot be smuggled through it.  The prefix is what
   * the sweep at start-up recognises as ours. */
  mg_snprintf(name, sizeof(name), "ircu-upload-%lu-%lu", (unsigned long) c->id,
              (unsigned long) mg_millis());
  mg_snprintf(up->hu_path, sizeof(up->hu_path), "%s/%s", httpd_spool, name);

  up->hu_next = httpd_uploads;
  httpd_uploads = up;

  mg_http_start_upload(c, hm, mg_str(name), mg_str(httpd_spool),
                       &mg_fs_posix, httpd_upload_finished);

  return 1;
}

/** Turn a parsed request into a transfer and post it.  Worker thread.
 * @return Non-zero if it went up.
 */
static int httpd_post_request(struct mg_connection* c,
                              struct mg_http_message* hm)
{
  struct HttpXfer* xfer;
  int answered;

  if (hm->body.len > HTTP_BODY_MAX) {
    /* An upload would have been taken at MG_EV_HTTP_HDRS, before any of
     * it arrived; reaching here with a body this size means it was not
     * one -- no spool directory, the wrong method, or more than this
     * server takes. */
    httpd_status(c, 413, "413 Payload Too Large");
    return 1;
  }

  if (!(xfer = httpd_xfer_from(c, hm, &answered)))
    return answered;

  if (hm->body.len) {
    memcpy(xfer->hx_body, hm->body.buf, hm->body.len);
    xfer->hx_bodylen = hm->body.len;
  }
  xfer->hx_body[xfer->hx_bodylen] = '\0';

  return httpd_post_xfer(c, xfer);
}

/** Write an answer that came back from the main thread.  Worker thread. */
/** Stream a file the handler named.  Worker thread.
 *
 * The request itself is long gone -- it went up, was answered, and came
 * back as bytes -- so the message mg_http_serve_file() wants is rebuilt
 * from what travelled with the answer.  It reads three things out of it:
 * the method, @c Range and @c If-None-Match.  Mongoose does the rest,
 * including the partial-content arithmetic, and streams the file outside
 * the event loop.
 */
static void httpd_serve_file(struct mg_connection* c,
                             const struct HttpXfer* xfer,
                             const char* headers)
{
  struct mg_http_message hm;
  struct mg_http_serve_opts opts;
  unsigned int n = 0;

  memset(&hm, 0, sizeof(hm));
  memset(&opts, 0, sizeof(opts));

  hm.method = mg_str("GET");

  if (xfer->hx_range[0]) {
    hm.headers[n].name = mg_str("Range");
    hm.headers[n].value = mg_str(xfer->hx_range);
    n++;
  }

  if (xfer->hx_inm[0]) {
    hm.headers[n].name = mg_str("If-None-Match");
    hm.headers[n].value = mg_str(xfer->hx_inm);
    n++;
  }

  opts.extra_headers = headers;

  /* No mime_types: with a Content-Type header already in `headers` the
   * handler's choice wins, and without one Mongoose guesses from the
   * name, which is what a handler that did not care asked for. */
  mg_http_serve_file(c, &hm, xfer->hx_sendfile, &opts);

  c->is_resp = 0;
}

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

  if (xfer->hx_sendfile[0]) {
    httpd_serve_file(c, xfer, headers);
    return;
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
    if (!c->is_listening) {
      httpd_conns--;
      /* Whatever was written for this connection goes with it, answered
       * or not: nobody is going to ask for it again. */
      httpd_upload_done(c->id);
    }
    break;

  case MG_EV_HTTP_HDRS:
    /* Before any of the body has arrived, which is the only moment it can
     * be sent anywhere but into memory. */
    httpd_upload_begin(c, (struct mg_http_message*) ev_data);
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

    /* The request has been answered, so the spool file has served its
     * purpose.  A handler that wanted the bytes moved them while it had
     * the request; this deletes what is left, which is usually nothing. */
    httpd_upload_done(c->id);
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
  httpd_upload_max = args->ha_upload_max;
  ircd_strncpy(httpd_spool, args->ha_spool, sizeof(httpd_spool) - 1);
  httpd_sweep_spool();
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
  httpd_listener = listener;

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
  httpd_listener = 0;
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

  /* A body too big to carry is a path instead.  The handler is told
   * where it is; the worker deletes it once this request is answered. */
  if (xfer->hx_file[0]) {
    req.hreq_file = xfer->hx_file;
    req.hreq_filelen = xfer->hx_filelen;
  }

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

  if (res->hres_file[0]) {
    ircd_strncpy(xfer->hx_sendfile, res->hres_file,
                 sizeof(xfer->hx_sendfile) - 1);
    ircd_strncpy(xfer->hx_range, res->hres_range, sizeof(xfer->hx_range) - 1);
    ircd_strncpy(xfer->hx_inm, res->hres_inm, sizeof(xfer->hx_inm) - 1);
  }

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

  /* Read here, in the main thread, and handed over: a worker never reads
   * a feature.  Both or neither -- a size with nowhere to write it is not
   * an offer to take uploads. */
  if (http_upload_available()) {
    ircd_strncpy(args->ha_spool, feature_str(FEAT_HTTP_SPOOL_DIR),
                 sizeof(args->ha_spool) - 1);
    args->ha_upload_max = http_upload_max();
  }

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
