#
# Example modules.
#
# These are built and installed alongside the server so there is always a
# working reference for module authors, and so CI compiles the module API on
# every change to it.
#
# Every .c file here is built as a module named after the file; adding one
# needs no change to this file.  A module spanning several sources is the
# exception: list it in EXCLUDE below and add an ircu_add_module() call.
#
include(IrcuModules)

ircu_add_modules()
