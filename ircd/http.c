/*
 * IRC - Internet Relay Chat, ircd/http.c
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
 * @brief The register between whoever serves HTTP and whoever wants a route.
 *
 * See include/http.h.  There is no socket in this file and no parsing --
 * those are ircd/http_server.c's, and Mongoose's under it.  What is here
 * is a list of routes, the requests waiting for an answer, the deadline
 * that bounds them, and one indirection to the transport so that all of
 * it can be tested without a socket (http_t).
 */
#include "config.h"

#include "http.h"
#include "client.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_events.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"

#include <string.h>

/** One route a module has claimed. */
struct HttpRoute {
  struct HttpRoute*    hrt_next;    /**< Next, newest first. */
  struct ModuleHandle* hrt_mod;     /**< Who owns it. */
  char   hrt_method[HTTP_METHOD_MAX + 1]; /**< Uppercase. */
  char   hrt_path[HTTP_PATH_MAX + 1];     /**< As registered. */
  int    hrt_prefix;                /**< It ends in '/', so it matches
                                         everything under it. */
  HttpHandlerFn hrt_fn;             /**< What to call. */
  void*  hrt_user;                  /**< And with what. */
};

/** One request the core is holding an answer open for. */
struct HttpCall {
  struct HttpCall*     hc_next;   /**< Next, in no order. */
  http_req_t           hc_id;     /**< The core's handle. */
  http_req_t           hc_pid;    /**< The transport's, passed back. */
  struct ModuleHandle* hc_mod;    /**< Whose route it went to. */
  time_t               hc_deadline; /**< When it gives up. */
  struct HttpResponse  hc_res;    /**< Being filled in. */
  char                 hc_body[HTTP_BODY_MAX + 1]; /**< Its storage. */
};

/** The transport, or NULL when nothing is listening. */
static const struct HttpTransport* http_transport;

/** Every route, newest first. */
static struct HttpRoute* http_routes;

/** Every request in flight. */
static struct HttpCall* http_calls;
/** Handle for the next one. */
static unsigned long http_last_id;
/** How many are in flight. */
static unsigned int http_call_count;

/** The deadline timer, and whether it has been initialised.
 *
 * timer_init() zeroes GEN_MARKED, which is the only thing telling
 * timer_add() it is re-arming a timer timer_run() still holds, so it runs
 * once and every re-arm from inside the expiry is a bare timer_add().
 * See CLAUDE.md.
 */
static struct Timer http_timer;
static int http_timer_ready;
static int http_timer_armed;

static void http_timeout(struct Event* ev);

/* ------------------------------------------------------------------- *
 * Responses                                                           *
 * ------------------------------------------------------------------- */

void http_response_set(struct HttpResponse* res, int status,
                       const char* type, const char* body, size_t bodylen)
{
  assert(0 != res);

  res->hres_status = status;

  ircd_strncpy(res->hres_type, type ? type : "text/plain",
               sizeof(res->hres_type) - 1);
  res->hres_type[sizeof(res->hres_type) - 1] = '\0';

  res->hres_bodylen = 0;

  if (!body || !res->hres_body || !res->hres_bodymax)
    return;

  if (bodylen > res->hres_bodymax)
    bodylen = res->hres_bodymax;

  memcpy(res->hres_body, body, bodylen);
  res->hres_body[bodylen] = '\0';
  res->hres_bodylen = bodylen;
}

int http_response_header(struct HttpResponse* res, const char* name,
                         const char* value)
{
  struct HttpHeader* h;

  assert(0 != res);

  if (!name || !*name || res->hres_nheaders >= HTTP_HEADERS_MAX)
    return 0;

  h = &res->hres_headers[res->hres_nheaders++];

  ircd_strncpy(h->hh_name, name, sizeof(h->hh_name) - 1);
  h->hh_name[sizeof(h->hh_name) - 1] = '\0';
  ircd_strncpy(h->hh_value, value ? value : "", sizeof(h->hh_value) - 1);
  h->hh_value[sizeof(h->hh_value) - 1] = '\0';

  return 1;
}

const char* http_request_header(const struct HttpRequest* req,
                                const char* name)
{
  unsigned int i;

  if (!req || !name)
    return 0;

  for (i = 0; i < req->hreq_nheaders; i++)
    if (!ircd_strcmp(req->hreq_headers[i].hh_name, name))
      return req->hreq_headers[i].hh_value;

  return 0;
}

/* ------------------------------------------------------------------- *
 * The transport                                                       *
 * ------------------------------------------------------------------- */

int http_set_transport(const struct HttpTransport* transport)
{
  if (!transport || !transport->ht_name || !transport->ht_respond
      || !transport->ht_cancel)
    return 0;

  if (http_transport) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "http: %s cannot take over: %s is already the transport",
              transport->ht_name, http_transport->ht_name);
    return 0;
  }

  http_transport = transport;

  return 1;
}

/** Answer and release one call.
 * @param[in] call The call, already unlinked.
 * @param[in] status What to send, or 0 to send nothing at all.
 */
static void http_finish(struct HttpCall* call, int status)
{
  if (status && http_transport) {
    struct HttpResponse res;

    memset(&res, 0, sizeof(res));
    res.hres_status = status;
    ircd_strncpy(res.hres_type, "text/plain", sizeof(res.hres_type) - 1);

    (http_transport->ht_respond)(call->hc_pid, &res);
  } else if (!status && http_transport) {
    (http_transport->ht_cancel)(call->hc_pid);
  }

  MyFree(call);
  http_call_count--;
}

void http_clear_transport(void)
{
  if (!http_transport)
    return;

  /* The transport is going, so there is nobody to answer through: the
   * calls are dropped without a word rather than answered into a socket
   * that no longer has an owner. */
  http_transport = NULL;

  while (http_calls) {
    struct HttpCall* call = http_calls;

    http_calls = call->hc_next;
    MyFree(call);
    http_call_count--;
  }
}

int http_listening(void)
{
  return http_transport != NULL;
}

const char* http_transport_name(void)
{
  return http_transport ? http_transport->ht_name : 0;
}

int http_available(void)
{
  /* The feature, not the socket.  A consumer is asking whether its route
   * will ever be reached; a listener that is momentarily down between a
   * rehash and the next poll is not something a module should see, and
   * answering from the socket would make module load order decide what a
   * module does. */
  return feature_int(FEAT_HTTP_PORT) != 0;
}

/* ------------------------------------------------------------------- *
 * Routes                                                              *
 * ------------------------------------------------------------------- */

/** Find a route claimed for exactly this method and path. */
static struct HttpRoute* http_route_exact(const char* method,
                                          const char* path)
{
  struct HttpRoute* r;

  for (r = http_routes; r; r = r->hrt_next)
    if (!ircd_strcmp(r->hrt_method, method) && !strcmp(r->hrt_path, path))
      return r;

  return 0;
}

int http_add_route(struct ModuleHandle* mod, const char* method,
                   const char* path, HttpHandlerFn fn, void* user)
{
  struct HttpRoute* route;
  size_t len;

  if (!method || !*method || !path || path[0] != '/' || !fn)
    return 0;

  if (strlen(method) > HTTP_METHOD_MAX || strlen(path) > HTTP_PATH_MAX)
    return 0;

  if (http_route_exact(method, path))
    return 0;

  if (http_route_count(mod) >= HTTP_ROUTES_MAX)
    return 0;

  route = (struct HttpRoute*) MyCalloc(1, sizeof(*route));
  route->hrt_mod = mod;
  ircd_strncpy(route->hrt_method, method, sizeof(route->hrt_method) - 1);
  ircd_strncpy(route->hrt_path, path, sizeof(route->hrt_path) - 1);
  route->hrt_fn = fn;
  route->hrt_user = user;

  len = strlen(route->hrt_path);
  route->hrt_prefix = (len > 0 && route->hrt_path[len - 1] == '/');

  route->hrt_next = http_routes;
  http_routes = route;

  return 1;
}

int http_del_route(struct ModuleHandle* mod, const char* method,
                   const char* path)
{
  struct HttpRoute** pp;

  for (pp = &http_routes; *pp; pp = &(*pp)->hrt_next) {
    struct HttpRoute* r = *pp;

    if (r->hrt_mod != mod)
      continue;
    if (ircd_strcmp(r->hrt_method, method) || strcmp(r->hrt_path, path))
      continue;

    *pp = r->hrt_next;
    MyFree(r);
    return 1;
  }

  return 0;
}

unsigned int http_route_count(const struct ModuleHandle* mod)
{
  struct HttpRoute* r;
  unsigned int n = 0;

  for (r = http_routes; r; r = r->hrt_next)
    if (r->hrt_mod == mod)
      n++;

  return n;
}

/** The route that should answer, or NULL.
 *
 * An exact match wins over a prefix, and a longer prefix over a shorter
 * one: "/files/thumb/" is more specific than "/files/" and the module
 * that asked for it meant it.
 */
static struct HttpRoute* http_route_find(const char* method,
                                         const char* path)
{
  struct HttpRoute* best = 0;
  size_t bestlen = 0;
  struct HttpRoute* r;

  for (r = http_routes; r; r = r->hrt_next) {
    size_t len;

    if (ircd_strcmp(r->hrt_method, method))
      continue;

    if (!strcmp(r->hrt_path, path))
      return r;

    if (!r->hrt_prefix)
      continue;

    len = strlen(r->hrt_path);

    if (strncmp(r->hrt_path, path, len))
      continue;

    if (len > bestlen) {
      best = r;
      bestlen = len;
    }
  }

  return best;
}

/* ------------------------------------------------------------------- *
 * Requests in flight                                                  *
 * ------------------------------------------------------------------- */

/** How long a handler has, clamped. */
static int http_timeout_ms(void)
{
  int ms = feature_int(FEAT_HTTP_TIMEOUT);

  if (ms <= 0)
    ms = HTTP_TIMEOUT_DEFAULT_MS;
  if (ms > HTTP_TIMEOUT_MAX_MS)
    ms = HTTP_TIMEOUT_MAX_MS;

  return ms;
}

/** The earliest deadline there is, or zero. */
static time_t http_deadline(void)
{
  struct HttpCall* call;
  time_t earliest = 0;

  for (call = http_calls; call; call = call->hc_next)
    if (!earliest || call->hc_deadline < earliest)
      earliest = call->hc_deadline;

  return earliest;
}

/** Make sure the timer will fire by the earliest deadline. */
static void http_timer_arm(void)
{
  time_t deadline;

  if (http_timer_armed)
    return;

  if (!(deadline = http_deadline()))
    return;

  if (!http_timer_ready) {
    timer_init(&http_timer);
    http_timer_ready = 1;
  }

  timer_add(&http_timer, http_timeout, 0, TT_ABSOLUTE, deadline);
  http_timer_armed = 1;
}

/** Take one call off the list, or NULL. */
static struct HttpCall* http_call_take(http_req_t id)
{
  struct HttpCall** pp;

  for (pp = &http_calls; *pp; pp = &(*pp)->hc_next) {
    struct HttpCall* call = *pp;

    if (call->hc_id != id)
      continue;

    *pp = call->hc_next;
    return call;
  }

  return 0;
}

int http_expire(time_t now)
{
  int gone = 0;

  for (;;) {
    struct HttpCall** pp;
    struct HttpCall* late = 0;

    for (pp = &http_calls; *pp; pp = &(*pp)->hc_next)
      if ((*pp)->hc_deadline <= now) {
        late = *pp;
        *pp = late->hc_next;
        break;
      }

    if (!late)
      break;

    log_write(LS_SYSTEM, L_WARNING, 0,
              "http: a handler did not answer in time; sending 504");

    /* A gateway timeout is the truthful answer: something behind this
     * server was asked and did not reply.  Holding the socket open
     * instead is the failure the deadline exists to bound. */
    http_finish(late, 504);
    gone++;
  }

  return gone;
}

/** A deadline passed. */
static void http_timeout(struct Event* ev)
{
  if (ev_type(ev) != ET_EXPIRE) {
    if (ev_type(ev) == ET_DESTROY)
      http_timer_armed = 0;
    return;
  }

  http_timer_armed = 0;

  http_expire(CurrentTime);
  http_timer_arm();
}

int http_dispatch(const struct HttpRequest* req, http_req_t pid)
{
  struct HttpRoute* route;
  struct HttpCall* call;
  int done;

  assert(0 != req);

  /* Only the transport calls this, so in the running server there always
   * is one; the guard is what makes the file testable on its own and what
   * stops a handler running for an answer that could not be sent. */
  if (!http_transport)
    return -1;

  route = http_route_find(req->hreq_method ? req->hreq_method : "",
                          req->hreq_path ? req->hreq_path : "");

  /* Nothing claimed it, so the core never owned it: the transport sends
   * its own 404 and there is nothing here to clean up. */
  if (!route)
    return -1;

  call = (struct HttpCall*) MyCalloc(1, sizeof(*call));
  call->hc_id = ++http_last_id;
  call->hc_pid = pid;
  call->hc_mod = route->hrt_mod;
  call->hc_deadline = CurrentTime + (http_timeout_ms() + 999) / 1000;
  call->hc_res.hres_body = call->hc_body;
  call->hc_res.hres_bodymax = HTTP_BODY_MAX;
  call->hc_res.hres_status = 200;
  ircd_strncpy(call->hc_res.hres_type, "text/plain",
               sizeof(call->hc_res.hres_type) - 1);

  call->hc_next = http_calls;
  http_calls = call;
  http_call_count++;

  done = (route->hrt_fn)(call->hc_id, req, &call->hc_res, route->hrt_user);

  if (done) {
    struct HttpCall* mine = http_call_take(call->hc_id);

    /* The handler may have called http_respond() itself and *also*
     * returned non-zero; then the call is already gone and there is
     * nothing to send twice. */
    if (mine) {
      if (http_transport)
        (http_transport->ht_respond)(mine->hc_pid, &mine->hc_res);
      MyFree(mine);
      http_call_count--;
    }

    return 0;
  }

  http_timer_arm();

  return 1;
}

void http_respond(http_req_t id, const struct HttpResponse* res)
{
  struct HttpCall* call = http_call_take(id);

  /* Already answered, or timed out.  A module that answers late has been
   * slow, not wrong, and refusing to notice is the only safe thing to do
   * about it. */
  if (!call)
    return;

  if (http_transport)
    (http_transport->ht_respond)(call->hc_pid,
                                 res ? res : &call->hc_res);

  MyFree(call);
  http_call_count--;
}

/* ------------------------------------------------------------------- *
 * Bookkeeping                                                         *
 * ------------------------------------------------------------------- */

void http_drop_module(struct ModuleHandle* mod)
{
  struct HttpRoute** rp;
  struct HttpCall** cp;

  rp = &http_routes;
  while (*rp) {
    struct HttpRoute* r = *rp;

    if (r->hrt_mod == mod) {
      *rp = r->hrt_next;
      MyFree(r);
    } else
      rp = &r->hrt_next;
  }

  /* A request whose handler is being unloaded will never be answered, so
   * the transport is told to let the connection go rather than hold it
   * open until the deadline. */
  cp = &http_calls;
  while (*cp) {
    struct HttpCall* call = *cp;

    if (call->hc_mod == mod) {
      *cp = call->hc_next;
      http_finish(call, 0);
    } else
      cp = &call->hc_next;
  }
}

unsigned int http_pending(void)
{
  return http_call_count;
}

void http_shutdown(void)
{
  while (http_routes) {
    struct HttpRoute* r = http_routes;

    http_routes = r->hrt_next;
    MyFree(r);
  }

  while (http_calls) {
    struct HttpCall* call = http_calls;

    http_calls = call->hc_next;
    MyFree(call);
  }

  http_call_count = 0;
  http_transport = NULL;

  if (http_timer_armed) {
    timer_del(&http_timer);
    http_timer_armed = 0;
  }
}
