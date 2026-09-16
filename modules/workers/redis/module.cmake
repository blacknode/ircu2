#
# modules/workers/redis/module.cmake
#
# The cache driver needs hiredis, which is not a dependency of the ircd and
# should not become one.  It is found here, in the module's own fragment,
# and the core's build files never learn that it exists.
#
# When it is missing the module is not built and the rest of the tree is
# unaffected: a server with no cache module is a perfectly ordinary server,
# and the Redis{} block simply sits unread in the configuration.  Nothing
# else changes, because the cache is never the truth -- see include/cache.h.
#
# Debian and Ubuntu:   apt install libhiredis-dev
# Red Hat and Fedora:  dnf install hiredis-devel
# FreeBSD:             pkg install hiredis
#
# See cmake/IrcuModules.cmake for the variables this may set.  Note that it
# must not call return(): this file is include()d from inside a function, and
# a return here would end the whole module scan rather than this fragment.

find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
  pkg_check_modules(IRCU_HIREDIS QUIET IMPORTED_TARGET hiredis)
endif()

if(NOT IRCU_HIREDIS_FOUND)
  find_path(IRCU_HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
  find_library(IRCU_HIREDIS_LIBRARY NAMES hiredis)
endif()

if(NOT IRCU_HIREDIS_FOUND
   AND (NOT IRCU_HIREDIS_INCLUDE_DIR OR NOT IRCU_HIREDIS_LIBRARY))
  set(IRCU_MODULE_SKIP "hiredis not found")
else()
  if(IRCU_HIREDIS_FOUND)
    list(APPEND IRCU_MODULE_LINK_LIBRARIES PkgConfig::IRCU_HIREDIS)
  else()
    list(APPEND IRCU_MODULE_LINK_LIBRARIES "${IRCU_HIREDIS_LIBRARY}")
    list(APPEND IRCU_MODULE_INCLUDE_DIRECTORIES "${IRCU_HIREDIS_INCLUDE_DIR}")
  endif()

  # The connections are dedicated workers, so this module needs the pthreads
  # the ircd already uses; asking for it here keeps the module linkable on
  # the platforms that want an explicit -pthread.
  find_package(Threads QUIET)
  if(Threads_FOUND)
    list(APPEND IRCU_MODULE_LINK_LIBRARIES Threads::Threads)
  endif()
endif()
