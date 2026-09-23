/*
 * IRC - Internet Relay Chat, modules/services/filehost/filehost.h
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
 * @brief What the pieces of the file host share.  Private to the module.
 */
#ifndef INCLUDED_filehost_h
#define INCLUDED_filehost_h

#include "client.h"
#include "db.h"
#include "http.h"
#include "module.h"

/** Characters in an identifier; what appears in a URL. */
#define FILE_ID_LEN 16

/** Longest name a file may be remembered under. */
#define FILE_NAME_MAX 96

/** How long an upload ticket is good for, in seconds.
 *
 * Long enough to paste a command into a shell and watch it run, short
 * enough that a ticket found in a scrollback tomorrow is worth nothing.
 * Not a feature: an operator has no reason to want a different number and
 * every knob is a thing that can be set wrong.
 */
#define FILE_TICKET_WINDOW 900

/** What an upload ticket is minted for; see ircd_token.h. */
#define FILE_TICKET_LABEL "ircu-file-upload-v1"

/** This module's handle, for db_query() and http_add_route(). */
extern struct ModuleHandle* file_mod;

/*
 * file_store.c -- every statement this module sends, and the objects on
 * disk.  One file, for the reason hist_store.c is one file: retention,
 * deletion and quota all have to happen in one place or they happen in
 * three and disagree.
 */

/** One row, as much of it as anything here needs. */
struct FileRow {
  char   fr_id[FILE_ID_LEN + 1];
  char   fr_account[ACCOUNTLEN + 1];
  char   fr_target[CHANNELLEN + 1];
  char   fr_name[FILE_NAME_MAX + 1];
  char   fr_type[HTTP_HVALUE_MAX + 1];
  long long fr_size;
};

/** Called when a row has been looked up.
 * @param[in] row The row, or NULL if there is none.
 * @param[in] user The caller's pointer.
 */
typedef void (*FileRowFn)(const struct FileRow* row, void* user);

/** Called when a write has finished.
 * @param[in] ok Non-zero if it worked.
 * @param[in] user The caller's pointer.
 */
typedef void (*FileDoneFn)(int ok, void* user);

/** Non-zero if this server has somewhere to put files and a database. */
extern int file_store_ready(void);

/** Where the objects live; "" when not configured. */
extern const char* file_store_dir(void);

/** Build the path an identifier's object lives at.
 * @param[out] buf Where to write it.
 * @param[in] len Size of \a buf.
 * @param[in] id The identifier, already checked.
 * @return Non-zero on success.
 */
extern int file_store_path(char* buf, size_t len, const char* id);

/** Move a request's body into the store and write its row.
 *
 * The move first: a row pointing at an object that is not there is worse
 * than an object nobody has a row for, which the sweep will collect.
 * The request and not a path, because where the bytes are is the core's
 * business -- http_request_save() is what knows, and a body small enough
 * to have come through memory is as much an upload as one that did not.
 *
 * @param[in] row What to record.
 * @param[in] req The request whose body to take.
 * @param[in] retention Days to keep it, or 0 for ever.
 * @param[in] done Called with the answer.
 * @param[in] user Passed to \a done.
 */
extern void file_store_put(const struct FileRow* row,
                           const struct HttpRequest* req,
                           int retention, FileDoneFn done, void* user);

/** Look one up by identifier. */
extern void file_store_get(const char* id, FileRowFn done, void* user);

/** Delete one, object and row, if \a account may.
 * @param[in] id What to delete.
 * @param[in] account Who is asking; "" for an administrator.
 */
extern void file_store_delete(const char* id, const char* account,
                              FileDoneFn done, void* user);

/** Called with a listing.
 * @param[in] rows The rows, or NULL on failure.
 * @param[in] count How many.
 * @param[in] user The caller's pointer.
 */
typedef void (*FileListFn)(const struct FileRow* rows, unsigned int count,
                           void* user);

/** List what an account holds, newest first. */
extern void file_store_list(const char* account, unsigned int limit,
                            FileListFn done, void* user);

/** Add up what an account is using, for the quota. */
extern void file_store_usage(const char* account,
                             void (*done)(long long bytes, int ok, void* user),
                             void* user);

/** Delete everything past its keeping date.  Called by the timer. */
extern void file_store_sweep(void);

/** Non-zero if \a id is the shape this module hands out.
 *
 * Checked before it reaches a path: an identifier is sixteen characters
 * of base 62 and nothing else, so nothing a client sends can name a file
 * outside the store.
 */
extern int file_id_valid(const char* id);

/** Make a new identifier. */
extern void file_id_make(char* buf, size_t len);

/*
 * file_http.c -- the two routes.
 */

/** Claim them.  Called once the configuration has been read. */
extern int file_http_start(void);

/*
 * file_cmd.c -- the /FILE command.
 */

/** Register it. */
extern int file_cmd_start(void);

/** Build the URL an identifier is served at.
 * @return Non-zero on success; zero when no base URL is configured.
 */
extern int file_url(char* buf, size_t len, const char* id);

#endif /* INCLUDED_filehost_h */
