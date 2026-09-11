#
# System probes: the CMake equivalent of the AC_CHECK_* / unet_* macros that
# used to live in configure.ac and acinclude.m4.
#
# Everything set here ends up in config.h (see cmake/config.h.cmake.in).
#

include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckSymbolExists)
include(CheckTypeSize)
include(CheckCSourceCompiles)
include(CheckLibraryExists)
include(TestBigEndian)

# ---------------------------------------------------------------------------
# Libraries: crypt() and the socket/resolver split found on Solaris & friends
# (the old AC_SEARCH_LIBS(crypt) and AC_LIBRARY_NET).
# ---------------------------------------------------------------------------
set(IRCU_SYSTEM_LIBRARIES "")

check_function_exists(crypt IRCU_CRYPT_IN_LIBC)
if(NOT IRCU_CRYPT_IN_LIBC)
  set(_ircu_crypt_found FALSE)
  foreach(_lib crypt descrypt)
    check_library_exists(${_lib} crypt "" IRCU_CRYPT_IN_LIB${_lib})
    if(IRCU_CRYPT_IN_LIB${_lib})
      list(APPEND IRCU_SYSTEM_LIBRARIES ${_lib})
      set(_ircu_crypt_found TRUE)
      break()
    endif()
  endforeach()
  if(NOT _ircu_crypt_found)
    message(FATAL_ERROR "Unable to find library containing crypt()")
  endif()
endif()

check_function_exists(gethostbyname IRCU_GETHOSTBYNAME_IN_LIBC)
if(NOT IRCU_GETHOSTBYNAME_IN_LIBC)
  check_library_exists(nsl gethostbyname "" HAVE_LIBNSL)
  if(HAVE_LIBNSL)
    list(APPEND IRCU_SYSTEM_LIBRARIES nsl)
  else()
    check_library_exists(socket gethostbyname "" HAVE_LIBSOCKET)
    if(HAVE_LIBSOCKET)
      list(APPEND IRCU_SYSTEM_LIBRARIES socket)
    else()
      check_library_exists(resolv gethostbyname "" HAVE_LIBRESOLV)
      if(HAVE_LIBRESOLV)
        list(APPEND IRCU_SYSTEM_LIBRARIES resolv)
      endif()
    endif()
  endif()
endif()

check_function_exists(socket IRCU_SOCKET_IN_LIBC)
if(NOT IRCU_SOCKET_IN_LIBC AND NOT HAVE_LIBSOCKET)
  check_library_exists(socket socket "" IRCU_SOCKET_IN_LIBSOCKET)
  if(IRCU_SOCKET_IN_LIBSOCKET)
    set(HAVE_LIBSOCKET TRUE)
    list(APPEND IRCU_SYSTEM_LIBRARIES socket)
  endif()
endif()

# Worker threads (ircd/worker.c).  Required, not optional: the worker API is
# always compiled in, and FEAT_WORKER_THREADS decides at run time whether any
# thread is ever created.  See doc/readme.workers.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
list(APPEND IRCU_SYSTEM_LIBRARIES Threads::Threads)

list(REMOVE_DUPLICATES IRCU_SYSTEM_LIBRARIES)

# ---------------------------------------------------------------------------
# Header files
# ---------------------------------------------------------------------------
foreach(_hdr
    crypt.h poll.h inttypes.h stdint.h stdio.h stdlib.h string.h strings.h
    unistd.h sys/devpoll.h sys/epoll.h sys/event.h sys/param.h sys/resource.h
    sys/socket.h sys/stat.h sys/time.h sys/types.h)
  string(TOUPPER "HAVE_${_hdr}" _var)
  string(REGEX REPLACE "[/.]" "_" _var "${_var}")
  check_include_file("${_hdr}" ${_var})
endforeach()

# The C90 headers are always present on anything we can build on; the macro is
# kept only so config.h stays a drop-in replacement for the autoconf one.
set(STDC_HEADERS 1)

# ---------------------------------------------------------------------------
# Library functions
# ---------------------------------------------------------------------------
check_function_exists(kqueue HAVE_KQUEUE)
check_function_exists(setrlimit HAVE_SETRLIMIT)
check_function_exists(getrusage HAVE_GETRUSAGE)
check_function_exists(times HAVE_TIMES)

# ---------------------------------------------------------------------------
# Byte order, type sizes and fallback integer typedefs
# (the old AC_C_BIGENDIAN and unet_CHECK_TYPE_SIZES).
# ---------------------------------------------------------------------------
test_big_endian(WORDS_BIGENDIAN)

set(CMAKE_EXTRA_INCLUDE_FILES stdint.h)
check_type_size("short"     SIZEOF_SHORT)
check_type_size("int"       SIZEOF_INT)
check_type_size("long"      SIZEOF_LONG)
check_type_size("void *"    SIZEOF_VOID_P)
check_type_size("long long" SIZEOF_LONG_LONG)
check_type_size("int64_t"   SIZEOF_INT64_T)
unset(CMAKE_EXTRA_INCLUDE_FILES)

# On any system without <stdint.h> typedefs, fall back to a type of the right
# width.  config.h #defines the name only when the real typedef is missing.
set(IRCU_TYPEDEF_INT16  "")
set(IRCU_TYPEDEF_UINT16 "")
set(IRCU_TYPEDEF_INT32  "")
set(IRCU_TYPEDEF_UINT32 "")
set(IRCU_TYPEDEF_INT64  "")
set(IRCU_TYPEDEF_UINT64 "")

set(CMAKE_EXTRA_INCLUDE_FILES stdint.h)
check_type_size("int16_t"  IRCU_SIZEOF_INT16_T)
check_type_size("uint16_t" IRCU_SIZEOF_UINT16_T)
check_type_size("int32_t"  IRCU_SIZEOF_INT32_T)
check_type_size("uint32_t" IRCU_SIZEOF_UINT32_T)
check_type_size("uint64_t" IRCU_SIZEOF_UINT64_T)
unset(CMAKE_EXTRA_INCLUDE_FILES)

if(NOT HAVE_IRCU_SIZEOF_INT16_T)
  if(SIZEOF_INT EQUAL 2)
    set(IRCU_TYPEDEF_INT16 "int")
    set(IRCU_TYPEDEF_UINT16 "unsigned int")
  elseif(SIZEOF_SHORT EQUAL 2)
    set(IRCU_TYPEDEF_INT16 "short")
    set(IRCU_TYPEDEF_UINT16 "unsigned short")
  else()
    message(FATAL_ERROR "Cannot find a type with size of 16 bits")
  endif()
endif()

if(NOT HAVE_IRCU_SIZEOF_INT32_T)
  if(SIZEOF_INT EQUAL 4)
    set(IRCU_TYPEDEF_INT32 "int")
    set(IRCU_TYPEDEF_UINT32 "unsigned int")
  elseif(SIZEOF_SHORT EQUAL 4)
    set(IRCU_TYPEDEF_INT32 "short")
    set(IRCU_TYPEDEF_UINT32 "unsigned short")
  elseif(SIZEOF_LONG EQUAL 4)
    set(IRCU_TYPEDEF_INT32 "long")
    set(IRCU_TYPEDEF_UINT32 "unsigned long")
  else()
    message(FATAL_ERROR "Cannot find a type with size of 32 bits")
  endif()
endif()

if(NOT HAVE_SIZEOF_INT64_T)
  if(SIZEOF_LONG_LONG EQUAL 8)
    set(IRCU_TYPEDEF_INT64 "long long")
    set(IRCU_TYPEDEF_UINT64 "unsigned long long")
  else()
    message(FATAL_ERROR "Cannot find a type with size of 64 bits")
  endif()
endif()

# ---------------------------------------------------------------------------
# size_t / uid_t / gid_t / socklen_t / struct tm
# ---------------------------------------------------------------------------
check_type_size("size_t" IRCU_SIZEOF_SIZE_T)
if(HAVE_IRCU_SIZEOF_SIZE_T)
  set(IRCU_TYPEDEF_SIZE_T "")
else()
  set(IRCU_TYPEDEF_SIZE_T "unsigned int")
endif()

check_c_source_compiles("
#include <sys/types.h>
int main(void) { uid_t u = 0; gid_t g = 0; (void)u; (void)g; return 0; }
" IRCU_HAVE_UID_T)
if(IRCU_HAVE_UID_T)
  set(IRCU_TYPEDEF_UID_T "")
  set(IRCU_TYPEDEF_GID_T "")
else()
  set(IRCU_TYPEDEF_UID_T "int")
  set(IRCU_TYPEDEF_GID_T "int")
endif()

# `struct tm` normally lives in <time.h>; a few ancient systems only declare it
# in <sys/time.h>.
check_c_source_compiles("
#include <sys/types.h>
#include <time.h>
int main(void) { struct tm tm; tm.tm_sec = 0; return tm.tm_sec; }
" IRCU_STRUCT_TM_IN_TIME_H)
if(NOT IRCU_STRUCT_TM_IN_TIME_H)
  set(TM_IN_SYS_TIME 1)
endif()

# socklen_t: traditional BSD uses int, some systems something else.  Probe the
# real type first and only fall back to guessing the getpeername() signature.
check_c_source_compiles("
#include <sys/types.h>
#include <sys/socket.h>
int main(void) { socklen_t len = 0; return (int)len; }
" IRCU_HAVE_SOCKLEN_T)

if(IRCU_HAVE_SOCKLEN_T)
  set(IRCU_TYPEDEF_SOCKLEN_T "")
else()
  set(IRCU_TYPEDEF_SOCKLEN_T "")
  foreach(_arg2 "struct sockaddr" "void")
    foreach(_type "int" "size_t" "unsigned" "long" "unsigned long")
      if(NOT IRCU_TYPEDEF_SOCKLEN_T)
        string(MAKE_C_IDENTIFIER "ircu_socklen_${_arg2}_${_type}" _cachevar)
        check_c_source_compiles("
#include <sys/types.h>
#include <sys/socket.h>
int getpeername (int, ${_arg2} *, ${_type} *);
int main(void) { ${_type} len; getpeername(0, 0, &len); return 0; }
" ${_cachevar})
        if(${_cachevar})
          set(IRCU_TYPEDEF_SOCKLEN_T "${_type}")
        endif()
      endif()
    endforeach()
  endforeach()
  if(NOT IRCU_TYPEDEF_SOCKLEN_T)
    set(IRCU_TYPEDEF_SOCKLEN_T "int")
  endif()
  message(STATUS "socklen_t equivalent: ${IRCU_TYPEDEF_SOCKLEN_T}")
endif()

# ---------------------------------------------------------------------------
# IPv6 availability (feeds the default of IRCU_ENABLE_IPV6)
# ---------------------------------------------------------------------------
check_c_source_compiles("
#include <sys/types.h>
#include <netinet/in.h>
int main(void) { struct sockaddr_in6 sa; sa.sin6_port = 0; return 0; }
" IRCU_HAVE_SOCKADDR_IN6)

# ---------------------------------------------------------------------------
# va_copy / __va_copy
# ---------------------------------------------------------------------------
check_c_source_compiles("
#include <stdarg.h>
int main(void) { va_list ap1, ap2; va_copy(ap1, ap2); va_end(ap1); return 0; }
" HAVE_VA_COPY)

check_c_source_compiles("
#include <stdarg.h>
int main(void) { va_list ap1, ap2; __va_copy(ap1, ap2); va_end(ap1); return 0; }
" HAVE___VA_COPY)

# ---------------------------------------------------------------------------
# Non-blocking socket flavour (the old unet_NONBLOCKING).
#
# Historically this was a run-time test, which made it useless when
# cross-compiling.  Compiling the call is enough to tell the three flavours
# apart: a system that declares O_NONBLOCK/F_SETFL honours them.
# ---------------------------------------------------------------------------
check_c_source_compiles("
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
int main(void) {
  int f = socket(AF_INET, SOCK_DGRAM, 0);
  return fcntl(f, F_SETFL, O_NONBLOCK);
}
" IRCU_NONBLOCK_POSIX)

if(IRCU_NONBLOCK_POSIX)
  set(NBLOCK_POSIX 1)
else()
  check_c_source_compiles("
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
int main(void) {
  int f = socket(AF_INET, SOCK_DGRAM, 0);
  return fcntl(f, F_SETFL, O_NDELAY);
}
" IRCU_NONBLOCK_BSD)
  if(IRCU_NONBLOCK_BSD)
    set(NBLOCK_BSD 1)
  else()
    set(NBLOCK_SYSV 1)
  endif()
endif()

# ---------------------------------------------------------------------------
# Signal flavour (the old unet_SIGNALS).
# ---------------------------------------------------------------------------
check_c_source_compiles("
#include <signal.h>
int main(void) {
  sigaction(SIGTERM, (struct sigaction *)0L, (struct sigaction *)0L);
  return 0;
}
" IRCU_SIGNALS_POSIX)

if(IRCU_SIGNALS_POSIX)
  set(POSIX_SIGNALS 1)
else()
  # Without POSIX sigaction we cannot tell reliable BSD signals from
  # unreliable SysV ones without running code, so assume the safe answer.
  set(SYSV_UNRELIABLE_SIGNALS 1)
endif()

# ---------------------------------------------------------------------------
# Platform quirks
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
  set(IRCU_SOLARIS 1)
endif()

# ---------------------------------------------------------------------------
# Compatibility typedefs emitted into config.h.
#
# Each of these is only defined when the platform does not already provide the
# real thing, mirroring what AC_CHECK_TYPE used to produce.
# ---------------------------------------------------------------------------
set(IRCU_COMPAT_TYPEDEFS "")

function(_ircu_compat_typedef name replacement)
  if(replacement)
    string(APPEND IRCU_COMPAT_TYPEDEFS
      "/* Define to a suitable substitute if the system lacks ${name}. */\n"
      "#define ${name} ${replacement}\n\n")
    set(IRCU_COMPAT_TYPEDEFS "${IRCU_COMPAT_TYPEDEFS}" PARENT_SCOPE)
  endif()
endfunction()

_ircu_compat_typedef(int16_t   "${IRCU_TYPEDEF_INT16}")
_ircu_compat_typedef(uint16_t  "${IRCU_TYPEDEF_UINT16}")
_ircu_compat_typedef(int32_t   "${IRCU_TYPEDEF_INT32}")
_ircu_compat_typedef(uint32_t  "${IRCU_TYPEDEF_UINT32}")
_ircu_compat_typedef(int64_t   "${IRCU_TYPEDEF_INT64}")
_ircu_compat_typedef(uint64_t  "${IRCU_TYPEDEF_UINT64}")
_ircu_compat_typedef(size_t    "${IRCU_TYPEDEF_SIZE_T}")
_ircu_compat_typedef(uid_t     "${IRCU_TYPEDEF_UID_T}")
_ircu_compat_typedef(gid_t     "${IRCU_TYPEDEF_GID_T}")
_ircu_compat_typedef(socklen_t "${IRCU_TYPEDEF_SOCKLEN_T}")

if(NOT IRCU_COMPAT_TYPEDEFS)
  set(IRCU_COMPAT_TYPEDEFS
    "/* This platform provides every type ircu needs; nothing to substitute. */\n")
endif()
