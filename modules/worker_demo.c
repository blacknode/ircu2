/*
 * IRC - Internet Relay Chat, modules/worker_demo.c
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
 * @brief Hashes a password in a worker thread; the reference for worker.h.
 *
 * Adds /HASHPASS, an operator command that derives a key from a password
 * with PBKDF2-HMAC-SHA1 and hands it back.  The point is not the hash -- it
 * is that the hash is deliberately expensive, which is what a password hash
 * is for, and that the server keeps answering everybody else while it runs.
 *
 *   /HASHPASS hunter2                 -- 100000 iterations, about 0.1s
 *   /HASHPASS hunter2 5000000         -- several seconds, and the server
 *                                        stays responsive throughout
 *
 * Run the second one with WORKER_THREADS at 0 and the whole server freezes
 * for the duration; that comparison is the whole reason this module exists.
 *
 * Everything the worker touches came copied in the task: the password, the
 * salt and the iteration count.  It does not look at the client, at
 * CurrentTime, or at anything else the main thread might be changing.  The
 * SHA-1 routines it does call take a context the caller supplies and keep
 * no state of their own, which is what makes them safe to use from here;
 * see doc/readme.workers.
 */
#include "config.h"

#include "client.h"
#include "ircd.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_sha1.h"
#include "ircd_string.h"
#include "module.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "random.h"
#include "send.h"
#include "worker.h"

#include <stdlib.h>
#include <string.h>

/** Handle for this module, so the command handler can submit work. */
static struct ModuleHandle* demo_module;

/** Iterations when the operator does not say. */
#define DEMO_DEFAULT_ROUNDS 100000
/** The most we will do in one go, so a typo cannot occupy a thread for a day. */
#define DEMO_MAX_ROUNDS 50000000
/** Salt length, in bytes. */
#define DEMO_SALT_LEN 16
/** Longest password we will hash. */
#define DEMO_PASS_LEN 128

/** What the main thread hands to the worker.
 *
 * Plain data, copied: no pointers into anything the main thread owns.
 */
struct DemoRequest {
  unsigned int  dr_rounds;               /**< PBKDF2 iterations. */
  unsigned char dr_salt[DEMO_SALT_LEN];  /**< Salt. */
  char          dr_pass[DEMO_PASS_LEN + 1]; /**< Password, NUL-terminated. */
};

/** What the worker hands back. */
struct DemoResult {
  unsigned char dv_key[SHA1_DIGEST_LENGTH];   /**< Derived key. */
  unsigned char dv_salt[DEMO_SALT_LEN];       /**< Salt it used. */
  unsigned int  dv_rounds;                    /**< Iterations it did. */
};

/* ------------------------------------------------------------------------
 * The work.  Everything from here to the end of the section runs in a
 * worker thread and may touch nothing but its arguments.
 * ------------------------------------------------------------------------ */

/** SHA-1 block size, in bytes; HMAC's padding is defined in terms of it. */
#define SHA1_BLOCK 64

/** HMAC-SHA1 of \a data under \a key.
 * @param[in] key Key.
 * @param[in] keylen Length of \a key.
 * @param[in] data Message.
 * @param[in] datalen Length of \a data.
 * @param[out] out Receives the 20-byte tag.
 */
static void demo_hmac(const unsigned char* key, size_t keylen,
                      const unsigned char* data, size_t datalen,
                      unsigned char out[SHA1_DIGEST_LENGTH])
{
  unsigned char pad[SHA1_BLOCK];
  unsigned char inner[SHA1_DIGEST_LENGTH];
  unsigned char shortened[SHA1_DIGEST_LENGTH];
  SHA1_CTX ctx;
  size_t i;

  /* A key longer than the block is replaced by its own hash. */
  if (keylen > SHA1_BLOCK) {
    SHA1Init(&ctx);
    SHA1Update(&ctx, key, keylen);
    SHA1Final(shortened, &ctx);
    key = shortened;
    keylen = sizeof(shortened);
  }

  memset(pad, 0, sizeof(pad));
  memcpy(pad, key, keylen);
  for (i = 0; i < sizeof(pad); i++)
    pad[i] ^= 0x36;

  SHA1Init(&ctx);
  SHA1Update(&ctx, pad, sizeof(pad));
  SHA1Update(&ctx, data, datalen);
  SHA1Final(inner, &ctx);

  memset(pad, 0, sizeof(pad));
  memcpy(pad, key, keylen);
  for (i = 0; i < sizeof(pad); i++)
    pad[i] ^= 0x5c;

  SHA1Init(&ctx);
  SHA1Update(&ctx, pad, sizeof(pad));
  SHA1Update(&ctx, inner, sizeof(inner));
  SHA1Final(out, &ctx);
}

/** PBKDF2-HMAC-SHA1, one output block.  Runs in a worker thread.
 *
 * The loop is the point: every iteration is one HMAC, and the operator
 * chooses how many.  That is where the seconds go.
 */
static void demo_work(struct WorkTask* task)
{
  const struct DemoRequest* req = (const struct DemoRequest*) task->wt_in;
  struct DemoResult* res;
  unsigned char block[DEMO_SALT_LEN + 4];
  unsigned char u[SHA1_DIGEST_LENGTH];
  unsigned char acc[SHA1_DIGEST_LENGTH];
  unsigned int round;
  size_t i;

  /* worker_alloc(), never MyMalloc(): the core's pools are not locked. */
  if (!(res = (struct DemoResult*) worker_alloc(sizeof(*res)))) {
    task->wt_status = -1;
    return;
  }

  /* U1 = HMAC(pass, salt || INT(1)) */
  memcpy(block, req->dr_salt, DEMO_SALT_LEN);
  block[DEMO_SALT_LEN + 0] = 0;
  block[DEMO_SALT_LEN + 1] = 0;
  block[DEMO_SALT_LEN + 2] = 0;
  block[DEMO_SALT_LEN + 3] = 1;

  demo_hmac((const unsigned char*) req->dr_pass, strlen(req->dr_pass),
            block, sizeof(block), u);
  memcpy(acc, u, sizeof(acc));

  for (round = 1; round < req->dr_rounds; round++) {
    demo_hmac((const unsigned char*) req->dr_pass, strlen(req->dr_pass),
              u, sizeof(u), u);
    for (i = 0; i < sizeof(acc); i++)
      acc[i] ^= u[i];
  }

  memcpy(res->dv_key, acc, sizeof(res->dv_key));
  memcpy(res->dv_salt, req->dr_salt, sizeof(res->dv_salt));
  res->dv_rounds = req->dr_rounds;

  task->wt_out = res;
  task->wt_out_len = sizeof(*res);
  task->wt_status = 0;
}

/* ------------------------------------------------------------------------
 * Back in the main thread.
 * ------------------------------------------------------------------------ */

/** Render \a len bytes of \a in as lowercase hex into \a out. */
static void demo_hex(const unsigned char* in, size_t len, char* out)
{
  static const char digits[] = "0123456789abcdef";
  size_t i;

  for (i = 0; i < len; i++) {
    out[i * 2]     = digits[in[i] >> 4];
    out[i * 2 + 1] = digits[in[i] & 0x0f];
  }
  out[len * 2] = '\0';
}

/** Deliver the hash.  Runs in the main thread.
 *
 * The client is looked up rather than remembered: several seconds passed,
 * and a user who asked for fifty million rounds and then quit is exactly
 * the case a stored pointer would get wrong.
 */
static void demo_done(struct WorkTask* task)
{
  const struct DemoResult* res = (const struct DemoResult*) task->wt_out;
  struct Client* sptr = worker_task_client(task);
  char keyhex[SHA1_DIGEST_LENGTH * 2 + 1];
  char salthex[DEMO_SALT_LEN * 2 + 1];

  if (!sptr)
    return;   /* they left while we were working; nothing to answer */

  if (task->wt_status || !res) {
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :HASHPASS: the worker failed",
                  sptr);
    return;
  }

  demo_hex(res->dv_key, sizeof(res->dv_key), keyhex);
  demo_hex(res->dv_salt, sizeof(res->dv_salt), salthex);

  sendcmdto_one(&me, CMD_NOTICE, sptr,
                "%C :HASHPASS: pbkdf2-sha1$%u$%s$%s", sptr,
                res->dv_rounds, salthex, keyhex);
}

/** Handle HASHPASS from an operator.
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 * @return Zero.
 */
static int mo_hashpass(struct Client* cptr, struct Client* sptr,
                       int parc, char* parv[])
{
  struct DemoRequest* req;
  struct WorkTask* task;
  unsigned long rounds = DEMO_DEFAULT_ROUNDS;
  unsigned int i;

  if (parc < 2 || EmptyString(parv[1]))
    return need_more_params(sptr, "HASHPASS");

  if (parc > 2 && !EmptyString(parv[2])) {
    rounds = strtoul(parv[2], NULL, 10);
    if (rounds < 1 || rounds > DEMO_MAX_ROUNDS) {
      sendcmdto_one(&me, CMD_NOTICE, sptr,
                    "%C :HASHPASS: iterations must be between 1 and %u",
                    sptr, (unsigned int) DEMO_MAX_ROUNDS);
      return 0;
    }
  }

  if (!(task = worker_task_new(demo_work, demo_done))) {
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :HASHPASS: out of memory", sptr);
    return 0;
  }

  if (!(req = (struct DemoRequest*) worker_alloc(sizeof(*req)))) {
    worker_task_free(task);
    sendcmdto_one(&me, CMD_NOTICE, sptr, "%C :HASHPASS: out of memory", sptr);
    return 0;
  }

  /* The salt is drawn here, in the main thread: ircrandom() keeps state of
   * its own and is not safe to call from a worker.  Handing the worker the
   * finished bytes is the pattern -- whatever it needs, it gets copied.
   */
  for (i = 0; i < DEMO_SALT_LEN; i++)
    req->dr_salt[i] = (unsigned char) (ircrandom() & 0xff);

  ircd_strncpy(req->dr_pass, parv[1], DEMO_PASS_LEN);
  req->dr_rounds = (unsigned int) rounds;

  task->wt_in = req;
  task->wt_in_len = sizeof(*req);

  /* The numnick, not the pointer.  See worker_task_set_client(). */
  worker_task_set_client(task, sptr);

  if (!module_submit_work(demo_module, task)) {
    worker_task_free(task);
    sendcmdto_one(&me, CMD_NOTICE, sptr,
                  "%C :HASHPASS: no workers available. Set WORKER_THREADS to "
                  "a non-zero value, or wait for the queue to drain.", sptr);
    return 0;
  }

  sendcmdto_one(&me, CMD_NOTICE, sptr,
                "%C :HASHPASS: working on %u iterations; the answer will "
                "follow. Notice that the server has not stopped.", sptr,
                req->dr_rounds);

  return 0;
}

/** Register the command.
 * @param[in] mod Handle for this module.
 * @return Zero on success, non-zero to refuse to load.
 */
static int demo_init(struct ModuleHandle* mod)
{
  MessageHandler handlers[LAST_HANDLER_TYPE];

  demo_module = mod;

  handlers[UNREGISTERED_HANDLER] = NULL;
  handlers[CLIENT_HANDLER]       = NULL;   /* opers only: it costs CPU */
  handlers[SERVER_HANDLER]       = NULL;
  handlers[OPER_HANDLER]         = mo_hashpass;
  handlers[SERVICE_HANDLER]      = NULL;

  if (!module_add_command(mod, "HASHPASS", "HASHPASS", 3, MFLG_SLOW,
                          handlers))
    return -1;

  /* Deliberately not a failure: the module loads with workers off and says
   * so when the command is used.  A server that turns WORKER_THREADS on
   * later should not have to remember to load this again.
   */
  if (!worker_enabled())
    log_write(LS_SYSTEM, L_WARNING, 0,
              "worker_demo: loaded, but WORKER_THREADS is 0, so /HASHPASS "
              "will refuse work until it is set");

  return 0;
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "worker_demo",
  "1.0.0",
  "ircu developers",
  "Adds /HASHPASS; the reference for the worker API",
  demo_init,
  NULL,           /* the loader removes the command and cancels the work */
  NULL
};
