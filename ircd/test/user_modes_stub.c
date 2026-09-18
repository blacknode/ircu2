/* user_modes_stub.c - stand-ins for the parts of client.c the user mode
 * registry does not exercise.
 *
 * client.c also holds the ping/class lookups and the privilege table, which
 * reach into the config, feature and send layers -- most of the server, for
 * code this test never calls.  Only send_umode_out() matters here: removing
 * a mode announces the change, and the test checks that it is announced for
 * our own users and nobody else's.
 */

#include "client.h"
#include "s_user.h"

#include <stddef.h>

/** Head of the client list walked by client_remove_user_mode(). */
struct Client* GlobalClientList;

/** Number of send_umode_out() calls since the test last reset it. */
int stub_umode_out_calls;
/** Client of the most recent send_umode_out() call. */
struct Client* stub_umode_out_client;
/** Prior user modes passed to the most recent send_umode_out() call. */
flag_t stub_umode_out_old;

void send_umode_out(struct Client* cptr, struct Client* sptr, flag_t old,
                    int prop)
{
  (void) prop;
  (void) cptr;

  stub_umode_out_calls++;
  stub_umode_out_client = sptr;
  stub_umode_out_old = old;
}

/* --- The rest of client.c ---------------------------------------------
 *
 * The ping/class lookups and the privilege report are compiled along with
 * the registry; these keep the link happy without dragging in the config,
 * feature and send layers.
 */

#include "class.h"
#include "ircd_features.h"
#include "msgq.h"
#include "send.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int get_conf_ping(const struct ConfItem* aconf)
{
  (void) aconf;
  return 0;
}

int feature_int(enum Feature feat)
{
  (void) feat;
  return 0;
}

struct ConnectionClass* find_remote_oper_class(void)
{
  return NULL;
}

char* rpl_str(int numeric)
{
  (void) numeric;
  return "";
}

struct MsgBuf* msgq_make(struct Client* dest, const char* format, ...)
{
  (void) dest;
  (void) format;
  return NULL;
}

void msgq_append(struct Client* dest, struct MsgBuf* mb, const char* format,
                 ...)
{
  (void) dest;
  (void) mb;
  (void) format;
}

void msgq_clean(struct MsgBuf* mb)
{
  (void) mb;
}

void send_buffer(struct Client* to, struct Client* from, struct MsgBuf* buf,
                 int prio, const struct MsgTagCtx* ctx,
                 struct TagSendCache* cache)
{
  (void) to;
  (void) from;
  (void) buf;
  (void) prio;
  (void) ctx;
  (void) cache;
}

void server_panic(const char* message)
{
  fprintf(stderr, "server_panic: %s\n", message);
  abort();
}
