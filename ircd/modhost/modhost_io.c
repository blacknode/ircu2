/*
 * IRC - Internet Relay Chat, ircd/modhost/modhost_io.c
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
 * @brief The host's end of the socket.
 *
 * Blocking, on purpose and throughout.  The rule the server lives by --
 * never wait for anything -- is the server's; this process exists so that
 * a module can be as slow as it likes without anybody else noticing.  So
 * there is no event loop here, no non-blocking I/O and no state machine:
 * read a frame, do what it says, write what comes of it.
 *
 * That is also what makes a synchronous request possible.  A module
 * asking the equivalent of feature_int() writes a frame and reads the
 * reply, which the server answers out of state it already has; the module
 * sees an ordinary function call, and the only cost is this process
 * waiting, which is this process's whole job.
 */
#include "config.h"

#include "modhost_priv.h"

#include "ircd_snprintf.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Bytes read and not yet framed. */
static char mhost_in[MODHOST_FRAME_MAX * 2];
static size_t mhost_inlen;

/** Bytes of the frame the caller is still holding, consumed on the next
 * read.  See mhost_read(). */
static size_t mhost_consumed;

/** Serials this side issues, for its own requests. */
static unsigned long mhost_serial;

/** Write \a len bytes, all of them. */
static int mhost_write_all(const char* buf, size_t len)
{
  while (len) {
    ssize_t n = write(MODHOST_FD, buf, len);

    if (n < 0) {
      if (errno == EINTR)
        continue;
      return 0;
    }

    if (!n)
      return 0;

    buf += n;
    len -= (size_t) n;
  }

  return 1;
}

int mhost_send(unsigned char verb, unsigned long serial, unsigned int argc,
               const char* const* argv, const size_t* lens)
{
  static char frame[MODHOST_FRAME_MAX];
  size_t n = modhost_encode(frame, sizeof(frame), verb, serial, argc, argv,
                            lens);

  if (!n)
    return 0;

  return mhost_write_all(frame, n);
}

int mhost_read(struct ModHostFrame* f)
{
  for (;;) {
    long used;
    ssize_t n;

    /* The decode does not copy: the frame points into this buffer, so
     * the bytes cannot be moved down until the caller has finished with
     * it.  They are consumed here, on the next read, which is exactly the
     * contract -- a frame is valid until the one after it. */
    if (mhost_consumed) {
      memmove(mhost_in, mhost_in + mhost_consumed,
              mhost_inlen - mhost_consumed);
      mhost_inlen -= mhost_consumed;
      mhost_consumed = 0;
    }

    used = modhost_decode(mhost_in, mhost_inlen, f);

    if (used > 0) {
      mhost_consumed = (size_t) used;
      return 1;
    }

    if (used < 0)
      return 0;                 /* not the server, or not this protocol */

    if (mhost_inlen >= sizeof(mhost_in))
      return 0;

    n = read(MODHOST_FD, mhost_in + mhost_inlen,
             sizeof(mhost_in) - mhost_inlen);

    if (n < 0) {
      if (errno == EINTR)
        continue;
      return 0;
    }

    if (!n)
      return 0;                 /* the server closed: time to go */

    mhost_inlen += (size_t) n;
  }
}

int mhost_request(int req, unsigned int argc, const char* const* argv,
                  struct ModHostFrame* f)
{
  const char* out[MODHOST_ARGS_MAX];
  char reqbuf[16];
  unsigned long serial = ++mhost_serial;
  unsigned int i;

  if (argc + 1 > MODHOST_ARGS_MAX)
    return 0;

  ircd_snprintf(0, reqbuf, sizeof(reqbuf), "%d", req);
  out[0] = reqbuf;

  for (i = 0; i < argc; i++)
    out[i + 1] = argv[i];

  if (!mhost_send(MH_REQUEST, serial, argc + 1, out, 0))
    return 0;

  /* Read until the answer to *this* request.  The server may be sending
   * commands and hooks meanwhile; they are dropped rather than run,
   * because running one here would be running a module's handler from
   * inside another module's handler, one stack frame deeper every time.
   * The server forgets a command it sent, so nothing is owed; a hook that
   * wanted an answer is refused by its own deadline, which is the same
   * outcome as the module having taken too long -- which it has. */
  for (;;) {
    if (!mhost_read(f))
      return 0;

    if (f->mhf_verb == MH_REPLY && f->mhf_serial == serial)
      return 1;

    if (f->mhf_verb == MH_FINI)
      return 0;
  }
}

void mhost_log(int level, const char* fmt, ...)
{
  char text[512];
  char levbuf[16];
  const char* argv[2];
  va_list vl;

  va_start(vl, fmt);
  vsnprintf(text, sizeof(text), fmt, vl);
  va_end(vl);

  ircd_snprintf(0, levbuf, sizeof(levbuf), "%d", level);
  argv[0] = levbuf;
  argv[1] = text;

  mhost_send(MH_LOG, 0, 2, argv, 0);
}
