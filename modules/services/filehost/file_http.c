/*
 * IRC - Internet Relay Chat, modules/services/filehost/file_http.c
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
 * @brief The two routes: put one file here, get one file back.
 *
 * Neither handler answers in the main thread.  An upload has to be
 * recorded and a download has to be looked up, and both of those are a
 * database round trip -- so the handler keeps the request and answers
 * when the answer exists, which is what include/http.h is shaped for.
 *
 * **The bytes never cross the event loop.**  Going up, the worker wrote
 * the body to the spool and what the handler gets is a path; going down,
 * the handler names a file and the worker streams it.  A hundred
 * megabytes is a rename() and a sendfile, not a buffer in the middle of
 * the server.
 */
#include "config.h"

#include "filehost.h"

#include "ircd_alloc.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "ircd_token.h"

#include <string.h>

/** Content types served as themselves.
 *
 * Everything else is sent as an attachment of unknown type.  This is not
 * about being tidy: a file host that serves whatever type the uploader
 * declared is a way to put HTML, and therefore script, on the server's
 * own origin -- and whatever else that origin is trusted for goes with
 * it.  The list is what a chat client renders inline and nothing more.
 */
static const char* const file_inline_types[] = {
  "image/png", "image/jpeg", "image/gif", "image/webp",
  "audio/mpeg", "audio/ogg", "audio/mp4",
  "video/mp4", "video/webm",
  "text/plain",
  NULL
};

/** Non-zero if \a type is one of those. */
static int file_type_inline(const char* type)
{
  unsigned int i;

  for (i = 0; file_inline_types[i]; i++)
    if (!ircd_strcmp(type, file_inline_types[i]))
      return 1;

  return 0;
}

/** Copy a declared content type, or fall back.
 *
 * Only the type itself: parameters (@c ;charset=...) are dropped, because
 * what is being decided is whether the server will serve this as itself,
 * and a parameter cannot make that safer.
 */
static void file_type_clean(char* buf, size_t len, const char* declared)
{
  size_t i;

  ircd_strncpy(buf, "application/octet-stream", len - 1);

  if (EmptyString(declared))
    return;

  for (i = 0; i + 1 < len && declared[i]; i++) {
    unsigned char c = (unsigned char) declared[i];

    if (c == ';' || c == ' ')
      break;
    if (c < 0x21 || c > 0x7e)
      return;                      /* not a type; keep the fallback */
    buf[i] = (char) c;
  }

  if (i == 0)
    return;

  buf[i] = '\0';

  if (!file_type_inline(buf))
    ircd_strncpy(buf, "application/octet-stream", len - 1);
}

/** Reduce a name to one harmless path component.
 *
 * It is shown to people and put in a Content-Disposition, never used to
 * build a path -- the object is named by its identifier -- but a name
 * with a quote or a newline in it is a header of somebody else's
 * choosing, so what survives is letters, digits and a short list.
 */
static void file_name_clean(char* buf, size_t len, const char* given)
{
  size_t n = 0;
  size_t i;

  if (EmptyString(given)) {
    ircd_strncpy(buf, "file", len - 1);
    return;
  }

  for (i = 0; given[i] && n + 1 < len; i++) {
    unsigned char c = (unsigned char) given[i];

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
      buf[n++] = (char) c;
    else if (n && buf[n - 1] != '-')
      buf[n++] = '-';
  }

  buf[n] = '\0';

  if (!n || !strcmp(buf, ".") || !strcmp(buf, ".."))
    ircd_strncpy(buf, "file", len - 1);
}

/* ------------------------------------------------------------------- *
 * PUT /u/<id>                                                          *
 * ------------------------------------------------------------------- */

/** One upload waiting for its row to be written. */
struct FileUpload {
  http_req_t fu_req;                    /**< The request being answered. */
  char       fu_id[FILE_ID_LEN + 1];
};

/** The row is written, or it is not.  Answer the client. */
static void file_upload_recorded(int ok, void* user)
{
  struct FileUpload* up = (struct FileUpload*) user;
  struct HttpResponse res;
  char url[HTTP_PATH_MAX + 1];
  char body[HTTP_PATH_MAX + 64];

  memset(&res, 0, sizeof(res));
  res.hres_body = body;
  res.hres_bodymax = sizeof(body) - 1;

  if (!ok) {
    http_response_set(&res, 500, "text/plain", "could not store it\n", 19);
  } else {
    if (!file_url(url, sizeof(url), up->fu_id))
      ircd_strncpy(url, up->fu_id, sizeof(url) - 1);

    res.hres_status = 201;
    http_response_header(&res, "Location", url);
    http_response_set(&res, 201, "text/plain", url, strlen(url));
    /* A trailing newline, because the first thing anybody does with this
     * is pipe it into something that reads lines. */
    if (res.hres_bodylen + 1 < res.hres_bodymax) {
      res.hres_body[res.hres_bodylen++] = '\n';
      res.hres_body[res.hres_bodylen] = '\0';
    }
  }

  http_respond(up->fu_req, &res);
  MyFree(up);
}

/** Answer now, with a status and a line of text. */
static int file_answer(struct HttpResponse* res, int status, const char* text)
{
  http_response_set(res, status, "text/plain", text, strlen(text));

  return 1;
}

/** PUT or POST /u/<id> -- the upload. */
static int file_http_upload(http_req_t id, const struct HttpRequest* req,
                            struct HttpResponse* res, void* user)
{
  struct FileRow row;
  struct FileUpload* up;
  char payload[TOKEN_PAYLOAD_MAX + 1];
  char ticket[TOKEN_MAX + 1];
  const char* header;
  const char* path_id;
  char* colon;

  (void) user;

  if (!file_store_ready())
    return file_answer(res, 503, "this server is not hosting files\n");

  /* Where the body is, is the core's business: a big one was written to
   * disk by the worker as it arrived, a small one came through memory,
   * and http_request_save() below takes either.  What is refused here is
   * an upload with nothing in it. */
  if (!http_request_bodylen(req))
    return file_answer(res, 400, "no body\n");

  path_id = req->hreq_path + strlen("/u/");

  if (!file_id_valid(path_id))
    return file_answer(res, 404, "no such upload\n");

  /* The ticket.  Authorization first, because that is where a client
   * that can set headers puts it; the query is for one that cannot. */
  ticket[0] = '\0';

  if ((header = http_request_header(req, "Authorization"))
      && !ircd_strncmp(header, "Bearer ", 7))
    ircd_strncpy(ticket, header + 7, sizeof(ticket) - 1);
  else if (!ircd_strncmp(req->hreq_query, "t=", 2))
    ircd_strncpy(ticket, req->hreq_query + 2, sizeof(ticket) - 1);

  if (ircd_token_check(ticket, FILE_TICKET_LABEL, payload, sizeof(payload),
                       CurrentTime) != TOKEN_OK)
    return file_answer(res, 403, "that ticket is no good\n");

  /* <id>:<account>:<target> -- the identifier is in the ticket as well as
   * in the path, and they have to be the same one: the ticket is what
   * says this upload was asked for, and a ticket that worked for any
   * identifier would be a ticket to overwrite somebody else's file. */
  if (!(colon = strchr(payload, ':')))
    return file_answer(res, 403, "that ticket is no good\n");

  *colon = '\0';

  if (strcmp(payload, path_id))
    return file_answer(res, 403, "that ticket is for another upload\n");

  memset(&row, 0, sizeof(row));
  ircd_strncpy(row.fr_id, path_id, FILE_ID_LEN);
  row.fr_size = (long long) http_request_bodylen(req);

  {
    char* target = strchr(colon + 1, ':');

    if (target) {
      *target = '\0';
      ircd_strncpy(row.fr_target, target + 1, CHANNELLEN);
    }

    ircd_strncpy(row.fr_account, colon + 1, ACCOUNTLEN);
  }

  file_type_clean(row.fr_type, sizeof(row.fr_type),
                  http_request_header(req, "Content-Type"));
  file_name_clean(row.fr_name, sizeof(row.fr_name),
                  http_request_header(req, "X-File-Name"));

  up = (struct FileUpload*) MyCalloc(1, sizeof(*up));
  up->fu_req = id;
  ircd_strncpy(up->fu_id, path_id, FILE_ID_LEN);

  /* The body is the core's until this request is answered, and moving it
   * is how it stops being: http_request_save() takes it out of the spool,
   * and the delete that follows the answer then finds nothing. */
  file_store_put(&row, req, feature_int(FEAT_FILEHOST_RETENTION),
                 file_upload_recorded, up);

  return 0;
}

/* ------------------------------------------------------------------- *
 * GET /f/<id>                                                          *
 * ------------------------------------------------------------------- */

/** One download waiting for its row. */
struct FileDownload {
  http_req_t fd_req;
};

/** The row is there, or it is not. */
static void file_download_found(const struct FileRow* row, void* user)
{
  struct FileDownload* down = (struct FileDownload*) user;
  struct HttpResponse res;
  struct HttpRequest req;
  char path[HTTP_PATH_MAX + 1];
  char disp[FILE_NAME_MAX + 64];
  char body[64];

  memset(&res, 0, sizeof(res));
  memset(&req, 0, sizeof(req));
  res.hres_body = body;
  res.hres_bodymax = sizeof(body) - 1;

  if (!row || !file_store_path(path, sizeof(path), row->fr_id)) {
    http_response_set(&res, 404, "text/plain", "not found\n", 10);
    http_respond(down->fd_req, &res);
    MyFree(down);
    return;
  }

  /* Never sniffed, and never anything but what was decided at upload:
   * the type stored is already one of the few served as themselves. */
  http_response_header(&res, "X-Content-Type-Options", "nosniff");

  ircd_snprintf(0, disp, sizeof(disp), "%s; filename=\"%s\"",
                file_type_inline(row->fr_type) ? "inline" : "attachment",
                row->fr_name);
  http_response_header(&res, "Content-Disposition", disp);

  /* The conditional headers of the request are gone by now -- this is a
   * later turn of the loop -- so Range is lost for this answer.  What is
   * not lost is that the transport streams the file rather than buffering
   * it, which is the part that matters for a hundred megabytes. */
  http_response_file(&res, &req, path, row->fr_type);

  http_respond(down->fd_req, &res);
  MyFree(down);
}

/** GET /f/<id> -- the download. */
static int file_http_download(http_req_t id, const struct HttpRequest* req,
                              struct HttpResponse* res, void* user)
{
  struct FileDownload* down;
  const char* path_id;

  (void) user;

  if (!file_store_ready())
    return file_answer(res, 503, "this server is not hosting files\n");

  path_id = req->hreq_path + strlen("/f/");

  if (!file_id_valid(path_id))
    return file_answer(res, 404, "not found\n");

  down = (struct FileDownload*) MyCalloc(1, sizeof(*down));
  down->fd_req = id;

  file_store_get(path_id, file_download_found, down);

  return 0;
}

int file_http_start(void)
{
  if (!http_add_route(file_mod, "PUT", "/u/", file_http_upload, NULL))
    return 0;

  if (!http_add_route(file_mod, "POST", "/u/", file_http_upload, NULL))
    return 0;

  if (!http_add_route(file_mod, "GET", "/f/", file_http_download, NULL))
    return 0;

  return 1;
}
