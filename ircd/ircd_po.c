/*
 * IRC - Internet Relay Chat, ircd/ircd_po.c
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
 * Deliberately independent of the rest of the server -- ircd_alloc.h and
 * nothing else -- so the unit test can drive it without a client or a
 * catalog in sight.  What it produces is the file as written: the entry
 * order, the flags, the untranslated entries.  Deciding what to keep is
 * ircd_i18n.c's job.
 */
#include "config.h"

#include "ircd_po.h"
#include "ircd_alloc.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* A growable string                                                       */
/* ---------------------------------------------------------------------- */

struct PoBuf {
  char*  b_data;
  size_t b_len;
  size_t b_room;
};

static void buf_init(struct PoBuf* b)
{
  b->b_data = 0;
  b->b_len = 0;
  b->b_room = 0;
}

static void buf_add(struct PoBuf* b, const char* s, size_t n)
{
  if (b->b_len + n + 1 > b->b_room) {
    size_t room = b->b_room ? b->b_room * 2 : 64;

    while (room < b->b_len + n + 1)
      room *= 2;
    b->b_data = (char*)MyRealloc(b->b_data, room);
    b->b_room = room;
  }
  memcpy(b->b_data + b->b_len, s, n);
  b->b_len += n;
  b->b_data[b->b_len] = '\0';
}

/** Hand the string over, or an empty one if nothing was ever added. */
static char* buf_take(struct PoBuf* b)
{
  char* s;

  if (!b->b_data)
    buf_add(b, "", 0);
  s = b->b_data;
  buf_init(b);
  return s;
}

static void buf_free(struct PoBuf* b)
{
  MyFree(b->b_data);
  buf_init(b);
}

/* ---------------------------------------------------------------------- */
/* The parser                                                              */
/* ---------------------------------------------------------------------- */

/** Which string the next continuation line belongs to. */
enum PoField {
  PF_NONE,
  PF_CTX,
  PF_ID,
  PF_PLURAL,
  PF_STR      /* pe_str[cur_index] */
};

struct PoParser {
  const char*     text;
  unsigned int    line;
  char*           err;
  size_t          errlen;

  /* The entry being read. */
  struct PoBuf    ctx, id, plural, str[I18N_NPLURALS_MAX];
  int             has_ctx, has_id, has_plural, has_str;
  unsigned int    nstr;
  int             fuzzy, obsolete;
  unsigned int    start_line;
  enum PoField    field;
  unsigned int    index;

  /* The file being built. */
  struct PoFile*  pf;
  unsigned int    room;
  int             header_seen;
};

static void po_error(struct PoParser* p, const char* what)
{
  if (p->err && p->errlen)
    snprintf(p->err, p->errlen, "line %u: %s", p->line, what);
}

static int hexval(int c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/** Read one quoted string starting at \a s, appending its unescaped
 * contents to \a out.
 * @return Pointer past the closing quote, or NULL on error.
 */
static const char* read_string(struct PoParser* p, const char* s,
                               struct PoBuf* out)
{
  if (*s != '"') {
    po_error(p, "expected a quoted string");
    return 0;
  }
  s++;

  while (*s && *s != '"' && *s != '\n') {
    if (*s != '\\') {
      const char* start = s;

      while (*s && *s != '"' && *s != '\\' && *s != '\n')
        s++;
      buf_add(out, start, s - start);
      continue;
    }

    /* An escape. */
    s++;
    switch (*s) {
    case 'n':  buf_add(out, "\n", 1); s++; break;
    case 't':  buf_add(out, "\t", 1); s++; break;
    case 'r':  buf_add(out, "\r", 1); s++; break;
    case 'a':  buf_add(out, "\a", 1); s++; break;
    case 'b':  buf_add(out, "\b", 1); s++; break;
    case 'f':  buf_add(out, "\f", 1); s++; break;
    case 'v':  buf_add(out, "\v", 1); s++; break;
    case '\\': buf_add(out, "\\", 1); s++; break;
    case '"':  buf_add(out, "\"", 1); s++; break;
    case '\'': buf_add(out, "'", 1); s++; break;
    case '?':  buf_add(out, "?", 1); s++; break;
    case 'x': {
      int v = 0, digits = 0;

      s++;
      while (hexval(*s) >= 0 && digits < 2) {
        v = v * 16 + hexval(*s);
        s++;
        digits++;
      }
      if (!digits) {
        po_error(p, "\\x with no hex digits");
        return 0;
      }
      if (!v) {
        po_error(p, "a NUL byte in a string");
        return 0;
      }
      {
        char c = (char)v;
        buf_add(out, &c, 1);
      }
      break;
    }
    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7': {
      int v = 0, digits = 0;

      while (*s >= '0' && *s <= '7' && digits < 3) {
        v = v * 8 + (*s - '0');
        s++;
        digits++;
      }
      if (!v) {
        po_error(p, "a NUL byte in a string");
        return 0;
      }
      if (v > 255) {
        po_error(p, "octal escape out of range");
        return 0;
      }
      {
        char c = (char)v;
        buf_add(out, &c, 1);
      }
      break;
    }
    case '\0':
    case '\n':
      po_error(p, "unterminated string");
      return 0;
    default:
      po_error(p, "unknown escape sequence");
      return 0;
    }
  }

  if (*s != '"') {
    po_error(p, "unterminated string");
    return 0;
  }
  s++;

  /* Only blanks or a comment may follow. */
  while (*s == ' ' || *s == '\t' || *s == '\r')
    s++;
  if (*s && *s != '\n' && *s != '#') {
    po_error(p, "text after the closing quote");
    return 0;
  }

  return s;
}

static void entry_reset(struct PoParser* p)
{
  unsigned int i;

  buf_free(&p->ctx);
  buf_free(&p->id);
  buf_free(&p->plural);
  for (i = 0; i < I18N_NPLURALS_MAX; i++)
    buf_free(&p->str[i]);
  p->has_ctx = p->has_id = p->has_plural = p->has_str = 0;
  p->nstr = 0;
  p->fuzzy = p->obsolete = 0;
  p->field = PF_NONE;
  p->index = 0;
  p->start_line = 0;
}

static int entry_pending(const struct PoParser* p)
{
  return p->has_ctx || p->has_id || p->has_plural || p->has_str
    || p->fuzzy || p->obsolete;
}

/** Pull one "Key: value" line out of a header's msgstr.
 * @return A copy of the value, trimmed, or NULL.
 */
static char* header_field(const char* hdr, const char* key)
{
  size_t klen = strlen(key);
  const char* s = hdr;

  while (*s) {
    const char* eol = strchr(s, '\n');
    size_t len = eol ? (size_t)(eol - s) : strlen(s);

    if (len > klen && !strncasecmp(s, key, klen) && s[klen] == ':') {
      const char* v = s + klen + 1;
      const char* end = s + len;
      char* copy;

      while (v < end && (*v == ' ' || *v == '\t'))
        v++;
      while (end > v && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
        end--;
      copy = (char*)MyMalloc(end - v + 1);
      memcpy(copy, v, end - v);
      copy[end - v] = '\0';
      return copy;
    }

    if (!eol)
      break;
    s = eol + 1;
  }

  return 0;
}

/** The header entry: charset and Plural-Forms. */
static int take_header(struct PoParser* p)
{
  const char* hdr = p->str[0].b_data ? p->str[0].b_data : "";
  char* ctype;
  char* forms;

  if (p->header_seen) {
    po_error(p, "a second header entry");
    return 0;
  }
  p->header_seen = 1;

  if ((ctype = header_field(hdr, "Content-Type"))) {
    const char* cs = ctype;

    while (*cs) {
      if (!strncasecmp(cs, "charset=", 8)) {
        const char* v = cs + 8;
        const char* end = v;

        while (*end && *end != ';' && *end != ' ' && *end != '\t')
          end++;
        p->pf->pf_charset = (char*)MyMalloc(end - v + 1);
        memcpy(p->pf->pf_charset, v, end - v);
        p->pf->pf_charset[end - v] = '\0';
        break;
      }
      cs++;
    }
    MyFree(ctype);
  }

  if ((forms = header_field(hdr, "Plural-Forms"))) {
    char why[128];

    p->pf->pf_plural = po_plural_compile(forms, &p->pf->pf_nplurals, why,
                                         sizeof(why));
    MyFree(forms);
    if (!p->pf->pf_plural) {
      char msg[192];

      snprintf(msg, sizeof(msg), "bad Plural-Forms: %s", why);
      po_error(p, msg);
      return 0;
    }
  }

  return 1;
}

/** Close the entry being read and add it to the file. */
static int entry_flush(struct PoParser* p)
{
  struct PoEntry* e;
  unsigned int i;

  if (!entry_pending(p))
    return 1;

  if (p->obsolete) {
    entry_reset(p);
    return 1;
  }

  if (!p->has_id) {
    po_error(p, "an entry with no msgid");
    return 0;
  }
  if (!p->has_str) {
    po_error(p, "an entry with no msgstr");
    return 0;
  }
  /* The header: msgid "" without a context. */
  if (!p->has_ctx && p->id.b_len == 0 && !p->has_plural) {
    int ok = take_header(p);

    entry_reset(p);
    return ok;
  }

  if (p->pf->pf_count == p->room) {
    p->room = p->room ? p->room * 2 : 64;
    p->pf->pf_entries = (struct PoEntry*)
      MyRealloc(p->pf->pf_entries, p->room * sizeof(struct PoEntry));
  }
  e = &p->pf->pf_entries[p->pf->pf_count++];
  memset(e, 0, sizeof(*e));

  e->pe_ctx = p->has_ctx ? buf_take(&p->ctx) : 0;
  e->pe_id = buf_take(&p->id);
  e->pe_plural = p->has_plural ? buf_take(&p->plural) : 0;
  e->pe_nstr = p->nstr;
  for (i = 0; i < p->nstr; i++)
    e->pe_str[i] = buf_take(&p->str[i]);
  e->pe_fuzzy = p->fuzzy ? 1 : 0;
  e->pe_line = p->start_line;

  entry_reset(p);
  return 1;
}

/** Note that a keyword line begins a field of the current entry. */
static int begin_field(struct PoParser* p, enum PoField f, unsigned int idx)
{
  if (!p->start_line)
    p->start_line = p->line;

  switch (f) {
  case PF_CTX:
    if (p->has_ctx || p->has_id) {
      po_error(p, "msgctxt out of order");
      return 0;
    }
    p->has_ctx = 1;
    break;
  case PF_ID:
    if (p->has_id) {
      po_error(p, "a second msgid in one entry");
      return 0;
    }
    p->has_id = 1;
    break;
  case PF_PLURAL:
    if (!p->has_id || p->has_plural || p->has_str) {
      po_error(p, "msgid_plural out of order");
      return 0;
    }
    p->has_plural = 1;
    break;
  case PF_STR:
    if (!p->has_id) {
      po_error(p, "msgstr before msgid");
      return 0;
    }
    if (idx >= I18N_NPLURALS_MAX) {
      po_error(p, "too many plural forms");
      return 0;
    }
    if (idx != p->nstr) {
      po_error(p, "msgstr[N] out of sequence");
      return 0;
    }
    p->has_str = 1;
    p->nstr = idx + 1;
    break;
  default:
    break;
  }

  p->field = f;
  p->index = idx;
  return 1;
}

static struct PoBuf* field_buf(struct PoParser* p)
{
  switch (p->field) {
  case PF_CTX:    return &p->ctx;
  case PF_ID:     return &p->id;
  case PF_PLURAL: return &p->plural;
  case PF_STR:    return &p->str[p->index];
  default:        return 0;
  }
}

/** Parse one line.  \a s points at its first character; the line ends at
 * '\n' or at the end of the text.
 */
static int parse_line(struct PoParser* p, const char* s)
{
  const char* end;

  /* A blank line ends the entry. */
  while (*s == ' ' || *s == '\t' || *s == '\r')
    s++;
  if (!*s || *s == '\n')
    return entry_flush(p);

  /* So does anything that begins the next one -- a comment or a msgctxt
   * or msgid -- once this one has its msgstr.  The tools always leave a
   * blank line between entries; a hand-edited file may not.
   */
  if (p->has_str && !p->obsolete
      && (*s == '#' || !strncmp(s, "msgctxt", 7) || !strncmp(s, "msgid", 5))
      && !entry_flush(p))
    return 0;

  if (*s == '#') {
    if (s[1] == ',') {
      /* Flags: only fuzzy matters, and only if the entry has begun --
       * a flags line belongs to the entry that follows it.
       */
      const char* f = s + 2;

      if (!p->start_line)
        p->start_line = p->line;
      while (*f && *f != '\n') {
        while (*f == ' ' || *f == ',')
          f++;
        if (!strncmp(f, "fuzzy", 5) && (f[5] == ',' || f[5] == ' '
                                        || f[5] == '\n' || f[5] == '\r'
                                        || !f[5]))
          p->fuzzy = 1;
        while (*f && *f != ',' && *f != '\n')
          f++;
      }
    } else if (s[1] == '~') {
      /* An obsolete entry: read nothing of it, and drop it whole. */
      if (!p->start_line)
        p->start_line = p->line;
      p->obsolete = 1;
    }
    /* Every other comment is for the tools. */
    return 1;
  }

  if (p->obsolete) {
    po_error(p, "an obsolete entry followed by a live line");
    return 0;
  }

  if (*s == '"') {
    struct PoBuf* b = field_buf(p);

    if (!b) {
      po_error(p, "a continuation line with nothing to continue");
      return 0;
    }
    return read_string(p, s, b) != 0;
  }

  if (!strncmp(s, "msgctxt", 7) && isspace((unsigned char)s[7])) {
    if (!begin_field(p, PF_CTX, 0))
      return 0;
    s += 7;
  } else if (!strncmp(s, "msgid_plural", 12) && isspace((unsigned char)s[12])) {
    if (!begin_field(p, PF_PLURAL, 0))
      return 0;
    s += 12;
  } else if (!strncmp(s, "msgid", 5) && isspace((unsigned char)s[5])) {
    if (!begin_field(p, PF_ID, 0))
      return 0;
    s += 5;
  } else if (!strncmp(s, "msgstr[", 7)) {
    unsigned int idx = 0;
    int digits = 0;

    s += 7;
    while (*s >= '0' && *s <= '9' && digits < 3) {
      idx = idx * 10 + (*s - '0');
      s++;
      digits++;
    }
    if (!digits || *s != ']') {
      po_error(p, "bad msgstr[N]");
      return 0;
    }
    s++;
    if (!begin_field(p, PF_STR, idx))
      return 0;
  } else if (!strncmp(s, "msgstr", 6) && isspace((unsigned char)s[6])) {
    if (!begin_field(p, PF_STR, 0))
      return 0;
    s += 6;
  } else {
    po_error(p, "unknown keyword");
    return 0;
  }

  while (*s == ' ' || *s == '\t')
    s++;
  end = read_string(p, s, field_buf(p));
  return end != 0;
}

struct PoFile* po_parse(const char* text, char* err, size_t errlen)
{
  struct PoParser p;
  const char* s = text;
  int ok = 1;

  if (err && errlen)
    *err = '\0';

  memset(&p, 0, sizeof(p));
  p.text = text;
  p.err = err;
  p.errlen = errlen;
  p.pf = (struct PoFile*)MyCalloc(1, sizeof(struct PoFile));

  /* A UTF-8 byte order mark is not part of the file. */
  if (!strncmp(s, "\xef\xbb\xbf", 3))
    s += 3;

  while (ok && *s) {
    const char* eol = strchr(s, '\n');

    p.line++;
    if (eol && (size_t)(eol - s) > I18N_PO_LINE_MAX) {
      po_error(&p, "line too long");
      ok = 0;
      break;
    }
    ok = parse_line(&p, s);
    if (!eol)
      break;
    s = eol + 1;
  }

  if (ok)
    ok = entry_flush(&p);

  entry_reset(&p);

  if (!ok) {
    po_free(p.pf);
    return 0;
  }

  return p.pf;
}

struct PoFile* po_parse_file(const char* path, char* err, size_t errlen)
{
  struct PoFile* pf;
  FILE* f;
  char* text;
  size_t len = 0, room = 16384;
  long bad;

  if (err && errlen)
    *err = '\0';

  f = fopen(path, "rb");
  if (!f) {
    if (err && errlen)
      snprintf(err, errlen, "cannot open: %s", strerror(errno));
    return 0;
  }

  text = (char*)MyMalloc(room);
  for (;;) {
    size_t n = fread(text + len, 1, room - len - 1, f);

    len += n;
    if (n == 0)
      break;
    if (len + 1 >= room) {
      room *= 2;
      text = (char*)MyRealloc(text, room);
    }
  }
  if (ferror(f)) {
    if (err && errlen)
      snprintf(err, errlen, "read error: %s", strerror(errno));
    fclose(f);
    MyFree(text);
    return 0;
  }
  fclose(f);
  text[len] = '\0';

  /* A NUL inside the file would silently end it here. */
  if (strlen(text) != len) {
    if (err && errlen)
      snprintf(err, errlen, "a NUL byte at offset %lu",
               (unsigned long)strlen(text));
    MyFree(text);
    return 0;
  }

  if ((bad = po_utf8_check(text, len)) >= 0) {
    unsigned int line = 1;
    long i;

    for (i = 0; i < bad; i++)
      if (text[i] == '\n')
        line++;
    if (err && errlen)
      snprintf(err, errlen, "line %u: not valid UTF-8", line);
    MyFree(text);
    return 0;
  }

  pf = po_parse(text, err, errlen);
  MyFree(text);
  return pf;
}

void po_free(struct PoFile* pf)
{
  unsigned int i, j;

  if (!pf)
    return;

  for (i = 0; i < pf->pf_count; i++) {
    struct PoEntry* e = &pf->pf_entries[i];

    MyFree(e->pe_ctx);
    MyFree(e->pe_id);
    MyFree(e->pe_plural);
    for (j = 0; j < e->pe_nstr; j++)
      MyFree(e->pe_str[j]);
  }
  MyFree(pf->pf_entries);
  MyFree(pf->pf_charset);
  po_plural_free(pf->pf_plural);
  MyFree(pf);
}

/* ---------------------------------------------------------------------- */
/* UTF-8                                                                   */
/* ---------------------------------------------------------------------- */

long po_utf8_check(const char* text, size_t len)
{
  const unsigned char* s = (const unsigned char*)text;
  size_t i = 0;

  while (i < len) {
    unsigned char c = s[i];
    size_t need;
    unsigned long cp;
    size_t k;

    if (c < 0x80) {
      i++;
      continue;
    }
    if ((c & 0xE0) == 0xC0) {
      need = 1;
      cp = c & 0x1F;
      if (cp < 2) /* overlong two-byte form */
        return (long)i;
    } else if ((c & 0xF0) == 0xE0) {
      need = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      need = 3;
      cp = c & 0x07;
    } else {
      return (long)i;
    }

    if (i + need >= len) /* not enough bytes left */
      return (long)i;
    for (k = 1; k <= need; k++) {
      if (i + k >= len || (s[i + k] & 0xC0) != 0x80)
        return (long)i;
      cp = (cp << 6) | (s[i + k] & 0x3F);
    }
    if ((need == 2 && cp < 0x800) || (need == 3 && cp < 0x10000))
      return (long)i; /* overlong */
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
      return (long)i;
    i += need + 1;
  }

  return -1;
}

/* ---------------------------------------------------------------------- */
/* Plural-Forms                                                            */
/* ---------------------------------------------------------------------- */

enum PoOp {
  OP_NUM,     /* a constant */
  OP_N,       /* the variable */
  OP_NOT,
  OP_TERNARY,
  OP_OR, OP_AND,
  OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
  OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD
};

struct PoNode {
  enum PoOp     op;
  unsigned long value;
  int           kids[3];  /* indices into the pool; -1 when absent */
};

struct PoPlural {
  struct PoNode* nodes;
  unsigned int   count;
  unsigned int   room;
  unsigned int   nplurals;
  int            root;
};

struct PoExprParser {
  const char*      s;
  struct PoPlural* pl;
  const char*      err;
  unsigned int     depth;
};

/** Deepest nesting a rule may have; the real ones are three or four. */
#define PO_EXPR_DEPTH_MAX 64

static int node_new(struct PoExprParser* p, enum PoOp op, int a, int b, int c)
{
  struct PoPlural* pl = p->pl;
  struct PoNode* n;

  if (pl->count == pl->room) {
    pl->room = pl->room ? pl->room * 2 : 16;
    pl->nodes = (struct PoNode*)
      MyRealloc(pl->nodes, pl->room * sizeof(struct PoNode));
  }
  n = &pl->nodes[pl->count];
  n->op = op;
  n->value = 0;
  n->kids[0] = a;
  n->kids[1] = b;
  n->kids[2] = c;
  return (int)pl->count++;
}

static void skip_ws(struct PoExprParser* p)
{
  while (*p->s == ' ' || *p->s == '\t')
    p->s++;
}

static int parse_ternary(struct PoExprParser* p);

static int parse_primary(struct PoExprParser* p)
{
  int n;

  skip_ws(p);
  if (*p->s == '(') {
    p->s++;
    n = parse_ternary(p);
    if (n < 0)
      return -1;
    skip_ws(p);
    if (*p->s != ')') {
      p->err = "missing )";
      return -1;
    }
    p->s++;
    return n;
  }
  if (*p->s == 'n' && !isalnum((unsigned char)p->s[1]) && p->s[1] != '_') {
    p->s++;
    return node_new(p, OP_N, -1, -1, -1);
  }
  if (*p->s >= '0' && *p->s <= '9') {
    unsigned long v = 0;

    while (*p->s >= '0' && *p->s <= '9') {
      if (v > 100000000UL) {
        p->err = "number too large";
        return -1;
      }
      v = v * 10 + (*p->s - '0');
      p->s++;
    }
    n = node_new(p, OP_NUM, -1, -1, -1);
    p->pl->nodes[n].value = v;
    return n;
  }
  p->err = *p->s ? "unexpected character" : "unexpected end";
  return -1;
}

static int parse_unary(struct PoExprParser* p)
{
  int n;

  skip_ws(p);
  if (*p->s == '!' && p->s[1] != '=') {
    p->s++;
    if (++p->depth > PO_EXPR_DEPTH_MAX) {
      p->err = "too deeply nested";
      return -1;
    }
    n = parse_unary(p);
    p->depth--;
    if (n < 0)
      return -1;
    return node_new(p, OP_NOT, n, -1, -1);
  }
  return parse_primary(p);
}

/** A left-associative binary level.  \a ops is the list of operator
 * spellings, \a codes the matching OP_ values, \a next the level below.
 */
static int parse_binary(struct PoExprParser* p, const char* const* ops,
                        const enum PoOp* codes,
                        int (*next)(struct PoExprParser*))
{
  int left = next(p);

  if (left < 0)
    return -1;

  for (;;) {
    int i, right;

    skip_ws(p);
    for (i = 0; ops[i]; i++) {
      size_t len = strlen(ops[i]);

      if (!strncmp(p->s, ops[i], len)) {
        /* "<" must not match "<=", nor "=" the first half of "==". */
        if (len == 1 && p->s[1] == '=' && (*ops[i] == '<' || *ops[i] == '>'))
          continue;
        break;
      }
    }
    if (!ops[i])
      return left;
    p->s += strlen(ops[i]);
    right = next(p);
    if (right < 0)
      return -1;
    left = node_new(p, codes[i], left, right, -1);
  }
}

static int parse_mul(struct PoExprParser* p)
{
  static const char* const ops[] = { "*", "/", "%", 0 };
  static const enum PoOp codes[] = { OP_MUL, OP_DIV, OP_MOD };
  return parse_binary(p, ops, codes, parse_unary);
}

static int parse_add(struct PoExprParser* p)
{
  static const char* const ops[] = { "+", "-", 0 };
  static const enum PoOp codes[] = { OP_ADD, OP_SUB };
  return parse_binary(p, ops, codes, parse_mul);
}

static int parse_rel(struct PoExprParser* p)
{
  static const char* const ops[] = { "<=", ">=", "<", ">", 0 };
  static const enum PoOp codes[] = { OP_LE, OP_GE, OP_LT, OP_GT };
  return parse_binary(p, ops, codes, parse_add);
}

static int parse_eq(struct PoExprParser* p)
{
  static const char* const ops[] = { "==", "!=", 0 };
  static const enum PoOp codes[] = { OP_EQ, OP_NE };
  return parse_binary(p, ops, codes, parse_rel);
}

static int parse_and(struct PoExprParser* p)
{
  static const char* const ops[] = { "&&", 0 };
  static const enum PoOp codes[] = { OP_AND };
  return parse_binary(p, ops, codes, parse_eq);
}

static int parse_or(struct PoExprParser* p)
{
  static const char* const ops[] = { "||", 0 };
  static const enum PoOp codes[] = { OP_OR };
  return parse_binary(p, ops, codes, parse_and);
}

static int parse_ternary(struct PoExprParser* p)
{
  int cond, yes, no;

  if (++p->depth > PO_EXPR_DEPTH_MAX) {
    p->err = "too deeply nested";
    return -1;
  }

  cond = parse_or(p);
  if (cond < 0) {
    p->depth--;
    return -1;
  }

  skip_ws(p);
  if (*p->s != '?') {
    p->depth--;
    return cond;
  }
  p->s++;

  yes = parse_ternary(p);
  if (yes < 0) {
    p->depth--;
    return -1;
  }
  skip_ws(p);
  if (*p->s != ':') {
    p->err = "missing : in ?:";
    p->depth--;
    return -1;
  }
  p->s++;
  no = parse_ternary(p);
  p->depth--;
  if (no < 0)
    return -1;

  return node_new(p, OP_TERNARY, cond, yes, no);
}

struct PoPlural* po_plural_compile(const char* rule, unsigned int* nplurals,
                                   char* err, size_t errlen)
{
  struct PoExprParser p;
  struct PoPlural* pl;
  const char* s = rule;
  const char* expr = 0;
  unsigned int np = 0;
  int have_np = 0;

  if (err && errlen)
    *err = '\0';
  if (nplurals)
    *nplurals = 0;

  /* "nplurals=2; plural=(n != 1);" in either order, blanks allowed. */
  while (*s) {
    while (*s == ' ' || *s == '\t' || *s == ';')
      s++;
    if (!*s)
      break;
    if (!strncmp(s, "nplurals", 8)) {
      s += 8;
      while (*s == ' ' || *s == '\t')
        s++;
      if (*s != '=') {
        if (err && errlen)
          snprintf(err, errlen, "expected = after nplurals");
        return 0;
      }
      s++;
      while (*s == ' ' || *s == '\t')
        s++;
      if (*s < '0' || *s > '9') {
        if (err && errlen)
          snprintf(err, errlen, "nplurals is not a number");
        return 0;
      }
      np = 0;
      while (*s >= '0' && *s <= '9') {
        np = np * 10 + (*s - '0');
        s++;
        if (np > I18N_NPLURALS_MAX)
          break;
      }
      have_np = 1;
    } else if (!strncmp(s, "plural", 6)) {
      s += 6;
      while (*s == ' ' || *s == '\t')
        s++;
      if (*s != '=') {
        if (err && errlen)
          snprintf(err, errlen, "expected = after plural");
        return 0;
      }
      s++;
      expr = s;
      while (*s && *s != ';')
        s++;
    } else {
      if (err && errlen)
        snprintf(err, errlen, "unknown word in Plural-Forms");
      return 0;
    }
  }

  if (!have_np || !expr) {
    if (err && errlen)
      snprintf(err, errlen, "needs both nplurals and plural");
    return 0;
  }
  if (np < 1 || np > I18N_NPLURALS_MAX) {
    if (err && errlen)
      snprintf(err, errlen, "nplurals must be 1..%d", I18N_NPLURALS_MAX);
    return 0;
  }

  pl = (struct PoPlural*)MyCalloc(1, sizeof(struct PoPlural));
  pl->nplurals = np;

  memset(&p, 0, sizeof(p));
  p.s = expr;
  p.pl = pl;
  pl->root = parse_ternary(&p);
  if (pl->root >= 0) {
    skip_ws(&p);
    if (*p.s && *p.s != ';') {
      p.err = "trailing characters";
      pl->root = -1;
    }
  }
  if (pl->root < 0) {
    if (err && errlen)
      snprintf(err, errlen, "%s at \"%.20s\"", p.err ? p.err : "syntax error",
               p.s);
    po_plural_free(pl);
    return 0;
  }

  if (nplurals)
    *nplurals = np;
  return pl;
}

static unsigned long eval(const struct PoPlural* pl, int idx, unsigned long n)
{
  const struct PoNode* nd = &pl->nodes[idx];
  unsigned long a, b;

  switch (nd->op) {
  case OP_NUM:     return nd->value;
  case OP_N:       return n;
  case OP_NOT:     return !eval(pl, nd->kids[0], n);
  case OP_TERNARY:
    return eval(pl, nd->kids[0], n) ? eval(pl, nd->kids[1], n)
                                    : eval(pl, nd->kids[2], n);
  case OP_OR:      return eval(pl, nd->kids[0], n) || eval(pl, nd->kids[1], n);
  case OP_AND:     return eval(pl, nd->kids[0], n) && eval(pl, nd->kids[1], n);
  default:
    break;
  }

  a = eval(pl, nd->kids[0], n);
  b = eval(pl, nd->kids[1], n);
  switch (nd->op) {
  case OP_EQ:  return a == b;
  case OP_NE:  return a != b;
  case OP_LT:  return a < b;
  case OP_GT:  return a > b;
  case OP_LE:  return a <= b;
  case OP_GE:  return a >= b;
  case OP_ADD: return a + b;
  case OP_SUB: return a - b;
  case OP_MUL: return a * b;
  case OP_DIV: return b ? a / b : 0;
  case OP_MOD: return b ? a % b : 0;
  default:     return 0;
  }
}

unsigned int po_plural_eval(const struct PoPlural* rule, unsigned long n)
{
  unsigned long form;

  if (!rule || rule->root < 0)
    return n != 1;

  form = eval(rule, rule->root, n);
  if (form >= rule->nplurals)
    form = rule->nplurals - 1;
  return (unsigned int)form;
}

void po_plural_free(struct PoPlural* rule)
{
  if (!rule)
    return;
  MyFree(rule->nodes);
  MyFree(rule);
}
