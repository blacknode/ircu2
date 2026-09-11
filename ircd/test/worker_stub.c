/* worker_stub.c - the three things worker.c reaches out of itself for.
 *
 * worker.c is almost self-contained: its queues, its threads and its mutex
 * are all its own.  What it does need from the rest of the server is the
 * event system (to have the wake-up pipe watched), the feature system (to
 * be told how many threads to run) and numnicks (to turn a stored numnick
 * back into a client).  Linking the real ones would drag in most of the
 * server, so here they are as stubs a test can steer.
 *
 * Shared by worker_t and module_t.
 */

#include "client.h"
#include "ircd_events.h"
#include "ircd_features.h"
#include "ircd_osdep.h"
#include "numnicks.h"

#include <string.h>

/* feature_int() is not here: module_t links this file alongside
 * user_modes_stub.c, which already supplies one that answers zero -- and
 * zero for FEAT_WORKER_THREADS is exactly what module_t wants.  worker_t
 * defines its own, steerable one instead.
 */

/** Non-zero while the wake-up socket is registered. */
int stub_socket_live;
/** Descriptor worker.c asked to have watched. */
int stub_socket_fd = -1;

/** The client findNUser() answers with, or NULL. */
struct Client* stub_numnick_client;
/** The numnick it answers for. */
char stub_numnick[8];

/* The test never runs an event loop, so the socket is only recorded.  The
 * queue is drained by calling worker_drain() directly, which is what the
 * real callback does once the engine has woken it.
 */
int socket_add(struct Socket* sock, EventCallBack call, void* data,
               enum SocketState state, unsigned int events, int fd)
{
  (void) data; (void) state; (void) events;

  sock->s_header.gh_call = call;
  sock->s_fd = fd;
  stub_socket_fd = fd;
  stub_socket_live = 1;

  return 1;
}

void socket_del(struct Socket* sock)
{
  (void) sock;
  stub_socket_live = 0;
}

/* worker.c closes the read end from its ET_DESTROY branch, which only runs
 * through a real engine; with this stub nothing generates that event, so
 * the test closes the descriptor itself if it cares.
 */

int os_set_nonblocking(int fd)
{
  (void) fd;
  return 1;
}

struct Client* findNUser(const char* yxx)
{
  if (stub_numnick_client && 0 == strcmp(yxx, stub_numnick))
    return stub_numnick_client;

  return 0;
}
