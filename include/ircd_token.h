/*
 * IRC - Internet Relay Chat, include/ircd_token.h
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
 * @brief Something the server hands out and must recognise again.
 *
 * A token carries what it asserts, in the clear, and an HMAC over it:
 *
 * @verbatim 1.<base64 of payload>.<base64 of 16 bytes of tag> @endverbatim
 *
 * Nothing is stored.  There is no table of outstanding tokens to keep, to
 * expire or to leak, and a token minted on one server of a network is
 * recognised on every other -- because the key is derived from the
 * @c Security{} key, which is the same on all of them by construction
 * (doc/readme.accounting).  The price is that a token cannot be revoked
 * before it expires, which is why every caller gives one a short life and
 * why what a token grants is always one small thing.
 *
 * The @a label is what keeps two uses apart.  It goes into the key
 * derivation, so a token minted to verify an address cannot be presented
 * as one that authorises an upload: same key material, different keys,
 * and neither one is the key that ciphers hostnames.
 *
 * Sixteen bytes of tag and not thirty-two because a person pastes these
 * back, and 128 bits is far past what forging one is worth.
 */
#ifndef INCLUDED_ircd_token_h
#define INCLUDED_ircd_token_h

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

/** Longest payload a token may carry. */
#define TOKEN_PAYLOAD_MAX 384
/** Longest token ircd_token_make() will produce. */
#define TOKEN_MAX 640

/** Why a token was refused. */
enum TokenResult {
  TOKEN_OK,         /**< Signed by this network, and still valid. */
  TOKEN_MALFORMED,  /**< Not a token this server ever issued. */
  TOKEN_BAD,        /**< Well formed, wrong signature. */
  TOKEN_EXPIRED,    /**< Signed by this network, too old. */
  TOKEN_NOKEY       /**< No Security{} key, so nothing can be signed. */
};

/** Mint a token.
 *
 * @param[out] buf Where to write it; #TOKEN_MAX is always enough.
 * @param[in] len Size of \a buf.
 * @param[in] label What this token is for; see the file comment.
 * @param[in] payload What it asserts.  Not a secret: whoever reads the
 *   token can read this, and the signature is what makes it trustworthy.
 *   It may not contain a NUL or a newline.
 * @param[in] now Current time.
 * @param[in] window How long it is good for, in seconds.
 * @return Non-zero on success; zero when there is no key, the payload is
 *   too long or malformed, or \a buf is too small.
 */
extern int ircd_token_make(char* buf, size_t len, const char* label,
                           const char* payload, time_t now, int window);

/** Check a token and hand back what it asserts.
 *
 * @param[in] token The token, as it was presented.
 * @param[in] label What it must have been minted for.
 * @param[out] payload Where to write the assertion; untouched unless
 *   #TOKEN_OK.
 * @param[in] len Size of \a payload.
 * @param[in] now Current time.
 * @return #TOKEN_OK, or why not.
 */
extern enum TokenResult ircd_token_check(const char* token, const char* label,
                                         char* payload, size_t len,
                                         time_t now);

#endif /* INCLUDED_ircd_token_h */
