//-----------------------------------------------------------------------------
//
// Configure default lists (sampling / intrumented mode) of hardware counters sent to Tracy
//
//   Sampling mode:
//   ==============
//      Add '-tracy_hw_sampling' to the game command line.
//      In this mode, ETW will use the counter overflow feature of the Performance
//      Monitoring Unit (PMU) - once the counter reaches a specific value (sampling interval)
//      an interrupt fires and the instruction pointer is captured.
//
//      The list of counters is defined using one of the following macros:
//
//          * HW_COUNTER_DEFAULT_LIST_SAMPLING_2( counterId, counterId )
//          * HW_COUNTER_DEFAULT_LIST_SAMPLING_4( counterId, counterId, counterId, counterId )
//      (cf below for description of 'counterId')   
//
//   Instrumented mode:
//   ==================
//      Add '-tracy_hw_events' to the game command line.
//      In this mode, ETW is configured to read the value of hardware counters
//      whenever certian kernel events fire (context switches & system calls)
//      
//      The list of counters is defined using the following macro:
//
//          * HW_COUNTER_DEFAULT_LIST_INSTRUMENTED( counterId )
//
//  
// In both mode, 'counterId' is one of the counter id below:
//      * VALVE_COUNTER_CPU_CYCLES
//      * VALVE_COUNTER_INSTRUCTION
//      * VALVE_COUNTER_BRANCH
//      * VALVE_COUNTER_BRANCH_MISPREDICT
//      * VALVE_COUNTER_L1_CACHE_ACCESS
//      * VALVE_COUNTER_L1_CACHE_MISS
//      * VALVE_COUNTER_L2_CACHE_ACCESS
//      * VALVE_COUNTER_L2_CACHE_MISS
//      * VALVE_COUNTER_CUSTOM_00
//      * VALVE_COUNTER_CUSTOM_01
//      * ...
//      * VALVE_COUNTER_CUSTOM_07
//
// You can only define a single list per mode
//
//-----------------------------------------------------------------------------

// List in sampling mode
//-----------------------------------------------------------------------------

// L2 cache & branches
HW_COUNTER_DEFAULT_LIST_SAMPLING_4( VALVE_COUNTER_L2_CACHE_ACCESS, VALVE_COUNTER_L2_CACHE_MISS, VALVE_COUNTER_BRANCH, VALVE_COUNTER_BRANCH_MISPREDICT )


// Other useful lists (comment line above & uncomment one of the list below):
    // L1 cache & branches
    // HW_COUNTER_DEFAULT_LIST_SAMPLING_4( VALVE_COUNTER_L1_CACHE_ACCESS, VALVE_COUNTER_L1_CACHE_MISS, VALVE_COUNTER_BRANCH, VALVE_COUNTER_BRANCH_MISPREDICT )
    // L2 cache & cpi
    // HW_COUNTER_DEFAULT_LIST_SAMPLING_4( VALVE_COUNTER_L2_CACHE_ACCESS, VALVE_COUNTER_L2_CACHE_MISS, VALVE_COUNTER_CPU_CYCLES, VALVE_COUNTER_INSTRUCTION )
    // L1 cache & cpi
    // HW_COUNTER_DEFAULT_LIST_SAMPLING_4( VALVE_COUNTER_L1_CACHE_ACCESS, VALVE_COUNTER_L1_CACHE_MISS, VALVE_COUNTER_CPU_CYCLES, VALVE_COUNTER_INSTRUCTION )
    // Branches & cpi
    // HW_COUNTER_DEFAULT_LIST_SAMPLING_4( VALVE_COUNTER_BRANCH, VALVE_COUNTER_BRANCH_MISPREDICT, VALVE_COUNTER_CPU_CYCLES, VALVE_COUNTER_INSTRUCTION )
    
    // L1 cache only
    // HW_COUNTER_DEFAULT_LIST_SAMPLING_2( VALVE_COUNTER_L1_CACHE_ACCESS, VALVE_COUNTER_L1_CACHE_MISS )
    // L2 cache only
    // HW_COUNTER_DEFAULT_LIST_SAMPLING_2( VALVE_COUNTER_L2_CACHE_ACCESS, VALVE_COUNTER_L2_CACHE_MISS )
    // Branches only
    // HW_COUNTER_DEFAULT_LIST_SAMPLING_2( VALVE_COUNTER_BRANCH, VALVE_COUNTER_BRANCH_MISPREDICT )
    // cpi only
    // HW_COUNTER_DEFAULT_LIST_SAMPLING_2( VALVE_COUNTER_CPU_CYCLES, VALVE_COUNTER_INSTRUCTION )


// List in instrumented mode
//-----------------------------------------------------------------------------

HW_COUNTER_DEFAULT_LIST_INSTRUMENTED( VALVE_COUNTER_L2_CACHE_MISS )

// Other useful lists (comment line above & uncomment one of the list below):
    // HW_COUNTER_DEFAULT_LIST_INSTRUMENTED( VALVE_COUNTER_L1_CACHE_MISS )

    //HW_COUNTER_DEFAULT_LIST_INSTRUMENTED( VALVE_COUNTER_CPU_CYCLES )


//-----------------------------------------------------------------------------
//
// Micro architecture definitions
//
// A micro architecture is defined as:
//   HW_COUNTER_UARCH_CONFIG( name, manufacturer, def )
//
// where
//   * 'name' is the name of the micro architecture
//   * 'manufacturer' is one of 'AMD' or 'INTEL'
//   * 'def' is the micro architecture definition (family, model, stepping). Use
//     one of the following macros:
//         * UARCH_FAMILY_MODEL_STEPPING      ( family, model, stepping )                 --> exact family, exact model, exact stepping
//         * UARCH_FAMILY_MODEL_RANGE         ( family, minModel, maxModel )              --> exact family, model in range[minModel, maxModel], all stepping
//         * UARCH_FAMILY_MODEL_STEPPING_RANGE( family, model, minStepping, maxStepping ) --> exact family, exact model, stepping in range[minStepping, maxStepping]
//
//-----------------------------------------------------------------------------

// AMD micro architectures
//-----------------------------------------------------------------------------
HW_COUNTER_UARCH_CONFIG( amd_zen2_castlepeak, AMD, UARCH_FAMILY_MODEL_RANGE( 0x17, 0x30, 0x3F ) )
HW_COUNTER_UARCH_CONFIG( amd_zen3_vermeer   , AMD, UARCH_FAMILY_MODEL_RANGE( 0x19, 0x20, 0x2F ) )
HW_COUNTER_UARCH_CONFIG( amd_zen4_raphael   , AMD, UARCH_FAMILY_MODEL_RANGE( 0x19, 0x60, 0x6F ) )

// Intel micro architectures
//-----------------------------------------------------------------------------
HW_COUNTER_UARCH_CONFIG( intel_skylakex, INTEL, UARCH_FAMILY_MODEL_STEPPING_RANGE( 0x6, 0x55, 0, 4 ) )


//-----------------------------------------------------------------------------
//
//  Hardware counter definitions (per micro architecture)
//
// A counter is defined as:
//   HW_COUNTER( uarch, counterId, def, interval, shortName, vendorName, description )
//
// where
//   * 'uarch' is one of the micro architecture defined above
//   * 'counterId' is one of the counter id below:
//           * VALVE_COUNTER_CPU_CYCLES
//           * VALVE_COUNTER_INSTRUCTION
//           * VALVE_COUNTER_BRANCH
//           * VALVE_COUNTER_BRANCH_MISPREDICT
//           * VALVE_COUNTER_L1_CACHE_ACCESS
//           * VALVE_COUNTER_L1_CACHE_MISS
//           * VALVE_COUNTER_L2_CACHE_ACCESS
//           * VALVE_COUNTER_L2_CACHE_MISS
//           * VALVE_COUNTER_CUSTOM_00
//           * VALVE_COUNTER_CUSTOM_01
//           * ...
//           * VALVE_COUNTER_CUSTOM_07
//   * 'def' is the counter definition (event, unit, ...). Use one of the following macro:
//           * AMD_COUNTER_DEF( AmdEventName, Event, Unit )
//           * INTEL_COUNTER_DEF( IntelEventName, Event, Unit, CMask, CMaskInvert, AnyThread, EdgeDetect )
//   * 'interval' is the sampling interval (only used in sampling mode)
//   * 'shortName' is the name of the counter used by Tracy (graph heading, graph units, ...)
//   * 'vendorName' is the name of the counter as it appears in the intel/amd specs. Displayed in Tracy when hovering over hw counter data.
//   * "description' is a short description. Displayed in Tracy when hovering over hw counter data.
//
//
// You can find the list of hardware performance counters supported by each cpu vendor and how to define them on:
//   * Intel: 
//        * https://github.com/intel/perfmon - counters lists available in json format. Find your 
//          micro architecture by Family/Model/Stepping code in https://github.com/intel/perfmon/blob/main/mapfile.csv
//          and open the json file in the corresponding folder.
//   * Amd:
//        * refer to the 'Processor Programming Reference' of the specific processor (PRR - https://developer.amd.com/resources/developer-guides-manuals/)
//        * or install AMD uProf profiler. You can then find the counters lists in xml format - located 
//          at C:\Program Files\AMD\AMDuProf\bin\Data\Events\ (one list per micro architecture)
//
//-----------------------------------------------------------------------------

// AMD Zen2 CastlePeak (eg threadripper 3970x)
//-----------------------------------------------------------------------------
HW_COUNTER( amd_zen2_castlepeak, VALVE_COUNTER_CPU_CYCLES          , AMD_COUNTER_DEF( 0x76, 0x00 ), 64*1024, "cycles"               , CYCLES_NOT_IN_HALT    , "The number of cpu cycles when the thread is not in halt state." )
HW_COUNTER( amd_zen2_castlepeak, VALVE_COUNTER_INSTRUCTION         , AMD_COUNTER_DEF( 0xC0, 0x00 ), 64*1024, "retired instructions" , RETIRED_INST          , "The number of instructions retired from execution. This count includes exceptions and interrupts. Each exception or interrupt is counted as one instruction." )
HW_COUNTER( amd_zen2_castlepeak, VALVE_COUNTER_BRANCH              , AMD_COUNTER_DEF( 0xC2, 0x00 ), 64*1024, "retired branches"     , RETIRED_BR_INST       , "The number of branch instructions retired. This includes all types of architectural control flow changes, including exceptions and interrupts." )
HW_COUNTER( amd_zen2_castlepeak, VALVE_COUNTER_BRANCH_MISPREDICT   , AMD_COUNTER_DEF( 0xC3, 0x00 ), 64*1024, "branch mispredictions", RETIRED_BR_INST_MISP  , "The number of retired branch instructions, that were mispredicted.Note that only EX direct mispredicts and indirect target mispredicts are counted." )
HW_COUNTER( amd_zen2_castlepeak, VALVE_COUNTER_L1_CACHE_ACCESS     , AMD_COUNTER_DEF( 0x29, 0x07 ), 64*1024, "L1 cache accesses"    , L1_DC_ACCESSES_ALL    , "The number of load and store ops dispatched to LS unit. This counts the dispatch of single op that performs a memory load, dispatch of single op that performs a memory store, dispatch of a single op that performs a load from and store to the same memory address." )
HW_COUNTER( amd_zen2_castlepeak, VALVE_COUNTER_L1_CACHE_MISS       , AMD_COUNTER_DEF( 0x60, 0xC8 ), 64*1024, "L1 cache misses"      , L2_CACHE_ACCESS_FROM_L1_DC_MISS, "The L2 cache access requests due to L1 data cache misses. This also counts hardware and software prefetches" )
// Actually the same as L1 cache miss
HW_COUNTER( amd_zen2_castlepeak, VALVE_COUNTER_L2_CACHE_ACCESS     , AMD_COUNTER_DEF( 0x60, 0xC8 ), 64*1024, "L2 cache accesses"    , L2_CACHE_ACCESS_FROM_L1_DC_MISS, "The L2 cache access requests due to L1 data cache misses. This also counts hardware and software prefetches" )
HW_COUNTER( amd_zen2_castlepeak, VALVE_COUNTER_L2_CACHE_MISS       , AMD_COUNTER_DEF( 0x64, 0x08 ), 64*1024, "L2 cache misses"      , L2_CACHE_MISS_FROM_L1_DC_MISS, "Count all the L2 Cache Misses that Misses in L1 Data Cache Miss." )


// AMD Zen3 Vermeer
//-----------------------------------------------------------------------------
HW_COUNTER( amd_zen3_vermeer, VALVE_COUNTER_CPU_CYCLES          , AMD_COUNTER_DEF( 0x76, 0x00 ), 64*1024, "cycles"               , CYCLES_NOT_IN_HALT    , "The number of cpu cycles when the thread is not in halt state." )
HW_COUNTER( amd_zen3_vermeer, VALVE_COUNTER_INSTRUCTION         , AMD_COUNTER_DEF( 0xC0, 0x00 ), 64*1024, "retired instructions" , RETIRED_INST          , "The number of instructions retired from execution. This count includes exceptions and interrupts. Each exception or interrupt is counted as one instruction." )
HW_COUNTER( amd_zen3_vermeer, VALVE_COUNTER_BRANCH              , AMD_COUNTER_DEF( 0xC2, 0x00 ), 64*1024, "retired branches"     , RETIRED_BR_INST       , "The number of branch instructions retired. This includes all types of architectural control flow changes, including exceptions and interrupts." )
HW_COUNTER( amd_zen3_vermeer, VALVE_COUNTER_BRANCH_MISPREDICT   , AMD_COUNTER_DEF( 0xC3, 0x00 ), 64*1024, "branch mispredictions", RETIRED_BR_INST_MISP  , "The number of retired branch instructions, that were mispredicted.Note that only EX direct mispredicts and indirect target mispredicts are counted." )
HW_COUNTER( amd_zen3_vermeer, VALVE_COUNTER_L1_CACHE_ACCESS     , AMD_COUNTER_DEF( 0x29, 0x07 ), 64*1024, "L1 cache accesses"    , L1_DC_ACCESSES_ALL    , "The number of load and store ops dispatched to LS unit. This counts the dispatch of single op that performs a memory load, dispatch of single op that performs a memory store, dispatch of a single op that performs a load from and store to the same memory address." )
HW_COUNTER( amd_zen3_vermeer, VALVE_COUNTER_L1_CACHE_MISS       , AMD_COUNTER_DEF( 0x60, 0xE8 ), 64*1024, "L1 cache misses"      , L2_CACHE_ACCESS_FROM_L1_DC_MISS, "The L2 cache access requests due to L1 data cache misses. This also counts hardware and software prefetches" )
// Actually the same as L1 cache miss
HW_COUNTER( amd_zen3_vermeer, VALVE_COUNTER_L2_CACHE_ACCESS     , AMD_COUNTER_DEF( 0x60, 0xE8 ), 64*1024, "L2 cache accesses"    , L2_CACHE_ACCESS_FROM_L1_DC_MISS, "The L2 cache access requests due to L1 data cache misses. This also counts hardware and software prefetches" )
HW_COUNTER( amd_zen3_vermeer, VALVE_COUNTER_L2_CACHE_MISS       , AMD_COUNTER_DEF( 0x64, 0x08 ), 64*1024, "L2 cache misses"      , L2_CACHE_MISS_FROM_L1_DC_MISS, "Count all the L2 Cache Misses that Misses in L1 Data Cache Miss." )


// AMD Zen4 Raphael
//-----------------------------------------------------------------------------
HW_COUNTER( amd_zen4_raphael, VALVE_COUNTER_CPU_CYCLES          , AMD_COUNTER_DEF( 0x76, 0x00 ), 64*1024, "cycles"               , CYCLES_NOT_IN_HALT    , "The number of cpu cycles when the thread is not in halt state." )
HW_COUNTER( amd_zen4_raphael, VALVE_COUNTER_INSTRUCTION         , AMD_COUNTER_DEF( 0xC0, 0x00 ), 64*1024, "retired instructions" , RETIRED_INST          , "The number of instructions retired from execution. This count includes exceptions and interrupts. Each exception or interrupt is counted as one instruction." )
HW_COUNTER( amd_zen4_raphael, VALVE_COUNTER_BRANCH              , AMD_COUNTER_DEF( 0xC2, 0x00 ), 64*1024, "retired branches"     , RETIRED_BR_INST       , "The number of branch instructions retired. This includes all types of architectural control flow changes, including exceptions and interrupts." )
HW_COUNTER( amd_zen4_raphael, VALVE_COUNTER_BRANCH_MISPREDICT   , AMD_COUNTER_DEF( 0xC3, 0x00 ), 64*1024, "branch mispredictions", RETIRED_BR_INST_MISP  , "The number of retired branch instructions, that were mispredicted.Note that only EX direct mispredicts and indirect target mispredicts are counted." )
HW_COUNTER( amd_zen4_raphael, VALVE_COUNTER_L1_CACHE_ACCESS     , AMD_COUNTER_DEF( 0x29, 0x07 ), 64*1024, "L1 cache accesses"    , L1_DC_ACCESSES_ALL    , "The number of load and store ops dispatched to LS unit. This counts the dispatch of single op that performs a memory load, dispatch of single op that performs a memory store, dispatch of a single op that performs a load from and store to the same memory address." )
HW_COUNTER( amd_zen4_raphael, VALVE_COUNTER_L1_CACHE_MISS       , AMD_COUNTER_DEF( 0x60, 0xE8 ), 64*1024, "L1 cache misses"      , L2_CACHE_ACCESS_FROM_L1_DC_MISS, "The L2 cache access requests due to L1 data cache misses. This also counts hardware and software prefetches" )
// Actually the same as L1 cache miss
HW_COUNTER( amd_zen4_raphael, VALVE_COUNTER_L2_CACHE_ACCESS     , AMD_COUNTER_DEF( 0x60, 0xE8 ), 64*1024, "L2 cache accesses"    , L2_CACHE_ACCESS_FROM_L1_DC_MISS, "The L2 cache access requests due to L1 data cache misses. This also counts hardware and software prefetches" )
HW_COUNTER( amd_zen4_raphael, VALVE_COUNTER_L2_CACHE_MISS       , AMD_COUNTER_DEF( 0x64, 0x08 ), 64*1024, "L2 cache misses"      , L2_CACHE_MISS_FROM_L1_DC_MISS, "Count all the L2 Cache Misses that Misses in L1 Data Cache Miss." )


// Intel SkylakeX
//-----------------------------------------------------------------------------
HW_COUNTER( intel_skylakex, VALVE_COUNTER_CPU_CYCLES            , INTEL_COUNTER_DEF( 0x00, 0x02, 0, 0, 0, 0 ), 64*1024, "cycles"               , CPU_CLK_UNHALTED.THREAD         , "Core cycles when the thread is not in halt state." )
HW_COUNTER( intel_skylakex, VALVE_COUNTER_INSTRUCTION           , INTEL_COUNTER_DEF( 0x00, 0x01, 0, 0, 0, 0 ), 64*1024, "retired instructions" , INST_RETIRED.ANY                , "Instructions retired from execution." )
HW_COUNTER( intel_skylakex, VALVE_COUNTER_BRANCH                , INTEL_COUNTER_DEF( 0xC4, 0x00, 0, 0, 0, 0 ), 64*1024, "retired branches"     , BR_INST_RETIRED.ALL_BRANCHES    , "All (macro) branch instructions retired." )
HW_COUNTER( intel_skylakex, VALVE_COUNTER_BRANCH_MISPREDICT     , INTEL_COUNTER_DEF( 0xC5, 0x00, 0, 0, 0, 0 ), 64*1024, "branch mispredictions", BR_MISP_RETIRED.ALL_BRANCHES    , "All mispredicted macro branch instructions retired." )
//HW_COUNTER( intel_skylakex, VALVE_COUNTER_L1_CACHE_ACCESS       , INTEL_COUNTER_DEF( 0xC5, 0x00, 0, 0, 0, 0 ), 64*1024, "L1 cache accesses"    , BR_MISP_RETIRED.ALL_BRANCHES    , "All mispredicted macro branch instructions retired." )
HW_COUNTER( intel_skylakex, VALVE_COUNTER_L1_CACHE_MISS         , INTEL_COUNTER_DEF( 0x51, 0x01, 0, 0, 0, 0 ), 64*1024, "L1 cache misses"      , L1D.REPLACEMENT                 , "L1D data line replacements." )
// Assuming any l2 data cache access comes from an L1 data cache miss
HW_COUNTER( intel_skylakex, VALVE_COUNTER_L2_CACHE_ACCESS       , INTEL_COUNTER_DEF( 0xD1, 0x08, 0, 0, 0, 0 ), 64*1024, "L2 cache accesses"    , MEM_LOAD_RETIRED.L1_MISS        , "Retired load instructions missed L1 cache as data sources." )
HW_COUNTER( intel_skylakex, VALVE_COUNTER_L2_CACHE_MISS         , INTEL_COUNTER_DEF( 0xD1, 0x10, 0, 0, 0, 0 ), 64*1024, "L2 cache misses"      , MEM_LOAD_RETIRED.L2_MISS        , "Retired load instructions missed L2 cache as data sources." )

