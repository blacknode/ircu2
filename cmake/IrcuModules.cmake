#
# Building loadable modules.
#
# A module is a shared object the server opens with dlopen() at run time.
# It links against nothing: the core's symbols are resolved against the ircd
# executable, which carries a dynamic symbol table because the ircd target
# sets ENABLE_EXPORTS.
#
# Dropping a .c file into modules/ is enough to get it built: modules/
# CMakeLists.txt calls ircu_add_modules(), which globs the directory.  Use
# ircu_add_module() directly only for a module built from several sources.
#
#   ircu_add_modules()                    # every modules/*.c
#   ircu_add_module(nocaps nocaps.c)      # one module, named explicitly
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

  # IRCU_MPATH is the same directory the server loads from (MOD_PATH in
  # config.h, with any chroot prefix stripped), so an installed module is
  # found by name with no further configuration.
  install(TARGETS ${name}
    LIBRARY DESTINATION "${IRCU_MPATH}")
endfunction()

# ircu_add_modules([EXCLUDE <name>...])
#
# Builds one module per .c file in the calling directory, named after the
# file: nocaps.c becomes the module "nocaps", loaded as `Module { name =
# "nocaps"; };`.  A module that needs more than one source file is left out
# with EXCLUDE and given its own ircu_add_module() call.
#
# CONFIGURE_DEPENDS makes the build re-run the glob, so a newly added file
# is picked up by `cmake --build` without configuring again.
function(ircu_add_modules)
  cmake_parse_arguments(arg "" "" "EXCLUDE" ${ARGN})
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "ircu_add_modules: unexpected argument(s): ${arg_UNPARSED_ARGUMENTS}")
  endif()

  file(GLOB sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
  list(SORT sources)

  set(names "")
  foreach(source IN LISTS sources)
    get_filename_component(name "${source}" NAME)
    string(REGEX REPLACE "\\.c$" "" name "${name}")
    if(NOT name IN_LIST arg_EXCLUDE)
      ircu_add_module(${name} "${source}")
      list(APPEND names ${name})
    endif()
  endforeach()

  if(names)
    message(STATUS "Modules: ${names}")
  else()
    message(STATUS "Modules: none")
  endif()
endfunction()
