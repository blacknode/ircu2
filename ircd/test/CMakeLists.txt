#
# IRC - Internet Relay Chat, ircd/test/CMakeLists.txt
#
# Unit tests, registered with CTest.  Run them with:
#
#   ctest --test-dir <build dir> --output-on-failure
#
# Each test links the handful of server modules it exercises plus test_stub.c,
# which supplies the logging and client hooks those modules expect.  The
# autotools build reused ../*.o for this; recompiling the same sources here is
# a few seconds of work and keeps the tests independent of the ircd target's
# object layout.
#

set(IRCU_IRCD_DIR "${PROJECT_SOURCE_DIR}/ircd")

# ircd_string.c #includes the generated chattr.tab.c; source properties do not
# cross directories, so the dependency is restated here.
set_source_files_properties("${IRCU_IRCD_DIR}/ircd_string.c"
  PROPERTIES OBJECT_DEPENDS "${IRCU_CHATTR_TAB}")

# add_ircu_test(<name> [extra sources...])
#
# Builds <name> from <name>.c, test_stub.c and the given server sources, then
# registers it with CTest.
function(add_ircu_test name)
  add_executable(${name} ${name}.c test_stub.c ${ARGN})
  target_include_directories(${name} PRIVATE
    "${PROJECT_BINARY_DIR}/ircd")   # chattr.tab.c
  target_link_libraries(${name} PRIVATE ircu_config ${IRCU_SYSTEM_LIBRARIES})
  target_compile_options(${name} PRIVATE -g -Wall)
  add_dependencies(${name} ircu_chattr)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_ircu_test(cidr_lookups_t
  "${IRCU_IRCD_DIR}/cidr_lookups.c"
  "${IRCU_IRCD_DIR}/ircd_alloc.c"
  "${IRCU_IRCD_DIR}/ircd_string.c")

add_ircu_test(ircd_chattr_t
  "${IRCU_IRCD_DIR}/ircd_string.c")

add_ircu_test(ircd_in_addr_t
  "${IRCU_IRCD_DIR}/ircd_alloc.c"
  "${IRCU_IRCD_DIR}/ircd_string.c"
  "${IRCU_IRCD_DIR}/match.c"
  "${IRCU_IRCD_DIR}/numnicks.c")

add_ircu_test(ircd_match_t
  "${IRCU_IRCD_DIR}/ircd_string.c"
  "${IRCU_IRCD_DIR}/match.c")

add_ircu_test(ircd_string_t
  "${IRCU_IRCD_DIR}/ircd_string.c")

add_ircu_test(msgq_excise_t
  "${IRCU_IRCD_DIR}/ircd_alloc.c"
  "${IRCU_IRCD_DIR}/ircd_snprintf.c"
  "${IRCU_IRCD_DIR}/ircd_string.c"
  "${IRCU_IRCD_DIR}/msgq.c")
