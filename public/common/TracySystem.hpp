#ifndef __TRACYSYSTEM_HPP__
#define __TRACYSYSTEM_HPP__

#include <stdint.h>

#include "TracyApi.h"

namespace tracy
{

namespace detail
{
TRACY_API uint32_t GetThreadHandleImpl();
}

struct TracyThreadName
{
    enum { MaxLength = 128 };
    
    uint32_t id;
    int32_t groupHint;

    size_t len;
    char str[ MaxLength + 1 ];
};

#ifdef TRACY_ENABLE
TRACY_API uint32_t GetThreadHandle();
#else
static inline uint32_t GetThreadHandle()
{
    return detail::GetThreadHandleImpl();
}
#endif

TRACY_API void SetThreadName( const char* name );
TRACY_API void SetThreadNameWithHint( const char* name, int32_t groupHint );
TRACY_API void GetThreadName( uint32_t id, TracyThreadName *pName );

TRACY_API const char* GetEnvVar( const char* name );

}

#endif
