/*
 * IRC - Internet Relay Chat, ircd/ircd_i18n.c
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
 * @brief Domains, catalogs, client preferences and the lookup chain.
 *
 * Three tables hold everything:
 *
 *   - the @em code table: every language code that has a catalog in some
 *     domain, numbered from 1.  It is rebuilt by i18n_resolve() whenever
 *     the set of catalogs changes, and it is what makes a lookup cheap: a
 *     domain keeps an array indexed by code number, and a preference is a
 *     list of code numbers;
 *   - the @em preference table: every distinct preference a client has
 *     asked for, interned, so a client carries a 16-bit index rather than
 *     three strings.  A preference remembers its codes as written -- that
 *     is what LG and WHOIS show -- and the chain of code numbers they
 *     resolve to, exact code first and primary subtag second;
 *   - the @em default chain, from FEAT_DEFAULT_LANGUAGE, tried after the
 *     client's own.
 *
 * A lookup is then: for each code number in the chain, the domain's
 * catalog for it, and in that catalog a hash probe.  A client with no
 * preference on a server with no default language costs one comparison.
 *
 * Every catalog is validated as it is loaded -- see ircd_i18n.h for the
 * rules -- and a file that will not parse leaves the previous catalog in
 * place, the way a rehash with a broken block keeps the old one.
 */
#include "config.h"

#include "ircd_i18n.h"
#include "ircd_po.h"

#include "capab.h"
#include "client.h"
#include "ircd_alloc.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "numeric.h"
#include "s_stats.h"
#include "send.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ---------------------------------------------------------------------- */
/* Types                                                                   */
/* ---------------------------------------------------------------------- */

/** One translated entry. */
struct I18nEntry {
  struct I18nEntry* ie_next;                /**< Hash chain. */
  unsigned int      ie_hash;
  char*             ie_ctx;                 /**< msgctxt, or NULL. */
  char*             ie_id;                  /**< msgid. */
  char*             ie_str[I18N_NPLURALS_MAX]; /**< The forms. */
  unsigned int      ie_nstr;
  unsigned int      ie_plural : 1;          /**< Had a msgid_plural. */
};

/** One language of one domain: a loaded .po file. */
struct I18nCatalog {
  struct I18nCatalog* ic_next;
  char                ic_lang[I18N_LANG_MAX];
  struct I18nEntry**  ic_buckets;
  unsigned int        ic_nbuckets;
  unsigned int        ic_count;        /**< Entries loaded. */
  unsigned int        ic_rejected;     /**< Entries refused at load. */
  struct PoPlural*    ic_plural;       /**< Plural-Forms, or NULL. */
  unsigned int        ic_nplurals;
  unsigned int        ic_seen : 1;     /**< Scratch for a reload. */
};

struct I18nDomain {
  struct I18nDomain*  id_next;
  char*               id_name;
  char*               id_dir;
  struct I18nCatalog* id_catalogs;
  unsigned int        id_refused;      /**< Files refused at last load. */
  /** Catalog for each code number, or NULL.  Rebuilt by i18n_resolve(). */
  struct I18nCatalog* id_by_code[I18N_CODE_TABLE_MAX];
};

/** A chain entry that means "the source language: stop here". */
#define I18N_CODE_SOURCE  (I18N_CODE_TABLE_MAX - 1)
/** Longest chain: two per preferred code. */
#define I18N_CHAIN_MAX    (I18N_PREF_MAX * 2)

/** One interned preference. */
struct I18nPref {
  struct I18nPref* ip_hnext;
  unsigned int     ip_hash;
  unsigned int     ip_index;       /**< Its slot in pref_table[]. */
  unsigned int     ip_count;
  char             ip_codes[I18N_PREF_MAX][I18N_LANG_MAX];
  unsigned char    ip_chain[I18N_CHAIN_MAX];
  unsigned int     ip_chain_len;
};

/* ---------------------------------------------------------------------- */
/* State                                                                   */
/* ---------------------------------------------------------------------- */

struct I18nDomain* i18n_core;

/** Every open domain, core first. */
static struct I18nDomain* domains;

/** The code table; slot 0 is "no code" and the last slot is the source
 * language sentinel, so codes live in 1 .. I18N_CODE_TABLE_MAX-2. */
static char code_table[I18N_CODE_TABLE_MAX][I18N_LANG_MAX];
static unsigned int code_count = 1;

/** The preference table, by index; slot 0 is "no preference". */
static struct I18nPref* pref_table[I18N_PREF_TABLE_MAX];
static unsigned int pref_count = 1;
#define PREF_BUCKETS 1024
static struct I18nPref* pref_buckets[PREF_BUCKETS];
static int pref_table_full_reported;

/** FEAT_DEFAULT_LANGUAGE resolved. */
static unsigned char default_chain[2];
static unsigned int default_chain_len;

/** The capability value, "3,es,en". */
static char cap_value[256];

/** Set once the first load has happened: after that, problems go to the
 * opers and the log only, not to stderr. */
static int i18n_started;

/* ---------------------------------------------------------------------- */
/* Small helpers                                                           */
/* ---------------------------------------------------------------------- */

static unsigned int hash_bytes(unsigned int h, const char* s)
{
  /* FNV-1a. */
  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= 16777619u;
  }
  return h;
}

static unsigned int entry_hash(const char* ctx, const char* msgid)
{
  unsigned int h = 2166136261u;

  if (ctx) {
    h = hash_bytes(h, ctx);
    h ^= 0x04;
    h *= 16777619u;
  }
  return hash_bytes(h, msgid);
}

static void lower_copy(char* dst, const char* src, size_t len)
{
  size_t i;

  for (i = 0; i + 1 < len && src[i]; i++)
    dst[i] = (src[i] >= 'A' && src[i] <= 'Z') ? src[i] + ('a' - 'A') : src[i];
  dst[i] = '\0';
}

/** Report a problem with a catalog the three ways yyerror() does: to the
 * opers, to the log and -- until the server is up -- to stderr, so a
 * broken file is visible however the server was started.
 */
static void i18n_report(const char* fmt, ...)
{
  char msg[512];
  va_list vl;

  va_start(vl, fmt);
  ircd_vsnprintf(0, msg, sizeof(msg), fmt, vl);
  va_end(vl);

  sendto_opmask_butone(0, SNO_ALL, "%s", msg);
  log_write(LS_CONFIG, L_ERROR, 0, "%s", msg);
  if (!i18n_started)
    fprintf(stderr, "%s\n", msg);
}

/* ---------------------------------------------------------------------- */
/* Language codes                                                          */
/* ---------------------------------------------------------------------- */

static int is_alpha(int c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_alnum(int c)
{
  return is_alpha(c) || (c >= '0' && c <= '9');
}

int i18n_valid_code(const char* code)
{
  const char* s = code;
  unsigned int n;

  if (!code || strlen(code) >= I18N_LANG_MAX)
    return 0;

  /* [A-Za-z]{2,3} */
  for (n = 0; is_alpha(*s); s++)
    n++;
  if (n < 2 || n > 3)
    return 0;

  /* (-[A-Za-z0-9]{1,8})* */
  while (*s == '-') {
    s++;
    for (n = 0; is_alnum(*s); s++)
      n++;
    if (n < 1 || n > 8)
      return 0;
  }

  return *s == '\0';
}

/** The primary subtag of \a code, in \a out.  "es-ar" -> "es". */
static void primary_of(char* out, const char* code)
{
  size_t i;

  for (i = 0; i + 1 < I18N_LANG_MAX && code[i] && code[i] != '-'; i++)
    out[i] = code[i];
  out[i] = '\0';
}

static int is_source_lang(const char* code)
{
  return 0 == strcmp(code, I18N_SOURCE_LANG);
}

/** Number of \a code in the code table, or 0. */
static unsigned int code_find(const char* code)
{
  unsigned int i;

  for (i = 1; i < code_count; i++)
    if (0 == strcmp(code_table[i], code))
      return i;
  return 0;
}

/** Number of \a code, adding it if room allows; 0 if the table is full. */
static unsigned int code_intern(const char* code)
{
  unsigned int i = code_find(code);

  if (i)
    return i;
  if (code_count >= I18N_CODE_SOURCE)
    return 0;
  ircd_strncpy(code_table[code_count], code, I18N_LANG_MAX - 1);
  code_table[code_count][I18N_LANG_MAX - 1] = '\0';
  return code_count++;
}

/** Append the chain \a code resolves to: itself, then its primary
 * subtag, then the stop sentinel if either is the source language.
 * @return Non-zero if the chain reached the source language.
 */
static int chain_add(unsigned char* chain, unsigned int* len, const char* code)
{
  char primary[I18N_LANG_MAX];
  unsigned int id;
  int stop = 0;

  if ((id = code_find(code)) && *len < I18N_CHAIN_MAX)
    chain[(*len)++] = (unsigned char)id;
  if (is_source_lang(code))
    stop = 1;

  primary_of(primary, code);
  if (strcmp(primary, code)) {
    if ((id = code_find(primary)) && *len < I18N_CHAIN_MAX)
      chain[(*len)++] = (unsigned char)id;
    if (is_source_lang(primary))
      stop = 1;
  }

  if (stop && *len < I18N_CHAIN_MAX)
    chain[(*len)++] = I18N_CODE_SOURCE;
  return stop;
}

int i18n_language_known(const char* code)
{
  char lower[I18N_LANG_MAX];
  char primary[I18N_LANG_MAX];

  if (!i18n_valid_code(code))
    return 0;
  lower_copy(lower, code, sizeof(lower));
  primary_of(primary, lower);

  return is_source_lang(lower) || is_source_lang(primary)
    || code_find(lower) || code_find(primary);
}

/* ---------------------------------------------------------------------- */
/* Preferences                                                             */
/* ---------------------------------------------------------------------- */

static unsigned int pref_hash(const char* const* codes, unsigned int count)
{
  unsigned int h = 2166136261u;
  unsigned int i;

  for (i = 0; i < count; i++) {
    h = hash_bytes(h, codes[i]);
    h ^= 0x20;
    h *= 16777619u;
  }
  return h;
}

static int pref_equal(const struct I18nPref* p, const char* const* codes,
                      unsigned int count)
{
  unsigned int i;

  if (p->ip_count != count)
    return 0;
  for (i = 0; i < count; i++)
    if (strcmp(p->ip_codes[i], codes[i]))
      return 0;
  return 1;
}

static void pref_resolve(struct I18nPref* p)
{
  unsigned int i;

  p->ip_chain_len = 0;
  for (i = 0; i < p->ip_count; i++)
    if (chain_add(p->ip_chain, &p->ip_chain_len, p->ip_codes[i]))
      break;
}

/** Find or add the preference \a codes; 0 if it cannot be interned. */
static unsigned int pref_intern(const char* const* codes, unsigned int count)
{
  unsigned int h = pref_hash(codes, count);
  struct I18nPref* p;
  unsigned int i;

  for (p = pref_buckets[h % PREF_BUCKETS]; p; p = p->ip_hnext)
    if (p->ip_hash == h && pref_equal(p, codes, count))
      break;
  if (p)
    return p->ip_index;

  if (pref_count >= I18N_PREF_TABLE_MAX)
    return 0;

  p = (struct I18nPref*)MyCalloc(1, sizeof(struct I18nPref));
  p->ip_hash = h;
  p->ip_count = count;
  for (i = 0; i < count; i++) {
    ircd_strncpy(p->ip_codes[i], codes[i], I18N_LANG_MAX - 1);
    p->ip_codes[i][I18N_LANG_MAX - 1] = '\0';
  }
  pref_resolve(p);
  p->ip_hnext = pref_buckets[h % PREF_BUCKETS];
  pref_buckets[h % PREF_BUCKETS] = p;
  p->ip_index = pref_count;
  pref_table[pref_count] = p;
  return pref_count++;
}

unsigned int i18n_set_languages(struct Client* cptr, const char* const* codes,
                                unsigned int count)
{
  char lower[I18N_PREF_MAX][I18N_LANG_MAX];
  const char* list[I18N_PREF_MAX];
  unsigned int n = 0, i, idx;

  assert(0 != cptr);

  for (i = 0; i < count && n < I18N_PREF_MAX; i++) {
    unsigned int k;

    if (!i18n_valid_code(codes[i]))
      continue;
    lower_copy(lower[n], codes[i], I18N_LANG_MAX);
    /* The same code twice adds nothing. */
    for (k = 0; k < n; k++)
      if (0 == strcmp(lower[k], lower[n]))
        break;
    if (k < n)
      continue;
    list[n] = lower[n];
    n++;
  }

  if (!n) {
    cli_lang(cptr) = 0;
    return 0;
  }

  idx = pref_intern(list, n);
  if (!idx && n > 1) {
    /* The table is full: the first code alone may already be there. */
    idx = pref_intern(list, 1);
    if (idx)
      n = 1;
  }
  if (!idx) {
    if (!pref_table_full_reported) {
      pref_table_full_reported = 1;
      log_write(LS_SYSTEM, L_WARNING, 0, "Language preference table full "
                "(%d entries); new preferences are being dropped",
                I18N_PREF_TABLE_MAX);
    }
    cli_lang(cptr) = 0;
    return 0;
  }

  cli_lang(cptr) = (unsigned short)idx;
  return n;
}

const char* i18n_languages_str(const struct Client* cptr)
{
  static char buf[I18N_PREF_MAX * I18N_LANG_MAX];
  const struct I18nPref* p;
  unsigned int i;
  size_t len = 0;

  if (!cptr || !cli_lang(cptr) || cli_lang(cptr) >= pref_count)
    return 0;
  p = pref_table[cli_lang(cptr)];

  buf[0] = '\0';
  for (i = 0; i < p->ip_count; i++) {
    if (i)
      buf[len++] = ' ';
    strcpy(buf + len, p->ip_codes[i]);
    len += strlen(p->ip_codes[i]);
  }
  return buf;
}

/* ---------------------------------------------------------------------- */
/* Format validation                                                       */
/* ---------------------------------------------------------------------- */

/** Everything ircd_snprintf() accepts between the '%' and the conversion:
 * flags, width, precision and type modifiers. */
static const char directive_middle[] = "-+ #:0123456789.*hlqLjtzZT";
/** The conversions it knows. */
static const char directive_end[] = "sdiXoxucpnmvCH";

/** Find the directive after \a *s, if any.
 * @param[in,out] s Advanced past the directive found, or to the end.
 * @param[out] start Where the directive begins, its '%' included.
 * @param[out] len Its length.
 * @return 1 if a directive was found, 0 at the end of the string, -1 if
 *   a '%' begins something that is not a directive.
 */
static int next_directive(const char** s, const char** start, size_t* len)
{
  const char* p = *s;

  for (;;) {
    p = strchr(p, '%');
    if (!p) {
      *s = p ? p : *s + strlen(*s);
      return 0;
    }
    if (p[1] == '%') {
      p += 2;
      continue;
    }
    break;
  }

  *start = p;
  p++;
  while (*p && strchr(directive_middle, *p))
    p++;
  if (!*p || !strchr(directive_end, *p)) {
    /* Show the byte that broke it, when there is one. */
    *len = p - *start + (*p ? 1 : 0);
    *s = p;
    return -1;
  }
  p++;
  *len = p - *start;
  *s = p;
  return 1;
}

int i18n_check_format(const char* msgid, const char* msgstr, char* why,
                      size_t whylen)
{
  const char* a = msgid;
  const char* b = msgstr;
  unsigned int n = 0;

  if (why && whylen)
    *why = '\0';

  if (strchr(msgstr, '\n') || strchr(msgstr, '\r')) {
    if (why && whylen)
      snprintf(why, whylen, "a line break in the translation");
    return 1;
  }

  for (;;) {
    const char *da = 0, *db = 0;
    size_t la = 0, lb = 0;
    int ra = next_directive(&a, &da, &la);
    int rb = next_directive(&b, &db, &lb);

    if (ra < 0) {
      if (why && whylen)
        snprintf(why, whylen, "malformed directive \"%.*s\" in the original",
                 (int)la, da);
      return 1;
    }
    if (rb < 0) {
      if (why && whylen)
        snprintf(why, whylen, "malformed directive \"%.*s\"", (int)lb, db);
      return 1;
    }
    if (!ra && !rb)
      return 0;
    n++;
    if (!ra) {
      if (why && whylen)
        snprintf(why, whylen, "directive %u, \"%.*s\", is not in the original",
                 n, (int)lb, db);
      return 1;
    }
    if (!rb) {
      if (why && whylen)
        snprintf(why, whylen, "directive %u, \"%.*s\", is missing", n,
                 (int)la, da);
      return 1;
    }
    if (la != lb || memcmp(da, db, la)) {
      if (why && whylen)
        snprintf(why, whylen, "directive %u is \"%.*s\", the original has "
                 "\"%.*s\"", n, (int)lb, db, (int)la, da);
      return 1;
    }
  }
}

/* ---------------------------------------------------------------------- */
/* Catalogs                                                                */
/* ---------------------------------------------------------------------- */

static void entry_free(struct I18nEntry* e)
{
  unsigned int i;

  MyFree(e->ie_ctx);
  MyFree(e->ie_id);
  for (i = 0; i < e->ie_nstr; i++)
    MyFree(e->ie_str[i]);
  MyFree(e);
}

static void catalog_free(struct I18nCatalog* cat)
{
  unsigned int i;

  if (!cat)
    return;
  for (i = 0; i < cat->ic_nbuckets; i++) {
    struct I18nEntry* e;
    struct I18nEntry* next;

    for (e = cat->ic_buckets[i]; e; e = next) {
      next = e->ie_next;
      entry_free(e);
    }
  }
  MyFree(cat->ic_buckets);
  po_plural_free(cat->ic_plural);
  MyFree(cat);
}

static struct I18nEntry* catalog_find(const struct I18nCatalog* cat,
                                      unsigned int h, const char* ctx,
                                      const char* msgid)
{
  struct I18nEntry* e;

  for (e = cat->ic_buckets[h & (cat->ic_nbuckets - 1)]; e; e = e->ie_next) {
    if (e->ie_hash != h)
      continue;
    if ((ctx == 0) != (e->ie_ctx == 0))
      continue;
    if (ctx && strcmp(ctx, e->ie_ctx))
      continue;
    if (strcmp(msgid, e->ie_id))
      continue;
    return e;
  }
  return 0;
}

/** Refuse one entry, saying why. */
static void reject(struct I18nCatalog* cat, const char* path,
                   const struct PoEntry* pe, const char* why)
{
  cat->ic_rejected++;
  i18n_report("%s:%u: entry \"%.40s%s\" rejected: %s", path, pe->pe_line,
              pe->pe_id, strlen(pe->pe_id) > 40 ? "..." : "", why);
}

/** Validate one entry and, if it passes, add it to \a cat. */
static void catalog_add(struct I18nCatalog* cat, const char* path,
                        const struct PoEntry* pe)
{
  struct I18nEntry* e;
  unsigned int h, i, nonempty = 0;
  char why[256];

  if (pe->pe_fuzzy)
    return; /* what msgfmt does: a fuzzy entry is not a translation */

  for (i = 0; i < pe->pe_nstr; i++)
    if (pe->pe_str[i][0])
      nonempty++;
  if (!nonempty)
    return; /* untranslated */

  if (pe->pe_plural) {
    if (!cat->ic_plural) {
      reject(cat, path, pe, "a plural entry in a catalog with no "
             "Plural-Forms header");
      return;
    }
    if (pe->pe_nstr != cat->ic_nplurals) {
      snprintf(why, sizeof(why), "%u plural forms, the header declares %u",
               pe->pe_nstr, cat->ic_nplurals);
      reject(cat, path, pe, why);
      return;
    }
    if (nonempty != pe->pe_nstr) {
      reject(cat, path, pe, "an empty plural form");
      return;
    }
    for (i = 0; i < pe->pe_nstr; i++) {
      if (i18n_check_format(pe->pe_plural, pe->pe_str[i], why, sizeof(why))) {
        char full[320];

        snprintf(full, sizeof(full), "msgstr[%u]: %s", i, why);
        reject(cat, path, pe, full);
        return;
      }
    }
  } else {
    if (pe->pe_nstr != 1) {
      reject(cat, path, pe, "msgstr[N] without msgid_plural");
      return;
    }
    if (i18n_check_format(pe->pe_id, pe->pe_str[0], why, sizeof(why))) {
      reject(cat, path, pe, why);
      return;
    }
  }

  h = entry_hash(pe->pe_ctx, pe->pe_id);
  if (catalog_find(cat, h, pe->pe_ctx, pe->pe_id)) {
    reject(cat, path, pe, "a duplicate of an earlier entry");
    return;
  }

  e = (struct I18nEntry*)MyCalloc(1, sizeof(struct I18nEntry));
  e->ie_hash = h;
  if (pe->pe_ctx)
    DupString(e->ie_ctx, pe->pe_ctx);
  DupString(e->ie_id, pe->pe_id);
  e->ie_nstr = pe->pe_nstr;
  for (i = 0; i < pe->pe_nstr; i++)
    DupString(e->ie_str[i], pe->pe_str[i]);
  e->ie_plural = pe->pe_plural ? 1 : 0;

  e->ie_next = cat->ic_buckets[h & (cat->ic_nbuckets - 1)];
  cat->ic_buckets[h & (cat->ic_nbuckets - 1)] = e;
  cat->ic_count++;
}

/** Build a catalog from a parsed file.  Never fails: what does not pass
 * is left out, and the count of that is in ic_rejected. */
static struct I18nCatalog* catalog_build(const char* lang, const char* path,
                                         struct PoFile* pf)
{
  struct I18nCatalog* cat;
  unsigned int i, want;

  cat = (struct I18nCatalog*)MyCalloc(1, sizeof(struct I18nCatalog));
  ircd_strncpy(cat->ic_lang, lang, I18N_LANG_MAX - 1);
  cat->ic_lang[I18N_LANG_MAX - 1] = '\0';

  /* Power of two, about one bucket per entry. */
  for (want = 16; want < pf->pf_count; want *= 2)
    ;
  cat->ic_nbuckets = want;
  cat->ic_buckets = (struct I18nEntry**)
    MyCalloc(want, sizeof(struct I18nEntry*));

  /* Take the plural rule over; the PoFile is freed by the caller. */
  cat->ic_plural = pf->pf_plural;
  cat->ic_nplurals = pf->pf_nplurals;
  pf->pf_plural = 0;

  for (i = 0; i < pf->pf_count; i++)
    catalog_add(cat, path, &pf->pf_entries[i]);

  return cat;
}

/** Non-zero if the header's charset is UTF-8, or absent. */
static int charset_ok(const struct PoFile* pf)
{
  const char* cs = pf->pf_charset;

  if (!cs || !*cs)
    return 1;
  return !strcasecmp(cs, "UTF-8") || !strcasecmp(cs, "UTF8");
}

/* ---------------------------------------------------------------------- */
/* Domains                                                                 */
/* ---------------------------------------------------------------------- */

static struct I18nCatalog** domain_slot(struct I18nDomain* dom,
                                        const char* lang)
{
  struct I18nCatalog** pp;

  for (pp = &dom->id_catalogs; *pp; pp = &(*pp)->ic_next)
    if (0 == strcmp((*pp)->ic_lang, lang))
      break;
  return pp;
}

/** Keep the list sorted by code, so /STATS and the capability value come
 * out in a stable order. */
static void domain_insert(struct I18nDomain* dom, struct I18nCatalog* cat)
{
  struct I18nCatalog** pp;

  for (pp = &dom->id_catalogs; *pp; pp = &(*pp)->ic_next)
    if (strcmp((*pp)->ic_lang, cat->ic_lang) > 0)
      break;
  cat->ic_next = *pp;
  *pp = cat;
}

/** Load one file into \a dom, replacing the catalog of its language.
 * @return Number of problems: 1 if the file was refused, else the number
 *   of entries rejected.
 */
static unsigned int domain_load_file(struct I18nDomain* dom,
                                     const char* lang, const char* path)
{
  struct PoFile* pf;
  struct I18nCatalog* cat;
  struct I18nCatalog** slot;
  char err[256];

  slot = domain_slot(dom, lang);

  pf = po_parse_file(path, err, sizeof(err));
  if (pf && !charset_ok(pf)) {
    snprintf(err, sizeof(err), "charset is %s, not UTF-8", pf->pf_charset);
    po_free(pf);
    pf = 0;
  }
  if (!pf) {
    /* The previous catalog, if any, stays -- and stays seen, so the sweep
     * after the directory is read does not drop it. */
    i18n_report("%s: refused: %s%s", path, err,
                *slot ? "; keeping the previous catalog" : "");
    if (*slot)
      (*slot)->ic_seen = 1;
    dom->id_refused++;
    return 1;
  }

  cat = catalog_build(lang, path, pf);
  po_free(pf);

  if (*slot) {
    struct I18nCatalog* old = *slot;

    *slot = old->ic_next;
    catalog_free(old);
  }
  cat->ic_seen = 1;
  domain_insert(dom, cat);

  log_write(LS_SYSTEM, L_INFO, 0, "Loaded %s translations for %s: %u "
            "entries, %u rejected", lang, dom->id_name, cat->ic_count,
            cat->ic_rejected);

  return cat->ic_rejected;
}

static int name_cmp(const void* a, const void* b)
{
  return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/** Read \a dom's directory again.  Does not resolve. */
static unsigned int domain_load(struct I18nDomain* dom)
{
  DIR* d;
  struct dirent* de;
  struct I18nCatalog* cat;
  struct I18nCatalog** pp;
  char* names[I18N_CODE_TABLE_MAX];
  unsigned int nnames = 0, i, problems = 0;

  for (cat = dom->id_catalogs; cat; cat = cat->ic_next)
    cat->ic_seen = 0;
  dom->id_refused = 0;

  d = opendir(dom->id_dir);
  if (!d) {
    /* No directory is no catalogs, which is fine: not every install
     * has translations.  Anything else is worth a line.
     */
    if (errno != ENOENT)
      i18n_report("%s: cannot read: %s", dom->id_dir, strerror(errno));
  } else {
    while ((de = readdir(d))) {
      size_t len = strlen(de->d_name);

      if (de->d_name[0] == '.' || len < 4
          || strcmp(de->d_name + len - 3, ".po"))
        continue;
      if (nnames >= I18N_CODE_TABLE_MAX) {
        i18n_report("%s: too many catalogs; ignoring %s and the rest",
                    dom->id_dir, de->d_name);
        problems++;
        break;
      }
      DupString(names[nnames], de->d_name);
      nnames++;
    }
    closedir(d);
  }

  /* In a fixed order, so the log reads the same every time. */
  qsort(names, nnames, sizeof(names[0]), name_cmp);

  for (i = 0; i < nnames; i++) {
    char lang[I18N_LANG_MAX];
    char path[1024];
    size_t len = strlen(names[i]) - 3;

    /* The code is the name without ".po"; too long is not a code. */
    lower_copy(lang, names[i], len + 1 < sizeof(lang) ? len + 1 : sizeof(lang));
    if (len >= I18N_LANG_MAX || !i18n_valid_code(lang)) {
      i18n_report("%s/%s: ignored: \"%.*s\" is not a language code",
                  dom->id_dir, names[i], (int)len, names[i]);
      problems++;
    } else {
      snprintf(path, sizeof(path), "%s/%s", dom->id_dir, names[i]);
      problems += domain_load_file(dom, lang, path);
    }
    MyFree(names[i]);
  }

  /* A language whose file is gone goes with it. */
  for (pp = &dom->id_catalogs; (cat = *pp);) {
    if (cat->ic_seen) {
      pp = &cat->ic_next;
      continue;
    }
    *pp = cat->ic_next;
    log_write(LS_SYSTEM, L_INFO, 0, "Dropped %s translations for %s: file "
              "removed", cat->ic_lang, dom->id_name);
    catalog_free(cat);
  }

  return problems;
}

struct I18nDomain* i18n_domain_open(const char* name, const char* dir)
{
  struct I18nDomain* dom;
  struct I18nDomain** pp;

  assert(0 != name);
  assert(0 != dir);

  dom = (struct I18nDomain*)MyCalloc(1, sizeof(struct I18nDomain));
  DupString(dom->id_name, name);
  DupString(dom->id_dir, dir);

  /* At the tail: the core, opened first, stays first. */
  for (pp = &domains; *pp; pp = &(*pp)->id_next)
    ;
  *pp = dom;

  domain_load(dom);
  i18n_resolve();
  return dom;
}

void i18n_domain_close(struct I18nDomain* dom)
{
  struct I18nDomain** pp;
  struct I18nCatalog* cat;
  struct I18nCatalog* next;

  if (!dom)
    return;

  for (pp = &domains; *pp; pp = &(*pp)->id_next) {
    if (*pp == dom) {
      *pp = dom->id_next;
      break;
    }
  }
  if (dom == i18n_core)
    i18n_core = 0;

  for (cat = dom->id_catalogs; cat; cat = next) {
    next = cat->ic_next;
    catalog_free(cat);
  }
  MyFree(dom->id_name);
  MyFree(dom->id_dir);
  MyFree(dom);

  i18n_resolve();
}

unsigned int i18n_domain_reload(struct I18nDomain* dom)
{
  unsigned int problems;

  assert(0 != dom);
  problems = domain_load(dom);
  i18n_resolve();
  return problems;
}

const char* i18n_domain_name(const struct I18nDomain* dom)
{
  return dom ? dom->id_name : "";
}

int i18n_domain_has(const struct I18nDomain* dom, const char* code)
{
  char lower[I18N_LANG_MAX];

  if (!dom || !code)
    return 0;
  lower_copy(lower, code, sizeof(lower));
  return *domain_slot((struct I18nDomain*)dom, lower) != 0;
}

/* ---------------------------------------------------------------------- */
/* Resolution                                                              */
/* ---------------------------------------------------------------------- */

static void cap_value_build(void)
{
  const char* codes[I18N_CODE_TABLE_MAX];
  const char* def = 0;
  char lower[I18N_LANG_MAX];
  unsigned int n = 0, i, k;
  size_t len;
  int have_source = 0;

  /* Every code with a catalog, plus the source language, once each. */
  for (i = 1; i < code_count; i++) {
    codes[n++] = code_table[i];
    if (is_source_lang(code_table[i]))
      have_source = 1;
  }
  if (!have_source)
    codes[n++] = I18N_SOURCE_LANG;
  qsort(codes, n, sizeof(codes[0]), name_cmp);

  /* The server's own language first, if it is one of them. */
  lower_copy(lower, feature_str(FEAT_DEFAULT_LANGUAGE)
             ? feature_str(FEAT_DEFAULT_LANGUAGE) : "", sizeof(lower));
  for (i = 0; i < n; i++)
    if (0 == strcmp(codes[i], lower))
      def = codes[i];

  len = (size_t)snprintf(cap_value, sizeof(cap_value), "%d", I18N_PREF_MAX);
  if (def) {
    len += (size_t)snprintf(cap_value + len, sizeof(cap_value) - len, ",%s",
                            def);
  }
  for (i = 0; i < n; i++) {
    if (codes[i] == def)
      continue;
    k = (unsigned int)snprintf(0, 0, ",%s", codes[i]);
    if (len + k >= sizeof(cap_value))
      break; /* the value has a limit; the rest are still loadable */
    len += (size_t)snprintf(cap_value + len, sizeof(cap_value) - len, ",%s",
                            codes[i]);
  }

  cap_set_value(E_CAP_LANGUAGES, cap_value);
}

void i18n_resolve(void)
{
  struct I18nDomain* dom;
  struct I18nCatalog* cat;
  const char* def;
  unsigned int i;

  /* The code table, from every catalog that exists right now. */
  code_count = 1;
  for (dom = domains; dom; dom = dom->id_next)
    for (cat = dom->id_catalogs; cat; cat = cat->ic_next)
      if (!code_intern(cat->ic_lang))
        i18n_report("%s: too many languages loaded; %s cannot be reached",
                    dom->id_name, cat->ic_lang);

  /* Each domain's catalog by code number. */
  for (dom = domains; dom; dom = dom->id_next) {
    memset(dom->id_by_code, 0, sizeof(dom->id_by_code));
    for (cat = dom->id_catalogs; cat; cat = cat->ic_next) {
      unsigned int id = code_find(cat->ic_lang);

      if (id)
        dom->id_by_code[id] = cat;
    }
  }

  /* Every preference, and the default. */
  for (i = 1; i < pref_count; i++)
    pref_resolve(pref_table[i]);

  default_chain_len = 0;
  def = feature_str(FEAT_DEFAULT_LANGUAGE);
  if (def && *def) {
    char lower[I18N_LANG_MAX];
    unsigned char chain[I18N_CHAIN_MAX];
    unsigned int len = 0;

    lower_copy(lower, def, sizeof(lower));
    if (i18n_valid_code(lower)) {
      chain_add(chain, &len, lower);
      for (i = 0; i < len && i < 2; i++)
        default_chain[i] = chain[i];
      default_chain_len = len < 2 ? len : 2;
    }
  }

  cap_value_build();
}

const char* i18n_cap_value(void)
{
  return cap_value;
}

/* ---------------------------------------------------------------------- */
/* Lookup                                                                  */
/* ---------------------------------------------------------------------- */

/** Find the entry for \a ctx / \a msgid in \a dom for \a to, walking the
 * client's chain and then the default one.
 * @param[out] found_in Receives the catalog the entry was found in.
 */
static const struct I18nEntry* lookup(const struct I18nDomain* dom,
                                      const struct Client* to,
                                      const char* ctx, const char* msgid,
                                      const struct I18nCatalog** found_in)
{
  const struct I18nEntry* e;
  const unsigned char* chain;
  unsigned int len, i, h;

  if (!dom || !to || !msgid)
    return 0;
  if (!cli_lang(to) && !default_chain_len)
    return 0;
  /* A server, or this server, reads English. */
  if (cli_serv(to))
    return 0;

  h = entry_hash(ctx, msgid);

  if (cli_lang(to) && cli_lang(to) < pref_count) {
    chain = pref_table[cli_lang(to)]->ip_chain;
    len = pref_table[cli_lang(to)]->ip_chain_len;
    for (i = 0; i < len; i++) {
      const struct I18nCatalog* cat;

      if (chain[i] == I18N_CODE_SOURCE)
        return 0;
      cat = dom->id_by_code[chain[i]];
      if (cat && (e = catalog_find(cat, h, ctx, msgid))) {
        *found_in = cat;
        return e;
      }
    }
  }

  for (i = 0; i < default_chain_len; i++) {
    const struct I18nCatalog* cat;

    if (default_chain[i] == I18N_CODE_SOURCE)
      return 0;
    cat = dom->id_by_code[default_chain[i]];
    if (cat && (e = catalog_find(cat, h, ctx, msgid))) {
      *found_in = cat;
      return e;
    }
  }

  return 0;
}

const char* i18n_ctext(const struct I18nDomain* dom, const struct Client* to,
                       const char* ctx, const char* msgid)
{
  const struct I18nCatalog* cat;
  const struct I18nEntry* e = lookup(dom, to, ctx, msgid, &cat);

  if (!e || e->ie_plural)
    return msgid;
  return e->ie_str[0];
}

const char* i18n_text(const struct I18nDomain* dom, const struct Client* to,
                      const char* msgid)
{
  return i18n_ctext(dom, to, 0, msgid);
}

const char* i18n_cntext(const struct I18nDomain* dom, const struct Client* to,
                        const char* ctx, const char* msgid,
                        const char* plural, unsigned long n)
{
  const struct I18nCatalog* cat;
  const struct I18nEntry* e = lookup(dom, to, ctx, msgid, &cat);
  unsigned int form;

  if (!e || !e->ie_plural || !cat->ic_plural)
    return n == 1 ? msgid : plural;
  form = po_plural_eval(cat->ic_plural, n);
  if (form >= e->ie_nstr)
    form = e->ie_nstr - 1;
  return e->ie_str[form];
}

const char* i18n_ntext(const struct I18nDomain* dom, const struct Client* to,
                       const char* msgid, const char* plural, unsigned long n)
{
  return i18n_cntext(dom, to, 0, msgid, plural, n);
}

/* ---------------------------------------------------------------------- */
/* Lifecycle                                                               */
/* ---------------------------------------------------------------------- */

void i18n_init(void)
{
  struct I18nDomain* dom;
  struct I18nDomain** pp;

  if (i18n_core)
    return;

  /* Opened by hand rather than with i18n_domain_open(): nothing is read
   * yet.  The catalogs come with i18n_load(), once the configuration has
   * been read, so that whatever it says about logging is in force.
   */
  dom = (struct I18nDomain*)MyCalloc(1, sizeof(struct I18nDomain));
  DupString(dom->id_name, "core");
  DupString(dom->id_dir, PO_PATH);
  for (pp = &domains; *pp; pp = &(*pp)->id_next)
    ;
  *pp = dom;
  i18n_core = dom;
  cap_value_build();
}

unsigned int i18n_load(void)
{
  unsigned int problems;

  if (!i18n_core)
    i18n_init();

  problems = domain_load(i18n_core);
  i18n_resolve();
  i18n_started = 1;
  return problems;
}

unsigned int i18n_rehash(void)
{
  struct I18nDomain* dom;
  unsigned int problems = 0;

  for (dom = domains; dom; dom = dom->id_next)
    problems += domain_load(dom);
  i18n_resolve();
  return problems;
}

void i18n_close(void)
{
  unsigned int i;

  while (domains)
    i18n_domain_close(domains);

  for (i = 1; i < pref_count; i++)
    MyFree(pref_table[i]);
  memset(pref_table, 0, sizeof(pref_table));
  memset(pref_buckets, 0, sizeof(pref_buckets));
  pref_count = 1;
  code_count = 1;
  default_chain_len = 0;
  i18n_started = 0;
}

/* ---------------------------------------------------------------------- */
/* /STATS                                                                  */
/* ---------------------------------------------------------------------- */

void i18n_stats(struct Client* sptr, const struct StatDesc* sd, char* param)
{
  struct I18nDomain* dom;
  struct I18nCatalog* cat;

  (void) sd;
  (void) param;

  for (dom = domains; dom; dom = dom->id_next) {
    if (!dom->id_catalogs)
      send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
                 N_(":%s: no catalogs in %s%s"), dom->id_name, dom->id_dir,
                 dom->id_refused ? " (files refused at the last load)" : "");
    for (cat = dom->id_catalogs; cat; cat = cat->ic_next)
      send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
                 N_(":%s %s: %u entries loaded, %u rejected%s%s"), dom->id_name,
                 cat->ic_lang, cat->ic_count, cat->ic_rejected,
                 cat->ic_plural ? ", plurals" : "",
                 cat != dom->id_catalogs || !dom->id_refused
                 ? "" : " (files refused at the last load)");
  }
  send_reply(sptr, SND_EXPLICIT | RPL_STATSDEBUG,
             N_(":%u language preference%s interned, default \"%s\", cap %s"),
             pref_count - 1, pref_count - 1 == 1 ? "" : "s",
             feature_str(FEAT_DEFAULT_LANGUAGE)
             ? feature_str(FEAT_DEFAULT_LANGUAGE) : "", cap_value);
}
