/* ircd_i18n_t.c - Test the PO reader, the validator and the lookup chain.
 *
 * Three layers, bottom up: ircd_po.c (escapes, continuation lines,
 * msgctxt, plurals, fuzzy, obsolete entries, the header), the format
 * validation of ircd_i18n.c (every rule with a case that passes and one
 * that does not, the exotic ircd_snprintf directives included), and the
 * lookup itself against real directories of catalogs -- the preference
 * chain, the default language, a reload that drops a language, a broken
 * file that keeps the previous catalog.
 *
 * Last, every .po in the source tree is loaded and the test fails if any
 * entry is rejected: this is what msgfmt --check would be, if msgfmt
 * understood %C and %Tu.  It is the check that runs in CI.
 */

#include "ircd_i18n.h"
#include "ircd_po.h"

#include "capab.h"
#include "client.h"
#include "ircd_features.h"
#include "ircd_string.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------------------------------------------------------------------- */
/* Stubs                                                                   */
/* ---------------------------------------------------------------------- */

/** What feature_str(FEAT_DEFAULT_LANGUAGE) answers. */
static const char* stub_default_language;

const char* feature_str(enum Feature feat)
{
  assert(feat == FEAT_DEFAULT_LANGUAGE);
  return stub_default_language;
}

/** The last capability value set. */
static char stub_cap_value[256];

void cap_set_value(int cap, const char* value)
{
  assert(cap == E_CAP_LANGUAGES);
  ircd_strncpy(stub_cap_value, value, sizeof(stub_cap_value) - 1);
}

/** Server notices are counted, and the last one kept. */
static int stub_notices;
static char stub_last_notice[512];

void sendto_opmask_butone(struct Client* one, unsigned int mask,
                          const char* pattern, ...)
{
  va_list vl;

  (void) one;
  (void) mask;
  va_start(vl, pattern);
  vsnprintf(stub_last_notice, sizeof(stub_last_notice), pattern, vl);
  va_end(vl);
  stub_notices++;
}

int send_reply(struct Client* to, int reply, ...)
{
  (void) to;
  (void) reply;
  return 0;
}

const char* visible_username(const struct Client* cptr)
{
  (void) cptr;
  return "";
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ---------------------------------------------------------------------- */

static char tmpdir[256];

static void make_tmpdir(void)
{
  const char* base = getenv("TMPDIR");

  snprintf(tmpdir, sizeof(tmpdir), "%s/ircd_i18n_t.XXXXXX",
           base && *base ? base : "/tmp");
  assert(mkdtemp(tmpdir) != NULL);
}

static void write_file(const char* dir, const char* name, const char* text)
{
  char path[1024];
  FILE* f;

  snprintf(path, sizeof(path), "%s/%s", dir, name);
  f = fopen(path, "wb");
  assert(f != NULL);
  fputs(text, f);
  fclose(f);
}

static void remove_file(const char* dir, const char* name)
{
  char path[1024];

  snprintf(path, sizeof(path), "%s/%s", dir, name);
  unlink(path);
}

static void make_dir(char* out, size_t len, const char* name)
{
  snprintf(out, len, "%s/%s", tmpdir, name);
  assert(mkdir(out, 0700) == 0);
}

static void rm_tree(const char* dir)
{
  DIR* d = opendir(dir);
  struct dirent* de;
  char path[1024];

  if (!d)
    return;
  while ((de = readdir(d))) {
    struct stat sb;

    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
      continue;
    snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
    if (stat(path, &sb) == 0 && S_ISDIR(sb.st_mode))
      rm_tree(path);
    else
      unlink(path);
  }
  closedir(d);
  rmdir(dir);
}

/** A client with a preference. */
static void client_init(struct Client* c, const char* langs)
{
  memset(c, 0, sizeof(*c));
  cli_status(c) = STAT_USER;
  if (langs) {
    char copy[128];
    const char* codes[8];
    unsigned int n = 0;
    char* tok;
    char* save = 0;

    ircd_strncpy(copy, langs, sizeof(copy) - 1);
    for (tok = strtok_r(copy, " ", &save); tok && n < 8;
         tok = strtok_r(0, " ", &save))
      codes[n++] = tok;
    i18n_set_languages(c, codes, n);
  }
}

/* ---------------------------------------------------------------------- */
/* The parser                                                              */
/* ---------------------------------------------------------------------- */

static void test_parser_basics(void)
{
  const char* text =
    "# translator comment\n"
    "#. extracted comment\n"
    "#: ircd/s_err.c:37\n"
    "#| msgid \"old\"\n"
    "msgid \"\"\n"
    "msgstr \"\"\n"
    "\"Project-Id-Version: ircu2\\n\"\n"
    "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
    "\"Plural-Forms: nplurals=2; plural=(n != 1);\\n\"\n"
    "\n"
    "msgctxt \"401\"\n"
    "msgid \"%s :No such nick\"\n"
    "msgstr \"%s :No existe ese nick\"\n"
    "\n"
    "msgid \"tab\\there \\\"quoted\\\" back\\\\slash hex\\x41 oct\\101\"\n"
    "msgstr \"\"\n"
    "\"first \"\n"
    "\"second\"\n"
    "\n"
    "msgid \"one user\"\n"
    "msgid_plural \"%d users\"\n"
    "msgstr[0] \"un usuario\"\n"
    "msgstr[1] \"%d usuarios\"\n"
    "\n"
    "#, fuzzy, c-format\n"
    "msgid \"fuzzy one\"\n"
    "msgstr \"borroso\"\n"
    "\n"
    "#~ msgid \"gone\"\n"
    "#~ msgstr \"ido\"\n"
    "\n"
    "msgid \"last\"\n"
    "msgstr \"último\"";   /* no final newline */
  char err[256];
  struct PoFile* pf = po_parse(text, err, sizeof(err));

  assert(pf != NULL);
  assert(pf->pf_count == 5);
  assert(pf->pf_nplurals == 2);
  assert(pf->pf_plural != NULL);
  assert(pf->pf_charset && !strcmp(pf->pf_charset, "UTF-8"));

  assert(pf->pf_entries[0].pe_ctx && !strcmp(pf->pf_entries[0].pe_ctx, "401"));
  assert(!strcmp(pf->pf_entries[0].pe_id, "%s :No such nick"));
  assert(pf->pf_entries[0].pe_nstr == 1);
  assert(!strcmp(pf->pf_entries[0].pe_str[0], "%s :No existe ese nick"));
  assert(pf->pf_entries[0].pe_line == 11);

  assert(!pf->pf_entries[1].pe_ctx);
  assert(!strcmp(pf->pf_entries[1].pe_id,
                 "tab\there \"quoted\" back\\slash hexA octA"));
  assert(!strcmp(pf->pf_entries[1].pe_str[0], "first second"));

  assert(pf->pf_entries[2].pe_plural
         && !strcmp(pf->pf_entries[2].pe_plural, "%d users"));
  assert(pf->pf_entries[2].pe_nstr == 2);
  assert(!strcmp(pf->pf_entries[2].pe_str[1], "%d usuarios"));

  assert(pf->pf_entries[3].pe_fuzzy);
  assert(!strcmp(pf->pf_entries[3].pe_id, "fuzzy one"));

  assert(!pf->pf_entries[4].pe_fuzzy);
  assert(!strcmp(pf->pf_entries[4].pe_id, "last"));
  assert(!strcmp(pf->pf_entries[4].pe_str[0], "\xc3\xbaltimo"));

  po_free(pf);
  printf("ok - parser: comments, header, msgctxt, escapes, continuation, "
         "plurals, fuzzy, obsolete\n");
}

static void test_parser_header_without_plurals(void)
{
  const char* text =
    "msgid \"\"\n"
    "msgstr \"Content-Type: text/plain; charset=utf-8\\n\"\n"
    "\n"
    "msgid \"a\"\n"
    "msgstr \"b\"\n";
  struct PoFile* pf = po_parse(text, 0, 0);

  assert(pf != NULL);
  assert(pf->pf_count == 1);
  assert(pf->pf_nplurals == 0);
  assert(!pf->pf_plural);
  assert(!strcmp(pf->pf_charset, "utf-8"));
  po_free(pf);

  /* No header at all is a file too. */
  pf = po_parse("msgid \"a\"\nmsgstr \"b\"\n", 0, 0);
  assert(pf && pf->pf_count == 1 && !pf->pf_charset);
  po_free(pf);

  /* And so is an empty one. */
  pf = po_parse("", 0, 0);
  assert(pf && pf->pf_count == 0);
  po_free(pf);

  /* Entries need no blank line between them: a comment or the next
   * msgid ends the previous one, as msgfmt reads it. */
  pf = po_parse("msgid \"a\"\nmsgstr \"b\"\n#: x.c:1\nmsgid \"c\"\n"
                "msgstr \"d\"\nmsgctxt \"k\"\nmsgid \"e\"\nmsgstr \"f\"\n"
                "#, fuzzy\nmsgid \"g\"\nmsgstr \"h\"\n", 0, 0);
  assert(pf && pf->pf_count == 4);
  assert(!strcmp(pf->pf_entries[1].pe_id, "c"));
  assert(pf->pf_entries[2].pe_ctx && !strcmp(pf->pf_entries[2].pe_ctx, "k"));
  assert(!pf->pf_entries[2].pe_fuzzy && pf->pf_entries[3].pe_fuzzy);
  po_free(pf);

  printf("ok - parser: header without Plural-Forms, no header, empty file\n");
}

static void parser_rejects(const char* what, const char* text,
                           const char* needle)
{
  char err[256];
  struct PoFile* pf = po_parse(text, err, sizeof(err));

  if (pf) {
    printf("FAIL - %s: parsed, %u entries\n", what, pf->pf_count);
    assert(0);
  }
  if (!strstr(err, needle)) {
    printf("FAIL - %s: wanted \"%s\" in: %s\n", what, needle, err);
    assert(0);
  }
  printf("ok - parser refuses %s\n      (%s)\n", what, err);
}

static void test_parser_errors(void)
{
  parser_rejects("an unterminated string",
                 "msgid \"abc\nmsgstr \"x\"\n", "line 1: unterminated");
  parser_rejects("a truncated file",
                 "msgid \"abc\"\nmsgstr \"x", "line 2: unterminated");
  parser_rejects("a msgstr with no msgid",
                 "msgstr \"x\"\n", "msgstr before msgid");
  parser_rejects("a msgid with no msgstr",
                 "msgid \"x\"\n\nmsgid \"y\"\nmsgstr \"z\"\n", "no msgstr");
  parser_rejects("an unknown keyword",
                 "msgid \"x\"\nmsgstrr \"y\"\n", "unknown keyword");
  parser_rejects("text after the closing quote",
                 "msgid \"x\" y\nmsgstr \"z\"\n", "after the closing quote");
  parser_rejects("an unknown escape",
                 "msgid \"\\q\"\nmsgstr \"z\"\n", "unknown escape");
  parser_rejects("a NUL escape",
                 "msgid \"a\\000b\"\nmsgstr \"z\"\n", "NUL");
  parser_rejects("msgstr[N] out of sequence",
                 "msgid \"a\"\nmsgid_plural \"b\"\nmsgstr[1] \"c\"\n",
                 "out of sequence");
  parser_rejects("a bad Plural-Forms header",
                 "msgid \"\"\nmsgstr \"Plural-Forms: nplurals=2; plural=(n +;\\n\"\n",
                 "Plural-Forms");
  parser_rejects("a second msgid in one entry",
                 "msgid \"a\"\nmsgid \"b\"\nmsgstr \"c\"\n", "second msgid");
  parser_rejects("a continuation line with nothing to continue",
                 "\"abc\"\n", "nothing to continue");
}

/* ---------------------------------------------------------------------- */
/* Plural-Forms                                                            */
/* ---------------------------------------------------------------------- */

struct PluralCase {
  unsigned long n;
  unsigned int  form;
};

static void check_plural(const char* lang, const char* rule,
                         unsigned int want_np, const struct PluralCase* cases)
{
  unsigned int np = 0;
  char err[128];
  struct PoPlural* pl = po_plural_compile(rule, &np, err, sizeof(err));
  int i;

  if (!pl) {
    printf("FAIL - %s: %s\n", lang, err);
    assert(0);
  }
  assert(np == want_np);
  for (i = 0; cases[i].n != (unsigned long)-1; i++) {
    unsigned int got = po_plural_eval(pl, cases[i].n);

    if (got != cases[i].form) {
      printf("FAIL - %s: n=%lu gave form %u, wanted %u\n", lang, cases[i].n,
             got, cases[i].form);
      assert(0);
    }
  }
  po_plural_free(pl);
  printf("ok - plural forms for %s\n", lang);
}

static void test_plurals(void)
{
  static const struct PluralCase en[] = {
    { 0, 1 }, { 1, 0 }, { 2, 1 }, { 100, 1 }, { (unsigned long)-1, 0 } };
  static const struct PluralCase fr[] = {
    { 0, 0 }, { 1, 0 }, { 2, 1 }, { 100, 1 }, { (unsigned long)-1, 0 } };
  static const struct PluralCase ru[] = {
    { 0, 2 }, { 1, 0 }, { 2, 1 }, { 4, 1 }, { 5, 2 }, { 11, 2 }, { 12, 2 },
    { 21, 0 }, { 22, 1 }, { 25, 2 }, { 101, 0 }, { 111, 2 }, { 112, 2 },
    { (unsigned long)-1, 0 } };
  static const struct PluralCase pl[] = {
    { 0, 2 }, { 1, 0 }, { 2, 1 }, { 4, 1 }, { 5, 2 }, { 12, 2 }, { 14, 2 },
    { 22, 1 }, { 25, 2 }, { 102, 1 }, { 112, 2 }, { (unsigned long)-1, 0 } };
  static const struct PluralCase ar[] = {
    { 0, 0 }, { 1, 1 }, { 2, 2 }, { 3, 3 }, { 10, 3 }, { 11, 4 }, { 99, 4 },
    { 100, 5 }, { 102, 5 }, { 103, 3 }, { 111, 4 }, { 200, 5 },
    { (unsigned long)-1, 0 } };
  static const struct PluralCase ja[] = {
    { 0, 0 }, { 1, 0 }, { 7, 0 }, { (unsigned long)-1, 0 } };
  char err[128];
  unsigned int np;

  check_plural("en", "nplurals=2; plural=(n != 1);", 2, en);
  check_plural("fr", "nplurals=2; plural=(n > 1);", 2, fr);
  check_plural("ru", "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : "
               "n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);",
               3, ru);
  check_plural("pl", "nplurals=3; plural=(n==1 ? 0 : n%10>=2 && n%10<=4 && "
               "(n%100<10 || n%100>=20) ? 1 : 2);", 3, pl);
  check_plural("ar", "nplurals=6; plural=(n==0 ? 0 : n==1 ? 1 : n==2 ? 2 : "
               "n%100>=3 && n%100<=10 ? 3 : n%100>=11 ? 4 : 5);", 6, ar);
  check_plural("ja", "nplurals=1; plural=0;", 1, ja);
  /* The order of the two clauses does not matter, nor do blanks. */
  check_plural("en (reversed)", " plural = ( n != 1 ) ; nplurals = 2 ", 2, en);

  assert(!po_plural_compile("nplurals=2;", &np, err, sizeof(err)));
  assert(!po_plural_compile("plural=n;", &np, err, sizeof(err)));
  assert(!po_plural_compile("nplurals=9; plural=0;", &np, err, sizeof(err)));
  assert(!po_plural_compile("nplurals=0; plural=0;", &np, err, sizeof(err)));
  assert(!po_plural_compile("nplurals=2; plural=(n", &np, err, sizeof(err)));
  assert(!po_plural_compile("nplurals=2; plural=n ? 1;", &np, err,
                            sizeof(err)));
  assert(!po_plural_compile("nplurals=2; plural=m;", &np, err, sizeof(err)));
  assert(!po_plural_compile("nplurals=2; plural=n 1;", &np, err, sizeof(err)));
  printf("ok - plural forms: malformed headers are refused\n");

  /* Division by zero is 0, and a form past nplurals is clamped. */
  {
    struct PoPlural* p = po_plural_compile("nplurals=2; plural=n/0;", &np, 0, 0);
    assert(p && po_plural_eval(p, 5) == 0);
    po_plural_free(p);
    p = po_plural_compile("nplurals=2; plural=n;", &np, 0, 0);
    assert(p && po_plural_eval(p, 5) == 1 && po_plural_eval(p, 0) == 0);
    po_plural_free(p);
  }
  printf("ok - plural forms: n/0 and out-of-range forms are harmless\n");
}

/* ---------------------------------------------------------------------- */
/* Format validation                                                       */
/* ---------------------------------------------------------------------- */

static void accepts(const char* msgid, const char* msgstr)
{
  char why[256];

  if (i18n_check_format(msgid, msgstr, why, sizeof(why))) {
    printf("FAIL - \"%s\" -> \"%s\" refused: %s\n", msgid, msgstr, why);
    assert(0);
  }
}

static void refuses(const char* msgid, const char* msgstr, const char* needle)
{
  char why[256];

  if (!i18n_check_format(msgid, msgstr, why, sizeof(why))) {
    printf("FAIL - \"%s\" -> \"%s\" accepted\n", msgid, msgstr);
    assert(0);
  }
  if (!strstr(why, needle)) {
    printf("FAIL - \"%s\" -> \"%s\": wanted \"%s\" in: %s\n", msgid, msgstr,
           needle, why);
    assert(0);
  }
}

static void test_format_validation(void)
{
  /* Rule 1: no line breaks. */
  accepts("hello", "hola");
  refuses("hello", "hola\nQUIT", "line break");
  refuses("hello", "hola\r", "line break");

  /* Rule 2: the same directives, in the same order. */
  accepts("%s :No such nick", "%s :No existe ese nick");
  accepts("%s :No such nick", ":No existe el nick %s");
  accepts("%s %s :%s", "%s %s :%s");
  refuses("%s :No such nick", ":No existe ese nick", "missing");
  refuses("%s :No such nick", "%s %s :No existe", "not in the original");
  refuses("%s :No such nick", "%d :No existe", "directive 1 is \"%d\"");
  refuses("%s %d", "%d %s", "directive 1 is \"%d\"");
  refuses("%s", "%s %", "malformed");
  refuses("%s", "%s %y", "malformed");
  refuses("%s", "%s %-", "malformed");

  /* %% may come and go. */
  accepts("100%% sure %s", "%s seguro al 100%%");
  accepts("%s", "%s al 50%%");
  accepts("100%% of %s", "%s");

  /* The exotic ones, byte for byte. */
  accepts("%:#C %s %C %v", "%:#C %s %C %v");
  refuses("%:#C %s", "%#C %s", "directive 1 is \"%#C\"");
  accepts("%Tu seconds", "%Tu segundos");
  refuses("%Tu seconds", "%lu segundos", "directive 1 is \"%lu\"");
  accepts("%*.*s|", "|%*.*s");
  refuses("%*.*s", "%*s", "directive 1 is \"%*s\"");
  accepts("%-12s %s", "%-12s %s");
  refuses("%-12s %s", "%12s %s", "directive 1 is \"%12s\"");
  accepts("%#x %u %c %H %m %p %ld %hhd %qd %jd %td %zd",
          "%#x %u %c %H %m %p %ld %hhd %qd %jd %td %zd");
  accepts("%5.3d", "%5.3d");
  refuses("%5.3d", "%5.2d", "directive 1");

  /* An original with a malformed directive is refused too, so a bad
   * msgid in the sources cannot pass by being copied. */
  refuses("%y", "%y", "in the original");

  printf("ok - format validation: every rule, both ways\n");
}

/* ---------------------------------------------------------------------- */
/* Domains and lookup                                                      */
/* ---------------------------------------------------------------------- */

static const char ES_PO[] =
  "msgid \"\"\n"
  "msgstr \"\"\n"
  "\"Content-Type: text/plain; charset=UTF-8\\n\"\n"
  "\"Plural-Forms: nplurals=2; plural=(n != 1);\\n\"\n"
  "\n"
  "msgctxt \"401\"\n"
  "msgid \"%s :No such nick\"\n"
  "msgstr \"%s :No existe ese nick\"\n"
  "\n"
  "msgid \":Flushing MOTD cache\"\n"
  "msgstr \":Vaciando la caché del MOTD\"\n"
  "\n"
  "msgid \"only in es\"\n"
  "msgstr \"sólo en es\"\n"
  "\n"
  "msgid \"%u user\"\n"
  "msgid_plural \"%u users\"\n"
  "msgstr[0] \"%u usuario\"\n"
  "msgstr[1] \"%u usuarios\"\n"
  "\n"
  "#, fuzzy\n"
  "msgid \"fuzzy\"\n"
  "msgstr \"borroso\"\n"
  "\n"
  "msgid \"untranslated\"\n"
  "msgstr \"\"\n"
  "\n"
  "msgid \"bad %s\"\n"
  "msgstr \"malo %d\"\n"
  "\n"
  "msgid \"break\"\n"
  "msgstr \"salto\\n\"\n"
  "\n"
  "msgid \"dup\"\n"
  "msgstr \"uno\"\n"
  "\n"
  "msgid \"dup\"\n"
  "msgstr \"dos\"\n"
  "\n"
  "msgid \"one %u\"\n"
  "msgid_plural \"many %u\"\n"
  "msgstr[0] \"uno %u\"\n"
  "msgstr[1] \"\"\n";

static const char ES_AR_PO[] =
  "msgid \"\"\n"
  "msgstr \"Content-Type: text/plain; charset=UTF-8\\n\"\n"
  "\n"
  "msgctxt \"401\"\n"
  "msgid \"%s :No such nick\"\n"
  "msgstr \"%s :No existe ese nick, che\"\n"
  "\n"
  "msgid \"%u user\"\n"
  "msgid_plural \"%u users\"\n"
  "msgstr[0] \"%u usuario, che\"\n"
  "msgstr[1] \"%u usuarios, che\"\n";

static const char PT_PO[] =
  "msgid \"\"\n"
  "msgstr \"Content-Type: text/plain; charset=UTF-8\\n\"\n"
  "\n"
  "msgid \":Flushing MOTD cache\"\n"
  "msgstr \":A limpar a cache do MOTD\"\n";

static void test_domain_load_and_validation(void)
{
  char dir[512];
  struct I18nDomain* dom;
  struct Client es;
  int before = stub_notices;

  make_dir(dir, sizeof(dir), "core1");
  write_file(dir, "es.po", ES_PO);
  write_file(dir, "es-AR.po", ES_AR_PO);
  write_file(dir, "pt.po", PT_PO);
  write_file(dir, "README", "not a catalog\n");
  write_file(dir, "x.po", "msgid \"a\"\nmsgstr \"b\"\n"); /* not a code */

  dom = i18n_domain_open("test", dir);
  assert(dom != NULL);
  assert(i18n_domain_has(dom, "es"));
  assert(i18n_domain_has(dom, "es-ar"));
  assert(i18n_domain_has(dom, "ES-AR"));
  assert(i18n_domain_has(dom, "pt"));
  assert(!i18n_domain_has(dom, "x"));
  assert(!i18n_domain_has(dom, "fr"));

  /* Four entries rejected in es.po (bad %d, line break, duplicate, a
   * plural with an empty form), one in es-AR.po (a plural with no
   * Plural-Forms), one file ignored: all reported. */
  assert(stub_notices - before == 6);
  printf("ok - domain: three catalogs loaded, bad entries and a bad name "
         "reported (%d notices)\n", stub_notices - before);

  client_init(&es, "es");
  assert(!strcmp(i18n_ctext(dom, &es, "401", "%s :No such nick"),
                 "%s :No existe ese nick"));
  assert(!strcmp(i18n_text(dom, &es, ":Flushing MOTD cache"),
                 ":Vaciando la caché del MOTD"));
  /* Without the context the numeric's text is not found. */
  assert(!strcmp(i18n_text(dom, &es, "%s :No such nick"), "%s :No such nick"));
  /* Rejected, fuzzy and untranslated entries fall back to the original. */
  assert(!strcmp(i18n_text(dom, &es, "bad %s"), "bad %s"));
  assert(!strcmp(i18n_text(dom, &es, "break"), "break"));
  assert(!strcmp(i18n_text(dom, &es, "fuzzy"), "fuzzy"));
  assert(!strcmp(i18n_text(dom, &es, "untranslated"), "untranslated"));
  /* The first of two duplicates wins. */
  assert(!strcmp(i18n_text(dom, &es, "dup"), "uno"));
  /* Plurals. */
  assert(!strcmp(i18n_ntext(dom, &es, "%u user", "%u users", 1),
                 "%u usuario"));
  assert(!strcmp(i18n_ntext(dom, &es, "%u user", "%u users", 0),
                 "%u usuarios"));
  assert(!strcmp(i18n_ntext(dom, &es, "%u user", "%u users", 7),
                 "%u usuarios"));
  /* A plural asked for as a singular, or the other way round, is not it. */
  assert(!strcmp(i18n_text(dom, &es, "%u user"), "%u user"));
  assert(!strcmp(i18n_ntext(dom, &es, "dup", "dups", 2), "dups"));
  assert(!strcmp(i18n_ntext(dom, &es, "one %u", "many %u", 2), "many %u"));
  printf("ok - lookup: context, plurals, and every kind of miss\n");

  i18n_domain_close(dom);
  rm_tree(dir);
}

static void test_lookup_chain(void)
{
  char dir[512];
  struct I18nDomain* dom;
  struct Client c;

  make_dir(dir, sizeof(dir), "core2");
  write_file(dir, "es.po", ES_PO);
  write_file(dir, "es-ar.po", ES_AR_PO);
  write_file(dir, "pt.po", PT_PO);
  dom = i18n_domain_open("test", dir);

  /* es-AR: exact catalog first, then es. */
  client_init(&c, "es-AR");
  assert(!strcmp(i18n_ctext(dom, &c, "401", "%s :No such nick"),
                 "%s :No existe ese nick, che"));
  assert(!strcmp(i18n_text(dom, &c, ":Flushing MOTD cache"),
                 ":Vaciando la caché del MOTD"));
  assert(!strcmp(i18n_text(dom, &c, "nowhere"), "nowhere"));
  /* es-AR has no Plural-Forms: its plural entries were rejected, and the
   * es ones are used. */
  assert(!strcmp(i18n_ntext(dom, &c, "%u user", "%u users", 2),
                 "%u usuarios"));

  /* es-MX: no such catalog, so es. */
  client_init(&c, "es-MX");
  assert(!strcmp(i18n_ctext(dom, &c, "401", "%s :No such nick"),
                 "%s :No existe ese nick"));

  /* pt, then es: the first that has it. */
  client_init(&c, "pt es");
  assert(!strcmp(i18n_text(dom, &c, ":Flushing MOTD cache"),
                 ":A limpar a cache do MOTD"));
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "sólo en es"));

  /* en stops the chain: what follows it is never consulted. */
  client_init(&c, "en es");
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "only in es"));
  client_init(&c, "en-US es");
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "only in es"));

  /* A client with no preference gets the original... */
  client_init(&c, 0);
  assert(cli_lang(&c) == 0);
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "only in es"));

  /* ... unless the server has a default language. */
  stub_default_language = "es";
  i18n_resolve();
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "sólo en es"));
  assert(!strcmp(i18n_text(dom, &c, ":Flushing MOTD cache"),
                 ":Vaciando la caché del MOTD"));
  /* The default follows the client's own preference, not the reverse. */
  client_init(&c, "pt");
  assert(!strcmp(i18n_text(dom, &c, ":Flushing MOTD cache"),
                 ":A limpar a cache do MOTD"));
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "sólo en es"));
  /* A default that is not a catalog is the original. */
  stub_default_language = "fr";
  i18n_resolve();
  client_init(&c, 0);
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "only in es"));
  /* A default of "en" is the original even when a client asks for
   * something else that does not exist. */
  stub_default_language = "en";
  i18n_resolve();
  client_init(&c, "fr");
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "only in es"));
  stub_default_language = 0;
  i18n_resolve();

  /* NULL domain, NULL client, a server: always the original. */
  assert(!strcmp(i18n_text(0, &c, "only in es"), "only in es"));
  assert(!strcmp(i18n_text(dom, 0, "only in es"), "only in es"));
  client_init(&c, "es");
  {
    /* Only the pointer is looked at, so anything non-NULL is a server. */
    cli_serv(&c) = (struct Server*)&c;
    assert(!strcmp(i18n_text(dom, &c, "only in es"), "only in es"));
    cli_serv(&c) = 0;
    assert(!strcmp(i18n_text(dom, &c, "only in es"), "sólo en es"));
  }
  printf("ok - lookup chain: es-ar -> es -> default -> original, en stops it\n");

  /* The preference as LG and WHOIS show it: lower case, deduplicated,
   * at most I18N_PREF_MAX, invalid codes dropped. */
  client_init(&c, "ES-ar es Es pt EN");
  assert(!strcmp(i18n_languages_str(&c), "es-ar es pt"));
  client_init(&c, "x es 1234 toolongcodeforus-x es-");
  assert(!strcmp(i18n_languages_str(&c), "es"));
  client_init(&c, "xx");
  assert(!strcmp(i18n_languages_str(&c), "xx"));
  client_init(&c, "");
  assert(i18n_languages_str(&c) == NULL);
  /* The same preference is interned once. */
  {
    struct Client d;
    client_init(&c, "es-ar es");
    client_init(&d, "ES-AR ES");
    assert(cli_lang(&c) == cli_lang(&d) && cli_lang(&c) != 0);
    client_init(&d, "es es-ar");
    assert(cli_lang(&c) != cli_lang(&d));
  }
  printf("ok - preferences: normalised, deduplicated, interned\n");

  /* What LANGUAGE accepts: a catalog, a variant of one, or en. */
  assert(i18n_language_known("es"));
  assert(i18n_language_known("ES-MX"));
  assert(i18n_language_known("pt"));
  assert(i18n_language_known("en"));
  assert(i18n_language_known("en-GB"));
  assert(!i18n_language_known("fr"));
  assert(!i18n_language_known("x"));
  assert(!i18n_language_known(""));
  assert(!i18n_language_known("es_MX"));
  printf("ok - known languages: exact, primary subtag, source language\n");

  /* The capability value: max, then the default, then the rest sorted. */
  assert(!strcmp(i18n_cap_value(), "3,en,es,es-ar,pt"));
  stub_default_language = "pt";
  i18n_resolve();
  assert(!strcmp(i18n_cap_value(), "3,pt,en,es,es-ar"));
  assert(!strcmp(stub_cap_value, "3,pt,en,es,es-ar"));
  stub_default_language = "fr"; /* not a catalog: not first */
  i18n_resolve();
  assert(!strcmp(i18n_cap_value(), "3,en,es,es-ar,pt"));
  stub_default_language = 0;
  i18n_resolve();
  printf("ok - capability value: %s\n", i18n_cap_value());

  /* A reload that drops a language, and one that replaces it. */
  remove_file(dir, "pt.po");
  write_file(dir, "es.po",
             "msgid \"only in es\"\nmsgstr \"nuevo\"\n");
  i18n_domain_reload(dom);
  assert(!i18n_domain_has(dom, "pt"));
  assert(!i18n_language_known("pt"));
  client_init(&c, "pt es");
  assert(!strcmp(i18n_text(dom, &c, ":Flushing MOTD cache"),
                 ":Flushing MOTD cache"));
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "nuevo"));
  assert(!strcmp(i18n_cap_value(), "3,en,es,es-ar"));
  printf("ok - reload: a removed catalog is dropped, a changed one replaced\n");

  /* A broken file keeps the previous catalog.  (Two problems per reload
   * from here on: the broken es.po, and the plural entry es-ar.po cannot
   * hold without a Plural-Forms header.) */
  write_file(dir, "es.po", "msgid \"only in es\nmsgstr \"roto\"\n");
  assert(i18n_domain_reload(dom) == 2);
  assert(i18n_domain_has(dom, "es"));
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "nuevo"));
  assert(strstr(stub_last_notice, "keeping the previous catalog"));
  /* And a wrong charset is a broken file. */
  write_file(dir, "es.po",
             "msgid \"\"\nmsgstr \"Content-Type: text/plain; charset=ISO-8859-1\\n\"\n"
             "\nmsgid \"only in es\"\nmsgstr \"latin1\"\n");
  assert(i18n_domain_reload(dom) == 2);
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "nuevo"));
  assert(strstr(stub_last_notice, "not UTF-8"));
  /* And so is one that is not valid UTF-8. */
  write_file(dir, "es.po", "msgid \"only in es\"\nmsgstr \"mal \xff\"\n");
  assert(i18n_domain_reload(dom) == 2);
  assert(!strcmp(i18n_text(dom, &c, "only in es"), "nuevo"));
  assert(strstr(stub_last_notice, "UTF-8"));
  printf("ok - reload: a file that does not parse keeps the old catalog\n");

  /* A second domain is separate: nothing crosses. */
  {
    char dir2[512];
    struct I18nDomain* mod;

    make_dir(dir2, sizeof(dir2), "mod");
    write_file(dir2, "es.po", "msgid \"module text\"\nmsgstr \"texto del módulo\"\n");
    write_file(dir2, "fr.po", "msgid \"module text\"\nmsgstr \"texte du module\"\n");
    mod = i18n_domain_open("mod", dir2);
    client_init(&c, "es");
    assert(!strcmp(i18n_text(mod, &c, "module text"), "texto del módulo"));
    assert(!strcmp(i18n_text(dom, &c, "module text"), "module text"));
    assert(!strcmp(i18n_text(mod, &c, "only in es"), "only in es"));
    /* Its language counts for everybody. */
    assert(i18n_language_known("fr"));
    assert(!strcmp(i18n_cap_value(), "3,en,es,es-ar,fr"));
    i18n_domain_close(mod);
    assert(!i18n_language_known("fr"));
    assert(!strcmp(i18n_text(dom, &c, "only in es"), "nuevo"));
    rm_tree(dir2);
    printf("ok - domains do not see each other; closing one keeps the rest\n");
  }

  /* A directory that does not exist is a domain with nothing in it. */
  {
    struct I18nDomain* none = i18n_domain_open("none", "/nonexistent/po");
    int before = stub_notices;
    assert(none != NULL);
    assert(!strcmp(i18n_text(none, &c, "x"), "x"));
    i18n_domain_reload(none);
    assert(stub_notices == before);
    i18n_domain_close(none);
    printf("ok - a missing directory is not an error\n");
  }

  i18n_domain_close(dom);
  rm_tree(dir);
}

/* ---------------------------------------------------------------------- */
/* The catalogs in the tree                                                */
/* ---------------------------------------------------------------------- */

static unsigned int tree_problems;

static void check_tree_dir(const char* dir, const char* name)
{
  struct I18nDomain* dom;
  int before = stub_notices;
  struct stat sb;

  if (stat(dir, &sb) < 0 || !S_ISDIR(sb.st_mode))
    return;

  dom = i18n_domain_open(name, dir);
  if (stub_notices != before) {
    printf("FAIL - %s: %d problem(s) loading %s; last: %s\n", name,
           stub_notices - before, dir, stub_last_notice);
    tree_problems += stub_notices - before;
  } else {
    printf("ok - %s: every catalog in %s loads clean\n", name, dir);
  }
  i18n_domain_close(dom);
}

static void test_tree_catalogs(void)
{
  char dir[1024];
  DIR* types;
  struct dirent* t;

  snprintf(dir, sizeof(dir), "%s/po", IRCU_SOURCE_DIR);
  check_tree_dir(dir, "core");

  snprintf(dir, sizeof(dir), "%s/modules", IRCU_SOURCE_DIR);
  types = opendir(dir);
  assert(types != NULL);
  while ((t = readdir(types))) {
    char typedir[1024];
    DIR* mods;
    struct dirent* m;

    if (t->d_name[0] == '.')
      continue;
    snprintf(typedir, sizeof(typedir), "%s/modules/%s", IRCU_SOURCE_DIR,
             t->d_name);
    if (!(mods = opendir(typedir)))
      continue;
    while ((m = readdir(mods))) {
      if (m->d_name[0] == '.')
        continue;
      if (snprintf(dir, sizeof(dir), "%s/%s/po", typedir, m->d_name)
          >= (int)sizeof(dir))
        continue;
      check_tree_dir(dir, m->d_name);
    }
    closedir(mods);
  }
  closedir(types);

  assert(tree_problems == 0);
}

int main(int argc, char* argv[])
{
  (void) argc;
  (void) argv;

  setvbuf(stdout, NULL, _IONBF, 0);
  make_tmpdir();

  test_parser_basics();
  test_parser_header_without_plurals();
  test_parser_errors();
  test_plurals();
  test_format_validation();
  test_domain_load_and_validation();
  test_lookup_chain();
  test_tree_catalogs();

  i18n_close();
  rm_tree(tmpdir);

  printf("ok - all done\n");
  return 0;
}
