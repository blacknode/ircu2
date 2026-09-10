/* chan_modes_stub.c - stand-ins for the parts of the server the channel
 * mode register talks to.
 *
 * The register itself only allocates, so the stubs are all about what
 * removing a mode does: it walks the channel list and announces a "-<c>"
 * on every channel that still has the mode.  The test builds that list by
 * hand and counts the announcements, so modebuf_* are recorded rather than
 * sent, and the rest of channel.c is not linked at all.
 */

#include "channel.h"
#include "client.h"
#include "ircd_features.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/** Head of the channel list walked by channel_remove_chan_mode(). */
struct Channel* GlobalChannelList;

/* "me" comes from test_stub.c, which every test links. */

/** Number of modebuf_mode() calls since the test last reset it. */
int stub_modebuf_calls;
/** Channel of the most recent modebuf_init() call. */
struct Channel* stub_modebuf_channel;
/** Mode of the most recent modebuf_mode() call. */
chanmode_t stub_modebuf_mode;
/** Destination flags of the most recent modebuf_init() call. */
unsigned int stub_modebuf_dest;

void modebuf_init(struct ModeBuf *mbuf, struct Client *source,
                  struct Client *connect, struct Channel *chan,
                  unsigned int dest)
{
  (void) source;
  (void) connect;

  mbuf->mb_add = 0;
  mbuf->mb_rem = 0;
  mbuf->mb_count = 0;
  mbuf->mb_channel = chan;
  mbuf->mb_dest = dest;

  stub_modebuf_channel = chan;
  stub_modebuf_dest = dest;
}

void modebuf_mode(struct ModeBuf *mbuf, chanmode_t mode)
{
  (void) mbuf;

  stub_modebuf_calls++;
  stub_modebuf_mode = mode;
}

int modebuf_flush(struct ModeBuf *mbuf)
{
  (void) mbuf;
  return 0;
}

/** Every feature reads as off; only FEAT_OPLEVELS is consulted here, and
 * the test flips it through stub_oplevels rather than through the feature
 * layer, which is not linked.
 */
int stub_oplevels = 1;

int feature_bool(enum Feature feat)
{
  if (feat == FEAT_OPLEVELS)
    return stub_oplevels;
  return 0;
}

/** chan_modes.c panics if it cannot allocate a mode; a test that got here
 * has already failed, so say so and stop rather than carry on.
 */
void server_panic(const char *message)
{
  fprintf(stderr, "server_panic: %s\n", message);
  exit(1);
}
