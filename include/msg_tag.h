#ifndef INCLUDED_msg_tag_h
#define INCLUDED_msg_tag_h
/*
 * IRC - Internet Relay Chat, include/msg_tag.h
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */
/** @file
 * @brief IRCv3 message tag support
 */
#ifndef INCLUDED_client_h
#include "client.h"
#endif
#ifndef INCLUDED_time_h
#include <time.h>
#endif
#ifndef INCLUDED_stddef_h
#include <stddef.h>
#define INCLUDED_stddef_h
#endif

struct Client;

/** A single message tag (key-value pair). */
struct MsgTag {
  struct MsgTag *next;  /**< Next tag in the list. */
  char *key;            /**< Tag key (points into parse buffer). */
  char *value;          /**< Tag value, or NULL if tag has no '=' value. */
};

/** Bitmask of outbound tag profiles for recipient bucketing. */
enum MsgTagProfile {
  TAGP_NONE    = 0,
  TAGP_TIME    = 0x01,
  TAGP_MSGID   = 0x02,
  TAGP_BATCH   = 0x04,
};

/*
 * The message identifier of the line being handled.
 *
 * One line in is one message, however many sends it turns into: a channel
 * message fans out to every member, is echoed back to its sender, and
 * crosses every link, and all of those have to carry the same identifier
 * or nothing downstream can tell they are the same message.  So the
 * identifier belongs to the line, not to a send call, and parse.c opens
 * and closes it around the handler.
 *
 * Outside that window there is no identifier at all, and that is the right
 * answer: a message the server invents on its own -- a notice from a
 * timer, a reply to a command -- is not a user's message and nothing will
 * ever want to refer back to it.
 */

/** Begin a line.  Called by parse.c for every command it dispatches.
 * @param[in] tok The command's P10 token.
 * @param[in] tags Tags parsed from the line.
 * @param[in] from_server Non-zero if the line came from another server, in
 *   which case an identifier it carries is the network's and is kept.  A
 *   client's own @c msgid is never trusted: an identifier a client could
 *   choose is one it could use to overwrite somebody else's message in
 *   whatever stores them.
 */
void msg_tag_line_begin(const char *tok, struct MsgTag *tags, int from_server);

/** End the line.  After this there is no current identifier. */
void msg_tag_line_end(void);

/** The identifier for the line being handled, or NULL.
 *
 * @param[in] tok The command about to go out.  The identifier names the
 *   message the line carried, so only the relay of that same command is
 *   that message: a numeric sent while handling it, or a notice the server
 *   generates in passing, are not, and get nothing.  NULL asks for the
 *   line's own identifier regardless, which only parse.c has a use for.
 *
 * Generated on first use rather than at msg_tag_line_begin(): most lines
 * never reach a recipient that wants one.
 */
const char *msg_tag_line_msgid(const char *tok);

/** Arm the line's identifier for \a tok, whatever the command was.
 *
 * For a message that did not arrive as one line: a long message comes in
 * as a batch of pieces and goes out as a batch, and the one identifier it
 * has belongs to the line that opens that batch, not to any of the pieces.
 */
void msg_tag_line_force_msgid(const char *tok);

/** The line's network-stable timestamp, in ISO 8601 with milliseconds.
 *
 * The @c time tag the line arrived with, when it had one -- every server
 * on the network then records the same instant for the same message --
 * and this server's clock when the message started here.
 *
 * @param[out] buf Where to write it; 32 bytes is always enough.
 * @param[in] buflen Size of \a buf.
 */
void msg_tag_line_time(char *buf, size_t buflen);

/** Return non-zero if a command should carry an identifier.
 *
 * PRIVMSG, NOTICE and TAGMSG: the messages a user sends and that something
 * might later refer back to.  Not every command -- an identifier on a MODE
 * would be a name nothing ever says out loud. */
int msg_tag_needs_msgid(const char *tok);

/** Parse IRCv3 tags from a wire section (without leading '@').
 * Mutates the buffer in place; returned list pointers alias \a start..\a end.
 * @param[in,out] start First character after '@'.
 * @param[in] end Character after the last tag byte (the separating space).
 * @return Head of tag list, or NULL if no tags.
 */
struct MsgTag *msg_tag_parse(char *start, char *end);

/** Find a tag by key in a list. */
struct MsgTag *msg_tag_find(struct MsgTag *tags, const char *key);

/** Rebuild CLIENTTAGDENY state from configured feature value. */
void msg_tag_clienttagdeny_rebuild(void);

/** Return non-zero if \a key is a server-managed tag (not relayed from clients). */
int msg_tag_key_server(const char *key);

/** Return non-zero if \a key is a client-only tag (\a key begins with '+'). */
int msg_tag_key_client_only(const char *key);

/** Return non-zero if client-only tag \a key may be relayed (CLIENTTAGDENY). */
int msg_tag_client_allowed(const char *key);

/** Return non-zero if \a tags contains relayable client-only tags. */
int msg_tag_have_client_relay(struct MsgTag *tags);

/** Keep only allowed client-only tags (strip all others). */
struct MsgTag *msg_tag_filter_client(struct MsgTag *tags);

/** Classify which server tags a recipient should receive.
 * @param[in] to Recipient.
 * @param[in] msgid Identifier this message carries, or NULL for none.
 */
unsigned int msg_tag_profile(struct Client *to, const char *msgid);

/** Return non-zero if \a key is federated on server-server links. */
int msg_tag_key_federated(const char *key);

/** Return non-zero if P10 token \a tok should carry a network-stable
 * \a time tag on S2S.  Link/state and net-admin tokens return zero. */
int msg_tag_s2s_needs_time(const char *tok);

/** Format federated tags for a server-server wire line.
 * When \a invent_time is non-zero, include \a time (from \a tags or
 * \a local_time).  Otherwise never invent or forward \a time.
 * Also forwards other federated keys present in \a tags (e.g. \a batch).
 * Never includes \a account or client-only tags.
 * @param[in] msgid Identifier this message carries, or NULL for none.
 * @return Length of prefix beginning with '@' and ending with a space, or 0.
 */
unsigned int msg_tag_format_s2s(char *buf, size_t buflen, struct MsgTag *tags,
                                time_t local_time, int invent_time,
                                const char *msgid);

/** Format tags for a client into \a buf.
 * @param[out] buf Output buffer.
 * @param[in] buflen Size of \a buf.
 * @param[in] to Recipient (for capability checks).
 * @param[in] from Message source (may be NULL).
 * @param[in] tags Upstream tag list from parse (may be NULL).
 * @param[in] local_time Time to use when no upstream \a time tag is present.
 * @param[in] msgid Identifier this message carries, or NULL for none.  Sent
 *   only to a client that negotiated message-tags, so a traditional client
 *   sees exactly the line it has always seen.
 * @return Length of formatted prefix excluding trailing space, or 0 if no tags.
 *         On success \a buf begins with '@' and ends with a separating space.
 */
unsigned int msg_tag_format(char *buf, size_t buflen, struct Client *to,
                            struct Client *from, struct MsgTag *tags,
                            time_t local_time, const char *msgid);

/** Append \a prefix then \a body into \a out (NUL-terminated, no extra CRLF).
 * @return Total length written, or 0 on overflow.
 */
unsigned int msg_tag_assemble(char *out, size_t outlen,
                              const char *prefix, unsigned int prefix_len,
                              const char *body, unsigned int body_len);

#endif /* INCLUDED_msg_tag_h */
