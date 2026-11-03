/* LAME 3.100 build configuration, written for Quire's CMake build in place
   of the one autotools generates (config.h.in). The same on Linux, macOS
   and Windows: C99 headers, IEEE 754 floats, no assembly, no SSE (so every
   platform runs the same C code), no decoder (mpglib) and no frontends. */
#ifndef LAME_CONFIG_H
#define LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1
#define PROTOTYPES 1
#define USE_FAST_LOG 1
#define TAKEHIRO_IEEE754_HACK 1
#define PACKAGE "lame"
#define VERSION "3.100"

typedef float ieee754_float32_t;
typedef double ieee754_float64_t;

#endif
