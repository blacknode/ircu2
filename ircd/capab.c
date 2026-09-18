/*
 * IRC - Internet Relay Chat, ircd/capab.c
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
 * @brief The register of client capabilities.
 *
 * Everything that advertises, parses or reports a capability reads this
 * list rather than carrying its own copy of the names, which is what lets
 * a module add one: m_cap.c walks the register, and the register is what
 * module_add_cap() appends to.
 *
 * It lives apart from m_cap.c for the same reason migration.c lives apart
 * from migration_run.c: this half knows nothing about clients, sending or
 * the network, so it can be tested on its own.  The half that does -- CAP
 * LS, CAP REQ, CAP NEW, CAP DEL -- is in m_cap.c, and this file reaches it
 * through cap_new() and cap_del() when a capability appears or goes away.
 */
#include "config.h"

#include "capab.h"
#include "ircd_alloc.h"
#include "ircd_log.h"
#include "ircd_string.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

/** The core's capabilities, straight from #CAPLIST.
 *
 * Their positions are the values of #Capab, so they are registered in that
 * order and before anything else can take a slot.
 */
static const struct CapDefault {
  const char*   name;
  unsigned int  config;
  unsigned long flags;
} capDefaults[] = {
#define _CAP(cap, config, flags, name) { (name), (config), (flags) }
  CAPLIST
#undef _CAP
};

/** Number of capabilities in #capDefaults. */
#define CAP_DEFAULT_COUNT (sizeof(capDefaults) / sizeof(capDefaults[0]))

/** The register, sorted by name so that CAP LS comes out in a stable
 * order no matter when a module was loaded. */
static struct Capability* CapList;

/** Position -> capability, so a lookup by bit is not a walk. */
static struct Capability* CapByIndex[CAP_MAX];

/** Number of capabilities registered. */
static unsigned int CapCount;

/** Return non-zero if \a cs has no capability set at all.
 * @param[in] cs Set to test.
 */
int cap_set_empty(const capset_t* cs)
{
  unsigned int i;

  assert(0 != cs);

  for (i = 0; i < sizeof(cs->bits) / sizeof(cs->bits[0]); ++i)
    if (cs->bits[i])
      return 0;
  return 1;
}

/** Return non-zero if \a name is usable as a capability name.
 *
 * IRCv3 names are letters, digits, '-', '.' and '_', optionally prefixed
 * by a vendor and a '/': "draft/chathistory", "example.org/foo".  At most
 * one '/', and it may be neither first nor last.
 * @param[in] name Candidate name.
 */
int cap_name_valid(const char* name)
{
  const char* p;
  int slashes = 0;
  size_t len;

  if (EmptyString(name))
    return 0;

  len = strlen(name);
  if (len > CAPNAMELEN)
    return 0;

  for (p = name; *p; ++p) {
    if (*p == '/') {
      if (p == name || !p[1] || ++slashes > 1)
        return 0;
      continue;
    }
    if (!IsAlnum(*p) && *p != '-' && *p != '.' && *p != '_')
      return 0;
  }

  return 1;
}

/** First registered capability, for iteration.
 * @return Head of the register, sorted by name.
 */
const struct Capability* cap_first(void)
{
  return CapList;
}

/** Find a capability by name.
 * @param[in] name Name to look for; matched case-insensitively.
 * @return The capability, or NULL.
 */
const struct Capability* cap_find(const char* name)
{
  struct Capability* cap;

  if (EmptyString(name))
    return 0;

  for (cap = CapList; cap; cap = cap->cap_next)
    if (!ircd_strcmp(cap->cap_name, name))
      return cap;

  return 0;
}

/** Find a capability by the position it occupies.
 * @param[in] index Bit position.
 * @return The capability, or NULL if nothing holds that position.
 */
const struct Capability* cap_find_index(int index)
{
  if (index < 0 || index >= CAP_MAX)
    return 0;
  return CapByIndex[index];
}

/** Number of capabilities currently registered. */
unsigned int cap_count(void)
{
  return CapCount;
}

/** Return non-zero if \a cap is advertised and may be requested.
 * @param[in] cap Capability to test, or NULL.
 */
int cap_is_available(const struct Capability* cap)
{
  if (!cap)
    return 0;
  if (cap->cap_flags & CAPFL_UNAVAILABLE)
    return 0;
  if (cap->cap_config != 0 && !feature_bool(cap->cap_config))
    return 0;
  return 1;
}

/** Lowest position no capability holds, or #CAP_NONE if the register is
 * full.  Positions below #CAP_LAST_CORE_CAP belong to the core and are
 * taken before a module can ask, so a module never lands on one.
 */
static int cap_alloc_index(void)
{
  int i;

  for (i = 0; i < CAP_MAX; ++i)
    if (!CapByIndex[i])
      return i;

  return CAP_NONE;
}

/** Insert \a cap into the register, keeping it sorted by name.
 * @param[in] cap Capability to insert.
 */
static void cap_link(struct Capability* cap)
{
  struct Capability** p;

  for (p = &CapList; *p; p = &(*p)->cap_next)
    if (ircd_strcmp((*p)->cap_name, cap->cap_name) > 0)
      break;

  cap->cap_next = *p;
  *p = cap;

  CapByIndex[cap->cap_index] = cap;
  CapCount++;
}

/** Release one capability.
 *
 * The name is freed through a local because MyFree() clears what it is
 * given, and the field it lives in is const to everybody who reads the
 * register.
 * @param[in] cap Capability to free.
 */
static void cap_free(struct Capability* cap)
{
  char* name = (char*)cap->cap_name;

  MyFree(name);
  MyFree(cap);
}

/** Take \a cap out of the register and free it.
 * @param[in] cap Capability to remove; must be registered.
 */
static void cap_unlink(struct Capability* cap)
{
  struct Capability** p;

  for (p = &CapList; *p; p = &(*p)->cap_next)
    if (*p == cap) {
      *p = cap->cap_next;
      break;
    }

  CapByIndex[cap->cap_index] = 0;
  CapCount--;

  cap_free(cap);
}

/** Register a capability.
 *
 * The core's own go in first, from cap_init(), and keep the positions the
 * #Capab enumeration gives them.  A module's gets whatever slot is free
 * and must keep the value it is handed: unlike a channel mode, the
 * position does not follow from the name, because capabilities never cross
 * a server link and two servers never have to agree on one.
 *
 * @param[in] mod Module registering it, or NULL for the core.
 * @param[in] name Name as it goes on the wire.
 * @param[in] config Feature gating it, or 0.
 * @param[in] flags CAPFL_* flags.
 * @param[out] index Receives the position, or #CAP_NONE.  May be NULL.
 * @return Non-zero on success.
 */
int cap_register(struct ModuleHandle* mod, const char* name,
                 unsigned int config, unsigned long flags, int* index)
{
  struct Capability* cap;
  int slot;

  if (index)
    *index = CAP_NONE;

  if (!cap_name_valid(name))
    return 0;
  if (cap_find(name))
    return 0;

  slot = cap_alloc_index();
  if (slot == CAP_NONE)
    return 0;

  cap = (struct Capability*)MyMalloc(sizeof(struct Capability));
  assert(0 != cap);
  memset(cap, 0, sizeof(*cap));

  DupString(*(char**)&cap->cap_name, name);
  cap->cap_index = slot;
  cap->cap_config = config;
  cap->cap_flags = flags;
  cap->cap_owner = mod;

  cap_link(cap);

  if (index)
    *index = slot;

  /* The core's own are registered before there is anyone to tell, and
   * cap_new() is a no-op then; a module's arrives on a running server and
   * the clients that asked for cap-notify hear about it.
   */
  if (mod)
    cap_new(slot);

  return 1;
}

/** Remove a capability a module registered.
 *
 * Announced with CAP DEL and cleared from every client that had it, so
 * nobody is left believing a capability is in force that nothing
 * implements any more.
 *
 * @param[in] mod Module that registered it.
 * @param[in] name Name to remove.
 * @return Non-zero if it was found and removed.
 */
int cap_unregister(struct ModuleHandle* mod, const char* name)
{
  struct Capability* cap;

  assert(0 != mod);

  for (cap = CapList; cap; cap = cap->cap_next)
    if (cap->cap_owner == mod && !ircd_strcmp(cap->cap_name, name)) {
      cap_del(cap->cap_index);
      cap_unlink(cap);
      return 1;
    }

  return 0;
}

/** Remove every capability \a mod registered.
 * @param[in] mod Module being unloaded.
 */
void cap_drop_module(struct ModuleHandle* mod)
{
  struct Capability* cap;
  struct Capability* next;

  assert(0 != mod);

  for (cap = CapList; cap; cap = next) {
    next = cap->cap_next;
    if (cap->cap_owner == mod) {
      cap_del(cap->cap_index);
      cap_unlink(cap);
    }
  }
}

/** Number of capabilities \a mod currently has registered.
 * @param[in] mod Module to count for.
 */
unsigned int cap_module_count(const struct ModuleHandle* mod)
{
  const struct Capability* cap;
  unsigned int count = 0;

  assert(0 != mod);

  for (cap = CapList; cap; cap = cap->cap_next)
    if (cap->cap_owner == mod)
      count++;

  return count;
}

/** Non-const lookup by position, for the two mutators below. */
static struct Capability* cap_by_index(int index)
{
  if (index < 0 || index >= CAP_MAX)
    return 0;
  return CapByIndex[index];
}

/** Set the value a capability is advertised with to CAP LS 302 clients.
 * @param[in] cap Position of the capability.
 * @param[in] value Value to advertise; "" for none.
 */
void cap_set_value(int cap, const char *value)
{
  struct Capability* c = cap_by_index(cap);

  if (!c || !value)
    return;

  ircd_strncpy(c->cap_value, value, sizeof(c->cap_value) - 1);
  c->cap_value[sizeof(c->cap_value) - 1] = '\0';
}

/** Mark a capability available or not, announcing the change.
 *
 * Nothing is sent when the state is already what is being asked for, so
 * this is safe to call on every rehash.
 * @param[in] cap Position of the capability.
 * @param[in] available Non-zero to make it available.
 */
void cap_update_availability(int cap, int available)
{
  struct Capability* c = cap_by_index(cap);
  int was_available;

  if (!c)
    return;

  was_available = !(c->cap_flags & CAPFL_UNAVAILABLE);

  if (was_available && !available) {
    c->cap_flags |= CAPFL_UNAVAILABLE;
    cap_del(cap);
  } else if (!was_available && available) {
    c->cap_flags &= ~CAPFL_UNAVAILABLE;
    cap_new(cap);
  }
}

/** Populate the register with the core's capabilities.
 *
 * Called once, before any module can be loaded, so that the core's take
 * the positions #Capab names for them.
 */
void cap_init(void)
{
  unsigned int i;

  assert(0 == CapCount);

  for (i = 0; i < CAP_DEFAULT_COUNT; ++i) {
    int index = CAP_NONE;

    if (!cap_register(0, capDefaults[i].name, capDefaults[i].config,
                      capDefaults[i].flags, &index)) {
      /* Only a mistake in CAPLIST itself can get here: a malformed name or
       * the same one twice.  It is not something a running server can
       * recover from, and it is the same every time, so say which one.
       */
      log_write(LS_SYSTEM, L_CRIT, 0, "Cannot register capability %s",
                capDefaults[i].name);
      assert(0);
      continue;
    }

    /* The positions must come out as enum Capab says, or every CapHas()
     * in the tree is testing the wrong bit.
     */
    assert(index == (int)i);
  }
}

/** Release the register.  main() only, once, at exit. */
void cap_close(void)
{
  struct Capability* cap;
  struct Capability* next;

  for (cap = CapList; cap; cap = next) {
    next = cap->cap_next;
    cap_free(cap);
  }

  CapList = 0;
  CapCount = 0;
  memset(CapByIndex, 0, sizeof(CapByIndex));
}
