/*
 * IRC - Internet Relay Chat, modules/services/irc_services/irc_services.c
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
 * along with this project; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
/** @file
 * @brief Network services embedded in the server.
 *
 * Every Service{} block in ircd.conf names a service bot -- a NickServ,
 * a ChanServ -- and this module introduces one for each, as a client of
 * this server with user modes +S +k +B (see include/bot.h).  A user
 * anywhere on the network who sends it a PRIVMSG or NOTICE, or says
 * "!command" on a channel it is on, reaches the service's command table
 * through HOOK_MESSAGE_RECEIVED; the services themselves are the
 * svc_*.c files, and services.h says how to add one.
 *
 * The bots are the configuration's, not an operator's.  They are
 * created when the configuration has been read in full -- on
 * HOOK_CONFIG_LOADED, which is after start-up and after every rehash,
 * or at once if an operator loaded the module by hand -- and a rehash
 * that adds, removes or changes a block is followed by the bots
 * changing to match.  /BOT will not rename or destroy them.
 *
 * The network may still take one away: a KILL from another server, a
 * nick collision it loses.  HOOK_CLIENT_EXITING notices, and a timer
 * brings the bot back, killing whoever holds its nick unless that is
 * another service; retries back off from SVC_RETRY_MIN to SVC_RETRY_MAX
 * seconds so that two servers configured with the same service do not
 * fight at full speed.
 */
#include "config.h"

#include "services.h"

#include "bot.h"
#include "channel.h"
#include "client.h"
#include "hash.h"
#include "hooks.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_events.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "list.h"
#include "module.h"
#include "msg.h"
#include "numeric.h"
#include "s_conf.h"
#include "s_misc.h"
#include "send.h"
#include "struct.h"

#include <string.h>

/** First retry after a bot is lost or cannot be introduced, in seconds. */
#define SVC_RETRY_MIN 5
/** Retries back off by doubling up to this many seconds. */
#define SVC_RETRY_MAX 300

/** The service types this module knows, ended by NULL. */
static const struct ServiceType* service_types[] = {
  &svc_type_nickserv,
  &svc_type_chanserv,
  NULL
};

/** Our handle; the owner of every bot we create. */
static struct ModuleHandle* svc_mod;

/** Services, in configuration order. */
static struct Service* svc_list;

/** Fires when a missing bot is due to be introduced again. */
static struct Timer svc_timer;

/** Set while mi_fini runs, so a bot leaving is not brought back. */
static int svc_unloading;

const char* svc_module_version(void)
{
  return module_version(svc_mod);
}

/*
 * Lookups.
 */

static const struct ServiceType* find_type(const char* name)
{
  const struct ServiceType** t;

  for (t = service_types; *t; t++)
    if (0 == ircd_strcmp((*t)->st_name, name))
      return *t;

  return NULL;
}

static struct Service* svc_find_by_client(const struct Client* cptr)
{
  struct Service* sv;

  if (!cptr)
    return NULL;

  for (sv = svc_list; sv; sv = sv->sv_next)
    if (sv->sv_client == cptr)
      return sv;

  return NULL;
}

static struct Service* svc_find_by_name(const char* nick)
{
  struct Service* sv;

  for (sv = svc_list; sv; sv = sv->sv_next)
    if (0 == ircd_strcmp(sv->sv_name, nick))
      return sv;

  return NULL;
}

/*
 * Records.
 */

/** Copy a string the block may not have given. */
static char* dup_or_null(const char* s)
{
  char* copy = NULL;

  if (s && *s)
    DupString(copy, s);

  return copy;
}

/** Compare two strings either of which may be NULL. */
static int str_same(const char* a, const char* b)
{
  if (!a || !*a)
    return !b || !*b;
  return b && 0 == strcmp(a, b);
}

/** Build a record from a block, linked at the end of the list. */
static struct Service* svc_new(const struct ServiceConf* conf,
                               const struct ServiceType* type)
{
  struct Service* sv;
  struct Service** pp;
  struct SLink* lp;
  struct SLink** tail;

  sv = (struct Service*) MyCalloc(1, sizeof(*sv));
  sv->sv_type = type;
  ircd_strncpy(sv->sv_name, conf->name, NICKLEN);
  sv->sv_username = dup_or_null(conf->username);
  sv->sv_host = dup_or_null(conf->host);
  sv->sv_description = dup_or_null(conf->description);

  tail = &sv->sv_channels;
  for (lp = conf->channels; lp; lp = lp->next) {
    struct SLink* copy = make_link();

    DupString(copy->value.cp, lp->value.cp);
    copy->next = NULL;
    *tail = copy;
    tail = &copy->next;
  }

  for (pp = &svc_list; *pp; pp = &(*pp)->sv_next)
    ;
  *pp = sv;

  return sv;
}

/** Does a record still describe what its block says? */
static int svc_matches(const struct Service* sv,
                       const struct ServiceConf* conf,
                       const struct ServiceType* type)
{
  const struct SLink* a;
  const struct SLink* b;

  if (sv->sv_type != type
      || !str_same(sv->sv_username, conf->username)
      || !str_same(sv->sv_host, conf->host)
      || !str_same(sv->sv_description, conf->description))
    return 0;

  for (a = sv->sv_channels, b = conf->channels; a && b;
       a = a->next, b = b->next)
    if (0 != ircd_strcmp(a->value.cp, b->value.cp))
      return 0;

  return !a && !b;
}

/** Take a record out of the list and free it, quitting its bot first.
 * @param[in] sv The record.
 * @param[in] reason Quit reason for the bot, if it exists.
 */
static void svc_remove(struct Service* sv, const char* reason)
{
  struct Service** pp;
  struct SLink* lp;
  struct SLink* next;

  /* Tell the exiting hook this one is not to be brought back. */
  sv->sv_expected = 1;
  if (sv->sv_client)
    bot_destroy(sv->sv_client, &me, reason);
  sv->sv_client = NULL;

  for (pp = &svc_list; *pp; pp = &(*pp)->sv_next)
    if (*pp == sv) {
      *pp = sv->sv_next;
      break;
    }

  for (lp = sv->sv_channels; lp; lp = next) {
    next = lp->next;
    MyFree(lp->value.cp);
    free_link(lp);
  }
  MyFree(sv->sv_username);
  MyFree(sv->sv_host);
  MyFree(sv->sv_description);
  MyFree(sv);
}

/*
 * Introducing, and trying again.
 */

static void svc_timer_cb(struct Event* ev);

/** Arm the timer for the earliest retry that is due, if any. */
static void svc_schedule(void)
{
  struct Service* sv;
  time_t next = 0;

  for (sv = svc_list; sv; sv = sv->sv_next) {
    time_t at;

    if (sv->sv_client || sv->sv_broken)
      continue;
    /* Never tried counts as due now. */
    at = sv->sv_retry_at ? sv->sv_retry_at : CurrentTime;
    if (!next || at < next)
      next = at;
  }

  if (!next)
    return;
  if (next <= CurrentTime)
    next = CurrentTime + 1;

  /* Re-arming a timer that is queued would queue it twice; changing it
   * is the way, and works from inside its own callback as well.
   */
  if (t_active(&svc_timer))
    timer_chg(&svc_timer, TT_ABSOLUTE, next);
  else
    timer_add(&svc_timer, svc_timer_cb, NULL, TT_ABSOLUTE, next);
}

/** Note that a bot could not be introduced, and when to try again.
 * @param[in] sv The service.
 * @param[in] why What stood in the way, for the log.
 * @return Zero, for the caller to return.
 */
static int svc_retry_later(struct Service* sv, const char* why)
{
  sv->sv_backoff = sv->sv_backoff ? sv->sv_backoff * 2 : SVC_RETRY_MIN;
  if (sv->sv_backoff > SVC_RETRY_MAX)
    sv->sv_backoff = SVC_RETRY_MAX;
  sv->sv_retry_at = CurrentTime + sv->sv_backoff;

  log_write(LS_SYSTEM, L_WARNING, 0,
            "Cannot introduce service %s: %s; trying again in %u seconds",
            sv->sv_name, why, sv->sv_backoff);
  return 0;
}

/** Give up on a bot the configuration makes impossible. */
static int svc_give_up(struct Service* sv, const char* why)
{
  sv->sv_broken = 1;
  sendto_opmask_butone(0, SNO_OLDSNO, "Service %s cannot be introduced: %s",
                       sv->sv_name, why);
  log_write(LS_SYSTEM, L_ERROR, 0, "Service %s cannot be introduced: %s",
            sv->sv_name, why);
  return 0;
}

/** Kill the user holding a service's nick, as services have always done.
 *
 * The sequence is the one m_nick.c uses for a collision: tell the
 * network, mark the client so its exit sends no QUIT of its own, then
 * remove it here.  A local victim is told first, the way m_kill does.
 */
static void svc_kill_holder(struct Client* acptr, const char* reason)
{
  if (MyConnect(acptr))
    sendcmdto_one(&me, CMD_KILL, acptr, "%C :%s %s", acptr, cli_name(&me),
                  reason);
  sendcmdto_serv_butone(&me, CMD_KILL, NULL, "%C :%s (%s)", acptr,
                        cli_name(&me), reason);
  SetFlag(acptr, FLAG_KILLED);
  exit_client_msg(cli_from(acptr), acptr, &me, "Killed (%s (%s))",
                  cli_name(&me), reason);
}

/** Introduce a service's bot, clearing its nick if it must.
 * @param[in] sv The service, with no bot.
 * @return Non-zero if the bot now exists.
 */
static int svc_introduce(struct Service* sv)
{
  struct Client* bot;
  struct SLink* lp;
  int err;

  assert(!sv->sv_client);

  err = bot_check_nick(sv->sv_name, NULL);
  if (err == ERR_NICKNAMEINUSE) {
    struct Client* acptr = FindClient(sv->sv_name);

    if (!acptr)
      return svc_retry_later(sv, "the nick is juped");
    if (IsServer(acptr))
      return svc_give_up(sv, "a server has that name");
    if (IsServiceBot(acptr) || IsChannelService(acptr)) {
      char why[BUFSIZE];

      /* Another service has the nick -- most likely the same block on
       * another server.  Killing it would be a war; waiting is not.
       */
      ircd_snprintf(0, why, sizeof(why), "%s!%s@%s on %s is a service too",
                    cli_name(acptr), cli_user(acptr)->username,
                    cli_user(acptr)->host, cli_name(cli_user(acptr)->server));
      return svc_retry_later(sv, why);
    }

    sendto_opmask_butone(0, SNO_OLDSNO, "Killing %s (%s@%s): nick %s is "
                         "reserved for network services",
                         cli_name(acptr), cli_user(acptr)->username,
                         cli_user(acptr)->host, sv->sv_name);
    log_write(LS_SYSTEM, L_INFO, 0, "Killed %#C: nick reserved for service",
              acptr);
    svc_kill_holder(acptr, "Nick reserved for network services");
  } else if (err) {
    return svc_give_up(sv, "the name is not a valid nick");
  }

  bot = bot_create(svc_mod, sv->sv_name, sv->sv_username, sv->sv_host,
                   sv->sv_description, BOT_SERVICE);
  if (!bot)
    return svc_retry_later(sv, "no numeric nick is free");

  sv->sv_client = bot;
  sv->sv_backoff = 0;
  sv->sv_retry_at = 0;

  for (lp = sv->sv_channels; lp; lp = lp->next) {
    if (bot_check_channel(lp->value.cp))
      log_write(LS_SYSTEM, L_WARNING, 0,
                "Service %s: cannot join %s: not an acceptable channel",
                sv->sv_name, lp->value.cp);
    else
      bot_join(bot, lp->value.cp);
  }

  sendto_opmask_butone(0, SNO_OLDSNO, "Service %s (%s@%s) [%s] is online",
                       cli_name(bot), cli_user(bot)->username,
                       cli_user(bot)->host, sv->sv_type->st_name);
  log_write(LS_SYSTEM, L_INFO, 0, "Service %s (%s@%s) [%s] is online",
            cli_name(bot), cli_user(bot)->username, cli_user(bot)->host,
            sv->sv_type->st_name);

  return 1;
}

/** Introduce every bot that is missing and due. */
static void svc_introduce_due(void)
{
  struct Service* sv;

  for (sv = svc_list; sv; sv = sv->sv_next)
    if (!sv->sv_client && !sv->sv_broken && sv->sv_retry_at <= CurrentTime)
      svc_introduce(sv);

  svc_schedule();
}

/** The retry timer.
 * @param[in] ev ET_EXPIRE when it is time; ET_DESTROY when it is dropped.
 */
static void svc_timer_cb(struct Event* ev)
{
  if (ev_type(ev) != ET_EXPIRE)
    return;

  svc_introduce_due();
}

/*
 * Matching the configuration.
 */

/** Make the services match the Service{} blocks.
 *
 * Mark and sweep: every record is stale until a block claims it, a block
 * that matches an existing record claims it, a block that does not gets
 * a fresh record (after the old one of that name, if any, is gone), and
 * what is still stale at the end is removed.  Then whatever has no bot
 * is introduced.
 */
static void svc_reconcile(void)
{
  const struct ServiceConf* conf;
  struct Service* sv;
  struct Service* next;

  for (sv = svc_list; sv; sv = sv->sv_next)
    sv->sv_stale = 1;

  for (conf = conf_service_list(); conf; conf = conf->next) {
    const struct ServiceType* type = find_type(conf->type);

    if (!type) {
      sendto_opmask_butone(0, SNO_OLDSNO, "Service %s: unknown type \"%s\"; "
                           "it is not introduced", conf->name, conf->type);
      log_write(LS_SYSTEM, L_ERROR, 0, "Service %s: unknown type \"%s\"",
                conf->name, conf->type);
      continue;
    }

    sv = svc_find_by_name(conf->name);
    if (sv && svc_matches(sv, conf, type)) {
      sv->sv_stale = 0;
      continue;
    }

    if (sv)
      svc_remove(sv, "Service reconfigured");

    svc_new(conf, type);
  }

  for (sv = svc_list; sv; sv = next) {
    next = sv->sv_next;
    if (sv->sv_stale)
      svc_remove(sv, "Service removed from the configuration");
  }

  svc_introduce_due();
}

/*
 * Hooks.
 */

/** A message for one of our bots. */
static enum HookResult svc_on_message(struct HookContext* ctx, void* user)
{
  struct Service* sv = svc_find_by_client(ctx->hc_client);

  (void) user;

  if (sv && ctx->hc_source && ctx->hc_arg)
    svc_dispatch(sv, ctx->hc_source, ctx->hc_channel, ctx->hc_notice,
                 ctx->hc_arg);

  return HOOK_CONTINUE;
}

/** A client is leaving; if it is one of our bots, arrange its return.
 *
 * Not brought back here: the client is half-way out and its nick still
 * hashed.  The timer does it, from a clean event loop iteration.
 */
static enum HookResult svc_on_exiting(struct HookContext* ctx, void* user)
{
  struct Service* sv = svc_find_by_client(ctx->hc_client);

  (void) user;

  if (!sv)
    return HOOK_CONTINUE;

  sv->sv_client = NULL;

  if (sv->sv_expected || svc_unloading)
    return HOOK_CONTINUE;

  sendto_opmask_butone(0, SNO_OLDSNO, "Service %s is gone (%s); bringing it "
                       "back in %d seconds", sv->sv_name,
                       ctx->hc_arg ? ctx->hc_arg : "no reason", SVC_RETRY_MIN);
  log_write(LS_SYSTEM, L_WARNING, 0, "Service %s is gone (%s); bringing it "
            "back in %d seconds", sv->sv_name,
            ctx->hc_arg ? ctx->hc_arg : "no reason", SVC_RETRY_MIN);

  sv->sv_backoff = 0;
  sv->sv_retry_at = CurrentTime + SVC_RETRY_MIN;
  svc_schedule();

  return HOOK_CONTINUE;
}

/** The configuration has been read in full. */
static enum HookResult svc_on_config(struct HookContext* ctx, void* user)
{
  (void) ctx;
  (void) user;

  svc_reconcile();

  return HOOK_CONTINUE;
}

/*
 * The module.
 */

static int svc_init(struct ModuleHandle* mod)
{
  svc_mod = mod;
  svc_unloading = 0;
  timer_init(&svc_timer);

  /* Exiting first: from the moment a bot exists, its loss must be seen. */
  if (!module_add_hook(mod, HOOK_CLIENT_EXITING, svc_on_exiting,
                       HOOK_PRIORITY_DEFAULT, NULL)
      || !module_add_hook(mod, HOOK_MESSAGE_RECEIVED, svc_on_message,
                          HOOK_PRIORITY_DEFAULT, NULL)
      || !module_add_hook(mod, HOOK_CONFIG_LOADED, svc_on_config,
                          HOOK_PRIORITY_DEFAULT, NULL))
    return -1;

  /* Loaded from a Module{} block, this runs in the middle of the parse,
   * with the Service{} blocks after it unread and, at start-up, the
   * server not yet ready to introduce anyone: HOOK_CONFIG_LOADED will
   * come.  Loaded by an operator, the configuration is complete and the
   * server is up, and nothing else will call.
   */
  if (module_loaded_by(mod))
    svc_reconcile();

  return 0;
}

static void svc_fini(struct ModuleHandle* mod)
{
  (void) mod;

  svc_unloading = 1;

  if (t_active(&svc_timer))
    timer_del(&svc_timer);

  while (svc_list)
    svc_remove(svc_list, "Services unloaded");

  svc_mod = NULL;
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "irc_services",
  "0.1.0",
  "ircu developers",
  "Network services (NickServ, ChanServ, ...) embedded in the server",
  svc_init,
  svc_fini,
  NULL
};
