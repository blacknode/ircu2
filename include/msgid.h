#ifndef INCLUDED_msgid_h
#define INCLUDED_msgid_h
/*
 * IRC - Internet Relay Chat, include/msgid.h
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
 * @brief Message identifiers (IRCv3 @c msgid).
 *
 * A message identifier names one message, once, for the whole network: the
 * server the message originated on invents it and every other server
 * forwards the same one, so that two clients on opposite ends of the
 * network agree on what to call it.
 *
 * It is what everything that refers back to a message is built on -- a
 * reply in a thread, a reaction, an edit, a deletion, a read marker, an
 * entry in a history store.  None of those can exist without a name for
 * the message they point at, which is why this comes before all of them.
 *
 * The identifier is opaque: nothing may parse it, and nothing may assume
 * anything about it beyond the character set below.  What is guaranteed is
 * that it is unique on the network and that it never repeats, including
 * across a restart of the server that made it.
 *
 * Shape: the server's P10 numeric, then a counter in base 62.  The numeric
 * makes it unique between servers without anything being negotiated, and
 * the counter -- seeded from the clock, so a restart never walks back over
 * identifiers it already handed out -- makes it unique within one.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>   /* time_t */
#define INCLUDED_sys_types_h
#endif

/** Longest identifier this server generates or accepts, not counting the
 * terminator.  Ours are around a dozen characters; the slack is for a
 * peer that is more generous with its own. */
#define MSGIDLEN 64

/** Start the generator.
 *
 * @param[in] prefix The server's P10 numeric, or NULL.  Copied.  Two
 *   servers with the same numeric cannot be on the same network, so this
 *   alone separates their identifiers.
 * @param[in] now Current time, used to seed the counter.
 */
extern void msgid_init(const char* prefix, time_t now);

/** Generate the next identifier.
 * @return A pointer to a static buffer, valid until the next call.
 */
extern const char* msgid_new(void);

/** Return non-zero if \a id is acceptable as an identifier from a peer.
 *
 * Length and character set only: the value is opaque, and a server that
 * spells its identifiers differently from ours is not wrong.  What this
 * rejects is what would not survive the wire -- anything that is not a
 * printable ASCII character, and the tag separators in particular.
 */
extern int msgid_valid(const char* id);

/** Number of identifiers generated since start-up, for /STATS. */
extern unsigned long msgid_count(void);

#endif /* INCLUDED_msgid_h */
