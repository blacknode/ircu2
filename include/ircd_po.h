#ifndef INCLUDED_ircd_po_h
#define INCLUDED_ircd_po_h
/*
 * IRC - Internet Relay Chat, include/ircd_po.h
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
 * @brief A reader for GNU PO files, and an evaluator for Plural-Forms.
 *
 * The subset of PO every tool produces: @c msgctxt, @c msgid,
 * @c msgid_plural, @c msgstr and @c msgstr[N], C escapes in the strings,
 * continuation lines, the @c #, @c #., @c #:, @c #| and @c #, comments
 * and the obsolete @c #~ entries.  The header entry is parsed for its
 * charset and its Plural-Forms.  Nothing here knows about clients or
 * formats: ircd_i18n.c validates what this hands back.
 */

#ifndef INCLUDED_ircd_i18n_h
#include "ircd_i18n.h"  /* I18N_NPLURALS_MAX */
#endif

/** One entry of a PO file, as written. */
struct PoEntry {
  char*        pe_ctx;                       /**< msgctxt, or NULL. */
  char*        pe_id;                        /**< msgid. */
  char*        pe_plural;                    /**< msgid_plural, or NULL. */
  char*        pe_str[I18N_NPLURALS_MAX];    /**< msgstr, or msgstr[N]. */
  unsigned int pe_nstr;                      /**< How many of pe_str. */
  unsigned int pe_fuzzy : 1;                 /**< Flagged fuzzy. */
  unsigned int pe_line;                      /**< Line the entry starts on. */
};

/** A compiled Plural-Forms expression.  Opaque. */
struct PoPlural;

/** A parsed PO file. */
struct PoFile {
  struct PoEntry*  pf_entries;   /**< Every entry but the header. */
  unsigned int     pf_count;     /**< Number of entries. */
  unsigned int     pf_nplurals;  /**< From the header; 0 if it says nothing. */
  struct PoPlural* pf_plural;    /**< The plural rule, or NULL. */
  char*            pf_charset;   /**< From the header, or NULL. */
};

/** Parse the text of a PO file.
 * @param[in] text The whole file, NUL-terminated.
 * @param[out] err Receives a reason on failure, with the line number.
 * @param[in] errlen Room in \a err.
 * @return The file, or NULL if it does not parse.  A file that parses
 *   may still have entries the caller refuses.
 */
extern struct PoFile* po_parse(const char* text, char* err, size_t errlen);

/** Read and parse a PO file from disk.  As po_parse(), plus I/O errors. */
extern struct PoFile* po_parse_file(const char* path, char* err,
                                    size_t errlen);

extern void po_free(struct PoFile* pf);

/** Compile a Plural-Forms header value, "nplurals=2; plural=(n != 1);".
 * @param[in] rule The value.
 * @param[out] nplurals Receives nplurals.
 * @param[out] err Receives a reason on failure; may be NULL.
 * @param[in] errlen Room in \a err.
 * @return The compiled rule, or NULL.
 */
extern struct PoPlural* po_plural_compile(const char* rule,
                                          unsigned int* nplurals,
                                          char* err, size_t errlen);

/** Which form \a n takes under \a rule: 0 .. nplurals-1. */
extern unsigned int po_plural_eval(const struct PoPlural* rule,
                                   unsigned long n);

extern void po_plural_free(struct PoPlural* rule);

/** Offset of the first byte that is not valid UTF-8, or -1 if all is. */
extern long po_utf8_check(const char* text, size_t len);

#endif /* INCLUDED_ircd_po_h */
