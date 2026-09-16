/*
 * IRC - Internet Relay Chat, modules/hooks/richtext.c
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
 * @brief Rich text, and the plain text that goes with it.
 *
 * Phase 4 of proposal 006.  IRC has no content type, and the thing not to
 * do about that is invent a dialect of control codes: every client would
 * have to learn it, every one that did not would show the codes, and the
 * network would be split between the two for ever.
 *
 * So the format is negotiated -- capability @c blacknode/richtext -- and
 * the body is bounded Markdown, marked with @c +blacknode/format=markdown.
 * A client that asked for the capability gets the Markdown.  **Everybody
 * else gets the plain-text equivalent, generated here.**  That is not a
 * nicety; it is the whole design.  Without it the network splits in two
 * and every decision after this one is made twice.
 *
 * Which means one message goes out as two bodies, and this module does
 * not deliver either of them.  It hooks #HOOK_MESSAGE_PRE_CHANNEL and
 * #HOOK_MESSAGE_PRE_PRIVATE, checks the Markdown, writes the plain text
 * into HookContext::hc_alt and names the capability that chooses between
 * them; the relay does the rest.  Mechanism in the core, policy in the
 * module: *how* a message reaches two kinds of client is the relay's
 * business, and *what Markdown means* is this module's.
 *
 * The sanitising happens here too, and only here.  A client that says its
 * text is Markdown does not get to decide what Markdown means: the
 * nesting depth, the length of a link, and the fact that there is no HTML
 * in it at all are the server's to enforce, because a client that
 * enforced them would be one that could be replaced by one that did not.
 */
#include "config.h"

#include "capab.h"
#include "client.h"
#include "hooks.h"
#include "ircd.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "module.h"
#include "msg.h"
#include "msg_tag.h"
#include "parse.h"
#include "struct.h"

#include <string.h>

/** The capability a client negotiates to be sent Markdown. */
#define RICH_CAP_NAME "blacknode/richtext"

/** Its value under CAP LS 302: what this server will accept. */
#define RICH_CAP_VALUE "markdown"

/** The tag that says a body is Markdown. */
#define RICH_TAG "+blacknode/format"

/** The only value it may have. */
#define RICH_FORMAT "markdown"

/** How deep emphasis, quotes and lists may nest.
 *
 * Three is a quoted list with a bold word in it.  Past that the text is
 * not being formatted, it is being used to make a parser work hard.
 */
#define RICH_MAX_DEPTH 3

/** Longest link target kept. */
#define RICH_MAX_LINK 256

/** This module's handle. */
static struct ModuleHandle* rich_mod;

/** The capability's position, or -1. */
static int rich_cap = -1;

/* ------------------------------------------------------------------- *
 * Sanitising                                                          *
 * ------------------------------------------------------------------- */

/** Non-zero if \a text contains anything that must never be relayed.
 *
 * There is no HTML in Markdown here.  Markdown as a language allows raw
 * HTML, and a client rendering what it is told is a client that can be
 * made to render anything; the rule is that this server's Markdown is the
 * inline subset and nothing else, and the way to keep it that way is to
 * refuse the characters that start the parts that are not.
 */
static int rich_forbidden(const char* text)
{
  const char* p;

  for (p = text; *p; p++) {
    /* No tags, no entities, no comments.  An angle bracket has no meaning
     * in the subset this server accepts, so refusing it costs nothing and
     * closes the whole question. */
    if (*p == '<' || *p == '>')
      return 1;

    /* Formatting is what the client does with the Markdown; a message
     * that also carries IRC colour codes is one that renders differently
     * in the two halves of the network no matter what is done here. */
    if (*p == '\002' || *p == '\003' || *p == '\017' || *p == '\026'
        || *p == '\035' || *p == '\037' || *p == '\036' || *p == '\021')
      return 1;
  }

  return 0;
}

/** Non-zero if the nesting in \a text stays inside #RICH_MAX_DEPTH.
 *
 * Counted rather than parsed: what is being bounded is how much work a
 * renderer can be made to do, and for that the number of open markers is
 * the number that matters.  A precise parser here would be a second
 * implementation of Markdown to keep in step with every client's.
 */
static int rich_depth_ok(const char* text)
{
  char open[RICH_MAX_DEPTH + 1];
  int depth = 0;
  const char* p;

  for (p = text; *p; p++) {
    char marker;

    if (*p == '\\' && p[1]) {
      p++;                      /* an escaped marker is not a marker */
      continue;
    }

    if (*p != '*' && *p != '_' && *p != '~' && *p != '`')
      continue;

    marker = *p;

    /* A run of the same character is one marker, not several: ** is
     * bold, and *** is bold and italic together, but both open once. */
    while (p[1] == marker)
      p++;

    if (depth > 0 && open[depth - 1] == marker) {
      depth--;                  /* it closes the one it matches */
      continue;
    }

    /* An unmatched marker counts as opening, which is the worst case and
     * therefore the right one to bound.  A client that meant it as
     * punctuation loses nothing: the check is on how deep it goes, not
     * on whether it was balanced. */
    if (depth >= RICH_MAX_DEPTH)
      return 0;

    open[depth++] = marker;
  }

  return 1;
}

/** Non-zero if every link in \a text is one this server will carry. */
static int rich_links_ok(const char* text)
{
  const char* p;

  for (p = text; *p; p++) {
    const char* target;
    size_t len;

    if (*p != '(' || p == text || p[-1] != ']')
      continue;

    target = p + 1;
    len = strcspn(target, ")");

    if (len > RICH_MAX_LINK)
      return 0;

    /* Only the two schemes that mean "somewhere on the web", and only
     * with the scheme written out.  A relative link means nothing without
     * a document to be relative to, and every other scheme is a way to
     * make a client do something rather than go somewhere. */
    if (ircd_strncmp(target, "https://", 8)
        && ircd_strncmp(target, "http://", 7))
      return 0;

    p = target + len;
    if (!*p)
      break;
  }

  return 1;
}

/* ------------------------------------------------------------------- *
 * Rendering                                                           *
 * ------------------------------------------------------------------- */

/** Write the plain-text equivalent of \a src into \a buf.
 *
 * What a person would have typed if they had not had formatting: the
 * markers come off, a link becomes its text followed by its target in
 * angle brackets, and a quote keeps its "> " because that is how a
 * quotation has been written in plain text since long before Markdown.
 *
 * It never fails and never refuses: by the time this runs the text has
 * already been through the checks, and what is left is a rendering that
 * has to produce something for every input or half the network gets
 * nothing.
 */
static void rich_to_plain(char* buf, size_t buflen, const char* src)
{
  const char* p = src;
  char* out = buf;
  char* end = buf + buflen - 1;

  while (*p && out < end) {
    /* An escaped character is the character. */
    if (*p == '\\' && p[1]) {
      *out++ = *++p;
      p++;
      continue;
    }

    /* A link: [text](target) becomes "text <target>". */
    if (*p == '[') {
      const char* text = p + 1;
      size_t tlen = strcspn(text, "]");

      if (text[tlen] == ']' && text[tlen + 1] == '(') {
        const char* target = text + tlen + 2;
        size_t alen = strcspn(target, ")");

        if (target[alen] == ')') {
          size_t i;

          for (i = 0; i < tlen && out < end; i++)
            *out++ = text[i];

          /* The target as well, because a plain-text reader who cannot
           * click the words needs to be able to read the address.  Unless
           * the words *are* the address, in which case saying it twice
           * helps nobody. */
          if (!(tlen == alen && !strncmp(text, target, tlen))) {
            if (out < end) *out++ = ' ';
            if (out < end) *out++ = '<';
            for (i = 0; i < alen && out < end; i++)
              *out++ = target[i];
            if (out < end) *out++ = '>';
          }

          p = target + alen + 1;
          continue;
        }
      }
    }

    /* Emphasis, code and strikethrough: the markers come off. */
    if (*p == '*' || *p == '_' || *p == '~' || *p == '`') {
      char marker = *p;

      while (*p == marker)
        p++;
      continue;
    }

    *out++ = *p++;
  }

  *out = '\0';
}

/* ------------------------------------------------------------------- *
 * Delivery                                                            *
 * ------------------------------------------------------------------- */

/** Give a message that says it is Markdown its plain-text twin.
 * @param[in,out] ctx What the server is about to relay.
 * @param[in] user Unused.
 * @return #HOOK_CONTINUE for anything that is not rich text.
 */
static enum HookResult rich_message(struct HookContext* ctx, void* user)
{
  struct MsgTag* tag;
  char plain[BUFSIZE];
  const char* text = ctx->hc_arg;

  (void) user;

  if (rich_cap < 0 || !text || !*text)
    return HOOK_CONTINUE;

  tag = msg_tag_find(parse_tags(), RICH_TAG);

  if (!tag || !tag->value || ircd_strcmp(tag->value, RICH_FORMAT))
    return HOOK_CONTINUE;

  /* A client that asked to send Markdown but never negotiated the
   * capability has not agreed to anything: the capability is what says
   * both ends know what this means. */
  if (!ctx->hc_source || !MyConnect(ctx->hc_source)
      || !CapActive(ctx->hc_source, rich_cap))
    return HOOK_CONTINUE;

  /* Refused one reason at a time, because "your message was wrong" is
   * not something anybody can act on. */
  if (rich_forbidden(text)) {
    ircd_strncpy(ctx->hc_reason,
                 "Rich text may not contain HTML or IRC formatting codes",
                 sizeof(ctx->hc_reason) - 1);
    return HOOK_DENY;
  }

  if (!rich_depth_ok(text)) {
    ircd_strncpy(ctx->hc_reason,
                 "Rich text formatting is nested too deeply",
                 sizeof(ctx->hc_reason) - 1);
    return HOOK_DENY;
  }

  if (!rich_links_ok(text)) {
    ircd_strncpy(ctx->hc_reason,
                 "A link in rich text must be http:// or https:// and short",
                 sizeof(ctx->hc_reason) - 1);
    return HOOK_DENY;
  }

  if (!ctx->hc_alt || ctx->hc_alt_len < 2)
    return HOOK_CONTINUE;      /* a hook point with no second body */

  rich_to_plain(plain, sizeof(plain), text);

  /* Nothing left after the markers came off is a message that was only
   * formatting.  Sending the Markdown to half the network and an empty
   * line to the other half is worse than refusing it. */
  if (!plain[0]) {
    ircd_strncpy(ctx->hc_reason, "That message is only formatting",
                 sizeof(ctx->hc_reason) - 1);
    return HOOK_DENY;
  }

  /* And that is the whole job.  The relay does the rest: the Markdown to
   * the clients that negotiated it and over the links, this to everybody
   * else, and this again to whatever stores messages -- because a
   * transcript is read back by whoever reads it, and the one body it can
   * keep is the one everybody can read.
   */
  ircd_strncpy(ctx->hc_alt, plain, ctx->hc_alt_len - 1);
  ctx->hc_alt[ctx->hc_alt_len - 1] = '\0';
  ctx->hc_alt_cap = rich_cap;
  ctx->hc_alt_tag = RICH_TAG;
  ctx->hc_alt_set = 1;

  return HOOK_CONTINUE;
}

/* ------------------------------------------------------------------- *
 * Lifecycle                                                           *
 * ------------------------------------------------------------------- */

/** @return Zero on success. */
static int richtext_init(struct ModuleHandle* mod)
{
  rich_mod = mod;

  if (!module_add_cap(mod, RICH_CAP_NAME, 0, &rich_cap)) {
    rich_mod = NULL;
    return -1;
  }

  cap_set_value(rich_cap, RICH_CAP_VALUE);

  if (!module_add_hook(mod, HOOK_MESSAGE_PRE_CHANNEL, rich_message,
                       HOOK_PRIORITY_DEFAULT, 0)
      || !module_add_hook(mod, HOOK_MESSAGE_PRE_PRIVATE, rich_message,
                          HOOK_PRIORITY_DEFAULT, 0)) {
    rich_mod = NULL;
    return -1;
  }

  /* The tag has to be relayable or a client with the capability would be
   * sent Markdown with nothing saying so.  Said once, at load, because an
   * operator who has to discover this from a bug report has already
   * shipped it. */
  if (!msg_tag_client_allowed(RICH_TAG))
    log_write(LS_SYSTEM, L_WARNING, 0,
              "richtext: CLIENTTAGDENY does not allow %s, so no message "
              "will ever be treated as Markdown; add -%s to it",
              RICH_TAG, RICH_TAG + 1);

  return 0;
}

/** @param[in] mod Handle for this module. */
static void richtext_fini(struct ModuleHandle* mod)
{
  (void) mod;

  rich_cap = -1;
  rich_mod = NULL;
}

/** What the loader reads. */
struct ModuleInfo ircu_module = {
  IRCU_MODULE_ABI,
  "richtext",
  "1.0",
  "ircu2",
  "bounded Markdown for the clients that asked, plain text for the rest",
  richtext_init,
  richtext_fini,
  0
};
