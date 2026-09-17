/*
 * IRC - Internet Relay Chat, include/modhost.h
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
 * @brief The protocol between the ircd and an out-of-process module host.
 *
 * Phase 7 of doc/proposals/006-comunicaciones-unificadas.md.  A module
 * declared @c isolation = @c "process" is not dlopen()ed into the server:
 * it is dlopen()ed into a host process of its own, and the module API it
 * calls travels over a socketpair as the frames described here.
 *
 * @section mh_why What this buys and what it does not
 *
 * A native module shares the server's address space and can write
 * anywhere in it; there is no barrier to put inside a process, and
 * pretending otherwise would be the worst outcome of all.  So the barrier
 * is a process: an isolated module that corrupts its heap, loops for ever
 * or dies takes its own host with it and nothing else.  The server
 * notices the socket close, reverts what the module had registered, and
 * carries on.
 *
 * It does not make an isolated module harmless.  It still speaks this
 * protocol, and everything it asks for it still gets -- what it cannot do
 * is reach past the protocol.  `isolation` is about failure, not about
 * privilege.
 *
 * @section mh_direction Which side may wait
 *
 * The asymmetry is the whole design.  **The host may block; the server
 * may not.**
 *
 *   - **host to server** is a synchronous request (#MH_REQ_*): the host
 *     writes a frame and reads the reply, and the server answers it from
 *     its own state without waiting for anything.  That is why a module
 *     can call the equivalent of feature_int() or FindClient() and get an
 *     answer on the spot, the way a native module does.
 *   - **server to host** is never waited on.  A command is dispatched and
 *     forgotten.  A hook that has to be *answered* -- a veto -- uses
 *     HOOK_PENDING and hook_resume(), which is exactly why phase 0's
 *     suspendable hooks had to come first: without them a veto in another
 *     process would stop the server on every call, which is the thing
 *     this exists to avoid.
 *
 * @section mh_frame The frame
 *
 * @verbatim
 *   uint32  length of everything after this field
 *   uint8   verb
 *   uint32  serial          (0 when nothing is waiting on a reply)
 *   uint8   argument count
 *   repeated argc times:
 *     uint32  argument length
 *     bytes   argument
 *     uint8   0
 * @endverbatim
 *
 * Big-endian, because a protocol that is read by two programs is read by
 * a third one day -- tcpdump, or a person.  Arguments are counted rather
 * than delimited, so they are binary-safe: a PRIVMSG body goes through
 * unexamined.  The trailing NUL is the encoder's, not the decoder's, and
 * it is the reason **the decoder never writes**: an argument is already
 * terminated when it arrives, so decoding is bounds checks and pointers
 * into a buffer treated as read-only.  One byte an argument buys a
 * parser that cannot corrupt what it is parsing, and on this wire -- fed
 * by the process that is in another address space because it is not
 * trusted -- that is worth considerably more than the byte.
 *
 * Numbers travel as decimal text.  This is not a hot path by
 * construction -- it is the path chosen for the code that is not trusted
 * to be fast or correct -- and a frame somebody can read in a hex dump is
 * worth more here than the bytes it saves.
 */
#ifndef INCLUDED_modhost_h
#define INCLUDED_modhost_h

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

/** Protocol version, compared exactly at the handshake.
 *
 * Separate from #IRCU_MODULE_ABI: that one is about the layout two
 * *compiled* objects agree on, this one is about the bytes two
 * *processes* agree on.  A server and a host from different builds have
 * to match on both, and they are checked in that order, because the ABI
 * is what the module was compiled against.
 */
#define MODHOST_VERSION 1

/** The descriptor the host inherits its end of the socketpair on. */
#define MODHOST_FD 3

/** Longest frame either side will read.
 *
 * A frame carries one command line, one hook, or one rendered message, so
 * this is generous by an order of magnitude.  It is a bound rather than a
 * policy: past it the host is not speaking this protocol and the
 * connection is dropped.
 */
#define MODHOST_FRAME_MAX 65536

/** Most arguments in one frame. */
#define MODHOST_ARGS_MAX 32

/** Bytes before the first argument: length, verb, serial, count. */
#define MODHOST_HEADER 10

/** Verbs.  Values are the wire; never renumber one. */
enum ModHostVerb {
  /* --- server to host --- */
  MH_HELLO = 1,        /**< version, module name, path, loaded_by, server */
  MH_TICK,             /**< CurrentTime and the server's name, sent
                            before a host is given anything to run: a
                            module reads the clock while its handler is
                            on the stack and nowhere else */
  MH_COMMAND,          /**< serial, name, handler type, client, parv... */
  MH_HOOK,             /**< serial, type, subject client, arg... */
  MH_REHASH,           /**< the configuration was re-read */
  MH_FINI,             /**< unload: run mi_fini and exit */
  MH_REPLY,            /**< serial, answer to an MH_REQUEST */

  /* --- host to server --- */
  MH_READY = 64,       /**< module name, version, description, ABI */
  MH_ERROR,            /**< what went wrong; the host then exits */
  MH_LOG,              /**< subsystem, level, text */
  MH_ADD_COMMAND,      /**< name, token, parameters, flags, handler mask */
  MH_DEL_COMMAND,      /**< name */
  MH_ADD_HOOK,         /**< type, priority */
  MH_DEL_HOOK,         /**< type */
  MH_ADD_CMD_HOOK,     /**< command, which, priority, flags */
  MH_SEND,             /**< target numnick, one rendered line */
  MH_REQUEST,          /**< serial, MH_REQ_*, args... */
  MH_HOOK_RESULT,      /**< serial, HookResult */
  MH_COMMAND_DONE      /**< serial: the handler returned */
};

/** What an #MH_REQUEST asks for.  Values are the wire. */
enum ModHostReq {
  MH_REQ_FEATURE_INT = 1,  /**< name -> decimal */
  MH_REQ_FEATURE_BOOL,     /**< name -> "0" or "1" */
  MH_REQ_FEATURE_STR,      /**< name -> text */
  MH_REQ_FIND_CLIENT,      /**< nick -> a client blob, or nothing */
  MH_REQ_FIND_NUMNICK,     /**< numnick -> a client blob, or nothing */
  MH_REQ_CHANNEL_HAS,      /**< channel, nick -> "0" or "1" */
  MH_REQ_IS_CHANOP         /**< channel, nick -> "0" or "1" */
};

/** Fields of a client blob, in order.
 *
 * Plain data, copied at the moment the server sent it.  There is no
 * handle and no way to reach the @c struct @c Client it was taken from:
 * an isolated module cannot hold a pointer into the server, which is the
 * point.  What it does with a stale copy is its own business, and what it
 * sends back is a numnick the server looks up again.
 */
enum ModHostClientField {
  MH_CLI_NUMNICK,      /**< P10 numnick; "" when there is no client. */
  MH_CLI_NAME,
  MH_CLI_USER,
  MH_CLI_HOST,
  MH_CLI_REALNAME,
  MH_CLI_ACCOUNT,      /**< "" unless the client is +r. */
  MH_CLI_SERVER,
  MH_CLI_FLAGS,        /**< Decimal #ModHostClientFlag mask. */
  MH_CLI_FIELDS        /**< How many.  Not a field. */
};

/** Bits of #MH_CLI_FLAGS. */
enum ModHostClientFlag {
  MH_CF_LOCAL    = 0x0001,  /**< MyConnect(): this server's own. */
  MH_CF_OPER     = 0x0002,
  MH_CF_SERVICE  = 0x0004,  /**< +S. */
  MH_CF_BOT      = 0x0008,  /**< +B. */
  MH_CF_ACCOUNT  = 0x0010,  /**< +r. */
  MH_CF_FROZEN   = 0x0020,  /**< +f. */
  MH_CF_SECURE   = 0x0040,  /**< Connected over TLS. */
  MH_CF_SERVER   = 0x0080   /**< Not a user at all. */
};

/** A frame, decoded.  Neither side holds one past the call it drives. */
struct ModHostFrame {
  unsigned char mhf_verb;
  unsigned long mhf_serial;
  unsigned int  mhf_argc;
  const char*   mhf_argv[MODHOST_ARGS_MAX]; /**< Into the caller's buffer,
                                                 NUL-terminated by the
                                                 sender; use mhf_len for
                                                 the ones that may hold a
                                                 NUL of their own. */
  size_t        mhf_len[MODHOST_ARGS_MAX];
  size_t        mhf_size;     /**< Bytes this frame took on the wire. */
};

/*
 * Encoding and decoding.  Shared by the server and the host, with no
 * dependency on either -- which is what lets modhost_t test the framing
 * on its own, the split ircd/migration.c has from migration_run.c.
 */

/** Build a frame into \a buf.
 *
 * @param[out] buf Where to write.
 * @param[in] buflen Its size.
 * @param[in] verb What this is.
 * @param[in] serial Reply pairing, or 0.
 * @param[in] argc How many arguments.
 * @param[in] argv The arguments.
 * @param[in] lens Their lengths, or NULL to use strlen().
 * @return Bytes written, or 0 if it does not fit or the arguments are
 *   more than #MODHOST_ARGS_MAX.
 */
extern size_t modhost_encode(char* buf, size_t buflen, unsigned char verb,
                             unsigned long serial, unsigned int argc,
                             const char* const* argv, const size_t* lens);

/** Decode one frame from \a buf.
 *
 * Does not copy and does not write: #ModHostFrame::mhf_argv points into
 * \a buf, which is left exactly as it was found.  The frame is valid for
 * as long as \a buf holds those bytes.
 *
 * @param[in] buf The bytes, starting at the length field.
 * @param[in] buflen How many are there.
 * @param[out] frame Filled in on success.
 * @return The whole frame's length in bytes when one was decoded, 0 when
 *   more bytes are needed, and -1 when \a buf does not hold a frame this
 *   protocol can produce -- which is not a recoverable state, because
 *   there is no way to know where the next one would start.
 */
extern long modhost_decode(const char* buf, size_t buflen,
                           struct ModHostFrame* frame);

/** The argument at \a i, or "" -- never NULL. */
extern const char* modhost_arg(const struct ModHostFrame* frame,
                               unsigned int i);

/** The argument at \a i read as a number, or \a def when it is absent or
 * is not one. */
extern long modhost_argi(const struct ModHostFrame* frame, unsigned int i,
                         long def);

/*
 * The server's side.  ircd/modhost.c; nothing else calls these, and the
 * host process does not link them at all.
 */

struct ModuleHandle;
struct Client;

/** Start a host process for \a mod and complete the handshake.
 *
 * Synchronous up to the module's own mi_init having run in the host, and
 * with a deadline, so that module_load() either comes back with a loaded
 * module or with a message saying why not -- the same contract dlopen()
 * gives for a native one.  Loading a module is an operator action or
 * start-up, never a hot path, so the wait costs nothing that matters;
 * after it the socket is non-blocking and lives in the event loop like
 * any other.
 *
 * @param[in] mod The handle, already linked and carrying the path.
 * @param[out] errstr Why not, on failure.
 * @return Non-zero on success.
 */
extern int modhost_start(struct ModuleHandle* mod, const char** errstr);

/** Stop a module's host and release everything it held.
 *
 * Sends #MH_FINI, waits briefly for the process to go, and kills it if it
 * will not.  A host that hangs in mi_fini is exactly what isolation is
 * for: it does not get to hold the server.
 */
extern void modhost_stop(struct ModuleHandle* mod);

/** Non-zero if \a mod runs in a host process. */
extern int modhost_isolated(const struct ModuleHandle* mod);

/** The host's process id, or 0. */
extern int modhost_pid(const struct ModuleHandle* mod);

/** Tell every host the configuration was re-read. */
extern void modhost_rehash(void);

/** Dispatch a command a host registered.  ircd/parse.c's proxy handler. */
extern int modhost_command(struct Client* cptr, struct Client* sptr,
                           int parc, char* parv[]);

/** Release everything, for shutdown. */
extern void modhost_shutdown(void);

#endif /* INCLUDED_modhost_h */
