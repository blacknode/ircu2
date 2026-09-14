#ifndef INCLUDED_ircd_i18n_h
#define INCLUDED_ircd_i18n_h
/*
 * IRC - Internet Relay Chat, include/ircd_i18n.h
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
 * @brief Translations from PO catalogs, per client.
 *
 * Everything the server says to one client -- a numeric, a NOTICE from
 * the server or from a bot -- can be translated.  The text stays in the
 * source in English; a catalog is a plain GNU PO file the server reads
 * itself (no @c msgfmt, no @c .mo, no libintl), one per language, under
 * a @em domain: @c core for the ircd, and one per module that ships a
 * @c po/ directory.  See doc/readme.translations and doc/proposals/005.
 *
 * @section i18n_marking Marking text
 *
 * @code
 *   send_reply(sptr, SND_EXPLICIT | RPL_REHASHING, N_(":Flushing MOTD cache"));
 *   sendcmdto_one(&me, CMD_NOTICE, sptr, _(sptr, "%C :Bot %s destroyed"), sptr, name);
 * @endcode
 *
 * _() and _n() translate now, for the client given, and return a string
 * that lives as long as the catalog: use it in the same statement, never
 * keep it.  N_() does nothing at run time: it marks a literal that is
 * translated later by whoever knows the recipient -- send_reply() for
 * the explicit formats, which looks every format up in @c core.  The
 * numerics of s_err.c are looked up with the numeric's code as context.
 *
 * A module defines @c I18N_DOMAIN before including this header so the
 * same macros go to its own domain:
 *
 * @code
 *   #define I18N_DOMAIN mod_i18n
 *   #include "ircd_i18n.h"
 *   static struct I18nDomain* mod_i18n;   // = module_i18n(mod) in mi_init
 * @endcode
 *
 * @section i18n_rules What a catalog may say
 *
 * A translation is a format for ircd_snprintf(), and a wrong one reads
 * memory it should not.  The loader refuses an entry, and keeps the
 * original, unless:
 *
 *   - the translation has no @c \\r, @c \\n or @c \\0 in it;
 *   - its directives are the same as the original's, in the same order,
 *     byte for byte -- flags, width, precision, modifier and conversion.
 *     @c %% may appear anywhere; only the text between directives is free;
 *   - a plural entry has exactly @c nplurals forms, each meeting the two
 *     rules above against @c msgid_plural;
 *   - the file is valid UTF-8 and its header says so, or says nothing.
 *
 * A file that does not parse at all is refused whole, and the previous
 * version of that language -- if there was one -- stays in force.
 *
 * @section i18n_threads Threads
 *
 * Nothing here may be called from a worker thread: the lookup reads the
 * client and the catalogs, and both belong to the main thread.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

struct Client;
struct StatDesc;

/** Room for a language code, terminator included: fifteen characters of
 * BCP 47 restricted to @c [A-Za-z]{2,3}(-[A-Za-z0-9]{1,8})*.
 */
#define I18N_LANG_MAX        16
/** Most codes a client may list, in order of preference. */
#define I18N_PREF_MAX        3
/** Most distinct preferences interned at once. */
#define I18N_PREF_TABLE_MAX  4096
/** Most distinct language codes with a catalog, across every domain. */
#define I18N_CODE_TABLE_MAX  256
/** Most plural forms a Plural-Forms header may declare (Arabic uses 6). */
#define I18N_NPLURALS_MAX    8
/** Longest line a PO file may have. */
#define I18N_PO_LINE_MAX     4096
/** The language the sources are written in: always available. */
#define I18N_SOURCE_LANG     "en"

/** A set of catalogs owned by the ircd or by one module.  Opaque. */
struct I18nDomain;

/** The ircd's own domain, loaded from PO_PATH. */
extern struct I18nDomain* i18n_core;

/*
 * Marking macros.  I18N_DOMAIN defaults to the core; a module redefines
 * it before including this header.
 */
#ifndef I18N_DOMAIN
#define I18N_DOMAIN i18n_core
#endif

/** Translate \a s now, for client \a to. */
#define _(to, s)            i18n_text(I18N_DOMAIN, (to), (s))
/** Translate \a s or its plural \a p for \a n, now, for client \a to. */
#define _n(to, s, p, n)     i18n_ntext(I18N_DOMAIN, (to), (s), (p), (n))
/** Mark \a s for extraction; it is translated later, where the recipient
 * is known. */
#define N_(s)               (s)

/*
 * Translation.
 */

/** Translate \a msgid for \a to in \a dom.
 * @param[in] dom Domain to search; NULL means "no translations".
 * @param[in] to Recipient; NULL or a server means "no translation".
 * @param[in] msgid Original text.
 * @return The translation, or \a msgid itself.  Valid until the domain is
 *   reloaded or closed; use it in the same statement.
 */
extern const char* i18n_text(const struct I18nDomain* dom,
                             const struct Client* to, const char* msgid);

/** Like i18n_text(), with a msgctxt.  send_reply() uses the numeric's
 * code ("401") as the context of every numeric format.
 */
extern const char* i18n_ctext(const struct I18nDomain* dom,
                              const struct Client* to, const char* ctx,
                              const char* msgid);

/** Translate \a msgid / \a plural for \a n according to the catalog's
 * Plural-Forms.  Without a catalog, or without Plural-Forms in it, this
 * returns \a msgid for n == 1 and \a plural otherwise.
 */
extern const char* i18n_ntext(const struct I18nDomain* dom,
                              const struct Client* to, const char* msgid,
                              const char* plural, unsigned long n);

/** Like i18n_ntext(), with a msgctxt. */
extern const char* i18n_cntext(const struct I18nDomain* dom,
                               const struct Client* to, const char* ctx,
                               const char* msgid, const char* plural,
                               unsigned long n);

/*
 * A client's preference.
 */

/** Non-zero if \a code has the syntax of a language code and fits. */
extern int i18n_valid_code(const char* code);

/** Non-zero if some domain has a catalog for \a code or for its primary
 * subtag, or if \a code is the source language.  What LANGUAGE accepts.
 */
extern int i18n_language_known(const char* code);

/** Set the preference of \a cptr to \a codes, in that order.
 *
 * Nothing is validated here beyond the syntax: ms_language() stores what
 * arrives so a hub can pass "fr-ca" on even without a catalog for it,
 * and m_language() checks availability itself before calling.
 * @param[in] cptr Client.
 * @param[in] codes Codes, each shorter than I18N_LANG_MAX.
 * @param[in] count Number of codes; more than I18N_PREF_MAX are dropped,
 *   and zero clears the preference.
 * @return Number of codes stored.
 */
extern unsigned int i18n_set_languages(struct Client* cptr,
                                       const char* const* codes,
                                       unsigned int count);

/** The preference of \a cptr as "es-ar es", for LG, 687 and 690.
 * @return The string, or NULL when the client has no preference.  Static
 *   storage, overwritten by the next call.
 */
extern const char* i18n_languages_str(const struct Client* cptr);

/*
 * Domains.
 */

/** Open a domain over \a dir and load every <code>.po in it.
 * @param[in] name Domain name, for logs and /STATS.
 * @param[in] dir Directory holding the catalogs.
 * @return The domain.  Never NULL: a directory with no catalogs is a
 *   domain with none.
 */
extern struct I18nDomain* i18n_domain_open(const char* name,
                                           const char* dir);

/** Close \a dom and free its catalogs.  NULL is accepted. */
extern void i18n_domain_close(struct I18nDomain* dom);

/** Load \a dom's directory again.  A file that fails to parse leaves the
 * language it names as it was; a language whose file is gone is dropped.
 * @return Number of files refused plus entries rejected.
 */
extern unsigned int i18n_domain_reload(struct I18nDomain* dom);

extern const char* i18n_domain_name(const struct I18nDomain* dom);

/** Non-zero if \a dom has a catalog for exactly \a code. */
extern int i18n_domain_has(const struct I18nDomain* dom, const char* code);

/*
 * Lifecycle.  Server-side; not for modules.
 */

/** Create the core domain, empty.  Before module_init(): a module loaded
 * from the configuration opens its own domain while the file is read.
 */
extern void i18n_init(void);

/** Load the core catalogs from PO_PATH, after the configuration is read.
 * @return Number of files refused plus entries rejected; ircd -k treats
 *   a non-zero value as an error.
 */
extern unsigned int i18n_load(void);

/** Reload every domain, core and modules, on /REHASH.  Problems are
 * reported the way configuration errors are: to the opers and the log.
 * @return As i18n_load().
 */
extern unsigned int i18n_rehash(void);

/** Free everything; main() only, at exit. */
extern void i18n_close(void);

/** Recompute the lookup chains and the capability value after the set of
 * catalogs or FEAT_DEFAULT_LANGUAGE changed.  Domain open and close and
 * the feature's notify call this; a reload does it once at the end.
 */
extern void i18n_resolve(void);

/** The draft/languages capability value: "<max>,<default>,<code>,...". */
extern const char* i18n_cap_value(void);

/** /STATS languages: every domain, each language, entries and rejects. */
extern void i18n_stats(struct Client* sptr, const struct StatDesc* sd,
                       char* param);

/*
 * The validation, exposed for the unit test.
 */

/** Check a translation against its original, rules one and two above.
 * @param[in] msgid Original format.
 * @param[in] msgstr Translation.
 * @param[out] why Receives the reason on failure; may be NULL.
 * @param[in] whylen Room in \a why.
 * @return Zero if \a msgstr is acceptable, non-zero otherwise.
 */
extern int i18n_check_format(const char* msgid, const char* msgstr,
                             char* why, size_t whylen);

#endif /* INCLUDED_ircd_i18n_h */
