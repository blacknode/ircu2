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
#include "module.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "s_conf.h"
#include "s_debug.h"
#include "send.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
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
     */
    send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
               ":Module %s (%s %s, ABI %u): %s -- %s [loaded by %s]",
               module_file(mod), module_name(mod), module_version(mod),
               (unsigned int) IRCU_MODULE_ABI, module_path(mod),
               module_description(mod),
               module_loaded_by(mod) ? module_loaded_by(mod)
                                     : "the configuration file");

  send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG, ":%u module%s loaded",
             module_count(), module_count() == 1 ? "" : "s");
}

/** Handle a MODULE command from an operator.
 *
 * parv[1] = subcommand: LIST, LOAD, UNLOAD or RELOAD
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
                "%C :MODULE subcommand must be LIST, LOAD, UNLOAD or RELOAD",
                sptr);
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
