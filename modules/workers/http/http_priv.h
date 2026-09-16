/*
 * IRC - Internet Relay Chat, modules/workers/http/http_priv.h
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
 * @brief Private declarations for the HTTP provider.
 *
 * Nothing outside this module sees any of it; what the rest of the server
 * uses is include/http.h.
 */
#ifndef INCLUDED_http_priv_h
#define INCLUDED_http_priv_h

#include "http.h"

#include <pthread.h>
#include <sys/types.h>

struct ModuleHandle;
struct Worker;
struct WorkTask;

/** Longest request line and header block this server will read.
 *
 * A request bigger than this is refused with 431 before anything is
 * parsed: everything past this point costs memory in a thread that
 * cannot be allowed to grow without a bound somebody chose.
 */
#define HTTPD_HEAD_MAX 8192

/** Longest body read into memory, which is what the core will carry. */
#define HTTPD_BODY_MAX HTTP_BODY_MAX

/** How long a connection may be idle before it is closed, in seconds. */
#define HTTPD_IDLE_SECONDS 30

/** What a connection is doing. */
enum HttpConnState {
  HTTPD_READING,   /**< Collecting the head, then the body. */
  HTTPD_WAITING,   /**< Handed to the main thread, waiting for an answer. */
  HTTPD_WRITING,   /**< Sending the answer. */
  HTTPD_CLOSING    /**< Finished with; the loop reaps it. */
};

/** One connection, owned entirely by the worker thread. */
struct HttpConn {
  struct HttpConn* hcn_next;      /**< Next, in no order. */
  int    hcn_fd;                  /**< Its socket. */
  enum HttpConnState hcn_state;   /**< What it is doing. */
  http_req_t hcn_id;              /**< This module's handle for it. */
  time_t hcn_touched;             /**< When it last did anything. */
  int    hcn_keep;                /**< Keep-alive was asked for. */

  char   hcn_in[HTTPD_HEAD_MAX + HTTPD_BODY_MAX + 2]; /**< What came in. */
  size_t hcn_inlen;               /**< How much of it. */
  size_t hcn_headlen;             /**< Where the body starts, or 0. */
  size_t hcn_want;                /**< Body bytes expected. */

  char*  hcn_out;                 /**< The answer, to be written. */
  size_t hcn_outlen;              /**< Its length. */
  size_t hcn_sent;                /**< How much has gone. */

  char   hcn_remote[64];          /**< Who it is, as text. */
};

/** A request on its way to the main thread, or an answer on its way back.
 *
 * Allocated with worker_alloc() because it crosses a queue, and holding
 * only bytes: no pointer into anything either side might free.
 */
struct HttpXfer {
  http_req_t hx_id;               /**< The connection it belongs to. */

  /* Going up: the request, already parsed. */
  char   hx_method[HTTP_METHOD_MAX + 1];
  char   hx_path[HTTP_PATH_MAX + 1];
  char   hx_query[HTTP_QUERY_MAX + 1];
  char   hx_remote[64];
  char   hx_version;              /**< '0' or '1': the HTTP/1.x minor. */
  unsigned int hx_nheaders;
  struct HttpHeader hx_headers[HTTP_HEADERS_MAX];
  size_t hx_bodylen;

  /* Coming down: the answer. */
  int    hx_status;
  char   hx_type[HTTP_HVALUE_MAX + 1];
  unsigned int hx_nreply;
  struct HttpHeader hx_reply[HTTP_HEADERS_MAX];
  size_t hx_replylen;

  char   hx_body[HTTPD_BODY_MAX + 1]; /**< Request body, then reply body. */
};

/** The queue of answers the main thread hands back to the worker.
 *
 * One lock, one condition, and a pipe: the worker is asleep in poll() on
 * its sockets, so the condition variable is no use to it and the pipe is
 * what wakes it.
 */
struct HttpQueue {
  pthread_mutex_t hq_lock;
  struct HttpXferNode* hq_head;
  struct HttpXferNode* hq_tail;
  int    hq_wake[2];              /**< Self-pipe: [0] read, [1] write. */
};

/** One answer waiting in the queue. */
struct HttpXferNode {
  struct HttpXferNode* hxn_next;
  struct HttpXfer*     hxn_xfer;
};

/* http_module.c */
extern struct ModuleHandle* httpd_mod;
extern struct HttpQueue httpd_queue;

/** Push an answer to the worker and wake it.
 * @return Non-zero if it was queued.
 */
extern int httpd_queue_push(struct HttpQueue* q, struct HttpXfer* xfer);

/** Take the next answer, or NULL.  Called only by the worker. */
extern struct HttpXfer* httpd_queue_pop(struct HttpQueue* q);

/** Hand a parsed request to whoever claimed its route.  Main thread.
 *
 * The wt_done of the task the worker posted: it turns the transfer back
 * into a struct HttpRequest and calls http_dispatch().  The transfer is
 * the task's payload, so the server frees it afterwards.
 */
extern void httpd_dispatch_done(struct WorkTask* task);

/* http_listen.c */

/** The worker's whole life: listen, accept, read, parse, answer. */
extern void httpd_worker_main(struct Worker* worker, void* arg);

/** Build the argument block the worker is started with.
 *
 * Read in the main thread, where the features live, and handed over: the
 * worker never reads a feature, because a feature is core state.
 * @return A worker_alloc()ed block, or NULL.
 */
extern void* httpd_args_new(int port, const char* bind, int max);

/* http_parse.c */

/** Work out whether \a conn has a whole request yet, and parse it.
 *
 * @param[in,out] conn The connection.
 * @param[out] xfer Filled in when a whole request has arrived.
 * @return 1 when \a xfer is ready, 0 when more bytes are needed, and a
 *   negative HTTP status when the request is one this server will not
 *   read (431 too large, 400 malformed, 413 body too big).
 */
extern int httpd_parse(struct HttpConn* conn, struct HttpXfer* xfer);

/** Render a response into a buffer the connection will write.
 * @param[in] xfer The answer.
 * @param[in] keep Non-zero to say the connection stays open.
 * @param[out] len Receives the length.
 * @return A worker_alloc()ed buffer, or NULL.
 */
extern char* httpd_render(const struct HttpXfer* xfer, int keep, size_t* len);

/** Render one of this module's own answers -- a 404, a 431 -- the same way. */
extern char* httpd_render_status(int status, int keep, size_t* len);

#endif /* INCLUDED_http_priv_h */
