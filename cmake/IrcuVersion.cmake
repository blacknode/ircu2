#
# Version components derived from project(... VERSION ...).
#
# These reproduce the macros the autotools build derived from AC_INIT:
#
#   BASE_VERSION    "u2.<major>"    e.g. "u2.10"
#   MAJOR_PROTOCOL  "<major>"       e.g. "10"
#   RELEASE         ".<minor>."     e.g. ".12."
#   PATCHLEVEL      "<patch>"       e.g. "19"
#
# ircd/version.c pastes them together as BASE_VERSION RELEASE PATCHLEVEL,
# so the resulting version string stays "u2.10.12.19".
#

set(IRCU_BASE_VERSION   "u2.${PROJECT_VERSION_MAJOR}")
set(IRCU_MAJOR_PROTOCOL "${PROJECT_VERSION_MAJOR}")
set(IRCU_RELEASE        ".${PROJECT_VERSION_MINOR}.")
set(IRCU_PATCHLEVEL     "${PROJECT_VERSION_PATCH}")

# Full human-readable version, e.g. "u2.10.12.19".
set(IRCU_FULL_VERSION
  "${IRCU_BASE_VERSION}${IRCU_RELEASE}${IRCU_PATCHLEVEL}")
