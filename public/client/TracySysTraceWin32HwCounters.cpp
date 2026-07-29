#include "TracySysTraceWin32HwCounters.hpp"

// Note:    File included in TracyClient.cpp (unity build). 
//          File included after TracySysTrace.cpp hence missing include files ...

#if defined( TRACY_HAS_SYSTEM_TRACING ) && defined( _WIN32 )

#include "tier0/platform.h"
#include "tier0/index_helpers.h"

namespace tracy
{

//-----------------------------------------------------------------------------
// Configuring custom hardware counters via NtSetSystemInformation
//  * cf EVENT_TRACE_PROFILE_ADD_INFORMATION in https://github.com/winsiderss/systeminformer/blob/master/phnt/include/ntexapi.h
//  * https://github.com/cmuratori/pmctrace/blob/main/pmctrace_server.cpp
//----------------------------------------------------------------------------- 

typedef enum _EVENT_TRACE_INFORMATION_CLASS
{
    EventTraceKernelVersionInformation, // EVENT_TRACE_VERSION_INFORMATION
    EventTraceGroupMaskInformation, // EVENT_TRACE_GROUPMASK_INFORMATION
    EventTracePerformanceInformation, // EVENT_TRACE_PERFORMANCE_INFORMATION
    EventTraceTimeProfileInformation, // EVENT_TRACE_TIME_PROFILE_INFORMATION
    EventTraceSessionSecurityInformation, // EVENT_TRACE_SESSION_SECURITY_INFORMATION
    EventTraceSpinlockInformation, // EVENT_TRACE_SPINLOCK_INFORMATION
    EventTraceStackTracingInformation, // EVENT_TRACE_STACK_TRACING_INFORMATION
    EventTraceExecutiveResourceInformation, // EVENT_TRACE_EXECUTIVE_RESOURCE_INFORMATION
    EventTraceHeapTracingInformation, // EVENT_TRACE_HEAP_TRACING_INFORMATION
    EventTraceHeapSummaryTracingInformation, // EVENT_TRACE_HEAP_TRACING_INFORMATION
    EventTracePoolTagFilterInformation, // EVENT_TRACE_POOLTAG_FILTER_INFORMATION
    EventTracePebsTracingInformation, // EVENT_TRACE_PEBS_TRACING_INFORMATION
    EventTraceProfileConfigInformation, // EVENT_TRACE_PROFILE_CONFIG_INFORMATION
    EventTraceProfileSourceListInformation, // EVENT_TRACE_PROFILE_LIST_INFORMATION
    EventTraceProfileEventListInformation, // EVENT_TRACE_PROFILE_EVENT_INFORMATION
    EventTraceProfileCounterListInformation, // EVENT_TRACE_PROFILE_COUNTER_INFORMATION
    EventTraceStackCachingInformation, // EVENT_TRACE_STACK_CACHING_INFORMATION
    EventTraceObjectTypeFilterInformation, // EVENT_TRACE_OBJECT_TYPE_FILTER_INFORMATION
    EventTraceSoftRestartInformation, // EVENT_TRACE_SOFT_RESTART_INFORMATION
    EventTraceLastBranchConfigurationInformation, // REDSTONE3
    EventTraceLastBranchEventListInformation, // EVENT_TRACE_PROFILE_EVENT_INFORMATION
    EventTraceProfileSourceAddInformation, // EVENT_TRACE_PROFILE_ADD_INFORMATION
    EventTraceProfileSourceRemoveInformation, // EVENT_TRACE_PROFILE_REMOVE_INFORMATION
    EventTraceProcessorTraceConfigurationInformation,
    EventTraceProcessorTraceEventListInformation, // EVENT_TRACE_PROFILE_EVENT_INFORMATION
    EventTraceCoverageSamplerInformation, // EVENT_TRACE_COVERAGE_SAMPLER_INFORMATION
    EventTraceUnifiedStackCachingInformation, // since 21H1
    EventTraceContextRegisterTraceInformation, // TRACE_CONTEXT_REGISTER_INFO // 24H2
    MaxEventTraceInfoClass
} EVENT_TRACE_INFORMATION_CLASS;

typedef enum _EVENT_TRACE_PROFILE_ADD_INFORMATION_VERSIONS
{
    EventTraceProfileAddInformationMinVersion = 0x2,
    EventTraceProfileAddInformationV2 = 0x2,
    EventTraceProfileAddInformationV3 = 0x3,
    EventTraceProfileAddInformationMaxVersion = 0x3,
} EVENT_TRACE_PROFILE_ADD_INFORMATION_VERSIONS;

typedef union _EVENT_TRACE_PROFILE_ADD_INFORMATION_V2
{
    struct
    {
        UCHAR PerfEvtEventSelect;
        UCHAR PerfEvtUnitSelect;
        UCHAR PerfEvtCMask;
        UCHAR PerfEvtCInv;
        UCHAR PerfEvtAnyThread;
        UCHAR PerfEvtEdgeDetect;
    } Intel;
    struct
    {
        UCHAR PerfEvtEventSelect;
        UCHAR PerfEvtUnitSelect;
    } Amd;
    struct
    {
        ULONG PerfEvtType;
        UCHAR AllowsHalt;
    } Arm;
} EVENT_TRACE_PROFILE_ADD_INFORMATION_V2;

typedef union _EVENT_TRACE_PROFILE_ADD_INFORMATION_V3
{
    struct
    {
        UCHAR PerfEvtEventSelect;
        UCHAR PerfEvtUnitSelect;
        UCHAR PerfEvtCMask;
        UCHAR PerfEvtCInv;
        UCHAR PerfEvtAnyThread;
        UCHAR PerfEvtEdgeDetect;
    } Intel;
    struct
    {
        USHORT PerfEvtEventSelect;
        UCHAR PerfEvtUnitSelect;
        UCHAR PerfEvtCMask;
        UCHAR PerfEvtCInv;
        UCHAR PerfEvtEdgeDetect;
        UCHAR PerfEvtHostGuest;
        UCHAR PerfPmuType;
    } Amd;
    struct
    {
        ULONG PerfEvtType;
        UCHAR AllowsHalt;
    } Arm;
} EVENT_TRACE_PROFILE_ADD_INFORMATION_V3;

typedef struct _EVENT_TRACE_PROFILE_ADD_INFORMATION
{
    EVENT_TRACE_INFORMATION_CLASS EventTraceInformationClass;
    UCHAR Version;
    // TODO V2 & V3 have different sizes !!!
    union
    {
        EVENT_TRACE_PROFILE_ADD_INFORMATION_V2 V2;
        EVENT_TRACE_PROFILE_ADD_INFORMATION_V3 V3;
    };
    ULONG CpuInfoHierarchy[3];
    ULONG InitialInterval;
    BOOLEAN Persist;
    WCHAR ProfileSourceDescription[127];
    UCHAR UnusedPad;
} EVENT_TRACE_PROFILE_ADD_INFORMATION, *PEVENT_TRACE_PROFILE_ADD_INFORMATION;

typedef struct _EVENT_TRACE_PROFILE_REMOVE_INFORMATION
{
    EVENT_TRACE_INFORMATION_CLASS EventTraceInformationClass;
    ULONG ProfileSource;
    ULONG CpuInfoHierarchy[0x3];
} EVENT_TRACE_PROFILE_REMOVE_INFORMATION, *PEVENT_TRACE_PROFILE_REMOVE_INFORMATION;

constexpr auto SystemPerformanceTraceInformation{static_cast<SYSTEM_INFORMATION_CLASS>(0x1f)};


extern "C" typedef NTSTATUS (NTAPI *t_NtSetSystemInformation)( SYSTEM_INFORMATION_CLASS, PVOID, ULONG );

t_NtSetSystemInformation NtSetSystemInformation = (t_NtSetSystemInformation)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "NtSetSystemInformation" );

//-----------------------------------------------------------------------------


enum class HwCounterCpuManufacturer_t : uint8_t
{
    INTEL,
    AMD,
    UNKNOWN
};

struct HwCounter_t
{
    HwCustomCounterId_t m_id;
    const char *m_szShortName;
    const char *m_szDescription;
    
    // ETW counter definition
    EVENT_TRACE_PROFILE_ADD_INFORMATION m_etwDef;
};

struct HwCounterCpuArchitectureConfig_t
{
    HwCounterCpuManufacturer_t m_architecture;
    uint8_t m_family;
    uint8_t m_minModel;
    uint8_t m_maxModel;
    uint8_t m_minStepping;
    uint8_t m_maxStepping;

    // Counter definitions
    HwCounter_t m_counters[IndexHelpers::IndexTypeToRawInteger( HwCustomCounterId_t::VALVE_COUNTER_MAX_COUNT )];

    // Mapping table from HwCustomCounterId_t to ETW Profile Source Id
    // (provided by PROFILE_SOURCE_INFO struct)
    // Only set for the current architecture and set to 0 otherwise.
    ULONG m_mappingToProfileSourceId[ IndexHelpers::IndexTypeToRawInteger( HwCustomCounterId_t::VALVE_COUNTER_MAX_COUNT ) ];
};

// Create the various micro architectures
#define UARCH_FAMILY_MODEL_STEPPING( family, model, stepping )                       .m_family = family, .m_minModel = model, .m_maxModel = model, .m_minStepping = stepping, .m_maxStepping = stepping
#define UARCH_FAMILY_MODEL_RANGE( family, minModel, maxModel )                       .m_family = family, .m_minModel = minModel, .m_maxModel = maxModel, .m_minStepping = 0, .m_maxStepping = 0xff
#define UARCH_FAMILY_MODEL_STEPPING_RANGE( family, model, minStepping, maxStepping ) .m_family = family, .m_minModel = model, .m_maxModel = model, .m_minStepping = minStepping, .m_maxStepping = maxStepping
#define HW_COUNTER_UARCH_CONFIG( A, B, C ) static HwCounterCpuArchitectureConfig_t s_uarch_ ## A = { .m_architecture = HwCounterCpuManufacturer_t::B ,C };
#define HW_COUNTER(...)
#define HW_COUNTER_DEFAULT_LIST_SAMPLING_2(...)
#define HW_COUNTER_DEFAULT_LIST_SAMPLING_4(...)
#define HW_COUNTER_DEFAULT_LIST_INSTRUMENTED(...)
#include "TracySysTraceWin32HwCountersTable.inc"
#undef HW_COUNTER_DEFAULT_LIST_INSTRUMENTED
#undef HW_COUNTER_DEFAULT_LIST_SAMPLING_4
#undef HW_COUNTER_DEFAULT_LIST_SAMPLING_2
#undef HW_COUNTER
#undef HW_COUNTER_UARCH_CONFIG
#undef UARCH_FAMILY_MODEL_STEPPING_RANGE
#undef UARCH_FAMILY_MODEL_RANGE
#undef UARCH_FAMILY_MODEL_STEPPING

// Table of all micro architectures
#define HW_COUNTER_UARCH_CONFIG( A, ... ) &s_uarch_ ## A,
#define HW_COUNTER(...)
#define HW_COUNTER_DEFAULT_LIST_SAMPLING_2(...)
#define HW_COUNTER_DEFAULT_LIST_SAMPLING_4(...)
#define HW_COUNTER_DEFAULT_LIST_INSTRUMENTED(...)
static HwCounterCpuArchitectureConfig_t *s_uarch_allTable[] =
{
#   include "TracySysTraceWin32HwCountersTable.inc"
};
#undef HW_COUNTER_DEFAULT_LIST_INSTRUMENTED
#undef HW_COUNTER_DEFAULT_LIST_SAMPLING_4
#undef HW_COUNTER_DEFAULT_LIST_SAMPLING_2
#undef HW_COUNTER
#undef HW_COUNTER_UARCH_CONFIG

// Micro architecture (and list of all the custom hardware counters)
// of the machine the app is running on.
HwCounterCpuArchitectureConfig_t *s_pCurrentUArch = nullptr;

//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Add all counters found in TracySysTraceWin32HwCountersTable.inc to the 
// corresponding HwCounterCpuArchitectureConfig_t struct
//-----------------------------------------------------------------------------
static void InitializeCountersForAllMicroArchitectures()
{
    // Ensure all the counters have an invalid id to start with
    for ( HwCounterCpuArchitectureConfig_t *pUArch : s_uarch_allTable )
    {
        for ( HwCounter_t &counter : pUArch->m_counters )
        {
            counter.m_id = HwCustomCounterId_t::VALVE_COUNTER_INVALID;
        }

        memset( pUArch->m_mappingToProfileSourceId, 0, sizeof( pUArch->m_mappingToProfileSourceId ) );
    }

    // Add all the counters from TracySysTraceWin32HwCountersTable.inc
#   define HW_COUNTER_DEFAULT_LIST_SAMPLING_2(...)
#   define HW_COUNTER_DEFAULT_LIST_SAMPLING_4(...)
#   define HW_COUNTER_DEFAULT_LIST_INSTRUMENTED(...)
#   define HW_COUNTER_UARCH_CONFIG(...)
#   define AMD_COUNTER_DEF( event, unit )                                       .Amd = { .PerfEvtEventSelect = event, .PerfEvtUnitSelect = unit }  
#   define INTEL_COUNTER_DEF( event, unit, cmask, cinv, anythread, edgedetect ) .Intel = { .PerfEvtEventSelect = event, .PerfEvtUnitSelect = unit, .PerfEvtCMask = cmask, .PerfEvtCInv = cinv, .PerfEvtAnyThread = anythread, .PerfEvtEdgeDetect = edgedetect }   
#   define HW_COUNTER( counterUAarch, counterId, counterDef, counterInterval, counterShortName, counterVendorName, counterDescrition )  \
        {   \
            HwCounter_t *pCounter = &s_uarch_ ## counterUAarch.m_counters[ IndexHelpers::IndexTypeToRawInteger( HwCustomCounterId_t::counterId ) ];   \
            pCounter->m_id = HwCustomCounterId_t::counterId;    \
            pCounter->m_szShortName = counterShortName;      \
            pCounter->m_szDescription = #counterVendorName " - " counterDescrition;      \
            pCounter->m_etwDef =    \
            {   \
                .EventTraceInformationClass = EventTraceProfileSourceAddInformation,    \
                .Version = EventTraceProfileAddInformationV2,    \
                .V2 = { counterDef },   \
                .CpuInfoHierarchy = { 0xffffffff, 0xffffffff, 0xffffffff}, \
                .InitialInterval = counterInterval, \
                .Persist = 0, \
                .ProfileSourceDescription = L###counterId,   \
            };    \
        }

#   include "TracySysTraceWin32HwCountersTable.inc"

#   undef HW_COUNTER_DEFAULT_LIST_INSTRUMENTED
#   undef HW_COUNTER_DEFAULT_LIST_SAMPLING_4
#   undef HW_COUNTER_DEFAULT_LIST_SAMPLING_2
#   undef HW_COUNTER
#   undef INTEL_COUNTER_DEF
#   undef AMD_COUNTER_DEF
#   undef HW_COUNTER_UARCH_CONFIG
}

//-----------------------------------------------------------------------------
// Read the cpu family, model, stepping and try to find the corresponding 
// micro architecture in s_uarch_allTable
// Returns nullptr is not found
//-----------------------------------------------------------------------------
static HwCounterCpuArchitectureConfig_t * DetectCurrentCpuArchitectureConfig()
{
    // Read cpu information about the current machine

    uint32 nFamily;
    uint32 nModel;
    uint32 nStepping;
    CPUInformation::GetIAFamilyModelStepping( &nFamily, &nModel, &nStepping );

    const CPUInformation &cpuInfo = GetCPUInformation();
    HwCounterCpuManufacturer_t manufacturer = HwCounterCpuManufacturer_t::UNKNOWN;
    if ( !strcmp( cpuInfo.m_szProcessorID, "AuthenticAMD" ) )
    {
        manufacturer = HwCounterCpuManufacturer_t::AMD;
    }
    if ( !strcmp( cpuInfo.m_szProcessorID, "GenuineIntel" ) )
    {
        manufacturer = HwCounterCpuManufacturer_t::INTEL;
    }


    // Loop over all predefined architectures

    s_pCurrentUArch = nullptr;
    for ( HwCounterCpuArchitectureConfig_t *pUArch : s_uarch_allTable )
    {
        if ( ( pUArch->m_architecture == manufacturer ) &&
             ( pUArch->m_family == nFamily ) &&
             ( nModel >= pUArch->m_minModel && nModel <= pUArch->m_maxModel ) &&
             ( nStepping >= pUArch->m_minStepping && nStepping <= pUArch->m_maxStepping ) )
        {
            return pUArch;
        }
    }

    return nullptr;
}

//-----------------------------------------------------------------------------
// Removes previously registered hw counters with ETW
// Note that all custom hw counters will be prefixed with 'VALVE_COUNTER_'
// (name will actually be one of 'HwCustomCounterId_t')
//-----------------------------------------------------------------------------
static void DeregisterETWOldCustomHwCounters()
{
    // Queries the list of PMC Profiling sources available
    ULONG nProfileSourceListInfoBufferSize;
    TraceQueryInformation( 0, TraceProfileSourceListInfo, NULL, 0, &nProfileSourceListInfoBufferSize );

    BYTE *pProfileSourceListInfoBuffer = ( BYTE * ) tracy_malloc( nProfileSourceListInfoBufferSize );
    TraceQueryInformation( 0, TraceProfileSourceListInfo, pProfileSourceListInfoBuffer, nProfileSourceListInfoBufferSize, &nProfileSourceListInfoBufferSize );

    // Pre-configure EVENT_TRACE_PROFILE_REMOVE_INFORMATION struct
    // Only 'ProfileSource' will need filling
    EVENT_TRACE_PROFILE_REMOVE_INFORMATION removeProfileSourceInfo;
    removeProfileSourceInfo.EventTraceInformationClass = EventTraceProfileSourceRemoveInformation;
    removeProfileSourceInfo.CpuInfoHierarchy[ 0 ] = 0xffffffff;
    removeProfileSourceInfo.CpuInfoHierarchy[ 1 ] = 0xffffffff;
    removeProfileSourceInfo.CpuInfoHierarchy[ 2 ] = 0xffffffff;

    // Iterate over PMC Profiling sources and remove the ones prefixed with 'VALVE_COUNTER_'
    PROFILE_SOURCE_INFO *pProfileSourceInfo = ( PROFILE_SOURCE_INFO * ) ( pProfileSourceListInfoBuffer );
    while ( pProfileSourceInfo )
    {
        if ( !wcsncmp( pProfileSourceInfo->Description, L"VALVE_COUNTER_", 14 ) )
        {
            removeProfileSourceInfo.ProfileSource = pProfileSourceInfo->Source;
            NtSetSystemInformation( SystemPerformanceTraceInformation, &removeProfileSourceInfo, sizeof( removeProfileSourceInfo ) );
        }

        // Move to the next source info
        if ( pProfileSourceInfo->NextEntryOffset != 0 )
        {
            pProfileSourceInfo = ( PROFILE_SOURCE_INFO * ) ( ( BYTE * ) pProfileSourceInfo + pProfileSourceInfo->NextEntryOffset );
        }
        else
        {
            pProfileSourceInfo = nullptr;
        }
    }

    tracy_free( pProfileSourceListInfoBuffer );
}

//-----------------------------------------------------------------------------
// Register all custom hw counters for the given architecture with ETW
//-----------------------------------------------------------------------------
static void RegisterETWCustomHwCounters( const HwCounterCpuArchitectureConfig_t *pUArch )
{
    if ( !pUArch )
        return;
    
    for ( const HwCounter_t &counter : pUArch->m_counters )
    {
        if ( counter.m_id == HwCustomCounterId_t::VALVE_COUNTER_INVALID )
            continue;

        NtSetSystemInformation( SystemPerformanceTraceInformation, (PVOID)&counter.m_etwDef, sizeof(counter.m_etwDef));
    }
}

//-----------------------------------------------------------------------------
// Create mapping table from HwCustomCounterId_t to ETW Profile Source Id.
// Profile Source Ids are given by calling 'TraceQueryInformation' with the 
// infromation class set to 'TraceProfileSourceListInfo'
//-----------------------------------------------------------------------------
static void CreateETWProfileSourceIdMapping( HwCounterCpuArchitectureConfig_t *pUArch )
{
    if ( !pUArch )
        return;

    // Queries the list of PMC Profiling sources available
    ULONG nProfileSourceListInfoBufferSize;
    TraceQueryInformation( 0, TraceProfileSourceListInfo, NULL, 0, &nProfileSourceListInfoBufferSize );

    BYTE *pProfileSourceListInfoBuffer = ( BYTE * ) tracy_malloc( nProfileSourceListInfoBufferSize );
    TraceQueryInformation( 0, TraceProfileSourceListInfo, pProfileSourceListInfoBuffer, nProfileSourceListInfoBufferSize, &nProfileSourceListInfoBufferSize );

    // Iterate over PMC Profiling sources and and find the ones corresponding to a counter for the given micro architecture.
    PROFILE_SOURCE_INFO *pProfileSourceInfo = ( PROFILE_SOURCE_INFO * ) ( pProfileSourceListInfoBuffer );
    while ( pProfileSourceInfo )
    {
        for ( const HwCounter_t &counter : pUArch->m_counters )
        {
            if ( lstrcmpW( pProfileSourceInfo->Description, counter.m_etwDef.ProfileSourceDescription ) == 0 )
            {
                pUArch->m_mappingToProfileSourceId[ IndexHelpers::IndexTypeToRawInteger( counter.m_id ) ] = pProfileSourceInfo->Source;
                break;
            }
        }

        // Move to the next source info
        if ( pProfileSourceInfo->NextEntryOffset != 0 )
        {
            pProfileSourceInfo = ( PROFILE_SOURCE_INFO * ) ( ( BYTE * ) pProfileSourceInfo + pProfileSourceInfo->NextEntryOffset );
        }
        else
        {
            pProfileSourceInfo = nullptr;
        }
    }

    tracy_free( pProfileSourceListInfoBuffer );
}

//-----------------------------------------------------------------------------
void SysTraceConfigureEtwCustomHwCounters()
{
    InitializeCountersForAllMicroArchitectures();

    s_pCurrentUArch = DetectCurrentCpuArchitectureConfig();

    DeregisterETWOldCustomHwCounters();
    RegisterETWCustomHwCounters( s_pCurrentUArch );

    CreateETWProfileSourceIdMapping( s_pCurrentUArch );

    // Send hw counter name & description to Tracy
    const HwCustomCounterId_t *pCounterEventsList = SysTraceGetCounterList_EventsMode();
    for ( int nCounter = 0; nCounter < HW_COUNTER_EVENTS_MAX_COUNT; ++nCounter )
    {
        // TODO Change code below if HW_COUNTER_EVENTS_MAX_COUNT > 1

        if ( !s_pCurrentUArch )
            continue;

        const HwCounter_t &counter = s_pCurrentUArch->m_counters[ IndexHelpers::IndexTypeToRawInteger( pCounterEventsList[ nCounter ] ) ];
        if ( counter.m_id == HwCustomCounterId_t::VALVE_COUNTER_INVALID )
            continue;
        
        TracyLfqPrepare( QueueType::HwCounterConfig );
        MemWrite( &item->hwCounterConfig.name, (uint64_t)counter.m_szShortName );
        MemWrite( &item->hwCounterConfig.description, (uint64_t)counter.m_szDescription );

#ifdef TRACY_ON_DEMAND
        GetProfiler().DeferItem( *item );
#endif

        TracyLfqCommit;
    }
}

//-----------------------------------------------------------------------------
// Returns the ETW Profile Source Id for the given counter.
// Returns 0 if the counter has not been configured with ETW
//-----------------------------------------------------------------------------
ULONG SysTraceGetCounterETWProfileSourceId( HwCustomCounterId_t id )
{
    if ( !s_pCurrentUArch || ( id == HwCustomCounterId_t::VALVE_COUNTER_INVALID ) )
        return 0;

    return s_pCurrentUArch->m_mappingToProfileSourceId[ IndexHelpers::IndexTypeToRawInteger( id ) ];
}

//-----------------------------------------------------------------------------
// Returns the list of hw counters to enable in sampling mode
// Returned array will have 'HW_COUNTER_SAMPLING_MAX_COUNT' elements
//-----------------------------------------------------------------------------
const HwCustomCounterId_t *SysTraceGetCounterList_SamplingMode()
{
    // Create the list from TracySysTraceWin32HwCountersTable.inc
#   define HW_COUNTER_UARCH_CONFIG(...)
#   define HW_COUNTER(...)
#   define HW_COUNTER_DEFAULT_LIST_SAMPLING_2( counterId1, counterId2 ) static HwCustomCounterId_t s_samplingHwCounters[] = { HwCustomCounterId_t::counterId1, HwCustomCounterId_t::counterId2, HwCustomCounterId_t::VALVE_COUNTER_INVALID, HwCustomCounterId_t::VALVE_COUNTER_INVALID };
#   define HW_COUNTER_DEFAULT_LIST_SAMPLING_4( counterId1, counterId2, counterId3, counterId4 ) static HwCustomCounterId_t s_samplingHwCounters[] = { HwCustomCounterId_t::counterId1, HwCustomCounterId_t::counterId2, HwCustomCounterId_t::counterId3, HwCustomCounterId_t::counterId4 };
#   define HW_COUNTER_DEFAULT_LIST_INSTRUMENTED(...)
#   include "TracySysTraceWin32HwCountersTable.inc"
#   undef HW_COUNTER_DEFAULT_LIST_INSTRUMENTED
#   undef HW_COUNTER_DEFAULT_LIST_SAMPLING_4
#   undef HW_COUNTER_DEFAULT_LIST_SAMPLING_2
#   undef HW_COUNTER
#   undef HW_COUNTER_UARCH_CONFIG

    static_assert( ( sizeof( s_samplingHwCounters ) / sizeof( s_samplingHwCounters[ 0 ] ) ) == HW_COUNTER_SAMPLING_MAX_COUNT );

    return s_samplingHwCounters;
}

//-----------------------------------------------------------------------------
// Returns the list of hw counters to be collected on ETW events (cswitch, syscall, ...)
// Returned array will have 'HW_COUNTER_EVENTS_MAX_COUNT' elements
//-----------------------------------------------------------------------------
const HwCustomCounterId_t *SysTraceGetCounterList_EventsMode()
{
    // Create the list from TracySysTraceWin32HwCountersTable.inc
#   define HW_COUNTER_UARCH_CONFIG(...)
#   define HW_COUNTER(...)
#   define HW_COUNTER_DEFAULT_LIST_SAMPLING_2(...)
#   define HW_COUNTER_DEFAULT_LIST_SAMPLING_4(...)
#   define HW_COUNTER_DEFAULT_LIST_INSTRUMENTED( counterId ) static HwCustomCounterId_t s_eventsHwCounters[] = { HwCustomCounterId_t::counterId };
#   include "TracySysTraceWin32HwCountersTable.inc"
#   undef HW_COUNTER_DEFAULT_LIST_INSTRUMENTED
#   undef HW_COUNTER_DEFAULT_LIST_SAMPLING_4
#   undef HW_COUNTER_DEFAULT_LIST_SAMPLING_2
#   undef HW_COUNTER
#   undef HW_COUNTER_UARCH_CONFIG

    static_assert( ( sizeof( s_eventsHwCounters ) / sizeof( s_eventsHwCounters[ 0 ] ) ) == HW_COUNTER_EVENTS_MAX_COUNT );
    
    return s_eventsHwCounters;
}

}   // namespace tracy

#endif  // #if defined( TRACY_HAS_SYSTEM_TRACING ) && defined( _WIN32 )
