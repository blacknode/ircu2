/*
 * IRC - Internet Relay Chat, ircd/m_cap.c
 * Copyright (C) 2004 Kevin L. Mitchell <klmitch@mit.edu>
 *
 * See file AUTHORS in IRC package for additional names of
 * the programmers.
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
 * @brief Capability negotiation commands
 * @version $Id$
 */

#include "config.h"

#include "capab.h"
#include "client.h"
#include "ircd.h"
#include "ircd_chattr.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "msg.h"
#include "numeric.h"
#include "send.h"
#include "s_auth.h"
#include "s_user.h"
#include "s_bsd.h"

#include <stdlib.h>
#include <string.h>

typedef int (*bqcmp)(const void *, const void *);

/** Pull the next capability name off a CAP REQ list.
 *
 * The list is a space-separated series of names, each optionally prefixed
 * by '-' to ask for the capability to be dropped.  The name is looked up
 * in the register; an unknown one still advances the pointer, because the
 * caller has to be able to reach the end of a list that names something
 * this server does not have.
 *
 * @param[in,out] caplist_p List to walk; set to NULL at the end.
 * @param[out] neg_p Set to non-zero if the entry was negated.
 * @return The capability, or NULL if this entry names none.
 */
static const struct Capability *
find_cap(const char **caplist_p, int *neg_p)
{
  const char *caplist = *caplist_p;
  const struct Capability *cap = 0;
  char name[CAPNAMELEN + 1];
  size_t len = 0;

  *neg_p = 0; /* clear negative flag... */

  /* Next non-whitespace character... */
  while (*caplist && IsSpace(*caplist))
    caplist++;

  /* We are now at the beginning of an element of the list; is it negative? */
  if (*caplist == '-') {
    caplist++; /* yes; step past the flag... */
    *neg_p = 1; /* remember that it is negative... */
  }

  /* Copy out the name, stopping at whitespace.  An over-long name cannot
   * be one we have registered, so it is collected and discarded rather
   * than truncated into a false match.
   */
  while (*caplist && !IsSpace(*caplist)) {
    if (len < sizeof(name) - 1)
      name[len] = *caplist;
    len++;
    caplist++;
  }

  if (len > 0 && len < sizeof(name)) {
    name[len] = '\0';
    cap = cap_find(name);
  }

  assert(caplist != *caplist_p || !*caplist); /* we *must* advance */

  /* move ahead in capability list string--or zero pointer if we hit end */
  *caplist_p = *caplist ? caplist : 0;

  return cap; /* and return the capability (if any) */
}

/** Send a CAP \a subcmd list of capability changes to \a sptr.
 * If more than one line is necessary, each line before the last has
 * an added "*" parameter before that line's capability list.
 * @param[in] sptr Client receiving capability list.
 * @param[in] set Capabilities to show as set, or NULL for none given.
 * @param[in] rem Capabilities to show as removed, or NULL for none given.
 * @param[in] subcmd Name of capability subcommand.
 */
static int
send_caplist(struct Client *sptr, const capset_t *set,
             const capset_t *rem, const char *subcmd)
{
  char capbuf[BUFSIZE] = "", pfx[16];
  struct MsgBuf *mb;
  const struct Capability *cap;
  int loc, len, pfx_len;
  unsigned long flags;

  /* set up the buffer for the final LS message... */
  mb = msgq_make(sptr, "%:#C " MSG_CAP " %C %s :", &me, sptr, subcmd);

  /* If the client has no capabilities set, and this is the LIST subcmd,
   * there is nothing to walk.
   */
  if (set && cap_set_empty(set) && !strcmp(subcmd, "LIST"))
    cap = 0;
  else
    cap = cap_first();

  for (loc = 0; cap; cap = cap->cap_next) {
    const char *cap_value = "";
    int value_len;

    flags = cap->cap_flags;

    /* Check if the capability is enabled in features() */
    if (cap->cap_config != 0 && !feature_bool(cap->cap_config))
      continue;

    /* Check if capability is hidden from IRCv3.2 clients */
    if (!set && HasFlag(sptr, FLAG_CAP302) && (flags & CAPFL_HIDDEN_302))
      continue;

    /* This is a little bit subtle, but just involves applying de
     * Morgan's laws to the obvious check: We must display the
     * capability if (and only if) it is set in \a rem or \a set, or
     * if neither was given and the capability is not hidden.
     */
    if (!(rem && CapHas(rem, cap->cap_index))
        && !(set && CapHas(set, cap->cap_index))
        && (rem || set || (flags & CAPFL_HIDDEN)))
      continue;

    /* Build the prefix (space separator). */
    pfx_len = 0;
    if (loc)
      pfx[pfx_len++] = ' ';
    if (rem && CapHas(rem, cap->cap_index))
        pfx[pfx_len++] = '-';
    pfx[pfx_len] = '\0';

    /* Get capability value for LS command */
    if (!strcmp(subcmd, "LS")) {
      if (cap->cap_value[0] != '\0' && HasFlag(sptr, FLAG_CAP302)) {
        cap_value = cap->cap_value;
      }
    }

    /* Calculate length including value */
    value_len = (cap_value[0] != '\0') ? strlen(cap_value) + 1 : 0; /* +1 for = */
    len = strlen(cap->cap_name) + pfx_len + value_len; /* how much we'd add... */
    if (msgq_bufleft(mb) < loc + len + 2) { /* would add too much; must flush */
      sendcmdto_one(&me, CMD_CAP, sptr, "%C %s * :%s", sptr, subcmd, capbuf);
      capbuf[(loc = 0)] = '\0'; /* re-terminate the buffer... */
    }

    if (cap_value[0] != '\0') {
      loc += ircd_snprintf(0, capbuf + loc, sizeof(capbuf) - loc, "%s%s=%s",
			   pfx, cap->cap_name, cap_value);
    } else {
      loc += ircd_snprintf(0, capbuf + loc, sizeof(capbuf) - loc, "%s%s",
			   pfx, cap->cap_name);
    }
  }

  msgq_append(0, mb, "%s", capbuf); /* append capabilities to the final cmd */
  send_buffer(sptr, NULL, mb, 0, NULL, NULL); /* send them out... */
  msgq_clean(mb); /* and release the buffer */

  return 0; /* convenience return */
}

static int
cap_ls(struct Client *sptr, const char *caplist)
{
  if (IsUserPort(sptr) || IsWebsocketPort(sptr)) /* registration hasn't completed; suspend it... */
    auth_cap_start(cli_auth(sptr));
  
  /* Check if client supports IRCv3.2 (LS version >= 302) */
  if (caplist) {
    int version = atoi(caplist);
    if (version >= 302) {
      SetFlag(sptr, FLAG_CAP302);
      CapSet(cli_active(sptr), CAP_CAPNOTIFY);
    }
  }
  
  return send_caplist(sptr, 0, 0, "LS"); /* send list of capabilities */
}

static int
cap_req(struct Client *sptr, const char *caplist)
{
  const char *cl = caplist;
  const struct Capability *cap;
  capset_t set, rem;
  capset_t cs = *cli_capab(sptr); /* capability set */
  capset_t as = *cli_active(sptr); /* active set */
  int neg;

  CapClrAll(&set);
  CapClrAll(&rem);

  if (IsUserPort(sptr) || IsWebsocketPort(sptr)) /* registration hasn't completed; suspend it... */
    auth_cap_start(cli_auth(sptr));

  while (cl) { /* walk through the capabilities list... */
    /* Skip separators; stop cleanly on trailing whitespace (do not NAK). */
    while (*cl && IsSpace(*cl))
      cl++;
    if (!*cl)
      break;

    if (!(cap = find_cap(&cl, &neg)) /* look up capability... */
        || (cap->cap_config != 0 && !feature_bool(cap->cap_config)) /* is it deactivated in config? */
        || (!neg && (cap->cap_flags & CAPFL_PROHIBIT)) /* is it prohibited? */
        || (neg && (cap->cap_flags & CAPFL_STICKY)) /* is it sticky? */
        || (neg && HasFlag(sptr, FLAG_CAP302) && (cap->cap_flags & CAPFL_STICKY_302))) { /* is it sticky for IRCv3.2? */
      sendcmdto_one(&me, CMD_CAP, sptr, "%C NAK :%s", sptr, caplist);
      return 0; /* can't complete requested op... */
    }

    if (neg) { /* set or clear the capability... */
      CapSet(&rem, cap->cap_index);
      CapClr(&set, cap->cap_index);
      CapClr(&cs, cap->cap_index);
      if (!(cap->cap_flags & CAPFL_PROTO))
	      CapClr(&as, cap->cap_index);
    } else {
      CapClr(&rem, cap->cap_index);
      CapSet(&set, cap->cap_index);
      CapSet(&cs, cap->cap_index);
      if (!(cap->cap_flags & CAPFL_PROTO))
	      CapSet(&as, cap->cap_index);
    }
  }

  /* Notify client of accepted changes and copy over results. */
  send_caplist(sptr, &set, &rem, "ACK");
  *cli_capab(sptr) = cs;
  *cli_active(sptr) = as;

  return 0;
}

static int
cap_end(struct Client *sptr, const char *caplist)
{
  if (!IsUserPort(sptr) && !IsWebsocketPort(sptr)) /* registration has completed... */
    return 0; /* so just ignore the message... */

  return auth_cap_done(cli_auth(sptr));
}

static int
cap_list(struct Client *sptr, const char *caplist)
{
  /* Send the list of the client's capabilities */
  return send_caplist(sptr, cli_capab(sptr), 0, "LIST");
}

/* Must stay sorted by cmd for bsearch() in m_cap(). */
static struct subcmd {
  char *cmd;
  int (*proc)(struct Client *sptr, const char *caplist);
} cmdlist[] = {
  { "ACK",   0         },
  { "DEL",   0         },
  { "END",   cap_end   },
  { "LIST",  cap_list  },
  { "LS",    cap_ls    },
  { "NAK",   0         },
  { "NEW",   0         },
  { "REQ",   cap_req   }
};

/** Announce a new capability with CAP NEW.
 *
 * Sent to every local user that asked to hear about capability changes.
 * Called when a module registers one on a running server, and when
 * something that was unavailable becomes available again.
 * @param[in] cap Position of the capability.
 */
void cap_new(int cap)
{
  const struct Capability *c = cap_find_index(cap);
  struct Client *acptr;
  int i;

  if (!c)
    return;

  /* Check if the capability should be advertised */
  if (c->cap_config != 0 && !feature_bool(c->cap_config))
    return;
  if (c->cap_flags & CAPFL_HIDDEN)
    return;

  for (i = 0; i <= HighestFd; i++) {
    if (!(acptr = LocalClientArray[i]))
      continue;

    /* Only send to registered users with cap-notify capability */
    if (!IsUser(acptr) || !MyConnect(acptr)
        || !CapHas(cli_active(acptr), CAP_CAPNOTIFY))
      continue;

    if (c->cap_value[0] && HasFlag(acptr, FLAG_CAP302)) {
      sendcmdto_one(&me, CMD_CAP, acptr, "%C NEW %s=%s", acptr, c->cap_name,
                    c->cap_value);
    } else {
      sendcmdto_one(&me, CMD_CAP, acptr, "%C NEW %s", acptr, c->cap_name);
    }
  }
}

/** Announce a capability going away with CAP DEL.
 *
 * Every local client loses it, whether or not it asked to be told: leaving
 * a client believing a capability is in force when nothing implements it
 * any more is worse than an unannounced change.  The clients that did ask
 * are told; the rest simply stop having it.
 * @param[in] cap Position of the capability.
 */
void cap_del(int cap)
{
  const struct Capability *c = cap_find_index(cap);
  struct Client *acptr;
  int i;

  if (!c)
    return;

  for (i = 0; i <= HighestFd; i++) {
    if (!(acptr = LocalClientArray[i]))
      continue;
    if (!MyConnect(acptr))
      continue;

    if (IsUser(acptr) && CapHas(cli_active(acptr), CAP_CAPNOTIFY))
      sendcmdto_one(&me, CMD_CAP, acptr, "%C DEL :%s", acptr, c->cap_name);

    /* Disable the capability for this client. */
    CapClr(cli_active(acptr), c->cap_index);
    CapClr(cli_capab(acptr), c->cap_index);
  }
}

static int
subcmd_search(const char *cmd, const struct subcmd *elem)
{
  return ircd_strcmp(cmd, elem->cmd);
}

/** Handle a capability request or response from a client.
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 * @see \ref m_functions
 */
int
m_cap(struct Client* cptr, struct Client* sptr, int parc, char* parv[])
{
  char *subcmd, *caplist = 0;
  struct subcmd *cmd;

  if (parc < 2) /* a subcommand is required */
    return 0;
  subcmd = parv[1];
  if (parc > 2) /* a capability list was provided */
    caplist = parv[2];

  /* find the subcommand handler */
  if (!(cmd = (struct subcmd *)bsearch(subcmd, cmdlist,
				       sizeof(cmdlist) / sizeof(struct subcmd),
				       sizeof(struct subcmd),
				       (bqcmp)subcmd_search)))
    return send_reply(sptr, ERR_UNKNOWNCAPCMD, subcmd);

  /* then execute it... */
  return cmd->proc ? (cmd->proc)(sptr, caplist) : 0;
}
