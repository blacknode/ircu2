/*
 * IRC - Internet Relay Chat, modules/workers/sendmail/sendmail.c
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
 * @brief The mail provider that ships: hand the message to the local MTA.
 *
 * The default because it is the one that needs nothing: every host that
 * sends mail at all has a @c sendmail, the interface has not changed in
 * forty years, and the queue, the retries, the TLS and the reputation are
 * somebody else's problem -- which is exactly where they belong.  A module
 * that speaks SMTP to a relay can register instead; the core does not
 * care which, and the @c Mail{} block is where it is told (see mail.h).
 *
 * **The program runs on a worker**, never on the main thread: fork, exec,
 * write and wait are all blocking, and an MTA that takes a second to
 * accept a message would be a second in which this server did nothing at
 * all.  What crosses to the worker is bytes -- the message, already
 * composed -- and what comes back is an exit status, which is the rule
 * worker.h states: no core state, no allocator, no log, no client.
 *
 * The message is built here and not in the core because it is the format
 * this program expects: RFC 5322 headers and a body, on stdin, with
 * @c -t to read the recipient from the headers and @c -i so a lone dot
 * does not end it early.
 */
#include "config.h"

#include "ircd_log.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "mail.h"
#include "module.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/** Where the MTA lives when the Mail{} block does not say. */
#define SENDMAIL_DEFAULT "/usr/sbin/sendmail"

/** Longest message this will hand over, headers included. */
#define SENDMAIL_MAX (MAIL_BODY_MAX + 1024)

/** This module's handle. */
static struct ModuleHandle* sendmail_mod;

/** What crosses to the worker and back.
 *
 * Plain bytes in one allocation: a worker may not touch anything the main
 * thread owns, so the program to run and the message to feed it are
 * copied in, and the only thing that comes back is a status and a line
 * for the log.
 */
struct SendmailWork {
  mail_id_t sw_id;                   /**< The core's handle. */
  char      sw_program[256];         /**< Program to run. */
  char      sw_sender[MAIL_ADDRESS_MAX + 1]; /**< Envelope sender. */
  char      sw_message[SENDMAIL_MAX];/**< Headers and body. */
  size_t    sw_len;                  /**< Bytes of #sw_message. */
  char      sw_detail[128];          /**< What went wrong, for the log. */
};

/** Write \a len bytes of \a buf to \a fd, or say why not.
 *
 * In a worker thread: no logging, no allocation, nothing but the write.
 * @return Non-zero on success.
 */
static int sendmail_write_all(int fd, const char* buf, size_t len)
{
  while (len > 0) {
    ssize_t wrote = write(fd, buf, len);

    if (wrote < 0) {
      if (errno == EINTR)
        continue;
      return 0;
    }

    buf += wrote;
    len -= (size_t) wrote;
  }

  return 1;
}

/** Run the program and feed it the message.  Worker thread.
 *
 * @param[in,out] task The work; #WorkTask::wt_in is a #SendmailWork.
 */
static void sendmail_run(struct WorkTask* task)
{
  struct SendmailWork* work = (struct SendmailWork*) task->wt_in;
  int pipefd[2];
  pid_t pid;
  int status = 0;
  int fed;

  task->wt_status = -1;

  if (pipe(pipefd) != 0) {
    ircd_strncpy(work->sw_detail, "could not make a pipe",
                 sizeof(work->sw_detail) - 1);
    return;
  }

  if ((pid = fork()) < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    ircd_strncpy(work->sw_detail, "could not fork",
                 sizeof(work->sw_detail) - 1);
    return;
  }

  if (pid == 0) {
    /* The child.  Nothing here may return: this side of a fork in a
     * threaded process may call only what is async-signal-safe, and the
     * one thing it is about to do is exec.
     */
    char* argv[6];
    int devnull;

    close(pipefd[1]);

    if (dup2(pipefd[0], STDIN_FILENO) < 0)
      _exit(127);
    close(pipefd[0]);

    /* The MTA's own chatter is not this server's log.  Its exit status is
     * what says whether the message was accepted. */
    if ((devnull = open("/dev/null", O_WRONLY)) >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO)
        close(devnull);
    }

    argv[0] = work->sw_program;
    argv[1] = (char*) "-t";   /* recipients come from the headers */
    argv[2] = (char*) "-i";   /* a lone dot is not the end of the message */
    argv[3] = (char*) "-f";
    argv[4] = work->sw_sender;
    argv[5] = NULL;

    execv(work->sw_program, argv);
    _exit(127);
  }

  /* The parent. */
  close(pipefd[0]);

  fed = sendmail_write_all(pipefd[1], work->sw_message, work->sw_len);

  close(pipefd[1]);

  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      ircd_strncpy(work->sw_detail, "could not wait for the program",
                   sizeof(work->sw_detail) - 1);
      return;
    }
  }

  if (!fed) {
    ircd_strncpy(work->sw_detail, "the program closed its input early",
                 sizeof(work->sw_detail) - 1);
    return;
  }

  if (WIFSIGNALED(status)) {
    ircd_snprintf(0, work->sw_detail, sizeof(work->sw_detail),
                  "the program died on signal %d", WTERMSIG(status));
    return;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    ircd_snprintf(0, work->sw_detail, sizeof(work->sw_detail),
                  "the program exited %d",
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return;
  }

  task->wt_status = 0;
}

/** Tell the core what happened.  Main thread.
 * @param[in] task The finished work.
 */
static void sendmail_done(struct WorkTask* task)
{
  struct SendmailWork* work = (struct SendmailWork*) task->wt_in;

  if (!work)
    return;

  if (task->wt_status == 0)
    mail_complete(work->sw_id, MAIL_OK, NULL);
  else
    mail_complete(work->sw_id, MAIL_ERR_FAILED,
                  *work->sw_detail ? work->sw_detail : NULL);
}

/** Build the message the program is fed.
 *
 * @param[in,out] work Where to build it.
 * @param[in] msg What to send.
 * @return Non-zero on success.
 */
static int sendmail_compose(struct SendmailWork* work,
                            const struct MailMessage* msg)
{
  unsigned int len;

  /* Date and Message-ID are the MTA's to add; what cannot be left out is
   * the envelope, which is what -t reads.  The core has already refused
   * anything with a newline in the address or the subject, which is what
   * would otherwise turn one of these lines into several.
   */
  len = ircd_snprintf(0, work->sw_message, sizeof(work->sw_message),
                      "From: %s\r\n"
                      "To: %s\r\n"
                      "Subject: %s\r\n"
                      "MIME-Version: 1.0\r\n"
                      "Content-Type: text/plain; charset=UTF-8\r\n"
                      "Auto-Submitted: auto-generated\r\n"
                      "\r\n"
                      "%s\r\n",
                      msg->mm_from, msg->mm_to, msg->mm_subject,
                      msg->mm_body);

  if (len >= sizeof(work->sw_message))
    return 0;

  work->sw_len = len;

  return 1;
}

/** Accept a message from the core.
 * @param[in] id Handle to answer with.
 * @param[in] msg What to send.
 * @return #MAIL_OK when the work was queued.
 */
static enum MailError sendmail_send(mail_id_t id,
                                    const struct MailMessage* msg)
{
  const struct MailConf* conf = mail_conf();
  struct SendmailWork* work;
  struct WorkTask* task;

  if (!(task = worker_task_new(sendmail_run, sendmail_done)))
    return MAIL_ERR_FAILED;

  if (!(work = (struct SendmailWork*) worker_alloc(sizeof(*work)))) {
    worker_task_free(task);
    return MAIL_ERR_FAILED;
  }

  memset(work, 0, sizeof(*work));
  work->sw_id = id;

  ircd_strncpy(work->sw_program,
               (conf && !EmptyString(conf->mconf_program))
                 ? conf->mconf_program : SENDMAIL_DEFAULT,
               sizeof(work->sw_program) - 1);
  ircd_strncpy(work->sw_sender, msg->mm_from, sizeof(work->sw_sender) - 1);

  if (!sendmail_compose(work, msg)) {
    worker_free(work);
    worker_task_free(task);
    return MAIL_ERR_TOOLONG;
  }

  task->wt_in = work;

  if (!module_submit_work(sendmail_mod, task)) {
    /* Worker threads are off.  Running the program here would stop the
     * server for as long as the MTA took, so this is the honest failure:
     * the server is not configured to send mail.
     */
    log_write(LS_SYSTEM, L_ERROR, 0,
              "sendmail: no worker threads (FEAT_WORKER_THREADS is zero), "
              "so no mail can be sent");
    worker_task_free(task);
    return MAIL_ERR_FAILED;
  }

  return MAIL_OK;
}

/** Forget a message the core has given up on.
 *
 * Nothing to do: the program is either already running -- and killing an
 * MTA half way through a message is how a message is delivered twice --
 * or queued, where it will run and find its handle gone, which
 * mail_complete() answers with a no-op.
 * @param[in] id The handle.
 */
static void sendmail_cancel(mail_id_t id)
{
  (void) id;
}

/** What this module registers. */
static const struct MailProvider sendmail_provider = {
  "sendmail",
  sendmail_send,
  sendmail_cancel
};

/** Register as the mail provider.
 * @param[in] mod Handle for this module.
 * @return Zero on success.
 */
static int sendmail_init(struct ModuleHandle* mod)
{
  sendmail_mod = mod;

  if (!mail_register_provider(mod, &sendmail_provider))
    return -1;

  return 0;
}

/** Withdraw, failing anything still out.
 * @param[in] mod Handle for this module.
 */
static void sendmail_fini(struct ModuleHandle* mod)
{
  mail_unregister_provider(mod);
  sendmail_mod = NULL;
}

/** The one symbol the server looks for. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "sendmail",
  "1.0.0",
  "ircu developers",
  "Hands outgoing mail to the local MTA, on a worker thread",
  sendmail_init,
  sendmail_fini,
  NULL
};
