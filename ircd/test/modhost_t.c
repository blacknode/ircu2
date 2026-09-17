/* modhost_t.c - Test the frames between the server and a module host.
 *
 * ircd/modhost_frame.c is a parser reading bytes written by a process
 * that runs somewhere else precisely because it is not trusted to be
 * correct, so what has to be pinned down is not that a good frame
 * round-trips -- though that too -- but that a bad one is refused
 * without reading, writing or believing anything past it.
 *
 * It depends on nothing, which is why it can be tested like this: the
 * split ircd/migration.c has from ircd/migration_run.c.
 */

#include "modhost.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* --- helpers --------------------------------------------------------- */

static char buf[MODHOST_FRAME_MAX + 64];

/** Encode into #buf and return the length. */
static size_t enc(unsigned char verb, unsigned long serial,
                  unsigned int argc, const char* const* argv)
{
  return modhost_encode(buf, sizeof(buf), verb, serial, argc, argv, 0);
}

/** Write a big-endian 32-bit number, for hand-built frames. */
static void put32(char* p, unsigned long v)
{
  p[0] = (char) ((v >> 24) & 0xff);
  p[1] = (char) ((v >> 16) & 0xff);
  p[2] = (char) ((v >> 8) & 0xff);
  p[3] = (char) (v & 0xff);
}

/* --- the round trip -------------------------------------------------- */

static void test_round_trip(void)
{
  static const char* const argv[] = { "SPAMFILTER", "AB001", "hello there" };
  struct ModHostFrame f;
  size_t n;

  n = enc(MH_COMMAND, 42, 3, argv);
  assert(n > 0);

  assert(modhost_decode(buf, n, &f) == (long) n);
  assert(f.mhf_verb == MH_COMMAND);
  assert(f.mhf_serial == 42);
  assert(f.mhf_argc == 3);
  assert(!strcmp(modhost_arg(&f, 0), "SPAMFILTER"));
  assert(!strcmp(modhost_arg(&f, 1), "AB001"));
  assert(!strcmp(modhost_arg(&f, 2), "hello there"));
  assert(f.mhf_size == n);

  /* An argument that is not there is "" and not a crash: every reader of
   * this protocol is reading a frame somebody else composed. */
  assert(!strcmp(modhost_arg(&f, 3), ""));
  assert(!strcmp(modhost_arg(&f, MODHOST_ARGS_MAX + 5), ""));

  printf("  round trip ok\n");
}

static void test_no_arguments(void)
{
  struct ModHostFrame f;
  size_t n = enc(MH_FINI, 0, 0, 0);

  assert(n > 0);
  assert(modhost_decode(buf, n, &f) == (long) n);
  assert(f.mhf_verb == MH_FINI);
  assert(f.mhf_serial == 0);
  assert(f.mhf_argc == 0);

  printf("  a frame with no arguments ok\n");
}

static void test_binary_safe(void)
{
  static const char body[] = "a\0b\0c";
  const char* argv[2];
  size_t lens[2];
  struct ModHostFrame f;
  size_t n;

  argv[0] = "PRIVMSG";
  lens[0] = 7;
  argv[1] = body;
  lens[1] = sizeof(body) - 1;   /* 5, NULs and all */

  n = modhost_encode(buf, sizeof(buf), MH_SEND, 1, 2, argv, lens);
  assert(n > 0);

  assert(modhost_decode(buf, n, &f) == (long) n);
  assert(f.mhf_len[1] == 5);
  assert(!memcmp(f.mhf_argv[1], body, 5));

  /* Read as text it stops at the first NUL, which is what a caller that
   * does not pass a length is asking for. */
  assert(!strcmp(modhost_arg(&f, 1), "a"));

  printf("  arguments are binary safe ok\n");
}

static void test_decode_does_not_write(void)
{
  static const char* const argv[] = { "one", "two" };
  char copy[sizeof(buf)];
  struct ModHostFrame f;
  size_t n = enc(MH_HOOK, 7, 2, argv);

  assert(n > 0);
  memcpy(copy, buf, n);

  assert(modhost_decode(buf, n, &f) == (long) n);

  /* The whole point of the encoder's trailing NUL: a parser fed by an
   * untrusted process cannot corrupt the buffer it is parsing, so a
   * caller may decode the same bytes twice, or keep them. */
  assert(!memcmp(copy, buf, n));

  assert(modhost_decode(buf, n, &f) == (long) n);
  assert(!strcmp(modhost_arg(&f, 1), "two"));

  printf("  decoding leaves the buffer alone ok\n");
}

/* --- streaming ------------------------------------------------------- */

static void test_partial(void)
{
  static const char* const argv[] = { "SPAMFILTER" };
  struct ModHostFrame f;
  size_t n = enc(MH_COMMAND, 1, 1, argv);
  size_t i;

  assert(n > 4);

  /* Every prefix short of the whole frame says "more", and says it
   * without touching the output: a socket delivers what it feels like. */
  for (i = 0; i < n; i++)
    assert(modhost_decode(buf, i, &f) == 0);

  assert(modhost_decode(buf, n, &f) == (long) n);

  printf("  a partial frame asks for more ok\n");
}

static void test_two_in_a_row(void)
{
  static const char* const one[] = { "first" };
  static const char* const two[] = { "second" };
  struct ModHostFrame f;
  size_t a;
  size_t b;
  long used;

  a = modhost_encode(buf, sizeof(buf), MH_LOG, 0, 1, one, 0);
  assert(a > 0);
  b = modhost_encode(buf + a, sizeof(buf) - a, MH_LOG, 0, 1, two, 0);
  assert(b > 0);

  used = modhost_decode(buf, a + b, &f);
  assert(used == (long) a);
  assert(!strcmp(modhost_arg(&f, 0), "first"));

  /* And the second one is still intact behind it, which it would not be
   * if decoding had written a terminator past the first frame. */
  used = modhost_decode(buf + a, b, &f);
  assert(used == (long) b);
  assert(!strcmp(modhost_arg(&f, 0), "second"));

  printf("  two frames in one read ok\n");
}

/* --- refusals -------------------------------------------------------- */

static void test_bad_length(void)
{
  struct ModHostFrame f;

  /* Longer than anything this protocol produces. */
  put32(buf, MODHOST_FRAME_MAX + 1);
  assert(modhost_decode(buf, 4, &f) == -1);

  /* Shorter than a header, so the header cannot be there. */
  put32(buf, 1);
  assert(modhost_decode(buf, 5, &f) == -1);

  put32(buf, 0);
  assert(modhost_decode(buf, 4, &f) == -1);

  printf("  impossible lengths refused ok\n");
}

static void test_argument_past_the_end(void)
{
  static const char* const argv[] = { "abc" };
  struct ModHostFrame f;
  size_t n = enc(MH_LOG, 0, 1, argv);

  assert(n > 0);

  /* The argument claims more bytes than the frame declares.  This is the
   * one that matters: believing it is a read past the buffer. */
  put32(buf + MODHOST_HEADER, 1000);
  assert(modhost_decode(buf, n, &f) == -1);

  /* And one that claims a length that would wrap the offset. */
  put32(buf + MODHOST_HEADER, 0xffffffffUL);
  assert(modhost_decode(buf, n, &f) == -1);

  printf("  an argument past the end refused ok\n");
}

static void test_missing_terminator(void)
{
  static const char* const argv[] = { "abc" };
  struct ModHostFrame f;
  size_t n = enc(MH_LOG, 0, 1, argv);

  assert(n > 0);
  assert(modhost_decode(buf, n, &f) == (long) n);

  /* The NUL is part of the format, not a courtesy: without it the
   * argument is not terminated and nothing downstream may treat it as a
   * string. */
  buf[MODHOST_HEADER + 4 + 3] = 'x';
  assert(modhost_decode(buf, n, &f) == -1);

  printf("  a missing terminator refused ok\n");
}

static void test_trailing_bytes(void)
{
  static const char* const argv[] = { "abc" };
  struct ModHostFrame f;
  size_t n = enc(MH_LOG, 0, 1, argv);

  assert(n > 0);

  /* Say the frame is four bytes longer than its arguments account for.
   * The encoder never does that, so it is not something to be lenient
   * about: leniency here is where one frame becomes two. */
  put32(buf, (unsigned long) (n - 4 + 4));
  memset(buf + n, 0, 4);
  assert(modhost_decode(buf, n + 4, &f) == -1);

  printf("  trailing bytes inside a frame refused ok\n");
}

static void test_too_many_arguments(void)
{
  const char* argv[MODHOST_ARGS_MAX + 1];
  struct ModHostFrame f;
  unsigned int i;
  size_t n;

  for (i = 0; i < MODHOST_ARGS_MAX + 1; i++)
    argv[i] = "x";

  /* The encoder refuses to build one... */
  assert(modhost_encode(buf, sizeof(buf), MH_LOG, 0, MODHOST_ARGS_MAX + 1,
                        argv, 0) == 0);

  /* ...and the decoder refuses to believe one, which is the half that
   * matters, because the other end is not this code. */
  n = enc(MH_LOG, 0, 1, argv);
  assert(n > 0);
  buf[9] = (char) (MODHOST_ARGS_MAX + 1);
  assert(modhost_decode(buf, n, &f) == -1);

  printf("  too many arguments refused ok\n");
}

static void test_encode_bounds(void)
{
  static char big[MODHOST_FRAME_MAX];
  const char* argv[1];
  size_t lens[1];
  char small[16];

  memset(big, 'x', sizeof(big));
  argv[0] = big;
  lens[0] = sizeof(big);

  /* One argument as long as the largest frame cannot fit in a frame,
   * because the frame has a header. */
  assert(modhost_encode(buf, sizeof(buf), MH_LOG, 0, 1, argv, lens) == 0);

  /* Nor does anything fit in a buffer too small for it, and finding that
   * out must not involve writing to it. */
  argv[0] = "a reasonably long argument";
  assert(modhost_encode(small, sizeof(small), MH_LOG, 0, 1, argv, 0) == 0);

  printf("  encoding refuses what will not fit ok\n");
}

/* --- numbers --------------------------------------------------------- */

static void test_numbers(void)
{
  static const char* const argv[] = { "17", "-3", "", "12x", "notanumber" };
  struct ModHostFrame f;
  size_t n = enc(MH_HOOK, 0, 5, argv);

  assert(n > 0);
  assert(modhost_decode(buf, n, &f) == (long) n);

  assert(modhost_argi(&f, 0, -1) == 17);
  assert(modhost_argi(&f, 1, -1) == -3);

  /* Absent, empty, and not a number all give the default.  A caller
   * asking for a number has a sensible answer for "the other end sent
   * rubbish", and it is never "0". */
  assert(modhost_argi(&f, 2, 99) == 99);
  assert(modhost_argi(&f, 3, 99) == 99);
  assert(modhost_argi(&f, 4, 99) == 99);
  assert(modhost_argi(&f, 9, 99) == 99);

  printf("  numbers, and what is not one ok\n");
}

int main(void)
{
  printf("modhost_t: frames between the server and a module host\n");

  test_round_trip();
  test_no_arguments();
  test_binary_safe();
  test_decode_does_not_write();
  test_partial();
  test_two_in_a_row();
  test_bad_length();
  test_argument_past_the_end();
  test_missing_terminator();
  test_trailing_bytes();
  test_too_many_arguments();
  test_encode_bounds();
  test_numbers();

  printf("modhost_t: all ok\n");

  return 0;
}
