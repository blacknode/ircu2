#
# Building loadable modules.
#
# A module is a shared object the server opens with dlopen() at run time.
# It links against nothing: the core's symbols are resolved against the
# ircd executable, which carries a dynamic symbol table because the ircd
# target sets ENABLE_EXPORTS.
#
# The source tree under modules/ is organised by kind of module:
#
#   modules/<type>/<name>.c        a module self-contained in one file
#   modules/<type>/<name>/         a module built from a directory: every
#                                  .c file below it (recursively) is
#                                  compiled in, .h files are private
#                                  headers, and anything else is a resource
#                                  copied next to the shared object
#
# A module that needs something the core knows nothing about -- a client
# library, a header outside the tree, a definition of its own -- says so in
# its own CMake fragment rather than in the core's build files:
#
#   modules/<type>/<name>/module.cmake    for a module built from a directory
#   modules/<type>/<name>.cmake           for a single-file module
#
# The fragment is read before the module is built, with IRCU_MODULE_NAME,
# IRCU_MODULE_TYPE and IRCU_MODULE_DIR set, and it answers by setting any of:
#
#   IRCU_MODULE_SKIP                  a reason not to build this module at
#                                     all -- a missing optional dependency,
#                                     say.  The rest of the tree still
#                                     builds, and the reason is printed.
#   IRCU_MODULE_LINK_LIBRARIES        libraries or imported targets to link
#   IRCU_MODULE_INCLUDE_DIRECTORIES   extra include directories
#   IRCU_MODULE_COMPILE_DEFINITIONS   extra -D definitions
#   IRCU_MODULE_COMPILE_OPTIONS       extra compiler flags
#
# That keeps the dependency where the module is: nothing in cmake/ or in the
# ircd's own build has to learn about a module's libraries, and a tree with
# no libpq simply builds one module fewer.
#
# <type> is any directory name -- commands, modes, hooks, workers, or one
# of your own -- and only organises the tree: the module is still named,
# loaded and unloaded by <name> alone, so a name may appear under one type
# only.  The build mirrors the layout, with a shared object where the
# sources were:
#
#   build/modules/<type>/<name>.so
#   build/modules/<type>/<name>/<name>.so   + the resources, same paths
#
# and installs the same shape under IRCU_MPATH, which is where the server
# looks (MOD_PATH in config.h, with any chroot prefix stripped).
#
# modules/CMakeLists.txt calls ircu_add_modules(), which discovers all of
# this; nothing has to be listed by hand.  ircu_add_module() underneath is
# what builds one module, and is exposed for a tree that wants to add a
# module from somewhere else:
#
#   ircu_add_module(nocaps SUBDIR hooks SOURCES nocaps.c)
#   ircu_add_module(bot SUBDIR commands/bot DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
#     SOURCES bot.c bot_cmds.c RESOURCES bot.conf)
#

# ircu_add_module(<name>
#                 SUBDIR <type>[/<name>]
#                 SOURCES <source>...
#                 [DIRECTORY <dir> [RESOURCES <file>...]]
#                 [LINK_LIBRARIES <lib>...] [INCLUDE_DIRECTORIES <dir>...]
#                 [COMPILE_DEFINITIONS <def>...] [COMPILE_OPTIONS <opt>...])
#
# Builds <name>.so from SOURCES into modules/<SUBDIR>/ of the build tree
# and installs it into <IRCU_MPATH>/<SUBDIR>/.  DIRECTORY is the module's
# own directory: it is added to the include path, so a header at its top
# is found from a source in any subdirectory, and each RESOURCES file is
# copied beside the shared object at build time and installed with it, at
# its path relative to DIRECTORY, so a resource in a subdirectory keeps
# that subdirectory.
function(ircu_add_module name)
  cmake_parse_arguments(arg "" "SUBDIR;DIRECTORY"
    "SOURCES;RESOURCES;LINK_LIBRARIES;INCLUDE_DIRECTORIES;COMPILE_DEFINITIONS;COMPILE_OPTIONS"
    ${ARGN})
  if(arg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "ircu_add_module(${name}): unexpected argument(s): "
      "${arg_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT arg_SUBDIR)
    message(FATAL_ERROR "ircu_add_module(${name}): SUBDIR is required")
  endif()
  if(NOT arg_SOURCES)
    message(FATAL_ERROR "ircu_add_module(${name}): SOURCES is required")
  endif()
  if(arg_RESOURCES AND NOT arg_DIRECTORY)
    message(FATAL_ERROR
      "ircu_add_module(${name}): RESOURCES needs DIRECTORY")
  endif()

  set(outdir "${PROJECT_BINARY_DIR}/modules/${arg_SUBDIR}")
  set(installdir "${IRCU_MPATH}/${arg_SUBDIR}")

  add_library(${name} MODULE ${arg_SOURCES})

  # Headers and IRCU2_BUILD, the same as the core sees.
  target_link_libraries(${name} PRIVATE ircu_config)

  # ircd_parser.h and the generated config.h live in the build tree.
  target_include_directories(${name} PRIVATE "${PROJECT_BINARY_DIR}/ircd")

  # A module's own headers.  The directory of each source is searched for
  # a quoted #include already; this adds the module's top, for a module
  # that keeps sources in a subdirectory of it.
  if(arg_DIRECTORY)
    target_include_directories(${name} PRIVATE "${arg_DIRECTORY}")
  endif()

  # Whatever the module's own fragment asked for.  A module still resolves
  # the core's symbols against the ircd executable; this is only for the
  # libraries the core does not have.
  if(arg_LINK_LIBRARIES)
    target_link_libraries(${name} PRIVATE ${arg_LINK_LIBRARIES})
  endif()
  if(arg_INCLUDE_DIRECTORIES)
    target_include_directories(${name} PRIVATE ${arg_INCLUDE_DIRECTORIES})
  endif()
  if(arg_COMPILE_DEFINITIONS)
    target_compile_definitions(${name} PRIVATE ${arg_COMPILE_DEFINITIONS})
  endif()
  if(arg_COMPILE_OPTIONS)
    target_compile_options(${name} PRIVATE ${arg_COMPILE_OPTIONS})
  endif()

  # "nocaps.so", not "libnocaps.so": the loader appends ".so" to the name.
  set_target_properties(${name} PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${outdir}")

  # IRCU_MPATH is the directory the server loads from, so an installed
  # module is found by name with no further configuration.
  install(TARGETS ${name}
    LIBRARY DESTINATION "${installdir}")

  # Resources are a build step rather than a configure-time copy, so a
  # changed resource reaches the build tree on the next build, the same as
  # a changed source does.
  set(copies "")
  foreach(resource IN LISTS arg_RESOURCES)
    file(RELATIVE_PATH relative "${arg_DIRECTORY}" "${resource}")
    set(copy "${outdir}/${relative}")
    add_custom_command(OUTPUT "${copy}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${resource}" "${copy}"
      DEPENDS "${resource}"
      COMMENT "Copying ${name} resource ${relative}"
      VERBATIM)
    list(APPEND copies "${copy}")

    get_filename_component(reldir "${relative}" DIRECTORY)
    install(FILES "${resource}" DESTINATION "${installdir}/${reldir}")
  endforeach()

  if(copies)
    add_custom_target(${name}_resources DEPENDS ${copies})
    add_dependencies(${name} ${name}_resources)
  endif()
endfunction()

# ircu_add_modules()
#
# Discovers and builds every module under the calling directory, laid out
# as described at the top of this file.  A hidden entry (a leading ".") is
# skipped at every level; a file that is neither .c nor .h beside the
# modules of a type is left alone, so a type directory may carry notes or
# a header its modules share.
#
# CONFIGURE_DEPENDS makes the build re-run the globs, so a new file or
# directory is picked up by `cmake --build` without configuring again.
function(ircu_add_modules)
  if(ARGN)
    message(FATAL_ERROR
      "ircu_add_modules: unexpected argument(s): ${ARGN}")
  endif()

  set(root "${CMAKE_CURRENT_SOURCE_DIR}")
  file(GLOB types LIST_DIRECTORIES true CONFIGURE_DEPENDS "${root}/*")
  list(SORT types)

  set(names "")
  set(origins "")
  set(built "")
  set(skipped "")
  foreach(typedir IN LISTS types)
    get_filename_component(type "${typedir}" NAME)
    if(type MATCHES "^\\.")
      continue()
    endif()

    if(NOT IS_DIRECTORY "${typedir}")
      # A source at the top is the old flat layout; say where it goes now
      # rather than silently not building it.
      if(type MATCHES "\\.c$")
        message(FATAL_ERROR
          "${typedir}: modules live under a type directory, "
          "modules/<type>/${type}; see cmake/IrcuModules.cmake")
      endif()
      continue()
    endif()

    file(GLOB entries LIST_DIRECTORIES true CONFIGURE_DEPENDS "${typedir}/*")
    list(SORT entries)
    foreach(entry IN LISTS entries)
      get_filename_component(leaf "${entry}" NAME)
      if(leaf MATCHES "^\\.")
        continue()
      endif()

      if(IS_DIRECTORY "${entry}")
        set(name "${leaf}")
        set(subdir "${type}/${name}")
        set(fragment "${entry}/module.cmake")
        set(directory "${entry}")
        _ircu_module_directory_contents("${entry}" sources resources)
        if(NOT sources)
          message(FATAL_ERROR
            "${entry}: a module directory needs at least one .c file")
        endif()
      elseif(leaf MATCHES "\\.c$")
        string(REGEX REPLACE "\\.c$" "" name "${leaf}")
        set(subdir "${type}")
        set(fragment "${typedir}/${name}.cmake")
        set(directory "")
        set(sources "${entry}")
        set(resources "")
      else()
        continue()
      endif()

      # One name, one module: the loader resolves a bare name against every
      # type, so two modules sharing one would be unloadable by that name.
      list(FIND names "${name}" seen)
      if(NOT seen EQUAL -1)
        list(GET origins ${seen} other)
        message(FATAL_ERROR
          "module name \"${name}\" is used twice: ${other} and ${entry}. "
          "A module is loaded by name alone, whatever type it is filed "
          "under, so every name must be unique across modules/")
      endif()
      list(APPEND names "${name}")
      list(APPEND origins "${entry}")

      # The module's own CMake fragment, if it has one: its chance to find
      # the libraries it needs, or to bow out when they are not there.
      set(IRCU_MODULE_NAME "${name}")
      set(IRCU_MODULE_TYPE "${type}")
      set(IRCU_MODULE_DIR "${directory}")
      set(IRCU_MODULE_SKIP "")
      set(IRCU_MODULE_LINK_LIBRARIES "")
      set(IRCU_MODULE_INCLUDE_DIRECTORIES "")
      set(IRCU_MODULE_COMPILE_DEFINITIONS "")
      set(IRCU_MODULE_COMPILE_OPTIONS "")

      if(EXISTS "${fragment}")
        # Re-run the configure step when the fragment changes, the same way
        # the globs above do.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
          "${fragment}")
        include("${fragment}")
      endif()

      if(IRCU_MODULE_SKIP)
        message(STATUS "Module ${name}: not built (${IRCU_MODULE_SKIP})")
        list(APPEND skipped "${name}")
        continue()
      endif()

      if(directory)
        ircu_add_module(${name} SUBDIR "${subdir}" SOURCES ${sources}
          DIRECTORY "${directory}" RESOURCES ${resources}
          LINK_LIBRARIES ${IRCU_MODULE_LINK_LIBRARIES}
          INCLUDE_DIRECTORIES ${IRCU_MODULE_INCLUDE_DIRECTORIES}
          COMPILE_DEFINITIONS ${IRCU_MODULE_COMPILE_DEFINITIONS}
          COMPILE_OPTIONS ${IRCU_MODULE_COMPILE_OPTIONS})
      else()
        ircu_add_module(${name} SUBDIR "${subdir}" SOURCES ${sources}
          LINK_LIBRARIES ${IRCU_MODULE_LINK_LIBRARIES}
          INCLUDE_DIRECTORIES ${IRCU_MODULE_INCLUDE_DIRECTORIES}
          COMPILE_DEFINITIONS ${IRCU_MODULE_COMPILE_DEFINITIONS}
          COMPILE_OPTIONS ${IRCU_MODULE_COMPILE_OPTIONS})
      endif()
      list(APPEND built "${name}")
    endforeach()
  endforeach()

  if(built)
    message(STATUS "Modules: ${built}")
  else()
    message(STATUS "Modules: none")
  endif()
  if(skipped)
    message(STATUS "Modules not built: ${skipped}")
  endif()
endfunction()

# _ircu_module_directory_contents(<dir> <sources_var> <resources_var>)
#
# Sorts everything below a module directory into what gets compiled and
# what gets copied.  Headers are neither: they are included, not shipped.
# Hidden files and directories are skipped, and so are CMakeLists.txt and
# the module's own .cmake fragment, which are build files rather than
# resources whatever else they might be.
function(_ircu_module_directory_contents dir sources_var resources_var)
  file(GLOB_RECURSE files CONFIGURE_DEPENDS "${dir}/*")
  list(SORT files)

  set(sources "")
  set(resources "")
  foreach(file IN LISTS files)
    file(RELATIVE_PATH relative "${dir}" "${file}")
    if(relative MATCHES "(^|/)\\.")
      continue()
    elseif(relative MATCHES "\\.c$")
      list(APPEND sources "${file}")
    elseif(relative MATCHES "\\.h$" OR relative STREQUAL "CMakeLists.txt"
           OR relative MATCHES "\\.cmake$")
      continue()
    else()
      list(APPEND resources "${file}")
    endif()
  endforeach()

  set(${sources_var} "${sources}" PARENT_SCOPE)
  set(${resources_var} "${resources}" PARENT_SCOPE)
endfunction()
