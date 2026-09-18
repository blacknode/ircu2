/*
 * IRC - Internet Relay Chat, ircd/m_language.c
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
 * @brief The LANGUAGE command and the LG token.
 *
 * LANGUAGE follows the IRCv3 "draft/languages" negotiation as Ergo
 * implements it: a client lists up to I18N_PREF_MAX language codes, most
 * preferred first, and from then on everything the server says to that
 * client is looked up in those languages, then in DEFAULT_LANGUAGE, then
 * left as written.  It is accepted before registration, which is what
 * lets the welcome itself come out translated, and the CAP REQ is not
 * required: the capability only advertises.
 *
 * The preference travels the network as LG, with the user as source,
 * because a reply is translated where it is generated and that is not
 * always the user's own server: a remote WHOIS, a remote STATS, and every
 * answer from a service bot living on another server.  See
 * doc/readme.translations.
 */
#include "config.h"

#include "client.h"
#include "hash.h"
#include "ircd.h"
#include "ircd_i18n.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "send.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

/** Join \a parv[1..parc-1] with spaces into \a buf, for the 687 and 982
 * replies, which echo what the client asked for. */
static void join_params(char* buf, size_t len, int parc, char* parv[],
                        const int* keep)
{
  int i;
  size_t used = 0;

  buf[0] = '\0';
  for (i = 1; i < parc; i++) {
    size_t l;

    if (keep && !keep[i])
      continue;
    l = strlen(parv[i]);
    if (used + l + 2 > len)
      break;
    if (used)
      buf[used++] = ' ';
    memcpy(buf + used, parv[i], l + 1);
    used += l;
  }
}

/** Handle a LANGUAGE command from a client, registered or not.
 *
 * \a parv has the following elements:
 * \li \a parv[1..parc-1] are language codes, most preferred first.
 *
 * See @ref m_functions for discussion of the arguments.
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 */
int m_language(struct Client* cptr, struct Client* sptr, int parc,
               char* parv[])
{
  char list[BUFSIZE];
  int unknown[MAXPARA + 1];
  int i, nunknown = 0;

  assert(cptr == sptr);

  if (parc < 2 || !*parv[1])
    return need_more_params(sptr, "LANGUAGE");

  if (parc - 1 > I18N_PREF_MAX)
    return send_reply(sptr, ERR_TOOMANYLANGUAGES, (unsigned int)I18N_PREF_MAX);

  /* The 982 lists exactly the codes that are not available, and nothing
   * changes: a client that asked for "pt es" and got "pt" back knows what
   * to drop and try again with.
   */
  memset(unknown, 0, sizeof(unknown));
  for (i = 1; i < parc; i++) {
    if (!i18n_language_known(parv[i])) {
      unknown[i] = 1;
      nunknown++;
    }
  }
  if (nunknown) {
    join_params(list, sizeof(list), parc, parv, unknown);
    return send_reply(sptr, ERR_NOLANGUAGE, list);
  }

  i18n_set_languages(sptr, (const char* const*)parv + 1, parc - 1);

  /* Already in the language just chosen. */
  join_params(list, sizeof(list), parc, parv, 0);
  send_reply(sptr, RPL_YOURLANGUAGESARE, list);

  /* A registered user's change goes out now; an unregistered one's goes
   * out with its NICK, from register_user().
   */
  if (IsUser(sptr) && i18n_languages_str(sptr))
    sendcmdto_serv_butone(sptr, CMD_LANGUAGE, cptr, "%s",
                          i18n_languages_str(sptr));

  return 0;
}

/** Handle an LG token from a server.
 *
 * \a parv has the following elements:
 * \li \a parv[1..parc-1] are language codes, most preferred first, or
 *   nothing to clear the preference.
 *
 * The codes are stored as they arrive, without checking them against
 * this server's catalogs: a hub that has no "fr-ca" still has to pass it
 * on whole.  What does not have the syntax of a code is dropped with a
 * protocol violation; what exceeds I18N_PREF_MAX is cut.
 *
 * See @ref m_functions for discussion of the arguments.
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 */
int ms_language(struct Client* cptr, struct Client* sptr, int parc,
                char* parv[])
{
  const char* codes[I18N_PREF_MAX];
  unsigned int n = 0;
  int i, bad = 0;

  assert(IsServer(cptr));

  if (!IsUser(sptr))
    return protocol_violation(cptr, "LG from %s, which is not a user",
                              cli_name(sptr));

  for (i = 1; i < parc && n < I18N_PREF_MAX; i++) {
    if (!i18n_valid_code(parv[i])) {
      bad++;
      continue;
    }
    codes[n++] = parv[i];
  }
  if (bad)
    protocol_violation(cptr, "LG for %s with %d code%s that %s not a "
                       "language code", cli_name(sptr), bad, bad == 1 ? "" : "s",
                       bad == 1 ? "is" : "are");

  i18n_set_languages(sptr, codes, n);

  if (i18n_languages_str(sptr))
    sendcmdto_serv_butone(sptr, CMD_LANGUAGE, cptr, "%s",
                          i18n_languages_str(sptr));
  else
    sendcmdto_serv_butone(sptr, CMD_LANGUAGE, cptr, "");

  return 0;
}
