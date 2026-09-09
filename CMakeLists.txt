#
# IRC - Internet Relay Chat, CMakeLists.txt
#
# Top-level build definition for ircu2.  This replaces the historical
# GNU autotools (configure.ac / Makefile.am) build.
#
# Quick start:
#   cmake -B build -DIRCU_DOMAIN=example.com
#   cmake --build build -j
#   ctest --test-dir build
#   cmake --install build
#
# Every historical `configure` switch has an IRCU_* cache variable
# counterpart; see cmake/IrcuOptions.cmake and INSTALL.
#

cmake_minimum_required(VERSION 3.16)

# The version here is the single source of truth for the whole tree; the
# BASE_VERSION / MAJOR_PROTOCOL / RELEASE / PATCHLEVEL macros consumed by the
# server are derived from it in cmake/IrcuVersion.cmake.
project(ircu2
  VERSION 10.12.19
  DESCRIPTION "Undernet IRC server (ircu)"
  HOMEPAGE_URL "https://github.com/UndernetIRC/ircu2"
  LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Guard against in-source builds.  The autotools build tolerated them, but a
# stray CMakeCache.txt in the source tree breaks every later out-of-tree build.
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are not supported.  Run cmake from a separate directory, "
    "e.g. `cmake -B build`.  Remove the CMakeCache.txt and CMakeFiles/ that "
    "this attempt just created in the source tree.")
endif()

# ---------------------------------------------------------------------------
# Language level and default build type
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Match `configure`'s default of -g -O2 when the user picks nothing.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
    "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)
endif()

# ircu decides assertions itself, through IRCU_ENABLE_ASSERTS -> NDEBUG in
# config.h.  CMake's Release-flavoured build types add -DNDEBUG of their own,
# which would silently override that switch (and strip the assertions out of
# the unit tests, which never include config.h), so take it back out.
foreach(_cfg RELEASE RELWITHDEBINFO MINSIZEREL)
  string(REPLACE "-DNDEBUG" "" CMAKE_C_FLAGS_${_cfg} "${CMAKE_C_FLAGS_${_cfg}}")
  string(STRIP "${CMAKE_C_FLAGS_${_cfg}}" CMAKE_C_FLAGS_${_cfg})
endforeach()

# The chroot directory has to be settled before the install prefix, because
# it decides where that prefix goes.  IrcuPaths.cmake does the rest of the
# work with it.  It is empty by default: a root directory rewrites every path
# compiled into the server to be relative to it, which is only correct for a
# server that will actually run inside that jail.
set(IRCU_CHROOT "" CACHE STRING
  "Directory the server will be chrooted into (empty for none)")
string(REGEX REPLACE "/+$" "" IRCU_CHROOT "${IRCU_CHROOT}")

# Where the install tree goes.
#
# ircu historically installed under $HOME; it defaults to the source tree
# instead, which keeps a build self-contained -- the binary, its data
# directory and its modules all land next to the sources, and the paths
# compiled into the server are real absolute paths that work without a
# wrapper.  With a chroot the whole tree belongs inside it, since nothing
# above the new root is reachable from within.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  if(IRCU_CHROOT)
    set(CMAKE_INSTALL_PREFIX "${IRCU_CHROOT}" CACHE PATH
      "Installation prefix" FORCE)
  else()
    set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}" CACHE PATH
      "Installation prefix" FORCE)
  endif()
  message(STATUS "Install prefix defaulted to ${CMAKE_INSTALL_PREFIX}")
endif()

# A chroot added to an existing build directory cannot move a prefix that is
# already cached, so say what to do rather than failing later on a path that
# the user never set.
if(IRCU_CHROOT)
  string(FIND "${CMAKE_INSTALL_PREFIX}" "${IRCU_CHROOT}" _ircu_prefix_pos)
  if(NOT _ircu_prefix_pos EQUAL 0)
    message(WARNING
      "The install prefix ${CMAKE_INSTALL_PREFIX} is outside the root "
      "directory ${IRCU_CHROOT}.  Paths derived from it cannot be made "
      "relative to the root directory; pass "
      "-DCMAKE_INSTALL_PREFIX=${IRCU_CHROOT} (or configure a fresh build "
      "directory) if that is not deliberate.")
  endif()
endif()

# Emit compile_commands.json so clangd, ccls and the editors built on them
# work in this tree without extra setup.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

include(IrcuVersion)
include(ConfigureChecks)   # probe the system: headers, types, functions
include(IrcuOptions)       # user-facing knobs (the old --enable/--with flags)
include(IrcuTLS)           # pick and locate the TLS backend
include(IrcuPaths)         # DPATH/CPATH/LPATH/SPATH, honouring --with-chroot

# ---------------------------------------------------------------------------
# config.h
# ---------------------------------------------------------------------------
configure_file(cmake/config.h.cmake.in "${PROJECT_BINARY_DIR}/config.h" @ONLY)

# Everything compiled here sees the generated config.h and the public headers.
add_library(ircu_config INTERFACE)
target_include_directories(ircu_config INTERFACE
  "${PROJECT_BINARY_DIR}"
  "${PROJECT_SOURCE_DIR}/include")
target_compile_definitions(ircu_config INTERFACE IRCU2_BUILD)

if(IRCU_ENABLE_WARNINGS)
  target_compile_options(ircu_config INTERFACE -Wall)
endif()
if(IRCU_ENABLE_PEDANTIC)
  target_compile_options(ircu_config INTERFACE -pedantic)
endif()
if(IRCU_ENABLE_PROFILE)
  target_compile_options(ircu_config INTERFACE -pg)
  target_link_options(ircu_config INTERFACE -pg)
endif()
if(IRCU_LEAK_DETECT)
  target_compile_definitions(ircu_config INTERFACE MDEBUG)
  target_link_libraries(ircu_config INTERFACE gc)
  if(NOT IRCU_LEAK_DETECT STREQUAL "yes")
    target_link_directories(ircu_config INTERFACE "${IRCU_LEAK_DETECT}")
  endif()
endif()

enable_testing()

add_subdirectory(ircd)
add_subdirectory(ircd/test)
add_subdirectory(modules)

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "ircu is now hopefully configured for your system.")
message(STATUS "")
message(STATUS "  Host system:         ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Prefix:              ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Build type:          ${CMAKE_BUILD_TYPE}")
message(STATUS "  Asserts:             ${IRCU_ENABLE_ASSERTS}")
message(STATUS "  Warnings:            ${IRCU_ENABLE_WARNINGS}")
message(STATUS "  Debug:               ${IRCU_ENABLE_DEBUG}")
message(STATUS "  Profile:             ${IRCU_ENABLE_PROFILE}")
message(STATUS "  Owner/mode:          ${IRCU_OWNER}.${IRCU_GROUP} (${IRCU_MODE})")
message(STATUS "  Chroot:              ${IRCU_CHROOT}")
message(STATUS "")
message(STATUS "  Domain:              ${IRCU_DOMAIN}")
message(STATUS "  DPath:               ${IRCU_DPATH}")
message(STATUS "  CPath:               ${IRCU_CPATH}")
message(STATUS "  LPath:               ${IRCU_LPATH}")
message(STATUS "  MPath:               ${IRCU_MPATH}")
message(STATUS "  Maximum connections: ${IRCU_MAXCON}")
message(STATUS "  TLS implementation:  ${IRCU_TLS}")
message(STATUS "  IPv6:                ${IRCU_ENABLE_IPV6}")
message(STATUS "")
message(STATUS "  poll() engine:       ${IRCU_ENABLE_POLL}")
message(STATUS "  kqueue() engine:     ${IRCU_ENABLE_KQUEUE}")
message(STATUS "  /dev/poll engine:    ${IRCU_ENABLE_DEVPOLL}")
message(STATUS "  epoll() engine:      ${IRCU_ENABLE_EPOLL}")
message(STATUS "")
