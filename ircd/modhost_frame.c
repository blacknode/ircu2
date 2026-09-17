/*
 * IRC - Internet Relay Chat, ircd/modhost_frame.c
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
 * @brief Frames on the wire between the server and a module host.
 *
 * Both ends link this, and it depends on neither: no @c struct @c Client,
 * no allocator, no log.  That is what lets the framing be tested on its
 * own (@c modhost_t) -- the split ircd/migration.c has from
 * ircd/migration_run.c -- and it matters more here than usual, because
 * this is a parser reading bytes written by a process that runs in
 * another address space precisely because it is not trusted to be
 * correct.
 *
 * **The decoder never writes.**  Each argument carries its length and is
 * followed by a NUL the *encoder* put there, so decoding is bounds
 * checks and pointers into a buffer it treats as read-only.  One byte per
 * argument buys a parser that cannot corrupt what it is parsing, which
 * on this particular wire is worth considerably more than the byte.
 */
#include "modhost.h"

#include <stdlib.h>
#include <string.h>

/** Read a big-endian 32-bit number. */
static unsigned long mh_get32(const unsigned char* p)
{
  return ((unsigned long) p[0] << 24) | ((unsigned long) p[1] << 16)
       | ((unsigned long) p[2] << 8) | (unsigned long) p[3];
}

/** Write one. */
static void mh_put32(unsigned char* p, unsigned long v)
{
  p[0] = (unsigned char) ((v >> 24) & 0xff);
  p[1] = (unsigned char) ((v >> 16) & 0xff);
  p[2] = (unsigned char) ((v >> 8) & 0xff);
  p[3] = (unsigned char) (v & 0xff);
}

size_t modhost_encode(char* buf, size_t buflen, unsigned char verb,
                      unsigned long serial, unsigned int argc,
                      const char* const* argv, const size_t* lens)
{
  unsigned char* p = (unsigned char*) buf;
  size_t need = MODHOST_HEADER;
  unsigned int i;

  if (argc > MODHOST_ARGS_MAX)
    return 0;

  for (i = 0; i < argc; i++) {
    size_t n = lens ? lens[i] : (argv[i] ? strlen(argv[i]) : 0);

    /* Checked as the total is built rather than at the end, so a length
     * whose sum would wrap is caught before it is added to anything. */
    if (n > MODHOST_FRAME_MAX)
      return 0;

    need += 4 + n + 1;

    if (need > MODHOST_FRAME_MAX || need > buflen)
      return 0;
  }

  mh_put32(p, (unsigned long) (need - 4));
  p += 4;
  *p++ = verb;
  mh_put32(p, serial);
  p += 4;
  *p++ = (unsigned char) argc;

  for (i = 0; i < argc; i++) {
    size_t n = lens ? lens[i] : (argv[i] ? strlen(argv[i]) : 0);

    mh_put32(p, (unsigned long) n);
    p += 4;

    if (n)
      memcpy(p, argv[i], n);

    p += n;
    *p++ = '\0';
  }

  return need;
}

long modhost_decode(const char* buf, size_t buflen,
                    struct ModHostFrame* frame)
{
  const unsigned char* p = (const unsigned char*) buf;
  unsigned long len;
  size_t off;
  unsigned int argc;
  unsigned int i;

  if (buflen < 4)
    return 0;

  len = mh_get32(p);

  /* A length this side would never write is not a frame: whatever is on
   * the other end is not speaking this protocol, and reading on would be
   * reading exactly what it wanted read. */
  if (len > MODHOST_FRAME_MAX || len < MODHOST_HEADER - 4)
    return -1;

  if (buflen < 4 + len)
    return 0;

  memset(frame, 0, sizeof(*frame));
  frame->mhf_verb = p[4];
  frame->mhf_serial = mh_get32(p + 5);
  argc = p[9];

  if (argc > MODHOST_ARGS_MAX)
    return -1;

  frame->mhf_argc = argc;
  off = MODHOST_HEADER;

  for (i = 0; i < argc; i++) {
    unsigned long alen;

    if (off + 4 > 4 + len)
      return -1;

    alen = mh_get32(p + off);
    off += 4;

    /* alen + off cannot wrap: alen is bounded by len, which is bounded
     * by MODHOST_FRAME_MAX, and off is bounded by 4 + len. */
    if (alen > len || off + alen + 1 > 4 + len)
      return -1;

    if (buf[off + alen] != '\0')
      return -1;                /* the encoder always writes it */

    frame->mhf_argv[i] = buf + off;
    frame->mhf_len[i] = alen;
    off += alen + 1;
  }

  /* Trailing bytes inside the declared length are not something the
   * encoder produces, so they are not something to be lenient about. */
  if (off != 4 + len)
    return -1;

  frame->mhf_size = 4 + len;

  return (long) frame->mhf_size;
}

const char* modhost_arg(const struct ModHostFrame* frame, unsigned int i)
{
  if (!frame || i >= frame->mhf_argc || !frame->mhf_argv[i])
    return "";

  return frame->mhf_argv[i];
}

long modhost_argi(const struct ModHostFrame* frame, unsigned int i, long def)
{
  const char* s = modhost_arg(frame, i);
  char* end;
  long v;

  if (!*s)
    return def;

  v = strtol(s, &end, 10);

  return *end ? def : v;
}
