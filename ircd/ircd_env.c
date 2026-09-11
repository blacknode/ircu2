/*
 * IRC - Internet Relay Chat, ircd/ircd_env.c
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
 * @brief Configuration from the process environment.
 *
 * See include/ircd_env.h for the placeholder syntax and the rules this file
 * keeps.  Two of those rules shape the code below:
 *
 *   - A variable's value is copied into the output and never rescanned, so
 *     one variable cannot expand into a reference to another.  Only the
 *     default @a word of a placeholder is expanded again, and only
 *     #ENV_MAX_DEPTH times.
 *
 *   - No diagnostic ever contains a value.  Every message here names the
 *     variable and stops there; the environment is where the link passwords
 *     and the oper hashes come from.
 */
#include "config.h"

#include "ircd_env.h"
#include "ircd_alloc.h"
#include "ircd_log.h"

/* NOTE: like ircd_lexer.c, this file uses <ctype.h> rather than
 * ircd_chattr.h: IsAlnum() is true for "{|}~[\\]^", which would let a
 * placeholder name characters no shell would accept.
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Growable output string used while expanding. */
struct env_buf {
  char* data;                   /**< NUL-terminated once finished. */
  size_t len;                   /**< Bytes used, excluding the NUL. */
  size_t cap;                   /**< Bytes allocated. */
};

/** Compare \a a and \a b ignoring ASCII case.
 * ircd_strcmp() would do, but it maps "[]\\" onto "{}|" the way nick
 * comparison needs, and this file only ever compares plain English words.
 * @return Non-zero if they are equal.
 */
static int env_streq_ci(const char* a, const char* b)
{
  while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
    ++a;
    ++b;
  }
  return *a == *b;
}

/** Record a failure in \a err and return zero.
 * @param[out] err Error to fill in (never NULL here).
 * @param[in] code Result code to report.
 * @param[in] fmt Message format; must not interpolate any variable's value.
 * @return Always zero, so callers can "return env_fail(...)".
 */
static int env_fail(struct EnvError* err, enum EnvResult code,
                    const char* fmt, ...)
{
  va_list vl;

  err->code = code;
  va_start(vl, fmt);
  vsnprintf(err->text, sizeof(err->text), fmt, vl);
  va_end(vl);
  return 0;
}

/** Make room for \a need more bytes in \a buf.
 * @return Non-zero on success, zero if the result would be too long.
 */
static int buf_reserve(struct env_buf* buf, size_t need, struct EnvError* err)
{
  size_t want;

  if (buf->len + need > ENV_EXPAND_MAX)
    return env_fail(err, ENV_ERR_TOOLONG,
                    "expanded value exceeds %d bytes", ENV_EXPAND_MAX);

  if (buf->len + need + 1 <= buf->cap)
    return 1;

  want = buf->cap ? buf->cap : 64;
  while (want < buf->len + need + 1)
    want *= 2;
  if (want > (size_t)ENV_EXPAND_MAX + 1)
    want = (size_t)ENV_EXPAND_MAX + 1;

  buf->data = (char*)MyRealloc(buf->data, want);
  buf->cap = want;
  return 1;
}

/** Append \a len bytes of \a text to \a buf. */
static int buf_add(struct env_buf* buf, const char* text, size_t len,
                   struct EnvError* err)
{
  if (!len)
    return buf_reserve(buf, 0, err);
  if (!buf_reserve(buf, len, err))
    return 0;
  memcpy(buf->data + buf->len, text, len);
  buf->len += len;
  buf->data[buf->len] = '\0';
  return 1;
}

/** Release the memory held by \a buf. */
static void buf_free(struct env_buf* buf)
{
  MyFree(buf->data);
  buf->data = NULL;
  buf->len = buf->cap = 0;
}

static int expand_into(struct env_buf* out, const char* src,
                       unsigned int flags, int depth, struct EnvError* err);

/** Expand one placeholder.
 * @param[in,out] out Output being built.
 * @param[in] src Whole source string.
 * @param[in,out] pos On entry, offset of the first character of the name
 *   (just past "${"); on success, offset just past the closing brace.
 * @param[in] flags Bitwise combination of #ENV_LENIENT.
 * @param[in] depth Current nesting depth.
 * @param[out] err Error description on failure.
 * @return Non-zero on success.
 */
static int expand_ref(struct env_buf* out, const char* src, size_t* pos,
                      unsigned int flags, int depth, struct EnvError* err)
{
  char name[ENV_NAME_MAX + 1];
  const char* value;
  char* word = NULL;
  size_t start = *pos;
  size_t p = *pos;
  size_t name_len;
  size_t word_len;
  int braces;
  int colon = 0;
  int mod = 0;
  int set;
  int ok;

  /* The name: the character set every shell agrees on. */
  if (!src[p])
    return env_fail(err, ENV_ERR_SYNTAX, "unterminated \"${\" reference");
  if (!isalpha((unsigned char)src[p]) && src[p] != '_')
    return env_fail(err, ENV_ERR_NAME,
                    "invalid environment variable name in \"${\" reference");
  while (isalnum((unsigned char)src[p]) || src[p] == '_')
    ++p;
  name_len = p - start;
  if (name_len > ENV_NAME_MAX)
    return env_fail(err, ENV_ERR_NAME,
                    "environment variable name longer than %d characters",
                    ENV_NAME_MAX);
  memcpy(name, src + start, name_len);
  name[name_len] = '\0';

  /* The modifier, if any. */
  if (src[p] == ':') {
    colon = 1;
    ++p;
  }
  if (src[p] == '-' || src[p] == '+' || src[p] == '?') {
    mod = src[p++];
  } else if (colon || src[p] != '}') {
    if (!src[p])
      return env_fail(err, ENV_ERR_SYNTAX,
                      "unterminated \"${%s\" reference", name);
    return env_fail(err, ENV_ERR_SYNTAX,
                    "unknown modifier '%c' in \"${%s\" reference", src[p],
                    name);
  }

  /* The word, up to the brace that closes this reference. */
  start = p;
  for (braces = 1; braces > 0; ++p) {
    if (!src[p])
      return env_fail(err, ENV_ERR_SYNTAX,
                      "unterminated \"${%s\" reference", name);
    if (src[p] == '{')
      ++braces;
    else if (src[p] == '}')
      --braces;
  }
  word_len = (p - 1) - start;
  *pos = p;

  value = getenv(name);
  set = value && (!colon || *value != '\0');

  if (set && mod != '+')                /* ${X}, ${X-w}, ${X?w} with a value */
    return buf_add(out, value, strlen(value), err);

  if (mod == 0) {                       /* ${X} with nothing to fall back on */
    if (flags & ENV_LENIENT)
      return 1;
    return env_fail(err, ENV_ERR_UNSET,
                    "environment variable %s is not set", name);
  }

  if (mod == '+' && !set)               /* ${X+w} with no value: nothing */
    return 1;

  word = (char*)MyMalloc(word_len + 1);
  memcpy(word, src + start, word_len);
  word[word_len] = '\0';

  if (mod != '?') {
    ok = expand_into(out, word, flags, depth + 1, err);
    MyFree(word);
    return ok;
  }

  /* ${NAME:?word}: the word is the operator's own error message. */
  if (word_len) {
    struct env_buf msg = { NULL, 0, 0 };

    ok = expand_into(&msg, word, flags | ENV_LENIENT, depth + 1, err);
    if (ok)
      env_fail(err, ENV_ERR_UNSET, "environment variable %s: %s", name,
               msg.data ? msg.data : "");
    buf_free(&msg);
  } else {
    env_fail(err, ENV_ERR_UNSET, "environment variable %s is not set", name);
  }
  MyFree(word);
  return 0;
}

/** Expand \a src into \a out.
 * @return Non-zero on success.
 */
static int expand_into(struct env_buf* out, const char* src,
                       unsigned int flags, int depth, struct EnvError* err)
{
  size_t pos = 0;
  size_t run;

  if (depth > ENV_MAX_DEPTH)
    return env_fail(err, ENV_ERR_DEPTH,
                    "environment references nested more than %d deep",
                    ENV_MAX_DEPTH);

  while (src[pos]) {
    /* Copy everything up to the next '$' in one go. */
    for (run = pos; src[run] && src[run] != '$'; ++run) {}
    if (run > pos) {
      if (!buf_add(out, src + pos, run - pos, err))
        return 0;
      pos = run;
    }
    if (!src[pos])
      break;

    if (src[pos + 1] == '$') {
      if (!buf_add(out, "$", 1, err))
        return 0;
      pos += 2;
    } else if (src[pos + 1] == '{') {
      pos += 2;
      if (!expand_ref(out, src, &pos, flags, depth, err))
        return 0;
    } else {
      if (!buf_add(out, "$", 1, err))   /* a lone '$' is just a '$' */
        return 0;
      ++pos;
    }
  }

  return buf_reserve(out, 0, err);      /* guarantee an allocation */
}

/** Read \a name from the environment.
 * @param[in] name Variable to look up.
 * @return Its value, or NULL if it is not set.  An empty variable returns
 *   the empty string; the typed accessors below treat that as unset.
 */
const char* env_get(const char* name)
{
  assert(name != NULL);
  return getenv(name);
}

/** Read a string from the environment.
 * @param[in] name Variable to look up.
 * @param[in] def Value to use when \a name is unset or empty.
 * @return The value or \a def.
 */
const char* env_str(const char* name, const char* def)
{
  const char* value = env_get(name);

  return (value && *value) ? value : def;
}

/** Read an integer from the environment.
 * Anything unparseable or out of range is reported once and ignored.
 * @param[in] name Variable to look up.
 * @param[in] def Value to use when \a name is unset, empty or invalid.
 * @param[in] min Smallest acceptable value.
 * @param[in] max Largest acceptable value.
 * @return The value or \a def.
 */
int env_int(const char* name, int def, int min, int max)
{
  const char* value = env_get(name);
  char* end;
  long num;

  if (!value || !*value)
    return def;

  errno = 0;
  num = strtol(value, &end, 10);
  while (isspace((unsigned char)*end))
    ++end;
  if (*end || end == value || errno == ERANGE) {
    log_write(LS_CONFIG, L_WARNING, 0,
              "environment variable %s is not a number; using %d", name, def);
    return def;
  }
  if (num < (long)min || num > (long)max) {
    log_write(LS_CONFIG, L_WARNING, 0,
              "environment variable %s is outside %d..%d; using %d", name,
              min, max, def);
    return def;
  }
  return (int)num;
}

/** Read a boolean from the environment.
 * Accepts 1/true/yes/on/enabled and 0/false/no/off/disabled, in any case.
 * @param[in] name Variable to look up.
 * @param[in] def Value to use when \a name is unset, empty or invalid.
 * @return Zero or one, or \a def.
 */
int env_bool(const char* name, int def)
{
  static const char* const yes[] = { "1", "true", "yes", "on", "enabled" };
  static const char* const no[] = { "0", "false", "no", "off", "disabled" };
  const char* value = env_get(name);
  unsigned int ii;

  if (!value || !*value)
    return def;

  for (ii = 0; ii < sizeof(yes) / sizeof(yes[0]); ++ii)
    if (env_streq_ci(value, yes[ii]))
      return 1;
  for (ii = 0; ii < sizeof(no) / sizeof(no[0]); ++ii)
    if (env_streq_ci(value, no[ii]))
      return 0;

  log_write(LS_CONFIG, L_WARNING, 0,
            "environment variable %s is not a boolean; using %s", name,
            def ? "yes" : "no");
  return def;
}

/** Tell whether \a src holds anything expansion would change.
 * @param[in] src String to examine.
 * @return Non-zero if it contains a '$'.
 */
int env_has_ref(const char* src)
{
  assert(src != NULL);
  return strchr(src, '$') != NULL;
}

/** Expand every placeholder in \a src.
 * @param[in] src String to expand.
 * @param[in] flags Bitwise combination of #ENV_LENIENT.
 * @param[out] err Filled in either way; may be NULL.
 * @return A newly allocated string the caller frees with MyFree(), or NULL
 *   if expansion failed.
 */
char* env_expand(const char* src, unsigned int flags, struct EnvError* err)
{
  struct env_buf out = { NULL, 0, 0 };
  struct EnvError local;

  assert(src != NULL);
  if (!err)
    err = &local;
  err->code = ENV_OK;
  err->text[0] = '\0';

  if (!expand_into(&out, src, flags, 0, err)) {
    buf_free(&out);
    return NULL;
  }

  assert(out.data != NULL);
  out.data[out.len] = '\0';
  return out.data;
}

/** Expand \a src into a caller-supplied buffer.
 * @param[out] dst Buffer to write to; left empty on failure.
 * @param[in] size Bytes available in \a dst, including the NUL.
 * @param[in] src String to expand.
 * @param[in] flags Bitwise combination of #ENV_LENIENT.
 * @param[out] err Filled in either way; may be NULL.
 * @return Length of the expansion, or -1 on failure.
 */
int env_expand_buf(char* dst, size_t size, const char* src,
                   unsigned int flags, struct EnvError* err)
{
  struct EnvError local;
  char* text;
  size_t len;

  assert(dst != NULL);
  assert(size > 0);
  if (!err)
    err = &local;

  dst[0] = '\0';
  text = env_expand(src, flags, err);
  if (!text)
    return -1;

  len = strlen(text);
  if (len >= size) {
    MyFree(text);
    env_fail(err, ENV_ERR_TOOLONG, "expanded value exceeds %lu bytes",
             (unsigned long)(size - 1));
    return -1;
  }

  memcpy(dst, text, len + 1);
  MyFree(text);
  return (int)len;
}

/** Expand \a src and require the result to be a number.
 * This is what an unquoted reference in ircd.conf goes through: the config
 * grammar wants a number there, and an environment variable that expands to
 * anything else must not become some other token.
 * @param[in] src String to expand.
 * @param[in] flags Bitwise combination of #ENV_LENIENT.
 * @param[out] out Set to the value on success.
 * @param[out] err Filled in either way; may be NULL.
 * @return Non-zero on success.
 */
int env_expand_number(const char* src, unsigned int flags, int* out,
                      struct EnvError* err)
{
  struct EnvError local;
  char* text;
  char* end;
  long num;
  int ok = 1;

  assert(out != NULL);
  if (!err)
    err = &local;

  text = env_expand(src, flags, err);
  if (!text)
    return 0;

  errno = 0;
  num = strtol(text, &end, 10);
  if (end == text || *end || errno == ERANGE || num < INT_MIN || num > INT_MAX)
    ok = env_fail(err, ENV_ERR_NOTNUM, "\"%s\" did not expand to a number",
                  src);
  else
    *out = (int)num;

  MyFree(text);
  return ok;
}
