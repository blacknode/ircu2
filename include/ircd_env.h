#ifndef INCLUDED_ircd_env_h
#define INCLUDED_ircd_env_h
/*
 * IRC - Internet Relay Chat, include/ircd_env.h
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
 * Two things live here, and they are meant to be used together:
 *
 *   - @b Typed @b accessors -- env_get(), env_str(), env_int(), env_bool() --
 *     for code that wants one value out of the environment with a default and
 *     a range, instead of spelling out getenv() plus strtol() plus a warning
 *     every time.
 *
 *   - @b Placeholder @b expansion -- env_expand() -- which rewrites every
 *     <tt>${NAME}</tt> reference in a string into the value of that
 *     environment variable.  ircd_lexer.c runs every quoted string in
 *     ircd.conf through it, which is what lets a container pass a link
 *     password or a server name in through the environment instead of
 *     templating the config file before start-up.
 *
 * @section env_syntax Placeholder syntax
 *
 * The forms are the familiar POSIX shell ones, and only these:
 *
 * <table>
 * <tr><td><tt>${NAME}</tt></td>
 *     <td>The value.  Unset is an error unless #ENV_LENIENT is given.</td></tr>
 * <tr><td><tt>${NAME:-word}</tt></td>
 *     <td>The value if set and non-empty, otherwise @a word.</td></tr>
 * <tr><td><tt>${NAME-word}</tt></td>
 *     <td>The value if set (even if empty), otherwise @a word.</td></tr>
 * <tr><td><tt>${NAME:+word}</tt></td>
 *     <td>@a word if set and non-empty, otherwise nothing.</td></tr>
 * <tr><td><tt>${NAME+word}</tt></td>
 *     <td>@a word if set (even if empty), otherwise nothing.</td></tr>
 * <tr><td><tt>${NAME:?word}</tt></td>
 *     <td>The value if set and non-empty, otherwise fail with @a word as the
 *         error message.</td></tr>
 * <tr><td><tt>${NAME?word}</tt></td>
 *     <td>The value if set, otherwise fail with @a word.</td></tr>
 * </table>
 *
 * A @c word may itself contain placeholders (<tt>${A:-${B:-none}}</tt>),
 * nested up to #ENV_MAX_DEPTH.  A variable's @em value never is: what comes
 * out of the environment is copied in verbatim and is not rescanned, so a
 * value can never smuggle a reference to another variable.
 *
 * A @c $ that does not start one of those forms is an ordinary character.
 * <tt>$$</tt> is a literal @c $, which is how a literal <tt>${</tt> is
 * written: <tt>$${</tt>.
 *
 * @section env_rules What expansion does not do
 *
 * Expansion is a string operation and nothing else.  In ircd.conf it happens
 * inside the lexer, on the contents of a quoted string, after the string has
 * been recognised: an expanded value can therefore never open a block, end a
 * statement or become a keyword, no matter what it contains.  The one place a
 * reference may appear unquoted is where the grammar wants a number, and
 * there the result has to be decimal digits or the config fails to parse.
 *
 * Nothing here ever puts a variable's value in a log line or in an error
 * message: the diagnostics name the variable, never the secret it holds.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>          /* size_t */
#define INCLUDED_sys_types_h
#endif

/** Longest environment variable name a placeholder may name. */
#define ENV_NAME_MAX 128
/** Longest string env_expand() will produce. */
#define ENV_EXPAND_MAX 8192
/** Deepest nesting of placeholders inside a default word. */
#define ENV_MAX_DEPTH 8

/** Outcome of an expansion. */
enum EnvResult {
  ENV_OK,               /**< Expanded; no problem. */
  ENV_ERR_SYNTAX,       /**< Malformed placeholder (no @c }, bad modifier). */
  ENV_ERR_NAME,         /**< Empty or otherwise invalid variable name. */
  ENV_ERR_UNSET,        /**< Variable unset and no default given. */
  ENV_ERR_DEPTH,        /**< Nested deeper than #ENV_MAX_DEPTH. */
  ENV_ERR_TOOLONG,      /**< Result would exceed #ENV_EXPAND_MAX. */
  ENV_ERR_NOTNUM        /**< env_expand_number(): result is not a number. */
};

/** Why an expansion failed, and where. */
struct EnvError {
  enum EnvResult code;  /**< #ENV_OK after a successful expansion. */
  char text[256];       /**< Message for the operator; never holds a value. */
};

/** Expand an unset <tt>${NAME}</tt> to the empty string instead of failing. */
#define ENV_LENIENT 0x0001

extern const char* env_get(const char* name);
extern const char* env_str(const char* name, const char* def);
extern int env_int(const char* name, int def, int min, int max);
extern int env_bool(const char* name, int def);

extern int env_has_ref(const char* src);
extern char* env_expand(const char* src, unsigned int flags,
                        struct EnvError* err);
extern int env_expand_buf(char* dst, size_t size, const char* src,
                          unsigned int flags, struct EnvError* err);
extern int env_expand_number(const char* src, unsigned int flags, int* out,
                             struct EnvError* err);

#endif /* INCLUDED_ircd_env_h */
