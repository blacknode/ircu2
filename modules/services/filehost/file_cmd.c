/*
 * IRC - Internet Relay Chat, modules/services/filehost/file_cmd.c
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
 * @brief @c /FILE -- how a client that speaks only IRC uploads a file.
 *
 * @verbatim
 *   FILE UPLOAD [<target>]   ask for somewhere to put one
 *   FILE LIST                what you are holding
 *   FILE DELETE <id>         take one back
 * @endverbatim
 *
 * The server does not receive the file over IRC.  It mints a ticket --
 * a signed statement that this account asked to upload one, good for
 * fifteen minutes -- and tells the client where to PUT it.  Anything that
 * can make an HTTP request can then do the upload, including curl, which
 * is what makes this usable from a client written in 1998.
 *
 * **Only an identified client may upload.**  The only durable handle on
 * a person here is the account the network's services vouched for:
 * filing an upload under a bare nick would put it under whoever wears
 * that nick next week, and quota, listing and deletion all hang off that
 * name.
 */
#include "config.h"

#include "filehost.h"

#include "handlers.h"
#include "ircd_alloc.h"
#include "numnicks.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "ircd_token.h"
#include "msg.h"
#include "numeric.h"
#include "s_user.h"
#include "send.h"

#include <string.h>

/** Say one line to a client, as this command's own voice. */
static void file_tell(struct Client* cptr, const char* pattern, ...)
{
  char text[BUFSIZE];
  va_list vl;

  va_start(vl, pattern);
  ircd_vsnprintf(cptr, text, sizeof(text), pattern, vl);
  va_end(vl);

  sendcmdto_one(&me, CMD_NOTICE, cptr, "%C :%s", cptr, text);
}

/** What a client that cannot upload is told, and why.
 * @return Non-zero if it may not.
 */
static int file_refused(struct Client* sptr)
{
  if (!file_store_ready()) {
    file_tell(sptr, "This server is not hosting files.");
    return 1;
  }

  if (!IsAccount(sptr)) {
    file_tell(sptr, "Identify to an account first: a file is filed under "
                    "the nickname you proved, and that is the only name "
                    "that still means you tomorrow.");
    return 1;
  }

  return 0;
}

/* ------------------------------------------------------------------- *
 * UPLOAD                                                              *
 * ------------------------------------------------------------------- */

/** What the quota answer is about. */
struct FileAsk {
  char fa_numnick[16];       /**< Who asked; never a pointer. */
  char fa_target[CHANNELLEN + 1];
};

/** Find the client that asked, if it is still here. */
static struct Client* file_asker(const char* numnick)
{
  return findNUser(numnick);
}

/** Mint the ticket and tell the client where to send the file. */
static void file_upload_ready(long long used, int ok, void* user)
{
  struct FileAsk* ask = (struct FileAsk*) user;
  struct Client* sptr = file_asker(ask->fa_numnick);
  char id[FILE_ID_LEN + 1];
  char payload[TOKEN_PAYLOAD_MAX + 1];
  char ticket[TOKEN_MAX + 1];
  char url[HTTP_PATH_MAX + 1];
  int quota = feature_int(FEAT_FILEHOST_QUOTA);
  unsigned int max = http_upload_max();

  if (!sptr) {
    MyFree(ask);
    return;
  }

  if (!ok) {
    file_tell(sptr, "The file store is not answering; try again later.");
    MyFree(ask);
    return;
  }

  if (quota > 0 && used >= quota) {
    file_tell(sptr, "You are holding %lld bytes and the limit is %d.  "
                    "Delete something with FILE DELETE <id>.", used, quota);
    MyFree(ask);
    return;
  }

  file_id_make(id, sizeof(id));

  /* <id>:<account>:<target> -- everything the upload needs to know, said
   * by the server and signed, so nothing the client sends later has to be
   * believed. */
  ircd_snprintf(0, payload, sizeof(payload), "%s:%s:%s", id,
                cli_user(sptr)->account, ask->fa_target);

  if (!ircd_token_make(ticket, sizeof(ticket), FILE_TICKET_LABEL, payload,
                       CurrentTime, FILE_TICKET_WINDOW)) {
    file_tell(sptr, "This server cannot sign an upload ticket.");
    MyFree(ask);
    return;
  }

  if (!file_url(url, sizeof(url), id)) {
    file_tell(sptr, "This server has no base URL configured for files.");
    MyFree(ask);
    return;
  }

  file_tell(sptr, "Upload with:");
  file_tell(sptr, "  curl -T <file> -H 'Authorization: Bearer %s' %.*s/u/%s",
            ticket, (int) (strlen(url) - strlen("/f/") - FILE_ID_LEN), url,
            id);
  file_tell(sptr, "It will then be at %s", url);

  if (max)
    file_tell(sptr, "At most %u bytes, and the ticket is good for %d "
                    "minutes.", max, FILE_TICKET_WINDOW / 60);

  MyFree(ask);
}

/** FILE UPLOAD [<target>] */
static void file_cmd_upload(struct Client* sptr, int parc, char* parv[])
{
  struct FileAsk* ask;

  if (!http_upload_available()) {
    file_tell(sptr, "This server is not taking uploads.");
    return;
  }

  ask = (struct FileAsk*) MyCalloc(1, sizeof(*ask));

  ircd_snprintf(0, ask->fa_numnick, sizeof(ask->fa_numnick), "%s%s",
                NumNick(sptr));

  if (parc > 2 && !EmptyString(parv[2]))
    ircd_strncpy(ask->fa_target, parv[2], CHANNELLEN);

  /* The quota first: telling somebody where to send a file and refusing
   * it when it arrives wastes their upload and their time. */
  file_store_usage(cli_user(sptr)->account, file_upload_ready, ask);
}

/* ------------------------------------------------------------------- *
 * LIST and DELETE                                                     *
 * ------------------------------------------------------------------- */

/** Show a listing. */
static void file_listed(const struct FileRow* rows, unsigned int count,
                        void* user)
{
  struct FileAsk* ask = (struct FileAsk*) user;
  struct Client* sptr = file_asker(ask->fa_numnick);
  char url[HTTP_PATH_MAX + 1];
  unsigned int i;

  if (!sptr) {
    MyFree(ask);
    return;
  }

  if (!rows) {
    file_tell(sptr, "The file store is not answering; try again later.");
    MyFree(ask);
    return;
  }

  if (!count) {
    file_tell(sptr, "You are holding nothing.");
    MyFree(ask);
    return;
  }

  for (i = 0; i < count; i++) {
    if (!file_url(url, sizeof(url), rows[i].fr_id))
      ircd_strncpy(url, rows[i].fr_id, sizeof(url) - 1);

    file_tell(sptr, "%s  %lld bytes  %s  %s", rows[i].fr_id, rows[i].fr_size,
              rows[i].fr_name, url);
  }

  file_tell(sptr, "End of list.");
  MyFree(ask);
}

/** FILE LIST */
static void file_cmd_list(struct Client* sptr)
{
  struct FileAsk* ask = (struct FileAsk*) MyCalloc(1, sizeof(*ask));

  ircd_snprintf(0, ask->fa_numnick, sizeof(ask->fa_numnick), "%s%s",
                NumNick(sptr));

  file_store_list(cli_user(sptr)->account, 32, file_listed, ask);
}

/** Say whether the delete happened. */
static void file_deleted(int ok, void* user)
{
  struct FileAsk* ask = (struct FileAsk*) user;
  struct Client* sptr = file_asker(ask->fa_numnick);

  if (sptr)
    file_tell(sptr, ok ? "It is gone."
                       : "There is no such file of yours.");

  MyFree(ask);
}

/** FILE DELETE <id> */
static void file_cmd_delete(struct Client* sptr, int parc, char* parv[])
{
  struct FileAsk* ask;

  if (parc < 3 || EmptyString(parv[2])) {
    file_tell(sptr, "FILE DELETE <id>");
    return;
  }

  if (!file_id_valid(parv[2])) {
    file_tell(sptr, "That is not an identifier this server hands out.");
    return;
  }

  ask = (struct FileAsk*) MyCalloc(1, sizeof(*ask));

  ircd_snprintf(0, ask->fa_numnick, sizeof(ask->fa_numnick), "%s%s",
                NumNick(sptr));

  /* The account goes into the statement, so the delete is by owner and
   * by identifier at once: checking first and deleting afterwards is two
   * statements with a gap, and the gap is where the wrong one goes. */
  file_store_delete(parv[2], cli_user(sptr)->account, file_deleted, ask);
}

/* ------------------------------------------------------------------- *
 * The command                                                         *
 * ------------------------------------------------------------------- */

/** FILE, from a client. */
static int file_m_file(struct Client* cptr, struct Client* sptr, int parc,
                       char* parv[])
{
  (void) cptr;

  if (!MyUser(sptr))
    return 0;

  if (parc < 2 || EmptyString(parv[1])) {
    file_tell(sptr, "FILE UPLOAD [<target>] | FILE LIST | FILE DELETE <id>");
    return 0;
  }

  if (file_refused(sptr))
    return 0;

  if (!ircd_strcmp(parv[1], "UPLOAD")) {
    file_cmd_upload(sptr, parc, parv);
    return 0;
  }

  if (!ircd_strcmp(parv[1], "LIST")) {
    file_cmd_list(sptr);
    return 0;
  }

  if (!ircd_strcmp(parv[1], "DELETE")) {
    file_cmd_delete(sptr, parc, parv);
    return 0;
  }

  return send_reply(sptr, ERR_UNKNOWNCOMMAND, parv[1]);
}

/** One handler, for clients.  A server never sends this: the ticket and
 * the upload are between one client and the server it is on, and the file
 * is on that server's disk. */
static MessageHandler file_handlers[] = {
  0,             /* unregistered */
  file_m_file,   /* client */
  0,             /* server */
  file_m_file,   /* oper */
  0              /* service */
};

int file_cmd_start(void)
{
  return module_add_command(file_mod, "FILE", "FILE", MAXPARA, 0,
                            file_handlers);
}
