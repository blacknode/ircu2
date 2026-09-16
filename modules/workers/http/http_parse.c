/*
 * IRC - Internet Relay Chat, modules/workers/http/http_parse.c
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
 * @brief Turning bytes into a request, and a response into bytes.
 *
 * Runs on the worker thread and touches nothing but its arguments: no
 * core state, no allocator but worker_alloc(), no log.  That is the rule
 * every worker follows and this file is where it would be easiest to
 * forget it.
 *
 * It parses the subset of HTTP/1.1 a server like this one needs: a
 * request line, headers, and a body whose length is given by
 * Content-Length.  There is no chunked request body, and that is a
 * refusal rather than silence -- a parser that quietly ignores a framing
 * it does not understand is one that can be made to read one request as
 * two.  What comes after a request stays in the connection's buffer and
 * is parsed once that request has been answered, so a client that
 * pipelined is not answered "keep-alive" and then ignored.
 */
#include "config.h"

#include "http_priv.h"
#include "worker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Lower-case one ASCII byte.  Not ToLower(): this is HTTP, where the
 * case rules are ASCII's and not IRC's. */
static char httpd_lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
}

/** Compare two strings, ASCII case-insensitively. */
static int httpd_casecmp(const char* a, const char* b)
{
  while (*a && *b) {
    char ca = httpd_lower(*a++);
    char cb = httpd_lower(*b++);

    if (ca != cb)
      return (int) ((unsigned char) ca) - (int) ((unsigned char) cb);
  }

  return (int) ((unsigned char) httpd_lower(*a))
       - (int) ((unsigned char) httpd_lower(*b));
}

/** Copy \a len bytes of \a src into \a dst, NUL-terminated and bounded. */
static void httpd_copy(char* dst, size_t dstlen, const char* src, size_t len)
{
  if (len >= dstlen)
    len = dstlen - 1;

  memcpy(dst, src, len);
  dst[len] = '\0';
}

/** Decode %XX in a path, in place.
 *
 * Only in the path, and only after the query has been split off: a query
 * string is the application's to decode, because "+" means a space there
 * and nowhere else, and a server that decided would be deciding for every
 * route at once.
 *
 * @param[in,out] path The path, decoded in place.
 * @return Non-zero when it decoded; zero when the path is one this server
 *   refuses to read at all -- see the comment on the encoded separator.
 */
static int httpd_unescape(char* path)
{
  char* out = path;
  const char* in = path;

  while (*in) {
    if (in[0] == '%' && in[1] && in[2]) {
      char hex[3];
      long v;
      char* end;

      hex[0] = in[1];
      hex[1] = in[2];
      hex[2] = '\0';

      v = strtol(hex, &end, 16);

      if (*end == '\0' && v > 0 && v < 256) {
        /* An encoded separator, or an encoded NUL, is refused rather than
         * decoded.  Routing happens on the decoded path, so "%2F" would
         * turn what the client wrote as data into a separator and let a
         * request reach a prefix route it does not belong under; "%00"
         * would shorten the path for whoever reads it with str*() next.
         * Both are the ambiguity a two-rule routing language exists to
         * not have, and neither is something anybody writes by hand. */
        if (v == '/' || v == '\\')
          return 0;

        *out++ = (char) v;
        in += 3;
        continue;
      }

      if (*end == '\0' && v == 0)
        return 0;
    }

    *out++ = *in++;
  }

  *out = '\0';

  return 1;
}

/** Non-zero if \a path is one this server will route on.
 *
 * It has to start with a slash and it may not contain "..".  Not because
 * anything here opens a file -- nothing does -- but because a path is
 * handed to modules, and one of them will open a file eventually.  The
 * check belongs where the path is first believed.
 */
static int httpd_path_ok(const char* path)
{
  const char* p;

  if (path[0] != '/')
    return 0;

  for (p = path; *p; p++) {
    if ((unsigned char) *p < 0x20)
      return 0;
    if (p[0] == '.' && p[1] == '.')
      return 0;
  }

  return 1;
}

/** Parse the head of a request into \a xfer.
 * @return 0 on success, or the HTTP status to refuse with.
 */
static int httpd_parse_head(char* head, struct HttpXfer* xfer)
{
  char* line = head;
  char* nl;
  char* sp;
  char* query;

  /* The request line: METHOD SP TARGET SP VERSION */
  if (!(nl = strchr(line, '\n')))
    return 400;

  *nl = '\0';
  if (nl > line && nl[-1] == '\r')
    nl[-1] = '\0';

  if (!(sp = strchr(line, ' ')))
    return 400;
  *sp++ = '\0';

  if (strlen(line) > HTTP_METHOD_MAX)
    return 501;

  httpd_copy(xfer->hx_method, sizeof(xfer->hx_method), line, strlen(line));

  {
    char* vsp = strchr(sp, ' ');

    if (!vsp)
      return 400;
    *vsp++ = '\0';

    /* HTTP/1.1 or HTTP/1.0.  Anything else is a framing this file does
     * not implement, and guessing at it is how one request becomes two. */
    if (strncmp(vsp, "HTTP/1.", 7) || (vsp[7] != '0' && vsp[7] != '1')
        || vsp[8])
      return 505;

    xfer->hx_version = vsp[7];
  }

  if ((query = strchr(sp, '?'))) {
    *query++ = '\0';
    httpd_copy(xfer->hx_query, sizeof(xfer->hx_query), query, strlen(query));
  } else
    xfer->hx_query[0] = '\0';

  if (!httpd_unescape(sp) || !httpd_path_ok(sp))
    return 400;

  if (strlen(sp) > HTTP_PATH_MAX)
    return 414;

  httpd_copy(xfer->hx_path, sizeof(xfer->hx_path), sp, strlen(sp));

  /* The headers. */
  line = nl + 1;
  xfer->hx_nheaders = 0;

  while (*line) {
    char* colon;
    char* value;

    if (!(nl = strchr(line, '\n')))
      return 400;

    *nl = '\0';
    if (nl > line && nl[-1] == '\r')
      nl[-1] = '\0';

    if (!*line)
      break;                    /* the blank line: headers are over */

    /* A leading space is an obsolete folded header.  Refused rather than
     * joined: nothing sends them, and the two readers that disagree about
     * whether this is one header or two are how a request is smuggled. */
    if (*line == ' ' || *line == '\t')
      return 400;

    if (!(colon = strchr(line, ':')))
      return 400;

    *colon = '\0';
    value = colon + 1;

    while (*value == ' ' || *value == '\t')
      value++;

    if (xfer->hx_nheaders < HTTP_HEADERS_MAX) {
      struct HttpHeader* h = &xfer->hx_headers[xfer->hx_nheaders++];

      httpd_copy(h->hh_name, sizeof(h->hh_name), line, strlen(line));
      httpd_copy(h->hh_value, sizeof(h->hh_value), value, strlen(value));
    }

    line = nl + 1;
  }

  return 0;
}

/** One header of a parsed request, or NULL. */
static const char* httpd_header(const struct HttpXfer* xfer,
                                const char* name)
{
  unsigned int i;

  for (i = 0; i < xfer->hx_nheaders; i++)
    if (!httpd_casecmp(xfer->hx_headers[i].hh_name, name))
      return xfer->hx_headers[i].hh_value;

  return 0;
}

int httpd_parse(struct HttpConn* conn, struct HttpXfer* xfer)
{
  char head[HTTPD_HEAD_MAX + 1];
  const char* len;
  const char* enc;
  const char* connection;
  size_t headlen;
  size_t used;
  int status;

  /* The head ends at the first blank line.  Until it does there is
   * nothing to decide, except that it is not allowed to go on for ever. */
  if (!conn->hcn_headlen) {
    char* end;

    conn->hcn_in[conn->hcn_inlen] = '\0';

    if (!(end = strstr(conn->hcn_in, "\r\n\r\n"))) {
      if (!(end = strstr(conn->hcn_in, "\n\n"))) {
        if (conn->hcn_inlen > HTTPD_HEAD_MAX)
          return -431;
        return 0;
      }
      conn->hcn_headlen = (size_t) (end - conn->hcn_in) + 2;
    } else
      conn->hcn_headlen = (size_t) (end - conn->hcn_in) + 4;

    if (conn->hcn_headlen > HTTPD_HEAD_MAX)
      return -431;
  }

  headlen = conn->hcn_headlen;

  memset(xfer, 0, sizeof(*xfer));

  httpd_copy(head, sizeof(head), conn->hcn_in, headlen);

  if ((status = httpd_parse_head(head, xfer)))
    return -status;

  /* No chunked request body.  Refused rather than ignored: a server that
   * reads a chunked body as if it were not one has been handed the next
   * request as this one's content. */
  if ((enc = httpd_header(xfer, "Transfer-Encoding"))
      && httpd_casecmp(enc, "identity"))
    return -501;

  conn->hcn_want = 0;

  if ((len = httpd_header(xfer, "Content-Length"))) {
    char* end;
    long v = strtol(len, &end, 10);

    if (*end || v < 0)
      return -400;

    if ((size_t) v > HTTPD_BODY_MAX)
      return -413;

    conn->hcn_want = (size_t) v;
  }

  if (conn->hcn_inlen < headlen + conn->hcn_want)
    return 0;                   /* more body to come */

  memcpy(xfer->hx_body, conn->hcn_in + headlen, conn->hcn_want);
  xfer->hx_body[conn->hcn_want] = '\0';
  xfer->hx_bodylen = conn->hcn_want;

  httpd_copy(xfer->hx_remote, sizeof(xfer->hx_remote), conn->hcn_remote,
             strlen(conn->hcn_remote));

  /* HTTP/1.1 keeps the connection open unless it is told not to; 1.0
   * closes unless it is told not to.  Answering "keep-alive" to a client
   * that did not ask for it is how a 1.0 client ends up waiting for a
   * second response on a connection it meant to be the last. */
  connection = httpd_header(xfer, "Connection");

  if (xfer->hx_version == '1')
    conn->hcn_keep = !connection || httpd_casecmp(connection, "close");
  else
    conn->hcn_keep = connection && !httpd_casecmp(connection, "keep-alive");

  /* Take this request out of the buffer and keep whatever came after it.
   * A client that pipelined sent the next request in the same packet, and
   * throwing it away while answering "keep-alive" would lose it silently.
   * There is nothing unbounded about holding it: the buffer is one head
   * and one body, the connection is not read again until this request has
   * been answered, and what is left is parsed then. */
  used = headlen + conn->hcn_want;

  if (conn->hcn_inlen > used)
    memmove(conn->hcn_in, conn->hcn_in + used, conn->hcn_inlen - used);

  conn->hcn_inlen -= used;
  conn->hcn_headlen = 0;
  conn->hcn_want = 0;

  return 1;
}

/** The text for a status, for the status line. */
static const char* httpd_reason(int status)
{
  switch (status) {
  case 200: return "OK";
  case 201: return "Created";
  case 202: return "Accepted";
  case 204: return "No Content";
  case 301: return "Moved Permanently";
  case 302: return "Found";
  case 304: return "Not Modified";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 409: return "Conflict";
  case 413: return "Content Too Large";
  case 414: return "URI Too Long";
  case 429: return "Too Many Requests";
  case 431: return "Request Header Fields Too Large";
  case 500: return "Internal Server Error";
  case 501: return "Not Implemented";
  case 503: return "Service Unavailable";
  case 504: return "Gateway Timeout";
  case 505: return "HTTP Version Not Supported";
  default:  return "Unknown";
  }
}

/** Build a response.
 * @param[in] status Its status.
 * @param[in] type Content-Type, or NULL.
 * @param[in] headers Extra headers, or NULL.
 * @param[in] nheaders How many.
 * @param[in] body The body, or NULL.
 * @param[in] bodylen Its length.
 * @param[in] keep Non-zero to keep the connection open.
 * @param[out] len Receives the length.
 * @return A worker_alloc()ed buffer, or NULL.
 */
static char* httpd_build(int status, const char* type,
                         const struct HttpHeader* headers,
                         unsigned int nheaders, const char* body,
                         size_t bodylen, int keep, size_t* len)
{
  size_t cap = HTTPD_HEAD_MAX + bodylen + 256;
  char* buf = (char*) worker_alloc(cap);
  size_t n;
  unsigned int i;

  if (!buf)
    return 0;

  n = (size_t) snprintf(buf, cap,
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Length: %lu\r\n"
                        "Content-Type: %s\r\n"
                        "Connection: %s\r\n",
                        status, httpd_reason(status),
                        (unsigned long) bodylen,
                        type && *type ? type : "text/plain",
                        keep ? "keep-alive" : "close");

  if (n >= cap) {
    worker_free(buf);
    return 0;
  }

  for (i = 0; i < nheaders; i++) {
    int wrote;

    /* A header a module wrote is not allowed to end the head early: a
     * newline in a value is how a response becomes two responses, and
     * the second one is whatever the module was handed. */
    if (strpbrk(headers[i].hh_name, "\r\n:")
        || strpbrk(headers[i].hh_value, "\r\n"))
      continue;

    wrote = snprintf(buf + n, cap - n, "%s: %s\r\n",
                     headers[i].hh_name, headers[i].hh_value);

    if (wrote < 0 || (size_t) wrote >= cap - n) {
      worker_free(buf);
      return 0;
    }

    n += (size_t) wrote;
  }

  if (n + 2 + bodylen >= cap) {
    worker_free(buf);
    return 0;
  }

  memcpy(buf + n, "\r\n", 2);
  n += 2;

  if (body && bodylen) {
    memcpy(buf + n, body, bodylen);
    n += bodylen;
  }

  *len = n;

  return buf;
}

char* httpd_render(const struct HttpXfer* xfer, int keep, size_t* len)
{
  return httpd_build(xfer->hx_status, xfer->hx_type, xfer->hx_reply,
                     xfer->hx_nreply, xfer->hx_body, xfer->hx_replylen,
                     keep, len);
}

char* httpd_render_status(int status, int keep, size_t* len)
{
  char body[128];
  int n = snprintf(body, sizeof(body), "%d %s\n", status,
                   httpd_reason(status));

  if (n < 0)
    n = 0;

  return httpd_build(status, "text/plain", 0, 0, body, (size_t) n, keep,
                     len);
}
