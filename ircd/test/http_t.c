/* http_t.c - Test the HTTP provider register, the routes and the requests.
 *
 * There is no socket in ircd/http.c and there is none here: what this has
 * to pin down is that a module asking for a route gets the requests it
 * asked for and nobody else's, that a consumer with no provider degrades
 * instead of failing, and that a request nobody answers is answered by
 * the deadline rather than held for ever.
 */

#include "http.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_string.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* --- stubs ---------------------------------------------------------- */

int feature_int(enum Feature feat)
{
  (void) feat;
  return 0;
}

const char* feature_str(enum Feature feat)
{
  (void) feat;
  return "";
}

/* Module handles; the register only ever compares these pointers. */
static struct ModuleHandle* const MOD_PROVIDER = (struct ModuleHandle*) 0x1;
static struct ModuleHandle* const MOD_A = (struct ModuleHandle*) 0x2;
static struct ModuleHandle* const MOD_B = (struct ModuleHandle*) 0x3;

/* --- what the fake provider was told --------------------------------- */

static int prov_responses;
static int prov_cancels;
static http_req_t prov_last_pid;
static int prov_last_status;
static char prov_last_body[HTTP_BODY_MAX + 1];

static void prov_reset(void)
{
  prov_responses = 0;
  prov_cancels = 0;
  prov_last_pid = 0;
  prov_last_status = 0;
  prov_last_body[0] = '\0';
}

static void fake_respond(http_req_t id, const struct HttpResponse* res)
{
  prov_responses++;
  prov_last_pid = id;
  prov_last_status = res->hres_status;

  if (res->hres_body && res->hres_bodylen) {
    memcpy(prov_last_body, res->hres_body, res->hres_bodylen);
    prov_last_body[res->hres_bodylen] = '\0';
  } else
    prov_last_body[0] = '\0';
}

static void fake_cancel(http_req_t id)
{
  prov_cancels++;
  prov_last_pid = id;
}

static const struct HttpProvider fake_provider = {
  "fake", fake_respond, fake_cancel
};

/* --- what the fake handlers were asked -------------------------------- */

static int handler_calls;
static char handler_path[HTTP_PATH_MAX + 1];
static char handler_query[HTTP_QUERY_MAX + 1];
static http_req_t handler_last_id;
static void* handler_last_user;

/** Answers on the spot. */
static int handler_now(http_req_t id, const struct HttpRequest* req,
                       struct HttpResponse* res, void* user)
{
  (void) id;
  handler_calls++;
  handler_last_user = user;
  ircd_strncpy(handler_path, req->hreq_path, sizeof(handler_path) - 1);
  ircd_strncpy(handler_query, req->hreq_query, sizeof(handler_query) - 1);

  http_response_set(res, 201, "application/json", "{\"ok\":true}", 11);

  return 1;
}

/** Keeps the identifier and answers later, or never. */
static int handler_later(http_req_t id, const struct HttpRequest* req,
                         struct HttpResponse* res, void* user)
{
  (void) req;
  (void) res;
  (void) user;
  handler_calls++;
  handler_last_id = id;

  return 0;
}

/* --- a request to hand in --------------------------------------------- */

static struct HttpRequest make_req(const char* method, const char* path,
                                   const char* query,
                                   const struct HttpHeader* headers,
                                   unsigned int nheaders)
{
  struct HttpRequest req;

  memset(&req, 0, sizeof(req));
  req.hreq_method = method;
  req.hreq_path = path;
  req.hreq_query = query ? query : "";
  req.hreq_remote = "192.0.2.1";
  req.hreq_headers = headers;
  req.hreq_nheaders = nheaders;
  req.hreq_body = "";
  req.hreq_bodylen = 0;

  return req;
}

/* --- tests ------------------------------------------------------------ */

/** With no provider a consumer gets nothing, and that is not an error. */
static void test_no_provider(void)
{
  struct HttpRequest req = make_req("GET", "/health", 0, 0, 0);

  http_shutdown();
  prov_reset();

  assert(http_available() == 0);
  assert(http_provider_name() == 0);

  /* A route may be claimed before a provider exists: the module that
   * wants it loads first as often as not, and making that an error would
   * make load order part of the configuration. */
  assert(http_add_route(MOD_A, "GET", "/health", handler_now, 0) == 1);
  assert(http_route_count(MOD_A) == 1);

  /* But nothing can be dispatched, and http_dispatch() says so without
   * touching anything. */
  assert(http_dispatch(&req, 7) < 0);
  assert(http_pending() == 0);

  printf("  no provider: a route waits, a request is refused\n");
}

/** One provider at a time. */
static void test_one_provider(void)
{
  static const struct HttpProvider other = { "other", fake_respond,
                                             fake_cancel };

  http_shutdown();

  assert(http_register_provider(MOD_PROVIDER, &fake_provider) == 1);
  assert(http_available() == 1);
  assert(!strcmp(http_provider_name(), "fake"));

  /* A second is refused rather than replacing the first: two things
   * owning the same socket is not a state to arrive at silently. */
  assert(http_register_provider(MOD_B, &other) == 0);
  assert(!strcmp(http_provider_name(), "fake"));

  /* And one that is missing half its calls is not a provider. */
  {
    static const struct HttpProvider broken = { "broken", 0, 0 };

    http_unregister_provider(MOD_PROVIDER);
    assert(http_register_provider(MOD_B, &broken) == 0);
    assert(http_available() == 0);
  }

  printf("  one provider at a time, and it has to be whole\n");
}

/** A handler that answers on the spot. */
static void test_synchronous(void)
{
  struct HttpHeader headers[1];
  struct HttpRequest req;

  http_shutdown();
  prov_reset();
  handler_calls = 0;

  ircd_strncpy(headers[0].hh_name, "X-Thing", sizeof(headers[0].hh_name) - 1);
  ircd_strncpy(headers[0].hh_value, "yes", sizeof(headers[0].hh_value) - 1);

  http_register_provider(MOD_PROVIDER, &fake_provider);
  assert(http_add_route(MOD_A, "POST", "/upload",
                        handler_now, (void*) 0x55) == 1);

  req = make_req("POST", "/upload", "a=1", headers, 1);

  assert(http_dispatch(&req, 42) == 0);
  assert(handler_calls == 1);
  assert(handler_last_user == (void*) 0x55);
  assert(!strcmp(handler_path, "/upload"));
  assert(!strcmp(handler_query, "a=1"));

  /* The provider was answered with its own identifier, not the core's. */
  assert(prov_responses == 1);
  assert(prov_last_pid == 42);
  assert(prov_last_status == 201);
  assert(!strcmp(prov_last_body, "{\"ok\":true}"));

  /* Nothing is left holding an answer open. */
  assert(http_pending() == 0);

  /* And the header lookup is case-insensitive, because HTTP's are. */
  assert(!strcmp(http_request_header(&req, "x-thing"), "yes"));
  assert(http_request_header(&req, "X-Other") == 0);

  printf("  a handler that answers now is answered now\n");
}

/** A handler that answers later. */
static void test_asynchronous(void)
{
  struct HttpRequest req = make_req("GET", "/slow", 0, 0, 0);
  struct HttpResponse res;
  char body[64];

  http_shutdown();
  prov_reset();
  handler_calls = 0;
  handler_last_id = 0;

  http_register_provider(MOD_PROVIDER, &fake_provider);
  http_add_route(MOD_A, "GET", "/slow", handler_later, 0);

  assert(http_dispatch(&req, 99) == 1);
  assert(handler_calls == 1);
  assert(handler_last_id != 0);

  /* Held: nothing has gone to the provider yet. */
  assert(prov_responses == 0);
  assert(http_pending() == 1);

  memset(&res, 0, sizeof(res));
  res.hres_body = body;
  res.hres_bodymax = sizeof(body) - 1;
  http_response_set(&res, 200, "text/plain", "done", 4);

  http_respond(handler_last_id, &res);

  assert(prov_responses == 1);
  assert(prov_last_pid == 99);
  assert(prov_last_status == 200);
  assert(!strcmp(prov_last_body, "done"));
  assert(http_pending() == 0);

  /* Answering twice is not a crash.  A module that answers late has been
   * slow, not wrong. */
  http_respond(handler_last_id, &res);
  assert(prov_responses == 1);

  printf("  a handler that answers later is answered later, once\n");
}

/** A request nobody ever answers. */
static void test_deadline(void)
{
  struct HttpRequest req = make_req("GET", "/slow", 0, 0, 0);

  http_shutdown();
  prov_reset();

  CurrentTime = 1000;

  http_register_provider(MOD_PROVIDER, &fake_provider);
  http_add_route(MOD_A, "GET", "/slow", handler_later, 0);

  assert(http_dispatch(&req, 5) == 1);
  assert(http_pending() == 1);

  /* Not yet. */
  assert(http_expire(CurrentTime) == 0);
  assert(http_pending() == 1);

  /* And then, whatever the module is doing. */
  assert(http_expire(CurrentTime + 3600) == 1);
  assert(http_pending() == 0);
  assert(prov_responses == 1);
  assert(prov_last_status == 504);

  printf("  a request nobody answers is answered by the deadline\n");
}

/** Which route takes a path. */
static void test_routing(void)
{
  struct HttpRequest req;

  http_shutdown();
  prov_reset();
  handler_calls = 0;

  http_register_provider(MOD_PROVIDER, &fake_provider);

  assert(http_add_route(MOD_A, "GET", "/files/", handler_now, 0) == 1);
  assert(http_add_route(MOD_A, "GET", "/files/thumb/", handler_now, 0) == 1);
  assert(http_add_route(MOD_A, "GET", "/files/index", handler_now, 0) == 1);

  /* A trailing slash means everything under it. */
  req = make_req("GET", "/files/abc", 0, 0, 0);
  assert(http_dispatch(&req, 1) == 0);
  assert(!strcmp(handler_path, "/files/abc"));

  /* The longer prefix wins: the module that asked for it meant it. */
  req = make_req("GET", "/files/thumb/abc", 0, 0, 0);
  assert(http_dispatch(&req, 2) == 0);
  assert(!strcmp(handler_path, "/files/thumb/abc"));

  /* An exact match beats any prefix. */
  req = make_req("GET", "/files/index", 0, 0, 0);
  assert(http_dispatch(&req, 3) == 0);
  assert(!strcmp(handler_path, "/files/index"));

  /* The method is part of the route. */
  req = make_req("DELETE", "/files/abc", 0, 0, 0);
  assert(http_dispatch(&req, 4) < 0);

  /* And nothing claimed is nothing claimed. */
  req = make_req("GET", "/elsewhere", 0, 0, 0);
  assert(http_dispatch(&req, 5) < 0);

  /* A second claim on the same method and path is refused: two handlers
   * for one route is a question with no right answer. */
  assert(http_add_route(MOD_B, "GET", "/files/", handler_now, 0) == 0);

  /* A path that is not a path is refused too. */
  assert(http_add_route(MOD_B, "GET", "files", handler_now, 0) == 0);
  assert(http_add_route(MOD_B, "GET", "/x", 0, 0) == 0);

  printf("  exact beats prefix, longer prefix beats shorter\n");
}

/** What unloading takes with it. */
static void test_unload(void)
{
  struct HttpRequest req = make_req("GET", "/slow", 0, 0, 0);

  http_shutdown();
  prov_reset();

  http_register_provider(MOD_PROVIDER, &fake_provider);
  http_add_route(MOD_A, "GET", "/slow", handler_later, 0);
  http_add_route(MOD_B, "GET", "/other", handler_now, 0);

  assert(http_dispatch(&req, 11) == 1);
  assert(http_pending() == 1);

  /* The consumer goes: its routes go, and the request it will never
   * answer is given back to the provider rather than held to the
   * deadline. */
  http_drop_module(MOD_A);

  assert(http_route_count(MOD_A) == 0);
  assert(http_route_count(MOD_B) == 1);
  assert(http_pending() == 0);
  assert(prov_cancels == 1);
  assert(prov_last_pid == 11);

  /* The provider goes: everything in flight goes with it, silently,
   * because there is nobody left to answer through. */
  prov_reset();
  http_add_route(MOD_A, "GET", "/slow", handler_later, 0);
  assert(http_dispatch(&req, 12) == 1);
  assert(http_pending() == 1);

  http_drop_module(MOD_PROVIDER);

  assert(http_available() == 0);
  assert(http_pending() == 0);
  assert(prov_responses == 0);
  assert(prov_cancels == 0);

  /* The consumer's routes are still there, waiting for another
   * provider. */
  assert(http_route_count(MOD_A) == 1);

  printf("  unloading takes the routes, and the requests with them\n");
}

/** Giving a route back. */
static void test_del_route(void)
{
  http_shutdown();

  http_add_route(MOD_A, "GET", "/a", handler_now, 0);
  http_add_route(MOD_A, "GET", "/b", handler_now, 0);

  assert(http_route_count(MOD_A) == 2);

  /* Somebody else's route is not yours to remove. */
  assert(http_del_route(MOD_B, "GET", "/a") == 0);
  assert(http_route_count(MOD_A) == 2);

  assert(http_del_route(MOD_A, "GET", "/a") == 1);
  assert(http_route_count(MOD_A) == 1);
  assert(http_del_route(MOD_A, "GET", "/a") == 0);

  printf("  a route is given back only by whoever claimed it\n");
}

/** A body longer than the core will carry. */
static void test_body_is_bounded(void)
{
  struct HttpResponse res;
  static char big[HTTP_BODY_MAX * 2];
  static char buf[HTTP_BODY_MAX + 1];

  memset(big, 'x', sizeof(big));
  memset(&res, 0, sizeof(res));
  res.hres_body = buf;
  res.hres_bodymax = HTTP_BODY_MAX;

  http_response_set(&res, 200, "text/plain", big, sizeof(big));

  /* Truncated, not refused and not overflowed: the limit is on what
   * crosses into the main thread, and a handler that hit it has already
   * decided something is wrong. */
  assert(res.hres_bodylen == HTTP_BODY_MAX);
  assert(res.hres_body[HTTP_BODY_MAX] == '\0');

  /* And the headers are bounded the same way. */
  {
    unsigned int i;

    res.hres_nheaders = 0;
    for (i = 0; i < HTTP_HEADERS_MAX; i++)
      assert(http_response_header(&res, "X-Pad", "1") == 1);

    assert(http_response_header(&res, "X-One-Too-Many", "1") == 0);
  }

  printf("  a body and a header list are bounded, not trusted\n");
}

int main(void)
{
  printf("http_t:\n");

  test_no_provider();
  test_one_provider();
  test_synchronous();
  test_asynchronous();
  test_deadline();
  test_routing();
  test_unload();
  test_del_route();
  test_body_is_bounded();

  http_shutdown();

  printf("http_t: all passed\n");
  return 0;
}
