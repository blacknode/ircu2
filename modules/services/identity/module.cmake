#
# modules/services/identity/module.cmake
#
# The identity module goes to the database through db.h and to the cache
# through cache.h, so it needs neither libpq nor hiredis -- both of those
# belong to the drivers.  What it does need is jansson, because that is the
# type db.h hands rows back in and the type the cache values are written
# in, and the ircd does not link against it.
#
# Debian and Ubuntu:   apt install libjansson-dev
# Red Hat and Fedora:  dnf install jansson-devel
# FreeBSD:             pkg install jansson
#
# Without it the module is not built and the rest of the tree is unaffected;
# a server with no identity module is a server where nobody can identify,
# which is exactly what this tree was before phase 1.
#
# Must not call return(): this file is include()d from inside a function.

find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
  pkg_check_modules(IRCU_ID_JANSSON QUIET IMPORTED_TARGET jansson)
endif()

if(NOT IRCU_ID_JANSSON_FOUND)
  find_path(IRCU_ID_JANSSON_INCLUDE_DIR jansson.h)
  find_library(IRCU_ID_JANSSON_LIBRARY NAMES jansson)
endif()

if(NOT IRCU_ID_JANSSON_FOUND
   AND (NOT IRCU_ID_JANSSON_INCLUDE_DIR OR NOT IRCU_ID_JANSSON_LIBRARY))
  set(IRCU_MODULE_SKIP "jansson not found")
else()
  if(IRCU_ID_JANSSON_FOUND)
    list(APPEND IRCU_MODULE_LINK_LIBRARIES PkgConfig::IRCU_ID_JANSSON)
  else()
    list(APPEND IRCU_MODULE_LINK_LIBRARIES "${IRCU_ID_JANSSON_LIBRARY}")
    list(APPEND IRCU_MODULE_INCLUDE_DIRECTORIES
      "${IRCU_ID_JANSSON_INCLUDE_DIR}")
  endif()
endif()
