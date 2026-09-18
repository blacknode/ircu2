/*
 * IRC - Internet Relay Chat, ircd/chan_modes.c
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
 * @brief The register of channel modes.
 *
 * Everything that renders or parses a channel mode reads this list rather
 * than carrying its own copy of the letters, which is what lets a module
 * add one: channel.c walks the register, and the register is what
 * module_add_chan_mode() appends to.
 *
 * It lives apart from channel.c because it is a small, self-contained
 * thing with a test of its own, and because the module loader needs it
 * without needing the rest of the channel code.
 */
#include "config.h"

#include "channel.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_log.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <stdlib.h>
#include <string.h>

/** The channel modes the server implements by itself.
 *
 * The bit is not written here: it follows from the letter (see
 * chan_flags.h), so the only thing this table carries beyond the letter is
 * what the mode *is* -- whether it takes an argument, whether it belongs
 * to a member, whether a user may set it at all.  Everything that renders
 * or parses modes reads those attributes instead of keeping its own copy
 * of the exceptions.
 */
static const struct ChanDefaultMode {
  chanmode_t   flag; /**< Mode flag, from chan_flags.h. */
  char         c;    /**< Character corresponding to the mode. */
  char         alt;  /**< Character used towards servers, or zero. */
  unsigned int attr; /**< Bitwise combination of CHANMODE_* attributes. */
} chanModeList[] = {
  {MODE_APASS,         'A', 0,   CHANMODE_PARAM | CHANMODE_OPLEVELS},
  {MODE_NOCTCP,        'C', 0,   0},
  {MODE_DELJOINS,      'D', 0,   0},
  {MODE_MODERATENOREG, 'M', 0,   0},
  {MODE_REGISTERED,    'R', 0,   CHANMODE_SERVERONLY},
  {MODE_UPASS,         'U', 0,   CHANMODE_PARAM | CHANMODE_OPLEVELS},
  {MODE_TLSONLY,       'Z', 0,   0},
  {MODE_BAN,           'b', 0,   CHANMODE_PARAM | CHANMODE_LIST},
  {MODE_NOCOLOR,       'c', 0,   0},
  {MODE_WASDELJOINS,   'd', 0,   CHANMODE_LOCAL | CHANMODE_INTERNAL},
  {MODE_INVITEONLY,    'i', 0,   0},
  {MODE_KEY,           'k', 0,   CHANMODE_PARAM},
  {MODE_LIMIT,         'l', 0,   CHANMODE_PARAM | CHANMODE_PARAM_SET},
  {MODE_MODERATED,     'm', 0,   0},
  {MODE_NOPRIVMSGS,    'n', 0,   0},
  {MODE_CHANOP,        'o', 0,   CHANMODE_PARAM | CHANMODE_MEMBER},
  {MODE_PRIVATE,       'p', 0,   0},
  {MODE_REGONLY,       'r', 0,   0},
  {MODE_SECRET,        's', 0,   0},
  {MODE_TOPICLIMIT,    't', 0,   0},
  {MODE_NOPARTMSGS,    'u', 0,   0},
  {MODE_VOICE,         'v', 0,   CHANMODE_PARAM | CHANMODE_MEMBER},
  /* +z is derived from +Z, never asked for, and goes out to servers as
   * 'Z'; it is the one mode whose letter depends on who is reading.
   */
  {MODE_TLSINSECURE,   'z', 'Z', CHANMODE_LOCAL | CHANMODE_INTERNAL |
                                CHANMODE_HIDDEN}
};

/** Length of #chanModeList. */
#define CHANMODELIST_SIZE (sizeof(chanModeList) / sizeof(struct ChanDefaultMode))

/** Head of the list of registered channel modes.  Private on purpose: the
 * only way in is channel_append_chan_mode(), the only way out is
 * channel_remove_chan_mode(), and modules see it through
 * channel_chan_modes() as a const list.
 */
static struct ChanMode *ChanModeList;

/** Return a read-only view of the registered channel modes.
 * @return Head of the channel mode list.
 */
const struct ChanMode *channel_chan_modes(void)
{
  return ChanModeList;
}

/** Bit a mode letter maps to.
 *
 * Nothing is allocated and nothing is consulted: the letter *is* the bit.
 * Two servers built from the same sources therefore agree on every mode
 * bit without exchanging a word about it, and a module's mode lands on the
 * same bit on every server that loads the module.
 *
 * @param[in] c Mode character.
 * @return The bit, or zero if \a c is not a mode letter.
 */
chanmode_t channel_chan_mode_flag(char c)
{
  if (!ChanModeCharInRange(c))
    return 0;

  return BITSET << ChanModeBit(c);
}

/** Link a mode into the register, keeping it ordered by bit.
 *
 * The order is the letters' own ('A' to 'Z', then 'a' to 'z'), which is
 * what makes a rendered mode string identical on every server: the order
 * cannot depend on which module was loaded first.
 *
 * @param[in] m Mode to link in; ownership passes to the register.
 */
static void chan_mode_link(struct ChanMode *m)
{
  struct ChanMode **ptr;
  unsigned int count;

  for (ptr = &ChanModeList; *ptr && (*ptr)->flag < m->flag; ptr = &(*ptr)->next)
    ;

  m->next = *ptr;
  *ptr = m;

  /* The count lives on the head, which may be the node just linked in. */
  for (count = 0, m = ChanModeList; m; m = m->next)
    count++;
  ChanModeList->count = count;
}

/** Register the modes the server implements by itself. */
void channel_init_chan_modes(void)
{
  size_t i;

  if (ChanModeList)
    return;

  for (i = 0; i < CHANMODELIST_SIZE; i++) {
    struct ChanMode *m = (struct ChanMode *)MyMalloc(sizeof(struct ChanMode));

    if (!m)
      server_panic("cannot allocate struct ChanMode*");

    m->flag = chanModeList[i].flag;
    m->c = chanModeList[i].c;
    m->alt = chanModeList[i].alt;
    m->attr = chanModeList[i].attr;
    m->count = 0;
    m->next = NULL;

    assert(m->flag == channel_chan_mode_flag(m->c));

    chan_mode_link(m);
  }
}

/** Find a registered channel mode by its character.
 * @param[in] c Mode character.
 * @return The mode, or NULL if no mode uses that character.
 */
const struct ChanMode *channel_find_chan_mode(char c)
{
  const struct ChanMode *p;

  for (p = ChanModeList; p; p = p->next)
    if (p->c == c)
      return p;

  return NULL;
}

/** Check whether a channel mode can still be registered.
 * @param[in] c Mode character.
 * @param[in] flag Mode flag bit.
 * @return Zero if the mode is free, CMODE_INVALID_MODE or
 *   CMODE_ALREADY_EXISTS otherwise.
 */
int channel_check_chan_mode(char c, chanmode_t flag)
{
  /* The bit follows from the letter, so a flag that does not match the
   * letter is a caller that computed it some other way -- refuse it
   * rather than register a mode nothing can render.
   */
  if (!ChanModeCharInRange(c) || 0 == flag ||
      flag != channel_chan_mode_flag(c) || (flag & CHANMODE_RESERVED))
    return CMODE_INVALID_MODE;

  if (channel_find_chan_mode(c))
    return CMODE_ALREADY_EXISTS;

  return 0;
}

/** Register a new channel mode.
 * The node is allocated and owned by the server, so that a module can be
 * unloaded without leaving the register pointing into its address space.
 * @param[in] c Mode character.
 * @param[in] flag Mode flag bit, as channel_chan_mode_flag() computes it.
 * @return CMODE_APPEND_OK on success, CMODE_INVALID_MODE or
 *   CMODE_ALREADY_EXISTS on failure.
 */
int channel_append_chan_mode(char c, chanmode_t flag)
{
  struct ChanMode *m;
  int check;

  check = channel_check_chan_mode(c, flag);
  if (check)
    return check;

  m = (struct ChanMode *)MyMalloc(sizeof(struct ChanMode));
  m->flag = flag;
  m->c = c;
  m->alt = 0;
  m->attr = 0;
  m->count = 0;
  m->next = NULL;

  chan_mode_link(m);

  return CMODE_APPEND_OK;
}

/** Remove a dynamically registered channel mode.
 *
 * Core modes (those listed in #chanModeList) cannot be removed.  Every
 * channel still carrying the mode loses it, and the change goes out as an
 * ordinary "-<c>" to the members and to the rest of the network, so that
 * nobody is left believing a channel enforces a policy nothing implements
 * any more.  A channel is the network's, not this server's: leaving the
 * mode standing elsewhere would be worse than losing it everywhere.
 *
 * @param[in] c Mode character to remove.
 * @return CMODE_REMOVE_OK on success, CMODE_INVALID_MODE, CMODE_CORE_MODE
 *   or CMODE_UNKNOWN_MODE on failure.
 */
int channel_remove_chan_mode(char c)
{
  struct ChanMode **ptr;
  struct ChanMode *m;
  struct Channel *chptr;
  size_t i;

  if (!ChanModeCharInRange(c))
    return CMODE_INVALID_MODE;

  for (i = 0; i < CHANMODELIST_SIZE; i++)
    if (chanModeList[i].c == c)
      return CMODE_CORE_MODE;

  for (ptr = &ChanModeList; *ptr; ptr = &(*ptr)->next)
    if ((*ptr)->c == c)
      break;

  if (!*ptr)
    return CMODE_UNKNOWN_MODE;

  m = *ptr;

  /* Announce the loss while the mode is still registered: modebuf_flush()
   * renders the change by walking this very list, and would emit nothing
   * at all once the node is unlinked.
   */
  for (chptr = GlobalChannelList; chptr; chptr = chptr->next) {
    struct ModeBuf mbuf;

    if (!HasCFlag(chptr, m->flag))
      continue;

    modebuf_init(&mbuf, &me, NULL, chptr,
                 (MODEBUF_DEST_CHANNEL | MODEBUF_DEST_SERVER |
                  MODEBUF_DEST_OPMODE));
    modebuf_mode(&mbuf, MODE_DEL | m->flag);
    ClrCFlag(chptr, m->flag);
    modebuf_flush(&mbuf);
  }

  *ptr = m->next;
  MyFree(m);

  if (ChanModeList) {
    unsigned int count = 0;
    struct ChanMode *p;

    for (p = ChanModeList; p; p = p->next)
      count++;
    ChanModeList->count = count;
  }

  return CMODE_REMOVE_OK;
}

/** Build the list of registered channel mode characters.
 *
 * RPL_MYINFO advertises the modes this server understands, which is not a
 * constant any more: a module that registers a mode has to appear there
 * too, or clients are told the mode does not exist.
 *
 * @return Pointer to a static buffer, valid until the next call.
 */
const char *channel_chan_mode_chars(void)
{
  static char buf[CHANMODE_CHARS_LEN];
  const struct ChanMode *p;
  size_t len = 0;

  for (p = ChanModeList; p && len + 1 < sizeof(buf); p = p->next) {
    if ((p->attr & CHANMODE_OPLEVELS) && !feature_bool(FEAT_OPLEVELS))
      continue;
    buf[len++] = p->c;
  }
  buf[len] = '\0';

  return buf;
}

/** Build the list of registered channel mode characters that take an
 * argument, for RPL_MYINFO.
 * @return Pointer to a static buffer, valid until the next call.
 */
const char *channel_chan_mode_param_chars(void)
{
  static char buf[CHANMODE_CHARS_LEN];
  const struct ChanMode *p;
  size_t len = 0;

  for (p = ChanModeList; p && len + 1 < sizeof(buf); p = p->next) {
    if (!(p->attr & CHANMODE_PARAM))
      continue;
    if ((p->attr & CHANMODE_OPLEVELS) && !feature_bool(FEAT_OPLEVELS))
      continue;
    buf[len++] = p->c;
  }
  buf[len] = '\0';

  return buf;
}

/** Build the ISUPPORT CHANMODES= value: the four groups, comma separated.
 *
 * Group A keeps a list, group B always takes an argument, group C takes
 * one only when set, group D takes none.  Modes that belong to a member
 * are advertised through PREFIX instead, and the ones the core sets by
 * itself are not advertised at all -- no client can ask for them.
 *
 * @return Pointer to a static buffer, valid until the next call.
 */
const char *channel_chanmodes_supported(void)
{
  static char buf[CHANMODE_CHARS_LEN * 4];
  static const unsigned int group[4] = {
    CHANMODE_LIST, CHANMODE_PARAM, CHANMODE_PARAM_SET, 0
  };
  const struct ChanMode *p;
  size_t len = 0;
  int i;

  for (i = 0; i < 4; i++) {
    if (i)
      buf[len++] = ',';

    for (p = ChanModeList; p && len + 2 < sizeof(buf); p = p->next) {
      /* A member's mode is advertised through PREFIX instead, and +z is
       * not a mode anyone can name: it is what the server calls a +Z it
       * could not honour.
       */
      if (p->attr & (CHANMODE_MEMBER | CHANMODE_HIDDEN))
        continue;
      if ((p->attr & CHANMODE_OPLEVELS) && !feature_bool(FEAT_OPLEVELS))
        continue;

      /* Each mode belongs to exactly one group: a list first, then "takes
       * an argument only when set", then "always takes one", then the
       * plain ones -- which is where every mode a module registers lands.
       */
      if (p->attr & CHANMODE_LIST) {
        if (group[i] != CHANMODE_LIST)
          continue;
      } else if (p->attr & CHANMODE_PARAM_SET) {
        if (group[i] != CHANMODE_PARAM_SET)
          continue;
      } else if (p->attr & CHANMODE_PARAM) {
        if (group[i] != CHANMODE_PARAM)
          continue;
      } else if (group[i])
        continue;

      buf[len++] = p->c;
    }
  }

  buf[len] = '\0';

  return buf;
}
