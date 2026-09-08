#
# Building loadable modules.
#
# A module is a shared object the server opens with dlopen() at run time.
# It links against nothing: the core's symbols are resolved against the ircd
# executable, which carries a dynamic symbol table because the ircd target
# sets ENABLE_EXPORTS.
#
# Usage, from a CMakeLists.txt under modules/:
#
#   ircu_add_module(nocaps nocaps.c)
#

# ircu_add_module(<name> <sources...>)
function(ircu_add_module name)
  add_library(${name} MODULE ${ARGN})

  # Headers and IRCU2_BUILD, the same as the core sees.
  target_link_libraries(${name} PRIVATE ircu_config)

  # ircd_parser.h and the generated config.h live in the build tree.
  target_include_directories(${name} PRIVATE "${PROJECT_BINARY_DIR}/ircd")

  # "nocaps.so", not "libnocaps.so": the config names the file directly.
  set_target_properties(${name} PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/modules")

  install(TARGETS ${name}
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/ircu/modules")
endfunction()
