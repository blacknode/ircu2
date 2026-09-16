#ifndef INCLUDED_capab_h
#define INCLUDED_capab_h
/*
 * IRC - Internet Relay Chat, include/capab.h
 * Copyright (C) 2004 Kevin L. Mitchell <klmitch@mit.edu>
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
 * @brief Client capabilities (IRCv3 CAP).
 *
 * A capability is a name a client negotiates with CAP LS / CAP REQ, and a
 * bit position in the two sets every local connection carries: the one the
 * client asked for (cli_capab()) and the one that is in force (cli_active()).
 *
 * The capabilities the server knows are held in a run-time register, the
 * same way user modes are held in client.c and channel modes in
 * chan_modes.c: the core registers its own at start-up, a module registers
 * its own with module_add_cap(), and everything that advertises, parses or
 * reports a capability walks the register.  There is no table of names
 * anywhere else.
 *
 * Unlike a channel mode, a capability's bit does *not* follow from its
 * name: capabilities never cross a server link, so two servers do not have
 * to agree on the numbering, and a module is handed whatever slot is free.
 * A module must therefore keep the value it is given and never assume one.
 */

#ifndef INCLUDED_client_h
#include "client.h"     /* capset_t, CAP_MAX, FlagHas() */
#endif

#ifndef INCLUDED_ircd_features_h
#include "ircd_features.h"
#endif

struct ModuleHandle;

#define CAPFL_HIDDEN    	0x0001	/**< Do not advertize this capability */
#define CAPFL_HIDDEN_302   	0x0002	/**< Do not advertize this capability to users supporting LS 302 */
#define CAPFL_PROHIBIT  	0x0004	/**< Client may not set this capability */
#define CAPFL_PROTO    		0x0008	/**< Cap must be acknowledged by client */
#define CAPFL_STICKY		0x0010	/**< Cap may not be cleared once set */
#define CAPFL_STICKY_302   	0x0020  /**< Cap may not be cleared once set by users supporting LS 302 */
#define CAPFL_UNAVAILABLE 	(CAPFL_HIDDEN | CAPFL_PROHIBIT)

/** The core's own capabilities.
 *
 * Only the core uses this list; a module never appears here, it registers
 * at run time.  The order fixes the bit each core capability occupies, so
 * entries are appended rather than inserted.
 */
#define CAPLIST	\
	_CAP(AWAYNOTIFY, FEAT_CAP_AWAYNOTIFY, 0 , "away-notify"), \
	_CAP(CHGHOST, FEAT_CAP_CHGHOST, 0, "chghost"), \
	_CAP(ECHOMESSAGE, FEAT_CAP_ECHOMESSAGE, 0, "echo-message"), \
	_CAP(INVITENOTIFY, FEAT_CAP_INVITENOTIFY, 0, "invite-notify"), \
	_CAP(UHNAMES, FEAT_CAP_UHNAMES, 0, "userhost-in-names"), \
	_CAP(MESSAGE_TAGS, FEAT_CAP_MESSAGE_TAGS, 0, "message-tags"), \
	_CAP(SERVER_TIME, FEAT_CAP_SERVER_TIME, 0, "server-time"), \
	_CAP(LANGUAGES, FEAT_CAP_LANGUAGES, 0, "draft/languages"), \
	_CAP(BATCH, FEAT_CAP_BATCH, 0, "batch"), \
	_CAP(LABELEDRESPONSE, FEAT_CAP_LABELEDRESPONSE, 0, "labeled-response"), \
	_CAP(MULTILINE, FEAT_CAP_MULTILINE, 0, "draft/multiline"), \
	_CAP(SASL, FEAT_CAP_SASL, 0, "sasl"), \
	_CAP(CAPNOTIFY, 0, CAPFL_HIDDEN_302 | CAPFL_STICKY_302, "cap-notify")

/** The core's capabilities, as bit positions. */
enum Capab {
#define _CAP(cap, config, flags, name)	E_CAP_ ## cap
  CAPLIST,
#undef _CAP
  CAP_LAST_CORE_CAP   /**< Number of capabilities the core registers. */
};

/** The same positions under their historical names.
 *
 * These used to be a bit mask, one bit per capability in an @c unsigned
 * @c short, which is why they read like one.  They are plain indices now:
 * the sets are bitsets of #CAP_MAX bits, so a capability is addressed by
 * its position and the set is never an integer the caller can inspect.
 */
enum CapabBits {
#define _CAP(cap, config, flags, name) CAP_ ## cap = E_CAP_ ## cap
  CAPLIST,
#undef _CAP
  _CAP_LAST_CAP = CAP_LAST_CORE_CAP
};

/** No capability.
 *
 * Passed where a capability is optional -- the @a require and @a forbid
 * arguments of the sendcmdto_*_capab_*() calls -- because zero is a valid
 * position now that these are indices rather than bits.
 */
#define CAP_NONE (-1)

/** Longest capability name the server accepts, not counting the
 * terminator.  IRCv3 sets no limit; this is what fits comfortably in a
 * CAP LS line alongside its value. */
#define CAPNAMELEN 63
/** Size of the buffer a capability's value is kept in. */
#define CAPVALUELEN 256

/** One registered capability.
 *
 * The definition is public because a module walks the register through
 * cap_first(); changing it changes the module ABI.
 */
struct Capability {
  const char* cap_name;             /**< Name as it goes on the wire. */
  int         cap_index;            /**< Bit position in a #capset_t. */
  unsigned int cap_config;          /**< Feature gating it, or 0 for none. */
  unsigned long cap_flags;          /**< CAPFL_* flags. */
  char        cap_value[CAPVALUELEN]; /**< Value for CAP LS 302, or "". */
  struct ModuleHandle* cap_owner;   /**< Module that registered it, or NULL
                                         for one of the core's. */
  struct Capability* cap_next;      /**< Next capability, sorted by name. */
};

/** Test whether capability \a cap is set in \a cs. */
#define CapHas(cs, cap)	FlagHas(cs, cap)
/** Set capability \a cap in \a cs. */
#define CapSet(cs, cap)	FlagSet(cs, cap)
/** Clear capability \a cap in \a cs. */
#define CapClr(cs, cap)	FlagClr(cs, cap)
/** Clear every capability in \a cs. */
#define CapClrAll(cs)   memset((cs), 0, sizeof(*(cs)))

/** Return non-zero if \a cs has no capability set at all. */
extern int cap_set_empty(const capset_t* cs);

/*
 * The register.  capab.c holds it and knows nothing about clients or the
 * network; m_cap.c speaks the protocol on top of it.
 */

/** Populate the register with the core's capabilities.  Called once. */
extern void cap_init(void);
/** Release the register; main() only, at exit. */
extern void cap_close(void);

/** First registered capability, for iteration; sorted by name. */
extern const struct Capability* cap_first(void);
/** Find a capability by name, case-insensitively.  NULL if there is none. */
extern const struct Capability* cap_find(const char* name);
/** Find a capability by its bit position.  NULL if nothing holds it. */
extern const struct Capability* cap_find_index(int index);
/** Number of capabilities currently registered. */
extern unsigned int cap_count(void);

/** Return non-zero if \a cap is advertised and may be requested.
 *
 * False when the feature gating it is off, or when something has marked it
 * unavailable through cap_update_availability().
 */
extern int cap_is_available(const struct Capability* cap);

/** Register a capability for \a mod.
 *
 * @param[in] mod Module registering it; NULL for the core.
 * @param[in] name Name as it goes on the wire, e.g. "draft/react".
 * @param[in] config Feature gating it, or 0 for none.
 * @param[in] flags CAPFL_* flags.
 * @param[out] index Receives the position assigned, or #CAP_NONE on
 *   failure.  May be NULL.
 * @return Non-zero on success; zero if the name is malformed or already
 *   registered, or if no slot is free.
 */
extern int cap_register(struct ModuleHandle* mod, const char* name,
                        unsigned int config, unsigned long flags,
                        int* index);

/** Remove a capability \a mod registered.
 *
 * Every client that has it loses it, announced with CAP DEL, the same way
 * unloading the module would do it.  A module cannot remove one of the
 * core's, nor one another module registered.
 * @return Non-zero if it was found and removed.
 */
extern int cap_unregister(struct ModuleHandle* mod, const char* name);

/** Remove every capability \a mod registered.  Called when it unloads. */
extern void cap_drop_module(struct ModuleHandle* mod);

/** Number of capabilities \a mod currently has registered. */
extern unsigned int cap_module_count(const struct ModuleHandle* mod);

/** Return non-zero if \a name is usable as a capability name.
 *
 * IRCv3 allows letters, digits, '-', '.' and '_', optionally prefixed by a
 * vendor and a '/'.
 */
extern int cap_name_valid(const char* name);

/** Set the value a capability is advertised with to CAP LS 302 clients. */
extern void cap_set_value(int cap, const char *value);

/** Mark a capability available or not, announcing the change.
 *
 * Nothing is sent when the state is already the one being asked for, so
 * this is safe to call on every rehash.
 */
extern void cap_update_availability(int cap, int available);

/*
 * The protocol side, in m_cap.c.  Declared here because the register calls
 * back into it when a capability appears or goes away.
 */

/** Announce a new capability with CAP NEW to every client that asked to
 * hear about them. */
extern void cap_new(int cap);

/** Announce a capability going away with CAP DEL, and clear it from every
 * client that had it. */
extern void cap_del(int cap);

#endif /* INCLUDED_capab_h */
