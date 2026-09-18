/*
 * IRC - Internet Relay Chat, ircd/modhost/modhost_api.c
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
 * @brief The module API, as an isolated module sees it.
 *
 * A module is compiled once.  Whether it runs inside the server or inside
 * this process is the operator's line in the configuration, and the
 * module's own source says nothing about it -- because the symbols it
 * calls are defined here, with the same names and the same signatures,
 * and what they do is write a frame instead of touching the server.
 *
 * @section mhapi_profile What is here, and what is not
 *
 * Not all of it.  The module API is large, and a faithful copy of every
 * call would be the "duplicates the surface of the module API" that
 * proposal 006 §7.7 warns the cost is.  What is here is the profile an
 * *integration* needs, which is what isolation is for (§7.7: "integrations,
 * third-party modules, anything that runs a client's logic"):
 *
 *   - commands and hooks,
 *   - the log,
 *   - the allocator, the string helpers and ircd_snprintf(),
 *   - the features,
 *   - looking a client up, and sending a line to one,
 *   - the clock.
 *
 * Everything else -- user modes, channel modes, capabilities, bots, the
 * database, the cache, HTTP routes, workers, migrations, translations --
 * is **not** here, and a module that calls one of them does not load:
 * dlopen() is RTLD_NOW, so the failure is at load, in the operator's
 * face, naming the symbol.  That is the honest failure.  The dishonest
 * one would be a stub that silently does nothing, and a module that
 * believes it registered a user mode nobody has.
 *
 * Those are all things that reach deep into the server's own state, and
 * each is a protocol design of its own rather than a line in a table.
 * They are added when something needs them; the profile grows, the rule
 * does not change.
 *
 * @section mhapi_client Clients
 *
 * A handler is given a real @c struct @c Client, built here from what the
 * server sent, so that @c cli_name(), @c IsAnOper() and @c MyConnect()
 * work the way a module's author expects.  It is a **copy**: it holds
 * what was true when the frame was written, it belongs to this process,
 * and it is freed when the call returns.  A module that stores the
 * pointer is storing a pointer to freed memory -- which is exactly the
 * rule a native module already lives under, so nothing new has to be
 * learnt.
 */
#include "config.h"

#include "modhost_priv.h"

#include "client.h"
#include "hooks.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "module.h"
#include "msg.h"
#include "numnicks.h"
#include "send.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** This process's idea of the time, from #MH_TICK.
 *
 * The server's clock, not this one's: two processes reading gettimeofday()
 * separately is two answers, and a module comparing a timestamp the
 * server gave it against a time it read itself would be comparing them.
 */
time_t CurrentTime;

/** The server, as much of it as a module can see.
 *
 * A module writing sendcmdto_one(&me, ...) means "from this server", and
 * that has to resolve to something; what it resolves to is a client with
 * the server's name in it and nothing else.
 */
struct Client me;

/* ------------------------------------------------------------------- *
 * Clients                                                             *
 * ------------------------------------------------------------------- */

/** One client built for the call in progress. */
struct MHostClient {
  struct MHostClient* mhc_next;
  struct Client       mhc_client;
  struct User         mhc_user;
  char                mhc_numnick[16];
};

/** Every client built since the last mhost_clients_reset(). */
static struct MHostClient* mhost_clients;

struct Client* mhost_client(const struct ModHostFrame* f, unsigned int off)
{
  struct MHostClient* wrap;
  struct Client* cptr;
  unsigned int flags;
  const char* numnick = modhost_arg(f, off + MH_CLI_NUMNICK);
  const char* name = modhost_arg(f, off + MH_CLI_NAME);

  if (!*numnick && !*name)
    return 0;

  wrap = (struct MHostClient*) MyCalloc(1, sizeof(*wrap));
  cptr = &wrap->mhc_client;

  flags = (unsigned int) modhost_argi(f, off + MH_CLI_FLAGS, 0);

  ircd_strncpy(cli_name(cptr), name, HOSTLEN);
  ircd_strncpy(cli_info(cptr), modhost_arg(f, off + MH_CLI_REALNAME),
               REALLEN);
  ircd_strncpy(wrap->mhc_numnick, numnick, sizeof(wrap->mhc_numnick) - 1);

  if (!(flags & MH_CF_SERVER)) {
    cli_user(cptr) = &wrap->mhc_user;
    ircd_strncpy(cli_user(cptr)->username,
                 modhost_arg(f, off + MH_CLI_USER), USERLEN);
    ircd_strncpy(cli_user(cptr)->host, modhost_arg(f, off + MH_CLI_HOST),
                 HOSTLEN);
    ircd_strncpy(cli_user(cptr)->account,
                 modhost_arg(f, off + MH_CLI_ACCOUNT), NICKLEN);
  }

  /* MyConnect() is cli_from(cptr) == cptr, and cli_from() reads through
   * cli_connect(): a local client is one whose connection is its own.
   * There is no Connection here and there is nothing in it a module may
   * read, so it points at a shared empty one that exists only to make the
   * macro answer correctly. */
  if (flags & MH_CF_LOCAL) {
    static struct Connection mhost_conn;

    cli_connect(cptr) = &mhost_conn;
    con_client(cli_connect(cptr)) = cptr;
  }

  if (flags & MH_CF_OPER)
    SetOper(cptr);
  if (flags & MH_CF_SERVICE)
    SetServiceBot(cptr);
  if (flags & MH_CF_BOT)
    SetBot(cptr);
  if (flags & MH_CF_ACCOUNT)
    SetAccount(cptr);
  if (flags & MH_CF_FROZEN)
    SetFrozen(cptr);
  if (flags & MH_CF_SECURE)
    SetTLS(cptr);

  wrap->mhc_next = mhost_clients;
  mhost_clients = wrap;

  return cptr;
}

const char* mhost_client_numnick(const struct Client* cptr)
{
  struct MHostClient* wrap;

  for (wrap = mhost_clients; wrap; wrap = wrap->mhc_next)
    if (&wrap->mhc_client == cptr)
      return wrap->mhc_numnick;

  return "";
}

void mhost_clients_reset(void)
{
  while (mhost_clients) {
    struct MHostClient* wrap = mhost_clients;

    mhost_clients = wrap->mhc_next;
    MyFree(wrap);
  }
}

void mhost_client_free(struct Client* cptr)
{
  (void) cptr;
  /* Released together, by mhost_clients_reset(), when the call that made
   * them returns.  Freeing one early would leave a handler holding a
   * pointer it was still using. */
}

/* ------------------------------------------------------------------- *
 * The module API                                                      *
 * ------------------------------------------------------------------- */

/** Registrations this host is holding, so mi_fini need not. */
struct MHostReg {
  struct MHostReg* mhr_next;
  int              mhr_kind;    /**< 0 command, 1 hook. */
  int              mhr_type;    /**< Hook type. */
  char             mhr_name[32];
  MessageHandler   mhr_handlers[LAST_HANDLER_TYPE];
  HookFn           mhr_fn;
  void*            mhr_user;
};

static struct MHostReg* mhost_regs;

/** Find the handler for a command, and which handler type. */
MessageHandler mhost_find_command(const char* name, int htype)
{
  struct MHostReg* reg;

  for (reg = mhost_regs; reg; reg = reg->mhr_next)
    if (!reg->mhr_kind && !ircd_strcmp(reg->mhr_name, name))
      return (htype >= 0 && htype < LAST_HANDLER_TYPE)
           ? reg->mhr_handlers[htype] : 0;

  return 0;
}

/** Find a hook the module registered for \a type. */
struct MHostReg* mhost_find_hook(int type)
{
  struct MHostReg* reg;

  for (reg = mhost_regs; reg; reg = reg->mhr_next)
    if (reg->mhr_kind == 1 && reg->mhr_type == type)
      return reg;

  return 0;
}

HookFn mhost_hook_fn(const struct MHostReg* reg) { return reg->mhr_fn; }
void* mhost_hook_user(const struct MHostReg* reg) { return reg->mhr_user; }

int module_add_command(struct ModuleHandle* mod, const char* cmd,
                       const char* tok, unsigned int parameters,
                       unsigned int flags, MessageHandler handlers[])
{
  struct MHostReg* reg;
  const char* argv[5];
  char parbuf[16];
  char flagbuf[16];
  char maskbuf[16];
  unsigned int mask = 0;
  int i;

  (void) mod;

  if (!cmd || !*cmd || !handlers)
    return 0;

  reg = (struct MHostReg*) MyCalloc(1, sizeof(*reg));
  ircd_strncpy(reg->mhr_name, cmd, sizeof(reg->mhr_name) - 1);

  for (i = 0; i < LAST_HANDLER_TYPE; i++) {
    reg->mhr_handlers[i] = handlers[i];
    if (handlers[i])
      mask |= 1u << i;
  }

  reg->mhr_next = mhost_regs;
  mhost_regs = reg;

  ircd_snprintf(0, parbuf, sizeof(parbuf), "%u", parameters);
  ircd_snprintf(0, flagbuf, sizeof(flagbuf), "%u", flags);
  ircd_snprintf(0, maskbuf, sizeof(maskbuf), "%u", mask);

  argv[0] = cmd;
  argv[1] = tok ? tok : "";
  argv[2] = parbuf;
  argv[3] = flagbuf;
  argv[4] = maskbuf;

  return mhost_send(MH_ADD_COMMAND, 0, 5, argv, 0);
}

int module_del_command(struct ModuleHandle* mod, const char* cmd)
{
  struct MHostReg** pp;
  const char* argv[1];

  (void) mod;

  for (pp = &mhost_regs; *pp; pp = &(*pp)->mhr_next)
    if (!(*pp)->mhr_kind && !ircd_strcmp((*pp)->mhr_name, cmd)) {
      struct MHostReg* dead = *pp;

      *pp = dead->mhr_next;
      MyFree(dead);
      break;
    }

  argv[0] = cmd;

  return mhost_send(MH_DEL_COMMAND, 0, 1, argv, 0);
}

unsigned int module_command_count(const struct ModuleHandle* mod)
{
  struct MHostReg* reg;
  unsigned int n = 0;

  (void) mod;

  for (reg = mhost_regs; reg; reg = reg->mhr_next)
    if (!reg->mhr_kind)
      n++;

  return n;
}

int module_add_hook(struct ModuleHandle* mod, enum HookType type, HookFn fn,
                    int priority, void* user)
{
  struct MHostReg* reg;
  const char* argv[2];
  char typebuf[16];
  char pribuf[16];

  (void) mod;

  if (!fn)
    return 0;

  reg = (struct MHostReg*) MyCalloc(1, sizeof(*reg));
  reg->mhr_kind = 1;
  reg->mhr_type = (int) type;
  reg->mhr_fn = fn;
  reg->mhr_user = user;
  reg->mhr_next = mhost_regs;
  mhost_regs = reg;

  ircd_snprintf(0, typebuf, sizeof(typebuf), "%d", (int) type);
  ircd_snprintf(0, pribuf, sizeof(pribuf), "%d", priority);

  argv[0] = typebuf;
  argv[1] = pribuf;

  return mhost_send(MH_ADD_HOOK, 0, 2, argv, 0);
}

int module_del_hook(struct ModuleHandle* mod, enum HookType type, HookFn fn)
{
  struct MHostReg** pp;
  const char* argv[1];
  char typebuf[16];

  (void) mod;

  for (pp = &mhost_regs; *pp; pp = &(*pp)->mhr_next)
    if ((*pp)->mhr_kind == 1 && (*pp)->mhr_type == (int) type
        && (*pp)->mhr_fn == fn) {
      struct MHostReg* dead = *pp;

      *pp = dead->mhr_next;
      MyFree(dead);
      break;
    }

  ircd_snprintf(0, typebuf, sizeof(typebuf), "%d", (int) type);
  argv[0] = typebuf;

  return mhost_send(MH_DEL_HOOK, 0, 1, argv, 0);
}

int module_hook_resume(struct ModuleHandle* mod, hook_token_t token,
                       enum HookResult result, const char* reason)
{
  const char* argv[2];
  char resbuf[16];

  (void) mod;

  ircd_snprintf(0, resbuf, sizeof(resbuf), "%d", (int) result);
  argv[0] = resbuf;
  argv[1] = reason ? reason : "";

  return mhost_send(MH_HOOK_RESULT, (unsigned long) token, 2, argv, 0);
}

/* --- identity ------------------------------------------------------- */

static char mhost_name[64];
static char mhost_path[1024];
static char mhost_dir[1024];
static char mhost_loaded_by[64];

void mhost_identity(const char* name, const char* path, const char* loaded_by)
{
  char* slash;

  ircd_strncpy(mhost_name, name, sizeof(mhost_name) - 1);
  ircd_strncpy(mhost_path, path, sizeof(mhost_path) - 1);
  ircd_strncpy(mhost_dir, path, sizeof(mhost_dir) - 1);
  ircd_strncpy(mhost_loaded_by, loaded_by, sizeof(mhost_loaded_by) - 1);

  if ((slash = strrchr(mhost_dir, '/')))
    *slash = '\0';
}

const char* module_dir(const struct ModuleHandle* mod)
{
  (void) mod;
  return mhost_dir;
}

const char* module_loaded_by(const struct ModuleHandle* mod)
{
  (void) mod;
  return *mhost_loaded_by ? mhost_loaded_by : 0;
}

/* --- features ------------------------------------------------------- */

/* A native module passes an enum Feature, which is an index into a table
 * the server holds.  The same enum travels here, as a number, and that is
 * safe for exactly one reason: adding a feature shifts the enum, so it is
 * an ABI change, so IRCU_MODULE_ABI goes up -- and the handshake compares
 * the host's against the server's before a single frame of work is done.
 * A host and a server that disagree about what feature 41 is are a host
 * and a server that never spoke.
 */

/** Ask for one, by its enum value. */
static long mhost_feature(int req, enum Feature feat, struct ModHostFrame* f)
{
  const char* argv[1];
  char numbuf[16];

  ircd_snprintf(0, numbuf, sizeof(numbuf), "%d", (int) feat);
  argv[0] = numbuf;

  if (!mhost_request(req, 1, argv, f))
    return 0;

  return 1;
}

int feature_int(enum Feature feat)
{
  struct ModHostFrame f;

  if (!mhost_feature(MH_REQ_FEATURE_INT, feat, &f))
    return 0;

  return (int) modhost_argi(&f, 0, 0);
}

int feature_bool(enum Feature feat)
{
  struct ModHostFrame f;

  if (!mhost_feature(MH_REQ_FEATURE_BOOL, feat, &f))
    return 0;

  return (int) modhost_argi(&f, 0, 0);
}

const char* feature_str(enum Feature feat)
{
  static char value[512];
  struct ModHostFrame f;

  value[0] = '\0';

  if (!mhost_feature(MH_REQ_FEATURE_STR, feat, &f))
    return value;

  ircd_strncpy(value, modhost_arg(&f, 0), sizeof(value) - 1);

  return value;
}

/* --- finding and sending -------------------------------------------- */

/** A client the module asked for, kept until the call returns. */
static struct Client* mhost_lookup(int req, const char* key)
{
  struct ModHostFrame f;
  const char* argv[1];

  argv[0] = key;

  if (!mhost_request(req, 1, argv, &f))
    return 0;

  return mhost_client(&f, 0);
}

struct Client* FindUser(const char* name)
{
  return mhost_lookup(MH_REQ_FIND_CLIENT, name);
}

struct Client* FindClient(const char* name)
{
  return mhost_lookup(MH_REQ_FIND_CLIENT, name);
}

struct Client* findNUser(const char* numnick)
{
  return mhost_lookup(MH_REQ_FIND_NUMNICK, numnick);
}

/** Send one already-rendered line to one client.
 *
 * Every send a module can make funnels through here: the line is built in
 * this process, with the server's own ircd_snprintf() and its formats, and
 * what crosses is the finished bytes and a numnick.  The server looks the
 * numnick up again and writes the line if that client is still local to
 * it, so a module cannot address anything it was not given.
 */
static void mhost_send_line(struct Client* to, const char* line)
{
  const char* argv[2];
  const char* num = mhost_client_numnick(to);

  if (!num || !*num)
    return;

  argv[0] = num;
  argv[1] = line;

  mhost_send(MH_SEND, 0, 2, argv, 0);
}

void sendrawto_one(struct Client* to, const char* pattern, ...)
{
  char line[BUFSIZE];
  va_list vl;

  va_start(vl, pattern);
  ircd_vsnprintf(to, line, sizeof(line) - 2, pattern, vl);
  va_end(vl);

  mhost_send_line(to, line);
}

void sendcmdto_one(struct Client* from, const char* cmd, const char* tok,
                   struct Client* to, const char* pattern, ...)
{
  char line[BUFSIZE];
  char body[BUFSIZE];
  va_list vl;

  (void) tok;

  va_start(vl, pattern);
  ircd_vsnprintf(to, body, sizeof(body) - 2, pattern, vl);
  va_end(vl);

  /* A module's "from" is either the server or a client it was handed.
   * Either way the prefix is a name, because what goes to a client is
   * always the long form. */
  ircd_snprintf(to, line, sizeof(line) - 2, ":%s %s %s",
                from && cli_name(from)[0] ? cli_name(from) : cli_name(&me),
                cmd, body);

  mhost_send_line(to, line);
}

void sendto_one_numeric(struct Client* to, int numeric, const char* pattern,
                        ...)
{
  char line[BUFSIZE];
  char body[BUFSIZE];
  va_list vl;

  va_start(vl, pattern);
  ircd_vsnprintf(to, body, sizeof(body) - 2, pattern, vl);
  va_end(vl);

  ircd_snprintf(to, line, sizeof(line) - 2, ":%s %03d %s %s", cli_name(&me),
                numeric, cli_name(to), body);

  mhost_send_line(to, line);
}

/* --- the log -------------------------------------------------------- */

void log_write(enum LogSys subsys, enum LogLevel severity, unsigned int flags,
               const char* fmt, ...)
{
  char text[512];
  va_list vl;

  (void) subsys;
  (void) flags;

  va_start(vl, fmt);
  ircd_vsnprintf(0, text, sizeof(text), fmt, vl);
  va_end(vl);

  mhost_log((int) severity, "%s", text);
}

void log_vwrite(enum LogSys subsys, enum LogLevel severity,
                unsigned int flags, const char* fmt, va_list vl)
{
  char text[512];

  (void) subsys;
  (void) flags;

  ircd_vsnprintf(0, text, sizeof(text), fmt, vl);
  mhost_log((int) severity, "%s", text);
}

/* ------------------------------------------------------------------- *
 * What the shared server sources need to link                         *
 * ------------------------------------------------------------------- */

/* ircd_alloc.c, ircd_string.c, ircd_snprintf.c, match.c and numnicks.c
 * are compiled into this process unchanged -- they are pure, and a module
 * calling ircd_snprintf() has to get the server's one and not a lookalike.
 * They reach for three things the rest of the server would have provided.
 */

/** assert()'s re-entry guard, from include/ircd_log.h. */
int log_inassert;

#ifdef DEBUGMODE
/** Debug(), which the shared sources call in a debug build.
 *
 * It goes to the server's log like everything else this process has to
 * say, at debug level: a host writing to a terminal of its own would be
 * writing to whatever the server happened to leave on that descriptor.
 */
void debug(int level, const char* form, ...)
{
  char text[512];
  va_list vl;

  (void) level;

  va_start(vl, form);
  ircd_vsnprintf(0, text, sizeof(text), form, vl);
  va_end(vl);

  mhost_log(L_DEBUG, "%s", text);
}
#endif

/** ircd_snprintf()'s @c %v, which a module may use in a format. */
const char* visible_username(const struct Client* cptr)
{
  return cptr && cli_user(cptr) ? cli_user(cptr)->username : "";
}

/** numnicks.c calls this when a numnick names nobody.
 *
 * In the server that is a desync and the link goes.  Here the "link" is
 * the server itself, this process has no authority over anything, and the
 * client in question is a copy -- so it is a no-op with a line in the
 * log, which is the honest thing a host can do about it.
 */
int exit_client(struct Client* cptr, struct Client* bcptr,
                struct Client* sptr, const char* comment)
{
  (void) cptr;
  (void) bcptr;
  (void) sptr;

  mhost_log(L_WARNING, "the module asked to exit a client: %s",
            comment ? comment : "");

  return 0;
}
