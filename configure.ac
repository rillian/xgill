AC_PREREQ(2.57)
AC_INIT(sixgill, 0.10, [bhackett at stanford.edu])
AC_CONFIG_HEADER([config.h])

AC_PROG_CC
AC_PROG_CXX

dnl *** check for zlib ***
AC_CHECK_HEADERS( zlib.h, [], [AC_MSG_ERROR(Zlib must be installed)] )

dnl ***********************
dnl CVC3 support
dnl ***********************
AC_LANG(C++)
AC_CHECK_HEADERS( cvc3/vc.h )
LDFLAGS_SAVED=$LDFLAGS
LDFLAGS="$LDFLAGS -lcvc3"
AC_LINK_IFELSE(
    [AC_LANG_PROGRAM([#include <cvc3/vc.h>], 
      [CVC3::Expr expr])],
    [AC_DEFINE(HAVE_CVC3, 1, [Define if you want to use CVC3])]
    [CVC3_LIBS="-lcvc3"]
    [USE_CVC3="yes"],
    [AC_MSG_WARN([cannot find CVC3 libraries])]
    [USE_CVC3="no"])
LDFLAGS=$LDFLAGS_SAVED
AC_SUBST(CVC3_LIBS)
AC_SUBST(USE_CVC3)

dnl ***********************
dnl Yices support
dnl ***********************
AC_ARG_WITH([yices],
            [AS_HELP_STRING([--with-yices=DIR],
              [Enable support for Yices from directory DIR])],
            [YICES_DIR=$withval])
if test "x$YICES_DIR" != "x" ; then
     AC_CHECK_FILE( ${YICES_DIR}/lib/libyices.a, [USE_YICES="yes"], [USE_YICES="no"] )
else
     [ USE_YICES="no" ] 
fi
AC_SUBST(USE_YICES)
AC_SUBST(YICES_DIR)

dnl ***********************
dnl GCC plugin support
dnl ***********************
GCC_PLUGIN_SUPPORT=no
GCC_PLUGIN_HEADERS=`$CC -print-file-name=plugin/include | awk '{print $NF}'`
# if we can't find the plugin/include path, we just get back "plugin/include"
if test "x$GCC_PLUGIN_HEADERS" != "xplugin/include" ; then
   GCC_PLUGIN_SUPPORT=yes
else
   AC_MSG_WARN([GCC plugin support not available, consider modifying your path to include gcc 4.5 or later])
fi
AC_SUBST(GCC_PLUGIN_SUPPORT)
AC_SUBST(GCC_PLUGIN_HEADERS)

dnl ***********************
dnl Results
dnl ***********************
AC_CONFIG_FILES([config.mk])

AC_OUTPUT

AC_MSG_RESULT([

===============================================================
sixgill configuration (Please review)

           * Install:
             - prefix:                  $prefix

           * Solvers:
             - CVC3:                    $USE_CVC3
             - Yices:                   $USE_YICES

           * GCC Plugin:                $GCC_PLUGIN_SUPPORT

===============================================================

])

