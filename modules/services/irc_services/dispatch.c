/*
 * IRC - Internet Relay Chat, modules/services/irc_services/dispatch.c
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
 * @brief From a line of text to a command call.
 *
 * A line is split on spaces into at most SVC_MAXARGS words, the first of
 * which names the command.  The command is looked up in the service's
 * own table and then in the table every service shares, so a service
 * may override HELP by defining its own.  What is refused is refused
 * here -- unknown, too few arguments, opers only, not from a channel --
 * so that a command's own code starts from a call it can trust.
 *
 * Everything here runs in the main thread, inside the hook that received
 * the message.  A command that has slow work to do -- a database lookup
 * -- hands it to db_query() or a worker and answers from the callback;
 * see doc/readme.services.
 */
#include "config.h"

#include "services.h"

#include "bot.h"
#include "channel.h"
#include "client.h"
#include "ircd.h"
#include "ircd_chattr.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_sha256.h"   /* ircd_crypto_wipe() */
#include "ircd_string.h"
#include "s_user.h"

#include <stdarg.h>
#include <string.h>

/** Look a name up in one command table.
 * @param[in] table Table ended by a NULL cmd_name.
 * @param[in] name Name to find, any case.
 * @return The row, or NULL.
 */
static const struct ServiceCommand* find_in(const struct ServiceCommand* table,
                                            const char* name)
{
  const struct ServiceCommand* c;

  for (c = table; c && c->cmd_name; c++)
    if (0 == ircd_strcmp(c->cmd_name, name))
      return c;

  return NULL;
}

const struct ServiceCommand* svc_find_command(const struct ServiceType* type,
                                              const char* name)
{
  const struct ServiceCommand* c;

  if ((c = find_in(type->st_commands, name)))
    return c;

  return find_in(svc_common_commands, name);
}

void svc_reply(const struct ServiceCall* call, const char* fmt, ...)
{
  char buf[BUFSIZE];
  va_list vl;

  if (!call->sc_service->sv_client)
    return;

  va_start(vl, fmt);
  ircd_vsnprintf(call->sc_source, buf, sizeof(buf), _(call->sc_source, fmt),
                 vl);
  va_end(vl);

  bot_send_user(call->sc_service->sv_client, call->sc_source, 1, buf);
}

/** Split \a line into words, in place.
 * @param[in,out] line Text to split; it is written to.
 * @param[out] argv Receives pointers into \a line.
 * @param[in] max Room in \a argv.
 * @return Number of words found.
 */
static int split_words(char* line, char* argv[], int max)
{
  int argc = 0;
  char* p = line;

  while (argc < max) {
    while (*p == ' ')
      p++;
    if (!*p)
      break;
    argv[argc++] = p;
    while (*p && *p != ' ')
      p++;
    if (!*p)
      break;
    *p++ = '\0';
  }

  return argc;
}

void svc_dispatch(struct Service* sv, struct Client* source,
                  struct Channel* chptr, int notice, const char* text)
{
  struct ServiceCall call;
  const struct ServiceCommand* cmd;
  char line[BUFSIZE];
  char* p;

  assert(0 != sv);
  assert(0 != source);
  assert(0 != text);

  /* A service talks to users.  Another service -- ours or another
   * server's -- gets no answer, so that two of them can never end up
   * answering each other's "unknown command" forever.
   */
  if (IsServiceBot(source) || IsChannelService(source))
    return;

  /* CTCP is not a command; a client that sends VERSION to every user it
   * sees should get nothing back from a service either.
   */
  if (*text == '\001')
    return;

  if (chptr) {
    if (*text != SVC_FANTASY_PREFIX)
      return;
    text++;
  }

  ircd_strncpy(line, text, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  memset(&call, 0, sizeof(call));
  call.sc_service = sv;
  call.sc_source = source;
  call.sc_channel = chptr;
  call.sc_notice = notice;
  call.sc_argc = split_words(line, call.sc_argv, SVC_MAXARGS);

  if (call.sc_argc == 0)
    goto done;

  for (p = call.sc_argv[0]; *p; p++)
    *p = ToUpper(*p);

  cmd = svc_find_command(sv->sv_type, call.sc_argv[0]);

  /* RFC 2812: nothing answers a NOTICE automatically.  A known command
   * sent as a NOTICE is still run -- the user asked for it -- but an
   * error is not an answer worth risking a loop over.
   */
  if (!cmd) {
    if (!notice && !chptr)
      svc_reply(&call, "Unknown command %s.  Type HELP for a list of "
                "commands.", call.sc_argv[0]);
    goto done;
  }

  if (chptr && !(cmd->cmd_flags & SVC_CMD_FANTASY))
    goto done;

  if ((cmd->cmd_flags & SVC_CMD_OPER) && !IsOper(source)) {
    if (!notice)
      svc_reply(&call, "Permission denied: %s is for IRC operators.",
                cmd->cmd_name);
    goto done;
  }

  if ((unsigned int) (call.sc_argc - 1) < cmd->cmd_min_args) {
    if (!notice)
      svc_reply(&call, "Not enough parameters.  Syntax: %s",
                _(source, cmd->cmd_syntax));
    goto done;
  }

  cmd->cmd_run(&call);

done:
  /* Some of these lines carry a password -- IDENTIFY does -- and this is
   * the copy this module made.  Every way out goes through here rather
   * than a flag on the rows that need it, so that a command added later
   * cannot forget.
   */
  ircd_crypto_wipe(line, sizeof(line));
}
