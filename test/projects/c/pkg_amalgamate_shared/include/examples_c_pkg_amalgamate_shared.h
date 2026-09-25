#ifndef EXAMPLES_C_PKG_AMALGAMATE_SHARED_H
#define EXAMPLES_C_PKG_AMALGAMATE_SHARED_H

#ifndef examples_c_pkg_amalgamate_shared_STATIC
#if defined(examples_c_pkg_amalgamate_shared_EXPORTS) && (defined(_MSC_VER) || defined(__MINGW32__))
  #define EXAMPLES_SHARED_API __declspec(dllexport)
#elif defined(examples_c_pkg_amalgamate_shared_EXPORTS)
  #define EXAMPLES_SHARED_API __attribute__((__visibility__("default")))
#elif defined(_MSC_VER)
  #define EXAMPLES_SHARED_API __declspec(dllimport)
#else
  #define EXAMPLES_SHARED_API
#endif
#else
  #define EXAMPLES_SHARED_API
#endif

#ifndef EXAMPLES_SHARED_CUSTOM
#define EXAMPLES_SHARED_EXTRA
#endif

EXAMPLES_SHARED_API
int examples_shared_value(void);

#ifdef EXAMPLES_SHARED_EXTRA
EXAMPLES_SHARED_API
int examples_shared_extra(void);
#endif

#endif
