#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.63])
AC_INIT([clearsync], [1.7], [http://www.clearfoundation.com/docs/developer/bug_tracker/])
AM_INIT_AUTOMAKE
AC_CONFIG_SRCDIR([csconf.cpp])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
AC_PROG_CXX
AC_PROG_CC
AC_PROG_INSTALL
AM_PROG_LIBTOOL
AC_PROG_MKDIR_P

# Checks for libraries.
AC_CHECK_LIB([rt], [timer_create], [], [AC_MSG_ERROR([librt not found.])])
AC_CHECK_LIB([dl], [dlopen], [], [AC_MSG_ERROR([libdl not found.])])
AC_CHECK_LIB([expat], [XML_ParserCreate], [], [AC_MSG_ERROR([libexpat not found.])])
AC_CHECK_LIB([pthread], [pthread_create], [], [AC_MSG_ERROR([libpthread not found.])])
AC_CHECK_LIB([crypto], [CRYPTO_set_locking_callback], [], [AC_MSG_ERROR([libcrypto not found.])])

# Check for optional parameters
AC_ARG_ENABLE(debug, AS_HELP_STRING([--enable-debug], [Enable heavy debug output]))

AS_IF([test "x$enable_debug" = "xyes"], [
    AC_DEFINE([_CS_DEBUG], [1], [Define to enable heavy debug output.])
])

# Checks for header files.
AC_CHECK_HEADERS([limits.h netdb.h netinet/in.h stddef.h stdint.h stdlib.h string.h sys/ioctl.h sys/file.h sys/socket.h sys/stat.h sys/time.h syslog.h unistd.h])

AC_CHECK_HEADERS([linux/netlink.h linux/rtnetlink.h], [], [],
[[#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
]])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT32_T
AC_TYPE_UINT8_T
AC_TYPE_UID_T

# Checks for library functions.
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_CHECK_FUNCS([clock_gettime endpwent getpagesize gettimeofday localtime_r memset pathconf regcomp socket strcasecmp strerror])

AC_CONFIG_FILES([Makefile clearsync.spec])
AC_OUTPUT
