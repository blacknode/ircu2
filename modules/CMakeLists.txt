#
# Example modules.
#
# These are built and installed alongside the server so there is always a
# working reference for module authors, and so CI compiles the module API on
# every change to it.
#
include(IrcuModules)

ircu_add_module(example_cmd example_cmd.c)
ircu_add_module(trace_events trace_events.c)
