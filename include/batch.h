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

/** Open a batch of \a type to one client, and return its identifier.
 *
 * The third kind of batch: not a labeled response and not a fan-out, but a
 * server telling one client that the next several messages are one answer.
 * What #CHATHISTORY needs, and what anything else that replies with a
 * series will need.
 *
 * Refused for a client that did not negotiate @c batch, because the
 * messages inside would arrive loose and indistinguishable from live
 * traffic -- which for history is worse than no answer at all.  The caller
 * checks the capability itself and says so properly; NULL here is the
 * belt to that brace.
 *
 * One is open at a time.  A command is dispatched, handled and finished
 * before the next line is read, and a database answer arrives in one
 * callback that sends everything it has, so a second cannot begin while
 * the first is open.
 *
 * The line that opens a batch is not inside it: while it is going out,
 * batch_current() answers with whatever batch was already in force, so a
 * batch opened during a labeled response nests inside it the way the
 * specification says.
 *
 * @param[in] to Client to answer.  Must be one of this server's.
 * @param[in] type Batch type, e.g. "chathistory".
 * @param[in] param One parameter for the @c BATCH line, or NULL.
 * @return The identifier, valid until batch_out_close(), or NULL.
 */
extern const char* batch_out_open(struct Client* to, const char* type,
                                  const char* param);

/** Close the batch batch_out_open() opened.
 *
 * Safe to call when none is open, and when the one open belongs to another
 * client.
 * @param[in] to Client it was opened for.
 */
extern void batch_out_close(struct Client* to);

/** Reset every batch and label.  Called when a client goes away, so a
 * connection that dies mid-response leaves nothing behind. */
extern void batch_client_exiting(const struct Client* cptr);

/* ------------------------------------------------------------------
 * Multiline: a batch a client sends, and a batch the server fans out.
 *
 * Implemented in ircd/multiline.c rather than ircd/batch.c: this half
 * needs channels, the hash tables and the relay, and keeping them out of
 * the other half is what lets the labeled-response logic be tested
 * without a server around it.
 * ------------------------------------------------------------------ */

/** Longest batch identifier a client may choose. */
#define BATCH_CLIENT_IDLEN 64

/** A batch a client has open, holding the parts of one long message.
 *
 * A message longer than a line arrives as a batch of PRIVMSGs, each a
 * piece; the server holds them until the batch closes and then relays the
 * whole thing.  Held in a list allocated on demand rather than in
 * #Connection, because almost no connection ever has one open and a buffer
 * per connection would cost megabytes to serve a handful of clients.
 */
struct InBatch;

/** Open an inbound batch for \a cptr.
 * @param[in] cptr Client opening it.
 * @param[in] id The identifier it chose.
 * @param[in] type Batch type; only "draft/multiline" is accepted.
 * @param[in] target Channel or nick the message is for.
 * @return Zero on success, or the numeric to refuse with.
 */
extern int batch_in_open(struct Client* cptr, const char* id,
                         const char* type, const char* target);

/** Close an inbound batch and hand back what it collected.
 * @param[in] cptr Client closing it.
 * @param[in] id The identifier, without the leading '-'.
 * @return Zero on success, or the numeric to refuse with.
 */
extern int batch_in_close(struct Client* cptr, const char* id);

/** The batch \a cptr has open, or NULL. */
extern struct InBatch* batch_in_find(const struct Client* cptr);

/** Add one part to an open batch.
 *
 * Called from the message handlers when a line carries a @c batch tag.
 * @param[in] cptr Client sending it.
 * @param[in] id The batch tag's value.
 * @param[in] notice Non-zero if the line was a NOTICE.
 * @param[in] target What the line was addressed to.
 * @param[in] text The line's text.
 * @param[in] concat Non-zero if the line carried
 *   @c draft/multiline-concat: it continues the previous part instead of
 *   starting a line of its own.
 * @return Zero if it was taken, or the numeric to refuse with.
 */
extern int batch_in_add(struct Client* cptr, const char* id, int notice,
                        const char* target, const char* text, int concat);

/** Relay everything an inbound batch collected, then free it.
 *
 * Clients that asked for @c draft/multiline get the parts inside a batch
 * of their own; everybody else gets them as ordinary separate messages,
 * which is the line a traditional client has always seen.
 * @param[in] cptr Client that sent it.
 * @param[in] batch The batch, from batch_in_close().
 */
extern void batch_in_deliver(struct Client* cptr, struct InBatch* batch);

/** Take a PRIVMSG or NOTICE that belongs to an open batch.
 *
 * Called from the message handlers before they relay anything.  Almost
 * always does nothing: it costs one tag lookup, and only on a line that
 * actually carries a @c batch tag.
 *
 * @param[in] cptr Client that sent the line.
 * @param[in] notice Non-zero if it was a NOTICE.
 * @param[in] target What it was addressed to.
 * @param[in] text Its text.
 * @return Non-zero if the line belonged to a batch and was taken; the
 *   caller must then relay nothing.  The client has already been told if
 *   the line was refused.
 */
extern int batch_in_capture(struct Client* cptr, int notice,
                            const char* target, const char* text);

/** The fan-out batch \a to is inside, or NULL.
 *
 * Asked by batch_current(), so that the two kinds of batch answer through
 * one place without this half having to know about the other.
 */
extern const char* multiline_batch_for(const struct Client* to);

/** Return non-zero if the piece being relayed continues the one before it.
 *
 * A long message is one *line* however many pieces it was sent in, and a
 * line does not fit on the wire: what goes out is as many PRIVMSGs as it
 * takes, and every one after the first carries @c draft/multiline-concat
 * so that a client which asked for draft/multiline puts the line back
 * together exactly as it was typed.  Without it the pieces are a message
 * with line breaks the sender never wrote -- and without the split the
 * tail of the line is simply dropped by the send layer.
 *
 * Only a client that negotiated the capability is told: to everybody else
 * the pieces are the series of separate messages they have always been.
 *
 * Asked by msg_tag_format(), the way multiline_batch_for() is asked by
 * batch_current().
 */
extern int multiline_concat_for(const struct Client* to);

/** Return non-zero if \a cptr has a batch open.
 *
 * Asked by the parser before it charges a line against the client's flood
 * allowance.
 */
extern int multiline_in_progress(const struct Client* cptr);

/** Forget an unfinished message when its sender goes away. */
extern void multiline_client_exiting(const struct Client* cptr);

/** Advertise the multiline limits in the capability's value. */
extern void batch_multiline_advertise(void);

#endif /* INCLUDED_batch_h */
