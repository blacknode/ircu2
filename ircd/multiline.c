/*
 * IRC - Internet Relay Chat, ircd/multiline.c
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
 * @brief IRCv3 draft/multiline: a message longer than a line.
 *
 * A client that has more to say than fits in 512 bytes opens a batch,
 * sends the pieces as ordinary PRIVMSGs carrying the batch tag, and closes
 * it.  This file holds the pieces until the batch closes and then relays
 * the whole thing.
 *
 * On the way out the pieces go through the ordinary relay, one at a time.
 * That is the whole compatibility story: a client that asked for none of
 * this sees a series of separate messages, which is exactly what it has
 * always seen, and one that did ask sees the same series wrapped in a
 * batch of its own.
 *
 * It lives apart from batch.c for the same reason migration_run.c lives
 * apart from migration.c: this half needs channels, the hash tables and
 * the relay, and keeping them out of the other half is what lets the
 * labeled-response logic be tested without a server around it.
 */
#include "config.h"

#include "batch.h"
#include "capab.h"
#include "channel.h"
#include "client.h"
#include "hash.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_relay.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "msg.h"
#include "msg_tag.h"
#include "numeric.h"
#include "parse.h"
#include "s_user.h"
#include "send.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <stdio.h>
#include <string.h>

/* From batch.c: one source of identifiers for both kinds of batch. */
extern unsigned long batch_seq;
extern void batch_next_id(char* buf, size_t len);

/** One piece of a long message. */
struct BatchPart {
  struct BatchPart* bp_next;
  char*             bp_text;
};

/** A batch a client has open.  See batch.h. */
struct InBatch {
  struct InBatch*   ib_next;
  struct Client*    ib_client;
  char              ib_id[BATCH_CLIENT_IDLEN + 1];
  char              ib_target[CHANNELLEN + 1];
  int               ib_notice;      /**< The parts are NOTICEs. */
  unsigned int      ib_lines;       /**< Parts collected so far. */
  size_t            ib_bytes;       /**< Their total length. */
  struct BatchPart* ib_parts;
  struct BatchPart** ib_tail;
};

/** Every inbound batch currently open, on any connection.
 *
 * A list and not a field in #Connection: almost no connection ever has one
 * open, and a buffer per connection would cost megabytes to serve a
 * handful of clients.
 */
static struct InBatch* InBatchList;

/** The batch the server is fanning out, if any.
 *
 * Distinct from the labeled response above: that one goes to exactly one
 * client, this one goes to every recipient of a message that asked for
 * draft/multiline.  Never both at once -- a labeled response is opened
 * inside a command handler and this one only while relaying -- but they
 * are separate state because they are answered to by different clients.
 */
static struct {
  char id[BATCHIDLEN + 1];
  int  open;
  int  emitting;
} fanout_ctx;

/** The batch \a to is inside as part of a fan-out, or NULL.
 * @param[in] to Recipient.
 */
const char* multiline_batch_for(const struct Client* to)
{
  if (!fanout_ctx.open || fanout_ctx.emitting)
    return 0;
  if (!to || IsServer(to) || !CapHas(cli_active(to), CAP_MULTILINE))
    return 0;

  return fanout_ctx.id;
}

/** Advertise the multiline limits in the capability's value.
 *
 * A client has to know what it may send before it sends it; the limits are
 * features, so this is called again after every rehash.
 */
void batch_multiline_advertise(void)
{
  char value[64];

  snprintf(value, sizeof(value), "max-bytes=%d,max-lines=%d",
           feature_int(FEAT_MULTILINE_MAX_BYTES),
           feature_int(FEAT_MULTILINE_MAX_LINES));
  cap_set_value(CAP_MULTILINE, value);
}

/** The batch \a cptr has open, or NULL. */
struct InBatch* batch_in_find(const struct Client* cptr)
{
  struct InBatch* b;

  for (b = InBatchList; b; b = b->ib_next)
    if (b->ib_client == cptr)
      return b;

  return 0;
}

/** Release a batch and everything it collected. */
static void batch_in_free(struct InBatch* batch)
{
  struct BatchPart* p;
  struct BatchPart* next;

  for (p = batch->ib_parts; p; p = next) {
    next = p->bp_next;
    MyFree(p->bp_text);
    MyFree(p);
  }

  MyFree(batch);
}

/** Take a batch out of the list. */
static void batch_in_unlink(struct InBatch* batch)
{
  struct InBatch** b_p;

  for (b_p = &InBatchList; *b_p; b_p = &(*b_p)->ib_next)
    if (*b_p == batch) {
      *b_p = batch->ib_next;
      return;
    }
}

/** Open an inbound batch for \a cptr.
 * @param[in] cptr Client opening it.
 * @param[in] id The identifier it chose.
 * @param[in] type Batch type.
 * @param[in] target Channel or nick the message is for.
 * @return Zero on success, or the numeric to refuse with.
 */
int batch_in_open(struct Client* cptr, const char* id, const char* type,
                  const char* target)
{
  struct InBatch* batch;

  if (EmptyString(id) || EmptyString(type) || EmptyString(target))
    return ERR_NEEDMOREPARAMS;

  if (!CapHas(cli_active(cptr), CAP_MULTILINE))
    return ERR_UNKNOWNCOMMAND;

  /* The only batch type a client may open.  Refusing the rest by name
   * rather than ignoring them means a client that tries something this
   * server has not learned yet hears about it.
   */
  if (ircd_strcmp(type, "draft/multiline"))
    return ERR_UNKNOWNCAPCMD;

  if (strlen(id) > BATCH_CLIENT_IDLEN)
    return ERR_INPUTTOOLONG;

  /* One at a time.  Nesting is allowed by the specification and worth
   * nothing here: a message is not inside another message.
   */
  if (batch_in_find(cptr))
    return ERR_UNKNOWNCAPCMD;

  batch = (struct InBatch*) MyCalloc(1, sizeof(struct InBatch));
  batch->ib_client = cptr;
  ircd_strncpy(batch->ib_id, id, sizeof(batch->ib_id) - 1);
  ircd_strncpy(batch->ib_target, target, sizeof(batch->ib_target) - 1);
  batch->ib_tail = &batch->ib_parts;

  batch->ib_next = InBatchList;
  InBatchList = batch;

  return 0;
}

/** Add one part to an open batch.  See batch.h. */
int batch_in_add(struct Client* cptr, const char* id, int notice,
                 const char* target, const char* text, int concat)
{
  struct InBatch* batch = batch_in_find(cptr);
  struct BatchPart* part;
  size_t len;

  if (!batch || ircd_strcmp(batch->ib_id, id))
    return ERR_UNKNOWNCAPCMD;

  /* Every part of one message goes to one place.  A batch that changed
   * target halfway would be two messages wearing one name.
   */
  if (EmptyString(target) || ircd_strcmp(batch->ib_target, target))
    return ERR_UNKNOWNCAPCMD;

  /* And they are all the same kind: a NOTICE and a PRIVMSG are not halves
   * of the same thing.
   */
  if (batch->ib_lines > 0 && batch->ib_notice != (notice ? 1 : 0))
    return ERR_UNKNOWNCAPCMD;
  batch->ib_notice = notice ? 1 : 0;

  len = text ? strlen(text) : 0;

  if (batch->ib_bytes + len > (size_t) feature_int(FEAT_MULTILINE_MAX_BYTES))
    return ERR_INPUTTOOLONG;
  if (!concat
      && batch->ib_lines >= (unsigned int) feature_int(FEAT_MULTILINE_MAX_LINES))
    return ERR_INPUTTOOLONG;

  batch->ib_bytes += len;

  /* A part marked concat continues the one before it rather than starting
   * a line of its own: that is how a client sends a line longer than a
   * line.
   */
  if (concat && batch->ib_parts) {
    struct BatchPart* last = batch->ib_parts;
    char* joined;
    size_t oldlen;

    while (last->bp_next)
      last = last->bp_next;

    oldlen = strlen(last->bp_text);
    joined = (char*) MyMalloc(oldlen + len + 1);
    memcpy(joined, last->bp_text, oldlen);
    memcpy(joined + oldlen, text ? text : "", len);
    joined[oldlen + len] = '\0';

    MyFree(last->bp_text);
    last->bp_text = joined;

    return 0;
  }

  part = (struct BatchPart*) MyCalloc(1, sizeof(struct BatchPart));
  DupString(part->bp_text, text ? text : "");

  *batch->ib_tail = part;
  batch->ib_tail = &part->bp_next;
  batch->ib_lines++;

  return 0;
}

/** Close an inbound batch.  See batch.h. */
int batch_in_close(struct Client* cptr, const char* id)
{
  struct InBatch* batch = batch_in_find(cptr);

  if (!batch || EmptyString(id) || ircd_strcmp(batch->ib_id, id))
    return ERR_UNKNOWNCAPCMD;

  return 0;
}

/** Open the fan-out batch that carries a long message.
 * @param[in] sptr Who sent it.
 * @param[in] batch The message.
 */
static void batch_fanout_open(struct Client* sptr, struct InBatch* batch)
{
  struct Channel* chptr = 0;

  batch_next_id(fanout_ctx.id, sizeof(fanout_ctx.id));
  fanout_ctx.open = 1;
  fanout_ctx.emitting = 1;

  /* The one identifier the whole message has, on the line that opens it.
   * The parts get none: they are pieces of this, not messages of their
   * own, and giving each a name would have anything that stores messages
   * record one message as several.
   */
  msg_tag_line_force_msgid(MSG_IRCBATCH);

  if (IsChannelPrefix(*batch->ib_target)
      && (chptr = FindChannel(batch->ib_target))) {
    sendcmdto_capflag_channel_butserv_butone(sptr, CMD_IRCBATCH, chptr,
                                             0, 0, CAP_MULTILINE, CAP_NONE,
                                             "+%s draft/multiline %H",
                                             fanout_ctx.id, chptr);
  } else {
    struct Client* acptr = FindUser(batch->ib_target);

    if (acptr && MyConnect(acptr) && CapHas(cli_active(acptr), CAP_MULTILINE))
      sendcmdto_one(sptr, CMD_IRCBATCH, acptr, "+%s draft/multiline %s",
                    fanout_ctx.id, batch->ib_target);
  }

  fanout_ctx.emitting = 0;
}

/** Close the fan-out batch. */
static void batch_fanout_close(struct Client* sptr, struct InBatch* batch)
{
  struct Channel* chptr;

  if (!fanout_ctx.open)
    return;

  fanout_ctx.emitting = 1;

  /* The identifier belongs to the message, and the message was named on
   * the line that opened the batch.  The closing line is a delimiter, not
   * a second copy of it.
   */
  msg_tag_line_force_msgid(0);

  if (IsChannelPrefix(*batch->ib_target)
      && (chptr = FindChannel(batch->ib_target))) {
    sendcmdto_capflag_channel_butserv_butone(sptr, CMD_IRCBATCH, chptr,
                                             0, 0, CAP_MULTILINE, CAP_NONE,
                                             "-%s", fanout_ctx.id);
  } else {
    struct Client* acptr = FindUser(batch->ib_target);

    if (acptr && MyConnect(acptr) && CapHas(cli_active(acptr), CAP_MULTILINE))
      sendcmdto_one(sptr, CMD_IRCBATCH, acptr, "-%s", fanout_ctx.id);
  }

  fanout_ctx.emitting = 0;
  fanout_ctx.open = 0;
  fanout_ctx.id[0] = '\0';
}

/** Relay everything an inbound batch collected, then free it.
 * @param[in] cptr Client that sent it.
 * @param[in] batch The batch.
 */
void batch_in_deliver(struct Client* cptr, struct InBatch* batch)
{
  struct BatchPart* part;
  int is_channel;

  assert(0 != batch);

  batch_in_unlink(batch);

  if (!batch->ib_parts) {
    /* A batch that carried nothing.  Nothing to say about it. */
    batch_in_free(batch);
    return;
  }

  is_channel = IsChannelPrefix(*batch->ib_target);

  batch_fanout_open(cptr, batch);

  /* Each part goes out through the ordinary relay, which is what makes a
   * client that asked for none of this see exactly what it always saw: a
   * series of separate messages.  The ones that did ask see the same
   * series, wrapped in the batch opened above.
   */
  for (part = batch->ib_parts; part; part = part->bp_next) {
    if (IsDead(cptr))
      break;

    if (is_channel) {
      if (batch->ib_notice)
        relay_channel_notice(cptr, batch->ib_target, part->bp_text);
      else
        relay_channel_message(cptr, batch->ib_target, part->bp_text);
    } else {
      if (batch->ib_notice)
        relay_private_notice(cptr, batch->ib_target, part->bp_text);
      else
        relay_private_message(cptr, batch->ib_target, part->bp_text);
    }
  }

  batch_fanout_close(cptr, batch);
  batch_in_free(batch);
}

/** Take a PRIVMSG or NOTICE that belongs to an open batch.  See batch.h. */
int batch_in_capture(struct Client* cptr, int notice, const char* target,
                     const char* text)
{
  struct MsgTag* tags;
  struct MsgTag* tag;
  int concat;
  int err;

  /* The whole cost on a line that is not part of a batch: one walk of a
   * tag list that is almost always empty.
   */
  if (!InBatchList)
    return 0;

  tags = parse_tags();
  tag = msg_tag_find(tags, "batch");
  if (!tag || !tag->value)
    return 0;

  concat = msg_tag_find(tags, "draft/multiline-concat") != 0;

  err = batch_in_add(cptr, tag->value, notice, target, text, concat);
  if (err) {
    /* A NOTICE is never answered with an error -- that is what makes it a
     * notice -- but the line is still swallowed: it named a batch, and
     * relaying it on its own would turn a refused piece into a message
     * nobody asked to send.
     */
    if (!notice)
      send_reply(cptr, err, MSG_IRCBATCH);
  }

  return 1;
}

/** Forget an unfinished message when its sender goes away.
 *
 * An unfinished message is not a message: whatever it had collected goes
 * with the connection that was collecting it.
 * @param[in] cptr Client leaving.
 */
void multiline_client_exiting(const struct Client* cptr)
{
  struct InBatch* batch = batch_in_find(cptr);

  if (batch) {
    batch_in_unlink(batch);
    batch_in_free(batch);
  }
}

/** Return non-zero if \a cptr has a batch open.
 *
 * Asked by the parser before it charges a line against the client's flood
 * allowance: see parse.c.
 * @param[in] cptr Client to test.
 */
int multiline_in_progress(const struct Client* cptr)
{
  return batch_in_find(cptr) != 0;
}
