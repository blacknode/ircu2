#
# Runtime paths compiled into the server: DPATH, CPATH, LPATH, SPATH, the
# module directory (MOD_PATH) and the translation directory (PO_PATH).
#
# When --with-chroot (IRCU_CHROOT) is in play the server sees the filesystem
# from inside the chroot, so every absolute path baked into the binary has the
# chroot prefix stripped, while the install rules keep using the full path.
#

# IRCU_CHROOT is declared in the top-level CMakeLists.txt, with its trailing
# slashes already stripped so the prefix comparisons below behave: the install
# prefix defaults to it, so it has to be known before GNUInstallDirs runs.

# ---------------------------------------------------------------------------
# Data directory
# ---------------------------------------------------------------------------
set(IRCU_DPATH "${CMAKE_INSTALL_FULL_LIBDIR}" CACHE STRING
  "Directory for all server data files")
string(REGEX REPLACE "/+$" "" IRCU_DPATH "${IRCU_DPATH}")

# ---------------------------------------------------------------------------
# Configuration and debug-log file names.  Relative names are resolved by the
# server against its data directory at run time.
# ---------------------------------------------------------------------------
set(IRCU_CPATH "ircd.conf" CACHE STRING "Default server configuration file")
set(IRCU_LPATH "ircd.log"  CACHE STRING "Default debugging log file")

# ---------------------------------------------------------------------------
# Path the server re-executes on /restart
# ---------------------------------------------------------------------------
if(IRCU_SYMLINK STREQUAL "no")
  set(_ircu_spath_default "${CMAKE_INSTALL_FULL_BINDIR}/ircd")
else()
  set(_ircu_spath_default "${CMAKE_INSTALL_FULL_BINDIR}/${IRCU_SYMLINK}")
endif()

set(IRCU_SPATH "${_ircu_spath_default}" CACHE STRING
  "Path to the server binary, re-executed on /restart")
string(REGEX REPLACE "/+$" "" IRCU_SPATH "${IRCU_SPATH}")
if(NOT IRCU_SPATH)
  set(IRCU_SPATH "${_ircu_spath_default}")
endif()
set(_ircu_spath "${IRCU_SPATH}")

# ---------------------------------------------------------------------------
# The host an isolated module runs in
# ---------------------------------------------------------------------------
# Beside the server binary, because the two are one version: the handshake
# compares their ABI and refuses a mismatch, so they are installed and
# upgraded together.  It is a separate variable all the same, for the same
# reason SPATH is one -- inside a chroot the server can only reach what is
# below the new root.  See doc/readme.isolation.
get_filename_component(_ircu_bindir "${_ircu_spath}" DIRECTORY)
set(_ircu_hostpath_default "${_ircu_bindir}/ircu-modhost")

set(IRCU_HOSTPATH "${_ircu_hostpath_default}" CACHE STRING
  "Path to ircu-modhost, which an isolated module runs inside")
string(REGEX REPLACE "/+$" "" IRCU_HOSTPATH "${IRCU_HOSTPATH}")
if(NOT IRCU_HOSTPATH)
  set(IRCU_HOSTPATH "${_ircu_hostpath_default}")
endif()

# ---------------------------------------------------------------------------
# Module directory
#
# Both ends of a module's life come from here: `ircu_add_module()` installs
# below this directory, as <type>/<name>.so or <type>/<name>/<name>.so, and
# the server searches its type directories for those same two shapes when
# a Module{} block or /MODULE LOAD names a module.  That is why a module is
# configured by name and never by path.
#
# It defaults to a "modules" subdirectory of the data directory, so it
# follows DPATH wherever that goes -- including inside a chroot, where the
# server can only reach what is below the new root.
#
# The cache variable is IRCU_MPATH; the macro compiled into the server is
# MOD_PATH, because MPATH is already taken by the runtime feature that names
# the MOTD file (FEAT_MPATH).
# ---------------------------------------------------------------------------
set(_ircu_mpath_default "${IRCU_DPATH}/modules")

set(IRCU_MPATH "${_ircu_mpath_default}" CACHE STRING
  "Directory modules are installed into and loaded from")
string(REGEX REPLACE "/+$" "" IRCU_MPATH "${IRCU_MPATH}")
if(NOT IRCU_MPATH)
  set(IRCU_MPATH "${_ircu_mpath_default}")
endif()

# ---------------------------------------------------------------------------
# Translation catalogs
#
# The same shape as the module directory: `po/*.po` in the source tree is
# installed here, and the server reads every <code>.po it finds here as the
# "core" domain (doc/readme.translations).  A module's catalogs are not
# involved: they are resources of the module and travel beside its .so.
#
# The cache variable is IRCU_POPATH; the macro compiled into the server is
# PO_PATH.
# ---------------------------------------------------------------------------
set(_ircu_popath_default "${IRCU_DPATH}/po")

set(IRCU_POPATH "${_ircu_popath_default}" CACHE STRING
  "Directory the core translation catalogs are installed into and read from")
string(REGEX REPLACE "/+$" "" IRCU_POPATH "${IRCU_POPATH}")
if(NOT IRCU_POPATH)
  set(IRCU_POPATH "${_ircu_popath_default}")
endif()

# ---------------------------------------------------------------------------
# Rebase the absolute paths onto the chroot
# ---------------------------------------------------------------------------
function(_ircu_strip_chroot out path what fatal)
  if(NOT IRCU_CHROOT)
    set(${out} "${path}" PARENT_SCOPE)
    return()
  endif()
  string(FIND "${path}" "${IRCU_CHROOT}" _pos)
  if(_pos EQUAL 0)
    string(LENGTH "${IRCU_CHROOT}" _len)
    string(SUBSTRING "${path}" ${_len} -1 _stripped)
    set(${out} "${_stripped}" PARENT_SCOPE)
  elseif(fatal)
    message(FATAL_ERROR
      "${what} ${path} is not below the root directory ${IRCU_CHROOT}.  "
      "Every path compiled into the server has to be reachable from the new "
      "root: either move it inside (the install prefix, which most of these "
      "paths derive from, is ${CMAKE_INSTALL_PREFIX}) or set it explicitly "
      "with the matching IRCU_* variable.")
  else()
    message(WARNING
      "${what} ${path} is not below the root directory ${IRCU_CHROOT}; "
      "restarts will probably fail")
    set(${out} "${path}" PARENT_SCOPE)
  endif()
endfunction()

_ircu_strip_chroot(SPATH "${_ircu_spath}" "Binary" FALSE)
_ircu_strip_chroot(HOST_PATH "${IRCU_HOSTPATH}" "Module host" FALSE)
_ircu_strip_chroot(DPATH "${IRCU_DPATH}"  "Data directory" TRUE)
_ircu_strip_chroot(MOD_PATH "${IRCU_MPATH}" "Module directory" TRUE)
_ircu_strip_chroot(PO_PATH "${IRCU_POPATH}" "Translation directory" TRUE)

if(IRCU_CPATH MATCHES "^/")
  _ircu_strip_chroot(CPATH "${IRCU_CPATH}" "Configuration file" TRUE)
else()
  set(CPATH "${IRCU_CPATH}")
endif()

if(IRCU_LPATH MATCHES "^/")
  if(IRCU_CHROOT)
    string(FIND "${IRCU_LPATH}" "${IRCU_CHROOT}" _pos)
    if(_pos EQUAL 0)
      _ircu_strip_chroot(LPATH "${IRCU_LPATH}" "Log file" TRUE)
    else()
      message(WARNING
        "Log file ${IRCU_LPATH} is not below the root directory "
        "${IRCU_CHROOT}; using the default ircd.log instead")
      set(LPATH "ircd.log")
    endif()
  else()
    set(LPATH "${IRCU_LPATH}")
  endif()
else()
  set(LPATH "${IRCU_LPATH}")
endif()
