/*
 * IRC - Internet Relay Chat, modules/services/filehost/filehost.c
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
 * @brief Files: an upload, a link, and a sweep that ends it.
 *
 * Phase 5 of proposal 006 (section 7.4), the half that was left for when
 * the HTTP side existed.  It does now, so this is a module on top of it:
 * two routes, one command, one table and a directory.
 *
 * **Nothing large crosses the event loop, in either direction.**  Going
 * up, the worker writes the body to the spool and the handler is given a
 * path to move; coming down, the handler names a file and the worker
 * streams it.  That is the rule the HTTP layer was built around and the
 * reason a file host can live in a single-threaded server at all.
 *
 * **Authorisation is a signed ticket, not a session.**  The server says,
 * in a statement it signs, "this account asked to upload one file, with
 * this identifier, for this target, in the next fifteen minutes"; the
 * upload presents it and nothing else has to be believed.  The key is
 * the network's (ircd_token.h), so the ticket works on the server that
 * issued it and on no other network -- and nothing is stored, so there is
 * no table of outstanding tickets to keep.
 *
 * What it does **not** do, deliberately: there is no `draft/filehost`
 * message tag yet.  A tag is read by clients, and the clients are the
 * next phase; until they exist the URL in the text is what every client
 * that has ever been written understands, and inventing the tag now would
 * be inventing one nobody reads.
 */
#include "config.h"

#include "filehost.h"

#include "hooks.h"
#include "ircd_alloc.h"
#include "ircd.h"
#include "ircd_events.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

/** This module's handle. */
struct ModuleHandle* file_mod;

/** How often expired files are collected. */
#define FILE_SWEEP_SECONDS 300

/** The sweep timer, and whether it is armed and initialised.
 *
 * timer_init() once and timer_add() thereafter: the flag it clears is the
 * only thing telling timer_add() it is re-arming a timer timer_run() is
 * still holding, and this one re-arms from inside its own expiry.
 */
static struct Timer file_timer;
static int file_timer_ready;
static int file_timer_armed;

/** Non-zero once the routes are claimed. */
static int file_started;

static void file_sweep_timer(struct Event* ev);

/** Arm the sweep. */
static void file_arm(void)
{
  if (file_timer_armed)
    return;

  if (!file_timer_ready) {
    timer_init(&file_timer);
    file_timer_ready = 1;
  }

  timer_add(&file_timer, file_sweep_timer, 0, TT_RELATIVE,
            FILE_SWEEP_SECONDS);
  file_timer_armed = 1;
}

static void file_sweep_timer(struct Event* ev)
{
  if (ev_type(ev) != ET_EXPIRE) {
    if (ev_type(ev) == ET_DESTROY)
      file_timer_armed = 0;
    return;
  }

  file_timer_armed = 0;

  file_store_sweep();
  file_arm();
}

int file_url(char* buf, size_t len, const char* id)
{
  const char* base = feature_str(FEAT_FILEHOST_BASE_URL);
  size_t blen;
  unsigned int wrote;

  if (EmptyString(base) || !file_id_valid(id))
    return 0;

  /* One slash between the two, whichever way the operator wrote it. */
  blen = strlen(base);
  wrote = ircd_snprintf(0, buf, len, "%s%sf/%s", base,
                        base[blen - 1] == '/' ? "" : "/", id);

  return wrote > 0 && wrote < len;
}

/** Say what is missing, once, in words an operator can act on.
 * @return Non-zero if everything needed is there.
 */
static int file_check_configuration(void)
{
  int ok = 1;

  if (EmptyString(file_store_dir())) {
    log_write(LS_SYSTEM, L_INFO, 0,
              "filehost: FILEHOST_DIR is not set, so no files are hosted");
    ok = 0;
  }

  if (!http_available()) {
    log_write(LS_SYSTEM, L_INFO, 0,
              "filehost: HTTP_PORT is 0, so there is nothing to serve files "
              "over");
    ok = 0;
  }

  if (!http_upload_available()) {
    log_write(LS_SYSTEM, L_INFO, 0,
              "filehost: HTTP_UPLOAD_MAX or HTTP_SPOOL_DIR is not set, so "
              "nothing can be uploaded");
    ok = 0;
  }

  if (EmptyString(feature_str(FEAT_FILEHOST_BASE_URL))) {
    log_write(LS_SYSTEM, L_INFO, 0,
              "filehost: FILEHOST_BASE_URL is not set, so there is no link "
              "to give anybody");
    ok = 0;
  }

  return ok;
}

/** Claim the routes once the configuration has been read.
 *
 * From HOOK_CONFIG_LOADED and never from mi_init: the features are not
 * final in the middle of a parse, and http_available() asked there would
 * answer about the configuration before this one.
 */
static enum HookResult file_configured(struct HookContext* ctx, void* user)
{
  (void) ctx;
  (void) user;

  if (file_started)
    return HOOK_CONTINUE;

  if (!file_check_configuration())
    return HOOK_CONTINUE;

  /* The directory has to exist and be ours alone: what goes in it is
   * other people's files. */
  if (mkdir(file_store_dir(), 0700) != 0 && errno != EEXIST) {
    log_write(LS_SYSTEM, L_ERROR, 0,
              "filehost: cannot use %s: %s", file_store_dir(),
              strerror(errno));
    return HOOK_CONTINUE;
  }

  if (!file_http_start()) {
    log_write(LS_SYSTEM, L_ERROR, 0, "filehost: could not claim its routes");
    return HOOK_CONTINUE;
  }

  file_started = 1;
  file_arm();

  log_write(LS_SYSTEM, L_INFO, 0,
            "filehost: serving %s, keeping files for %d day%s",
            file_store_dir(), feature_int(FEAT_FILEHOST_RETENTION),
            feature_int(FEAT_FILEHOST_RETENTION) == 1 ? "" : "s");

  return HOOK_CONTINUE;
}

/** Set up.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int filehost_init(struct ModuleHandle* mod)
{
  file_mod = mod;

  /* The command is registered whatever the configuration says: a client
   * that types FILE on a server with no store is told so, which is more
   * use than "unknown command". */
  if (!file_cmd_start()) {
    file_mod = NULL;
    return -1;
  }

  if (!module_add_hook(mod, HOOK_CONFIG_LOADED, file_configured,
                       HOOK_PRIORITY_DEFAULT, NULL)) {
    file_mod = NULL;
    return -1;
  }

  return 0;
}

/** Stop the sweep.  The routes and the command the loader reverts.
 * @param[in] mod Handle for this module.
 */
static void filehost_fini(struct ModuleHandle* mod)
{
  (void) mod;

  if (file_timer_armed) {
    timer_del(&file_timer);
    file_timer_armed = 0;
  }

  file_started = 0;
  file_mod = NULL;
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "filehost",
  "1.0.0",
  "ircu developers",
  "Files: an upload over HTTP, a link in a message, and a sweep",
  filehost_init,
  filehost_fini,
  NULL
};
