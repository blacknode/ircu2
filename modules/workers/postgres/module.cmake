#
# modules/workers/postgres/module.cmake
#
# The PostgreSQL driver is the one module in this tree with dependencies of
# its own: libpq to talk to the database, and jansson to hand the rows back
# as the json_t that db.h promises.  Neither is a dependency of the ircd, and
# neither should become one -- so they are found here, in the module's own
# fragment, and the core's build files never learn that they exist.
#
# When either is missing the module is not built and the rest of the tree is
# unaffected: a server with no database module is a perfectly ordinary
# server, and the Database{} block simply sits unread in the configuration.
#
# Debian and Ubuntu:   apt install libpq-dev libjansson-dev
# Red Hat and Fedora:  dnf install libpq-devel jansson-devel
# FreeBSD:             pkg install postgresql16-client jansson
#
# See cmake/IrcuModules.cmake for the variables this may set.  Note that it
# must not call return(): this file is include()d from inside a function, and
# a return here would end the whole module scan rather than this fragment.

find_package(PkgConfig QUIET)

# Both libraries ship a .pc file.  Where pkg-config cannot find one -- a Red
# Hat system with only libpq-devel installed, say -- look for the header and
# the library by hand instead.
if(PkgConfig_FOUND)
  pkg_check_modules(IRCU_PG QUIET IMPORTED_TARGET libpq)
  pkg_check_modules(IRCU_JANSSON QUIET IMPORTED_TARGET jansson)
endif()

if(NOT IRCU_PG_FOUND)
  find_path(IRCU_PG_INCLUDE_DIR libpq-fe.h PATH_SUFFIXES postgresql pgsql)
  find_library(IRCU_PG_LIBRARY NAMES pq)
endif()

if(NOT IRCU_JANSSON_FOUND)
  find_path(IRCU_JANSSON_INCLUDE_DIR jansson.h)
  find_library(IRCU_JANSSON_LIBRARY NAMES jansson)
endif()

set(_pg_missing "")
if(NOT IRCU_PG_FOUND AND (NOT IRCU_PG_INCLUDE_DIR OR NOT IRCU_PG_LIBRARY))
  list(APPEND _pg_missing "libpq")
endif()
if(NOT IRCU_JANSSON_FOUND
   AND (NOT IRCU_JANSSON_INCLUDE_DIR OR NOT IRCU_JANSSON_LIBRARY))
  list(APPEND _pg_missing "jansson")
endif()

if(_pg_missing)
  string(REPLACE ";" " and " _pg_missing_text "${_pg_missing}")
  set(IRCU_MODULE_SKIP "${_pg_missing_text} not found")
else()
  if(IRCU_PG_FOUND)
    list(APPEND IRCU_MODULE_LINK_LIBRARIES PkgConfig::IRCU_PG)
  else()
    list(APPEND IRCU_MODULE_LINK_LIBRARIES "${IRCU_PG_LIBRARY}")
    list(APPEND IRCU_MODULE_INCLUDE_DIRECTORIES "${IRCU_PG_INCLUDE_DIR}")
  endif()

  if(IRCU_JANSSON_FOUND)
    list(APPEND IRCU_MODULE_LINK_LIBRARIES PkgConfig::IRCU_JANSSON)
  else()
    list(APPEND IRCU_MODULE_LINK_LIBRARIES "${IRCU_JANSSON_LIBRARY}")
    list(APPEND IRCU_MODULE_INCLUDE_DIRECTORIES
      "${IRCU_JANSSON_INCLUDE_DIR}")
  endif()

  # The connection threads are dedicated workers, so this module needs the
  # pthreads the ircd already uses; asking for it here keeps the module
  # linkable on the platforms that want an explicit -pthread.
  find_package(Threads QUIET)
  if(Threads_FOUND)
    list(APPEND IRCU_MODULE_LINK_LIBRARIES Threads::Threads)
  endif()
endif()
