#ifndef INCLUDED_batch_h
#define INCLUDED_batch_h
/*
 * IRC - Internet Relay Chat, include/batch.h
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
 * @brief IRCv3 batches and labeled responses.
 *
 * A *batch* says "these messages belong together".  The server opens one
 * with @c BATCH @c +id, sends messages carrying @c \@batch=id, and closes
 * it with @c BATCH @c -id.  A client that did not ask for the @c batch
 * capability is sent neither the open and close lines nor the tag: it
 * receives the messages loose, exactly as it always did, which is what
 * makes the whole thing safe to add.
 *
 * A *labeled response* is the first thing that needs batches, and the
 * reason they are here now.  A client tags a command with @c \@label=xyz
 * and gets everything the command produced back under the same label, so
 * it can tell which reply belongs to which request -- something a terminal
 * client never needed and a graphical one cannot work without.
 *
 * The shape of the answer is fixed by the specification:
 *
 *   - the command produced nothing: @c \@label=xyz @c ACK
 *   - the command produced messages: they arrive inside a batch of type
 *     @c labeled-response, and the label rides on the @c BATCH line.
 *
 * Which is why nothing here buffers anything.  The batch is opened lazily,
 * on the first message the command actually sends, so the server never has
 * to know in advance how many there will be: if none ever arrives, the
 * label ends as an @c ACK instead.
 *
 * One label is in flight at a time.  A command is dispatched, handled and
 * finished before the next line is read -- the core is one thread and the
 * handler does not yield -- so a second label cannot begin while the first
 * is open, and the state below is a single context rather than a table.
 */

struct Client;

/** Longest label this server accepts from a client, not counting the
 * terminator.  The value is opaque and echoed back unchanged. */
#define LABELLEN 64

/** Size of a batch identifier, not counting the terminator. */
#define BATCHIDLEN 16

/** Begin a labeled response.
 *
 * Does nothing unless \a cptr negotiated both @c labeled-response and
 * @c batch: the specification builds one on the other, and a client that
 * asked for the label without the batch has no way to be answered.
 *
 * @param[in] cptr Client that sent the labeled command.
 * @param[in] label The value of its @c label tag.
 */
extern void label_begin(struct Client* cptr, const char* label);

/** Finish the labeled response opened by label_begin().
 *
 * Closes the batch if one was opened, and sends the bare @c ACK if the
 * command turned out to produce nothing at all.  Safe to call when no
 * label is in flight.
 */
extern void label_end(void);

/** Open the pending batch if this message is the first of a labeled
 * response.
 *
 * Called from send.c for every message about to reach a client, before the
 * tags are rendered.  Almost always does nothing: it costs one comparison
 * unless a label is in flight for this exact client.
 *
 * @param[in] to Client the message is going to.
 */
extern void label_before_send(struct Client* to);

/** The batch \a to is currently inside, or NULL.
 *
 * What msg_tag_format() renders as @c \@batch=.  NULL while the @c BATCH
 * line itself is being sent: the line that opens a batch is not in it.
 */
extern const char* batch_current(const struct Client* to);

/** The label this particular message must carry, or NULL.
 *
 * Only the @c BATCH line that opens a labeled response and the bare
 * @c ACK carry it; the messages inside the batch are identified by the
 * batch, not by the label repeated on each one.
 */
extern const char* batch_label_tag(const struct Client* to);

/** Reset every batch and label.  Called when a client goes away, so a
 * connection that dies mid-response leaves nothing behind. */
extern void batch_client_exiting(const struct Client* cptr);

#endif /* INCLUDED_batch_h */
