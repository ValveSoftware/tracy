#ifndef __TRACYSYSTRACEWIN32HWCOUNTERS_HPP__
#define __TRACYSYSTRACEWIN32HWCOUNTERS_HPP__
#pragma once

#if defined( _WIN32 )

#include <stdint.h>

namespace tracy
{

// Ensure the id is prefix by 'VALVE_COUNTER_' as it will be the name used to register
// the custom hw counter with ETW. 'VALVE_COUNTER_' prefix will identify which counters
// need to de-registered with ETW. 
enum class HwCustomCounterId_t : uint8_t
{
    VALVE_COUNTER_INVALID = 0xFF,
    
    VALVE_COUNTER_CPU_CYCLES = 0,
    VALVE_COUNTER_INSTRUCTION,

    VALVE_COUNTER_BRANCH,
    VALVE_COUNTER_BRANCH_MISPREDICT,

    VALVE_COUNTER_L1_CACHE_ACCESS,
    VALVE_COUNTER_L1_CACHE_MISS,
    VALVE_COUNTER_L2_CACHE_ACCESS,
    VALVE_COUNTER_L2_CACHE_MISS,

    VALVE_COUNTER_CUSTOM_00,
    VALVE_COUNTER_CUSTOM_01,
    VALVE_COUNTER_CUSTOM_02,
    VALVE_COUNTER_CUSTOM_03,
    VALVE_COUNTER_CUSTOM_04,
    VALVE_COUNTER_CUSTOM_05,
    VALVE_COUNTER_CUSTOM_06,
    VALVE_COUNTER_CUSTOM_07,

    VALVE_COUNTER_MAX_COUNT
};

// Maximum number of hw counters to enable in sampling mode
constexpr int HW_COUNTER_SAMPLING_MAX_COUNT = 4;
// Maximun number of hw counters to be collected on ETW events (cswitch, syscall ...)
constexpr int HW_COUNTER_EVENTS_MAX_COUNT = 1;

void SysTraceConfigureEtwCustomHwCounters();

// Returns the ETW Profile Source Id for the given counter.
// Returns 0 if the counter has not been configured with ETW
ULONG SysTraceGetCounterETWProfileSourceId( HwCustomCounterId_t id );

// Returns the list of hw counters to enable in sampling mode
// Returned array will have 'HW_COUNTER_SAMPLING_MAX_COUNT' elements
const HwCustomCounterId_t *SysTraceGetCounterList_SamplingMode();

// Returns the list of hw counters to be collected on ETW events (cswitch, syscall, ...)
// Returned array will have 'HW_COUNTER_EVENTS_MAX_COUNT' elements
const HwCustomCounterId_t *SysTraceGetCounterList_EventsMode();
}

#endif  // _WIN32

#endif  // __TRACYSYSTRACEWIN32HWCOUNTERS_HPP__
