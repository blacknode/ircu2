/*
 * IRC - Internet Relay Chat, ircd/ircd_base64.c
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
 * @brief RFC 4648 base64.
 */
#include "config.h"

#include "ircd_base64.h"

#include <string.h>

/** The standard alphabet, RFC 4648 table 1. */
static const char b64_alphabet[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Value of one base64 character, or -1 if it is not one.
 *
 * Derived from the alphabet rather than written out as a 256-entry table:
 * the table is where a transcription error hides, and the search is over
 * 64 bytes in a function that runs once per SASL message.
 */
static int b64_value(unsigned char c)
{
  const char* p = memchr(b64_alphabet, c, sizeof(b64_alphabet) - 1);

  /* memchr would find the terminator for c == 0 if the length were wrong;
   * the -1 above keeps it out of the search.
   */
  return p ? (int) (p - b64_alphabet) : -1;
}

/** Encode \a inlen bytes of \a in as base64 into \a out.
 * @param[in] in Bytes to encode.
 * @param[in] inlen How many.
 * @param[out] out Buffer for the text.
 * @param[in] outlen Size of \a out, terminator included.
 * @return Characters written, or -1 if \a out is too small.
 */
int ircd_base64_encode(const void* in, size_t inlen, char* out, size_t outlen)
{
  const unsigned char* src = (const unsigned char*) in;
  size_t need = IRCD_BASE64_ENCLEN(inlen);
  size_t o = 0;
  size_t i;

  if (!out || outlen <= need)
    return -1;
  if (inlen && !src)
    return -1;

  for (i = 0; i + 2 < inlen; i += 3) {
    unsigned long v = ((unsigned long) src[i] << 16)
                    | ((unsigned long) src[i + 1] << 8)
                    | src[i + 2];

    out[o++] = b64_alphabet[(v >> 18) & 0x3f];
    out[o++] = b64_alphabet[(v >> 12) & 0x3f];
    out[o++] = b64_alphabet[(v >> 6) & 0x3f];
    out[o++] = b64_alphabet[v & 0x3f];
  }

  /* The tail: one or two bytes, padded out to four characters. */
  if (i < inlen) {
    unsigned long v = (unsigned long) src[i] << 16;
    int two = (i + 1 < inlen);

    if (two)
      v |= (unsigned long) src[i + 1] << 8;

    out[o++] = b64_alphabet[(v >> 18) & 0x3f];
    out[o++] = b64_alphabet[(v >> 12) & 0x3f];
    out[o++] = two ? b64_alphabet[(v >> 6) & 0x3f] : '=';
    out[o++] = '=';
  }

  out[o] = '\0';

  return (int) o;
}

/** Decode the base64 text \a in into \a out.
 * @param[in] in Text to decode.
 * @param[in] inlen Its length, or 0 for strlen().
 * @param[out] out Buffer for the bytes.
 * @param[in] outlen Size of \a out.
 * @return Bytes written, or -1 if \a in is malformed or \a out too small.
 */
int ircd_base64_decode(const char* in, size_t inlen, void* out, size_t outlen)
{
  unsigned char* dst = (unsigned char*) out;
  size_t o = 0;
  size_t i;

  if (!in)
    return -1;
  if (!inlen)
    inlen = strlen(in);
  if (inlen % 4)
    return -1;
  if (!inlen)
    return 0;

  for (i = 0; i < inlen; i += 4) {
    unsigned long v = 0;
    int pad = 0;
    int j;

    for (j = 0; j < 4; j++) {
      char c = in[i + j];

      if (c == '=') {
        /* Padding is only ever the last one or two characters of the last
         * group.  Anywhere else it is a differently-encoded message, and
         * accepting it means accepting two encodings of the same bytes.
         */
        if (i + 4 != inlen || j < 2)
          return -1;
        pad++;
        v <<= 6;
      } else {
        int val;

        if (pad)            /* a character after the padding started */
          return -1;

        val = b64_value((unsigned char) c);
        if (val < 0)
          return -1;

        v = (v << 6) | (unsigned long) val;
      }
    }

    if (o + (size_t) (3 - pad) > outlen)
      return -1;

    dst[o++] = (unsigned char) ((v >> 16) & 0xff);
    if (pad < 2)
      dst[o++] = (unsigned char) ((v >> 8) & 0xff);
    if (pad < 1)
      dst[o++] = (unsigned char) (v & 0xff);
  }

  return (int) o;
}

/** Return non-zero if \a in is well-formed base64 of length \a inlen.
 * @param[in] in Text to check.
 * @param[in] inlen Its length, or 0 for strlen().
 */
int ircd_base64_valid(const char* in, size_t inlen)
{
  size_t i;

  if (!in)
    return 0;
  if (!inlen)
    inlen = strlen(in);
  if (inlen % 4)
    return 0;

  for (i = 0; i < inlen; i++) {
    if (in[i] == '=') {
      /* Only the last group, and only its last one or two characters. */
      if (i + 2 < inlen || i % 4 < 2)
        return 0;
      /* Everything from here on must be padding too. */
      for (; i < inlen; i++)
        if (in[i] != '=')
          return 0;
      break;
    }

    if (b64_value((unsigned char) in[i]) < 0)
      return 0;
  }

  return 1;
}
