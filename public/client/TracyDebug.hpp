#ifndef __TRACYDEBUG_HPP__
#define __TRACYDEBUG_HPP__

#ifdef TRACY_VERBOSE
#  include <stdio.h>
#  define TracyDebug(...) fprintf( stderr, __VA_ARGS__ );
#else
#  define TracyDebug(...)
#endif

#endif
