# AX_CXX_COMPILE_STDCXX_20
# Check if C++ compiler supports C++20

AC_DEFUN([AX_CXX_COMPILE_STDCXX_20], [
    AC_CACHE_CHECK([whether the C++ compiler supports C++20],
        [ax_cv_cxx_compile_cxx20],
        [AC_LANG_PUSH([C++])
        AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
#if __cplusplus < 202002L
#error "C++20 required"
#endif
int main() { return 0; }
        ]])],
            [ax_cv_cxx_compile_cxx20=yes],
            [ax_cv_cxx_compile_cxx20=no])
        AC_LANG_POP])

    if test "x$ax_cv_cxx_compile_cxx20" != "xyes"; then
        AC_MSG_ERROR([C++20 compiler is required but not found])
    fi

    # Set C++20 flag based on compiler
    case "$CXX" in
        *g++* | *clang++*)
            CXXFLAGS="$CXXFLAGS -std=c++20"
            ;;
        *icpc*)
            CXXFLAGS="$CXXFLAGS -std=c++20"
            ;;
        *)
            CXXFLAGS="$CXXFLAGS -std=c++20"
            ;;
    esac
])
