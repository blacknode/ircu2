#
# modules/workers/sendmail/module.cmake
#
# The default mail provider needs nothing: it runs the local MTA, which is
# a program and not a library.  The fragment exists only for the threads --
# the program is run on a worker, never on the main thread -- so that the
# module stays linkable on the platforms that want an explicit -pthread.
#
# See cmake/IrcuModules.cmake for the variables this may set.  Note that it
# must not call return(): this file is include()d from inside a function,
# and a return here would end the whole module scan rather than this
# fragment.

find_package(Threads QUIET)

if(Threads_FOUND)
  list(APPEND IRCU_MODULE_LINK_LIBRARIES Threads::Threads)
endif()
