/*
 * IRC - Internet Relay Chat, modules/example_cmd.c
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
 * @brief A module that adds one command, as a reference for module authors.
 *
 * Load it with a Module block, or with /MODULE LOAD:
 *
 *   Module { file = "/opt/ircu/lib/ircu/modules/example_cmd.so"; };
 *
 * Then any user can run /HELLO and get a notice back.
 */
#include "config.h"

#include "client.h"
#include "ircd.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "module.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "send.h"

/** Handle HELLO from a local user.
 *
 * Handlers use the same signature as every command in the core; see
 * ircd/m_*.c for the conventions.
 *
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 * @return Zero.
 */
static int m_hello(struct Client* cptr, struct Client* sptr,
                   int parc, char* parv[])
{
  sendcmdto_one(&me, CMD_NOTICE, sptr,
                "%C :Hello from a loadable module. You sent %d parameter%s.",
                sptr, parc - 1, (parc - 1) == 1 ? "" : "s");
  return 0;
}

/** Register the command.
 * @param[in] mod Handle for this module.
 * @return Zero on success, non-zero to refuse to load.
 */
static int example_init(struct ModuleHandle* mod)
{
  MessageHandler handlers[LAST_HANDLER_TYPE];

  /* One handler per client type.  NULL becomes m_ignore, so this command
   * is available to registered users and opers but silently ignored from
   * unregistered clients and from servers.
   */
  handlers[UNREGISTERED_HANDLER] = NULL;
  handlers[CLIENT_HANDLER]       = m_hello;
  handlers[SERVER_HANDLER]       = NULL;
  handlers[OPER_HANDLER]         = m_hello;
  handlers[SERVICE_HANDLER]      = NULL;

  /* MAXPARA splits the line into individual parameters, the way core
   * commands do; a smaller number folds the tail of the line into one
   * trailing parameter instead.
   *
   * Refusing to load when the name is already taken is the right call: a
   * module whose command never arrives is worse than one that says so.
   */
  if (!module_add_command(mod, "HELLO", "HELLO", MAXPARA, MFLG_SLOW,
                          handlers))
    return -1;

  return 0;
}

/** Tear down.
 *
 * The loader removes the command whether or not this does, so a module with
 * nothing else to clean up may leave mi_fini as NULL entirely.
 *
 * @param[in] mod Handle for this module.
 */
static void example_fini(struct ModuleHandle* mod)
{
  module_del_command(mod, "HELLO");
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "example_cmd",
  "1.0.0",
  "ircu developers",
  "Adds a /HELLO command; reference for module authors",
  example_init,
  example_fini,
  NULL
};
