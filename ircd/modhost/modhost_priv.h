/*
 * IRC - Internet Relay Chat, ircd/modhost/modhost_priv.h
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
 * @brief Private to ircu-modhost.
 */
#ifndef INCLUDED_modhost_priv_h
#define INCLUDED_modhost_priv_h

#include "modhost.h"

struct ModuleHandle;
struct Client;

/*
 * The socket.  Everything in the host goes through these; there is one
 * descriptor and one thread, so there is no locking anywhere in here.
 */

/** Write one frame.  Returns non-zero on success. */
extern int mhost_send(unsigned char verb, unsigned long serial,
                      unsigned int argc, const char* const* argv,
                      const size_t* lens);

/** Read frames until one arrives.  Blocks; the host is allowed to.
 * @return Non-zero when \a f holds a frame, zero when the socket closed.
 */
extern int mhost_read(struct ModHostFrame* f);

/** Ask the server something and wait for the answer.
 *
 * The one place the host blocks on purpose.  A module calling the
 * equivalent of feature_int() gets an answer on the spot, the way a
 * native one does, and the server answers from state it already has.
 *
 * @param[in] req One of #ModHostReq.
 * @param[in] argc How many arguments follow.
 * @param[in] argv The arguments.
 * @param[out] f The reply.
 * @return Non-zero on success.
 */
extern int mhost_request(int req, unsigned int argc, const char* const* argv,
                         struct ModHostFrame* f);

/** Send a line of text to the server's log. */
extern void mhost_log(int level, const char* fmt, ...);

/*
 * The module.
 */

/** The handle the module was given; it is opaque to the module and is
 * this host's own bookkeeping. */
extern struct ModuleHandle* mhost_mod;

/** Load the module and run its mi_init.
 * @param[in] path Shared object to open.
 * @param[in] name Name it was loaded by.
 * @param[out] why Filled in on failure.
 * @return Non-zero on success.
 */
extern int mhost_module_load(const char* path, const char* name,
                             const char* loaded_by, const char** why);

/** Run mi_fini and close the shared object. */
extern void mhost_module_unload(void);

/** Run mi_rehash. */
extern void mhost_module_rehash(void);

/** What the module declared, for the handshake. */
extern const char* mhost_module_name(void);
extern const char* mhost_module_version(void);
extern const char* mhost_module_description(void);

/*
 * Dispatch, from the frames the server sends.
 */

/** Run a command the module registered. */
extern void mhost_do_command(const struct ModHostFrame* f);

/** Run a hook the module registered, and answer if one is expected. */
extern void mhost_do_hook(const struct ModHostFrame* f);

/*
 * Clients.
 *
 * A blob off the wire is turned into a real struct Client so that the
 * macros in include/client.h -- cli_name(), IsAnOper(), MyConnect() --
 * work in a module's handler exactly as they do in the server.  The
 * struct is this host's, it holds only what the blob carried, and it is
 * released when the call that produced it returns.  A module that keeps
 * the pointer is keeping a pointer to freed memory, which is the same
 * rule a native module lives under.
 */

/** Build a client from #MH_CLI_FIELDS arguments starting at \a off.
 * @return The client, or NULL when the blob describes nobody.
 */
extern struct Client* mhost_client(const struct ModHostFrame* f,
                                   unsigned int off);

/** Release one. */
extern void mhost_client_free(struct Client* cptr);

/** The numnick a client came in with, for sending back. */
extern const char* mhost_client_numnick(const struct Client* cptr);

/** Release every client built for the call that is finishing. */
extern void mhost_clients_reset(void);

#endif /* INCLUDED_modhost_priv_h */
