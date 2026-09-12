/*
 * IRC - Internet Relay Chat, include/ircd_vhost.h
 * Copyright (C) 2026 ircu2 contributors
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
 * @brief Virtual host generation: the hidden host every user carries.
 *
 * Every registered user's visible host is a cipher of its real IP under a
 * network-wide key (see doc/readme.accounting).  The output has the shape
 * <tt>qWeRty.AsDfGh.v4</tt> for an IPv4 address and <tt>.v6</tt> for an
 * IPv6 one: two 32-bit words of TEA output, each rendered as six characters
 * of the P10 base64 alphabet.
 *
 * The cipher is TEA (Wheeler & Needham), 32 rounds, with the 128-bit key's
 * upper half zero; the input block for IPv4 is the address itself, for IPv6
 * the first 64 bits of the address.  A key is exactly ::VHOST_KEY_LEN
 * characters of the base64 alphabet, and every server on the network must
 * carry the same one so that they all agree on a user's host.
 */
#ifndef INCLUDED_ircd_vhost_h
#define INCLUDED_ircd_vhost_h

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

struct irc_in_addr;

/** Length of a virtual host key, in characters. */
#define VHOST_KEY_LEN 12

/** Longest virtual host the generator produces, without the nul: six
 * base64 characters, a dot, six more, and ".v4" or ".v6".
 */
#define VHOST_MAX_LEN 16

extern int vhost_key_valid(const char* key);
extern int vhost_set_key(const char* key);
extern int vhost_have_key(void);
extern const char* vhost_key(void);

extern int vhost_make(char* buf, size_t len, const struct irc_in_addr* ip);

extern void vhost_tea(const unsigned int v[2], const unsigned int k[2],
                      unsigned int x[2]);

/* Configuration-file glue; see ircd_parser.y and s_conf.c. */
extern void vhost_conf_unmark(void);
extern int vhost_conf_set_key(const char* key);
extern int vhost_conf_sweep(void);

#endif /* INCLUDED_ircd_vhost_h */
