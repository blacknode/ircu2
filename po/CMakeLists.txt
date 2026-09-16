#
# IRC - Internet Relay Chat, po/CMakeLists.txt
#
# The core translation catalogs.  Every <code>.po here is installed into
# IRCU_POPATH (PO_PATH in config.h), where the server reads it as the
# "core" domain; see doc/readme.translations.  Nothing is compiled: the
# .po is what is installed and what is read.
#
# The `pot` target regenerates the templates with xgettext when it is
# installed.  It is a development convenience, never a build dependency:
# a tree without gettext builds and installs the same catalogs.
#

file(GLOB IRCU_PO_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.po")
list(SORT IRCU_PO_FILES)

install(FILES ${IRCU_PO_FILES} DESTINATION "${IRCU_POPATH}")

if(IRCU_PO_FILES)
  message(STATUS "Translations: ${IRCU_PO_FILES}")
endif()

# ---------------------------------------------------------------------------
# Templates
#
# The keywords are the marking macros of include/ircd_i18n.h: _(to, s),
# _n(to, s, p, n), N_(s), and N(sym, ctx, fmt) -- the s_err.c table, whose
# numeric code is the msgctxt -- plus the two functions that translate a
# literal for their caller, svc_reply() and nick_tell().  no-c-format keeps the tools from applying
# the C printf check to formats that are ircd_snprintf's: the check that
# matters is the loader's, at run time.
# ---------------------------------------------------------------------------
find_program(IRCU_XGETTEXT xgettext)

if(IRCU_XGETTEXT)
  set(_xgettext_flags
    --language=C --from-code=UTF-8 --add-comments=TRANSLATORS
    --keyword=_:2 --keyword=_n:2,3 --keyword=N_ --keyword=N:2c,3
    --keyword=svc_reply:2 --keyword=nick_tell:2
    --flag=_:2:no-c-format --flag=_n:2:no-c-format --flag=_n:3:no-c-format
    --flag=N_:1:no-c-format --flag=N:3:no-c-format
    --flag=svc_reply:2:no-c-format --flag=nick_tell:2:no-c-format
    --no-wrap --sort-by-file
    --package-name=ircu2 --msgid-bugs-address=coder-com@undernet.org)

  # Paths relative to the tree, so the "#:" references in the template do
  # not name anybody's home directory.
  function(_ircu_relative_sources out)
    set(rel "")
    foreach(src IN LISTS ARGN)
      file(RELATIVE_PATH r "${PROJECT_SOURCE_DIR}" "${src}")
      list(APPEND rel "${r}")
    endforeach()
    set(${out} "${rel}" PARENT_SCOPE)
  endfunction()

  file(GLOB _core_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/ircd/*.c")
  list(SORT _core_sources)
  _ircu_relative_sources(_core_sources ${_core_sources})

  set(_pot_commands
    COMMAND "${IRCU_XGETTEXT}" ${_xgettext_flags}
            --default-domain=core
            -o "po/core.pot"
            ${_core_sources})
  set(_pot_outputs "po/core.pot")

  # One template per module that has a po/ directory of its own.
  file(GLOB _module_po_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/modules/*/*/po")
  foreach(_po_dir IN LISTS _module_po_dirs)
    if(NOT IS_DIRECTORY "${_po_dir}")
      continue()
    endif()
    get_filename_component(_mod_dir "${_po_dir}" DIRECTORY)
    get_filename_component(_mod_name "${_mod_dir}" NAME)
    file(GLOB_RECURSE _mod_sources CONFIGURE_DEPENDS "${_mod_dir}/*.c")
    list(SORT _mod_sources)
    _ircu_relative_sources(_mod_sources ${_mod_sources})
    file(RELATIVE_PATH _po_rel "${PROJECT_SOURCE_DIR}" "${_po_dir}")
    list(APPEND _pot_commands
      COMMAND "${IRCU_XGETTEXT}" ${_xgettext_flags}
              --default-domain=${_mod_name}
              -o "${_po_rel}/${_mod_name}.pot"
              ${_mod_sources})
    list(APPEND _pot_outputs "${_po_rel}/${_mod_name}.pot")
  endforeach()

  add_custom_target(pot
    ${_pot_commands}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Regenerating translation templates: ${_pot_outputs}"
    VERBATIM)
else()
  add_custom_target(pot
    COMMAND "${CMAKE_COMMAND}" -E echo
      "xgettext was not found when configuring;
      install gettext and re-run cmake"
    COMMENT "xgettext is not available"
    VERBATIM)
endif()
