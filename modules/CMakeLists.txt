#
# Example modules.
#
# These are built and installed alongside the server so there is always a
# working reference for module authors, and so CI compiles the module API on
# every change to it.
#
# The tree is organised by kind of module -- modules/<type>/ -- and every
# entry under a type is one module: a single <name>.c, or a <name>/
# directory holding its sources, private headers and resources.  Adding a
# module needs no change to this file; see cmake/IrcuModules.cmake for the
# layout and what the build makes of it.
#
include(IrcuModules)

ircu_add_modules()
