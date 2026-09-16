/*
 * IRC - Internet Relay Chat, modules/services/history/history.c
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
 * @brief What was said, kept.
 *
 * Phase 2 of proposal 006.  The module attaches to
 * #HOOK_MESSAGE_DELIVERED, which fires wherever this server relays a
 * message rather than only where one started, and writes what it hears
 * through include/db.h -- so the write leaves the main thread at once and
 * nothing here ever waits for a database.
 *
 * Three things shape it.
 *
 *   - @b Every @b server @b writes @b what @b it @b delivers.  There is no
 *     designated archivist, because a designated archivist is a server
 *     whose split takes the record with it.  The rows are identical --
 *     both the identifier and the timestamp travel with the message -- so
 *     the second writer's insert does nothing.  That needs two features
 *     on: @c NETWORK_FEATURES, which carries @c msgid over P10, and
 *     @c NETWORK_TIME, which carries @c time.  Without them each server
 *     invents its own name and its own clock reading for the same message
 *     and the network ends up with one row per server.
 *
 *   - @b The @b store @b is @b behind @b one @b interface.  Every
 *     statement is in hist_store.c (see hist_store.h for why), so
 *     retention, purging and deletion by account have one place to happen
 *     rather than a DELETE in whichever file first needed one.
 *
 *   - @b Deletion @b is @b designed @b in, @b not @b added @b later.  The
 *     schema is partitioned by month so that expiry is a DROP TABLE, and
 *     the migration ships history_forget() from the first version.  What
 *     is stored here is what people said, and in most places how long it
 *     may be kept is not a technical question.
 *
 * It owns its schema, which is why it is a directory module: migrations/
 * is compiled into the .so and an operator applies it with
 * /MODULE MIGRATION APPLY history.
 */
#include "config.h"

#include "capab.h"
#include "db.h"
#include "history.h"
#include "hooks.h"
#include "ircd.h"
#include "ircd_events.h"
#include "ircd_features.h"
#include "ircd_i18n.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "msg.h"
#include "module.h"

#include <string.h>

/** This module's handle. */
struct ModuleHandle* hist_mod;

/** This module's translation catalog. */
struct I18nDomain* hist_i18n;

/** The position the server handed out for draft/chathistory.
 *
 * Kept rather than recomputed: a capability's position is assigned, does
 * not follow from its name, and nothing on a server link has to agree on
 * it.  -1 means it is not registered.
 */
static int hist_cap = -1;

/** How many months ahead partitions are created. */
#define HIST_MONTHS_AHEAD 1

/** How often maintenance runs, in seconds. */
#define HIST_MAINTENANCE_EVERY (6 * 60 * 60)

/** How often it runs while the schema is not there yet.
 *
 * A module is loaded from ircd.conf and its migrations are applied by an
 * operator afterwards, so the first maintenance pass usually finds
 * nothing to talk to.  Six hours later is too late: everything said in
 * between lands in the default partition, and although the migration in
 * v3 rescues it when the month's partition is finally created, the point
 * is not to need rescuing.
 */
#define HIST_MAINTENANCE_WAITING (5 * 60)

/** The maintenance timer. */
static struct Timer hist_timer;
/** Whether #hist_timer has been initialised.
 *
 * timer_init() zeroes the generator's flags, GEN_MARKED among them, and
 * that flag is the only thing telling timer_add() it is re-arming a timer
 * timer_run() still holds.  So it runs once, here, and every re-arm from
 * inside the callback is a bare timer_add().  See CLAUDE.md.
 */
static int hist_timer_ready;

/** Put the largest answer this server will give in the capability's value.
 *
 * So a client knows the ceiling before it asks rather than by having its
 * limit quietly cut down.  Re-run on rehash, because HISTORY_MAX_LIMIT is
 * a feature and a feature can change while the server runs.
 */
static void hist_advertise(void)
{
  char value[16];
  int max = feature_int(FEAT_HISTORY_MAX_LIMIT);

  if (hist_cap < 0)
    return;

  ircd_snprintf(0, value, sizeof(value), "%d", max > 0 ? max : 1);
  cap_set_value(hist_cap, value);
}

/** Create partitions and drop what has expired. */
static void hist_maintenance(void)
{
  hist_store_ensure_partitions(HIST_MONTHS_AHEAD);
  hist_store_purge(feature_int(FEAT_HISTORY_RETENTION));
}

/** The maintenance timer went off. */
static void hist_timer_expired(struct Event* ev)
{
  if (ev_type(ev) != ET_EXPIRE)
    return;

  hist_maintenance();

  timer_add(&hist_timer, hist_timer_expired, 0, TT_RELATIVE,
            hist_store_ready() ? HIST_MAINTENANCE_EVERY
                               : HIST_MAINTENANCE_WAITING);
}

/** Say what an operator needs to hear before the first user does. */
static void hist_check_config(void)
{
  if (!db_available()) {
    log_write(LS_SYSTEM, L_WARNING, 0,
              "history: loaded with no database driver; nothing will be "
              "stored until one is loaded and Database{} is set");
    return;
  }

  /* Both of these are what makes one message one row.  A network running
   * without them gets a row per server for every message, which is not a
   * failure anybody notices until they read the history back.
   */
  if (!feature_bool(FEAT_NETWORK_FEATURES))
    log_write(LS_SYSTEM, L_WARNING, 0,
              "history: NETWORK_FEATURES is off, so msgid does not cross "
              "P10; every server will store its own copy of the same "
              "message");

  if (!feature_bool(FEAT_NETWORK_TIME))
    log_write(LS_SYSTEM, L_WARNING, 0,
              "history: NETWORK_TIME is off, so the time a message was "
              "sent does not cross P10; every server will store its own "
              "copy of the same message");
}

/** The configuration has been read in full.
 *
 * Fires once at start-up with the server ready, and again after every
 * rehash, which is the only moment at which what this module needs from
 * the configuration is all there.
 */
static enum HookResult hist_configured(struct HookContext* ctx, void* user)
{
  (void) ctx;
  (void) user;

  hist_advertise();
  hist_check_config();

  return HOOK_CONTINUE;
}

/** What the command runs as.
 *
 * A client only: a server never asks another for history, because every
 * server reads the same store, and an unregistered client has no account
 * and no channels, so there is nothing it could be shown.
 */
static MessageHandler handlers[] = {
  0,                    /* unregistered */
  hist_m_chathistory,   /* client */
  0,                    /* server */
  hist_m_chathistory,   /* oper */
  0                     /* service */
};

/** And /HISTORY.
 *
 * The oper slot only.  The client slot answers too, so that somebody who
 * is not an operator is told they have no privileges rather than that the
 * command does not exist -- which would be a difference they could use to
 * find out whether this server keeps history at all.
 */
static MessageHandler admin_handlers[] = {
  0,                /* unregistered */
  hist_m_history,   /* client */
  0,                /* server */
  hist_m_history,   /* oper */
  0                 /* service */
};

/** Attach to the hook and start maintaining the schema.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int history_init(struct ModuleHandle* mod)
{
  hist_mod = mod;

  hist_i18n = module_i18n(mod);

  if (!module_add_hook(mod, HOOK_MESSAGE_DELIVERED, hist_capture,
                       HOOK_PRIORITY_DEFAULT, 0)) {
    hist_mod = NULL;
    return -1;
  }

  if (!module_add_command(mod, MSG_CHATHISTORY, TOK_CHATHISTORY, MAXPARA,
                          0, handlers)) {
    hist_mod = NULL;
    return -1;
  }

  if (!module_add_command(mod, MSG_HISTORY, TOK_HISTORY, MAXPARA,
                          0, admin_handlers)) {
    hist_mod = NULL;
    return -1;
  }

  /* The capability last, and only if the command is really there: its
   * value is a promise about an answer, and advertising one the server
   * cannot give is worse than not advertising at all.
   */
  if (module_add_cap(mod, HIST_CAP_NAME, 0, &hist_cap))
    hist_advertise();

  /* Everything that depends on the configuration waits for
   * HOOK_CONFIG_LOADED.  mi_init runs in the middle of the parse: a
   * Database{} block or a Features{} block written after this module's own
   * Module{} line has not been read yet, so a check made here would warn
   * about a database that is configured and advertise a limit that is
   * about to change.
   */
  if (!module_add_hook(mod, HOOK_CONFIG_LOADED, hist_configured,
                       HOOK_PRIORITY_DEFAULT, 0)) {
    hist_mod = NULL;
    return -1;
  }

  if (!hist_timer_ready) {
    timer_init(&hist_timer);
    hist_timer_ready = 1;
  }
  timer_add(&hist_timer, hist_timer_expired, 0, TT_RELATIVE, 5);

  return 0;
}

/** Stop.
 * @param[in] mod Handle for this module.
 */
static void history_fini(struct ModuleHandle* mod)
{
  (void) mod;

  hist_cap = -1;
  hist_i18n = NULL;

  /* An export writes to a file this module holds open, and the pages it
   * is waiting for will be dropped with the rest of this module's
   * queries.  Closing it here leaves a partial file and says so, which is
   * better than a file that looks finished. */
  hist_admin_shutdown();

  if (hist_timer_ready) {
    timer_del(&hist_timer);
    hist_timer_ready = 0;
  }

  /* The hook goes with the module, and db.c drops the callbacks of a
   * module being unloaded, so an insert in flight simply never comes
   * back.  Nothing is waiting on one.
   */
  hist_mod = NULL;
}

/** What the loader reads. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "history",
  "1.0",
  "ircu2",
  "keeps what was said, on PostgreSQL",
  history_init,
  history_fini,
  0
};
