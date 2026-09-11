/* module_stub.c - parse.c stand-ins for module_t.
 *
 * module.c registers commands through parse_add_command() and
 * parse_del_command(), which live in parse.c along with msgtab and its
 * reference to every m_* handler in the server -- far too much to link into
 * a unit test.
 *
 * These stubs keep a tiny registry instead, which lets the test assert the
 * thing that actually matters at this layer: that every command a module
 * registers is unregistered exactly once when the module goes away.  The
 * real trie is exercised against a running server.
 */

struct Client;

#include "msg.h"
#include "parse.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/** Commands currently registered through the stub. */
int stub_commands_live;

/** Total registrations ever made, so the test can tell "never registered"
 * from "registered and then removed".
 */
int stub_commands_added;

/** Names currently registered, for the duplicate check. */
#define STUB_MAX_COMMANDS 32
static char* stub_names[STUB_MAX_COMMANDS];

static int stub_find(const char* cmd)
{
  int i;

  for (i = 0; i < STUB_MAX_COMMANDS; i++)
    if (stub_names[i] && 0 == strcmp(stub_names[i], cmd))
      return i;

  return -1;
}

struct Message *parse_add_command(const char *cmd, const char *tok,
                                  unsigned int parameters, unsigned int flags,
                                  MessageHandler handlers[])
{
  struct Message* msg;
  int slot;

  (void) parameters;
  (void) flags;
  (void) handlers;

  if (!tok)
    tok = cmd;

  /* Same refusal the real one makes, so the test can cover it. */
  if (stub_find(cmd) >= 0)
    return NULL;

  for (slot = 0; slot < STUB_MAX_COMMANDS; slot++)
    if (!stub_names[slot])
      break;
  assert(slot < STUB_MAX_COMMANDS);

  msg = calloc(1, sizeof(struct Message));
  assert(msg != NULL);
  msg->cmd = strdup(cmd);
  msg->tok = strdup(tok);

  stub_names[slot] = msg->cmd;
  stub_commands_live++;
  stub_commands_added++;

  return msg;
}

void parse_del_command(struct Message *msg)
{
  int slot;

  assert(msg != NULL);

  slot = stub_find(msg->cmd);
  assert(slot >= 0 && "command removed twice, or never registered");
  stub_names[slot] = NULL;
  stub_commands_live--;

  free(msg->cmd);
  free(msg->tok);
  free(msg);
}

/** Stub for hook_deny_reply(), the only part of hooks.c that reaches into
 * the send layer.  Answering a client is not what these tests are about,
 * and linking send.c would drag in most of the server.
 */
int send_reply(struct Client *to, int reply, ...)
{
  (void) to;
  (void) reply;
  return 0;
}

/* --- The channel side -------------------------------------------------
 *
 * module.c registers channel modes against chan_modes.c, which announces
 * a mode change on every channel that carries a mode being taken away.
 * These tests never build a channel, so the list is empty and the ModeBuf
 * calls are here only to satisfy the link.
 */

#include "channel.h"
#include "ircd_features.h"

/** Head of the channel list walked by channel_remove_chan_mode(). */
struct Channel* GlobalChannelList;

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
}

void modebuf_mode(struct ModeBuf *mbuf, chanmode_t mode)
{
  (void) mbuf;
  (void) mode;
}

int modebuf_flush(struct ModeBuf *mbuf)
{
  (void) mbuf;
  return 0;
}

int feature_bool(enum Feature feat)
{
  /* Oplevels on, so +A and +U are registered like on a stock server. */
  return feat == FEAT_OPLEVELS;
}

/** Stub for the one thing ircd_snprintf.c reaches out of itself for.
 *
 * module.c formats the load errors that migration.c hands it, and that
 * pulls in ircd_snprintf(), whose %C conversion can render a client's
 * username.  No module load error names a client.
 */
const char* visible_username(const struct Client* cptr)
{
  (void) cptr;
  return "";
}
