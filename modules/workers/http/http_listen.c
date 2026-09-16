/*
 * IRC - Internet Relay Chat, modules/workers/http/http_listen.c
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
 * @brief The thread that owns the socket.
 *
 * Everything in this file runs on the worker and touches nothing the main
 * thread owns: no @c struct @c Client, no @c CurrentTime, no
 * @c MyMalloc(), no @c log_write(), no @c sendto_*.  It has a listening
 * socket, some connections, and two queues -- requests going up through
 * worker_post(), answers coming back through the module's own queue and a
 * self-pipe to wake this poll().
 *
 * That is the whole reason the provider is a module with a thread rather
 * than a listener in the core: reading an HTTP request means waiting for
 * a peer that may be slow on purpose, and the core has one thread that
 * every client shares.
 */
#include "config.h"

#include "http_priv.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>

/** What the worker is given when it starts. */
struct HttpdArgs {
  int  ha_port;                   /**< Where to listen. */
  char ha_bind[64];               /**< What to bind to, or "". */
  int  ha_max;                    /**< Most connections at once. */
};

/** Everything the loop holds. */
struct HttpdState {
  struct Worker*   hs_worker;
  int              hs_listen;
  int              hs_count;
  int              hs_max;
  unsigned long    hs_last_id;
  struct HttpConn* hs_conns;
};

/** Put a descriptor in non-blocking mode. */
static void httpd_nonblock(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);

  if (flags >= 0)
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/** Open the listening socket, or return -1. */
static int httpd_listen(const struct HttpdArgs* args)
{
  struct sockaddr_in sin;
  int fd;
  int one = 1;

  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    return -1;

  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons((unsigned short) args->ha_port);

  if (args->ha_bind[0]) {
    if (inet_pton(AF_INET, args->ha_bind, &sin.sin_addr) != 1) {
      close(fd);
      return -1;
    }
  } else
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

  if (bind(fd, (struct sockaddr*) &sin, sizeof(sin)) < 0
      || listen(fd, 16) < 0) {
    close(fd);
    return -1;
  }

  httpd_nonblock(fd);

  return fd;
}

/** Find a connection by the handle the core knows it by. */
static struct HttpConn* httpd_find(struct HttpdState* st, http_req_t id)
{
  struct HttpConn* c;

  for (c = st->hs_conns; c; c = c->hcn_next)
    if (c->hcn_id == id)
      return c;

  return 0;
}

/** Close a connection and take it off the list. */
static void httpd_drop(struct HttpdState* st, struct HttpConn* conn)
{
  struct HttpConn** pp;

  for (pp = &st->hs_conns; *pp; pp = &(*pp)->hcn_next)
    if (*pp == conn) {
      *pp = conn->hcn_next;
      break;
    }

  if (conn->hcn_fd >= 0)
    close(conn->hcn_fd);

  if (conn->hcn_out)
    worker_free(conn->hcn_out);

  worker_free(conn);
  st->hs_count--;
}

/** Queue an answer this module wrote itself. */
static void httpd_answer_status(struct HttpConn* conn, int status)
{
  size_t len = 0;
  char* buf;

  /* Whatever the request asked for, an error answer ends the
   * connection: this server did not understand it, so it has no idea
   * where the next one would start. */
  conn->hcn_keep = 0;

  if (!(buf = httpd_render_status(status, 0, &len))) {
    conn->hcn_state = HTTPD_CLOSING;
    return;
  }

  if (conn->hcn_out)
    worker_free(conn->hcn_out);

  conn->hcn_out = buf;
  conn->hcn_outlen = len;
  conn->hcn_sent = 0;
  conn->hcn_state = HTTPD_WRITING;
}

/** Take a new connection. */
static void httpd_accept(struct HttpdState* st)
{
  struct sockaddr_in sin;
  socklen_t slen = sizeof(sin);
  struct HttpConn* conn;
  int fd;
  int one = 1;

  while ((fd = accept(st->hs_listen, (struct sockaddr*) &sin, &slen)) >= 0) {
    slen = sizeof(sin);

    /* A connection over the limit is closed rather than queued.  A queue
     * of connections nobody is reading is a way of running the server out
     * of descriptors politely. */
    if (st->hs_count >= st->hs_max) {
      close(fd);
      continue;
    }

    if (!(conn = (struct HttpConn*) worker_alloc(sizeof(*conn)))) {
      close(fd);
      continue;
    }

    memset(conn, 0, sizeof(*conn));
    conn->hcn_fd = fd;
    conn->hcn_state = HTTPD_READING;
    conn->hcn_id = ++st->hs_last_id;
    conn->hcn_touched = time(0);

    inet_ntop(AF_INET, &sin.sin_addr, conn->hcn_remote,
              sizeof(conn->hcn_remote) - 1);

    httpd_nonblock(fd);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    conn->hcn_next = st->hs_conns;
    st->hs_conns = conn;
    st->hs_count++;
  }
}

/** Send a parsed request up to the main thread.
 * @return Non-zero if it went.
 */
static int httpd_send_up(struct HttpdState* st, struct HttpConn* conn,
                         struct HttpXfer* parsed)
{
  struct WorkTask* task;
  struct HttpXfer* xfer;

  if (!(xfer = (struct HttpXfer*) worker_alloc(sizeof(*xfer))))
    return 0;

  *xfer = *parsed;
  xfer->hx_id = conn->hcn_id;

  if (!(task = worker_task_new(0, httpd_dispatch_done))) {
    worker_free(xfer);
    return 0;
  }

  task->wt_in = xfer;

  if (!worker_post(st->hs_worker, task)) {
    /* The main thread is not draining.  Dropping the request and
     * answering 503 here is better than blocking this thread, which is
     * the one thing it is not allowed to do. */
    task->wt_in = 0;
    worker_task_free(task);
    worker_free(xfer);
    return 0;
  }

  return 1;
}

/** Try to make a request out of what is already in the buffer.
 *
 * Called after a read, and again after an answer has gone out, because a
 * client that pipelined sent its next request in the same packet and
 * there will be no readability event for bytes that already arrived.
 */
static void httpd_advance(struct HttpdState* st, struct HttpConn* conn)
{
  struct HttpXfer parsed;
  int r;

  if (conn->hcn_state != HTTPD_READING || !conn->hcn_inlen)
    return;

  r = httpd_parse(conn, &parsed);

  if (r < 0) {
    httpd_answer_status(conn, -r);
    return;
  }

  if (!r)
    return;                     /* more to come */

  if (!httpd_send_up(st, conn, &parsed)) {
    httpd_answer_status(conn, 503);
    return;
  }

  conn->hcn_state = HTTPD_WAITING;
}

/** Read whatever a connection has, and act on it. */
static void httpd_readable(struct HttpdState* st, struct HttpConn* conn)
{
  size_t room;
  ssize_t n;

  room = sizeof(conn->hcn_in) - 1 - conn->hcn_inlen;

  if (!room) {
    httpd_answer_status(conn, 431);
    return;
  }

  n = read(conn->hcn_fd, conn->hcn_in + conn->hcn_inlen, room);

  if (n == 0) {
    conn->hcn_state = HTTPD_CLOSING;
    return;
  }

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return;
    conn->hcn_state = HTTPD_CLOSING;
    return;
  }

  conn->hcn_inlen += (size_t) n;
  conn->hcn_touched = time(0);

  httpd_advance(st, conn);
}

/** Write whatever is left of an answer. */
static void httpd_writable(struct HttpdState* st, struct HttpConn* conn)
{
  ssize_t n = write(conn->hcn_fd, conn->hcn_out + conn->hcn_sent,
                    conn->hcn_outlen - conn->hcn_sent);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return;
    conn->hcn_state = HTTPD_CLOSING;
    return;
  }

  conn->hcn_sent += (size_t) n;
  conn->hcn_touched = time(0);

  if (conn->hcn_sent < conn->hcn_outlen)
    return;

  worker_free(conn->hcn_out);
  conn->hcn_out = 0;
  conn->hcn_outlen = 0;
  conn->hcn_sent = 0;

  if (!conn->hcn_keep) {
    conn->hcn_state = HTTPD_CLOSING;
    return;
  }

  conn->hcn_state = HTTPD_READING;

  /* Whatever the client pipelined is still in the buffer and no poll()
   * will ever mention it again; it is a whole request or it is not. */
  httpd_advance(st, conn);
}

/** Take every answer the main thread has handed back. */
static void httpd_take_answers(struct HttpdState* st)
{
  struct HttpXfer* xfer;
  char drain[256];

  /* The pipe only ever says "look at the queue"; what is in it does not
   * matter and it is emptied so the next answer can wake us again. */
  while (read(httpd_queue.hq_wake[0], drain, sizeof(drain)) > 0)
    ;

  while ((xfer = httpd_queue_pop(&httpd_queue))) {
    struct HttpConn* conn = httpd_find(st, xfer->hx_id);
    size_t len = 0;
    char* buf;

    /* The connection went away while the answer was being worked out.
     * Nothing to do and nobody to tell: it is the ordinary case for a
     * client that pressed stop. */
    if (!conn || conn->hcn_state != HTTPD_WAITING) {
      worker_free(xfer);
      continue;
    }

    buf = httpd_render(xfer, conn->hcn_keep, &len);
    worker_free(xfer);

    if (!buf) {
      conn->hcn_state = HTTPD_CLOSING;
      continue;
    }

    if (conn->hcn_out)
      worker_free(conn->hcn_out);

    conn->hcn_out = buf;
    conn->hcn_outlen = len;
    conn->hcn_sent = 0;
    conn->hcn_state = HTTPD_WRITING;
  }
}

/** Close whatever has been idle too long. */
static void httpd_reap(struct HttpdState* st)
{
  time_t now = time(0);
  struct HttpConn* conn = st->hs_conns;

  while (conn) {
    struct HttpConn* next = conn->hcn_next;

    if (conn->hcn_state == HTTPD_CLOSING
        || (conn->hcn_state != HTTPD_WAITING
            && now - conn->hcn_touched > HTTPD_IDLE_SECONDS))
      httpd_drop(st, conn);

    conn = next;
  }
}

void httpd_worker_main(struct Worker* worker, void* arg)
{
  struct HttpdArgs* args = (struct HttpdArgs*) arg;
  struct HttpdState st;
  int stopfd = worker_stop_fd(worker);

  memset(&st, 0, sizeof(st));
  st.hs_worker = worker;
  st.hs_max = args->ha_max > 0 ? args->ha_max : 64;
  st.hs_listen = httpd_listen(args);

  if (st.hs_listen < 0) {
    /* No log from here: the rule is absolute.  The main thread notices
     * the worker stopped and says so. */
    worker_free(args);
    return;
  }

  worker_log("http: listening on port %d", args->ha_port);
  worker_free(args);

  while (!worker_stopping(worker)) {
    struct pollfd fds[3 + 256];
    struct HttpConn* conns[256];
    struct HttpConn* conn;
    nfds_t n = 0;
    unsigned int i;
    unsigned int nconn = 0;

    fds[n].fd = st.hs_listen;
    fds[n].events = POLLIN;
    fds[n++].revents = 0;

    fds[n].fd = stopfd;
    fds[n].events = POLLIN;
    fds[n++].revents = 0;

    fds[n].fd = httpd_queue.hq_wake[0];
    fds[n].events = POLLIN;
    fds[n++].revents = 0;

    for (conn = st.hs_conns; conn && nconn < 256; conn = conn->hcn_next) {
      short want = 0;

      if (conn->hcn_state == HTTPD_READING)
        want = POLLIN;
      else if (conn->hcn_state == HTTPD_WRITING)
        want = POLLOUT;

      if (!want)
        continue;               /* waiting on the main thread */

      conns[nconn] = conn;
      fds[n].fd = conn->hcn_fd;
      fds[n].events = want;
      fds[n].revents = 0;
      n++;
      nconn++;
    }

    if (poll(fds, n, 1000) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    if (worker_stopping(worker))
      break;

    if (fds[0].revents & POLLIN)
      httpd_accept(&st);

    /* Unconditionally, not only when the pipe said so: the pipe is how a
     * sleeping poll() is woken, not how the queue is found.  A wakeup that
     * was lost -- a full pipe, a failed write, a pipe that could not be
     * created at all -- must not turn into an answer nobody sends. */
    httpd_take_answers(&st);

    for (i = 0; i < nconn; i++) {
      struct pollfd* p = &fds[3 + i];

      if (!p->revents)
        continue;

      if (p->revents & (POLLERR | POLLHUP | POLLNVAL)) {
        conns[i]->hcn_state = HTTPD_CLOSING;
        continue;
      }

      if (p->revents & POLLIN)
        httpd_readable(&st, conns[i]);
      else if (p->revents & POLLOUT)
        httpd_writable(&st, conns[i]);
    }

    httpd_reap(&st);
  }

  while (st.hs_conns)
    httpd_drop(&st, st.hs_conns);

  close(st.hs_listen);
}

/** Build the argument block the worker is started with.
 *
 * Read in the main thread, where the features live, and handed over: the
 * worker never reads a feature, because a feature is core state.
 */
void* httpd_args_new(int port, const char* bind, int max)
{
  struct HttpdArgs* args =
    (struct HttpdArgs*) worker_alloc(sizeof(struct HttpdArgs));

  if (!args)
    return 0;

  memset(args, 0, sizeof(*args));
  args->ha_port = port;
  args->ha_max = max;

  if (bind && *bind) {
    strncpy(args->ha_bind, bind, sizeof(args->ha_bind) - 1);
    args->ha_bind[sizeof(args->ha_bind) - 1] = '\0';
  }

  return args;
}
