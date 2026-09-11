/*
 * IRC - Internet Relay Chat, ircd/client.c
 * Copyright (C) 1990 Darren Reed
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
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
 * @brief Implementation of functions for handling local clients.
 * @version $Id$
 */
#include "config.h"

#include "class.h"
#include "client.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "ircd_tls.h"
#include "list.h"
#include "msgq.h"
#include "numeric.h"
#include "s_conf.h"
#include "s_debug.h"
#include "s_user.h"
#include "send.h"
#include "struct.h"
#include "user_flags.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <stddef.h>
#include <string.h>

/** Core user modes.  #UserModeList is seeded from this table; only
 * modes registered on top of it can be removed again.
 */
static const struct UserDefaultMode {
  flag_t flag; /**< User mode constant. */
  char c;      /**< Character corresponding to the mode. */
} userModeList[] = {{FLAG_OPER, 'o'},       {FLAG_LOCOP, 'O'},
                    {FLAG_INVISIBLE, 'i'},  {FLAG_WALLOP, 'w'},
                    {FLAG_SERVNOTICE, 's'}, {FLAG_DEAF, 'd'},
                    {FLAG_CHSERV, 'k'},     {FLAG_DEBUG, 'g'},
                    {FLAG_ACCOUNT, 'r'},    {FLAG_BLOCK_UNAUTH_USERS, 'R'},
                    {FLAG_HIDDENHOST, 'x'}, {FLAG_TLS, 'z'},
                    {FLAG_HIDEIDLE, 'I'},   {FLAG_COMMONCHANS, 'c'}};

/** Length of #userModeList. */
#define USERMODELIST_SIZE sizeof(userModeList) / sizeof(struct UserDefaultMode)

/** Find the shortest non-zero ping time attached to a client.
 * If all attached ping times are zero, return the value for
 * FEAT_PINGFREQUENCY.
 * @param[in] acptr Client to find ping time for.
 * @return Ping time in seconds.
 */
int client_get_ping(const struct Client *acptr) {
  int ping = 0;
  struct ConfItem *aconf;
  struct SLink *link;

  assert(cli_verify(acptr));

  for (link = cli_confs(acptr); link; link = link->next) {
    aconf = link->value.aconf;
    if (aconf->status & (CONF_CLIENT | CONF_SERVER)) {
      int tmp = get_conf_ping(aconf);
      if (0 < tmp && (ping > tmp || !ping))
        ping = tmp;
    }
  }
  if (0 == ping)
    ping = feature_int(FEAT_PINGFREQUENCY);

  Debug((DEBUG_DEBUG, "Client %s Ping %d", cli_name(acptr), ping));

  return ping;
}

/** Find the default usermode for a client.
 * @param[in] sptr Client to find default usermode for.
 * @return Pointer to usermode string (or NULL, if there is no default).
 */
const char *client_get_default_umode(const struct Client *sptr) {
  struct ConfItem *aconf;
  struct SLink *link;

  assert(cli_verify(sptr));

  for (link = cli_confs(sptr); link; link = link->next) {
    aconf = link->value.aconf;
    if ((aconf->status & CONF_CLIENT) && ConfUmode(aconf))
      return ConfUmode(aconf);
  }
  return NULL;
}

/** Remove a connection from the list of connections with queued data.
 * @param[in] con Connection with no queued data.
 */
void client_drop_sendq(struct Connection *con) {
  if (con_prev_p(con)) { /* on the queued data list... */
    if (con_next(con))
      con_prev_p(con_next(con)) = con_prev_p(con);
    *(con_prev_p(con)) = con_next(con);

    con_next(con) = 0;
    con_prev_p(con) = 0;
  }
}

/** Add a connection to the list of connections with queued data.
 * @param[in] con Connection with queued data.
 * @param[in,out] con_p Previous pointer to next connection.
 */
void client_add_sendq(struct Connection *con, struct Connection **con_p) {
  if (!con_prev_p(con)) { /* not on the queued data list yet... */
    con_prev_p(con) = con_p;
    con_next(con) = *con_p;

    if (*con_p)
      con_prev_p(*con_p) = &(con_next(con));
    *con_p = con;
  }
}

/** Default privilege set for global operators. */
static struct Privs privs_global;
/** Default privilege set for local operators. */
static struct Privs privs_local;
/** Non-zero if #privs_global and #privs_local have been initialized. */
static int privs_defaults_set;

/** Set the privileges for a client.
 * @param[in] client Client who has become an operator.
 * @param[in] oper Configuration item describing oper's privileges.
 * @param[in] forceOper Make the user into an operator even if \a oper
 *   is null.
 */
void client_set_privs(struct Client *client, struct ConfItem *oper,
                      int forceOper) {
  struct Privs *source, *defaults;
  struct ConnectionClass *class;
  enum Priv priv;

  if (!MyConnect(client))
    return;

  /* Clear out client's privileges. */
  memset(cli_privs(client), 0, sizeof(struct Privs));

  if (!IsAnOper(client) || (!oper && !forceOper))
    return;

  if (!privs_defaults_set) {
    memset(&privs_global, -1, sizeof(privs_global));
    FlagClr(&privs_global, PRIV_WALK_LCHAN);
    FlagClr(&privs_global, PRIV_SET);
    FlagClr(&privs_global, PRIV_BADCHAN);
    FlagClr(&privs_global, PRIV_LOCAL_BADCHAN);
    FlagClr(&privs_global, PRIV_APASS_OPMODE);

    memset(&privs_local, 0, sizeof(privs_local));
    FlagSet(&privs_local, PRIV_CHAN_LIMIT);
    FlagSet(&privs_local, PRIV_MODE_LCHAN);
    FlagSet(&privs_local, PRIV_SHOW_INVIS);
    FlagSet(&privs_local, PRIV_SHOW_ALL_INVIS);
    FlagSet(&privs_local, PRIV_LOCAL_KILL);
    FlagSet(&privs_local, PRIV_REHASH);
    FlagSet(&privs_local, PRIV_LOCAL_GLINE);
    FlagSet(&privs_local, PRIV_LOCAL_JUPE);
    FlagSet(&privs_local, PRIV_LOCAL_OPMODE);
    FlagSet(&privs_local, PRIV_WHOX);
    FlagSet(&privs_local, PRIV_DISPLAY);
    FlagSet(&privs_local, PRIV_FORCE_LOCAL_OPMODE);

    privs_defaults_set = 1;
  }

  /* Should we look up the default remote oper block? */
  if (oper) {
    class = oper->conn_class;
  } else {
    class = find_remote_oper_class();
  }

  /* Decide whether to use global or local oper defaults. */
  if (oper && FlagHas(&oper->privs_dirty, PRIV_PROPAGATE))
    defaults =
        FlagHas(&oper->privs, PRIV_PROPAGATE) ? &privs_global : &privs_local;
  else if (!class || FlagHas(&class->privs_dirty, PRIV_PROPAGATE))
    defaults = (!class || FlagHas(&class->privs, PRIV_PROPAGATE))
                   ? &privs_global
                   : &privs_local;
  else {
    assert(0 && "Oper has no propagation and neither does connection class");
    return;
  }

  /* For each feature, figure out whether it comes from the operator
   * conf, the connection class conf, or the defaults, then apply it.
   */
  for (priv = 0; priv < PRIV_LAST_PRIV; ++priv) {
    /* Figure out most applicable definition for the privilege. */
    if (oper && FlagHas(&oper->privs_dirty, priv))
      source = &oper->privs;
    else if (class && FlagHas(&class->privs_dirty, priv))
      source = &class->privs;
    else
      source = defaults;

    /* Set it if necessary (privileges were already cleared). */
    if (FlagHas(source, priv))
      SetPriv(client, priv);
  }

  /* This should be handled in the config, but lets be sure... */
  if (HasPriv(client, PRIV_PROPAGATE)) {
    /* force propagating opers to display */
    SetPriv(client, PRIV_DISPLAY);
  } else {
    /* if they don't propagate oper status, prevent desyncs */
    ClrPriv(client, PRIV_KILL);
    ClrPriv(client, PRIV_GLINE);
    ClrPriv(client, PRIV_JUPE);
    ClrPriv(client, PRIV_OPMODE);
    ClrPriv(client, PRIV_BADCHAN);
  }
}

/** Array mapping privilege values to names and vice versa. */
static struct {
  char *name;        /**< Name of privilege. */
  unsigned int priv; /**< Enumeration value of privilege */
} privtab[] = {
/** Helper macro to define an array entry for a privilege. */
#define P(priv) {#priv, PRIV_##priv}
    P(CHAN_LIMIT),
    P(MODE_LCHAN),
    P(WALK_LCHAN),
    P(DEOP_LCHAN),
    P(SHOW_INVIS),
    P(SHOW_ALL_INVIS),
    P(UNLIMIT_QUERY),
    P(KILL),
    P(LOCAL_KILL),
    P(REHASH),
    P(RESTART),
    P(DIE),
    P(GLINE),
    P(LOCAL_GLINE),
    P(JUPE),
    P(LOCAL_JUPE),
    P(OPMODE),
    P(LOCAL_OPMODE),
    P(SET),
    P(WHOX),
    P(BADCHAN),
    P(LOCAL_BADCHAN),
    P(SEE_CHAN),
    P(PROPAGATE),
    P(DISPLAY),
    P(SEE_OPERS),
    P(WIDE_GLINE),
    P(LIST_CHAN),
    P(FORCE_OPMODE),
    P(FORCE_LOCAL_OPMODE),
    P(APASS_OPMODE),
    P(MODULE),
#undef P
    {0, 0}};

/** Report privileges of \a client to \a to.
 * @param[in] to Client requesting privilege list.
 * @param[in] client Client whos privileges should be listed.
 * @return Zero.
 */
int client_report_privs(struct Client *to, struct Client *client) {
  struct MsgBuf *mb;
  int found1 = 0;
  int i;

  mb = msgq_make(to, rpl_str(RPL_PRIVS), cli_name(&me), cli_name(to),
                 cli_name(client));

  for (i = 0; privtab[i].name; i++)
    if (HasPriv(client, privtab[i].priv))
      msgq_append(0, mb, "%s%s", found1++ ? " " : "", privtab[i].name);

  send_buffer(to, NULL, mb, 0, NULL, NULL); /* send response */
  msgq_clean(mb);

  return 0;
}

/** Head of the list of registered user modes.  Private on purpose: the
 * only way in is client_append_user_mode(), the only way out is
 * client_remove_user_mode(), and modules see it through
 * client_user_modes() as a const list.
 */
static struct UserMode *UserModeList;

/** Return a read-only view of the registered user modes.
 * @return Head of the user mode list.
 */
const struct UserMode *client_user_modes(void) {
  return UserModeList;
}

void client_init_user_modes(void) {
  if (UserModeList)
    return;

  struct UserMode **ptr = &UserModeList;
  size_t i;
  for (i = 0; i < USERMODELIST_SIZE; i++) {
    struct UserMode *m = (struct UserMode *)MyMalloc(sizeof(struct UserMode));
    if (!m) {
      server_panic("cannot allocate struct UserMode*");
    }
    m->c = userModeList[i].c;
    m->flag = userModeList[i].flag;
    m->count = 0;
    m->next = NULL;
    *ptr = m;
    ptr = &m->next;
  }
  if (UserModeList)
    UserModeList->count = i;
}

/** Find a registered user mode by its character.
 * @param[in] c Mode character.
 * @return The mode, or NULL if no mode uses that character.
 */
const struct UserMode *client_find_user_mode(char c) {
  const struct UserMode *p;

  for (p = UserModeList; p; p = p->next)
    if (p->c == c)
      return p;

  return NULL;
}

/** Find a user mode bit that no registered mode is using.
 *
 * Modules do not pick their own bit: two modules that both picked, say,
 * bit 3 would silently share one flag, and neither author would ever see
 * the other's module.  The server hands out the bits instead, and a bit
 * freed by client_remove_user_mode() is handed out again.
 *
 * @return A free bit, or zero when every bit is taken.
 */
flag_t client_alloc_user_mode_flag(void) {
  unsigned int bit;

  for (bit = 0; bit < sizeof(flag_t) * 8; bit++) {
    flag_t flag = BITSET << bit;
    const struct UserMode *p;

    for (p = UserModeList; p; p = p->next)
      if (p->flag & flag)
        break;

    if (!p)
      return flag;
  }

  return 0;
}

/** Build the list of registered user mode characters.
 *
 * RPL_MYINFO advertises the modes this server understands, which is not a
 * constant any more: a module that registers a mode has to appear there
 * too, or clients are told the mode does not exist.
 *
 * @return Pointer to a static buffer, valid until the next call.
 */
const char *client_user_mode_chars(void) {
  static char buf[USERMODE_CHARS_LEN];
  const struct UserMode *p;
  size_t len = 0;

  for (p = UserModeList; p && len + 1 < sizeof(buf); p = p->next)
    buf[len++] = p->c;
  buf[len] = '\0';

  return buf;
}

/** Check whether a user mode can still be registered.
 * @param[in] c Mode character.
 * @param[in] flag Mode flag bit.
 * @return Zero if the mode is free, UMODE_INVALID_MODE or
 *   UMODE_ALREADY_EXISTS otherwise.
 */
int client_check_user_mode(char c, flag_t flag) {
  const struct UserMode *p;

  if (!UmodeCharInRange(c) || 0 == flag)
    return UMODE_INVALID_MODE;

  /* Both the character and the bit must be free: two modes sharing a bit
   * would silently overwrite each other in cli_uflags().
   */
  for (p = UserModeList; p; p = p->next)
    if (p->c == c || (p->flag & flag))
      return UMODE_ALREADY_EXISTS;

  return 0;
}

/** Register a new user mode.
 * The node is allocated and owned by the server, so that a module can be
 * unloaded without leaving the list pointing into its address space.
 * @param[in] c Mode character.
 * @param[in] flag Mode flag bit.
 * @return UMODE_APPEND_OK on success, UMODE_INVALID_MODE or
 *   UMODE_ALREADY_EXISTS on failure.
 */
int client_append_user_mode(char c, flag_t flag) {
  struct UserMode **ptr;
  struct UserMode *m;
  int check;

  check = client_check_user_mode(c, flag);
  if (check)
    return check;

  m = (struct UserMode *)MyMalloc(sizeof(struct UserMode));
  m->c = c;
  m->flag = flag;
  m->count = 0;
  m->next = NULL;

  for (ptr = &UserModeList; *ptr; ptr = &(*ptr)->next)
    ;
  *ptr = m;
  UserModeList->count++;

  return UMODE_APPEND_OK;
}

/** Remove a dynamically registered user mode.
 * Core modes (those listed in #userModeList) cannot be removed.  Every
 * client still carrying the mode loses it, and the change is announced
 * as a regular "-<c>" mode change so that neither the users nor the
 * rest of the network are left believing the mode is still set; the bit
 * can then safely be handed out again.
 * @param[in] c Mode character to remove.
 * @return UMODE_REMOVE_OK on success, UMODE_INVALID_MODE,
 *   UMODE_CORE_MODE or UMODE_UNKNOWN_MODE on failure.
 */
int client_remove_user_mode(char c) {
  struct UserMode **ptr;
  struct UserMode *m;
  struct Client *acptr;
  flag_t old;
  size_t i;

  if (!UmodeCharInRange(c))
    return UMODE_INVALID_MODE;

  for (i = 0; i < USERMODELIST_SIZE; i++)
    if (userModeList[i].c == c)
      return UMODE_CORE_MODE;

  for (ptr = &UserModeList; *ptr; ptr = &(*ptr)->next)
    if ((*ptr)->c == c)
      break;

  if (!*ptr)
    return UMODE_UNKNOWN_MODE;

  m = *ptr;

  /* Announce the loss while the mode is still registered: send_umode()
   * renders the change by walking this very list, and would emit
   * nothing at all once the node is unlinked.
   */
  for (acptr = GlobalClientList; acptr; acptr = cli_next(acptr)) {
    if (!IsUser(acptr) || !HasUFlag(acptr, m->flag))
      continue;

    old = cli_uflags(acptr);
    ClrUFlag(acptr, m->flag);

    /* Only announce our own users: every other server runs this same
     * path for the clients it is responsible for, and announcing remote
     * users here would send the network one MODE per user per server.
     * "Our own" is decided by server, not by connection: a client this
     * server introduced on its own behalf (modules/commands/m_bot.c) has no
     * connection to be MyUser() through, and nobody else will speak for
     * it.
     */
    if (MyUser(acptr) || (cli_user(acptr) && cli_user(acptr)->server == &me))
      send_umode_out(acptr, acptr, old, HasPriv(acptr, PRIV_PROPAGATE));
  }

  *ptr = m->next;
  MyFree(m);

  if (UserModeList)
    UserModeList->count--;

  return UMODE_REMOVE_OK;
}
