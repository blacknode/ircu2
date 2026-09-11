/*
 * IRC - Internet Relay Chat, ircd/migration.c
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
 * @brief Deciding whether a module's migrations are worth loading.
 *
 * migration_build() takes the array of files the build embedded and either
 * produces an ordered #MigrationSet or says which file is wrong.  It runs
 * while a module is being loaded and its answer decides whether the module
 * loads at all.
 *
 * Everything migration.h promises is enforced here, and all of it is
 * enforced here rather than in the build: a module whose migrations are
 * malformed should be an error the operator reads when they type
 * /MODULE LOAD, not a build failure on somebody else's machine.  The build
 * embeds whatever it finds and asks no questions; see
 * cmake/GenerateMigrations.cmake.
 *
 * Running migrations is migration_run.c.  Keeping the two apart is what
 * lets this half be tested without a database, a client or an event loop --
 * see ircd/test/migration_t.c.
 */
#include "config.h"

#include "migration.h"

#include "ircd_alloc.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/** Buffer behind the @a errstr every failure path returns. */
static char migration_error[512];

int migration_reserved_name(const char* name)
{
  return name && 0 == ircd_strcmp(name, MIGRATION_CORE);
}

/** Pull the version, name and direction out of one file name.
 *
 * @param[in] filename Name to parse.
 * @param[out] version Receives the version.
 * @param[out] name Receives the migration name, NUL-terminated.
 * @param[out] is_up Receives non-zero for an @c up file.
 * @return Non-zero when \a filename follows the rules.
 */
static int migration_parse(const char* filename, unsigned int* version,
                           char* name, int* is_up)
{
  const char* p = filename;
  const char* start;
  unsigned long number;
  size_t len;

  if (*p++ != 'v')
    return 0;

  /* The version: digits, at least one, and no leading zero -- "v01" and
   * "v1" would be two spellings of one version, and the table can only
   * hold one of them.
   */
  if (*p < '1' || *p > '9')
    return 0;

  start = p;
  while (*p >= '0' && *p <= '9')
    p++;
  if (p - start > 9)
    return 0;                   /* absurd, and would overflow below */

  number = strtoul(start, 0, 10);

  if (*p++ != '_')
    return 0;

  /* The name: letters, digits and underscores.  Nothing else, because this
   * goes in the table and comes back out into a reply to an operator.
   */
  start = p;
  while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
         || (*p >= '0' && *p <= '9') || *p == '_')
    p++;

  len = (size_t) (p - start);
  if (len < 1 || len > MIGRATION_NAME_LEN)
    return 0;

  if (0 == strcmp(p, ".up.sql"))
    *is_up = 1;
  else if (0 == strcmp(p, ".down.sql"))
    *is_up = 0;
  else
    return 0;

  memcpy(name, start, len);
  name[len] = '\0';

  *version = (unsigned int) number;

  return 1;
}

/** Find or make room for \a version in \a set.
 *
 * @param[in,out] set Set being built.
 * @param[in] version Version wanted.
 * @return The slot, or NULL if there is no room left.
 */
static struct Migration* migration_slot(struct MigrationSet* set,
                                        unsigned int version)
{
  unsigned int i;

  for (i = 0; i < set->ms_count; i++)
    if (set->ms_list[i].mg_version == version)
      return &set->ms_list[i];

  if (set->ms_count >= MIGRATION_MAX)
    return 0;

  return &set->ms_list[set->ms_count++];
}

/** Order two migrations by version, for qsort(). */
static int migration_compare(const void* a, const void* b)
{
  const struct Migration* ma = (const struct Migration*) a;
  const struct Migration* mb = (const struct Migration*) b;

  if (ma->mg_version < mb->mg_version)
    return -1;
  if (ma->mg_version > mb->mg_version)
    return 1;

  return 0;
}

struct MigrationSet* migration_build(const char* module,
                                     const struct MigrationFile* files,
                                     const char** errstr)
{
  struct MigrationSet* set;
  unsigned int i;

  assert(0 != errstr);
  *errstr = 0;

  /* No migrations at all is the ordinary case, and not an error. */
  if (!files || !files[0].mf_name)
    return 0;

  if (!module || !*module || strlen(module) > MIGRATION_MODULE_LEN) {
    *errstr = "the module name is unusable as a migration owner";
    return 0;
  }

  set = (struct MigrationSet*) MyCalloc(1, sizeof(*set));
  set->ms_list = (struct Migration*)
    MyCalloc(MIGRATION_MAX, sizeof(struct Migration));
  ircd_strncpy(set->ms_module, module, MIGRATION_MODULE_LEN);

  for (i = 0; files[i].mf_name; i++) {
    char name[MIGRATION_NAME_LEN + 1];
    struct Migration* migration;
    unsigned int version;
    int is_up;

    if (!migration_parse(files[i].mf_name, &version, name, &is_up)) {
      ircd_snprintf(0, migration_error, sizeof(migration_error),
                    "migrations/%s is not a migration: the name must be "
                    "v<N>_<name>.up.sql or v<N>_<name>.down.sql, with <N> "
                    "starting at 1 and <name> made of letters, digits and "
                    "underscores", files[i].mf_name);
      *errstr = migration_error;
      migration_free(set);
      return 0;
    }

    if (!(migration = migration_slot(set, version))) {
      ircd_snprintf(0, migration_error, sizeof(migration_error),
                    "too many migrations: at most %u are allowed",
                    (unsigned int) MIGRATION_MAX);
      *errstr = migration_error;
      migration_free(set);
      return 0;
    }

    /* A version already seen must agree with itself about its name: two
     * files claiming v3 under different names are two migrations wearing
     * one number, and only one of them could ever be recorded.
     */
    if (migration->mg_version && 0 != strcmp(migration->mg_name, name)) {
      ircd_snprintf(0, migration_error, sizeof(migration_error),
                    "migrations/%s and v%u_%s.* are both version %u; a "
                    "version is one migration and has one name",
                    files[i].mf_name, version, migration->mg_name, version);
      *errstr = migration_error;
      migration_free(set);
      return 0;
    }

    if ((is_up && migration->mg_up) || (!is_up && migration->mg_down)) {
      ircd_snprintf(0, migration_error, sizeof(migration_error),
                    "migrations/%s is a second %s for version %u",
                    files[i].mf_name, is_up ? "up" : "down", version);
      *errstr = migration_error;
      migration_free(set);
      return 0;
    }

    migration->mg_version = version;
    ircd_strncpy(migration->mg_name, name, MIGRATION_NAME_LEN);

    if (is_up)
      migration->mg_up = files[i].mf_sql;
    else
      migration->mg_down = files[i].mf_sql;
  }

  qsort(set->ms_list, set->ms_count, sizeof(struct Migration),
        migration_compare);

  for (i = 0; i < set->ms_count; i++) {
    struct Migration* migration = &set->ms_list[i];

    /* Versions run 1..N.  A gap means a migration was deleted rather than
     * reverted, and the ones after it would apply out of order on a server
     * that had never seen the missing one.
     */
    if (migration->mg_version != i + 1) {
      ircd_snprintf(0, migration_error, sizeof(migration_error),
                    "migration versions must run from v1 with no gaps; "
                    "v%u is missing", i + 1);
      *errstr = migration_error;
      migration_free(set);
      return 0;
    }

    /* The pairing rule.  A migration that cannot be undone cannot honestly
     * be offered to an operator to apply.
     */
    if (!migration->mg_up || !migration->mg_down) {
      ircd_snprintf(0, migration_error, sizeof(migration_error),
                    "migrations/v%u_%s has no %s file; every up needs its "
                    "down and every down needs its up",
                    migration->mg_version, migration->mg_name,
                    migration->mg_up ? "down" : "up");
      *errstr = migration_error;
      migration_free(set);
      return 0;
    }
  }

  return set;
}

void migration_free(struct MigrationSet* set)
{
  if (!set)
    return;

  MyFree(set->ms_list);
  MyFree(set);
}

