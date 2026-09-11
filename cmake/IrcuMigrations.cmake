#
# Building migrations into the thing that owns them.
#
# A module with migrations is a module built from a directory -- a project,
# not a single file -- with a migrations/ subdirectory:
#
#   modules/<type>/<name>/migrations/v1_<migration_name>.up.sql
#   modules/<type>/<name>/migrations/v1_<migration_name>.down.sql
#   modules/<type>/<name>/migrations/v2_...
#
# Those .sql files are never copied: not into the build tree, not into the
# install tree, and not as module resources.  They are compiled into the
# shared object as string literals, which is what lets a module carry its
# migrations into a container with no source tree, and what makes it
# impossible for the SQL beside a module to be a different version from the
# code inside it.
#
# The ircd does the same with its own migrations (ircd/migrations/), which
# are the ones that create and maintain the "migrations" table itself.
#
# Nothing here validates the files.  The rules -- the v<N>_<name>.<up|down>.sql
# spelling, an up for every down, versions running 1..N with no gaps -- are
# checked when the module is loaded, so the operator who typed /MODULE LOAD
# is told what is wrong with it.  See ircd/migration.c.

# ircu_add_migrations(<label> <migrations_dir> <symbol> <generated_var>)
#
# Generates the C source embedding every .sql in <migrations_dir> as the
# array <symbol>, and returns its path in <generated_var> for the caller to
# add to its target's sources.  <label> only names the thing in the build's
# progress message.  Regenerates whenever a .sql file changes, appears or
# disappears.
function(ircu_add_migrations label dir symbol generated_var)
  # Named after the owner, not after the symbol: every module's embedded set
  # is called ircu_module_migrations, and two modules generated into one
  # directory would otherwise fight over the same file.
  set(output "${CMAKE_CURRENT_BINARY_DIR}/${label}_${symbol}.c")

  # CONFIGURE_DEPENDS so that adding a migration is picked up by a build,
  # the same way adding a source is.
  file(GLOB sql_files CONFIGURE_DEPENDS "${dir}/*.sql")

  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}"
            -DIRCU_MIGRATIONS_DIR=${dir}
            -DIRCU_SYMBOL=${symbol}
            -DIRCU_OUTPUT=${output}
            -P "${PROJECT_SOURCE_DIR}/cmake/GenerateMigrations.cmake"
    DEPENDS ${sql_files}
            "${PROJECT_SOURCE_DIR}/cmake/GenerateMigrations.cmake"
    COMMENT "Embedding migrations for ${label}"
    VERBATIM)

  set(${generated_var} "${output}" PARENT_SCOPE)
endfunction()
