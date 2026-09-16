#
# modules/workers/http/module.cmake
#
# The listener is a dedicated worker, so this module needs the pthreads the
# ircd already uses; asking for it here keeps it linkable on the platforms
# that want an explicit -pthread.  Nothing else: the parser, the renderer
# and the socket are this module's own, because an HTTP library would be a
# dependency the ircd does not have and should not grow.
#
# See cmake/IrcuModules.cmake for the variables this may set.  Note that it
# must not call return(): this file is include()d from inside a function, and
# a return here would end the whole module scan rather than this fragment.

find_package(Threads QUIET)

if(Threads_FOUND)
  list(APPEND IRCU_MODULE_LINK_LIBRARIES Threads::Threads)
endif()
