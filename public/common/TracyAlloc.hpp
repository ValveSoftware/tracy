#ifndef __TRACYALLOC_HPP__
#define __TRACYALLOC_HPP__

#include <memory>
#include <stdlib.h>
#include <type_traits>
#include <stddef.h>
#include <utility>

#if defined TRACY_ENABLE && !defined __EMSCRIPTEN__
#  include "TracyApi.h"
#  include "TracyForceInline.hpp"
#  include "../client/tracy_rpmalloc.hpp"
#  define TRACY_USE_RPMALLOC
#endif

namespace tracy
{

#ifdef TRACY_USE_RPMALLOC
TRACY_API void InitRpmalloc();
#else
static inline void InitRpmalloc() {}
#endif

static inline void* tracy_malloc( size_t size )
{
#ifdef TRACY_USE_RPMALLOC
    InitRpmalloc();
    return rpmalloc( size );
#else
    return malloc( size );
#endif
}

static inline void* tracy_malloc_fast( size_t size )
{
#ifdef TRACY_USE_RPMALLOC
    return rpmalloc( size );
#else
    return malloc( size );
#endif
}

static inline void tracy_free( void* ptr )
{
#ifdef TRACY_USE_RPMALLOC
    InitRpmalloc();
    rpfree( ptr );
#else
    free( ptr );
#endif
}

static inline void tracy_free_fast( void* ptr )
{
#ifdef TRACY_USE_RPMALLOC
    rpfree( ptr );
#else
    free( ptr );
#endif
}

static inline void* tracy_realloc( void* ptr, size_t size )
{
#ifdef TRACY_USE_RPMALLOC
    InitRpmalloc();
    return rprealloc( ptr, size );
#else
    return realloc( ptr, size );
#endif
}


template < typename T, bool FastAlloc = false >
struct StlAllocator
{
    using value_type = T;

    using pointer = T*;
    using const_pointer = const T*;

    using reference = T&;
    using const_reference = const T&;

    using size_type       = size_t;
    using difference_type = ptrdiff_t;

    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    template <class U>
    struct rebind
    {
        using other = StlAllocator<U>;
    };

    T* address( T& val ) const noexcept
    {
        return std::addressof( val );
    }

    const T* address( const T& val ) const noexcept
    {
        return std::addressof( val );
    }

    constexpr StlAllocator() noexcept {}

    constexpr StlAllocator(const StlAllocator&) noexcept = default;
    template <class _Other>
    constexpr StlAllocator(const StlAllocator<_Other>&) noexcept {}
    ~StlAllocator() = default;
    StlAllocator& operator=(const StlAllocator&) = default;

    void deallocate( T* const ptr, const size_t count )
    {
        if ( FastAlloc )
        {
            tracy_free_fast( ptr );
        }
        else
        {
            tracy_free( ptr );
        }
    }

    T* allocate( const size_t count )
    {
        if ( FastAlloc )
        {
            return static_cast<T*>( tracy_malloc_fast( count * sizeof(T) ) );
        }
        else
        {
            return static_cast<T*>( tracy_malloc( count * sizeof(T) ) );
        }
    }

    T* allocate( const size_t count, const void* )
    {
        return allocate( count );
    }

    template <class U, class... Types >
    void construct( U* const ptr , Types &&... args)
    {
        ::new (const_cast<void*>(static_cast<const volatile void*>( ptr ))) U( std::forward<Types>(args)...);
    }

    template <class U>
    void destroy( U* const ptr )
    {
        ptr->~U();
    }

    size_t max_size() const noexcept
    {
        return static_cast<size_t>(-1) / sizeof(T);
    }
};


}

#endif
