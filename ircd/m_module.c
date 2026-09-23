/*
 * IRC - Internet Relay Chat, ircd/m_module.c
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
 * @brief Handlers for the MODULE command.
 */
#include "config.h"

#include "client.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_i18n.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "migration.h"
#include "modhost.h"
#include "module.h"
#include "module_sync.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "s_conf.h"
#include "s_debug.h"
#include "send.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <stdlib.h>
#include <string.h>

/** Send the list of loaded modules to a client.
 * @param[in] sptr Client asking for the list.
 */
static void module_send_list(struct Client* sptr)
{
  struct ModuleHandle* mod;
  char digest[MODULE_DIGEST_LEN];

  for (mod = module_next(0); mod; mod = module_next(mod))
    /* The file name comes first: that is what LOAD, UNLOAD and RELOAD
     * take, and it need not match the name the module declares.
     *
     * The location is relative to the module directory, never the
     * absolute path: where the server keeps its files on the host is not
     * something to hand out over IRC, even to an operator.
     */
    send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
               N_(":Module %s (%s %s, ABI %u): modules/%s -- %s "
                  "[%s, loaded by %s]"),
               module_file(mod), module_name(mod), module_version(mod),
               (unsigned int) IRCU_MODULE_ABI, module_relpath(mod),
               module_description(mod),
               /* Where it runs, because an operator looking at this list
                * is usually asking exactly that -- and because a module
                * that was isolated and came back native after a reload
                * would otherwise look identical to one that never was. */
               modhost_pid(mod) ? "isolated" : "in the server",
               module_loaded_by(mod) ? module_loaded_by(mod)
                                     : "the configuration file");

  send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG, N_(":%u module%s loaded"),
             module_count(), module_count() == 1 ? "" : "s");

  /* The digest every link is compared on.  An operator looking at a
   * refused link wants to read it off both servers, and this is where.
   */
  module_set_digest(digest, sizeof(digest));
  send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG, N_(":Module set: %s"),
             digest);
}

/** Read a version out of an operator's argument.
 *
 * Accepts "v3" and "3" alike: the files are named v3, so that is what an
 * operator will type, but insisting on the "v" would be a pointless thing
 * to be strict about.
 * @param[in] text Argument to read.
 * @param[out] version Receives the version.
 * @return Non-zero when \a text was a version.
 */
static int module_parse_version(const char* text, unsigned int* version)
{
  char* end;
  unsigned long value;

  if (!text || !*text)
    return 0;

  if (*text == 'v' || *text == 'V')
    text++;

  value = strtoul(text, &end, 10);

  if (*end || value < 1 || value > 0xffff)
    return 0;

  *version = (unsigned int) value;

  return 1;
}

/** Handle the MIGRATION subcommands.
 *
 * parv[2] = LIST, STATUS, APPLY or REVERT
 * parv[3] = module name, for all but LIST
 * parv[4] = version, optional, for APPLY and REVERT
 *
 * Nothing here ever runs a module's migrations by itself; that is the whole
 * point of the subcommand existing.  See doc/readme.migrations.
 *
 * @param[in] sptr Operator who asked.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 * @return Zero.
 */
static int module_migration(struct Client* sptr, int parc, char* parv[])
{
  struct ModuleHandle* mod;
  unsigned int version = 0;
  char* what;

  if (parc < 3) {
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  _(sptr, "%C :MODULE MIGRATION subcommand must be LIST, STATUS, "
                  "APPLY or REVERT"), sptr);
    return 0;
  }

  what = parv[2];

  /* LIST is the whole table, every module at once, and takes nothing. */
  if (0 == ircd_strcmp(what, "LIST")) {
    migration_cmd_list(sptr);
    return 0;
  }

  if (parc < 4) {
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  _(sptr, "%C :MODULE MIGRATION %s needs a module name"), sptr, what);
    return 0;
  }

  /* The server's own migrations are not an operator's to drive: they exist
   * to make the table the rest are recorded in, and the daemon applies them
   * when it starts.
   */
  if (migration_reserved_name(parv[3])) {
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  _(sptr, "%C :The server's own migrations run at start-up and are "
                  "not applied or reverted by hand"), sptr);
    return 0;
  }

  if (!(mod = module_find(parv[3])) && !(mod = module_find_file(parv[3]))) {
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  _(sptr, "%C :No module named %s is loaded"),
                  sptr, parv[3]);
    return 0;
  }

  if (0 == ircd_strcmp(what, "STATUS")) {
    migration_cmd_status(sptr, mod);
    return 0;
  }

  if (parc > 4 && !EmptyString(parv[4])
      && !module_parse_version(parv[4], &version)) {
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  _(sptr, "%C :%s is not a version; migrations are numbered v1, v2 "
                  "and so on"), sptr, parv[4]);
    return 0;
  }

  /* Applying and reverting change the schema, so they go through the same
   * switch as LOAD and UNLOAD.
   */
  if (!feature_bool(FEAT_CONFIG_OPERCMDS))
    return send_reply(sptr, ERR_DISABLED, "MODULE");

  if (0 == ircd_strcmp(what, "APPLY")) {
    migration_cmd_apply(sptr, mod, version);
    return 0;
  }

  if (0 == ircd_strcmp(what, "REVERT")) {
    migration_cmd_revert(sptr, mod, version);
    return 0;
  }

  sendcmdto_one(&me, CMD_NOTICE, sptr,
                _(sptr, "%C :MODULE MIGRATION subcommand must be LIST, STATUS, "
                "APPLY or REVERT"), sptr);
  return 0;
}

/** Handle a MODULE command from an operator.
 *
 * parv[1] = subcommand: LIST, LOAD, UNLOAD, RELOAD or MIGRATION
 * parv[2] = module name; LOAD resolves it against the server's module
 *   directory, UNLOAD and RELOAD look it up among the loaded modules
 * parv[3] = for LOAD, "native" (the default) or "process"; see
 *   doc/readme.isolation.  RELOAD keeps whichever the module had.
 *
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 * @return Zero.
 */
int mo_module(struct Client* cptr, struct Client* sptr, int parc, char* parv[])
{
  char* subcmd;

  if (!HasPriv(sptr, PRIV_MODULE))
    return send_reply(sptr, ERR_NOPRIVILEGES);

  if (parc < 2)
    return send_reply(sptr, ERR_NEEDMOREPARAMS, "MODULE");

  subcmd = parv[1];

  if (0 == ircd_strcmp(subcmd, "LIST")) {
    module_send_list(sptr);
    return 0;
  }

  /* MIGRATION gates itself: its LIST and STATUS only read, and an operator
   * should be able to see what is applied without CONFIG_OPERCMDS being on.
   */
  if (0 == ircd_strcmp(subcmd, "MIGRATION"))
    return module_migration(sptr, parc, parv);

  /* Everything past LIST changes what code the server is running, so it
   * goes through the same switch that gates the other configuration
   * commands.
   */
  if (!feature_bool(FEAT_CONFIG_OPERCMDS))
    return send_reply(sptr, ERR_DISABLED, "MODULE");

  if (parc < 3)
    return send_reply(sptr, ERR_NEEDMOREPARAMS, "MODULE");

  /* LOAD, UNLOAD and RELOAD are not this server's to do alone: every
   * server on the network runs the same modules, so the change is a
   * transaction over all of them and one failure anywhere abandons it
   * everywhere.  See include/module_sync.h.
   */
  if (0 == ircd_strcmp(subcmd, "LOAD")) {
    int iso = MODULE_NATIVE;

    /* MODULE LOAD <name> [native|process].  An operator loading by hand
     * says where it runs, because nothing else can: there is no Module{}
     * block for a module the configuration does not mention. */
    if (parc > 3) {
      if (0 == ircd_strcmp(parv[3], "process"))
        iso = MODULE_PROCESS;
      else if (0 != ircd_strcmp(parv[3], "native")) {
        sendcmdto_one(&me, CMD_NOTICE, sptr,
                      _(sptr, "%C :Isolation must be native or process"),
                      sptr);
        return 0;
      }
    }

    if (!modsync_begin(sptr, MODSYNC_LOAD, parv[2], iso))
      return 0;

    sendto_opmask_butone(0, SNO_OLDSNO, "%s is loading module %s on the whole "
                         "network", cli_name(sptr), parv[2]);
    log_write(LS_SYSTEM, L_INFO, 0, "%#C started a network-wide load of "
              "module %s", sptr, parv[2]);
    return 0;
  }

  if (0 == ircd_strcmp(subcmd, "UNLOAD")) {
    if (!modsync_begin(sptr, MODSYNC_UNLOAD, parv[2], MODULE_NATIVE))
      return 0;

    sendto_opmask_butone(0, SNO_OLDSNO, "%s is unloading module %s on the "
                         "whole network", cli_name(sptr), parv[2]);
    log_write(LS_SYSTEM, L_INFO, 0, "%#C started a network-wide unload of "
              "module %s", sptr, parv[2]);
    return 0;
  }

  if (0 == ircd_strcmp(subcmd, "RELOAD")) {
    /* Where it runs is not an argument here: each server keeps whichever
     * its own copy had, the way a single-server reload always did.
     */
    if (!modsync_begin(sptr, MODSYNC_RELOAD, parv[2], MODULE_NATIVE))
      return 0;

    sendto_opmask_butone(0, SNO_OLDSNO, "%s is reloading module %s on the "
                         "whole network", cli_name(sptr), parv[2]);
    log_write(LS_SYSTEM, L_INFO, 0, "%#C started a network-wide reload of "
              "module %s", sptr, parv[2]);
    return 0;
  }

  sendcmdto_one(&me, CMD_NOTICE, sptr,
                _(sptr, "%C :MODULE subcommand must be LIST, LOAD, UNLOAD, RELOAD "
                "or MIGRATION"), sptr);
  return 0;
}

/** Handle a MODULE command from an ordinary user.
 *
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 * @return Zero.
 */
int m_module(struct Client* cptr, struct Client* sptr, int parc, char* parv[])
{
  return send_reply(sptr, ERR_NOPRIVILEGES);
}

/** Handle a MODULE message from another server.
 *
 * The whole server-to-server side of MODULE is the module-set protocol:
 * the digest a link announces, and the two-phase commit behind a
 * network-wide load or unload.  It is all in ircd/module_sync.c, which
 * is where the state that goes with it lives; this is only the seam.
 *
 * @param[in] cptr Link the message arrived on.
 * @param[in] sptr Server that sent it.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 * @return Zero, or CPTR_KILLED if the link was refused.
 */
int ms_module(struct Client* cptr, struct Client* sptr, int parc, char* parv[])
{
  return modsync_recv(cptr, sptr, parc, parv);
}
