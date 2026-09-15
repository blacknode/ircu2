/*
 * IRC - Internet Relay Chat, include/ircd_base64.h
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
 * @brief RFC 4648 base64, the standard alphabet.
 *
 * Not to be confused with the P10 base64 in numnicks.h, which uses a
 * different alphabet on purpose -- its output goes in a nick, so it cannot
 * contain '+' or '/'.  This one is the alphabet everything outside the
 * server uses: SASL messages, a stored salt, a certificate.
 *
 * Both directions are covered by the RFC 4648 section 10 vectors in
 * crypto_t.  A codec is exactly the kind of code that is wrong by one
 * character in one corner and right everywhere else, and no amount of
 * reading finds that; a published vector does.
 */
#ifndef INCLUDED_ircd_base64_h
#define INCLUDED_ircd_base64_h

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

/** Bytes an encoding of \a n bytes takes, without the terminator. */
#define IRCD_BASE64_ENCLEN(n)   ((((n) + 2) / 3) * 4)
/** Upper bound on the bytes a decoding of \a n characters yields. */
#define IRCD_BASE64_DECLEN(n)   (((n) / 4) * 3)

/** Encode \a inlen bytes of \a in as base64 into \a out.
 *
 * Always padded with '=', never wrapped: the callers here put the result
 * on one line of an IRC message or in one field of a password hash.
 *
 * @param[in] in Bytes to encode.
 * @param[in] inlen How many.
 * @param[out] out Buffer for the text, NUL-terminated on success.
 * @param[in] outlen Size of \a out, terminator included.
 * @return Characters written, not counting the terminator, or -1 if \a out
 *   is too small.
 */
extern int ircd_base64_encode(const void* in, size_t inlen, char* out,
                              size_t outlen);

/** Decode the base64 text \a in into \a out.
 *
 * Strict: the length must be a multiple of four, padding must be right,
 * and any character outside the alphabet is an error.  A lenient decoder
 * is how one encoding becomes two -- a sender and a receiver that disagree
 * about what a stray newline means -- and this decodes what a client sent,
 * which is to say what an attacker may have sent.
 *
 * @param[in] in Text to decode.
 * @param[in] inlen Its length, or 0 to use strlen().
 * @param[out] out Buffer for the bytes.
 * @param[in] outlen Size of \a out.
 * @return Bytes written, or -1 if \a in is malformed or \a out too small.
 */
extern int ircd_base64_decode(const char* in, size_t inlen, void* out,
                              size_t outlen);

/** Return non-zero if \a in is well-formed base64 of length \a inlen. */
extern int ircd_base64_valid(const char* in, size_t inlen);

#endif /* INCLUDED_ircd_base64_h */
