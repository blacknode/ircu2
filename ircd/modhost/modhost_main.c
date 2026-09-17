/*
 * IRC - Internet Relay Chat, ircd/modhost/modhost_main.c
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
 * @brief ircu-modhost: one module, one process.
 *
 * Started by the server, never by hand.  It inherits one descriptor --
 * #MODHOST_FD -- and is given the module's path and the name it was
 * loaded by; everything else it knows, it asks for.
 *
 * The whole program is: dlopen the module, run its mi_init, say READY,
 * then read frames until the socket closes.  There is no event loop and
 * nothing is asynchronous, because this process has exactly one thing to
 * do and is allowed to take as long over it as the module does.
 *
 * @section mhmain_limits What is put on it
 *
 * Before the module is opened the process takes rlimits of its own
 * (proposal 006 §7.7, "the cheap thing, and from now on"): a cap on
 * address space, on CPU, on files and on core size.  They do not protect
 * the server's memory -- nothing inside a process does -- but they bound
 * what a module can consume before the operating system stops it, and a
 * host that is stopped is a host the server notices going.
 *
 * It also drops into its own process group, so that a signal meant for
 * the server's group does not reach it and, more to the point, so the
 * server can take the whole group down if it has to.
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

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

/** Address space a module may have, in bytes.
 *
 * Generous for anything that is not a database, and small enough that a
 * leak stops here rather than on the machine.
 */
#define MHOST_RLIMIT_AS (512UL * 1024UL * 1024UL)

/** CPU seconds.  A module in an infinite loop is killed by the kernel
 * rather than noticed by a human. */
#define MHOST_RLIMIT_CPU 3600

/** Open files. */
#define MHOST_RLIMIT_FILES 64

/* From modhost_api.c. */
extern void mhost_identity(const char* name, const char* path,
                           const char* loaded_by);
extern MessageHandler mhost_find_command(const char* name, int htype);
extern struct MHostReg* mhost_find_hook(int type);
extern HookFn mhost_hook_fn(const struct MHostReg* reg);
extern void* mhost_hook_user(const struct MHostReg* reg);

/** The shared object, and what it exported. */
static void* mhost_dl;
static struct ModuleInfo* mhost_info;

/** A handle for the module.  Never dereferenced by it or by us: it is an
 * identity token, and the server's own handle lives in the server. */
static struct ModuleHandle* mhost_handle = (struct ModuleHandle*) &mhost_dl;

struct ModuleHandle* mhost_mod;

const char* mhost_module_name(void)
{
  return mhost_info && mhost_info->mi_name ? mhost_info->mi_name : "";
}

const char* mhost_module_version(void)
{
  return mhost_info && mhost_info->mi_version ? mhost_info->mi_version : "";
}

const char* mhost_module_description(void)
{
  return mhost_info && mhost_info->mi_description ? mhost_info->mi_description
                                                  : "";
}

int mhost_module_load(const char* path, const char* name,
                      const char* loaded_by, const char** why)
{
  static char errbuf[512];

  mhost_identity(name, path, loaded_by);
  mhost_mod = mhost_handle;

  /* RTLD_NOW, like the server: a module that reaches for a part of the
   * API this host does not implement fails here, naming the symbol, and
   * not in the middle of a hook six weeks later.  That is the whole
   * reason the profile in modhost_api.c is a set of definitions rather
   * than a set of stubs -- see the note there. */
  mhost_dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);

  if (!mhost_dl) {
    snprintf(errbuf, sizeof(errbuf), "%s", dlerror());
    *why = errbuf;
    return 0;
  }

  mhost_info = (struct ModuleInfo*) dlsym(mhost_dl, "ircu_module");

  if (!mhost_info) {
    *why = "no ircu_module symbol; is this an ircu module?";
    return 0;
  }

  if (mhost_info->mi_abi != IRCU_MODULE_ABI) {
    snprintf(errbuf, sizeof(errbuf),
             "ABI mismatch: module was built for %u, the host speaks %u",
             mhost_info->mi_abi, (unsigned int) IRCU_MODULE_ABI);
    *why = errbuf;
    return 0;
  }

  if (!mhost_info->mi_name || !*mhost_info->mi_name) {
    *why = "module declares no name";
    return 0;
  }

  if (mhost_info->mi_init && (*mhost_info->mi_init)(mhost_handle)) {
    snprintf(errbuf, sizeof(errbuf), "module %s refused to initialise",
             mhost_info->mi_name);
    *why = errbuf;
    return 0;
  }

  return 1;
}

void mhost_module_unload(void)
{
  if (mhost_info && mhost_info->mi_fini)
    (*mhost_info->mi_fini)(mhost_handle);

  mhost_info = 0;

  /* dlclose() is deliberately not called.  The process is about to exit,
   * so nothing is reclaimed by it, and a module whose mi_fini left a
   * thread or an atexit handler behind would be unmapped underneath it --
   * a crash in the last instant of a process that was about to go
   * quietly, and one that would look like the module having failed.
   */
}

void mhost_module_rehash(void)
{
  if (mhost_info && mhost_info->mi_rehash)
    (*mhost_info->mi_rehash)(mhost_handle);
}

/* ------------------------------------------------------------------- *
 * Dispatch                                                            *
 * ------------------------------------------------------------------- */

void mhost_do_command(const struct ModHostFrame* f)
{
  char* parv[MAXPARA + 2];
  struct Client* sptr;
  MessageHandler handler;
  const char* name = modhost_arg(f, 0);
  int htype = (int) modhost_argi(f, 1, CLIENT_HANDLER);
  unsigned int first = 2 + MH_CLI_FIELDS;
  unsigned int i;
  int parc = 0;

  handler = mhost_find_command(name, htype);

  if (!handler)
    return;

  sptr = mhost_client(f, 2);

  if (!sptr)
    return;

  /* parv[0] is the source, as everywhere else in this tree. */
  parv[parc++] = (char*) cli_name(sptr);

  for (i = first; i < f->mhf_argc && parc <= MAXPARA; i++)
    parv[parc++] = (char*) modhost_arg(f, i);

  parv[parc] = 0;

  /* cptr and sptr are the same here: an isolated module is only ever
   * given the client the command came from, never the link it arrived
   * on, because a link is a thing it has no business touching. */
  (*handler)(sptr, sptr, parc, parv);

  mhost_clients_reset();

  mhost_send(MH_COMMAND_DONE, f->mhf_serial, 0, 0, 0);
}

void mhost_do_hook(const struct ModHostFrame* f)
{
  struct HookContext ctx;
  struct MHostReg* reg;
  enum HookResult res;
  int type = (int) modhost_argi(f, 0, -1);

  reg = mhost_find_hook(type);

  if (!reg) {
    /* Not registered here.  If the server is waiting for an answer it
     * still gets one, or it would hold the operation to its deadline for
     * a hook that was never going to run. */
    const char* argv[2];
    char resbuf[16];

    ircd_snprintf(0, resbuf, sizeof(resbuf), "%d", (int) HOOK_CONTINUE);
    argv[0] = resbuf;
    argv[1] = "";
    mhost_send(MH_HOOK_RESULT, f->mhf_serial, 2, argv, 0);
    return;
  }

  memset(&ctx, 0, sizeof(ctx));
  ctx.hc_arg = modhost_arg(f, 1);
  ctx.hc_client = mhost_client(f, 3);
  ctx.hc_source = mhost_client(f, 3 + MH_CLI_FIELDS);

  /* The token is the frame's serial, which is what the answer is paired
   * with.  A module that returns HOOK_PENDING keeps it and calls
   * module_hook_resume() later, exactly as a native one does; the number
   * it holds means something to the server, not here. */
  ctx.hc_token = (hook_token_t) f->mhf_serial;

  res = (*mhost_hook_fn(reg))(&ctx, mhost_hook_user(reg));

  if (res != HOOK_PENDING) {
    const char* argv[2];
    char resbuf[16];

    ircd_snprintf(0, resbuf, sizeof(resbuf), "%d", (int) res);
    argv[0] = resbuf;
    argv[1] = ctx.hc_reason;
    mhost_send(MH_HOOK_RESULT, f->mhf_serial, 2, argv, 0);
  }

  mhost_clients_reset();
}

/* ------------------------------------------------------------------- *
 * The program                                                         *
 * ------------------------------------------------------------------- */

/** Say what went wrong and leave.  The server prints it for an operator. */
static void mhost_die(const char* why)
{
  const char* argv[1];

  argv[0] = why;
  mhost_send(MH_ERROR, 0, 1, argv, 0);

  _exit(1);
}

/** Bound what this process can consume.
 *
 * Not protection -- a module shares this address space and can write
 * anywhere in it, which is why the process boundary is where the
 * protection is.  This is the cheap part of proposal 006 §7.7: a leak, a
 * runaway loop or a descriptor flood ends here, and the server sees a
 * host go rather than a machine.
 */
static void mhost_limits(void)
{
  struct rlimit rl;

  rl.rlim_cur = rl.rlim_max = MHOST_RLIMIT_AS;
  setrlimit(RLIMIT_AS, &rl);

  rl.rlim_cur = rl.rlim_max = MHOST_RLIMIT_CPU;
  setrlimit(RLIMIT_CPU, &rl);

  rl.rlim_cur = rl.rlim_max = MHOST_RLIMIT_FILES;
  setrlimit(RLIMIT_NOFILE, &rl);

  /* A core from a host would be a core of the module's memory, which may
   * hold whatever the module was handed. */
  rl.rlim_cur = rl.rlim_max = 0;
  setrlimit(RLIMIT_CORE, &rl);
}

int main(int argc, char* argv[])
{
  struct ModHostFrame f;
  const char* why = 0;
  const char* ready[4];
  char verbuf[16];
  char abibuf[16];

  if (argc < 3) {
    fprintf(stderr, "ircu-modhost is started by the ircd, not by hand\n");
    return 2;
  }

  /* A module that writes to stdout would write into whatever the server
   * left there.  Its log is log_write(), which is a frame. */
  freopen("/dev/null", "r", stdin);
  freopen("/dev/null", "w", stdout);

  setpgid(0, 0);
  mhost_limits();

  /* A module that ignores SIGPIPE and one that does not are the same to
   * the server; what matters is that this process notices the socket
   * closing as a read of zero rather than as a signal. */
  signal(SIGPIPE, SIG_IGN);

  if (!mhost_read(&f))
    return 1;

  if (f.mhf_verb != MH_HELLO)
    mhost_die("the server did not say hello");

  if (modhost_argi(&f, 0, -1) != MODHOST_VERSION)
    mhost_die("the server and the module host speak different protocols; "
              "they are installed together and have to be upgraded together");

  /* Before the module runs: it may render a line from the server on its
   * very first call, and a prefix of "" is not one a client can read. */
  ircd_strncpy(cli_name(&me), modhost_arg(&f, 4), HOSTLEN);

  if (!mhost_module_load(modhost_arg(&f, 2), modhost_arg(&f, 1),
                         modhost_arg(&f, 3), &why))
    mhost_die(why ? why : "the module would not load");

  ircd_snprintf(0, verbuf, sizeof(verbuf), "%d", MODHOST_VERSION);
  ircd_snprintf(0, abibuf, sizeof(abibuf), "%u",
                (unsigned int) IRCU_MODULE_ABI);

  ready[0] = mhost_module_name();
  ready[1] = mhost_module_version();
  ready[2] = mhost_module_description();
  ready[3] = abibuf;

  /* Whatever mi_init registered has already gone out, ahead of this: the
   * server reads what is in the buffer behind the handshake, so the
   * module's commands and hooks are in place the moment it is loaded. */
  if (!mhost_send(MH_READY, 0, 4, ready, 0))
    return 1;

  while (mhost_read(&f)) {
    switch (f.mhf_verb) {
    case MH_TICK:
      CurrentTime = (time_t) modhost_argi(&f, 0, 0);
      ircd_strncpy(cli_name(&me), modhost_arg(&f, 1), HOSTLEN);
      break;

    case MH_COMMAND:
      mhost_do_command(&f);
      break;

    case MH_HOOK:
      mhost_do_hook(&f);
      break;

    case MH_REHASH:
      mhost_module_rehash();
      break;

    case MH_FINI:
      mhost_module_unload();
      return 0;

    default:
      /* A verb from a newer server.  Ignored rather than fatal: the
       * handshake already agreed the protocol version, so this is a
       * server sending something this build does not act on, and
       * dropping the module over it would be worse than not acting. */
      break;
    }
  }

  /* The socket closed without an MH_FINI: the server is gone, or it
   * decided this host was.  Either way the module gets its mi_fini --
   * this process is the only thing that can give it one. */
  mhost_module_unload();

  return 0;
}
