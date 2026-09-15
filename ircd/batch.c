/*
 * IRC - Internet Relay Chat, ircd/batch.c
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
 * @brief IRCv3 batches and labeled responses.  See batch.h.
 */
#include "config.h"

#include "batch.h"
#include "capab.h"
#include "client.h"
#include "ircd.h"
#include "ircd_log.h"
#include "ircd_string.h"
#include "msg.h"
#include "send.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <stdio.h>
#include <string.h>

/** The one labeled response that can be in flight.
 *
 * One at a time, and the reason is the shape of the server rather than a
 * simplification: a command is read, dispatched, handled and finished
 * before the next line is looked at, so a second label cannot begin while
 * this one is open.
 */
static struct {
  struct Client* client;               /**< Who sent the labeled command. */
  char label[LABELLEN + 1];            /**< Its label. */
  char batch[BATCHIDLEN + 1];          /**< The batch, once opened. */
  int  open;                           /**< The batch has been opened. */
  int  emitting;                       /**< Sending one of the lines that
                                            frame the response: those are
                                            not inside the batch. */
  int  emit_label;                     /**< ...and this one carries the
                                            label.  Only the line that
                                            opens the batch, and the bare
                                            ACK, do: the closing line is
                                            identified by the batch it
                                            names, and repeating the label
                                            there is not what the
                                            specification asks for. */
} label_ctx;

/** Source of batch identifiers.  They only have to be distinct among the
 * batches one client has open at once, which here is never more than one;
 * counting up is more than enough and keeps them short. */
static unsigned long batch_seq;

/** Render the next batch identifier into \a buf.
 *
 * Plain snprintf(): the identifier is a decimal number with none of the
 * IRC formatting ircd_snprintf() exists for, and not reaching for it keeps
 * this file free of the client rendering that would come with it.
 */
static void batch_next_id(char* buf, size_t len)
{
  snprintf(buf, len, "%lu", ++batch_seq);
}

/** Begin a labeled response.
 * @param[in] cptr Client that sent the labeled command.
 * @param[in] label The value of its label tag.
 */
void label_begin(struct Client* cptr, const char* label)
{
  memset(&label_ctx, 0, sizeof(label_ctx));

  if (EmptyString(label))
    return;
  if (!cptr || !MyConnect(cptr) || IsServer(cptr))
    return;

  /* labeled-response is defined in terms of batches, and the answer to a
   * command that produces more than one message is a batch.  A client that
   * asked for the label but not the batch cannot be answered the way the
   * specification says, so it is answered the way it would have been
   * before it asked for anything.
   */
  if (!CapHas(cli_active(cptr), CAP_LABELEDRESPONSE)
      || !CapHas(cli_active(cptr), CAP_BATCH))
    return;

  label_ctx.client = cptr;
  ircd_strncpy(label_ctx.label, label, sizeof(label_ctx.label) - 1);
  label_ctx.label[sizeof(label_ctx.label) - 1] = '\0';
}

/** Open the pending batch if this message is the first of a labeled
 * response.
 * @param[in] to Client the message is going to.
 */
void label_before_send(struct Client* to)
{
  /* The common case, and the whole cost on a server where nobody uses
   * labels: one comparison against a null pointer.
   */
  if (!label_ctx.client || label_ctx.open || label_ctx.emitting)
    return;
  if (to != label_ctx.client)
    return;

  batch_next_id(label_ctx.batch, sizeof(label_ctx.batch));
  label_ctx.open = 1;

  /* The line that opens a batch is not inside it: while it is going out,
   * batch_current() says no batch and batch_label_tag() says the label.
   */
  label_ctx.emitting = 1;
  label_ctx.emit_label = 1;
  sendcmdto_one(&me, CMD_IRCBATCH, to, "+%s labeled-response", label_ctx.batch);
  label_ctx.emit_label = 0;
  label_ctx.emitting = 0;
}

/** Finish the labeled response opened by label_begin(). */
void label_end(void)
{
  struct Client* to = label_ctx.client;

  if (!to)
    return;

  label_ctx.emitting = 1;

  if (label_ctx.open) {
    /* No label on the closing line: it names the batch, and the batch is
     * already tied to the label by the line that opened it.
     */
    sendcmdto_one(&me, CMD_IRCBATCH, to, "-%s", label_ctx.batch);
  } else {
    label_ctx.emit_label = 1;
    /* The command produced nothing at all.  Saying so is the point: a
     * client that heard neither a reply nor an error would be left waiting
     * for one, which is exactly what the label exists to prevent.
     *
     * No source prefix: the specification's ACK is a bare command.
     */
    sendrawto_one(to, "ACK");
  }

  memset(&label_ctx, 0, sizeof(label_ctx));
}

/** The batch \a to is currently inside, or NULL.
 * @param[in] to Recipient.
 */
const char* batch_current(const struct Client* to)
{
  if (!label_ctx.open || label_ctx.emitting)
    return 0;
  if (to != label_ctx.client)
    return 0;

  return label_ctx.batch;
}

/** The label this particular message must carry, or NULL.
 * @param[in] to Recipient.
 */
const char* batch_label_tag(const struct Client* to)
{
  if (!label_ctx.emit_label || to != label_ctx.client)
    return 0;

  return label_ctx.label;
}

/** Forget any batch or label belonging to a client that is going away.
 * @param[in] cptr Client leaving.
 */
void batch_client_exiting(const struct Client* cptr)
{
  if (label_ctx.client == cptr)
    memset(&label_ctx, 0, sizeof(label_ctx));
}
