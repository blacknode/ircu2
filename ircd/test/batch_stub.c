/* batch_stub.c - the send layer, recorded rather than sent.
 *
 * batch.c reaches the network in exactly two places: the BATCH lines that
 * frame a labeled response and the bare ACK that replaces them when the
 * command produced nothing.  Recording those is the whole of what the test
 * needs, and it avoids linking send.c, which would bring most of the
 * server with it.
 */

#include "client.h"
#include "ircd_string.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** Lines "sent", in order. */
char stub_sent[16][256];
/** How many. */
int stub_sent_count;

void stub_sent_reset(void)
{
  stub_sent_count = 0;
  memset(stub_sent, 0, sizeof(stub_sent));
}

static void stub_record(const char *what, const char *pattern, va_list vl)
{
  char args[192];

  if (stub_sent_count >= (int) (sizeof(stub_sent) / sizeof(stub_sent[0])))
    return;

  vsnprintf(args, sizeof(args), pattern, vl);
  snprintf(stub_sent[stub_sent_count], sizeof(stub_sent[0]), "%s %s",
           what, args);
  stub_sent_count++;
}

void sendcmdto_one(struct Client *from, const char *cmd, const char *tok,
                   struct Client *to, const char *pattern, ...)
{
  va_list vl;

  (void) from;
  (void) tok;
  (void) to;

  va_start(vl, pattern);
  stub_record(cmd, pattern, vl);
  va_end(vl);
}

void sendrawto_one(struct Client *to, const char *pattern, ...)
{
  va_list vl;

  (void) to;

  va_start(vl, pattern);
  stub_record("RAW", pattern, vl);
  va_end(vl);
}
