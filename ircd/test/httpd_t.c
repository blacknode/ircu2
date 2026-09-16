/* httpd_t.c - Test the HTTP provider's parser and its renderer.
 *
 * http_t covers the core's register: who gets which request, and what a
 * deadline does.  This covers the other half, the half that reads bytes
 * off a socket somebody else controls: httpd_parse() and httpd_render()
 * from modules/workers/http/http_parse.c.
 *
 * Those two functions are the module's whole attack surface and they are
 * pure -- a buffer in, a buffer out, no socket, no core state, not even
 * the module's own queue -- so they are tested here rather than through a
 * running server, the same split migration.c has from migration_run.c.
 *
 * The module's other two files are not testable this way and are not
 * tested here: http_listen.c is a poll() loop around a real descriptor
 * and http_module.c is a module.
 */

#include "http_priv.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- stubs -----------------------------------------------------------
 *
 * worker_alloc() and worker_free() are the system allocator on purpose --
 * see the comment on them in include/worker.h -- so the stub is the same
 * thing the real one is, and the test is not testing a different
 * allocator from the one the module uses.
 */

void* worker_alloc(size_t size)
{
  return calloc(1, size);
}

void worker_free(void* ptr)
{
  free(ptr);
}

/* --- helpers --------------------------------------------------------- */

static struct HttpConn conn;

/** Start a connection over, with \a text already received. */
static void feed(const char* text, size_t len)
{
  memset(&conn, 0, sizeof(conn));
  conn.hcn_fd = -1;
  conn.hcn_state = HTTPD_READING;
  conn.hcn_id = 1;
  strcpy(conn.hcn_remote, "192.0.2.7");

  assert(len < sizeof(conn.hcn_in));
  memcpy(conn.hcn_in, text, len);
  conn.hcn_inlen = len;
}

/** Feed a NUL-terminated request and parse it. */
static int parse_str(const char* text, struct HttpXfer* xfer)
{
  feed(text, strlen(text));
  return httpd_parse(&conn, xfer);
}

/** Non-zero if \a hay contains \a needle. */
static int has(const char* hay, size_t len, const char* needle)
{
  size_t nlen = strlen(needle);
  size_t i;

  for (i = 0; i + nlen <= len; i++)
    if (!memcmp(hay + i, needle, nlen))
      return 1;

  return 0;
}

/* --- the request line ------------------------------------------------ */

static void test_simple(void)
{
  struct HttpXfer xfer;

  assert(1 == parse_str("GET /a/b HTTP/1.1\r\nHost: x\r\n\r\n", &xfer));
  assert(!strcmp(xfer.hx_method, "GET"));
  assert(!strcmp(xfer.hx_path, "/a/b"));
  assert(!strcmp(xfer.hx_query, ""));
  assert(!strcmp(xfer.hx_remote, "192.0.2.7"));
  assert(xfer.hx_nheaders == 1);
  assert(!strcmp(xfer.hx_headers[0].hh_name, "Host"));
  assert(!strcmp(xfer.hx_headers[0].hh_value, "x"));
  assert(xfer.hx_bodylen == 0);

  /* HTTP/1.1 keeps the connection; the buffer is empty again. */
  assert(conn.hcn_keep);
  assert(conn.hcn_inlen == 0);

  printf("  simple request ok\n");
}

static void test_query_is_not_decoded(void)
{
  struct HttpXfer xfer;

  /* The path is decoded and the query is not: "+" means a space in a
   * query and nowhere else, and deciding that here would decide it for
   * every route at once. */
  assert(1 == parse_str("GET /a%20b?x=1+2%20&y=%2F HTTP/1.1\r\n\r\n", &xfer));
  assert(!strcmp(xfer.hx_path, "/a b"));
  assert(!strcmp(xfer.hx_query, "x=1+2%20&y=%2F"));

  printf("  query is carried raw ok\n");
}

static void test_bad_request_line(void)
{
  struct HttpXfer xfer;

  assert(-400 == parse_str("NOTAREQUEST\r\n\r\n", &xfer));
  assert(-400 == parse_str("GET /a\r\n\r\n", &xfer));

  /* A version this file does not implement is refused, not guessed at. */
  assert(-505 == parse_str("GET /a HTTP/2.0\r\n\r\n", &xfer));
  assert(-505 == parse_str("GET /a HTTP/1.\r\n\r\n", &xfer));
  assert(-505 == parse_str("GET /a HTTP/1.12\r\n\r\n", &xfer));

  /* A method longer than a method. */
  assert(-501 == parse_str("VERYLONGMETHODNAME /a HTTP/1.1\r\n\r\n", &xfer));

  printf("  malformed request lines refused ok\n");
}

static void test_path_refusals(void)
{
  struct HttpXfer xfer;

  /* Nothing here opens a file, but a path is handed to modules and one
   * of them will, so the check belongs where the path is first read. */
  assert(-400 == parse_str("GET /a/../../etc HTTP/1.1\r\n\r\n", &xfer));
  assert(-400 == parse_str("GET /a%2e%2e/b HTTP/1.1\r\n\r\n", &xfer));

  /* An encoded separator would turn what the client wrote as data into a
   * separator, and with it reach a prefix route it is not under. */
  assert(-400 == parse_str("GET /files%2Fsecret HTTP/1.1\r\n\r\n", &xfer));
  assert(-400 == parse_str("GET /files%2fsecret HTTP/1.1\r\n\r\n", &xfer));

  /* A NUL would shorten the path for whoever reads it with str*() next. */
  assert(-400 == parse_str("GET /a%00.txt HTTP/1.1\r\n\r\n", &xfer));

  /* A path that is not a path. */
  assert(-400 == parse_str("GET a/b HTTP/1.1\r\n\r\n", &xfer));

  printf("  dangerous paths refused ok\n");
}

/* --- headers --------------------------------------------------------- */

static void test_folded_header(void)
{
  struct HttpXfer xfer;

  /* Obsolete line folding, refused rather than joined: two readers that
   * disagree about whether this is one header or two is how a request is
   * smuggled past the one in front. */
  assert(-400 == parse_str("GET /a HTTP/1.1\r\nX: 1\r\n  2\r\n\r\n", &xfer));

  printf("  folded headers refused ok\n");
}

static void test_header_limits(void)
{
  char req[HTTPD_HEAD_MAX];
  struct HttpXfer xfer;
  size_t n;
  int i;

  n = (size_t) snprintf(req, sizeof(req), "GET /a HTTP/1.1\r\n");

  /* More headers than the core carries.  The extra ones are dropped and
   * the request is still a request: refusing it would make the limit a
   * thing every client had to know. */
  for (i = 0; i < HTTP_HEADERS_MAX + 8; i++)
    n += (size_t) snprintf(req + n, sizeof(req) - n, "X-%d: %d\r\n", i, i);

  n += (size_t) snprintf(req + n, sizeof(req) - n, "\r\n");

  assert(1 == parse_str(req, &xfer));
  assert(xfer.hx_nheaders == HTTP_HEADERS_MAX);

  printf("  header count capped ok\n");
}

static void test_head_too_large(void)
{
  static char req[HTTPD_HEAD_MAX + HTTPD_BODY_MAX];
  struct HttpXfer xfer;
  size_t n;

  n = (size_t) snprintf(req, sizeof(req), "GET /a HTTP/1.1\r\nX: ");
  memset(req + n, 'v', HTTPD_HEAD_MAX);
  n += HTTPD_HEAD_MAX;
  memcpy(req + n, "\r\n\r\n", 5);

  assert(-431 == parse_str(req, &xfer));

  printf("  oversized head refused ok\n");
}

/* --- the body -------------------------------------------------------- */

static void test_body_arrives_late(void)
{
  struct HttpXfer xfer;
  const char* head = "POST /a HTTP/1.1\r\nContent-Length: 5\r\n\r\nhe";

  /* Half a body is not a request yet, and saying so is the whole of the
   * incremental read: the loop puts the next bytes after these. */
  assert(0 == parse_str(head, &xfer));

  memcpy(conn.hcn_in + conn.hcn_inlen, "llo", 3);
  conn.hcn_inlen += 3;

  assert(1 == httpd_parse(&conn, &xfer));
  assert(xfer.hx_bodylen == 5);
  assert(!memcmp(xfer.hx_body, "hello", 5));

  printf("  body across two reads ok\n");
}

static void test_body_limits(void)
{
  struct HttpXfer xfer;
  char req[256];

  snprintf(req, sizeof(req), "POST /a HTTP/1.1\r\nContent-Length: %lu\r\n\r\n",
           (unsigned long) HTTPD_BODY_MAX + 1);
  assert(-413 == parse_str(req, &xfer));

  assert(-400 == parse_str("POST /a HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
                           &xfer));
  assert(-400 == parse_str("POST /a HTTP/1.1\r\nContent-Length: 5x\r\n\r\n",
                           &xfer));

  /* A chunked body is refused rather than read as if it were not one:
   * that is how the next request becomes this one's content. */
  assert(-501 == parse_str("POST /a HTTP/1.1\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n", &xfer));

  printf("  body limits and framings ok\n");
}

static void test_body_holds_nuls(void)
{
  struct HttpXfer xfer;
  char req[128];
  size_t n;

  n = (size_t) snprintf(req, sizeof(req),
                        "POST /a HTTP/1.1\r\nContent-Length: 4\r\n\r\n");
  memcpy(req + n, "a\0b\0", 4);

  feed(req, n + 4);
  assert(1 == httpd_parse(&conn, &xfer));
  assert(xfer.hx_bodylen == 4);
  assert(!memcmp(xfer.hx_body, "a\0b\0", 4));

  printf("  binary body ok\n");
}

/* --- keep-alive and what comes after --------------------------------- */

static void test_keep_alive(void)
{
  struct HttpXfer xfer;

  assert(1 == parse_str("GET /a HTTP/1.1\r\n\r\n", &xfer));
  assert(conn.hcn_keep);

  assert(1 == parse_str("GET /a HTTP/1.1\r\nConnection: close\r\n\r\n", &xfer));
  assert(!conn.hcn_keep);

  /* HTTP/1.0 is the other way round.  Answering "keep-alive" to a client
   * that did not ask is how it ends up waiting for a second response on
   * a connection it meant to be its last. */
  assert(1 == parse_str("GET /a HTTP/1.0\r\n\r\n", &xfer));
  assert(!conn.hcn_keep);

  assert(1 == parse_str("GET /a HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n",
                        &xfer));
  assert(conn.hcn_keep);

  printf("  keep-alive defaults ok\n");
}

static void test_pipelined(void)
{
  struct HttpXfer xfer;

  /* Two requests in one packet.  The second stays in the buffer: there
   * will never be another readability event for bytes that have already
   * arrived, so discarding them would lose the request silently after
   * having promised keep-alive. */
  assert(1 == parse_str("GET /one HTTP/1.1\r\n\r\n"
                        "GET /two HTTP/1.1\r\nConnection: close\r\n\r\n",
                        &xfer));
  assert(!strcmp(xfer.hx_path, "/one"));
  assert(conn.hcn_keep);
  assert(conn.hcn_inlen > 0);

  assert(1 == httpd_parse(&conn, &xfer));
  assert(!strcmp(xfer.hx_path, "/two"));
  assert(!conn.hcn_keep);
  assert(conn.hcn_inlen == 0);

  printf("  pipelined requests ok\n");
}

/* --- rendering ------------------------------------------------------- */

static void test_render(void)
{
  struct HttpXfer xfer;
  size_t len = 0;
  char* out;

  memset(&xfer, 0, sizeof(xfer));
  xfer.hx_status = 200;
  strcpy(xfer.hx_type, "application/json");
  strcpy(xfer.hx_reply[0].hh_name, "X-Thing");
  strcpy(xfer.hx_reply[0].hh_value, "yes");
  xfer.hx_nreply = 1;
  memcpy(xfer.hx_body, "{}", 2);
  xfer.hx_replylen = 2;

  out = httpd_render(&xfer, 1, &len);
  assert(out);
  assert(has(out, len, "HTTP/1.1 200 OK\r\n"));
  assert(has(out, len, "Content-Length: 2\r\n"));
  assert(has(out, len, "Content-Type: application/json\r\n"));
  assert(has(out, len, "Connection: keep-alive\r\n"));
  assert(has(out, len, "X-Thing: yes\r\n"));
  assert(has(out, len, "\r\n\r\n{}"));
  worker_free(out);

  out = httpd_render(&xfer, 0, &len);
  assert(out);
  assert(has(out, len, "Connection: close\r\n"));
  worker_free(out);

  printf("  rendering ok\n");
}

static void test_render_drops_injected_headers(void)
{
  struct HttpXfer xfer;
  size_t len = 0;
  char* out;

  memset(&xfer, 0, sizeof(xfer));
  xfer.hx_status = 200;
  strcpy(xfer.hx_type, "text/plain");

  /* A newline in a header a module wrote is how one response becomes
   * two, and the second one is whatever the module was handed.  The
   * header is dropped; the response is still sent. */
  strcpy(xfer.hx_reply[0].hh_name, "X-Bad");
  strcpy(xfer.hx_reply[0].hh_value, "1\r\nInjected: yes");
  strcpy(xfer.hx_reply[1].hh_name, "X-Also\r\nBad");
  strcpy(xfer.hx_reply[1].hh_value, "1");
  strcpy(xfer.hx_reply[2].hh_name, "X-Fine");
  strcpy(xfer.hx_reply[2].hh_value, "1");
  xfer.hx_nreply = 3;

  out = httpd_render(&xfer, 1, &len);
  assert(out);
  assert(!has(out, len, "Injected"));
  assert(!has(out, len, "X-Bad"));
  assert(!has(out, len, "X-Also"));
  assert(has(out, len, "X-Fine: 1\r\n"));
  worker_free(out);

  printf("  header injection refused ok\n");
}

static void test_render_status(void)
{
  size_t len = 0;
  char* out = httpd_render_status(404, 0, &len);

  assert(out);
  assert(has(out, len, "HTTP/1.1 404 Not Found\r\n"));
  assert(has(out, len, "Connection: close\r\n"));
  assert(has(out, len, "404 Not Found\n"));
  worker_free(out);

  printf("  status-only answers ok\n");
}

int main(void)
{
  printf("httpd_t: the HTTP provider's parser and renderer\n");

  test_simple();
  test_query_is_not_decoded();
  test_bad_request_line();
  test_path_refusals();
  test_folded_header();
  test_header_limits();
  test_head_too_large();
  test_body_arrives_late();
  test_body_limits();
  test_body_holds_nuls();
  test_keep_alive();
  test_pipelined();
  test_render();
  test_render_drops_injected_headers();
  test_render_status();

  printf("httpd_t: all ok\n");

  return 0;
}
