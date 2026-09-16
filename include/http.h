/*
 * IRC - Internet Relay Chat, include/http.h
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
 * @brief HTTP: served by the core, routed to whichever module asked.
 *
 * The server is Mongoose (ircd/mongoose/, ircd/http_server.c), running on
 * a worker thread of its own; this header is what the rest of the tree
 * sees of it.  A module claims a **route** and is handed requests that
 * are already parsed, in the main thread, and never touches a socket.
 *
 * The core serves it rather than a module, because HTTP is not a thing a
 * server either has or has not: a listener is one of the server's own
 * ports, it has to come up before any module is loaded and stay up
 * across a rehash that unloads one, and its TLS is the server's TLS.
 * What is pluggable is the routes, and those are what modules bring.
 *
 * @section http_degrade A consumer degrades, it does not guess
 *
 * A module that wants a route checks http_available() -- is a port
 * configured at all -- and says so, in the log and in @c /MODULE @c LIST,
 * if there is not.  It does not fail to load: the part of it that needed
 * HTTP is switched off and the rest works.  Ask from
 * @c HOOK_CONFIG_LOADED rather than from @c mi_init, because @c mi_init
 * runs in the middle of the parse and @c FEAT_HTTP_PORT may not have been
 * read yet.
 *
 * Claiming a route before the listener is up is fine and expected.  The
 * route is what the module owns; whether anybody is listening is the
 * operator's business, and making load order matter would make it part of
 * the configuration.
 *
 * @section http_async Answering later
 *
 * A handler is called in the main thread and does **not** have to answer
 * there.  Authorising an upload means asking a database, and a handler
 * that blocked for it would stop every client on the server.  So it is
 * given an #http_req_t, and it either fills in a response and calls
 * http_respond() before returning or keeps the identifier and calls it
 * when the answer arrives.
 *
 * Every request has a deadline (@c FEAT_HTTP_TIMEOUT) on a timer of the
 * core's own.  One that passes is answered 504 and the handler's later
 * answer, if it ever comes, is dropped: a client holding a socket open
 * because a module forgot about it is the failure this exists to bound.
 *
 * @section http_threads Which thread is which
 *
 * The socket is the worker's and the routes are the main thread's, and
 * neither reaches across.  Nothing in this header may be called from the
 * worker; nothing a handler is given outlives the main thread's turn.
 */
#ifndef INCLUDED_http_h
#define INCLUDED_http_h

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif
#ifndef INCLUDED_time_h
#include <time.h>
#define INCLUDED_time_h
#endif

struct ModuleHandle;

/** Handle for one request being answered.  Zero is never a valid one. */
typedef unsigned long http_req_t;

/** Longest method name, without the NUL.  The longest real one is
 * OPTIONS; anything longer is not a method. */
#define HTTP_METHOD_MAX 15

/** Longest path the core will route on, without the NUL. */
#define HTTP_PATH_MAX 255

/** Longest query string carried to a handler. */
#define HTTP_QUERY_MAX 1024

/** Longest header name, and longest header value. */
#define HTTP_HNAME_MAX 63
#define HTTP_HVALUE_MAX 1023

/** Most headers carried in either direction.
 *
 * A request with more than this many headers is not one anybody wrote by
 * hand, and the provider is free to refuse it before the core sees it.
 */
#define HTTP_HEADERS_MAX 32

/** Largest body the core will carry, either way.
 *
 * Not a limit on what HTTP can transfer: it is a limit on what crosses
 * into the main thread.  A file upload is streamed to storage by the
 * provider and its handler is given the metadata, because a hundred
 * megabytes buffered here would be a hundred megabytes the event loop is
 * holding while every other client waits.
 */
#define HTTP_BODY_MAX 65536

/** Most routes one module may register. */
#define HTTP_ROUTES_MAX 64

/** Default milliseconds a handler has to answer. */
#define HTTP_TIMEOUT_DEFAULT_MS 5000

/** Ceiling on that.  Past this the client has given up anyway. */
#define HTTP_TIMEOUT_MAX_MS 30000

/** One header, in either direction. */
struct HttpHeader {
  char hh_name[HTTP_HNAME_MAX + 1];   /**< Name, as it was written. */
  char hh_value[HTTP_HVALUE_MAX + 1]; /**< Value, unfolded, trimmed. */
};

/** A request, already parsed, as a handler sees it.
 *
 * Everything in it is valid until the handler returns.  A handler that
 * answers later copies what it needs, the way every other asynchronous
 * caller in this tree does.
 */
struct HttpRequest {
  const char* hreq_method;   /**< "GET", "POST", uppercase. */
  const char* hreq_path;     /**< Path, decoded, without the query. */
  const char* hreq_query;    /**< After the '?', raw, or "". */
  const char* hreq_remote;   /**< Who asked, as text. */
  int         hreq_tls;      /**< Non-zero if it arrived over TLS. */
  unsigned int hreq_nheaders;            /**< How many. */
  const struct HttpHeader* hreq_headers; /**< #hreq_nheaders of them. */
  const char* hreq_body;     /**< The body, NUL-terminated for
                                  convenience; may hold NULs. */
  size_t      hreq_bodylen;  /**< Its length in bytes. */
};

/** What a handler answers with.
 *
 * The core owns the storage.  A handler fills in the status, the content
 * type and the body, and may add headers with http_response_header().
 */
struct HttpResponse {
  int    hres_status;                    /**< 200, 404, ... */
  char   hres_type[HTTP_HVALUE_MAX + 1]; /**< Content-Type. */
  unsigned int hres_nheaders;            /**< Extra headers. */
  struct HttpHeader hres_headers[HTTP_HEADERS_MAX];
  char*  hres_body;                      /**< Body buffer, core-owned. */
  size_t hres_bodylen;                   /**< Bytes written into it. */
  size_t hres_bodymax;                   /**< Size of #hres_body. */
};

/** Handles one request.  Runs in the main thread.
 *
 * @param[in] id Handle to answer with, if not answering now.
 * @param[in] req What was asked.  Valid until this returns.
 * @param[in,out] res Where to write the answer.
 * @param[in] user What was passed to http_add_route().
 * @return Non-zero if \a res has been filled in and the core should send
 *   it; zero if the handler has kept \a id and will call http_respond().
 */
typedef int (*HttpHandlerFn)(http_req_t id, const struct HttpRequest* req,
                             struct HttpResponse* res, void* user);

/** How an answer reaches the socket.
 *
 * Core-internal: ircd/http_server.c is the one implementation and sets it
 * when the listener comes up.  It is an interface rather than a direct
 * call so that ircd/http.c -- the routes, the requests in flight and the
 * deadline -- stays testable with no socket at all (@c http_t), the same
 * split ircd/migration.c has from ircd/migration_run.c.  A module has no
 * business here.
 */
struct HttpTransport {
  /** Its name, for the log and for /STATS. */
  const char* ht_name;

  /** Send an answer for a request the core dispatched.
   * @param[in] id The request, as http_dispatch() was given it.
   * @param[in] res What to send.  Valid only during the call.
   */
  void (*ht_respond)(http_req_t id, const struct HttpResponse* res);

  /** Forget a request the core will never answer.
   *
   * Called when the module owning its route is unloaded, so the
   * connection is let go rather than held for an answer that is not
   * coming.
   */
  void (*ht_cancel)(http_req_t id);
};

/*
 * The server side.  ircd/http_server.c calls these; nothing else does.
 */

/** Install the transport.  One at a time.
 * @param[in] transport Its calls.  Must outlive the installation.
 * @return Non-zero on success.
 */
extern int http_set_transport(const struct HttpTransport* transport);

/** Take it away.  Every request in flight is dropped. */
extern void http_clear_transport(void);

/** The transport's name, or NULL when nothing is listening. */
extern const char* http_transport_name(void);

/** Non-zero when the listener is up this instant.
 *
 * For @c /STATS and the log.  A consumer wants http_available() instead:
 * whether the listener is momentarily down across a rehash is not a
 * module's business.
 */
extern int http_listening(void);

/*
 * The consumer side.
 */

/** Non-zero when this server is configured to serve HTTP at all.
 *
 * That is @c FEAT_HTTP_PORT, not "the socket is open this instant": a
 * consumer wants to know whether its route will ever be reached, and the
 * listener coming and going across a rehash is not its business.  See
 * @ref http_degrade for when to ask.
 */
extern int http_available(void);

/** Claim a route.
 *
 * A path ending in @c / matches everything under it -- @c "/files/" takes
 * @c "/files/abc" -- and anything else matches exactly.  That is the
 * whole routing language on purpose: a pattern syntax is a thing every
 * consumer would have to learn and every provider would have to agree
 * about.
 *
 * @param[in] mod The module that wants it; the route goes when it does.
 * @param[in] method "GET", "POST"; matched case-insensitively.
 * @param[in] path Where, starting with '/'.
 * @param[in] fn What to call.
 * @param[in] user Passed back to \a fn.
 * @return Non-zero on success; zero if the path is malformed, already
 *   claimed for that method, or this module has too many.
 */
extern int http_add_route(struct ModuleHandle* mod, const char* method,
                          const char* path, HttpHandlerFn fn, void* user);

/** Give up one route.
 * @return Non-zero if it was found.
 */
extern int http_del_route(struct ModuleHandle* mod, const char* method,
                          const char* path);

/** How many routes a module holds. */
extern unsigned int http_route_count(const struct ModuleHandle* mod);

/** Answer a request whose handler returned zero.
 *
 * Safe to call for a request that has already been answered or has timed
 * out: it does nothing.  A module that answers late has not done anything
 * wrong, it has been slow, and crashing it for that would be worse.
 *
 * @param[in] id The request.
 * @param[in] res What to send.
 */
extern void http_respond(http_req_t id, const struct HttpResponse* res);

/** Prepare a response: a status, a content type and a body.
 *
 * The convenience every handler would otherwise write for itself.  The
 * body is copied into the core's buffer and truncated at
 * #HTTP_BODY_MAX.
 */
extern void http_response_set(struct HttpResponse* res, int status,
                              const char* type, const char* body,
                              size_t bodylen);

/** Add one header to a response.
 * @return Non-zero on success; zero if there is no room.
 */
extern int http_response_header(struct HttpResponse* res, const char* name,
                                const char* value);

/** One header of a request, by name, case-insensitively, or NULL. */
extern const char* http_request_header(const struct HttpRequest* req,
                                       const char* name);

/*
 * ircd/http_server.c calls this; nothing else does.
 */

/** Hand a parsed request to whatever claimed its route.
 *
 * @param[in] req The request.
 * @param[in] pid What the transport wants to be called back with -- the
 *   connection, in its own terms.  The core passes it back unchanged, so
 *   the transport never has to keep a table of the core's handles.
 * @return Zero when the request was answered synchronously (ht_respond()
 *   has already run), non-zero when an answer is coming later, and
 *   negative when nothing claimed the route -- in which case the
 *   transport sends its own 404 and the core holds nothing.
 */
extern int http_dispatch(const struct HttpRequest* req, http_req_t pid);

/*
 * Bookkeeping the core does for itself.
 */

/** Drop every route and request belonging to a module being unloaded. */
extern void http_drop_module(struct ModuleHandle* mod);

/** Fail every request whose deadline has passed.
 *
 * The timer calls this; a test calls it directly, which is the only way
 * to exercise a deadline without a running event loop.
 * @param[in] now The time to judge against.
 * @return How many were failed.
 */
extern int http_expire(time_t now);

/** Requests in flight. */
extern unsigned int http_pending(void);

/** Forget everything.  For the tests, and for shutdown. */
extern void http_shutdown(void);

#endif /* INCLUDED_http_h */
