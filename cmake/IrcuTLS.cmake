#
# TLS backend selection (the old unet_TLS macro).
#
#   -DIRCU_TLS=auto      probe openssl, then gnutls, then libtls (default)
#   -DIRCU_TLS=openssl   OpenSSL / LibreSSL's libssl
#   -DIRCU_TLS=gnutls    GnuTLS
#   -DIRCU_TLS=libtls    LibreSSL's libtls
#   -DIRCU_TLS=none      build without TLS support
#
# On success this defines the imported target `ircu::tls`, which the ircd
# target links against, and sets IRCU_TLS_SOURCE to the backend to compile.
#

include(CheckLibraryExists)
include(CheckIncludeFile)
include(CheckSymbolExists)
include(CMakePushCheckState)

set(IRCU_TLS "auto" CACHE STRING
  "TLS library to use: auto, none, openssl, gnutls or libtls")
set_property(CACHE IRCU_TLS PROPERTY STRINGS auto none openssl gnutls libtls)

set(_ircu_tls_requested "${IRCU_TLS}")
if(NOT _ircu_tls_requested MATCHES "^(auto|none|openssl|gnutls|libtls)$")
  message(FATAL_ERROR
    "Unknown TLS library \"${IRCU_TLS}\"; expected one of: "
    "auto, none, openssl, gnutls, libtls")
endif()

find_package(PkgConfig QUIET)

# ---------------------------------------------------------------------------
# Probe the candidates.  Nothing is required at this point: with IRCU_TLS=auto
# the first one that turns up wins, and an explicit request that cannot be
# satisfied is a hard error further down.
# ---------------------------------------------------------------------------
set(_ircu_have_openssl FALSE)
if(_ircu_tls_requested MATCHES "^(auto|openssl)$")
  find_package(OpenSSL QUIET COMPONENTS SSL Crypto)
  if(OPENSSL_FOUND)
    set(_ircu_have_openssl TRUE)
  endif()
endif()

set(_ircu_have_gnutls FALSE)
if(_ircu_tls_requested MATCHES "^(auto|gnutls)$" AND PKG_CONFIG_FOUND)
  pkg_check_modules(GNUTLS QUIET IMPORTED_TARGET gnutls)
  if(GNUTLS_FOUND)
    set(_ircu_have_gnutls TRUE)
  endif()
endif()

set(_ircu_have_libtls FALSE)
if(_ircu_tls_requested MATCHES "^(auto|libtls)$")
  # OpenBSD ships libtls in base without a .pc file, so fall back to a plain
  # header + symbol probe when pkg-config comes up empty.
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBTLS QUIET IMPORTED_TARGET libtls)
  endif()
  if(LIBTLS_FOUND)
    set(_ircu_have_libtls TRUE)
  else()
    check_include_file(tls.h IRCU_HAVE_TLS_H)
    if(IRCU_HAVE_TLS_H)
      check_library_exists(tls tls_init "" IRCU_HAVE_LIBTLS)
      if(IRCU_HAVE_LIBTLS)
        set(_ircu_have_libtls TRUE)
      endif()
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Decide
# ---------------------------------------------------------------------------
if(_ircu_tls_requested STREQUAL "auto")
  if(_ircu_have_openssl)
    set(IRCU_TLS "openssl")
  elseif(_ircu_have_gnutls)
    set(IRCU_TLS "gnutls")
  elseif(_ircu_have_libtls)
    set(IRCU_TLS "libtls")
  else()
    message(STATUS "No TLS library found; building without TLS support")
    set(IRCU_TLS "none")
  endif()
endif()

add_library(ircu_tls INTERFACE)
add_library(ircu::tls ALIAS ircu_tls)

if(IRCU_TLS STREQUAL "openssl")
  if(NOT _ircu_have_openssl)
    message(FATAL_ERROR "IRCU_TLS=openssl was requested but OpenSSL was not found")
  endif()
  set(IRCU_TLS_SOURCE tls_openssl.c)
  target_link_libraries(ircu_tls INTERFACE OpenSSL::SSL OpenSSL::Crypto)

  # TLS 1.3 cipher suites are configured through a separate call.
  cmake_push_check_state()
  set(CMAKE_REQUIRED_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
  check_symbol_exists(SSL_set_ciphersuites "openssl/ssl.h" HAVE_SSL_SET_CIPHERSUITES)
  cmake_pop_check_state()

elseif(IRCU_TLS STREQUAL "gnutls")
  if(NOT _ircu_have_gnutls)
    message(FATAL_ERROR "IRCU_TLS=gnutls was requested but GnuTLS was not found")
  endif()
  set(IRCU_TLS_SOURCE tls_gnutls.c)
  target_link_libraries(ircu_tls INTERFACE PkgConfig::GNUTLS)

elseif(IRCU_TLS STREQUAL "libtls")
  if(NOT _ircu_have_libtls)
    message(FATAL_ERROR "IRCU_TLS=libtls was requested but libtls was not found")
  endif()
  set(IRCU_TLS_SOURCE tls_libtls.c)
  if(LIBTLS_FOUND)
    target_link_libraries(ircu_tls INTERFACE PkgConfig::LIBTLS)
  else()
    target_link_libraries(ircu_tls INTERFACE tls)
  endif()

else()
  set(IRCU_TLS_SOURCE tls_none.c)
endif()

# Reflect the resolved choice back into the cache so a re-run of cmake without
# arguments keeps the same backend.
set(IRCU_TLS "${IRCU_TLS}" CACHE STRING
  "TLS library to use: auto, none, openssl, gnutls or libtls" FORCE)
