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
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "migration.h"
#include "module.h"
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

  for (mod = module_next(0); mod; mod = module_next(mod))
    /* The file name comes first: that is what LOAD, UNLOAD and RELOAD
     * take, and it need not match the name the module declares.
     *
     * The location is relative to the module directory, never the
     * absolute path: where the server keeps its files on the host is not
     * something to hand out over IRC, even to an operator.
     */
    send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
               ":Module %s (%s %s, ABI %u): modules/%s -- %s [loaded by %s]",
               module_file(mod), module_name(mod), module_version(mod),
               (unsigned int) IRCU_MODULE_ABI, module_relpath(mod),
               module_description(mod),
               module_loaded_by(mod) ? module_loaded_by(mod)
                                     : "the configuration file");

  send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG, ":%u module%s loaded",
             module_count(), module_count() == 1 ? "" : "s");
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
                  "%C :MODULE MIGRATION subcommand must be LIST, STATUS, "
                  "APPLY or REVERT", sptr);
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
                  "%C :MODULE MIGRATION %s needs a module name", sptr, what);
    return 0;
  }

  /* The server's own migrations are not an operator's to drive: they exist
   * to make the table the rest are recorded in, and the daemon applies them
   * when it starts.
   */
  if (migration_reserved_name(parv[3])) {
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  "%C :The server's own migrations run at start-up and are "
                  "not applied or reverted by hand", sptr);
    return 0;
  }

  if (!(mod = module_find(parv[3])) && !(mod = module_find_file(parv[3]))) {
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :No module named %s is loaded",
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
                  "%C :%s is not a version; migrations are numbered v1, v2 "
                  "and so on", sptr, parv[4]);
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
                "%C :MODULE MIGRATION subcommand must be LIST, STATUS, "
                "APPLY or REVERT", sptr);
  return 0;
}

/** Handle a MODULE command from an operator.
 *
 * parv[1] = subcommand: LIST, LOAD, UNLOAD, RELOAD or MIGRATION
 * parv[2] = module name; LOAD resolves it against the server's module
 *   directory, UNLOAD and RELOAD look it up among the loaded modules
 *
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 * @return Zero.
 */
int mo_module(struct Client* cptr, struct Client* sptr, int parc, char* parv[])
{
  struct ModuleHandle* mod;
  const char* err = 0;
  char* subcmd;
  char name[256];

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

  if (0 == ircd_strcmp(subcmd, "LOAD")) {
    if (!module_load(parv[2], cli_name(sptr), &err)) {
      sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Could not load %s: %s",
                    sptr, parv[2], err ? err : "unknown error");
      return 0;
    }

    sendto_opmask_butone(0, SNO_OLDSNO, "%s loaded module %s",
                         cli_name(sptr), parv[2]);
    log_write(LS_SYSTEM, L_INFO, 0, "%#C loaded module %s", sptr, parv[2]);
    return 0;
  }

  if (0 == ircd_strcmp(subcmd, "UNLOAD")) {
    if (!(mod = module_find_file(parv[2]))) {
      sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :No module named %s is loaded",
                    sptr, parv[2]);
      return 0;
    }

    if (!module_unload(mod)) {
      sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Could not unload %s",
                    sptr, parv[2]);
      return 0;
    }

    sendto_opmask_butone(0, SNO_OLDSNO, "%s unloaded module %s",
                         cli_name(sptr), parv[2]);
    log_write(LS_SYSTEM, L_INFO, 0, "%#C unloaded module %s", sptr, parv[2]);
    return 0;
  }

  if (0 == ircd_strcmp(subcmd, "RELOAD")) {
    if (!(mod = module_find_file(parv[2]))) {
      sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :No module named %s is loaded",
                    sptr, parv[2]);
      return 0;
    }

    /* module_unload() frees the handle, so keep the name before it goes. */
    ircd_strncpy(name, module_file(mod), sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    if (!module_unload(mod)) {
      sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :Could not unload %s",
                    sptr, parv[2]);
      return 0;
    }

    if (!module_load(name, cli_name(sptr), &err)) {
      /* The old code is already gone; say so plainly rather than leaving
       * the operator to guess whether the module is still running.
       */
      sendcmdto_one(&me, CMD_NOTICE, sptr,
                    "%C :Unloaded %s but could not load it again: %s",
                    sptr, parv[2], err ? err : "unknown error");
      sendto_opmask_butone(0, SNO_OLDSNO,
                           "Module %s is now unloaded: reload failed: %s",
                           parv[2], err ? err : "unknown error");
      log_write(LS_SYSTEM, L_ERROR, 0,
                "Reload of module %s failed, module is unloaded: %s",
                parv[2], err ? err : "unknown error");
      return 0;
    }

    sendto_opmask_butone(0, SNO_OLDSNO, "%s reloaded module %s",
                         cli_name(sptr), parv[2]);
    log_write(LS_SYSTEM, L_INFO, 0, "%#C reloaded module %s", sptr, parv[2]);
    return 0;
  }

  sendcmdto_one(&me, CMD_NOTICE, sptr,
                "%C :MODULE subcommand must be LIST, LOAD, UNLOAD, RELOAD "
                "or MIGRATION", sptr);
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
