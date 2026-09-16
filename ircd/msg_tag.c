/*
 * IRC - Internet Relay Chat, ircd/msg_tag.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */
/** @file
 * @brief IRCv3 message tag parsing and formatting
 */
#include "config.h"

#include "msg_tag.h"
#include "batch.h"
#include "capab.h"
#include "client.h"
#include "ircd.h"
#include "ircd_defs.h"
#include "ircd_features.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "msg.h"
#include "msgid.h"
#include "parse.h"

#include <string.h>
#include <time.h>

/** Maximum tags per incoming line (generous; typical lines have few). */
#define MAX_PARSE_TAGS 64

/** Maximum CLIENTTAGDENY list entries. */
#define CLIENTTAGDENY_MAX 64

static struct MsgTag tag_storage[MAX_PARSE_TAGS];
static int tag_count;

static int clienttag_deny_all;
static char clienttag_names[CLIENTTAGDENY_MAX][256];
static int clienttag_name_count;

/** Unescape one IRCv3 tag value in place; returns new length.
 *  Mapping: \\s→SPACE, \\:→;, \\\\→\\, \\r→CR, \\n→LF.
 *  Invalid escapes drop the backslash (\\b→b); a trailing lone \\ is dropped.
 */
static unsigned int
msg_tag_unescape(char *value)
{
  char *r = value;
  char *w = value;

  while (*r) {
    if (*r == '\\') {
      if (!r[1]) {
        /* Trailing lone backslash: drop it. */
        break;
      }
      switch (r[1]) {
      case 's':  *w++ = ' ';  r += 2; break;
      case ':':  *w++ = ';';  r += 2; break;
      case '\\': *w++ = '\\'; r += 2; break;
      case 'r':  *w++ = '\r'; r += 2; break;
      case 'n':  *w++ = '\n'; r += 2; break;
      default:
        /* Invalid escape: drop the backslash, keep the next octet. */
        r++;
        *w++ = *r++;
        break;
      }
    } else {
      *w++ = *r++;
    }
  }
  *w = '\0';
  return (unsigned int)(w - value);
}

/** Escape a tag value into \a out; returns bytes written (excluding NUL).
 *  IRCv3: ;→\\:, SPACE→\\s, \\→\\\\, CR→\\r, LF→\\n.
 */
static unsigned int
msg_tag_escape(const char *value, char *out, size_t outlen)
{
  size_t used = 0;

  if (!value || !outlen)
    return 0;

  while (*value && used + 2 < outlen) {
    switch (*value) {
    case ' ':
      out[used++] = '\\';
      out[used++] = 's';
      break;
    case ';':
      out[used++] = '\\';
      out[used++] = ':';
      break;
    case '\\':
      out[used++] = '\\';
      out[used++] = '\\';
      break;
    case '\r':
      out[used++] = '\\';
      out[used++] = 'r';
      break;
    case '\n':
      out[used++] = '\\';
      out[used++] = 'n';
      break;
    default:
      out[used++] = *value;
      break;
    }
    value++;
  }
  out[used] = '\0';
  return (unsigned int)used;
}

/** Format \a t as ISO-8601 UTC with milliseconds into \a buf. */
static void
msg_tag_format_time(char *buf, size_t buflen, time_t t)
{
  struct tm tm;

  gmtime_r(&t, &tm);
  ircd_snprintf(0, buf, buflen, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, 0);
}

struct MsgTag *
msg_tag_parse(char *start, char *end)
{
  struct MsgTag *head = NULL;
  char *p = start;

  tag_count = 0;

  if (!start || start >= end)
    return NULL;

  *end = '\0';

  while (p < end && tag_count < MAX_PARSE_TAGS) {
    struct MsgTag *tag;
    char *semi = p;
    char *key;
    char *val;

    while (semi < end && *semi != ';')
      semi++;

    if (semi < end)
      *semi = '\0';

    key = p;
    val = strchr(key, '=');
    if (val) {
      *val++ = '\0';
      msg_tag_unescape(val);
    }

    if (*key) {
      tag = &tag_storage[tag_count++];
      tag->key = key;
      tag->value = val;
      tag->next = head;
      head = tag;
    }

    if (semi >= end)
      break;
    p = semi + 1;
  }

  return head;
}

struct MsgTag *
msg_tag_find(struct MsgTag *tags, const char *key)
{
  for (; tags; tags = tags->next) {
    if (!ircd_strcmp(tags->key, key))
      return tags;
  }
  return NULL;
}

/** Strip optional leading '+' from a client-only tag key for deny lookup. */
static const char *
clienttag_lookup_name(const char *key)
{
  if (key && key[0] == '+')
    return key + 1;
  return key;
}

void
msg_tag_clienttagdeny_rebuild(void)
{
  const char *cfg = feature_str(FEAT_CLIENTTAGDENY);
  char buf[512];
  char *p;
  char *entry;

  clienttag_deny_all = 0;
  clienttag_name_count = 0;

  if (!cfg || !*cfg)
    return;

  ircd_strncpy(buf, cfg, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  for (p = buf; (entry = strtok(p, ",")) != NULL; p = NULL) {
    while (*entry == ' ')
      entry++;

    if (!*entry)
      continue;

    if (clienttag_name_count >= CLIENTTAGDENY_MAX)
      break;

    if (!ircd_strcmp(entry, "*")) {
      clienttag_deny_all = 1;
      continue;
    }

    if (*entry == '-') {
      if (!clienttag_deny_all)
        continue;
      ircd_strncpy(clienttag_names[clienttag_name_count++],
                   clienttag_lookup_name(entry + 1),
                   sizeof(clienttag_names[0]) - 1);
      continue;
    }

    if (clienttag_deny_all)
      continue;

    ircd_strncpy(clienttag_names[clienttag_name_count++],
                 clienttag_lookup_name(entry),
                 sizeof(clienttag_names[0]) - 1);
  }
}

/* ------------------------------------------------------------------
 * The message identifier of the line being handled.  See msg_tag.h.
 * ------------------------------------------------------------------ */

/** Non-zero between msg_tag_line_begin() and msg_tag_line_end(). */
static int msgid_line_open;
/** Non-zero if this line's command carries an identifier at all. */
static int msgid_line_wanted;
/** The line's command token, so that only its own relay gets the id. */
static char msgid_line_tok[16];
/** The identifier, empty until something asks for one. */
static char msgid_line_value[MSGIDLEN + 1];

int
msg_tag_needs_msgid(const char *tok)
{
  if (!tok)
    return 0;

  /* What a user says, and nothing else.  These are the messages another
   * message can reply to, react to, edit, delete or store, which is the
   * whole reason an identifier exists.
   *
   * WALLCHOPS and WALLVOICES are deliberately not here even though they
   * are things a user says.  They go out to the channel as WALLCHOPS but
   * are echoed back to their sender as a NOTICE, so the sender would be
   * given a different name for the message than everyone else got -- and a
   * message two clients disagree about the name of is worse than one with
   * no name at all.  They can be added when that path is made to agree
   * with itself.
   */
  return !ircd_strcmp(tok, TOK_PRIVATE)
    || !ircd_strcmp(tok, TOK_NOTICE)
    || !ircd_strcmp(tok, TOK_TAGMSG);
}

void
msg_tag_line_begin(const char *tok, struct MsgTag *tags, int from_server)
{
  const struct MsgTag *tag;

  msgid_line_open = 1;
  msgid_line_wanted = msg_tag_needs_msgid(tok);
  msgid_line_value[0] = '\0';
  msgid_line_tok[0] = '\0';
  if (tok) {
    ircd_strncpy(msgid_line_tok, tok, sizeof(msgid_line_tok) - 1);
    msgid_line_tok[sizeof(msgid_line_tok) - 1] = '\0';
  }

  if (!msgid_line_wanted)
    return;

  /* An identifier that came with the message is the network's: the server
   * it started on named it, and every server has to call it the same
   * thing.  From a client it is not: an identifier a client could choose
   * is one it could use to point at -- or overwrite -- somebody else's
   * message wherever they are kept.
   */
  if (!from_server)
    return;

  tag = msg_tag_find(tags, "msgid");
  if (tag && tag->value && msgid_valid(tag->value)) {
    ircd_strncpy(msgid_line_value, tag->value, sizeof(msgid_line_value) - 1);
    msgid_line_value[sizeof(msgid_line_value) - 1] = '\0';
  }
}

void
msg_tag_line_force_msgid(const char *tok)
{
  if (!msgid_line_open)
    return;

  /* A NULL token disarms it: the line has already named what it had to
   * name and nothing after this carries an identifier.
   */
  if (!tok) {
    msgid_line_wanted = 0;
    msgid_line_value[0] = '\0';
    msgid_line_tok[0] = '\0';
    return;
  }

  msgid_line_wanted = 1;
  msgid_line_value[0] = '\0';
  ircd_strncpy(msgid_line_tok, tok, sizeof(msgid_line_tok) - 1);
  msgid_line_tok[sizeof(msgid_line_tok) - 1] = '\0';
}

void
msg_tag_line_end(void)
{
  msgid_line_open = 0;
  msgid_line_wanted = 0;
  msgid_line_value[0] = '\0';
  msgid_line_tok[0] = '\0';
}

void
msg_tag_line_time(char *buf, size_t buflen)
{
  const struct MsgTag *tag;

  if (!buf || buflen < 2)
    return;

  /* What upstream called it, if anything did.  A message that crossed a
   * link was stamped where it started, and a store that restamped it on
   * arrival would order the same conversation differently on every
   * server.
   */
  tag = msg_tag_find(parse_tags(), "time");
  if (tag && tag->value && tag->value[0]) {
    ircd_strncpy(buf, tag->value, buflen - 1);
    buf[buflen - 1] = '\0';
    return;
  }

  msg_tag_format_time(buf, buflen, CurrentTime);
}

const char *
msg_tag_line_msgid(const char *tok)
{
  if (!msgid_line_open || !msgid_line_wanted)
    return NULL;

  /* The identifier names the message this line carried.  A numeric sent
   * back while handling it, or a notice the server puts out in passing,
   * happen during the same line but are not that message, and giving them
   * its name would have anything that stores messages record the wrong
   * thing under it.
   */
  if (tok && ircd_strcmp(tok, msgid_line_tok))
    return NULL;

  if (!msgid_line_value[0]) {
    ircd_strncpy(msgid_line_value, msgid_new(), sizeof(msgid_line_value) - 1);
    msgid_line_value[sizeof(msgid_line_value) - 1] = '\0';
  }

  return msgid_line_value;
}

int
msg_tag_key_server(const char *key)
{
  if (!key)
    return 0;
  return !ircd_strcmp(key, "time") || !ircd_strcmp(key, "account")
    || !ircd_strcmp(key, "batch") || !ircd_strcmp(key, "msgid");
}

int
msg_tag_key_client_only(const char *key)
{
  return key && key[0] == '+';
}

int
msg_tag_client_allowed(const char *key)
{
  const char *name;
  int i;

  if (!msg_tag_key_client_only(key))
    return 0;

  name = clienttag_lookup_name(key);
  if (!name || !*name)
    return 0;

  if (clienttag_deny_all) {
    for (i = 0; i < clienttag_name_count; ++i) {
      if (!ircd_strcmp(name, clienttag_names[i]))
        return 1;
    }
    return 0;
  }

  for (i = 0; i < clienttag_name_count; ++i) {
    if (!ircd_strcmp(name, clienttag_names[i]))
      return 0;
  }
  return 1;
}

int
msg_tag_have_client_relay(struct MsgTag *tags)
{
  for (; tags; tags = tags->next) {
    if (msg_tag_key_client_only(tags->key)
        && msg_tag_client_allowed(tags->key))
      return 1;
  }
  return 0;
}

/** Return non-zero if a client is allowed to send \a key.
 *
 * Everything else a client puts in front of a line is dropped before the
 * command is dispatched: a client may not set a server tag, because a tag
 * the server vouches for is worth nothing if anybody can write it.
 *
 * The one non-client tag a client may send is @c label.  It is the whole
 * point of labeled-response -- the client names its own request so the
 * server can name the answer -- and it goes no further than the command it
 * arrived on: msg_tag_format() forwards only client-only tags to other
 * clients, and msg_tag_format_s2s() only federated ones, so a label never
 * reaches anybody but the client that wrote it.
 */
static int
msg_tag_client_may_send(const char *key)
{
  if (msg_tag_key_client_only(key))
    return msg_tag_client_allowed(key);

  /* A client sends @batch= to say which of its own batches a line belongs
   * to: that is how a message longer than a line is sent.  Like the label,
   * it is consumed here and goes no further -- see msg_tag_format_s2s().
   */
  return key && (!ircd_strcmp(key, "label")
                 || !ircd_strcmp(key, "batch")
                 || !ircd_strcmp(key, "draft/multiline-concat"));
}

struct MsgTag *
msg_tag_filter_client(struct MsgTag *tags)
{
  struct MsgTag *head = NULL;
  struct MsgTag **tail = &head;

  for (; tags; tags = tags->next) {
    if (msg_tag_client_may_send(tags->key)) {
      *tail = tags;
      tail = &tags->next;
    }
  }
  *tail = NULL;
  return head;
}

static int
msg_tag_wants_time(struct Client *to)
{
  if (!to || IsServer(to))
    return 0;
  return CapHas(cli_active(to), CAP_SERVER_TIME)
    || CapHas(cli_active(to), CAP_MESSAGE_TAGS);
}

int
msg_tag_key_federated(const char *key)
{
  if (!key)
    return 0;
  return !ircd_strcmp(key, "time") || !ircd_strcmp(key, "batch")
    || !ircd_strcmp(key, "msgid");
}

int
msg_tag_s2s_needs_time(const char *tok)
{
  if (!tok)
    return 0;
  /* Omit @time= on link/state and net-admin protocol.  Everything else that
   * hits S2S is treated as (eventually) client-visible. */
  if (!ircd_strcmp(tok, TOK_BURST)
      || !ircd_strcmp(tok, TOK_END_OF_BURST)
      || !ircd_strcmp(tok, TOK_END_OF_BURST_ACK)
      || !ircd_strcmp(tok, TOK_SERVER)
      || !ircd_strcmp(tok, TOK_PING)
      || !ircd_strcmp(tok, TOK_PONG)
      || !ircd_strcmp(tok, TOK_SETTIME)
      || !ircd_strcmp(tok, TOK_ASLL)
      || !ircd_strcmp(tok, TOK_RPING)
      || !ircd_strcmp(tok, TOK_RPONG)
      || !ircd_strcmp(tok, TOK_UPING)
      || !ircd_strcmp(tok, TOK_PASS)
      || !ircd_strcmp(tok, TOK_ERROR)
      || !ircd_strcmp(tok, TOK_PROTO)
      || !ircd_strcmp(tok, TOK_SQUIT)
      || !ircd_strcmp(tok, TOK_CONFIG)
      || !ircd_strcmp(tok, TOK_JUPE)
      || !ircd_strcmp(tok, TOK_GLINE)
      || !ircd_strcmp(tok, TOK_SLINE)
      /* Server<->services RPC: consumed by services software that parses
       * P10 fields positionally and does not strip tags.  A @time= prefix
       * shifts every field and breaks SASL/spamfilter routing. */
      || !ircd_strcmp(tok, TOK_XQUERY)
      || !ircd_strcmp(tok, TOK_XREPLY)
      || !ircd_strcmp(tok, TOK_DESTRUCT))
    return 0;
  return 1;
}

/** Append one tag to a wire prefix; \a *wrote tracks whether '@' was emitted. */
static char *
msg_tag_append(char *pos, char *end, int *wrote, const char *key,
               const char *value)
{
  if (!key || pos + 1 >= end)
    return NULL;

  if (*wrote)
    *pos++ = ';';
  else {
    *pos++ = '@';
    *wrote = 1;
  }

  /* ircd_snprintf() returns the untruncated (would-be) length, so pos can
   * land past end when the key does not fit.  Bail before the next write:
   * (end - pos) would go negative and wrap to a huge size_t. */
  pos += ircd_snprintf(0, pos, end - pos, "%s", key);
  if (pos >= end)
    return NULL;
  if (value) {
    *pos++ = '=';
    /* Escape straight into the output buffer (bounded by end): no fixed
     * scratch buffer, so long tag values are not truncated. */
    pos += msg_tag_escape(value, pos, end - pos);
  }

  return (pos < end) ? pos : NULL;
}

unsigned int
msg_tag_format_s2s(char *buf, size_t buflen, struct MsgTag *tags,
                   time_t local_time, int invent_time, const char *msgid)
{
  char *pos = buf;
  char *end = buf + buflen;
  int wrote = 0;
  struct MsgTag *tag;
  const struct MsgTag *time_tag = msg_tag_find(tags, "time");

  if (buflen < 3)
    return 0;

  /* Only stamp/forward time on client-event commands, and only when
   * NETWORK_TIME is enabled. */
  if (invent_time && feature_bool(FEAT_NETWORK_TIME)) {
    char tbuf[32];

    if (time_tag && time_tag->value)
      ircd_strncpy(tbuf, time_tag->value, sizeof(tbuf) - 1);
    else
      msg_tag_format_time(tbuf, sizeof(tbuf), local_time);
    tbuf[sizeof(tbuf) - 1] = '\0';

    pos = msg_tag_append(pos, end, &wrote, "time", tbuf);
    if (!pos)
      return 0;
  }

  /* msgid, so that every server on the network calls this message by the
   * same name.  Taken from the line rather than forwarded out of \a tags:
   * upstream's identifier is already there when the message came from a
   * server, and a client's own is not trusted (see msg_tag_line_begin).
   */
  if (msgid) {
    pos = msg_tag_append(pos, end, &wrote, "msgid", msgid);
    if (!pos)
      return 0;
  }

  for (tag = tags; tag; tag = tag->next) {
    if (!ircd_strcmp(tag->key, "time") || !ircd_strcmp(tag->key, "account"))
      continue;
    /* Handled above, from the line: never forwarded straight through. */
    if (!ircd_strcmp(tag->key, "msgid"))
      continue;
    /* A batch is between one server and one client: the identifier means
     * nothing on the next link, and a client's own would be forwarded as
     * if this server had vouched for it.  Long messages cross P10 as the
     * separate messages they are made of.
     */
    if (!ircd_strcmp(tag->key, "batch"))
      continue;
    if (msg_tag_key_client_only(tag->key))
      continue;
    if (!msg_tag_key_federated(tag->key))
      continue;
    pos = msg_tag_append(pos, end, &wrote, tag->key, tag->value);
    if (!pos)
      return 0;
  }

  if (!wrote)
    return 0;

  if (pos + 1 >= end)
    return 0;
  *pos++ = ' ';
  *pos = '\0';
  return (unsigned int)(pos - buf);
}

unsigned int
msg_tag_profile(struct Client *to, const char *msgid)
{
  unsigned int profile = TAGP_NONE;

  if (!to || IsServer(to))
    return TAGP_NONE;

  if (msg_tag_wants_time(to))
    profile |= TAGP_TIME;

  /* Its own bucket: the prefix cache reuses a rendered prefix across
   * recipients with the same profile, and a client that gets a msgid does
   * not get the same bytes as one that does not.
   */
  if (msgid && CapHas(cli_active(to), CAP_MESSAGE_TAGS))
    profile |= TAGP_MSGID;

  /* Likewise for a client that is inside a batch.  At most one client is,
   * so this never collapses two recipients into one bucket wrongly.
   */
  if (CapHas(cli_active(to), CAP_BATCH)
      && (batch_current(to) || batch_label_tag(to)))
    profile |= TAGP_BATCH;

  return profile;
}

unsigned int
msg_tag_format(char *buf, size_t buflen, struct Client *to,
               struct Client *from, struct MsgTag *tags, time_t local_time,
               const char *msgid)
{
  char *pos = buf;
  char *end = buf + buflen;
  int wrote = 0;
  struct MsgTag *tag;
  const struct MsgTag *time_tag;

  if (!to || IsServer(to) || buflen < 3)
    return 0;

  time_tag = msg_tag_find(tags, "time");

  /* server-time (before client tags, per IRCv3 ordering).
   * With NETWORK_TIME, prefer an upstream stamp; otherwise always use
   * the local queue/delivery time. */
  if (msg_tag_wants_time(to)) {
    char tbuf[32];

    if (feature_bool(FEAT_NETWORK_TIME) && time_tag && time_tag->value)
      ircd_strncpy(tbuf, time_tag->value, sizeof(tbuf) - 1);
    else
      msg_tag_format_time(tbuf, sizeof(tbuf), local_time);
    tbuf[sizeof(tbuf) - 1] = '\0';

    pos = msg_tag_append(pos, end, &wrote, "time", tbuf);
    if (!pos)
      return 0;
  }

  /* msgid, for a client that asked for message-tags.
   *
   * Nothing else may see it.  A client that negotiated nothing gets the
   * line it has always got: an identifier is an addition for the clients
   * that asked to be told about tags, never a change to what a
   * traditional client is sent.
   */
  if (msgid && CapHas(cli_active(to), CAP_MESSAGE_TAGS)) {
    pos = msg_tag_append(pos, end, &wrote, "msgid", msgid);
    if (!pos)
      return 0;
  }

  /* batch and label.
   *
   * A client that did not ask for batches is sent neither: it gets the
   * messages loose, which is the line it has always got.  The label rides
   * only on the BATCH line that opens a labeled response, or on the bare
   * ACK; the messages inside are identified by the batch.
   */
  if (CapHas(cli_active(to), CAP_BATCH)) {
    const char *id = batch_current(to);
    const char *label = batch_label_tag(to);

    if (label) {
      pos = msg_tag_append(pos, end, &wrote, "label", label);
      if (!pos)
        return 0;
    }
    if (id) {
      pos = msg_tag_append(pos, end, &wrote, "batch", id);
      if (!pos)
        return 0;
    }
  }

  /* client-only tags */
  if (CapHas(cli_active(to), CAP_MESSAGE_TAGS)) {
    for (tag = tags; tag; tag = tag->next) {
      if (!msg_tag_key_client_only(tag->key))
        continue;
      if (!msg_tag_client_allowed(tag->key))
        continue;
      pos = msg_tag_append(pos, end, &wrote, tag->key, tag->value);
      if (!pos)
        return 0;
    }
  }

  if (!wrote)
    return 0;

  if (pos + 1 >= end)
    return 0;
  *pos++ = ' ';
  *pos = '\0';
  return (unsigned int)(pos - buf);
}

unsigned int
msg_tag_assemble(char *out, size_t outlen,
                 const char *prefix, unsigned int prefix_len,
                 const char *body, unsigned int body_len)
{
  if (prefix_len + body_len >= outlen)
    return 0;
  if (prefix_len)
    memcpy(out, prefix, prefix_len);
  memcpy(out + prefix_len, body, body_len);
  out[prefix_len + body_len] = '\0';
  return prefix_len + body_len;
}
