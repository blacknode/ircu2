#ifndef INCLUDED_migration_h
#define INCLUDED_migration_h
/*
 * IRC - Internet Relay Chat, include/migration.h
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
 * @brief Versioned SQL migrations, per module.
 *
 * A module that needs tables of its own ships the SQL that makes them.  It
 * puts the files in a @c migrations/ subdirectory, the build compiles them
 * into the shared object, the server checks them when the module is loaded,
 * and an operator applies them by hand with @c /MODULE @c MIGRATION.
 *
 * @section mig_layout What a module ships
 *
 * A module with migrations is a module built from a directory -- a project,
 * not a single @c .c file -- and the directory holds:
 *
 * @code
 *   modules/<type>/<name>/migrations/v1_create_accounts.up.sql
 *   modules/<type>/<name>/migrations/v1_create_accounts.down.sql
 *   modules/<type>/<name>/migrations/v2_add_score.up.sql
 *   modules/<type>/<name>/migrations/v2_add_score.down.sql
 * @endcode
 *
 * The rules are checked at load time, and a module that breaks any of them
 * does not load -- the operator who asked for it is told which file is
 * wrong:
 *
 *   - the name is <tt>v\<N\>_\<name\>.\<up|down\>.sql</tt>, and nothing else;
 *   - @c \<name\> is letters, digits and underscores, and nothing else;
 *   - @c \<N\> starts at 1 and runs without gaps or repeats;
 *   - every @c up has a @c down with the same version @em and the same name.
 *
 * The last one is not bureaucracy.  A migration nobody can undo is a
 * migration nobody can safely apply to a live network.
 *
 * @section mig_never Nothing runs by itself
 *
 * A module's migrations are never applied and never reverted automatically:
 * not when it is loaded, not when it is reloaded, not when the server
 * starts.  Loading a module tells the server what SQL exists; an operator
 * decides whether the database should run it, and when.  A schema change on
 * a live network is an operational decision and it stays one.
 *
 * The one exception is the @b core set (ircd/migrations/), which creates and
 * maintains the @c migrations table itself.  That one runs at start-up,
 * because it is the table every other migration is recorded in and there is
 * nothing to decide about it.  Its module name is @c "core", which is why no
 * module may be called that.
 *
 * @section mig_table The record
 *
 * Every applied migration is a row in @c migrations, and reverting one
 * deletes its row.  What is in the table is what is applied: the server does
 * not keep a second opinion anywhere.
 *
 * See doc/readme.migrations.
 */

#ifndef INCLUDED_sys_types_h
#include <sys/types.h>
#define INCLUDED_sys_types_h
#endif

struct Client;
struct ModuleHandle;

/** Longest @c \<migration_name\>, without the NUL. */
#define MIGRATION_NAME_LEN 64

/** Longest module name a migration may be recorded under, without the NUL. */
#define MIGRATION_MODULE_LEN 64

/** The module name the server's own migrations are recorded under.
 *
 * No module may declare this name, and no module may be loaded from a file
 * called this: the core's rows in the table have to be unambiguously the
 * core's.
 */
#define MIGRATION_CORE "core"

/** Most migrations one module may ship.
 *
 * A sanity limit rather than a design one: a module with more than this
 * many versions has something else wrong with it.
 */
#define MIGRATION_MAX 256

/** One @c .sql file, exactly as the build found it.
 *
 * The build embeds an array of these and does not look inside the names;
 * migration_build() is what parses them, so that a malformed name is an
 * error an operator sees rather than one that stops a build.
 */
struct MigrationFile {
  const char* mf_name;   /**< File name, e.g. "v1_create_accounts.up.sql". */
  const char* mf_sql;    /**< Its entire contents. */
};

/** One version, with both of its halves. */
struct Migration {
  unsigned int mg_version;                /**< The @c N in @c vN. */
  char         mg_name[MIGRATION_NAME_LEN + 1]; /**< The name after it. */
  const char*  mg_up;                     /**< SQL that applies it. */
  const char*  mg_down;                   /**< SQL that reverts it. */
};

/** Everything one module ships, once it has been checked.
 *
 * Ordered by version, which after validation means index @c i is version
 * @c i+1.
 */
struct MigrationSet {
  char              ms_module[MIGRATION_MODULE_LEN + 1]; /**< Whose it is. */
  unsigned int      ms_count;      /**< Versions in #ms_list. */
  struct Migration* ms_list;       /**< #ms_count of them, v1 first. */
};

/*
 * Validation.  module.c calls this while loading; nothing else needs it.
 */

/** Check an embedded set of files and turn it into a #MigrationSet.
 *
 * Enforces every rule in @ref mig_layout.  The error message names the file
 * that broke the rule, because that is the only thing the operator can act
 * on.
 *
 * @param[in] module Module the migrations belong to.
 * @param[in] files NULL-terminated array from the build, or NULL for a
 *   module with no migrations at all.
 * @param[out] errstr Receives the reason when this returns NULL.  Points at
 *   a static buffer, good until the next call.
 * @return The set, or NULL.  NULL with @a *errstr NULL means the module
 *   simply has no migrations, which is not an error.
 */
extern struct MigrationSet* migration_build(const char* module,
                                            const struct MigrationFile* files,
                                            const char** errstr);

/** Release a set from migration_build().
 * @param[in] set Set to release, or NULL.
 */
extern void migration_free(struct MigrationSet* set);

/** Non-zero if \a name is one no module may use.
 * @param[in] name Name to check.
 */
extern int migration_reserved_name(const char* name);

/*
 * The core's own migrations.
 */

/** Apply whatever of the core set is not applied yet.  Main thread.
 *
 * Called from main() once the workers are up, and again whenever a database
 * driver registers -- a driver loaded by hand with /MODULE LOAD arrives long
 * after start-up, and the table still has to exist.  Does nothing when there
 * is no driver, when there is no @c Database{} block, or when it is already
 * running.
 */
extern void migration_core_start(void);

/*
 * Operator commands.  m_module.c calls these; they answer the client
 * themselves, asynchronously, because every one of them is a database round
 * trip.
 */

/** Show every row of the @c migrations table.
 * @param[in] sptr Operator to answer.
 */
extern void migration_cmd_list(struct Client* sptr);

/** Show what \a mod ships and which of it is applied.
 * @param[in] sptr Operator to answer.
 * @param[in] mod Module to report on.
 */
extern void migration_cmd_status(struct Client* sptr, struct ModuleHandle* mod);

/** Apply \a mod's pending migrations, in order, up to \a upto.
 *
 * Stops at the first one that fails and says which.  Each is its own
 * transaction: the ones before a failure stay applied, which is what makes
 * it safe to fix the broken one and run this again.
 * @param[in] sptr Operator to answer, and to record as @c exec_by.
 * @param[in] mod Module to migrate.
 * @param[in] upto Highest version to apply, or 0 for all of them.
 */
extern void migration_cmd_apply(struct Client* sptr, struct ModuleHandle* mod,
                                unsigned int upto);

/** Revert \a mod's applied migrations, newest first, down to \a downto.
 *
 * With \a downto zero this reverts exactly one -- the newest applied.
 * Reverting destroys whatever the migration created, so the default is one
 * step and not "all of them".
 * @param[in] sptr Operator to answer, and to record as @c exec_by.
 * @param[in] mod Module to migrate.
 * @param[in] downto Lowest version to revert, or 0 for one step.
 */
extern void migration_cmd_revert(struct Client* sptr, struct ModuleHandle* mod,
                                 unsigned int downto);

/** Release everything; main() only, at exit. */
extern void migration_shutdown(void);

#endif /* INCLUDED_migration_h */
