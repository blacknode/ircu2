#
# User-facing build options.
#
# Each one replaces a `configure` switch; the mapping is:
#
#   --enable-poll                 -> -DIRCU_ENABLE_POLL=ON
#   --enable-debug                -> -DIRCU_ENABLE_DEBUG=ON
#   --disable-asserts             -> -DIRCU_ENABLE_ASSERTS=OFF
#   --enable-profile              -> -DIRCU_ENABLE_PROFILE=ON
#   --enable-pedantic             -> -DIRCU_ENABLE_PEDANTIC=ON
#   --enable-warnings             -> -DIRCU_ENABLE_WARNINGS=ON
#   --disable-inlines             -> -DIRCU_ENABLE_INLINES=OFF
#   --disable-devpoll             -> -DIRCU_ENABLE_DEVPOLL=OFF
#   --disable-kqueue              -> -DIRCU_ENABLE_KQUEUE=OFF
#   --disable-epoll               -> -DIRCU_ENABLE_EPOLL=OFF
#   --without-ipv6                -> -DIRCU_ENABLE_IPV6=OFF
#   --with-leak-detect[=dir]      -> -DIRCU_LEAK_DETECT=yes|<dir>
#   --with-symlink=name           -> -DIRCU_SYMLINK=name  ("no" to skip)
#   --with-mode=mode              -> -DIRCU_MODE=711
#   --with-owner=owner            -> -DIRCU_OWNER=user
#   --with-group=group            -> -DIRCU_GROUP=group
#   --with-domain=domain          -> -DIRCU_DOMAIN=example.com
#   --with-chroot=dir             -> -DIRCU_CHROOT=/chroot
#   --with-dpath=dir              -> -DIRCU_DPATH=dir
#   --with-cpath=file             -> -DIRCU_CPATH=ircd.conf
#   --with-lpath=file             -> -DIRCU_LPATH=ircd.log
#   --with-maxcon=n               -> -DIRCU_MAXCON=16384
#   --with-tls=library            -> -DIRCU_TLS=auto|none|openssl|gnutls|libtls
#

# ---------------------------------------------------------------------------
# Event engines
#
# poll() defaults to on where it is a real system call, off where it is
# emulated on top of select() (SunOS 4, Darwin).  The other engines default to
# on and are silently disabled when the platform cannot provide them.
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME MATCHES "Linux|SunOS|OpenBSD|FreeBSD|NetBSD|DragonFly")
  set(_ircu_poll_default ON)
else()
  set(_ircu_poll_default OFF)
endif()
if(NOT HAVE_POLL_H)
  set(_ircu_poll_default OFF)
endif()

option(IRCU_ENABLE_POLL    "Use the poll() event engine"      ${_ircu_poll_default})
option(IRCU_ENABLE_DEVPOLL "Use the /dev/poll event engine"   ON)
option(IRCU_ENABLE_KQUEUE  "Use the kqueue() event engine"    ON)
option(IRCU_ENABLE_EPOLL   "Use the epoll() event engine"     ON)

# Clamp each engine to what the system actually offers.
if(IRCU_ENABLE_POLL AND NOT HAVE_POLL_H)
  message(STATUS "poll() engine requested but <poll.h> is missing; disabling")
  set(IRCU_ENABLE_POLL OFF)
endif()
if(IRCU_ENABLE_DEVPOLL AND NOT HAVE_SYS_DEVPOLL_H)
  set(IRCU_ENABLE_DEVPOLL OFF)
endif()
if(IRCU_ENABLE_KQUEUE AND NOT (HAVE_SYS_EVENT_H AND HAVE_KQUEUE))
  set(IRCU_ENABLE_KQUEUE OFF)
endif()
if(IRCU_ENABLE_EPOLL AND NOT HAVE_SYS_EPOLL_H)
  set(IRCU_ENABLE_EPOLL OFF)
endif()

# The engine list always ends in a fallback: poll() when it is available,
# select() otherwise.  ircd_events.c picks between the compiled-in engines at
# run time, so several may be built together.
set(IRCU_ENGINE_SOURCES "")
if(IRCU_ENABLE_EPOLL)
  list(APPEND IRCU_ENGINE_SOURCES engine_epoll.c)
endif()
if(IRCU_ENABLE_KQUEUE)
  list(APPEND IRCU_ENGINE_SOURCES engine_kqueue.c)
endif()
if(IRCU_ENABLE_DEVPOLL)
  list(APPEND IRCU_ENGINE_SOURCES engine_devpoll.c)
endif()
if(IRCU_ENABLE_POLL)
  list(APPEND IRCU_ENGINE_SOURCES engine_poll.c)
  set(USE_POLL 1)
else()
  list(APPEND IRCU_ENGINE_SOURCES engine_select.c)
endif()

if(IRCU_ENABLE_DEVPOLL)
  set(USE_DEVPOLL 1)
endif()
if(IRCU_ENABLE_KQUEUE)
  set(USE_KQUEUE 1)
endif()
if(IRCU_ENABLE_EPOLL)
  set(USE_EPOLL 1)
  # Some libcs ship <sys/epoll.h> without the syscall wrappers; ircd_events
  # then provides its own.
  include(CheckCSourceCompiles)
  check_c_source_compiles("
#include <sys/epoll.h>
int main(void) { return epoll_create(10); }
" IRCU_EPOLL_FUNCTIONS_DECLARED)
  if(NOT IRCU_EPOLL_FUNCTIONS_DECLARED)
    set(EPOLL_NEED_BODY 1)
  endif()
endif()

# ---------------------------------------------------------------------------
# Compilation behaviour
# ---------------------------------------------------------------------------
option(IRCU_ENABLE_DEBUG    "Turn on debugging mode (DEBUGMODE)"            OFF)
option(IRCU_ENABLE_ASSERTS  "Enable assertion checking"                     ON)
option(IRCU_ENABLE_PROFILE  "Enable gprof profiling support (-pg)"          OFF)
option(IRCU_ENABLE_PEDANTIC "Add -pedantic to the compiler flags"           OFF)
option(IRCU_ENABLE_WARNINGS "Add -Wall to the compiler flags"               OFF)
option(IRCU_ENABLE_INLINES  "Inline a few critical functions (FORCEINLINE)" ON)

if(IRCU_ENABLE_DEBUG)
  set(DEBUGMODE 1)
endif()
if(NOT IRCU_ENABLE_ASSERTS)
  set(NDEBUG 1)
endif()
if(IRCU_ENABLE_INLINES)
  set(FORCEINLINE 1)
endif()

set(IRCU_LEAK_DETECT "" CACHE STRING
  "Enable the Boehm GC leak detector: \"yes\", or the directory holding a patched libgc")

# ---------------------------------------------------------------------------
# IPv6
# ---------------------------------------------------------------------------
if(IRCU_HAVE_SOCKADDR_IN6)
  set(_ircu_ipv6_default ON)
else()
  set(_ircu_ipv6_default OFF)
endif()
option(IRCU_ENABLE_IPV6 "Enable IPv6 support" ${_ircu_ipv6_default})
if(IRCU_ENABLE_IPV6)
  if(NOT IRCU_HAVE_SOCKADDR_IN6)
    message(WARNING "IPv6 requested but struct sockaddr_in6 was not found")
  endif()
  set(IPV6 1)
endif()

# ---------------------------------------------------------------------------
# Connection limit
# ---------------------------------------------------------------------------
set(IRCU_MAXCON 16384 CACHE STRING
  "Maximum number of network connections the server will accept")
if(NOT IRCU_MAXCON MATCHES "^[0-9]+$")
  message(FATAL_ERROR "IRCU_MAXCON must be a number (got \"${IRCU_MAXCON}\")")
endif()
if(IRCU_MAXCON LESS 32)
  message(FATAL_ERROR "IRCU_MAXCON must be at least 32")
endif()
set(MAXCONNECTIONS "${IRCU_MAXCON}")

# ---------------------------------------------------------------------------
# Installed-binary ownership, permissions and symlink name
# ---------------------------------------------------------------------------
set(IRCU_SYMLINK "ircd" CACHE STRING
  "Name of the symlink created next to the installed binary (\"no\" to skip)")
set(IRCU_MODE "711" CACHE STRING
  "Octal permissions to give the installed binary")

if(NOT DEFINED CACHE{IRCU_OWNER} OR NOT IRCU_OWNER)
  execute_process(COMMAND id -un
    OUTPUT_VARIABLE _ircu_uid OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE _ircu_uid_res)
  if(NOT _ircu_uid_res EQUAL 0 OR NOT _ircu_uid)
    set(_ircu_uid "")
  endif()
  set(IRCU_OWNER "${_ircu_uid}" CACHE STRING
    "User that should own the installed binary")
endif()

if(NOT DEFINED CACHE{IRCU_GROUP} OR NOT IRCU_GROUP)
  execute_process(COMMAND id -gn
    OUTPUT_VARIABLE _ircu_gid OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET RESULT_VARIABLE _ircu_gid_res)
  if(NOT _ircu_gid_res EQUAL 0 OR NOT _ircu_gid)
    set(_ircu_gid "")
  endif()
  set(IRCU_GROUP "${_ircu_gid}" CACHE STRING
    "Group that should own the installed binary")
endif()

# ---------------------------------------------------------------------------
# Site domain, used for local statistics gathering
# ---------------------------------------------------------------------------
if(NOT DEFINED CACHE{IRCU_DOMAIN} OR NOT IRCU_DOMAIN)
  set(_ircu_domain "")
  if(EXISTS /etc/resolv.conf)
    file(STRINGS /etc/resolv.conf _ircu_resolv REGEX "^(domain|search)[ \t]+")
    foreach(_key domain search)
      foreach(_line IN LISTS _ircu_resolv)
        if(NOT _ircu_domain AND _line MATCHES "^${_key}[ \t]+([^ \t]+)")
          set(_ircu_domain "${CMAKE_MATCH_1}")
        endif()
      endforeach()
    endforeach()
  endif()
  set(IRCU_DOMAIN "${_ircu_domain}" CACHE STRING
    "Domain name used in local statistics gathering")
endif()

if(NOT IRCU_DOMAIN)
  message(FATAL_ERROR
    "Unable to determine the server DNS domain; pass -DIRCU_DOMAIN=<domain>")
endif()

set(DOMAINNAME "*${IRCU_DOMAIN}")
