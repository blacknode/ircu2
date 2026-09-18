/*
 * IRC - Internet Relay Chat, ircd/modhost.c
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
 * @brief The server's end of an out-of-process module.
 *
 * Phase 7 of doc/proposals/006-comunicaciones-unificadas.md.  See
 * include/modhost.h for the protocol and doc/readme.isolation for what a
 * module may do on the far side of it.
 *
 * The shape of it: a module declared @c isolation = @c "process" gets a
 * @c struct @c ModuleHandle here like any other, so that everything the
 * server already does per module -- @c /MODULE @c LIST, reverting
 * registrations on unload, the rehash reconciliation -- works without
 * knowing the difference.  What the handle does not have is a
 * @c dlopen() handle.  It has a process.
 *
 * @section mh_hand The handshake blocks; nothing else does
 *
 * module_load() is an operator action or start-up, and its native form
 * already blocks for as long as dlopen() and the module's mi_init take.
 * So the handshake is synchronous, with a deadline: modhost_start()
 * returns a loaded module or a message saying why not, which is the same
 * contract, and an operator typing @c /MODULE @c LOAD gets an answer
 * rather than a maybe.
 *
 * After that the socket is non-blocking and lives in the event loop, and
 * the server never waits on the host again.  A host that stops reading
 * fills its socket, and a write that would block is dropped with a line
 * in the log rather than stalling the server: that is the trade isolation
 * is, and it is the right way round.
 *
 * @section mh_veto Vetoes
 *
 * A hook that only notifies is written to the host and forgotten.  A hook
 * that can *refuse* something cannot be, and this is why
 * @c HOOK_PENDING had to exist first (proposal 006 §5.5): the server
 * suspends the hook, sends it, and resumes when the answer comes back.
 * If the host never answers, @c FEAT_HOOK_TIMEOUT refuses the operation,
 * because failing open would be exactly the outcome the veto was asked to
 * prevent.
 */
#include "config.h"

#include "modhost.h"

#include "channel.h"
#include "client.h"
#include "hash.h"
#include "hooks.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_events.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "module.h"
#include "msg.h"
#include "numnicks.h"
#include "parse.h"
#include "s_conf.h"
#include "s_bsd.h"
#include "s_debug.h"
#include "s_user.h"
#include "send.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/** How long the handshake may take, in milliseconds.
 *
 * Long enough for a module's mi_init to open a file or two, short enough
 * that a host which will never answer does not hold an operator's
 * /MODULE LOAD, or the start-up, for ever.
 */
#define MODHOST_HANDSHAKE_MS 5000

/** How long modhost_stop() waits for a host to leave after MH_FINI. */
#define MODHOST_EXIT_MS 2000

/** How much unwritten output a host may owe before it is given up on.
 *
 * A host that has stopped reading is a host that is wedged, and the one
 * thing the server must not do about that is grow a buffer for it.
 */
#define MODHOST_SENDQ_MAX (256 * 1024)

/** The shortest a host may be silent under questioning, in seconds.
 *
 * A veto that is never answered is refused by FEAT_HOOK_TIMEOUT, which is
 * right on its own -- but a module that has *stopped* answering refuses
 * every registration one deadline at a time, and an operator cannot
 * connect to unload it.  So silence is measured too: a host that was
 * asked something and has said nothing at all since, for longer than
 * this, is not slow, it is gone.  Treating it as gone turns "nobody can
 * log in until somebody restarts the server" back into "the module was
 * dropped and the server carried on", which is the whole reason for
 * running it in a process of its own.
 *
 * The window is twice FEAT_HOOK_TIMEOUT, so a host that is merely slow is
 * never caught by it, and never less than this.
 */
#define MODHOST_SILENCE_MIN 30

/** One module running in a process of its own. */
struct ModHost {
  struct ModHost*      mh_next;
  struct ModuleHandle* mh_mod;      /**< The handle it belongs to. */
  int                  mh_fd;       /**< Our end of the socketpair. */
  pid_t                mh_pid;
  struct Socket        mh_socket;   /**< In the event engine. */
  int                  mh_watching; /**< mh_socket is live. */
  int                  mh_dying;    /**< Being torn down; stop reacting. */

  char   mh_in[MODHOST_FRAME_MAX * 2]; /**< Bytes read, not yet framed. */
  size_t mh_inlen;
  size_t mh_held;                   /**< Bytes of the frame the handshake
                                         is still holding; see
                                         modhost_wait(). */

  char*  mh_out;                    /**< Bytes owed, not yet written. */
  size_t mh_outlen;
  size_t mh_outmax;

  unsigned long mh_serial;          /**< Next serial we issue. */
  time_t mh_ticked;                 /**< CurrentTime the host was last
                                         told; see modhost_freshen(). */

  /** The module's own description, built from MH_READY.
   *
   * A native module's ModuleInfo lives in its shared object; an isolated
   * one's lives here, because the shared object is in another process.
   */
  struct ModuleInfo mh_info;
  char   mh_name[64];
  char   mh_version[32];
  char   mh_descr[128];

  /** Commands it registered, so a dispatch can find its host. */
  struct ModHostCmd* mh_cmds;

  /** Hooks it registered. */
  struct ModHostHook* mh_hooks;

  /** Hook holds outstanding, by serial. */
  struct ModHostHold* mh_holds;

  /** When the first unanswered question went out, or 0.
   *
   * Cleared by *any* frame from the host, so a host that is talking at
   * all is never suspected.  Counting outstanding holds instead would not
   * work: the server drops a hold when its client leaves, without telling
   * this file, so the count would drift up and eventually reap a host
   * that was answering perfectly well.
   */
  time_t              mh_silent_since;
};

/** A command an isolated module registered. */
struct ModHostCmd {
  struct ModHostCmd* mhc_next;
  struct ModHost*    mhc_host;
  char               mhc_name[32];
};

/** One hook a host registered.
 *
 * The callback gets this as its @c user pointer, because HookContext does
 * not carry the hook's own type: one registration, one type, one place to
 * read it from.
 */
struct ModHostHook {
  struct ModHostHook* mhk_next;
  struct ModHost*     mhk_host;
  int                 mhk_type;
};

/** A suspended hook waiting for a host's answer. */
struct ModHostHold {
  struct ModHostHold* mhh_next;
  unsigned long       mhh_serial;
  hook_token_t        mhh_token;
};

/** Every host, newest first. */
static struct ModHost* modhosts;

static void modhost_callback(struct Event* ev);
static void modhost_reap(struct ModHost* host, const char* why);
static void modhost_freshen(struct ModHost* host);

/* ------------------------------------------------------------------- *
 * Writing                                                             *
 * ------------------------------------------------------------------- */

/** Queue \a len bytes for \a host, flushing what it can take now.
 * @return Non-zero if they were taken.
 */
static int modhost_push(struct ModHost* host, const char* buf, size_t len)
{
  ssize_t n;

  if (host->mh_fd < 0 || host->mh_dying)
    return 0;

  /* Anything already owed goes first, or frames would overtake each
   * other and the serials would refer to nothing. */
  if (!host->mh_outlen) {
    n = write(host->mh_fd, buf, len);

    if (n == (ssize_t) len)
      return 1;

    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        modhost_reap(host, "the socket to the host broke");
        return 0;
      }
      n = 0;
    }

    buf += n;
    len -= (size_t) n;
  }

  if (host->mh_outlen + len > MODHOST_SENDQ_MAX) {
    /* Not a buffer to grow: a host that is not reading is wedged, and the
     * server's memory is not the place to record that. */
    modhost_reap(host, "the host stopped reading");
    return 0;
  }

  if (host->mh_outlen + len > host->mh_outmax) {
    size_t want = host->mh_outlen + len + 8192;
    char* grown = (char*) MyMalloc(want);

    if (host->mh_outlen)
      memcpy(grown, host->mh_out, host->mh_outlen);

    MyFree(host->mh_out);
    host->mh_out = grown;
    host->mh_outmax = want;
  }

  memcpy(host->mh_out + host->mh_outlen, buf, len);
  host->mh_outlen += len;

  if (host->mh_watching)
    socket_events(&host->mh_socket, SOCK_ACTION_ADD | SOCK_EVENT_WRITABLE);

  return 1;
}

/** Build and send one frame.
 * @return Non-zero if it went, or was queued.
 */
static int modhost_send(struct ModHost* host, unsigned char verb,
                        unsigned long serial, unsigned int argc,
                        const char* const* argv, const size_t* lens)
{
  static char frame[MODHOST_FRAME_MAX];
  size_t n = modhost_encode(frame, sizeof(frame), verb, serial, argc, argv,
                            lens);

  if (!n) {
    log_write(LS_SYSTEM, L_WARNING, 0,
              "modhost: %s: a frame would not fit and was dropped",
              host->mh_name);
    return 0;
  }

  return modhost_push(host, frame, n);
}

/* ------------------------------------------------------------------- *
 * Client blobs                                                        *
 * ------------------------------------------------------------------- */

/** Where a blob's rendered numbers live for the length of one frame. */
struct ModHostBlob {
  char mhb_num[16];
  char mhb_flags[16];
};

/** Fill #MH_CLI_FIELDS arguments describing \a cptr.
 *
 * A blob and not a handle: the host gets what was true when the frame was
 * written and no way at all to reach the struct it came from.  What it
 * sends back is a numnick, which the server looks up again -- so a client
 * that left in between is simply not found, rather than a pointer into
 * freed memory.
 */
static void modhost_blob(struct Client* cptr, const char** argv,
                         struct ModHostBlob* scratch)
{
  unsigned int flags = 0;
  unsigned int i;

  for (i = 0; i < MH_CLI_FIELDS; i++)
    argv[i] = "";

  scratch->mhb_num[0] = '\0';

  if (!cptr) {
    argv[MH_CLI_FLAGS] = "0";
    return;
  }

  if (IsServer(cptr) || IsMe(cptr)) {
    flags |= MH_CF_SERVER;
    argv[MH_CLI_NAME] = cli_name(cptr);
    argv[MH_CLI_NUMNICK] = "";
  } else {
    /* NumNick() expands to two arguments, which is why this is rendered
     * rather than assigned. */
    ircd_snprintf(0, scratch->mhb_num, sizeof(scratch->mhb_num), "%s%s",
                  NumNick(cptr));
    argv[MH_CLI_NUMNICK] = scratch->mhb_num;
    argv[MH_CLI_NAME] = cli_name(cptr);
    argv[MH_CLI_USER] = cli_user(cptr) ? cli_user(cptr)->username : "";
    argv[MH_CLI_HOST] = cli_user(cptr) ? cli_user(cptr)->host : "";
    argv[MH_CLI_REALNAME] = cli_info(cptr);

    if (IsAccount(cptr) && cli_user(cptr))
      argv[MH_CLI_ACCOUNT] = cli_user(cptr)->account;

    if (cli_user(cptr) && cli_user(cptr)->server)
      argv[MH_CLI_SERVER] = cli_name(cli_user(cptr)->server);

    if (MyConnect(cptr))
      flags |= MH_CF_LOCAL;
    if (IsAnOper(cptr))
      flags |= MH_CF_OPER;
    if (IsServiceBot(cptr))
      flags |= MH_CF_SERVICE;
    if (IsBot(cptr))
      flags |= MH_CF_BOT;
    if (IsAccount(cptr))
      flags |= MH_CF_ACCOUNT;
    if (IsFrozen(cptr))
      flags |= MH_CF_FROZEN;
    if (MyConnect(cptr) && IsTLS(cptr))
      flags |= MH_CF_SECURE;
  }

  ircd_snprintf(0, scratch->mhb_flags, sizeof(scratch->mhb_flags), "%u",
                flags);
  argv[MH_CLI_FLAGS] = scratch->mhb_flags;
}

/* ------------------------------------------------------------------- *
 * What a host asks for                                                *
 * ------------------------------------------------------------------- */

/** Answer an MH_REQUEST.  Every one of these is a read of state the
 * server already has, so none of them waits for anything. */
static void modhost_request(struct ModHost* host,
                            const struct ModHostFrame* f)
{
  const char* argv[MH_CLI_FIELDS];
  struct ModHostBlob blob;
  char numbuf[32];
  long what = modhost_argi(f, 0, 0);
  const char* a = modhost_arg(f, 1);
  const char* b = modhost_arg(f, 2);

  switch (what) {
  /* The enum Feature the module was compiled with travels as a number,
   * which is safe because adding a feature shifts that enum, so it bumps
   * IRCU_MODULE_ABI, and the handshake compared the host's against this
   * server's before any of this could happen.  The type is checked all
   * the same: a module asking for the integer value of a string feature
   * gets zero, not whatever is at that offset. */
  case MH_REQ_FEATURE_INT:
  case MH_REQ_FEATURE_BOOL: {
    int feat = (int) modhost_argi(f, 1, -1);
    int want = (what == MH_REQ_FEATURE_INT) ? FEATURE_TYPE_INT
                                            : FEATURE_TYPE_BOOL;

    ircd_snprintf(0, numbuf, sizeof(numbuf), "%d",
                  feature_type(feat) == want
                    ? (want == FEATURE_TYPE_INT ? feature_int(feat)
                                                : feature_bool(feat))
                    : 0);
    argv[0] = numbuf;
    modhost_send(host, MH_REPLY, f->mhf_serial, 1, argv, 0);
    return;
  }

  case MH_REQ_FEATURE_STR: {
    int feat = (int) modhost_argi(f, 1, -1);

    argv[0] = feature_type(feat) == FEATURE_TYPE_STR ? feature_str(feat) : "";

    if (!argv[0])
      argv[0] = "";

    modhost_send(host, MH_REPLY, f->mhf_serial, 1, argv, 0);
    return;
  }

  case MH_REQ_FIND_CLIENT:
  case MH_REQ_FIND_NUMNICK: {
    struct Client* acptr = (what == MH_REQ_FIND_CLIENT)
                         ? FindUser(a) : findNUser(a);

    modhost_blob(acptr, argv, &blob);
    modhost_send(host, MH_REPLY, f->mhf_serial, MH_CLI_FIELDS, argv, 0);
    return;
  }

  case MH_REQ_CHANNEL_HAS:
  case MH_REQ_IS_CHANOP: {
    struct Channel* chptr = FindChannel(a);
    struct Client* acptr = FindUser(b);
    struct Membership* member = 0;
    int yes = 0;

    if (chptr && acptr)
      member = find_member_link(chptr, acptr);

    if (member)
      yes = (what == MH_REQ_CHANNEL_HAS) ? 1 : IsChanOp(member) ? 1 : 0;

    argv[0] = yes ? "1" : "0";
    modhost_send(host, MH_REPLY, f->mhf_serial, 1, argv, 0);
    return;
  }

  default:
    break;
  }

  /* An unknown request still gets an answer.  The host is waiting on
   * this read, and a server that simply said nothing would hang the very
   * process it is supposed to be able to outlive. */
  argv[0] = "";
  modhost_send(host, MH_REPLY, f->mhf_serial, 1, argv, 0);
}

/* ------------------------------------------------------------------- *
 * What a host registers                                               *
 * ------------------------------------------------------------------- */

/** The handler every isolated command gets.  See modhost_command(). */
static int modhost_handler(struct Client* cptr, struct Client* sptr,
                           int parc, char* parv[])
{
  return modhost_command(cptr, sptr, parc, parv);
}

/** Register a command on a host's behalf. */
static void modhost_add_command(struct ModHost* host,
                                const struct ModHostFrame* f)
{
  MessageHandler handlers[LAST_HANDLER_TYPE];
  struct ModHostCmd* cmd;
  const char* name = modhost_arg(f, 0);
  const char* tok = modhost_arg(f, 1);
  unsigned int parameters = (unsigned int) modhost_argi(f, 2, MAXPARA);
  unsigned int flags = (unsigned int) modhost_argi(f, 3, 0);
  unsigned int mask = (unsigned int) modhost_argi(f, 4, 0);
  int i;

  if (!*name)
    return;

  for (i = 0; i < LAST_HANDLER_TYPE; i++)
    handlers[i] = (mask & (1u << i)) ? modhost_handler : 0;

  if (!module_add_command(host->mh_mod, name, *tok ? tok : 0, parameters,
                          flags, handlers)) {
    log_write(LS_SYSTEM, L_WARNING, 0,
              "modhost: %s: command %s is already taken", host->mh_name,
              name);
    return;
  }

  cmd = (struct ModHostCmd*) MyCalloc(1, sizeof(*cmd));
  cmd->mhc_host = host;
  ircd_strncpy(cmd->mhc_name, name, sizeof(cmd->mhc_name) - 1);
  cmd->mhc_next = host->mh_cmds;
  host->mh_cmds = cmd;
}

/** Forget one. */
static void modhost_del_command(struct ModHost* host, const char* name)
{
  struct ModHostCmd** pp;

  module_del_command(host->mh_mod, name);

  for (pp = &host->mh_cmds; *pp; pp = &(*pp)->mhc_next)
    if (!ircd_strcmp((*pp)->mhc_name, name)) {
      struct ModHostCmd* dead = *pp;

      *pp = dead->mhc_next;
      MyFree(dead);
      return;
    }
}

/* ------------------------------------------------------------------- *
 * Hooks                                                               *
 * ------------------------------------------------------------------- */

/** Remember that \a token is waiting on \a serial. */
static void modhost_hold(struct ModHost* host, unsigned long serial,
                         hook_token_t token)
{
  struct ModHostHold* hold = (struct ModHostHold*) MyCalloc(1, sizeof(*hold));

  hold->mhh_serial = serial;
  hold->mhh_token = token;
  hold->mhh_next = host->mh_holds;
  host->mh_holds = hold;

  if (!host->mh_silent_since)
    host->mh_silent_since = CurrentTime;
}

/** Take a hold off the list, or return 0. */
static hook_token_t modhost_take_hold(struct ModHost* host,
                                      unsigned long serial)
{
  struct ModHostHold** pp;

  for (pp = &host->mh_holds; *pp; pp = &(*pp)->mhh_next)
    if ((*pp)->mhh_serial == serial) {
      struct ModHostHold* hold = *pp;
      hook_token_t token = hold->mhh_token;

      *pp = hold->mhh_next;
      MyFree(hold);
      return token;
    }

  return 0;
}

/** The hook callback registered on a host's behalf. */
static enum HookResult modhost_hook(struct HookContext* ctx, void* user)
{
  struct ModHostHook* reg = (struct ModHostHook*) user;
  struct ModHost* host = reg ? reg->mhk_host : 0;
  const char* argv[4 + MH_CLI_FIELDS * 2];
  struct ModHostBlob bsrc;
  struct ModHostBlob bcli;
  char typebuf[16];
  unsigned int argc = 0;
  unsigned long serial;

  if (!host || host->mh_fd < 0 || host->mh_dying)
    return HOOK_CONTINUE;

  serial = ++host->mh_serial;

  ircd_snprintf(0, typebuf, sizeof(typebuf), "%d", reg->mhk_type);
  argv[argc++] = typebuf;
  argv[argc++] = ctx->hc_arg ? ctx->hc_arg : "";
  argv[argc++] = ctx->hc_channel ? ctx->hc_channel->chname : "";

  modhost_blob(ctx->hc_client, argv + argc, &bcli);
  argc += MH_CLI_FIELDS;
  modhost_blob(ctx->hc_source, argv + argc, &bsrc);
  argc += MH_CLI_FIELDS;

  modhost_freshen(host);

  if (!modhost_send(host, MH_HOOK, serial, argc, argv, 0))
    return HOOK_CONTINUE;

  /* A point that cannot be suspended is a notification, and the server
   * does not wait for one: hc_token is zero exactly where there is no way
   * back in, and reading the answer there would mean blocking. */
  if (!ctx->hc_token)
    return HOOK_CONTINUE;

  modhost_hold(host, serial, ctx->hc_token);

  return HOOK_PENDING;
}

/* ------------------------------------------------------------------- *
 * Reading                                                             *
 * ------------------------------------------------------------------- */

/** Act on one decoded frame. */
static void modhost_frame(struct ModHost* host, const struct ModHostFrame* f)
{
  /* Anything at all from the host clears the silence: it is alive and
   * working, whatever it is working on. */
  host->mh_silent_since = 0;

  switch (f->mhf_verb) {
  case MH_LOG: {
    int level = (int) modhost_argi(f, 0, L_INFO);

    /* The host's log goes into the server's, named, because a module
     * with a log file of its own is a module whose complaint nobody
     * reads.  The level is the host's, clamped: an isolated module does
     * not get to declare its own troubles critical. */
    if (level < L_CRIT)
      level = L_CRIT;
    if (level > L_DEBUG)
      level = L_DEBUG;
    if (level < L_ERROR)
      level = L_ERROR;

    log_write(LS_SYSTEM, level, 0, "%s: %s", host->mh_name,
              modhost_arg(f, 1));
    break;
  }

  case MH_ADD_COMMAND:
    modhost_add_command(host, f);
    break;

  case MH_DEL_COMMAND:
    modhost_del_command(host, modhost_arg(f, 0));
    break;

  case MH_ADD_HOOK: {
    int type = (int) modhost_argi(f, 0, -1);
    struct ModHostHook* reg;

    if (type < 0 || type >= HOOK_LAST) {
      log_write(LS_SYSTEM, L_WARNING, 0,
                "modhost: %s: there is no hook %d", host->mh_name, type);
      break;
    }

    reg = (struct ModHostHook*) MyCalloc(1, sizeof(*reg));
    reg->mhk_host = host;
    reg->mhk_type = type;

    if (!hook_add(host->mh_mod, host->mh_name, (enum HookType) type,
                  modhost_hook,
                  (int) modhost_argi(f, 1, HOOK_PRIORITY_DEFAULT), reg)) {
      MyFree(reg);
      log_write(LS_SYSTEM, L_WARNING, 0,
                "modhost: %s: hook %d was refused", host->mh_name, type);
      break;
    }

    reg->mhk_next = host->mh_hooks;
    host->mh_hooks = reg;
    break;
  }

  case MH_DEL_HOOK:
    hook_del(host->mh_mod, (enum HookType) modhost_argi(f, 0, -1),
             modhost_hook);
    break;

  case MH_SEND: {
    struct Client* acptr = findNUser(modhost_arg(f, 0));

    /* By numnick, looked up now: the host was given a copy of a client,
     * not a pointer to one, so a client that left between the two is
     * simply not here any more. */
    if (acptr && MyConnect(acptr))
      sendrawto_one(acptr, "%s", modhost_arg(f, 1));
    break;
  }

  case MH_REQUEST:
    modhost_request(host, f);
    break;

  case MH_HOOK_RESULT: {
    hook_token_t token = modhost_take_hold(host, f->mhf_serial);
    long result = modhost_argi(f, 0, HOOK_CONTINUE);
    const char* reason = modhost_arg(f, 1);

    if (token)
      hook_resume(token, (enum HookResult) result, *reason ? reason : 0);
    break;
  }

  case MH_COMMAND_DONE:
    break;                      /* nothing waits on it; it closes the log */

  case MH_ERROR:
    log_write(LS_SYSTEM, L_ERROR, 0, "modhost: %s: %s", host->mh_name,
              modhost_arg(f, 0));
    break;

  case MH_READY:
    break;                      /* only the handshake expects one */

  default:
    log_write(LS_SYSTEM, L_WARNING, 0,
              "modhost: %s sent verb %u, which is not one of ours",
              host->mh_name, (unsigned int) f->mhf_verb);
    break;
  }
}

/** Read whatever is there and act on every whole frame.
 * @return Non-zero if the host is still alive.
 */
static int modhost_readable(struct ModHost* host)
{
  ssize_t n;
  size_t off;

  if (host->mh_inlen >= sizeof(host->mh_in)) {
    modhost_reap(host, "the host sent more than a frame's worth of nothing");
    return 0;
  }

  n = read(host->mh_fd, host->mh_in + host->mh_inlen,
           sizeof(host->mh_in) - host->mh_inlen);

  if (n == 0) {
    modhost_reap(host, "the host closed the connection");
    return 0;
  }

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return 1;
    modhost_reap(host, "the socket to the host broke");
    return 0;
  }

  host->mh_inlen += (size_t) n;
  off = 0;

  for (;;) {
    struct ModHostFrame f;
    long used = modhost_decode(host->mh_in + off, host->mh_inlen - off, &f);

    if (!used)
      break;

    if (used < 0) {
      /* There is no resynchronising on a length-prefixed stream: the
       * next frame could start anywhere.  This is the failure the
       * process boundary exists to contain, so it is contained. */
      modhost_reap(host, "the host sent something that is not a frame");
      return 0;
    }

    modhost_frame(host, &f);

    if (host->mh_dying)
      return 0;

    off += (size_t) used;
  }

  if (off) {
    memmove(host->mh_in, host->mh_in + off, host->mh_inlen - off);
    host->mh_inlen -= off;
  }

  return 1;
}

/** Write what is owed. */
static void modhost_writable(struct ModHost* host)
{
  ssize_t n;

  if (!host->mh_outlen) {
    if (host->mh_watching)
      socket_events(&host->mh_socket, SOCK_ACTION_DEL | SOCK_EVENT_WRITABLE);
    return;
  }

  n = write(host->mh_fd, host->mh_out, host->mh_outlen);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
      return;
    modhost_reap(host, "the socket to the host broke");
    return;
  }

  host->mh_outlen -= (size_t) n;

  if (host->mh_outlen)
    memmove(host->mh_out, host->mh_out + n, host->mh_outlen);
  else if (host->mh_watching)
    socket_events(&host->mh_socket, SOCK_ACTION_DEL | SOCK_EVENT_WRITABLE);
}

/** The event engine's way in. */
static void modhost_callback(struct Event* ev)
{
  struct ModHost* host = (struct ModHost*) s_data(ev_socket(ev));

  if (!host)
    return;

  switch (ev_type(ev)) {
  case ET_DESTROY:
    host->mh_watching = 0;
    break;

  case ET_READ:
    modhost_readable(host);
    break;

  case ET_WRITE:
    modhost_writable(host);
    break;

  case ET_ERROR:
  case ET_EOF:
    modhost_reap(host, "the host went away");
    break;

  default:
    break;
  }
}

/* ------------------------------------------------------------------- *
 * Lifecycle                                                           *
 * ------------------------------------------------------------------- */

/** Unlink and free a host, leaving its module handle alone. */
static void modhost_free(struct ModHost* host)
{
  struct ModHost** pp;

  for (pp = &modhosts; *pp; pp = &(*pp)->mh_next)
    if (*pp == host) {
      *pp = host->mh_next;
      break;
    }

  while (host->mh_cmds) {
    struct ModHostCmd* cmd = host->mh_cmds;

    host->mh_cmds = cmd->mhc_next;
    MyFree(cmd);
  }

  /* After hook_del_module() has run, which module_unload_internal() does
   * before it frees the handle: these are what the callbacks were given
   * as their user pointer, so freeing them earlier would leave the hook
   * list pointing at nothing. */
  while (host->mh_hooks) {
    struct ModHostHook* reg = host->mh_hooks;

    host->mh_hooks = reg->mhk_next;
    MyFree(reg);
  }

  while (host->mh_holds) {
    struct ModHostHold* hold = host->mh_holds;

    host->mh_holds = hold->mhh_next;

    /* The answer is not coming.  Refusing is the only safe reading of
     * "the module that was asked has died": failing open would be the
     * outcome the veto existed to prevent. */
    hook_resume(hold->mhh_token, HOOK_DENY,
                "the module that was asked is not running");
    MyFree(hold);
  }

  if (host->mh_watching)
    socket_del(&host->mh_socket);

  if (host->mh_fd >= 0)
    close(host->mh_fd);

  MyFree(host->mh_out);
  MyFree(host);
}

/** Kill a host's process and wait for it, briefly. */
static void modhost_kill(struct ModHost* host)
{
  int status;
  int i;

  if (host->mh_pid <= 0)
    return;

  /* The socket first: a host blocked reading it sees EOF and leaves on
   * its own, which is tidier than a signal and lets mi_fini run. */
  if (host->mh_fd >= 0) {
    if (host->mh_watching) {
      socket_del(&host->mh_socket);
      host->mh_watching = 0;
    }
    close(host->mh_fd);
    host->mh_fd = -1;
  }

  for (i = 0; i < MODHOST_EXIT_MS / 10; i++) {
    pid_t got = waitpid(host->mh_pid, &status, WNOHANG);

    if (got == host->mh_pid || (got < 0 && errno == ECHILD)) {
      host->mh_pid = 0;
      return;
    }

    usleep(10000);
  }

  /* It would not go.  A host that hangs is precisely what isolation is
   * for: it does not get to hold the server. */
  kill(host->mh_pid, SIGKILL);
  waitpid(host->mh_pid, &status, 0);
  host->mh_pid = 0;
}

/** A host died or misbehaved: say so, take the module out. */
static void modhost_reap(struct ModHost* host, const char* why)
{
  struct ModuleHandle* mod;

  if (host->mh_dying)
    return;

  host->mh_dying = 1;
  mod = host->mh_mod;

  log_write(LS_SYSTEM, L_ERROR, 0, "modhost: %s: %s", host->mh_name, why);
  sendto_opmask_butone(0, SNO_OLDSNO, "Isolated module %s: %s",
                       host->mh_name, why);

  modhost_kill(host);

  /* Everything it registered goes the way it goes for a native module
   * that is unloaded -- commands, hooks, bots, routes -- because it has
   * a ModuleHandle like any other.  That is why it has one. */
  if (mod)
    module_unload_isolated(mod);
  else
    modhost_free(host);
}

int modhost_isolated(const struct ModuleHandle* mod)
{
  return module_host(mod) != 0;
}

int modhost_pid(const struct ModuleHandle* mod)
{
  struct ModHost* host = (struct ModHost*) module_host(mod);

  return host ? (int) host->mh_pid : 0;
}

void modhost_stop(struct ModuleHandle* mod)
{
  struct ModHost* host = (struct ModHost*) module_host(mod);

  if (!host)
    return;

  /* Both ends of the link go before anything is freed.  modhost_start()
   * calls this on every failure path and module_unload_internal() calls
   * it again on the way out, so a handle still pointing at a freed host
   * is not a theoretical second call -- it is the first thing that
   * happens when a module will not start. */
  host->mh_mod = 0;
  module_set_host(mod, 0);

  if (!host->mh_dying && host->mh_fd >= 0)
    modhost_send(host, MH_FINI, 0, 0, 0, 0);

  host->mh_dying = 1;
  modhost_kill(host);
  modhost_free(host);
}

/* ------------------------------------------------------------------- *
 * Starting one                                                        *
 * ------------------------------------------------------------------- */

/** Read frames until one arrives, or the deadline passes.  Blocking.
 *
 * Only the handshake uses this; see the note at the top of the file.
 * @return Non-zero when \a f holds a frame.
 */
static int modhost_wait(struct ModHost* host, struct ModHostFrame* f,
                        int timeout_ms)
{
  struct pollfd pfd;
  int left = timeout_ms;

  for (;;) {
    long used;
    ssize_t n;

    /* The decode does not copy: the frame points into this buffer, so
     * the bytes cannot be moved down until the caller has finished with
     * it.  They are consumed here, at the top of the next call, which is
     * the contract -- a frame is valid until the one after it.  Shifting
     * them before returning would hand the caller a frame whose
     * arguments now read the bytes of the frame behind it, which is a
     * bug that looks exactly like a module failing to register. */
    if (host->mh_held) {
      memmove(host->mh_in, host->mh_in + host->mh_held,
              host->mh_inlen - host->mh_held);
      host->mh_inlen -= host->mh_held;
      host->mh_held = 0;
    }

    used = modhost_decode(host->mh_in, host->mh_inlen, f);

    if (used > 0) {
      host->mh_held = (size_t) used;
      return 1;
    }

    if (used < 0 || left <= 0)
      return 0;

    pfd.fd = host->mh_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    if (poll(&pfd, 1, left > 200 ? 200 : left) < 0) {
      if (errno == EINTR)
        continue;
      return 0;
    }

    left -= 200;

    if (!(pfd.revents & POLLIN))
      continue;

    if (host->mh_inlen >= sizeof(host->mh_in))
      return 0;

    n = read(host->mh_fd, host->mh_in + host->mh_inlen,
             sizeof(host->mh_in) - host->mh_inlen);

    if (n <= 0) {
      if (n < 0 && (errno == EAGAIN || errno == EINTR))
        continue;
      return 0;
    }

    host->mh_inlen += (size_t) n;
  }
}

int modhost_start(struct ModuleHandle* mod, const char** errstr)
{
  static char errbuf[512];
  struct ModHost* host;
  struct ModHostFrame f;
  const char* argv[5];
  char verbuf[16];
  int sv[2];
  pid_t pid;

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
    if (errstr)
      *errstr = "could not create a socket for the module host";
    return 0;
  }

  if ((pid = fork()) < 0) {
    close(sv[0]);
    close(sv[1]);
    if (errstr)
      *errstr = "could not fork a module host";
    return 0;
  }

  if (!pid) {
    /* The child.  exec() and not just fork(): a forked ircd would carry
     * the whole server's memory and every descriptor it has open, and
     * the point of this is that the module has neither. */
    char* args[4];

    close(sv[0]);

    if (sv[1] != MODHOST_FD) {
      dup2(sv[1], MODHOST_FD);
      close(sv[1]);
    }

    /* Everything but the socket, stdin, stdout and stderr.  A module that
     * cannot see the server's descriptors cannot write to a client by
     * accident, and cannot hold a listening socket open across a
     * restart. */
    close_connections_above(MODHOST_FD);

    args[0] = (char*) "ircu-modhost";
    args[1] = (char*) module_path(mod);
    args[2] = (char*) module_file(mod);
    args[3] = 0;

    execv(HOST_PATH, args);

    /* execv() only returns on failure, and there is nothing left to say
     * it with: this is a forked copy of the server and must not run any
     * of it. */
    _exit(127);
  }

  close(sv[1]);

  host = (struct ModHost*) MyCalloc(1, sizeof(*host));
  host->mh_fd = sv[0];
  host->mh_pid = pid;
  host->mh_mod = mod;
  ircd_strncpy(host->mh_name, module_file(mod), sizeof(host->mh_name) - 1);
  host->mh_next = modhosts;
  modhosts = host;

  module_set_host(mod, host);

  /* The handshake, blocking and bounded. */
  ircd_snprintf(0, verbuf, sizeof(verbuf), "%d", MODHOST_VERSION);
  argv[0] = verbuf;
  argv[1] = module_file(mod);
  argv[2] = module_path(mod);
  argv[3] = module_loaded_by(mod) ? module_loaded_by(mod) : "";
  /* The server's own name: a module writing sendcmdto_one(&me, ...) means
   * "from this server", and over there `me` is whatever this says it is. */
  argv[4] = cli_name(&me);

  if (!modhost_send(host, MH_HELLO, 0, 5, argv, 0)) {
    if (errstr)
      *errstr = "could not speak to the module host";
    modhost_stop(mod);
    return 0;
  }

  /* Read until MH_READY.  The module's mi_init runs in the host *before*
   * it reports ready -- it has to, because a module that refuses to
   * initialise must not have been announced -- so whatever it registered
   * arrives first and is acted on here.  By the time this returns, the
   * module is loaded and its commands and hooks are in place, which is
   * the same thing module_load() means for a native one. */
  for (;;) {
    if (!modhost_wait(host, &f, MODHOST_HANDSHAKE_MS)) {
      if (errstr)
        *errstr = "the module host did not answer";
      modhost_stop(mod);
      return 0;
    }

    if (f.mhf_verb == MH_READY)
      break;

    if (f.mhf_verb == MH_ERROR) {
      ircd_snprintf(0, errbuf, sizeof(errbuf), "%s", modhost_arg(&f, 0));
      if (errstr)
        *errstr = errbuf;
      modhost_stop(mod);
      return 0;
    }

    modhost_frame(host, &f);

    if (host->mh_dying) {
      if (errstr)
        *errstr = "the module host went away during the handshake";
      return 0;
    }
  }

  /* The host reports the ABI it was built with.  The two are installed
   * together and have to be upgraded together: a host from another build
   * would hand this server an enum Feature, a HookType or a HandlerType
   * that mean something else here, and every one of those travels as a
   * number. */
  if (modhost_argi(&f, 3, -1) != IRCU_MODULE_ABI) {
    ircd_snprintf(0, errbuf, sizeof(errbuf),
                  "ircu-modhost was built for ABI %ld and this server "
                  "speaks %u; they are installed together and have to be "
                  "upgraded together", modhost_argi(&f, 3, -1),
                  (unsigned int) IRCU_MODULE_ABI);
    if (errstr)
      *errstr = errbuf;
    modhost_stop(mod);
    return 0;
  }

  ircd_strncpy(host->mh_name, modhost_arg(&f, 0), sizeof(host->mh_name) - 1);
  ircd_strncpy(host->mh_version, modhost_arg(&f, 1),
               sizeof(host->mh_version) - 1);
  ircd_strncpy(host->mh_descr, modhost_arg(&f, 2),
               sizeof(host->mh_descr) - 1);

  host->mh_info.mi_abi = IRCU_MODULE_ABI;
  host->mh_info.mi_name = host->mh_name;
  host->mh_info.mi_version = host->mh_version;
  host->mh_info.mi_author = "";
  host->mh_info.mi_description = host->mh_descr;

  module_set_info(mod, &host->mh_info);

  /* From here the socket is the event loop's and nothing blocks again. */
  {
    int flags = fcntl(host->mh_fd, F_GETFL, 0);

    if (flags >= 0)
      fcntl(host->mh_fd, F_SETFL, flags | O_NONBLOCK);
  }

  if (!socket_add(&host->mh_socket, modhost_callback, host,
                  SS_CONNECTED, SOCK_EVENT_READABLE, host->mh_fd)) {
    if (errstr)
      *errstr = "could not watch the module host's socket";
    modhost_stop(mod);
    return 0;
  }

  host->mh_watching = 1;

  /* Whatever it sent behind MH_READY -- its registrations, typically --
   * is already in the buffer and will never make the socket readable
   * again on its own. */
  {
    size_t off = host->mh_held;

    host->mh_held = 0;

    for (;;) {
      struct ModHostFrame g;
      long used = modhost_decode(host->mh_in + off, host->mh_inlen - off, &g);

      if (used <= 0)
        break;

      modhost_frame(host, &g);

      if (host->mh_dying)
        return 0;

      off += (size_t) used;
    }

    if (off) {
      memmove(host->mh_in, host->mh_in + off, host->mh_inlen - off);
      host->mh_inlen -= off;
    }
  }

  return 1;
}

/* ------------------------------------------------------------------- *
 * Dispatch                                                            *
 * ------------------------------------------------------------------- */

int modhost_command(struct Client* cptr, struct Client* sptr, int parc,
                    char* parv[])
{
  struct ModHost* host;
  struct ModHostCmd* cmd = 0;
  const char* argv[3 + MH_CLI_FIELDS + MAXPARA + 1];
  struct ModHostBlob blob;
  char handbuf[16];
  unsigned int argc = 0;
  int i;

  /* parv[0] is the source; the command's own name is not in parv at all,
   * so which command this is comes from the message the parser matched. */
  for (host = modhosts; host && !cmd; host = host->mh_next) {
    struct ModHostCmd* c;

    for (c = host->mh_cmds; c; c = c->mhc_next)
      if (!ircd_strcmp(c->mhc_name, parse_current_command())) {
        cmd = c;
        break;
      }
  }

  if (!cmd)
    return 0;

  host = cmd->mhc_host;

  if (host->mh_dying || host->mh_fd < 0)
    return 0;

  ircd_snprintf(0, handbuf, sizeof(handbuf), "%d",
                IsServer(sptr) ? SERVER_HANDLER
                : IsAnOper(sptr) ? OPER_HANDLER
                : IsRegistered(sptr) ? CLIENT_HANDLER : UNREGISTERED_HANDLER);

  argv[argc++] = cmd->mhc_name;
  argv[argc++] = handbuf;

  modhost_blob(sptr, argv + argc, &blob);
  argc += MH_CLI_FIELDS;

  for (i = 1; i < parc && argc < 3 + MH_CLI_FIELDS + MAXPARA; i++)
    argv[argc++] = parv[i] ? parv[i] : "";

  modhost_freshen(host);
  modhost_send(host, MH_COMMAND, ++host->mh_serial, argc, argv, 0);

  (void) cptr;

  return 0;
}

/* ------------------------------------------------------------------- *
 * Housekeeping                                                        *
 * ------------------------------------------------------------------- */

/** Make sure a host's clock and its idea of the server are current.
 *
 * Called before a host is given anything to run, and only then: a module
 * reads CurrentTime while its handler is on the stack, and nowhere else,
 * so that is the only moment it has to be right.  A timer ticking at hosts
 * that are doing nothing would be a timer whose whole output is noise.
 *
 * The server's name rides along because at module-load time it may not be
 * known yet -- the configuration is parsed before the server has its
 * identity -- and a module rendering a line from a server called "" would
 * be sending a client a line it cannot read.
 */
static void modhost_freshen(struct ModHost* host)
{
  const char* argv[2];
  char buf[32];
  int silence = feature_int(FEAT_HOOK_TIMEOUT) * 2;

  if (silence < MODHOST_SILENCE_MIN)
    silence = MODHOST_SILENCE_MIN;

  /* Checked here because here is where the server is about to hand the
   * host more work, which is exactly when a host that is not answering
   * matters. */
  if (host->mh_silent_since
      && CurrentTime - host->mh_silent_since > silence) {
    modhost_reap(host, "the host stopped answering");
    return;
  }

  if (host->mh_ticked == CurrentTime)
    return;

  ircd_snprintf(0, buf, sizeof(buf), "%ld", (long) CurrentTime);
  argv[0] = buf;
  argv[1] = cli_name(&me);

  if (modhost_send(host, MH_TICK, 0, 2, argv, 0))
    host->mh_ticked = CurrentTime;
}

void modhost_rehash(void)
{
  struct ModHost* host;

  for (host = modhosts; host; host = host->mh_next)
    if (!host->mh_dying)
      modhost_send(host, MH_REHASH, 0, 0, 0, 0);
}

void modhost_shutdown(void)
{
  while (modhosts) {
    struct ModHost* host = modhosts;

    host->mh_mod = 0;
    host->mh_dying = 1;
    modhost_kill(host);
    modhost_free(host);
  }
}
