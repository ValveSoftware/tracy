#ifndef __TRACYLOCKHELPERS_HPP__
#define __TRACYLOCKHELPERS_HPP__

#include <stdint.h>

#include "../public/common/TracyForceInline.hpp"
#include "TracyEvent.hpp"

namespace tracy
{

static tracy_force_inline size_t GetThreadBit( uint8_t thread )
{
    return thread;
}

static tracy_force_inline bool IsThreadWaiting( ThreadWaitList bitlist, size_t threadBit )
{
    return bitlist.Test( threadBit );
}

static tracy_force_inline bool AreOtherWaiting( ThreadWaitList bitlist, size_t threadBit )
{
    return bitlist.Reset(threadBit).Any();
}

}

#endif
