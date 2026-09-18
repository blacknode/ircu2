#ifndef INCLUDED_chan_flags_h
#define INCLUDED_chan_flags_h
/*
 * IRC - Internet Relay Chat, include/chan_flags.h
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
 * @brief Channel mode flags.
 *
 * A channel mode is a bit of the mask in struct Mode::mode, and the bit is
 * a function of the letter, exactly as the user modes do it in
 * include/user_flags.h: 'A' to 'Z' take bits 0 to 25 and 'a' to 'z' take
 * bits 26 to 51.  Nobody hands bits out and nobody chooses one: two
 * servers running the same sources agree on every bit without having to
 * say anything to each other, and a mode registered by a module lands on
 * the same bit everywhere the module is loaded.
 *
 * Fifty-two letters exist, so the twelve bits above them can never be
 * claimed by a mode; #CHANMODE_RESERVED is where the flags that are not
 * modes at all live -- the direction of a change, and the ModeBuf
 * bookkeeping.
 */

#ifndef BITSET
#define BITSET 1ull
#endif

/** A channel mode mask.
 *
 * Sixty-four bits, like flag_t: fifty-two of them are the letters, the
 * top twelve are #CHANMODE_RESERVED.
 */
typedef unsigned long long chanmode_t;

/** Number of bits reserved for mode letters ('A'-'Z' then 'a'-'z'). */
#define CHANMODE_LETTERS 52

/** Bit a mode letter maps to.  Only valid for a letter. */
#define ChanModeBit(c) ((c) >= 'a' ? ((c) - 'a' + 26) : ((c) - 'A'))

/** Test whether \a c is usable as a channel mode character.
 * Only plain A-Z and a-z are valid: IsAlpha() would also accept the
 * accented letters of the configured character set, and those have no bit.
 */
#define ChanModeCharInRange(c) ((('A' <= (c)) && ((c) <= 'Z')) || \
                                (('a' <= (c)) && ((c) <= 'z')))

/** Mask of every bit a mode letter can use. */
#define CHANMODE_LETTER_MASK ((BITSET << CHANMODE_LETTERS) - 1)

/** Channel modes.  The bit follows from the letter; see the file comment. */
#define MODE_APASS      (BITSET << 0)  /**< +A Admin password */
#define MODE_NOCTCP     (BITSET << 2)  /**< +C No CTCPs except ACTION */
#define MODE_DELJOINS   (BITSET << 3)  /**< +D New join messages are delayed */
#define MODE_MODERATENOREG (BITSET << 12) /**< +M Moderate unauthed users */
#define MODE_REGISTERED (BITSET << 17) /**< +R Registered with services */
#define MODE_UPASS      (BITSET << 20) /**< +U User password */
#define MODE_TLSONLY    (BITSET << 25) /**< +Z TLS users only */
#define MODE_BAN        (BITSET << 27) /**< +b Ban */
#define MODE_NOCOLOR    (BITSET << 28) /**< +c No colors */
#define MODE_WASDELJOINS (BITSET << 29) /**< +d Not DELJOINS, but some joins
                                             pending */
#define MODE_INVITEONLY (BITSET << 34) /**< +i Invite only */
#define MODE_KEY        (BITSET << 36) /**< +k Keyed */
#define MODE_LIMIT      (BITSET << 37) /**< +l Limit */
#define MODE_MODERATED  (BITSET << 38) /**< +m Moderated */
#define MODE_NOPRIVMSGS (BITSET << 39) /**< +n No private messages */
#define MODE_CHANOP     (BITSET << 40) /**< +o Channel operator */
#define MODE_PRIVATE    (BITSET << 41) /**< +p Private */
#define MODE_REGONLY    (BITSET << 43) /**< +r Only +r users may join */
#define MODE_SECRET     (BITSET << 44) /**< +s Secret */
#define MODE_TOPICLIMIT (BITSET << 45) /**< +t Topic limited */
#define MODE_NOPARTMSGS (BITSET << 46) /**< +u No part messages */
#define MODE_VOICE      (BITSET << 47) /**< +v Voice */
#define MODE_TLSINSECURE (BITSET << 51) /**< +z TLS insecure network path */

/** Last bit a core channel mode uses. */
#define MODE_LAST_CFLAG MODE_TLSINSECURE

/*
 * Not modes: no letter maps to these bits, and channel_append_chan_mode()
 * refuses any flag that touches them.
 */
#define MODE_BURSTADDED (BITSET << 59) /**< Channel was created by a BURST */
#define MODE_FREE       (BITSET << 60) /**< String must be passed to MyFree() */
#define MODE_SAVE       (BITSET << 61) /**< Save this mode-with-arg 'til later */
#define MODE_DEL        (BITSET << 62) /**< Removing the mode */
#define MODE_ADD        (BITSET << 63) /**< Adding the mode */

/** Nothing at all. */
#define MODE_NULL       0

/** Bits that are not mode letters and can never be registered. */
#define CHANMODE_RESERVED (~CHANMODE_LETTER_MASK)

/** Mode flags which take another parameter (With PARAmeterS). */
#define MODE_WPARAS (MODE_CHANOP | MODE_VOICE | MODE_BAN | MODE_KEY | \
                     MODE_LIMIT | MODE_APASS | MODE_UPASS)

/*
 * Attributes of a registered mode.  They say what the mode is, so that the
 * places that render or parse modes can walk the register instead of
 * carrying a copy of the exceptions.  A module's mode has none of them: it
 * is a plain boolean mode on the channel, global, set by chanops.
 */
#define CHANMODE_PARAM      0x01 /**< Takes an argument. */
#define CHANMODE_PARAM_SET  0x02 /**< ...only when being set (+l). */
#define CHANMODE_LIST       0x04 /**< Keeps a list (+b). */
#define CHANMODE_MEMBER     0x08 /**< Applies to a member, not the channel. */
#define CHANMODE_LOCAL      0x10 /**< Shown to local clients only. */
#define CHANMODE_SERVERONLY 0x20 /**< Only a server, or MODE_PARSE_FORCE. */
#define CHANMODE_INTERNAL   0x40 /**< The core sets it; MODE never does. */
#define CHANMODE_OPLEVELS   0x80 /**< Only exists with FEAT_OPLEVELS. */
#define CHANMODE_HIDDEN    0x100 /**< Not advertised in ISUPPORT CHANMODES. */

/*
 * Results of registering or removing a mode, mirroring the UMODE_* codes.
 */
#define CMODE_ALREADY_EXISTS 0x01
#define CMODE_INVALID_MODE   0x02
#define CMODE_APPEND_OK      0x04
#define CMODE_UNKNOWN_MODE   0x08
#define CMODE_CORE_MODE      0x10
#define CMODE_REMOVE_OK      0x20

/** Size of the buffers the mode letters are rendered into.
 * A mode is one letter, so a list cannot outgrow the alphabet twice over;
 * the slack is for the terminator, the leading sign and for comfort.
 */
#define CHANMODE_CHARS_LEN 64

#endif /* INCLUDED_chan_flags_h */
