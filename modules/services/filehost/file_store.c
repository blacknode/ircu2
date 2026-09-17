/*
 * IRC - Internet Relay Chat, modules/services/filehost/file_store.c
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
 * @brief Every statement this module sends, and the objects on disk.
 *
 * One file, for the reason ircd/../hist_store.c is one file: retention,
 * deletion, quota and listing are four things that have to agree about
 * what a file is, and four places that each know a little of it is how
 * they stop agreeing.
 *
 * The bytes live in a directory and the rest lives in PostgreSQL.  Not
 * because the rows could not be sidecar files, but because "what is this
 * account using" and "what has expired" are questions a directory answers
 * by being walked, and a store that has to be walked to answer them is a
 * store that stops answering them once it is big.
 */
#include "config.h"

#include "filehost.h"

#include "ircd_alloc.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "random.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** Base 62, the alphabet an identifier is written in. */
static const char file_b62[] =
  "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/** Write the row and keep the object. */
static const char file_sql_insert[] =
  "INSERT INTO file (id, account, target, name, content_type, size,"
  "                  expires_at)"
  "  VALUES ($1, $2, $3, $4, $5, $6,"
  "          CASE WHEN $7::int > 0 THEN now() + ($7::int || ' days')::interval"
  "               ELSE NULL END)";

/** One file, by identifier, if it has not expired. */
static const char file_sql_get[] =
  "SELECT id, account, target, name, content_type, size FROM file"
  "  WHERE id = $1 AND (expires_at IS NULL OR expires_at > now())";

/** An account's files, newest first. */
static const char file_sql_list[] =
  "SELECT id, account, target, name, content_type, size FROM file"
  "  WHERE account = $1 AND (expires_at IS NULL OR expires_at > now())"
  "  ORDER BY created_at DESC LIMIT $2";

/** What an account is using. */
static const char file_sql_usage[] =
  "SELECT COALESCE(sum(size), 0) AS bytes FROM file WHERE account = $1";

/** Delete one, if the account asking owns it.
 *
 * The ownership is in the statement and not in a check beforehand: a
 * check and then a delete is two statements with a gap in the middle,
 * and the gap is where somebody else's delete goes.
 */
static const char file_sql_delete[] =
  "DELETE FROM file WHERE id = $1 AND ($2 = '' OR account = $2)"
  "  RETURNING id";

/** Everything past its keeping date, so the objects can go too. */
static const char file_sql_expired[] =
  "DELETE FROM file WHERE expires_at IS NOT NULL AND expires_at <= now()"
  "  RETURNING id";

/* ------------------------------------------------------------------- *
 * Identifiers and paths                                               *
 * ------------------------------------------------------------------- */

int file_id_valid(const char* id)
{
  size_t i;

  if (!id || strlen(id) != FILE_ID_LEN)
    return 0;

  for (i = 0; i < FILE_ID_LEN; i++)
    if (!strchr(file_b62, id[i]))
      return 0;

  return 1;
}

void file_id_make(char* buf, size_t len)
{
  size_t i;

  if (len < FILE_ID_LEN + 1) {
    if (len)
      buf[0] = '\0';
    return;
  }

  for (i = 0; i < FILE_ID_LEN; i++)
    buf[i] = file_b62[ircrandom() % (sizeof(file_b62) - 1)];

  buf[FILE_ID_LEN] = '\0';
}

const char* file_store_dir(void)
{
  const char* dir = feature_str(FEAT_FILEHOST_DIR);

  return dir ? dir : "";
}

int file_store_ready(void)
{
  return !EmptyString(file_store_dir()) && db_available();
}

int file_store_path(char* buf, size_t len, const char* id)
{
  unsigned int wrote;

  if (!file_id_valid(id) || EmptyString(file_store_dir()))
    return 0;

  /* Two characters of fan-out.  One directory with a million entries is
   * a directory every listing walks; two hundred and forty-odd is not. */
  wrote = ircd_snprintf(0, buf, len, "%s/%c%c/%s", file_store_dir(),
                        id[0], id[1], id);

  return wrote > 0 && wrote < len;
}

/** Make sure the fan-out directory for \a id exists.
 * @return Non-zero on success.
 */
static int file_store_mkdir(const char* id)
{
  char path[HTTP_PATH_MAX + 1];
  unsigned int wrote;

  wrote = ircd_snprintf(0, path, sizeof(path), "%s/%c%c", file_store_dir(),
                        id[0], id[1]);

  if (wrote == 0 || wrote >= sizeof(path))
    return 0;

  if (mkdir(path, 0700) == 0)
    return 1;

  /* Already there is the ordinary case after the first upload. */
  return errno == EEXIST;
}

/* ------------------------------------------------------------------- *
 * Writing                                                             *
 * ------------------------------------------------------------------- */

/** One write in flight. */
struct FilePut {
  FileDoneFn fp_done;
  void*      fp_user;
  char       fp_path[HTTP_PATH_MAX + 1];
};

/** Take the answer to the INSERT. */
static void file_store_put_done(const struct DbResult* res, void* user)
{
  struct FilePut* put = (struct FilePut*) user;

  if (res->err.dberr_code != DB_OK) {
    log_write(LS_SYSTEM, L_ERROR, 0, "filehost: could not record a file: %s",
              res->err.dberr_message);

    /* The object is there and nothing points at it.  Take it back out:
     * an upload that was not recorded did not happen, and leaving the
     * bytes would leave them for ever -- the sweep only knows about rows. */
    remove(put->fp_path);

    if (put->fp_done)
      (put->fp_done)(0, put->fp_user);
  } else if (put->fp_done) {
    (put->fp_done)(1, put->fp_user);
  }

  MyFree(put);
}

void file_store_put(const struct FileRow* row, const struct HttpRequest* req,
                    int retention, FileDoneFn done, void* user)
{
  struct DbParam p[7];
  struct DbParam* params[8];
  struct DbQuery query;
  struct FilePut* put;
  char size_text[32];
  char keep_text[16];
  unsigned int i;

  if (!file_store_ready() || !file_store_mkdir(row->fr_id)) {
    if (done)
      (done)(0, user);
    return;
  }

  put = (struct FilePut*) MyCalloc(1, sizeof(*put));
  put->fp_done = done;
  put->fp_user = user;

  if (!file_store_path(put->fp_path, sizeof(put->fp_path), row->fr_id)) {
    MyFree(put);
    if (done)
      (done)(0, user);
    return;
  }

  /* The move before the row.  For anything big this is a link and an
   * unlink, and the spool directory is meant to be on the same filesystem
   * for exactly that reason: a copy would read and write the whole file
   * in the main thread, which is the one thing that may not happen here. */
  if (!http_request_save(req, put->fp_path)) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "filehost: could not move an upload into %s (is HTTP_SPOOL_DIR "
              "on the same filesystem as FILEHOST_DIR?)", put->fp_path);
    MyFree(put);
    if (done)
      (done)(0, user);
    return;
  }

  ircd_snprintf(0, size_text, sizeof(size_text), "%lld", row->fr_size);
  ircd_snprintf(0, keep_text, sizeof(keep_text), "%d", retention);

  memset(p, 0, sizeof(p));
  p[0].type = DB_TYPE_TEXT;   p[0].value = row->fr_id;
  p[1].type = DB_TYPE_TEXT;   p[1].value = row->fr_account;
  p[2].type = DB_TYPE_TEXT;   p[2].value = row->fr_target;
  p[3].type = DB_TYPE_TEXT;   p[3].value = row->fr_name;
  p[4].type = DB_TYPE_TEXT;   p[4].value = row->fr_type;
  p[5].type = DB_TYPE_BIGINT; p[5].value = size_text;
  p[6].type = DB_TYPE_INT;    p[6].value = keep_text;

  for (i = 0; i < 7; i++)
    params[i] = &p[i];
  params[7] = NULL;

  query.sql = file_sql_insert;
  query.params = params;

  if (db_exec(file_mod, &query, file_store_put_done, put) != DB_OK) {
    remove(put->fp_path);
    MyFree(put);
    if (done)
      (done)(0, user);
  }
}

/* ------------------------------------------------------------------- *
 * Reading                                                             *
 * ------------------------------------------------------------------- */

/** Fill in a row from the answer. */
static void file_row_from(struct FileRow* row, struct json_t* data,
                          unsigned int n)
{
  const char* text;

  memset(row, 0, sizeof(*row));

  if ((text = db_row_str(data, n, "id")))
    ircd_strncpy(row->fr_id, text, FILE_ID_LEN);
  if ((text = db_row_str(data, n, "account")))
    ircd_strncpy(row->fr_account, text, NICKLEN);
  if ((text = db_row_str(data, n, "target")))
    ircd_strncpy(row->fr_target, text, CHANNELLEN);
  if ((text = db_row_str(data, n, "name")))
    ircd_strncpy(row->fr_name, text, FILE_NAME_MAX);
  if ((text = db_row_str(data, n, "content_type")))
    ircd_strncpy(row->fr_type, text, HTTP_HVALUE_MAX);

  row->fr_size = db_row_int(data, n, "size");
}

/** One lookup in flight. */
struct FileGet {
  FileRowFn fg_done;
  void*     fg_user;
};

static void file_store_get_done(const struct DbResult* res, void* user)
{
  struct FileGet* get = (struct FileGet*) user;
  struct FileRow row;

  if (res->err.dberr_code != DB_OK || db_rows(res->data) == 0) {
    (get->fg_done)(0, get->fg_user);
  } else {
    file_row_from(&row, res->data, 0);
    (get->fg_done)(&row, get->fg_user);
  }

  MyFree(get);
}

void file_store_get(const char* id, FileRowFn done, void* user)
{
  struct DbParam p;
  struct DbParam* params[2];
  struct DbQuery query;
  struct FileGet* get;

  if (!file_store_ready() || !file_id_valid(id)) {
    (done)(0, user);
    return;
  }

  get = (struct FileGet*) MyCalloc(1, sizeof(*get));
  get->fg_done = done;
  get->fg_user = user;

  memset(&p, 0, sizeof(p));
  p.type = DB_TYPE_TEXT;
  p.value = id;

  params[0] = &p;
  params[1] = NULL;

  query.sql = file_sql_get;
  query.params = params;

  if (db_query(file_mod, &query, file_store_get_done, get) != DB_OK) {
    MyFree(get);
    (done)(0, user);
  }
}

/** One listing in flight. */
struct FileList {
  FileListFn fl_done;
  void*      fl_user;
};

static void file_store_list_done(const struct DbResult* res, void* user)
{
  struct FileList* list = (struct FileList*) user;
  struct FileRow rows[32];
  unsigned int count = 0;

  if (res->err.dberr_code == DB_OK) {
    unsigned int have = db_rows(res->data);

    while (count < have && count < 32) {
      file_row_from(&rows[count], res->data, count);
      count++;
    }

    (list->fl_done)(rows, count, list->fl_user);
  } else {
    (list->fl_done)(0, 0, list->fl_user);
  }

  MyFree(list);
}

void file_store_list(const char* account, unsigned int limit,
                     FileListFn done, void* user)
{
  struct DbParam p[2];
  struct DbParam* params[3];
  struct DbQuery query;
  struct FileList* list;
  char limit_text[16];

  if (!file_store_ready() || EmptyString(account)) {
    (done)(0, 0, user);
    return;
  }

  if (limit == 0 || limit > 32)
    limit = 32;

  list = (struct FileList*) MyCalloc(1, sizeof(*list));
  list->fl_done = done;
  list->fl_user = user;

  ircd_snprintf(0, limit_text, sizeof(limit_text), "%u", limit);

  memset(p, 0, sizeof(p));
  p[0].type = DB_TYPE_TEXT; p[0].value = account;
  p[1].type = DB_TYPE_INT;  p[1].value = limit_text;

  params[0] = &p[0];
  params[1] = &p[1];
  params[2] = NULL;

  query.sql = file_sql_list;
  query.params = params;

  if (db_query(file_mod, &query, file_store_list_done, list) != DB_OK) {
    MyFree(list);
    (done)(0, 0, user);
  }
}

/** One usage question in flight. */
struct FileUsage {
  void (*fu_done)(long long bytes, int ok, void* user);
  void* fu_user;
};

static void file_store_usage_done(const struct DbResult* res, void* user)
{
  struct FileUsage* usage = (struct FileUsage*) user;

  if (res->err.dberr_code != DB_OK || db_rows(res->data) == 0)
    (usage->fu_done)(0, 0, usage->fu_user);
  else
    (usage->fu_done)(db_row_int(res->data, 0, "bytes"), 1, usage->fu_user);

  MyFree(usage);
}

void file_store_usage(const char* account,
                      void (*done)(long long bytes, int ok, void* user),
                      void* user)
{
  struct DbParam p;
  struct DbParam* params[2];
  struct DbQuery query;
  struct FileUsage* usage;

  if (!file_store_ready() || EmptyString(account)) {
    (done)(0, 0, user);
    return;
  }

  usage = (struct FileUsage*) MyCalloc(1, sizeof(*usage));
  usage->fu_done = done;
  usage->fu_user = user;

  memset(&p, 0, sizeof(p));
  p.type = DB_TYPE_TEXT;
  p.value = account;

  params[0] = &p;
  params[1] = NULL;

  query.sql = file_sql_usage;
  query.params = params;

  if (db_query(file_mod, &query, file_store_usage_done, usage) != DB_OK) {
    MyFree(usage);
    (done)(0, 0, user);
  }
}

/* ------------------------------------------------------------------- *
 * Deleting                                                            *
 * ------------------------------------------------------------------- */

/** Take the objects out for every row a DELETE returned. */
static void file_store_unlink_rows(const struct DbResult* res)
{
  char path[HTTP_PATH_MAX + 1];
  unsigned int i;
  unsigned int rows = db_rows(res->data);

  for (i = 0; i < rows; i++) {
    const char* id = db_row_str(res->data, i, "id");

    if (id && file_store_path(path, sizeof(path), id))
      remove(path);
  }
}

/** One delete in flight. */
struct FileDel {
  FileDoneFn fd_done;
  void*      fd_user;
};

static void file_store_delete_done(const struct DbResult* res, void* user)
{
  struct FileDel* del = (struct FileDel*) user;
  int ok = 0;

  if (res->err.dberr_code == DB_OK && db_rows(res->data) > 0) {
    /* The row went, so the object goes.  In that order: a row with no
     * object is a download that 404s, an object with no row is bytes
     * nobody can reach and nothing will ever collect. */
    file_store_unlink_rows(res);
    ok = 1;
  }

  if (del->fd_done)
    (del->fd_done)(ok, del->fd_user);

  MyFree(del);
}

void file_store_delete(const char* id, const char* account,
                       FileDoneFn done, void* user)
{
  struct DbParam p[2];
  struct DbParam* params[3];
  struct DbQuery query;
  struct FileDel* del;

  if (!file_store_ready() || !file_id_valid(id)) {
    if (done)
      (done)(0, user);
    return;
  }

  del = (struct FileDel*) MyCalloc(1, sizeof(*del));
  del->fd_done = done;
  del->fd_user = user;

  memset(p, 0, sizeof(p));
  p[0].type = DB_TYPE_TEXT; p[0].value = id;
  p[1].type = DB_TYPE_TEXT; p[1].value = account ? account : "";

  params[0] = &p[0];
  params[1] = &p[1];
  params[2] = NULL;

  query.sql = file_sql_delete;
  query.params = params;

  if (db_exec(file_mod, &query, file_store_delete_done, del) != DB_OK) {
    MyFree(del);
    if (done)
      (done)(0, user);
  }
}

/** Take the answer to the sweep. */
static void file_store_swept(const struct DbResult* res, void* user)
{
  (void) user;

  if (res->err.dberr_code != DB_OK)
    return;

  if (db_rows(res->data) == 0)
    return;

  file_store_unlink_rows(res);

  log_write(LS_SYSTEM, L_INFO, 0, "filehost: %u expired file%s removed",
            db_rows(res->data), db_rows(res->data) == 1 ? "" : "s");
}

void file_store_sweep(void)
{
  struct DbQuery query;

  if (!file_store_ready())
    return;

  query.sql = file_sql_expired;
  query.params = NULL;

  db_exec(file_mod, &query, file_store_swept, NULL);
}
