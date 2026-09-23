#ifndef INCLUDED_module_sync_h
#define INCLUDED_module_sync_h
/*
 * IRC - Internet Relay Chat, include/module_sync.h
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
 * A module registers commands, user modes, channel modes, capabilities
 * and hooks, which is to say it changes what the server does with what
 * arrives on a link.  Two servers that do not run the same modules do
 * not speak the same protocol, and the difference shows up as a desync
 * rather than as an error.  So the rule here is flat: **every server on
 * the network runs exactly the same modules, for the whole time it is on
 * it**.  There are two halves to keeping that true.
 *
 * **A link is refused when the sets differ.**  #modsync_announce() sends
 * this server's digest (#module_set_digest) as the first thing after the
 * SERVER line, and the peer's arrives just as early; each side compares
 * and drops the link by itself, so neither has to be told and there is no
 * ordering to get right.  A peer that says nothing at all is refused when
 * its burst ends (#modsync_burst_done), because a set that cannot be
 * compared is not a set that matched.
 *
 * **Loading or unloading is a transaction over the whole network.**
 * /MODULE LOAD, UNLOAD and RELOAD do not act on the server the operator
 * happens to be on: that server becomes the coordinator of a two-phase
 * commit (#modsync_begin).  Every server prepares -- which for a load is
 * the load itself, because nothing short of it can say whether the module
 * will load here -- and answers.  One failure anywhere aborts everywhere,
 * which is the point: a network where the module loaded on three servers
 * out of four is the state this whole file exists to prevent.  A server
 * that never answers is a failure like any other, under
 * @c FEAT_MODULE_SYNC_TIMEOUT.
 *
 * Only LOAD can be undone by aborting, since only LOAD had nothing before
 * it.  An aborted UNLOAD never unloaded; an aborted RELOAD converges on
 * the module being *gone* everywhere, because the alternative -- some
 * servers running it and some not -- is the one outcome that is worse
 * than losing it.
 *
 * Everything here runs in the main thread.
 */

struct Client;
struct ModuleHandle;

/** What a module transaction does. */
enum ModSyncOp {
  MODSYNC_LOAD,     /**< Load a module that nobody has loaded. */
  MODSYNC_UNLOAD,   /**< Unload a module everybody has loaded. */
  MODSYNC_RELOAD    /**< Unload and load it again, everywhere. */
};

/*
 * The link check.
 */

/** Send our module set to a peer; server_estab() only, before the burst. */
extern void modsync_announce(struct Client* cptr);

/** The local module set changed: tell every peer, and drop the links to
 * the ones that no longer match.
 *
 * Called after a rehash, whose Module{} blocks are this server's own and
 * are therefore not propagated -- they are *detected*.  Also called after
 * a transaction settles, where every server made the same change and the
 * announcement is what proves it.
 */
extern void modsync_local_change(void);

/** Non-zero once \a cptr told us its module set. */
extern int modsync_peer_checked(const struct Client* cptr);

/** The peer's burst ended; refuse the link if it never said what it runs.
 * @param[in] cptr Peer whose END_OF_BURST arrived.
 * @return Non-zero if the link was dropped, and \a cptr with it.
 */
extern int modsync_burst_done(struct Client* cptr);

/*
 * Transactions.
 */

/** Start a network-wide module transaction.
 *
 * @param[in] sptr Operator who asked; answered with notices as it goes.
 * @param[in] op What to do.
 * @param[in] name Module name, as /MODULE takes it.
 * @param[in] isolation Where it should run, for a load.
 * @return Non-zero if the transaction started; zero after a reply saying
 *   why it could not.
 */
extern int modsync_begin(struct Client* sptr, enum ModSyncOp op,
                         const char* name, int isolation);

/** Non-zero while this server has a transaction in flight. */
extern int modsync_busy(void);

/** A server left the network; stop waiting for it, or give up if it was
 * the one coordinating.  From exit_one_client(). */
extern void modsync_server_gone(struct Client* server);

/** Handle a MODULE message from another server; ms_module() only.
 * @return Zero, or CPTR_KILLED if \a cptr was dropped.
 */
extern int modsync_recv(struct Client* cptr, struct Client* sptr, int parc,
                        char* parv[]);

/** Set up the timers.  Called once from the server's own start-up. */
extern void modsync_init(void);

/** Drop anything in flight; server shutdown only. */
extern void modsync_shutdown(void);

#endif /* INCLUDED_module_sync_h */
