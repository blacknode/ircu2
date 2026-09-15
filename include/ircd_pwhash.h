#ifndef INCLUDED_ircd_pwhash_h
#define INCLUDED_ircd_pwhash_h
/*
 * IRC - Internet Relay Chat, include/ircd_pwhash.h
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
 * @brief Storing a password, and checking one.
 *
 * A stored password is one string that carries everything needed to check
 * it again: the algorithm, its cost, the salt and the tag.
 *
 * @code
 * $argon2id$v=19$m=65536,t=3,p=1$c29tZSBzYWx0$Vi1lIGhhc2ggZ29lcyBoZXJl
 * @endcode
 *
 * The costs travel with the hash and are not read from the configuration
 * when checking, which is what lets them be raised later: an old password
 * keeps verifying under the cost it was made with, and
 * ircd_pwhash_outdated() says when it is worth making again.
 *
 * **Both calls here are pure.**  They read no server state, allocate
 * nothing that outlives them, and touch no @c struct @c Client.  That is
 * deliberate and it is the point: hashing a password takes between fifty
 * and two hundred and fifty milliseconds *on purpose*, which on the main
 * thread means ten people logging in at once stop the server for a
 * second.  These are meant to be called from a worker thread -- see
 * worker.h and doc/readme.workers -- with the password copied in and the
 * result copied out.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

/** Longest encoded password hash, not counting the terminator. */
#define PWHASH_MAX 256

/** Default cost: memory in kibibytes. */
#define PWHASH_DEFAULT_MEMORY 65536
/** Default cost: passes. */
#define PWHASH_DEFAULT_TIME 3
/** Default cost: lanes. */
#define PWHASH_DEFAULT_LANES 1

/** Make a stored password.
 *
 * @param[out] out Where the encoded hash goes; #PWHASH_MAX + 1 bytes.
 * @param[in] pwd The password.
 * @param[in] pwdlen Its length.
 * @param[in] salt Random bytes, at least 8, different for every password.
 *   The caller supplies them: this file has no opinion about where
 *   randomness comes from and no way to be tested if it did.
 * @param[in] saltlen How many.
 * @param[in] secret The server-wide pepper, or NULL.
 * @param[in] secretlen Its length.
 * @param[in] m_cost Memory in kibibytes, or 0 for the default.
 * @param[in] t_cost Passes, or 0 for the default.
 * @return Non-zero on success.
 */
extern int ircd_pwhash_make(char* out, const void* pwd, size_t pwdlen,
                            const void* salt, size_t saltlen,
                            const void* secret, size_t secretlen,
                            unsigned int m_cost, unsigned int t_cost);

/** Check a password against a stored hash.
 *
 * @param[in] stored The encoded hash.
 * @param[in] pwd The password offered.
 * @param[in] pwdlen Its length.
 * @param[in] secret The server-wide pepper, or NULL.
 * @param[in] secretlen Its length.
 * @return Non-zero if it matches.  A stored hash this server cannot
 *   parse never matches, rather than matching everything.
 */
extern int ircd_pwhash_verify(const char* stored, const void* pwd,
                              size_t pwdlen, const void* secret,
                              size_t secretlen);

/** Return non-zero if \a stored was made with a cost below what is asked
 * for now, so it is worth hashing again next time the password is known.
 * @param[in] stored The encoded hash.
 * @param[in] m_cost Memory wanted now, or 0 for the default.
 * @param[in] t_cost Passes wanted now, or 0 for the default.
 */
extern int ircd_pwhash_outdated(const char* stored, unsigned int m_cost,
                                unsigned int t_cost);

#endif /* INCLUDED_ircd_pwhash_h */
