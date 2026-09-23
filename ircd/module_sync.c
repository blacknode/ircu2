/*
 * IRC - Internet Relay Chat, ircd/module_sync.c
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
 * @brief The module set as a property of the network.
 *
 * See include/module_sync.h for what this enforces and why.  This file
 * is the wire: the MD (MODULE) messages a server sends another, the
 * comparison that refuses a link, and the two-phase commit that makes a
 * load or an unload happen on every server or on none.
 *
 * Every message has the same shape past the command:
 *
 * @code
 *   <source> MD <subcommand> <target> [arguments...]
 * @endcode
 *
 * @c <target> is @c * for something every server is to read, or a
 * server's numeric for an answer meant for one.  That one field is all
 * the routing there is: a server that is not the target forwards and
 * stops thinking about it.
 */
#include "config.h"

#include "module_sync.h"

#include "client.h"
#include "hash.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_events.h"
#include "ircd_features.h"
#include "ircd_i18n.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "list.h"
#include "module.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "modhost.h"
#include "s_conf.h"
#include "s_debug.h"
#include "s_misc.h"
#include "send.h"
#include "struct.h"

#include <stdarg.h>
#include <string.h>

/** Longest transaction identifier, NUL included: this server's numeric
 * and a counter, which is what modsync_begin() builds it out of. */
#define MODSYNC_TXID_LEN 32
/** Longest module name a transaction carries, NUL included. */
#define MODSYNC_NAME_LEN 64
/** Longest failure reason kept, NUL included. */
#define MODSYNC_REASON_LEN 256

/** One server a coordinator is still waiting on. */
struct ModSyncPeer {
  struct ModSyncPeer* mp_next;
  struct Client*      mp_server;  /**< Never dereferenced after it exits:
                                       modsync_server_gone() drops it
                                       first, from exit_one_client(). */
};

/** What this server is coordinating, if anything. */
static struct {
  int                 active;
  char                txid[MODSYNC_TXID_LEN];
  enum ModSyncOp      op;
  char                name[MODSYNC_NAME_LEN];
  int                 isolation;    /**< #MODULE_NATIVE or #MODULE_PROCESS;
                                         what a load was asked for. */
  char                asker[NICKLEN + 1];  /**< Operator's nick, for the
                                                notices; a copy, since the
                                                client may leave. */
  struct ModSyncPeer* waiting;
  int                 failed;
  char                reason[MODSYNC_REASON_LEN];
} coord;

/** What this server was asked to prepare, if anything. */
static struct {
  int                 active;
  char                txid[MODSYNC_TXID_LEN];
  enum ModSyncOp      op;
  char                name[MODSYNC_NAME_LEN];
  int                 isolation_is_process; /**< Where a load should run. */
  char                asker[NICKLEN + HOSTLEN + 2]; /**< Who asked, as
                                                         "<nick> on <server>",
                                                         for the listings. */
  int                 undo_load;   /**< We loaded it; abort must unload. */
  struct Client*      coordinator; /**< Server that asked, or &me. */
} part;

/** Counter behind the transaction identifiers this server mints. */
static unsigned int modsync_counter;

/** Bounds the wait, on either side of a transaction. */
static struct Timer modsync_timer;

/** Non-zero once #modsync_timer has been initialised. */
static int modsync_timer_ready;

static void modsync_timer_cb(struct Event* ev);
static void part_settle(int commit);

/* ------------------------------------------------------------------ */
/* Small shared helpers                                               */
/* ------------------------------------------------------------------ */

/** The name of an operation, as it travels. */
static const char* modsync_opname(enum ModSyncOp op)
{
  switch (op) {
  case MODSYNC_LOAD:   return "LOAD";
  case MODSYNC_UNLOAD: return "UNLOAD";
  case MODSYNC_RELOAD: return "RELOAD";
  }
  return "?";
}

/** Read an operation back off the wire.
 * @param[in] text What arrived.
 * @param[out] op Receives the operation.
 * @return Non-zero if \a text named one.
 */
static int modsync_op(const char* text, enum ModSyncOp* op)
{
  if (0 == ircd_strcmp(text, "LOAD"))
    *op = MODSYNC_LOAD;
  else if (0 == ircd_strcmp(text, "UNLOAD"))
    *op = MODSYNC_UNLOAD;
  else if (0 == ircd_strcmp(text, "RELOAD"))
    *op = MODSYNC_RELOAD;
  else
    return 0;

  return 1;
}

/** Tell the operator who asked, if they are still here. */
static void modsync_tell(const char* pattern, ...)
{
  struct Client* acptr;
  struct VarData vd;

  if (!coord.asker[0])
    return;
  if (!(acptr = FindUser(coord.asker)) || !MyUser(acptr))
    return;

  vd.vd_format = pattern;
  va_start(vd.vd_args, pattern);
  sendcmdto_one(&me, CMD_NOTICE, acptr, "%C :%v", acptr, &vd);
  va_end(vd.vd_args);
}

/** Arm the deadline, or push it out. */
static void modsync_arm(void)
{
  int seconds = feature_int(FEAT_MODULE_SYNC_TIMEOUT);

  if (seconds < 5)
    seconds = 5;

  /* timer_init() zeroes GEN_MARKED, which is the only thing that tells
   * timer_add() it is re-arming a timer timer_run() still holds; so it
   * happens once, at start-up, and never again.  See CLAUDE.md.
   */
  if (!modsync_timer_ready) {
    timer_init(&modsync_timer);
    modsync_timer_ready = 1;
  }

  if (t_active(&modsync_timer))
    timer_chg(&modsync_timer, TT_RELATIVE, seconds);
  else
    timer_add(&modsync_timer, modsync_timer_cb, NULL, TT_RELATIVE, seconds);
}

/** Drop the deadline when nothing is outstanding on either side. */
static void modsync_disarm(void)
{
  if (!coord.active && !part.active && t_active(&modsync_timer))
    timer_del(&modsync_timer);
}

/* ------------------------------------------------------------------ */
/* The link check                                                     */
/* ------------------------------------------------------------------ */

void modsync_announce(struct Client* cptr)
{
  char digest[MODULE_DIGEST_LEN];
  char names[BUFSIZE];

  assert(0 != cptr);

  if (!feature_bool(FEAT_MODULE_SYNC))
    return;

  module_set_digest(digest, sizeof(digest));
  module_set_names(names, sizeof(names));

  /* The names are for the operator reading the mismatch message on the
   * other side; the digest is what decides.  Sent as the trailing
   * parameter so a long list is truncated by the send layer rather than
   * pushing anything that matters off the line.
   */
  sendcmdto_one(&me, CMD_MODULE, cptr, "SET * %s %u :%s", digest,
                module_count(), names);
}

int modsync_peer_checked(const struct Client* cptr)
{
  return cptr && HasFlag(cptr, FLAG_MODSYNC_OK);
}

/** Refuse a link whose module set is not ours.
 * @param[in] cptr Peer to drop.
 * @param[in] theirs Digest it announced, or NULL when it announced none.
 * @param[in] names What it says it runs, or NULL.
 * @return CPTR_KILLED, always: \a cptr is gone when this returns.
 */
static int modsync_refuse(struct Client* cptr, const char* theirs,
                          const char* names)
{
  char ours[MODULE_DIGEST_LEN];
  char mine[BUFSIZE];

  module_set_digest(ours, sizeof(ours));
  module_set_names(mine, sizeof(mine));

  sendto_opmask_butone(0, SNO_OLDSNO,
                       "Refusing link with %s: it does not run the same "
                       "modules.  Here: %s.  There: %s", cli_name(cptr),
                       mine[0] ? mine : "(none)",
                       names && *names ? names
                                       : (theirs ? "(none)" : "(not stated)"));
  log_write(LS_NETWORK, L_ERROR, 0,
            "Refusing link with %s: module set %s here, %s there", cli_name(cptr),
            ours, theirs ? theirs : "(not stated)");

  return exit_client_msg(cptr, cptr, &me,
                         "Module set mismatch: every server on the network "
                         "must run the same modules");
}

/** Handle an incoming "MD SET".
 * @param[in] cptr Link it arrived on; only a directly linked server may
 *   send one, since this is a property of the link and is never relayed.
 * @param[in] digest Digest the peer announced.
 * @param[in] names What it says it runs, for the message; may be NULL.
 * @return Zero, or CPTR_KILLED when the link was refused.
 */
static int modsync_recv_set(struct Client* cptr, const char* digest,
                            const char* names)
{
  char ours[MODULE_DIGEST_LEN];

  if (!feature_bool(FEAT_MODULE_SYNC))
    return 0;

  /* A transaction in flight is exactly when the two ends legitimately
   * disagree: the peer committed a moment before this server did, or the
   * other way round.  The announcement each of them sends once it has
   * settled is what closes that window, so an announcement arriving in
   * the middle of one says nothing and is ignored rather than acted on.
   */
  if (modsync_busy()) {
    Debug((DEBUG_DEBUG, "Ignoring module set from %s: a transaction is in "
           "flight", cli_name(cptr)));
    return 0;
  }

  module_set_digest(ours, sizeof(ours));

  if (0 != strcmp(ours, digest))
    return modsync_refuse(cptr, digest, names);

  SetFlag(cptr, FLAG_MODSYNC_OK);

  Debug((DEBUG_DEBUG, "Module set of %s matches (%s)", cli_name(cptr), ours));

  return 0;
}

void modsync_local_change(void)
{
  struct DLink* lp;
  struct DLink* next;

  if (!feature_bool(FEAT_MODULE_SYNC))
    return;

  /* Announcing to every peer rather than only to the ones that differ:
   * this server cannot know what they run, and one line per link is what
   * asking them would have cost anyway.  Each peer compares and drops the
   * link by itself, which is the same path a new link takes.
   */
  for (lp = cli_serv(&me)->down; lp; lp = next) {
    next = lp->next;
    modsync_announce(lp->value.cptr);
  }
}

int modsync_burst_done(struct Client* cptr)
{
  if (!feature_bool(FEAT_MODULE_SYNC) || !MyConnect(cptr))
    return 0;

  if (modsync_peer_checked(cptr))
    return 0;

  /* A service of the network is not a node of it.  The services run on
   * a server of their own -- classically a different program entirely,
   * which has no modules of ours to run and nothing to compare -- and
   * they are known by the +s flag in their SERVER line or by a Uworld{}
   * block naming them.  Requiring a module set from one would make this
   * rule a rule against having services at all.
   *
   * What is excused is silence, not disagreement: a service server that
   * does announce a set is compared like anybody else, above.
   */
  if (IsService(cptr)
      || find_conf_byhost(cli_confs(cptr), cli_name(cptr), CONF_UWORLD)) {
    Debug((DEBUG_DEBUG, "%s is a service; it has no module set to state",
           cli_name(cptr)));
    return 0;
  }

  /* Same window as above: what arrived during a transaction was ignored
   * rather than compared, so there is nothing yet to hold against it.
   */
  if (modsync_busy())
    return 0;

  /* A peer that never said what it runs has not been compared, and an
   * uncompared set is not a set that matched -- the whole rule is that a
   * server is on this network only while it is known to run what everyone
   * else runs.
   */
  return modsync_refuse(cptr, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* Transactions: the coordinator                                      */
/* ------------------------------------------------------------------ */

/** Forget every server the coordinator is waiting on. */
static void coord_clear_peers(void)
{
  struct ModSyncPeer* mp;
  struct ModSyncPeer* next;

  for (mp = coord.waiting; mp; mp = next) {
    next = mp->mp_next;
    MyFree(mp);
  }
  coord.waiting = NULL;
}

/** Note that \a server has answered, or has gone.
 * @return Non-zero if it was one we were waiting for.
 */
static int coord_drop_peer(struct Client* server)
{
  struct ModSyncPeer** pp;
  struct ModSyncPeer* mp;

  for (pp = &coord.waiting; *pp; pp = &(*pp)->mp_next)
    if ((*pp)->mp_server == server) {
      mp = *pp;
      *pp = mp->mp_next;
      MyFree(mp);
      return 1;
    }

  return 0;
}

/** Record every server on the network as one to wait for.
 * @return How many there are.
 */
static unsigned int coord_collect_peers(void)
{
  struct Client* acptr;
  unsigned int n = 0;

  coord_clear_peers();

  for (acptr = GlobalClientList; acptr; acptr = cli_next(acptr)) {
    struct ModSyncPeer* mp;

    if (!IsServer(acptr) || IsMe(acptr))
      continue;

    mp = (struct ModSyncPeer*) MyMalloc(sizeof(*mp));
    mp->mp_server = acptr;
    mp->mp_next = coord.waiting;
    coord.waiting = mp;
    n++;
  }

  return n;
}

/** Put the transaction away, whichever way it ended. */
static void coord_end(void)
{
  coord_clear_peers();
  coord.active = 0;
  coord.asker[0] = '\0';
  modsync_disarm();
}

/** Broadcast the outcome and stop coordinating.
 * @param[in] commit Non-zero to commit, zero to abort.
 */
static void coord_finish(int commit)
{
  char opname[16];
  char name[MODSYNC_NAME_LEN];

  ircd_strncpy(opname, modsync_opname(coord.op), sizeof(opname) - 1);
  opname[sizeof(opname) - 1] = '\0';
  ircd_strncpy(name, coord.name, sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';

  if (commit)
    sendcmdto_serv_butone(&me, CMD_MODULE, NULL, "COMMIT * %s", coord.txid);
  else
    sendcmdto_serv_butone(&me, CMD_MODULE, NULL, "ABORT * %s :%s", coord.txid,
                          coord.reason[0] ? coord.reason : "aborted");

  if (commit) {
    modsync_tell("%s of %s is done on every server.", opname, name);
    sendto_opmask_butone(0, SNO_OLDSNO,
                         "Module %s: %s completed on the whole network",
                         name, opname);
  } else {
    modsync_tell("%s of %s was abandoned: %s", opname, name,
                 coord.reason[0] ? coord.reason : "aborted");
    sendto_opmask_butone(0, SNO_OLDSNO,
                         "Module %s: %s abandoned network-wide: %s", name,
                         opname, coord.reason[0] ? coord.reason : "aborted");
  }

  /* This server's own half of the transaction is a participant like any
   * other, and settles after the outcome has gone out -- which also puts
   * the announcement that follows it (modsync_local_change()) behind the
   * COMMIT on every link, so no peer compares module sets with this one
   * before it has applied the same change.
   */
  if (part.active && 0 == strcmp(part.txid, coord.txid))
    part_settle(commit);

  coord_end();
}

/** Everything that was going to answer has answered. */
static void coord_settle(void)
{
  coord_finish(!coord.failed);
}

/** Record a failure; the first reason is the one kept, since it is the
 * one that happened before anybody else gave up waiting. */
static void coord_fail(const char* reason)
{
  if (!coord.failed) {
    coord.failed = 1;
    ircd_strncpy(coord.reason, reason ? reason : "no reason given",
                 sizeof(coord.reason) - 1);
    coord.reason[sizeof(coord.reason) - 1] = '\0';
  }
}

/* ------------------------------------------------------------------ */
/* Transactions: the participant                                      */
/* ------------------------------------------------------------------ */

/** Answer the coordinator.
 * @param[in] ok Non-zero for READY, zero for FAILED.
 * @param[in] reason Why it failed; ignored when \a ok.
 */
static void part_answer(int ok, const char* reason)
{
  /* Only a server that asked over the wire is answered: a transaction
   * this server started prepares in modsync_begin(), which has the
   * coordinator's state to hand and never comes through here.
   */
  assert(0 != part.coordinator);
  assert(part.coordinator != &me);

  if (ok)
    sendcmdto_one(&me, CMD_MODULE, part.coordinator, "READY %s %s",
                  NumServ(part.coordinator), part.txid);
  else
    sendcmdto_one(&me, CMD_MODULE, part.coordinator, "FAILED %s %s :%s",
                  NumServ(part.coordinator), part.txid,
                  reason ? reason : "no reason given");
}

/** Put the participant state away. */
static void part_end(void)
{
  part.active = 0;
  part.undo_load = 0;
  part.coordinator = NULL;
  modsync_disarm();
}

/** Undo what preparing did, as far as it can be undone.
 *
 * Only a load can be taken back: it had nothing before it.  An unload
 * never happened yet, so there is nothing to undo.  A reload has already
 * replaced the module, and the only state every server can still agree on
 * is the module being gone -- see include/module_sync.h.
 */
static void part_rollback(void)
{
  struct ModuleHandle* mod;

  if (!part.undo_load)
    return;

  if ((mod = module_find_file(part.name)) || (mod = module_find(part.name))) {
    if (module_unload(mod))
      log_write(LS_SYSTEM, L_INFO, 0,
                "Module %s: transaction %s abandoned, unloaded again",
                part.name, part.txid);
    else
      log_write(LS_SYSTEM, L_ERROR, 0,
                "Module %s: transaction %s abandoned but it could not be "
                "unloaded again", part.name, part.txid);
  }
}

/** Do the half of an operation that can be done before committing.
 * @param[out] why Receives the reason on failure.
 * @param[in] whylen Size of \a why.
 * @return Non-zero when this server is ready to commit.
 */
static int part_prepare(char* why, size_t whylen)
{
  struct ModuleHandle* mod;
  const char* err = NULL;
  char file[MODSYNC_NAME_LEN];
  int was_isolated = 0;

  switch (part.op) {
  case MODSYNC_LOAD:
    if (module_find_file(part.name) || module_find(part.name)) {
      ircd_snprintf(0, why, whylen, "%s: already loaded on %s", part.name,
                    cli_name(&me));
      return 0;
    }
    if (!module_load_isolation(part.name, part.asker[0] ? part.asker : NULL,
                               part.isolation_is_process ? MODULE_PROCESS
                                                         : MODULE_NATIVE,
                               &err)) {
      ircd_snprintf(0, why, whylen, "%s: %s on %s", part.name,
                    err ? err : "could not be loaded", cli_name(&me));
      return 0;
    }
    /* Loaded, but not yet committed: an abort takes it out again. */
    part.undo_load = 1;
    return 1;

  case MODSYNC_UNLOAD:
    if (!module_find_file(part.name) && !module_find(part.name)) {
      ircd_snprintf(0, why, whylen, "%s: not loaded on %s", part.name,
                    cli_name(&me));
      return 0;
    }
    /* Nothing is done until the commit: an unload that has happened
     * cannot be taken back, so it waits for everyone else.
     */
    return 1;

  case MODSYNC_RELOAD:
    if (!(mod = module_find_file(part.name))
        && !(mod = module_find(part.name))) {
      ircd_snprintf(0, why, whylen, "%s: not loaded on %s", part.name,
                    cli_name(&me));
      return 0;
    }
    /* module_unload() frees the handle, so the name it was loaded by is
     * taken first -- it need not be the name the module declares.
     */
    ircd_strncpy(file, module_file(mod), sizeof(file) - 1);
    file[sizeof(file) - 1] = '\0';
    /* A reload keeps the module where it was running: bringing an
     * isolated module quietly back inside the server is the one change an
     * operator would never think to check for.
     */
    was_isolated = modhost_isolated(mod);
    if (!module_unload(mod)) {
      ircd_snprintf(0, why, whylen, "%s: could not be unloaded on %s",
                    part.name, cli_name(&me));
      return 0;
    }
    if (!module_load_isolation(file, part.asker[0] ? part.asker : NULL,
                               was_isolated ? MODULE_PROCESS : MODULE_NATIVE,
                               &err)) {
      ircd_snprintf(0, why, whylen, "%s: unloaded but would not load again "
                    "on %s: %s", part.name, cli_name(&me),
                    err ? err : "unknown error");
      return 0;
    }
    part.undo_load = 1;
    return 1;
  }

  ircd_snprintf(0, why, whylen, "unknown operation");
  return 0;
}

/** Make the prepared operation final. */
static void part_commit(void)
{
  struct ModuleHandle* mod;

  if (part.op == MODSYNC_UNLOAD) {
    if ((mod = module_find_file(part.name)) || (mod = module_find(part.name))) {
      if (!module_unload(mod))
        log_write(LS_SYSTEM, L_ERROR, 0,
                  "Module %s: committed unload failed on %s", part.name,
                  cli_name(&me));
    }
  }

  log_write(LS_SYSTEM, L_INFO, 0, "Module %s: %s committed network-wide",
            part.name, modsync_opname(part.op));
}

/** Apply an outcome to this server and forget the transaction.
 * @param[in] commit Non-zero to keep what was prepared, zero to undo it.
 */
static void part_settle(int commit)
{
  if (!part.active)
    return;

  if (commit)
    part_commit();
  else
    part_rollback();

  part_end();

  /* Whatever just happened changed what this server runs, and the links
   * are what the change has to agree with.
   */
  modsync_local_change();
}

/* ------------------------------------------------------------------ */
/* The deadline                                                       */
/* ------------------------------------------------------------------ */

/** Nobody answered in time, on whichever side was waiting. */
static void modsync_timer_cb(struct Event* ev)
{
  if (ev_type(ev) != ET_EXPIRE)
    return;

  if (coord.active) {
    struct ModSyncPeer* mp;
    char who[HOSTLEN + 32];

    mp = coord.waiting;
    ircd_snprintf(0, who, sizeof(who), "%s did not answer",
                  mp ? cli_name(mp->mp_server) : "a server");
    coord_fail(who);
    coord_settle();
    return;
  }

  if (part.active) {
    /* The coordinator went quiet.  Undoing is the only safe answer: it
     * leaves this server where it was, which is where every server that
     * also failed to hear a commit will be.
     */
    log_write(LS_SYSTEM, L_WARNING, 0,
              "Module %s: transaction %s was never settled; undoing",
              part.name, part.txid);
    part_settle(0);
  }
}

/* ------------------------------------------------------------------ */
/* Entry points                                                       */
/* ------------------------------------------------------------------ */

int modsync_busy(void)
{
  return coord.active || part.active;
}

int modsync_begin(struct Client* sptr, enum ModSyncOp op, const char* name,
                  int isolation)
{
  char why[MODSYNC_REASON_LEN];
  unsigned int peers;

  assert(0 != sptr);
  assert(0 != name);

  if (modsync_busy()) {
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  _(sptr, "%C :Another module transaction is in progress; "
                    "try again in a moment"), sptr);
    return 0;
  }

  /* With the rule turned off here, this is one server's change and not
   * the network's; doing it anyway under a name that promises otherwise
   * would be the worst of both.
   */
  if (!feature_bool(FEAT_MODULE_SYNC)) {
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  _(sptr, "%C :MODULE_SYNC is off on %s, so %s cannot be done "
                    "network-wide from here"), sptr, cli_name(&me),
                  modsync_opname(op));
    return 0;
  }

  memset(&coord, 0, sizeof(coord));
  coord.active = 1;
  coord.op = op;
  coord.isolation = isolation;
  ircd_strncpy(coord.name, name, sizeof(coord.name) - 1);
  coord.name[sizeof(coord.name) - 1] = '\0';
  ircd_strncpy(coord.asker, cli_name(sptr), NICKLEN);
  coord.asker[NICKLEN] = '\0';
  ircd_snprintf(0, coord.txid, sizeof(coord.txid), "%s%u", NumServ(&me),
                ++modsync_counter);

  peers = coord_collect_peers();

  /* This server prepares too, and it does it first: a change nobody could
   * make here is not one to ask the rest of the network about.
   */
  memset(&part, 0, sizeof(part));
  part.active = 1;
  part.op = op;
  part.isolation_is_process = (isolation == MODULE_PROCESS);
  part.coordinator = &me;
  ircd_strncpy(part.txid, coord.txid, sizeof(part.txid) - 1);
  part.txid[sizeof(part.txid) - 1] = '\0';
  ircd_strncpy(part.name, name, sizeof(part.name) - 1);
  part.name[sizeof(part.name) - 1] = '\0';
  ircd_snprintf(0, part.asker, sizeof(part.asker), "%s on %s", cli_name(sptr),
                cli_name(&me));

  if (!part_prepare(why, sizeof(why))) {
    part_rollback();
    part_end();
    coord_end();
    sendcmdto_one(&me, CMD_NOTICE, sptr, _(sptr, "%C :Could not %s %s: %s"),
                  sptr, modsync_opname(op), name, why);
    return 0;
  }

  sendcmdto_serv_butone(&me, CMD_MODULE, NULL, "PREPARE * %s %s %s %s :%s",
                        coord.txid, modsync_opname(coord.op), coord.name,
                        coord.isolation == MODULE_PROCESS ? "process"
                                                          : "native",
                        part.asker);

  sendcmdto_one(&me, CMD_NOTICE, sptr,
                _(sptr, "%C :%s of %s is ready here; waiting for %u other "
                  "server(s)"), sptr, modsync_opname(op), name, peers);

  if (!peers) {
    /* A network of one settles at once. */
    coord_settle();
    return 1;
  }

  modsync_arm();

  return 1;
}

void modsync_server_gone(struct Client* server)
{
  if (!server || !IsServer(server))
    return;

  if (coord.active && coord_drop_peer(server)) {
    /* A server that split before answering never applied the change, and
     * it will come back without it; the rest of the network must not be
     * left carrying something it does not have.
     */
    char why[HOSTLEN + 32];

    ircd_snprintf(0, why, sizeof(why), "%s left the network", cli_name(server));
    coord_fail(why);
    if (!coord.waiting)
      coord_settle();
  }

  if (part.active && part.coordinator == server) {
    log_write(LS_SYSTEM, L_WARNING, 0,
              "Module %s: the server coordinating %s left; undoing", part.name,
              part.txid);
    part_rollback();
    part_end();
  }
}

/* ------------------------------------------------------------------ */
/* Receiving                                                          */
/* ------------------------------------------------------------------ */

/** Is \a target this server, or somewhere behind another link? */
static struct Client* modsync_target(const char* target)
{
  if (!target || 0 == strcmp(target, "*"))
    return NULL;

  return FindNServer(target);
}

int modsync_recv(struct Client* cptr, struct Client* sptr, int parc,
                 char* parv[])
{
  struct Client* target;
  const char* sub;

  if (!parv || parc < 3)
    return 0;

  sub = parv[1];
  target = modsync_target(parv[2]);

  if (0 == ircd_strcmp(sub, "SET")) {
    /* A link's own statement about itself: never relayed, and only ever
     * read from the server on the other end of the link it arrived on.
     */
    if (parc < 4 || sptr != cptr || !IsServer(sptr))
      return 0;
    return modsync_recv_set(cptr, parv[3], parc > 5 ? parv[5] : NULL);
  }

  if (0 == ircd_strcmp(sub, "PREPARE")) {
    char why[MODSYNC_REASON_LEN];
    enum ModSyncOp op;

    if (parc < 6 || !IsServer(sptr))
      return 0;

    /* Every server reads this one, so it is passed on before it is acted
     * on: preparing can take as long as a dlopen() and an mi_init, and
     * the servers behind us should not wait for ours.
     */
    sendcmdto_serv_butone(sptr, CMD_MODULE, cptr, "PREPARE * %s %s %s %s :%s",
                          parv[3], parv[4], parv[5],
                          parc > 6 ? parv[6] : "native",
                          parc > 7 ? parv[7] : "another server");

    if (!modsync_op(parv[4], &op))
      return 0;

    /* A server that has opted out of the rule is not a server that can
     * be asked to keep it.  Saying so is better than staying quiet and
     * letting the coordinator time out: the answer is the same, and the
     * operator gets a reason instead of a wait.
     */
    if (!feature_bool(FEAT_MODULE_SYNC)) {
      char why[HOSTLEN + 48];

      ircd_snprintf(0, why, sizeof(why), "%s does not synchronise modules",
                    cli_name(&me));
      sendcmdto_one(&me, CMD_MODULE, sptr, "FAILED %s %s :%s", NumServ(sptr),
                    parv[3], why);
      return 0;
    }

    if (modsync_busy()) {
      sendcmdto_one(&me, CMD_MODULE, sptr, "FAILED %s %s :%s", NumServ(sptr),
                    parv[3], "another module transaction is in progress");
      return 0;
    }

    memset(&part, 0, sizeof(part));
    part.active = 1;
    part.op = op;
    part.coordinator = sptr;
    part.isolation_is_process = parc > 6
                                && 0 == ircd_strcmp(parv[6], "process");
    ircd_strncpy(part.txid, parv[3], sizeof(part.txid) - 1);
    part.txid[sizeof(part.txid) - 1] = '\0';
    ircd_strncpy(part.name, parv[5], sizeof(part.name) - 1);
    part.name[sizeof(part.name) - 1] = '\0';
    ircd_strncpy(part.asker, parc > 7 ? parv[7] : "another server",
                 sizeof(part.asker) - 1);
    part.asker[sizeof(part.asker) - 1] = '\0';

    if (!part_prepare(why, sizeof(why))) {
      part_rollback();
      part_answer(0, why);
      part_end();
      return 0;
    }

    part_answer(1, NULL);
    modsync_arm();
    return 0;
  }

  if (0 == ircd_strcmp(sub, "READY") || 0 == ircd_strcmp(sub, "FAILED")) {
    int ok = (0 == ircd_strcmp(sub, "READY"));

    if (parc < 4 || !IsServer(sptr))
      return 0;

    if (target && target != &me) {
      sendcmdto_one(sptr, CMD_MODULE, target,
                    ok ? "%s %s %s" : "%s %s %s :%s", sub, parv[2], parv[3],
                    parc > 4 ? parv[4] : "");
      return 0;
    }

    if (!coord.active || 0 != strcmp(coord.txid, parv[3]))
      return 0;

    if (!ok)
      coord_fail(parc > 4 ? parv[4] : "a server refused");

    coord_drop_peer(sptr);
    if (!coord.waiting)
      coord_settle();
    else
      modsync_arm();

    return 0;
  }

  if (0 == ircd_strcmp(sub, "COMMIT") || 0 == ircd_strcmp(sub, "ABORT")) {
    int commit = (0 == ircd_strcmp(sub, "COMMIT"));

    if (parc < 4 || !IsServer(sptr))
      return 0;

    sendcmdto_serv_butone(sptr, CMD_MODULE, cptr,
                          commit ? "%s * %s" : "%s * %s :%s", sub, parv[3],
                          parc > 4 ? parv[4] : "aborted");

    if (!part.active || 0 != strcmp(part.txid, parv[3]))
      return 0;

    if (!commit)
      log_write(LS_SYSTEM, L_INFO, 0, "Module %s: %s abandoned: %s", part.name,
                modsync_opname(part.op), parc > 4 ? parv[4] : "aborted");

    part_settle(commit);
    return 0;
  }

  return 0;
}

void modsync_init(void)
{
  memset(&coord, 0, sizeof(coord));
  memset(&part, 0, sizeof(part));

  if (!modsync_timer_ready) {
    timer_init(&modsync_timer);
    modsync_timer_ready = 1;
  }
}

void modsync_shutdown(void)
{
  coord_clear_peers();
  coord.active = 0;
  part.active = 0;

  if (modsync_timer_ready && t_active(&modsync_timer))
    timer_del(&modsync_timer);
}
