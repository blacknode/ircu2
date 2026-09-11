/* migration_t.c - Test the migration validator.
 *
 * migration_build() is the gate a module with migrations has to get past to
 * be loaded at all, so this covers what it lets through and, at more length,
 * what it does not: a name that is not a migration, a name with a character
 * that has no business in the table, a version with no down, a down with no
 * up, two files claiming one version, a gap in the numbering.
 *
 * Every rejection is checked for saying which file was wrong, because an
 * error an operator cannot act on is not much better than no error.
 */

#include "migration.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct Client;

/** Stub for the one thing ircd_snprintf.c reaches out of itself for.
 *
 * migration.c formats its error messages with ircd_snprintf(), whose %C
 * conversion can render a client's username.  No migration error names a
 * client, so this never runs.
 */
const char* visible_username(const struct Client* cptr)
{
  (void) cptr;
  return "";
}

/** Build a set from a NULL-terminated list of names, with dummy SQL. */
static struct MigrationSet* build(const char** names, const char** errstr)
{
  struct MigrationFile files[16];
  unsigned int i;

  for (i = 0; names[i]; i++) {
    files[i].mf_name = names[i];
    files[i].mf_sql = "select 1;";
  }
  files[i].mf_name = 0;
  files[i].mf_sql = 0;

  return migration_build("demo", files, errstr);
}

/** Assert that \a names is refused, and that the reason mentions \a needle. */
static void reject(const char* what, const char** names, const char* needle)
{
  const char* err = 0;
  struct MigrationSet* set = build(names, &err);

  assert(!set);
  assert(err != 0);

  if (needle && !strstr(err, needle)) {
    printf("FAIL - %s\n  wanted \"%s\" in: %s\n", what, needle, err);
    assert(0);
  }

  printf("ok - %s\n      (%s)\n", what, err);
}

static void test_accepts_a_good_set(void)
{
  const char* names[] = {
    "v2_add_score.down.sql", "v1_create_accounts.up.sql",
    "v1_create_accounts.down.sql", "v2_add_score.up.sql", 0
  };
  const char* err = 0;
  struct MigrationSet* set = build(names, &err);

  assert(set != 0);
  assert(!err);
  assert(set->ms_count == 2);
  assert(!strcmp(set->ms_module, "demo"));

  /* Sorted by version whatever order the directory listed them in. */
  assert(set->ms_list[0].mg_version == 1);
  assert(!strcmp(set->ms_list[0].mg_name, "create_accounts"));
  assert(set->ms_list[0].mg_up && set->ms_list[0].mg_down);
  assert(set->ms_list[1].mg_version == 2);
  assert(!strcmp(set->ms_list[1].mg_name, "add_score"));

  migration_free(set);

  printf("ok - a complete set is accepted and ordered by version\n");
}

static void test_no_migrations_is_not_an_error(void)
{
  struct MigrationFile empty[] = { { 0, 0 } };
  const char* err = "not reset";

  assert(!migration_build("demo", empty, &err));
  assert(!err);

  err = "not reset";
  assert(!migration_build("demo", 0, &err));
  assert(!err);

  printf("ok - a module with no migrations is not a module with bad ones\n");
}

static void test_name_format(void)
{
  const char* no_v[] = { "1_x.up.sql", "1_x.down.sql", 0 };
  const char* no_version[] = { "v_x.up.sql", "v_x.down.sql", 0 };
  const char* zero[] = { "v0_x.up.sql", "v0_x.down.sql", 0 };
  const char* leading_zero[] = { "v01_x.up.sql", "v01_x.down.sql", 0 };
  const char* no_name[] = { "v1_.up.sql", "v1_.down.sql", 0 };
  const char* no_underscore[] = { "v1x.up.sql", "v1x.down.sql", 0 };
  const char* sideways[] = { "v1_x.sideways.sql", "v1_x.down.sql", 0 };
  const char* not_sql[] = { "v1_x.up.txt", "v1_x.down.sql", 0 };
  const char* readme[] = { "README", "v1_x.up.sql", "v1_x.down.sql", 0 };

  reject("a name with no v is refused", no_v, "1_x.up.sql");
  reject("a v with no number is refused", no_version, "v_x.up.sql");
  reject("v0 is refused; versions start at 1", zero, "v0_x.up.sql");
  reject("a leading zero is refused", leading_zero, "v01_x.up.sql");
  reject("an empty migration name is refused", no_name, "v1_.up.sql");
  reject("a missing underscore is refused", no_underscore, "v1x.up.sql");
  reject("a direction that is not up or down is refused", sideways,
         "v1_x.sideways.sql");
  reject("a suffix that is not .sql is refused", not_sql, "v1_x.up.txt");

  /* There is no "other files are ignored" rule: a stray file in
   * migrations/ is a mistake, and saying so beats guessing.
   */
  reject("a file that is not a migration at all is refused", readme,
         "README");
}

static void test_name_charset(void)
{
  const char* dash[] = { "v1_add-score.up.sql", "v1_add-score.down.sql", 0 };
  const char* dot[] = { "v1_add.score.up.sql", "v1_add.score.down.sql", 0 };
  const char* space[] = { "v1_add score.up.sql", "v1_add score.down.sql", 0 };
  const char* quote[] = { "v1_add'score.up.sql", "v1_add'score.down.sql", 0 };
  const char* good[] = { "v1_Add_Score_2.up.sql", "v1_Add_Score_2.down.sql",
                         0 };
  const char* err = 0;
  struct MigrationSet* set;

  reject("a dash in the name is refused", dash, "v1_add-score.up.sql");
  reject("a dot in the name is refused", dot, "v1_add.score.up.sql");
  reject("a space in the name is refused", space, "v1_add score.up.sql");
  reject("a quote in the name is refused", quote, "v1_add'score.up.sql");

  set = build(good, &err);
  assert(set != 0);
  assert(!strcmp(set->ms_list[0].mg_name, "Add_Score_2"));
  migration_free(set);

  printf("ok - letters, digits and underscores are allowed\n");
}

static void test_pairing(void)
{
  const char* up_only[] = { "v1_x.up.sql", 0 };
  const char* down_only[] = { "v1_x.down.sql", 0 };
  const char* half[] = {
    "v1_x.up.sql", "v1_x.down.sql", "v2_y.up.sql", 0
  };

  reject("an up with no down is refused", up_only, "no down file");
  reject("a down with no up is refused", down_only, "no up file");
  reject("a later version missing its down is refused", half, "v2_y");
}

static void test_duplicates_and_gaps(void)
{
  const char* twice[] = {
    "v1_x.up.sql", "v1_x.down.sql", "v1_x.up.sql", 0
  };
  const char* renamed[] = {
    "v1_x.up.sql", "v1_x.down.sql", "v1_y.up.sql", "v1_y.down.sql", 0
  };
  const char* gap[] = {
    "v1_x.up.sql", "v1_x.down.sql", "v3_z.up.sql", "v3_z.down.sql", 0
  };
  const char* not_one[] = { "v2_x.up.sql", "v2_x.down.sql", 0 };

  reject("the same file twice is refused", twice, "second up");
  reject("one version under two names is refused", renamed, "one name");
  reject("a gap in the numbering is refused", gap, "v2 is missing");
  reject("a set that does not start at v1 is refused", not_one,
         "v1 is missing");
}

static void test_reserved_name(void)
{
  assert(migration_reserved_name("core"));
  assert(migration_reserved_name("CORE"));
  assert(!migration_reserved_name("corey"));
  assert(!migration_reserved_name("postgres"));
  assert(!migration_reserved_name(0));

  printf("ok - \"core\" is reserved, in any case\n");
}

static void test_long_name(void)
{
  char up[MIGRATION_NAME_LEN + 32];
  char down[MIGRATION_NAME_LEN + 32];
  const char* names[3];
  char name[MIGRATION_NAME_LEN + 2];
  const char* err = 0;
  struct MigrationSet* set;
  unsigned int i;

  for (i = 0; i < MIGRATION_NAME_LEN; i++)
    name[i] = 'a';
  name[MIGRATION_NAME_LEN] = '\0';

  sprintf(up, "v1_%s.up.sql", name);
  sprintf(down, "v1_%s.down.sql", name);
  names[0] = up;
  names[1] = down;
  names[2] = 0;

  set = build(names, &err);
  assert(set != 0);
  assert(strlen(set->ms_list[0].mg_name) == MIGRATION_NAME_LEN);
  migration_free(set);

  printf("ok - a name of exactly %u characters fits\n",
         (unsigned int) MIGRATION_NAME_LEN);

  /* One more than fits is refused rather than quietly truncated: two names
   * that differ past the limit would otherwise become one.
   */
  name[MIGRATION_NAME_LEN] = 'a';
  name[MIGRATION_NAME_LEN + 1] = '\0';
  sprintf(up, "v1_%s.up.sql", name);
  sprintf(down, "v1_%s.down.sql", name);

  reject("a name one character too long is refused", names, "not a migration");
}

int main(void)
{
  test_accepts_a_good_set();
  test_no_migrations_is_not_an_error();
  test_name_format();
  test_name_charset();
  test_pairing();
  test_duplicates_and_gaps();
  test_reserved_name();
  test_long_name();

  printf("migration_t: all tests passed\n");

  return 0;
}
