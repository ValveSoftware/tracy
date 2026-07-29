#ifdef TRACY_ENABLE

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include "stdlib.h"
#  include <winsock2.h>
#  include <windows.h>
#  include <tlhelp32.h>
#  include <inttypes.h>
#  include <intrin.h>
#  include "../common/TracyUwp.hpp"
#else
#  include <sys/time.h>
#  include <sys/param.h>
#endif

#ifdef _GNU_SOURCE
#  include <errno.h>
#endif

#ifdef __linux__
#  include <dirent.h>
#  include <pthread.h>
#  include <sys/types.h>
#  include <sys/syscall.h>
#endif

#if defined __APPLE__ || defined BSD
#  include <sys/types.h>
#  include <sys/sysctl.h>
#endif

#if defined __APPLE__
#  include "TargetConditionals.h"
#  include <mach-o/dyld.h>
#endif

#ifdef __ANDROID__
#  include <sys/mman.h>
#  include <sys/system_properties.h>
#  include <stdio.h>
#  include <stdint.h>
#  include <algorithm>
#  include <vector>
#endif

#ifdef __QNX__
#  include <stdint.h>
#  include <stdio.h>
#  include <string.h>
#  include <sys/syspage.h>
#  include <sys/stat.h>
#endif

#include <algorithm>
#include <array>
#include <assert.h>
#include <atomic>
#include <chrono>
#include <limits>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>
#include <unordered_map>

#include <vector> //VALVE: gcc 11 complaining about this missing

#include "../common/TracyAlign.hpp"
#include "../common/TracyAlloc.hpp"
#include "../common/TracySocket.hpp"
#include "../common/TracySystem.hpp"
#include "../common/TracyYield.hpp"
#include "../common/tracy_lz4.hpp"
#include "tracy_rpmalloc.hpp"
#include "TracyCallstack.hpp"
#include "TracyDebug.hpp"
#include "TracyDxt1.hpp"
#include "TracyScoped.hpp"
#include "TracyProfiler.hpp"
#include "TracyThread.hpp"
#include "TracyArmCpuTable.hpp"
#include "TracySysTrace.hpp"
#include "../tracy/TracyC.h"
#include "../tracy/Tracy.hpp"
#include "../tracy/TracyExternal.h"

#if defined TRACY_MANUAL_LIFETIME && !defined(TRACY_DELAYED_INIT)
#  error "TRACY_MANUAL_LIFETIME requires enabled TRACY_DELAYED_INIT"
#endif

#ifdef TRACY_PORT
#  ifndef TRACY_DATA_PORT
#    define TRACY_DATA_PORT TRACY_PORT
#  endif
#  ifndef TRACY_BROADCAST_PORT
#    define TRACY_BROADCAST_PORT TRACY_PORT
#  endif
#endif

#ifdef __APPLE__
#  ifndef TRACY_DELAYED_INIT
#    define TRACY_DELAYED_INIT
#  endif
#else
#  ifdef __GNUC__
#    define init_order( val ) __attribute__ ((init_priority(val)))
#  else
#    define init_order(x)
#  endif
#endif

#if defined _WIN32
#  include <lmcons.h>
extern "C" typedef LONG (WINAPI *t_RtlGetVersion)( PRTL_OSVERSIONINFOW );
extern "C" typedef BOOL (WINAPI *t_GetLogicalProcessorInformationEx)( LOGICAL_PROCESSOR_RELATIONSHIP, PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, PDWORD );
extern "C" typedef char* (WINAPI *t_WineGetVersion)();
extern "C" typedef char* (WINAPI *t_WineGetBuildId)();
#else
#  include <unistd.h>
#  include <limits.h>
#  include <fcntl.h>
#endif
#if defined __linux__
#  include <sys/sysinfo.h>
#  include <sys/utsname.h>
#endif

#if !defined _WIN32 && ( defined __i386 || defined _M_IX86 || defined __x86_64__ || defined _M_X64 )
#  include "TracyCpuid.hpp"
#endif

#if !( ( defined _WIN32 && _WIN32_WINNT >= _WIN32_WINNT_VISTA ) || defined __linux__ )
#  include <mutex>
#endif

#ifdef __QNX__
extern char* __progname;
#endif


// NOTE(sev): Symbol loading can be *very* slow, we're shutting down and don't really care anymore
// Additionally, we may be stopping while we still have zones active, this will hang the
// server forever, because it expects all zones to be properly ended before it sends its
// responding quit signal... This is "fine" in theory if the profiler is the very last thing
// shutting down, however since we're unloading dlls, which may then crash the profiler
// we *have* to shut down early, potentially causing a hang.
// We really don't care and just quit...
#define TRACY_PROCESS_REMAINING_QUERIES 0


namespace tracy
{


static void UpdateTimeRange( int64_t& rBeg, int64_t& rEnd, int64_t time )
{
    uint64_t beg = (uint64_t)rBeg - 1u;
    int64_t end = rEnd;
    beg = std::min( beg, (uint64_t)time - 1u );
    end = std::max( end,           time );
    rBeg = (int64_t)(beg + 1u);
    rEnd = end;
}

static void UpdateCpuRange( Profiler::ProcessStats &rInfo, int64_t time )
{
    UpdateTimeRange( rInfo.cpuBeg, rInfo.cpuEnd, time );
}

static void UpdateGpuRange( Profiler::ProcessStats &rInfo, int64_t time )
{
    UpdateTimeRange( rInfo.gpuBeg, rInfo.gpuEnd, time );
}

static void UpdateCpuRanges( Profiler::ProcessStats &rInfo, int64_t time0, int64_t time1 )
{
    uint64_t beg = (uint64_t)rInfo.cpuBeg - 1u;
    int64_t end = rInfo.cpuEnd;
    beg = std::min( beg, (uint64_t)time0 - 1u );
    end = std::max( end,           time0 );
    beg = std::min( beg, (uint64_t)time1 - 1u );
    end = std::max( end,           time1 );
    rInfo.cpuBeg = (int64_t)(beg + 1u);
    rInfo.cpuEnd = end;
}


static const char* TracyGetCommandLine()
{
    static const char* s_szCommandLine;
    if ( !s_szCommandLine )
    {
#ifdef _WIN32
        s_szCommandLine = GetCommandLineA();
#elif defined __linux__
        enum { MaxCommandLineBufferSize = 4096 };
        static char s_szCommandLineBuf[ MaxCommandLineBufferSize + 1 ];
        FILE *f = fopen( "/proc/self/cmdline", "rb" );
        if ( f )
        {
            const size_t readSize = (size_t)MaxCommandLineBufferSize;
            size_t readBytes = fread( s_szCommandLineBuf, 1, readSize, f );
            for ( size_t index = 0; index < readBytes; index++ )
            {
                if ( s_szCommandLineBuf[ index ] == 0 )
                {
                    s_szCommandLineBuf[ index ] = ' ';
                }
            }

            s_szCommandLine = s_szCommandLineBuf;
            fclose( f );
        }
        else
        {
            s_szCommandLine = "";
        }
#else
        s_szCommandLine = "";
#endif
    }

    assert( s_szCommandLine );
    return s_szCommandLine;
}


static bool TracyHasCommandLineOption( const char *pOption )
{
    const char *szCommandLine = TracyGetCommandLine();
    bool result = ( szCommandLine && szCommandLine[ 0 ] && pOption && strstr( szCommandLine, pOption ) );
    return result;
}


template < typename T >
using TracyVector = std::vector< T, StlAllocator< T > >;

template < typename K, typename T, typename Hasher = std::hash< K >, typename Eq = std::equal_to< K > >
using TracyUnorderedMap = std::unordered_map< K, T, Hasher, Eq, StlAllocator< std::pair< const K, T > > >;

template < typename K, typename Hasher = std::hash< K >, typename Eq = std::equal_to< K > >
using TracyUnorderedSet = std::unordered_set< K, Hasher, Eq, StlAllocator< K > >;


class UiConnection : public Profiler::IBufferHandler
{
public:
    uint32_t id;

    static UiConnection *Create( Socket *sock )
    {
        UiConnection *conn = nullptr;
        if ( sock )
        {
            conn = new( tracy_malloc( sizeof( UiConnection ) ) ) UiConnection( sock );
        }
        return conn;
    }

    static void Destroy( UiConnection *& conn )
    {
        if ( conn )
        {
            conn->~UiConnection();
            tracy_free( conn );
            conn = nullptr;
        }
    }

    virtual bool Commit( size_t pendingSize ) override
    {
        bool succeeded = false;
        if ( m_buffer.pBuffer )
        {
            const char *pSend = m_buffer.pBuffer + m_buffer.start;
            size_t sendLen = m_buffer.offset - m_buffer.start;
            if ( m_buffer.offset > m_buffer.wrapLimit )
            {
                m_buffer.offset = 0;
            }
            m_buffer.start = m_buffer.offset;

            if ( IsValid() && ( sendLen > 0 ) )
            {
                succeeded = ( SendCompressed( pSend, ( int ) sendLen ) > 0 );
            }

            m_error |= !succeeded;
        }

        return succeeded;
    }

    void ResetBuffer()
    {
        m_buffer.start = 0;
        m_buffer.offset = 0;
    }

    bool IsValid() const
    {
        return !m_error && (m_sock != nullptr);
    }

    bool HasData() const
    {
        bool result = false;
        if (IsValid())
        {
            result = m_sock->HasData();
        }
        return result;
    }

    bool Read( void* buf, int len, int timeout )
    {
        bool succeeded = false;
        if (IsValid() && (len >= 0))
        {
            succeeded = m_sock->Read(buf, len, timeout);
        }
        m_error |= !succeeded;
        return succeeded;
    }

    bool ReadRaw( void* buf, int len, int timeout )
    {
        bool succeeded = false;
        if (IsValid() && (len >= 0))
        {
            succeeded = m_sock->ReadRaw(buf, len, timeout);
        }
        m_error |= !succeeded;
        return succeeded;
    }

    int Send( const void *buf, int len )
    {
        int result = -1;
        if (IsValid() && (len >= 0))
        {
            const void* sendBuf = buf;
            int sentTotal = 0;
            while ( len > 0 )
            {
                int sendLen = (len  > TargetFrameSize ? TargetFrameSize : len );
                MemoryBuffer send = { .ptr = (void*)sendBuf, .size = (size_t)sendLen };
                int sent = m_sock->Send(send.ptr, (int)send.size);
                if ( sent > 0 )
                {
                    sentTotal += sent;
                }
                else
                {
                    sentTotal = -1;
                    break;
                }

                sendBuf = (const void*)( ((const char*)buf) + sendLen );
                len -= sendLen;
            }

            result = sentTotal;
        }
        m_error |= (result < 0);
        return result;
    }

    int SendCompressed( const void *buf, int len )
    {
        int result = -1;
        if (IsValid() && (len >= 0))
        {
            const void* sendBuf = buf;
            int sentTotal = 0;
            while ( len > 0 )
            {
                int sendLen = (len  > TargetFrameSize ? TargetFrameSize : len );
                MemoryBuffer send = m_compressor.Compress(sendBuf, sendLen);

                int sent = m_sock->Send(send.ptr, (int)send.size);
                if ( sent > 0 )
                {
                    sentTotal += sent;
                }
                else
                {
                    sentTotal = -1;
                    break;
                }

                sendBuf = (const void*)( ((const char*)buf) + sendLen );
                len -= sendLen;
            }

            result = sentTotal;
        }
        m_error |= (result < 0);
        return result;
    }

private:
    UiConnection( Socket* sock )
        : id( 0 )
        , m_compressor( LZ4Size + sizeof( lz4sz_t ) )
        , m_sock( sock )
        , m_error( sock == nullptr )
    {
        memset( &m_buffer, 0, sizeof( m_buffer ) );
        m_buffer.size = TargetFrameSize * 3;
        m_buffer.commitLimit = TargetFrameSize;
        m_buffer.wrapLimit = TargetFrameSize * 2;
        m_buffer.pBuffer = (char*)tracy_malloc( m_buffer.size );
        m_buffer.start = 0;
        m_buffer.offset = 0;
    }

    virtual ~UiConnection()
    {
        tracy_free( m_buffer.pBuffer );
        memset( &m_buffer, 0, sizeof( m_buffer ) );

        if( m_sock )
        {
            m_sock->~Socket();
            tracy_free( m_sock );
        }
        m_sock = nullptr;
    }


    struct MemoryBuffer
    {
        void* ptr = nullptr;
        size_t size = 0;
    };


    struct Lz4Compressor
    {
        Lz4Compressor( size_t bufSize )
        : m_size( bufSize )
        , m_stream( nullptr )
        , m_lz4Buf( nullptr )
        , m_srcBufferSize( TargetFrameSize * 3 )
        , m_srcBufferOffset( 0 )
        , m_srcBuffer( nullptr )
        {
            Allocate();
        }

        ~Lz4Compressor()
        {
            Deallocate();
        }

        void Allocate()
        {
            m_stream = LZ4_createStream();
            m_lz4Buf = (char*)tracy_malloc( m_size );

            m_srcBuffer = (char*)tracy_malloc( m_srcBufferSize );
            m_srcBufferOffset = 0;
        }

        void Deallocate()
        {
            if (m_lz4Buf)
            {
                tracy_free( m_lz4Buf );
                m_lz4Buf = nullptr;
            }

            if (m_stream)
            {
                LZ4_freeStream( (LZ4_stream_t*)m_stream );
                m_stream = nullptr;
            }

            if ( m_srcBuffer  )
            {
                tracy_free( m_srcBuffer );
                m_srcBuffer = nullptr;
            }

            m_srcBufferOffset = 0;
        }

        void Reset()
        {
            if (m_stream)
            {
                LZ4_resetStream( (LZ4_stream_t*)m_stream );

                m_srcBufferOffset = 0;
            }
        }

        MemoryBuffer Compress( const void *data, size_t len )
        {
            MemoryBuffer result = { 0 };

            if (m_stream && m_srcBuffer)
            {
                // NOTE(sev): this memcpy is needed, because lz4 needs the old data around for the dictionary.
                // otherwise we would need to reset the stream every time
                char* pSrc = (m_srcBuffer + m_srcBufferOffset);
                memcpy( pSrc, data, len );

                const lz4sz_t lz4sz = LZ4_compress_fast_continue( (LZ4_stream_t*)m_stream, (const char*)pSrc, m_lz4Buf + sizeof(lz4sz_t), ( int ) len, LZ4Size, 1);
                assert( lz4sz >= 0 );
                memcpy( m_lz4Buf, &lz4sz, sizeof( lz4sz ) );

                m_srcBufferOffset += (size_t)len;
                if( m_srcBufferOffset > TargetFrameSize * 2 )
                {
                    m_srcBufferOffset = 0;
                }

                result.size = lz4sz + sizeof( lz4sz_t );
                result.ptr = m_lz4Buf;
            }
            else
            {
                result.size = len;
                result.ptr = (void*)data;
            }

            return result;
        }

        size_t m_size;
        void* m_stream;
        char* m_lz4Buf;
        size_t m_srcBufferSize;
        size_t m_srcBufferOffset;
        char* m_srcBuffer;
    };

    bool m_error;
    Socket *m_sock;
    Lz4Compressor m_compressor;
};


#ifdef __ANDROID__
// Implementation helpers of EnsureReadable(address).
// This is so far only needed on Android, where it is common for libraries to be mapped
// with only executable, not readable, permissions. Typical example (line from /proc/self/maps):
/*
746b63b000-746b6dc000 --xp 00042000 07:48 35                             /apex/com.android.runtime/lib64/bionic/libc.so
*/
// See https://github.com/wolfpld/tracy/issues/125 .
// To work around this, we parse /proc/self/maps and we use mprotect to set read permissions
// on any mappings that contain symbols addresses hit by HandleSymbolCodeQuery.

namespace {
// Holds some information about a single memory mapping.
struct MappingInfo {
    // Start of address range. Inclusive.
    uintptr_t start_address;
    // End of address range. Exclusive, so the mapping is the half-open interval
    // [start, end) and its length in bytes is `end - start`. As in /proc/self/maps.
    uintptr_t end_address;
    // Read/Write/Executable permissions.
    bool perm_r, perm_w, perm_x;
};
}  // anonymous namespace

   // Internal implementation helper for LookUpMapping(address).
   //
   // Parses /proc/self/maps returning a vector<MappingInfo>.
   // /proc/self/maps is assumed to be sorted by ascending address, so the resulting
   // vector is sorted by ascending address too.
static std::vector<MappingInfo> ParseMappings()
{
    std::vector<MappingInfo> result;
    FILE* file = fopen( "/proc/self/maps", "r" );
    if( !file ) return result;
    char line[1024];
    while( fgets( line, sizeof( line ), file ) )
    {
        uintptr_t start_addr;
        uintptr_t end_addr;
#if defined(__LP64__)
        if( sscanf( line, "%lx-%lx", &start_addr, &end_addr ) != 2 ) continue;
#else
        if (sscanf( line, "%dx-%dx", &start_addr, &end_addr ) != 2 ) continue;
#endif
        char* first_space = strchr( line, ' ' );
        if( !first_space ) continue;
        char* perm = first_space + 1;
        char* second_space = strchr( perm, ' ' );
        if( !second_space || second_space - perm != 4 ) continue;
        result.emplace_back();
        auto& mapping = result.back();
        mapping.start_address = start_addr;
        mapping.end_address = end_addr;
        mapping.perm_r = perm[0] == 'r';
        mapping.perm_w = perm[1] == 'w';
        mapping.perm_x = perm[2] == 'x';
    }
    fclose( file );
    return result;
}

// Internal implementation helper for LookUpMapping(address).
//
// Takes as input an `address` and a known vector `mappings`, assumed to be
// sorted by increasing addresses, as /proc/self/maps seems to be.
// Returns a pointer to the MappingInfo describing the mapping that this
// address belongs to, or nullptr if the address isn't in `mappings`.
static MappingInfo* LookUpMapping(std::vector<MappingInfo>& mappings, uintptr_t address)
{
    // Comparison function for std::lower_bound. Returns true if all addresses in `m1`
    // are lower than `addr`.
    auto Compare = []( const MappingInfo& m1, uintptr_t addr ) {
        // '<=' because the address ranges are half-open intervals, [start, end).
        return m1.end_address <= addr;
    };
    auto iter = std::lower_bound( mappings.begin(), mappings.end(), address, Compare );
    if( iter == mappings.end() || iter->start_address > address) {
        return nullptr;
    }
    return &*iter;
}

// Internal implementation helper for EnsureReadable(address).
//
// Takes as input an `address` and returns a pointer to a MappingInfo
// describing the mapping that this address belongs to, or nullptr if
// the address isn't in any known mapping.
//
// This function is stateful and not reentrant (assumes to be called from
// only one thread). It holds a vector of mappings parsed from /proc/self/maps.
//
// Attempts to react to mappings changes by re-parsing /proc/self/maps.
static MappingInfo* LookUpMapping(uintptr_t address)
{
    // Static state managed by this function. Not constant, we mutate that state as
    // we turn some mappings readable. Initially parsed once here, updated as needed below.
    static std::vector<MappingInfo> s_mappings = ParseMappings();
    MappingInfo* mapping = LookUpMapping( s_mappings, address );
    if( mapping ) return mapping;

    // This address isn't in any known mapping. Try parsing again, maybe
    // mappings changed.
    s_mappings = ParseMappings();
    return LookUpMapping( s_mappings, address );
}

// Internal implementation helper for EnsureReadable(address).
//
// Attempts to make the specified `mapping` readable if it isn't already.
// Returns true if and only if the mapping is readable.
static bool EnsureReadable( MappingInfo& mapping )
{
    if( mapping.perm_r )
    {
        // The mapping is already readable.
        return true;
    }
    int prot = PROT_READ;
    if( mapping.perm_w ) prot |= PROT_WRITE;
    if( mapping.perm_x ) prot |= PROT_EXEC;
    if( mprotect( reinterpret_cast<void*>( mapping.start_address ),
        mapping.end_address - mapping.start_address, prot ) == -1 )
    {
        // Failed to make the mapping readable. Shouldn't happen, hasn't
        // been observed yet. If it happened in practice, we should consider
        // adding a bool to MappingInfo to track this to avoid retrying mprotect
        // everytime on such mappings.
        return false;
    }
    // The mapping is now readable. Update `mapping` so the next call will be fast.
    mapping.perm_r = true;
    return true;
}

// Attempts to set the read permission on the entire mapping containing the
// specified address. Returns true if and only if the mapping is now readable.
static bool EnsureReadable( uintptr_t address )
{
    MappingInfo* mapping = LookUpMapping(address);
    return mapping && EnsureReadable( *mapping );
}
#elif defined WIN32
static bool EnsureReadable( uintptr_t address )
{
    MEMORY_BASIC_INFORMATION memInfo;
    VirtualQuery( reinterpret_cast<void*>( address ), &memInfo, sizeof( memInfo ) );
    return memInfo.Protect != PAGE_NOACCESS;
}
#else
static bool EnsureReadable( uintptr_t address )
{
    return true;
}
#endif

#ifndef TRACY_DELAYED_INIT

struct InitTimeWrapper
{
    int64_t val;
};

struct ProducerWrapper
{
    tracy::moodycamel::ConcurrentQueue<QueueItem>::ExplicitProducer* ptr;
};

struct ThreadHandleWrapper
{
    uint32_t val;
};
#endif


#if defined __i386 || defined _M_IX86 || defined __x86_64__ || defined _M_X64
enum CpuidRegister
{
    CpuidRegister_eax,
    CpuidRegister_ebx,
    CpuidRegister_ecx,
    CpuidRegister_edx,
};

static inline void CpuId( uint32_t* regs, uint32_t leaf )
{
    memset(regs, 0, sizeof(uint32_t) * 4);
#if defined _MSC_VER
    __cpuidex( (int*)regs, leaf, 0 );
#else
    __get_cpuid( leaf, regs, regs+1, regs+2, regs+3 );
#endif
}

static void InitFailure( const char* msg )
{
#if defined _WIN32
    bool hasConsole = false;
    bool reopen = false;
    const auto attached = AttachConsole( ATTACH_PARENT_PROCESS );
    if( attached )
    {
        hasConsole = true;
        reopen = true;
    }
    else
    {
        const auto err = GetLastError();
        if( err == ERROR_ACCESS_DENIED )
        {
            hasConsole = true;
        }
    }
    if( hasConsole )
    {
        fprintf( stderr, "Tracy Profiler initialization failure: %s\n", msg );
        if( reopen )
        {
            freopen( "CONOUT$", "w", stderr );
            fprintf( stderr, "Tracy Profiler initialization failure: %s\n", msg );
        }
    }
    else
    {
#  ifndef TRACY_UWP
        MessageBoxA( nullptr, msg, "Tracy Profiler initialization failure", MB_ICONSTOP );
#  endif
    }
#else
    fprintf( stderr, "Tracy Profiler initialization failure: %s\n", msg );
#endif
    exit( 1 );
}

static bool CheckHardwareSupportsInvariantTSC()
{
    // Disabling 'invariant TSC' check (currently failing if an exe runs on a virtual machine eg buildbot)
    return true;

#if 0
    const char* noCheck = GetEnvVar( "TRACY_NO_INVARIANT_CHECK" );
    if( noCheck && noCheck[0] == '1' ) return true;

    uint32_t regs[4];
    CpuId( regs, 1 );
    if( !( regs[3] & ( 1 << 4 ) ) )
    {
#if !defined TRACY_TIMER_QPC && !defined TRACY_TIMER_FALLBACK
        InitFailure( "CPU doesn't support RDTSC instruction." );
#else
        return false;
#endif
    }
    CpuId( regs, 0x80000007 );
    if( regs[3] & ( 1 << 8 ) ) return true;

    return false;
#endif
}

#if defined TRACY_TIMER_FALLBACK && defined TRACY_HW_TIMER
bool HardwareSupportsInvariantTSC()
{
    static bool cachedResult = CheckHardwareSupportsInvariantTSC();
    return cachedResult;
}
#endif

static int64_t SetupHwTimer()
{
#if !defined TRACY_TIMER_QPC && !defined TRACY_TIMER_FALLBACK
    if( !CheckHardwareSupportsInvariantTSC() )
    {
#if defined _WIN32
        InitFailure( "CPU doesn't support invariant TSC.\nDefine TRACY_NO_INVARIANT_CHECK=1 to ignore this error, *if you know what you are doing*.\nAlternatively you may rebuild the application with the TRACY_TIMER_QPC or TRACY_TIMER_FALLBACK define to use lower resolution timer." );
#else
        InitFailure( "CPU doesn't support invariant TSC.\nDefine TRACY_NO_INVARIANT_CHECK=1 to ignore this error, *if you know what you are doing*.\nAlternatively you may rebuild the application with the TRACY_TIMER_FALLBACK define to use lower resolution timer." );
#endif
    }
#endif

    return Profiler::GetTime();
}
#else
static int64_t SetupHwTimer()
{
    return Profiler::GetTime();
}
#endif

static const char* GetProcessName()
{
    const char* processName = "unknown";
#ifdef _WIN32
    static char buf[_MAX_PATH];
    GetModuleFileNameA( nullptr, buf, _MAX_PATH );
    const char* ptr = buf;
    while( *ptr != '\0' ) ptr++;
    while( ptr > buf && *ptr != '\\' && *ptr != '/' ) ptr--;
    if( ptr > buf ) ptr++;
    processName = ptr;

    if ( TracyHasCommandLineOption( "-dedicated" ) && (strlen(processName) + strlen("DS_") < _MAX_PATH ) )
    {
        sprintf( buf, "DS_%s", processName );
        processName = buf;
    }

#elif defined __ANDROID__
#  if __ANDROID_API__ >= 21
    auto buf = getprogname();
    if( buf ) processName = buf;
#  endif
#elif defined __linux__ && defined _GNU_SOURCE
    if( program_invocation_short_name ) processName = program_invocation_short_name;
#elif defined __APPLE__ || defined BSD
    auto buf = getprogname();
    if( buf ) processName = buf;
#elif defined __QNX__
    processName = __progname;
#endif
    return processName;
}

static const char* GetProcessExecutablePath()
{
#ifdef _WIN32
    static char buf[_MAX_PATH];
    GetModuleFileNameA( nullptr, buf, _MAX_PATH );
    return buf;
#elif defined __ANDROID__
    return nullptr;
#elif defined __linux__ && defined _GNU_SOURCE
    return program_invocation_name;
#elif defined __APPLE__
    static char buf[1024];
    uint32_t size = 1024;
    _NSGetExecutablePath( buf, &size );
    return buf;
#elif defined __DragonFly__
    static char buf[1024];
    readlink( "/proc/curproc/file", buf, 1024 );
    return buf;
#elif defined __FreeBSD__
    static char buf[1024];
    int mib[4];
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PATHNAME;
    mib[3] = -1;
    size_t cb = 1024;
    sysctl( mib, 4, buf, &cb, nullptr, 0 );
    return buf;
#elif defined __NetBSD__
    static char buf[1024];
    readlink( "/proc/curproc/exe", buf, 1024 );
    return buf;
#elif defined __QNX__
    static char buf[_PC_PATH_MAX + 1];
    _cmdname(buf);
    return buf;
#else
    return nullptr;
#endif
}

#if defined __linux__ && defined __ARM_ARCH
static uint32_t GetHex( char*& ptr, int skip )
{
    uint32_t ret;
    ptr += skip;
    char* end;
    if( ptr[0] == '0' && ptr[1] == 'x' )
    {
        ptr += 2;
        ret = strtol( ptr, &end, 16 );
    }
    else
    {
        ret = strtol( ptr, &end, 10 );
    }
    ptr = end;
    return ret;
}
#endif

static const char* GetHostInfo()
{
    static char buf[1024];
    auto ptr = buf;
#if defined _WIN32
#  ifdef TRACY_UWP
    auto GetVersion = &::GetVersionEx;
#  else
    auto GetVersion = (t_RtlGetVersion)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "RtlGetVersion" );
#  endif
    if( !GetVersion )
    {
#  ifdef __MINGW32__
        ptr += sprintf( ptr, "OS: Windows (MingW)\n" );
#  else
        ptr += sprintf( ptr, "OS: Windows\n" );
#  endif
    }
    else
    {
        RTL_OSVERSIONINFOW ver = { sizeof( RTL_OSVERSIONINFOW ) };
        GetVersion( &ver );

#  ifdef __MINGW32__
        ptr += sprintf( ptr, "OS: Windows %i.%i.%i (MingW)\n", (int)ver.dwMajorVersion, (int)ver.dwMinorVersion, (int)ver.dwBuildNumber );
#  else
        auto WineGetVersion = (t_WineGetVersion)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "wine_get_version" );
        auto WineGetBuildId = (t_WineGetBuildId)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "wine_get_build_id" );
        if( WineGetVersion && WineGetBuildId )
        {
            ptr += sprintf( ptr, "OS: Windows %lu.%lu.%lu (Wine %s [%s])\n", ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber, WineGetVersion(), WineGetBuildId() );
        }
        else
        {
        ptr += sprintf( ptr, "OS: Windows %lu.%lu.%lu\n", ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber );
        }
#  endif
    }
#elif defined __linux__
    struct utsname utsName;
    uname( &utsName );
#  if defined __ANDROID__
    ptr += sprintf( ptr, "OS: Linux %s (Android)\n", utsName.release );
#  else
    ptr += sprintf( ptr, "OS: Linux %s\n", utsName.release );
#  endif
#elif defined __APPLE__
#  if TARGET_OS_IPHONE == 1
    ptr += sprintf( ptr, "OS: Darwin (iOS)\n" );
#  elif TARGET_OS_MAC == 1
    ptr += sprintf( ptr, "OS: Darwin (OSX)\n" );
#  else
    ptr += sprintf( ptr, "OS: Darwin (unknown)\n" );
#  endif
#elif defined __DragonFly__
    ptr += sprintf( ptr, "OS: BSD (DragonFly)\n" );
#elif defined __FreeBSD__
    ptr += sprintf( ptr, "OS: BSD (FreeBSD)\n" );
#elif defined __NetBSD__
    ptr += sprintf( ptr, "OS: BSD (NetBSD)\n" );
#elif defined __OpenBSD__
    ptr += sprintf( ptr, "OS: BSD (OpenBSD)\n" );
#elif defined __QNX__
    ptr += sprintf( ptr, "OS: QNX\n" );
#else
    ptr += sprintf( ptr, "OS: unknown\n" );
#endif

#if defined _MSC_VER
#  if defined __clang__
    ptr += sprintf( ptr, "Compiler: MSVC clang-cl %i.%i.%i\n", __clang_major__, __clang_minor__, __clang_patchlevel__ );
#  else
    ptr += sprintf( ptr, "Compiler: MSVC %i\n", _MSC_VER );
#  endif
#elif defined __clang__
    ptr += sprintf( ptr, "Compiler: clang %i.%i.%i\n", __clang_major__, __clang_minor__, __clang_patchlevel__ );
#elif defined __GNUC__
    ptr += sprintf( ptr, "Compiler: gcc %i.%i.%i\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__ );
#else
    ptr += sprintf( ptr, "Compiler: unknown\n" );
#endif

#if defined _WIN32
    InitWinSock();

    char hostname[512];
    gethostname( hostname, 512 );

#  ifdef TRACY_UWP
    const char* user = "";
#  else
    DWORD userSz = UNLEN+1;
    char user[UNLEN+1];
    GetUserNameA( user, &userSz );
#  endif

    ptr += sprintf( ptr, "User: %s@%s\n", user, hostname );
#else
    char hostname[_POSIX_HOST_NAME_MAX]{};
    char user[_POSIX_LOGIN_NAME_MAX]{};

    gethostname( hostname, _POSIX_HOST_NAME_MAX );
#  if defined __ANDROID__
    const auto login = getlogin();
    if( login )
    {
        strcpy( user, login );
    }
    else
    {
        memcpy( user, "(?)", 4 );
    }
#  else
    getlogin_r( user, _POSIX_LOGIN_NAME_MAX );
#  endif

    ptr += sprintf( ptr, "User: %s@%s\n", user, hostname );
#endif

#if defined __i386 || defined _M_IX86
    ptr += sprintf( ptr, "Arch: x86\n" );
#elif defined __x86_64__ || defined _M_X64
    ptr += sprintf( ptr, "Arch: x64\n" );
#elif defined __aarch64__
    ptr += sprintf( ptr, "Arch: ARM64\n" );
#elif defined __ARM_ARCH
    ptr += sprintf( ptr, "Arch: ARM\n" );
#else
    ptr += sprintf( ptr, "Arch: unknown\n" );
#endif

#if defined __i386 || defined _M_IX86 || defined __x86_64__ || defined _M_X64
    uint32_t regs[4];
    char cpuModel[4*4*3+1] = {};
    auto modelPtr = cpuModel;
    for( uint32_t i=0x80000002; i<0x80000005; ++i )
    {
        CpuId( regs, i );
        memcpy( modelPtr, regs, sizeof( regs ) ); modelPtr += sizeof( regs );
    }

    ptr += sprintf( ptr, "CPU: %s\n", cpuModel );
#elif defined __linux__ && defined __ARM_ARCH
    bool cpuFound = false;
    FILE* fcpuinfo = fopen( "/proc/cpuinfo", "rb" );
    if( fcpuinfo )
    {
        enum { BufSize = 4*1024 };
        char buf[BufSize];
        const auto sz = fread( buf, 1, BufSize, fcpuinfo );
        fclose( fcpuinfo );
        const auto end = buf + sz;
        auto cptr = buf;

        uint32_t impl = 0;
        uint32_t var = 0;
        uint32_t part = 0;
        uint32_t rev = 0;

        while( end - cptr > 20 )
        {
            while( end - cptr > 20 && memcmp( cptr, "CPU ", 4 ) != 0 )
            {
                cptr += 4;
                while( end - cptr > 20 && *cptr != '\n' ) cptr++;
                cptr++;
            }
            if( end - cptr <= 20 ) break;
            cptr += 4;
            if( memcmp( cptr, "implementer\t: ", 14 ) == 0 )
            {
                if( impl != 0 ) break;
                impl = GetHex( cptr, 14 );
            }
            else if( memcmp( cptr, "variant\t: ", 10 ) == 0 ) var = GetHex( cptr, 10 );
            else if( memcmp( cptr, "part\t: ", 7 ) == 0 ) part = GetHex( cptr, 7 );
            else if( memcmp( cptr, "revision\t: ", 11 ) == 0 ) rev = GetHex( cptr, 11 );
            while( *cptr != '\n' && *cptr != '\0' ) cptr++;
            cptr++;
        }

        if( impl != 0 || var != 0 || part != 0 || rev != 0 )
        {
            cpuFound = true;
            ptr += sprintf( ptr, "CPU: %s%s r%ip%i\n", DecodeArmImplementer( impl ), DecodeArmPart( impl, part ), var, rev );
        }
    }
    if( !cpuFound )
    {
        ptr += sprintf( ptr, "CPU: unknown\n" );
    }
#elif defined __APPLE__ && TARGET_OS_IPHONE == 1
    {
        size_t sz;
        sysctlbyname( "hw.machine", nullptr, &sz, nullptr, 0 );
        auto str = (char*)tracy_malloc( sz );
        sysctlbyname( "hw.machine", str, &sz, nullptr, 0 );
        ptr += sprintf( ptr, "Device: %s\n", DecodeIosDevice( str ) );
        tracy_free( str );
    }
#else
    ptr += sprintf( ptr, "CPU: unknown\n" );
#endif
#ifdef __ANDROID__
    char deviceModel[PROP_VALUE_MAX+1];
    char deviceManufacturer[PROP_VALUE_MAX+1];
    __system_property_get( "ro.product.model", deviceModel );
    __system_property_get( "ro.product.manufacturer", deviceManufacturer );
    ptr += sprintf( ptr, "Device: %s %s\n", deviceManufacturer, deviceModel );
#endif

    ptr += sprintf( ptr, "CPU cores: %i\n", std::thread::hardware_concurrency() );

#if defined _WIN32
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof( statex );
    GlobalMemoryStatusEx( &statex );
#  ifdef _MSC_VER
    ptr += sprintf( ptr, "RAM: %I64u MB\n", statex.ullTotalPhys / 1024 / 1024 );
#  else
    ptr += sprintf( ptr, "RAM: %llu MB\n", statex.ullTotalPhys / 1024 / 1024 );
#  endif
#elif defined __linux__
    struct sysinfo sysInfo;
    sysinfo( &sysInfo );
    ptr += sprintf( ptr, "RAM: %lu MB\n", sysInfo.totalram / 1024 / 1024 );
#elif defined __APPLE__
    size_t memSize;
    size_t sz = sizeof( memSize );
    sysctlbyname( "hw.memsize", &memSize, &sz, nullptr, 0 );
    ptr += sprintf( ptr, "RAM: %zu MB\n", memSize / 1024 / 1024 );
#elif defined BSD
    size_t memSize;
    size_t sz = sizeof( memSize );
    sysctlbyname( "hw.physmem", &memSize, &sz, nullptr, 0 );
    ptr += sprintf( ptr, "RAM: %zu MB\n", memSize / 1024 / 1024 );
#elif defined __QNX__
    struct asinfo_entry *entries = SYSPAGE_ENTRY(asinfo);
    size_t count = SYSPAGE_ENTRY_SIZE(asinfo) / sizeof(struct asinfo_entry);
    char *strings = SYSPAGE_ENTRY(strings)->data;

    uint64_t memSize = 0;
    size_t i;
    for (i = 0; i < count; i++) {
        struct asinfo_entry *entry = &entries[i];
        if (strcmp(strings + entry->name, "ram") == 0) {
            memSize += entry->end - entry->start + 1;
        }
    }
    memSize = memSize / 1024 / 1024;
    ptr += sprintf( ptr, "RAM: %llu MB\n", memSize);
#else
    ptr += sprintf( ptr, "RAM: unknown\n" );
#endif

    return buf;
}

static uint64_t GetPid()
{
#if defined _WIN32
    return uint64_t( GetCurrentProcessId() );
#else
    return uint64_t( getpid() );
#endif
}

void Profiler::AckServerQuery()
{
    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::AckServerQueryNoop );
    NeedDataSize( QueueDataSize[(int)QueueType::AckServerQueryNoop] );
    AppendDataUnsafe( &item, QueueDataSize[(int)QueueType::AckServerQueryNoop] );
}

void Profiler::AckSymbolCodeNotAvailable()
{
    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::AckSymbolCodeNotAvailable );
    NeedDataSize( QueueDataSize[(int)QueueType::AckSymbolCodeNotAvailable] );
    AppendDataUnsafe( &item, QueueDataSize[(int)QueueType::AckSymbolCodeNotAvailable] );
}

static BroadcastMessage& GetBroadcastMessage( const char* procname, size_t pnsz, int& len, int port )
{
    static BroadcastMessage msg;
    static_assert( std::numeric_limits<uint8_t>::max() >= WelcomeMessageProgramNameSize );
    size_t nameLen = std::min( (size_t)WelcomeMessageProgramNameSize, pnsz );

    msg.broadcastVersion = BroadcastVersion;
    msg.protocolVersion = ProtocolVersion;
    msg.listenPort = port;
    msg.pid = GetPid();
    msg.nameLen = (uint8_t)nameLen;
    msg.msgLen = 0;
    memcpy( msg.strBuffer, procname, msg.nameLen );
    memset( msg.strBuffer + pnsz, 0, WelcomeMessageProgramNameSize - nameLen );

    len = int( offsetof( BroadcastMessage, strBuffer ) + nameLen );
    return msg;
}

#if defined _WIN32 && !defined TRACY_UWP && !defined TRACY_NO_CRASH_HANDLER
static DWORD s_profilerThreadId = 0;
static DWORD s_symbolThreadId = 0;
static char s_crashText[1024];

LONG WINAPI CrashFilter( PEXCEPTION_POINTERS pExp )
{
    if( !GetProfiler().IsConnected() ) return EXCEPTION_CONTINUE_SEARCH;

    const unsigned ec = pExp->ExceptionRecord->ExceptionCode;
    auto msgPtr = s_crashText;
    switch( ec )
    {
    case EXCEPTION_ACCESS_VIOLATION:
        msgPtr += sprintf( msgPtr, "Exception EXCEPTION_ACCESS_VIOLATION (0x%x). ", ec );
        switch( pExp->ExceptionRecord->ExceptionInformation[0] )
        {
        case 0:
            msgPtr += sprintf( msgPtr, "Read violation at address 0x%" PRIxPTR ".", pExp->ExceptionRecord->ExceptionInformation[1] );
            break;
        case 1:
            msgPtr += sprintf( msgPtr, "Write violation at address 0x%" PRIxPTR ".", pExp->ExceptionRecord->ExceptionInformation[1] );
            break;
        case 8:
            msgPtr += sprintf( msgPtr, "DEP violation at address 0x%" PRIxPTR ".", pExp->ExceptionRecord->ExceptionInformation[1] );
            break;
        default:
            break;
        }
        break;
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        msgPtr += sprintf( msgPtr, "Exception EXCEPTION_ARRAY_BOUNDS_EXCEEDED (0x%x). ", ec );
        break;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        msgPtr += sprintf( msgPtr, "Exception EXCEPTION_DATATYPE_MISALIGNMENT (0x%x). ", ec );
        break;
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        msgPtr += sprintf( msgPtr, "Exception EXCEPTION_FLT_DIVIDE_BY_ZERO (0x%x). ", ec );
        break;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        msgPtr += sprintf( msgPtr, "Exception EXCEPTION_ILLEGAL_INSTRUCTION (0x%x). ", ec );
        break;
    case EXCEPTION_IN_PAGE_ERROR:
        msgPtr += sprintf( msgPtr, "Exception EXCEPTION_IN_PAGE_ERROR (0x%x). ", ec );
        break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        msgPtr += sprintf( msgPtr, "Exception EXCEPTION_INT_DIVIDE_BY_ZERO (0x%x). ", ec );
        break;
    case EXCEPTION_PRIV_INSTRUCTION:
        msgPtr += sprintf( msgPtr, "Exception EXCEPTION_PRIV_INSTRUCTION (0x%x). ", ec );
        break;
    case EXCEPTION_STACK_OVERFLOW:
        msgPtr += sprintf( msgPtr, "Exception EXCEPTION_STACK_OVERFLOW (0x%x). ", ec );
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }

    {
        GetProfiler().SendCallstack( 60, "KiUserExceptionDispatcher" );

        TracyQueuePrepare( QueueType::CrashReport );
        item->crashReport.time = Profiler::GetTime();
        item->crashReport.text = (uint64_t)s_crashText;
        TracyQueueCommit( crashReportThread );
    }

    HANDLE h = CreateToolhelp32Snapshot( TH32CS_SNAPTHREAD, 0 );
    if( h == INVALID_HANDLE_VALUE ) return EXCEPTION_CONTINUE_SEARCH;

    THREADENTRY32 te = { sizeof( te ) };
    if( !Thread32First( h, &te ) )
    {
        CloseHandle( h );
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto pid = GetCurrentProcessId();
    const auto tid = GetCurrentThreadId();

    do
    {
        if( te.th32OwnerProcessID == pid && te.th32ThreadID != tid && te.th32ThreadID != s_profilerThreadId && te.th32ThreadID != s_symbolThreadId )
        {
            HANDLE th = OpenThread( THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID );
            if( th != INVALID_HANDLE_VALUE )
            {
                SuspendThread( th );
                CloseHandle( th );
            }
        }
    }
    while( Thread32Next( h, &te ) );
    CloseHandle( h );

    {
        TracyLfqPrepare( QueueType::Crash );
        TracyLfqCommit;
    }

    std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
    GetProfiler().RequestShutdown();
    while( !GetProfiler().HasShutdownFinished() ) { std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) ); };

    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static Profiler* s_instance = nullptr;
static Thread* s_thread;
#ifndef TRACY_NO_FRAME_IMAGE
static Thread* s_compressThread;
#endif
#ifdef TRACY_NEEDS_SYMBOL_WORKER
static Thread* s_symbolThread{ nullptr };
std::atomic<bool> s_symbolThreadRunning{ false };
static DbgHelpLoaderFunc* s_pDbgHelpLoader{ nullptr };
#endif
#ifdef TRACY_HAS_SYSTEM_TRACING
static Thread* s_sysTraceThread = nullptr;
#endif

#if defined __linux__ && !defined TRACY_NO_CRASH_HANDLER
#  ifndef TRACY_CRASH_SIGNAL
#    define TRACY_CRASH_SIGNAL SIGPWR
#  endif

static long s_profilerTid = 0;
static long s_symbolTid = 0;
static char s_crashText[1024];
static std::atomic<bool> s_alreadyCrashed( false );

static void ThreadFreezer( int /*signal*/ )
{
    for(;;) sleep( 1000 );
}

static inline void HexPrint( char*& ptr, uint64_t val )
{
    if( val == 0 )
    {
        *ptr++ = '0';
        return;
    }

    static const char HexTable[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    char buf[16];
    auto bptr = buf;

    do
    {
        *bptr++ = HexTable[val%16];
        val /= 16;
    }
    while( val > 0 );

    do
    {
        *ptr++ = *--bptr;
    }
    while( bptr != buf );
}

static void CrashHandler( int signal, siginfo_t* info, void* /*ucontext*/ )
{
    bool expected = false;
    if( !s_alreadyCrashed.compare_exchange_strong( expected, true ) ) ThreadFreezer( signal );

    struct sigaction act = {};
    act.sa_handler = SIG_DFL;
    sigaction( SIGABRT, &act, nullptr );

    auto msgPtr = s_crashText;
    switch( signal )
    {
    case SIGILL:
        strcpy( msgPtr, "Illegal Instruction.\n" );
        while( *msgPtr ) msgPtr++;
        switch( info->si_code )
        {
        case ILL_ILLOPC:
            strcpy( msgPtr, "Illegal opcode.\n" );
            break;
        case ILL_ILLOPN:
            strcpy( msgPtr, "Illegal operand.\n" );
            break;
        case ILL_ILLADR:
            strcpy( msgPtr, "Illegal addressing mode.\n" );
            break;
        case ILL_ILLTRP:
            strcpy( msgPtr, "Illegal trap.\n" );
            break;
        case ILL_PRVOPC:
            strcpy( msgPtr, "Privileged opcode.\n" );
            break;
        case ILL_PRVREG:
            strcpy( msgPtr, "Privileged register.\n" );
            break;
        case ILL_COPROC:
            strcpy( msgPtr, "Coprocessor error.\n" );
            break;
        case ILL_BADSTK:
            strcpy( msgPtr, "Internal stack error.\n" );
            break;
        default:
            break;
        }
        break;
    case SIGFPE:
        strcpy( msgPtr, "Floating-point exception.\n" );
        while( *msgPtr ) msgPtr++;
        switch( info->si_code )
        {
        case FPE_INTDIV:
            strcpy( msgPtr, "Integer divide by zero.\n" );
            break;
        case FPE_INTOVF:
            strcpy( msgPtr, "Integer overflow.\n" );
            break;
        case FPE_FLTDIV:
            strcpy( msgPtr, "Floating-point divide by zero.\n" );
            break;
        case FPE_FLTOVF:
            strcpy( msgPtr, "Floating-point overflow.\n" );
            break;
        case FPE_FLTUND:
            strcpy( msgPtr, "Floating-point underflow.\n" );
            break;
        case FPE_FLTRES:
            strcpy( msgPtr, "Floating-point inexact result.\n" );
            break;
        case FPE_FLTINV:
            strcpy( msgPtr, "Floating-point invalid operation.\n" );
            break;
        case FPE_FLTSUB:
            strcpy( msgPtr, "Subscript out of range.\n" );
            break;
        default:
            break;
        }
        break;
    case SIGSEGV:
        strcpy( msgPtr, "Invalid memory reference.\n" );
        while( *msgPtr ) msgPtr++;
        switch( info->si_code )
        {
        case SEGV_MAPERR:
            strcpy( msgPtr, "Address not mapped to object.\n" );
            break;
        case SEGV_ACCERR:
            strcpy( msgPtr, "Invalid permissions for mapped object.\n" );
            break;
#  ifdef SEGV_BNDERR
        case SEGV_BNDERR:
            strcpy( msgPtr, "Failed address bound checks.\n" );
            break;
#  endif
#  ifdef SEGV_PKUERR
        case SEGV_PKUERR:
            strcpy( msgPtr, "Access was denied by memory protection keys.\n" );
            break;
#  endif
        default:
            break;
        }
        break;
    case SIGPIPE:
        strcpy( msgPtr, "Broken pipe.\n" );
        while( *msgPtr ) msgPtr++;
        break;
    case SIGBUS:
        strcpy( msgPtr, "Bus error.\n" );
        while( *msgPtr ) msgPtr++;
        switch( info->si_code )
        {
        case BUS_ADRALN:
            strcpy( msgPtr, "Invalid address alignment.\n" );
            break;
        case BUS_ADRERR:
            strcpy( msgPtr, "Nonexistent physical address.\n" );
            break;
        case BUS_OBJERR:
            strcpy( msgPtr, "Object-specific hardware error.\n" );
            break;
#  ifdef BUS_MCEERR_AR
        case BUS_MCEERR_AR:
            strcpy( msgPtr, "Hardware memory error consumed on a machine check; action required.\n" );
            break;
#  endif
#  ifdef BUS_MCEERR_AO
        case BUS_MCEERR_AO:
            strcpy( msgPtr, "Hardware memory error detected in process but not consumed; action optional.\n" );
            break;
#  endif
        default:
            break;
        }
        break;
    case SIGABRT:
        strcpy( msgPtr, "Abort signal from abort().\n" );
        break;
    default:
        abort();
    }
    while( *msgPtr ) msgPtr++;

    if( signal != SIGPIPE )
    {
        strcpy( msgPtr, "Fault address: 0x" );
        while( *msgPtr ) msgPtr++;
        HexPrint( msgPtr, uint64_t( info->si_addr ) );
        *msgPtr++ = '\n';
    }

    {
        GetProfiler().SendCallstack( 60, "__kernel_rt_sigreturn" );

        TracyQueuePrepare( QueueType::CrashReport );
        item->crashReport.time = Profiler::GetTime();
        item->crashReport.text = (uint64_t)s_crashText;
        TracyQueueCommit( crashReportThread );
    }

    DIR* dp = opendir( "/proc/self/task" );
    if( !dp ) abort();

    const auto selfTid = syscall( SYS_gettid );

    struct dirent* ep;
    while( ( ep = readdir( dp ) ) != nullptr )
    {
        if( ep->d_name[0] == '.' ) continue;
        int tid = atoi( ep->d_name );
        if( tid != selfTid && tid != s_profilerTid && tid != s_symbolTid )
        {
            syscall( SYS_tkill, tid, TRACY_CRASH_SIGNAL );
        }
    }
    closedir( dp );

#ifdef TRACY_NEEDS_SYMBOL_WORKER
    if( selfTid == s_symbolTid ) s_symbolThreadRunning.store( false, std::memory_order_release );
#endif

    TracyLfqPrepare( QueueType::Crash );
    TracyLfqCommit;

    std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );
    GetProfiler().RequestShutdown();
    while( !GetProfiler().HasShutdownFinished() ) { std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) ); };

    abort();
}
#endif


enum { QueuePrealloc = 256 * 1024 };

TRACY_API int64_t GetFrequencyQpc()
{
#if defined _WIN32
    LARGE_INTEGER t;
    QueryPerformanceFrequency( &t );
    return t.QuadPart;
#else
    return 0;
#endif
}

#ifdef TRACY_DELAYED_INIT
struct ThreadNameData;
TRACY_API moodycamel::ConcurrentQueue<QueueItem>& GetQueue();

struct ProfilerData
{
    int64_t initTime = SetupHwTimer();
    moodycamel::ConcurrentQueue<QueueItem> queue;
    Profiler profiler;
    std::atomic<uint32_t> lockCounter { 0 };
    std::atomic<uint8_t> gpuCtxCounter { 0 };
    std::atomic<ThreadNameData*> threadNameData { nullptr };
};

struct ProducerWrapper
{
    ProducerWrapper( ProfilerData& data ) : detail( data.queue ), ptr( data.queue.get_explicit_producer( detail ) ) {}
    moodycamel::ProducerToken detail;
    tracy::moodycamel::ConcurrentQueue<QueueItem>::ExplicitProducer* ptr;
};

struct ProfilerThreadData
{
    ProfilerThreadData( ProfilerData& data ) : token( data ), gpuCtx( { nullptr } ) {}
    ProducerWrapper token;
    GpuCtxWrapper gpuCtx;
#  ifdef TRACY_ON_DEMAND
    LuaZoneState luaZoneState;
#  endif
};

std::atomic<int> RpInitDone { 0 };
std::atomic<int> RpInitLock { 0 };
thread_local bool RpThreadInitDone = false;
thread_local bool RpThreadShutdown = false;

#  ifdef TRACY_MANUAL_LIFETIME
ProfilerData* s_profilerData = nullptr;
static ProfilerThreadData& GetProfilerThreadData();
static std::atomic<bool> s_isProfilerStarted { false };
TRACY_API void StartupProfiler()
{
    s_profilerData = (ProfilerData*)tracy_malloc( sizeof( ProfilerData ) );
    new (s_profilerData) ProfilerData();

    DbgHelpLoaderFunc *pLoader = nullptr;
#if defined( _WIN32 ) && defined( TRACY_HAS_CALLSTACK ) && defined( TRACY_HAS_CUSTOM_DBG_HELP_LOADER )
    extern void *TracyCustomDbgHelpLoader();
    pLoader = &TracyCustomDbgHelpLoader;
#endif

    s_profilerData->profiler.SpawnWorkerThreads( nullptr, pLoader );
    GetProfilerThreadData().token = ProducerWrapper( *s_profilerData );
    s_isProfilerStarted.store( true, std::memory_order_seq_cst );
}
static ProfilerData& GetProfilerData()
{
    assert( s_profilerData );
    return *s_profilerData;
}
TRACY_API void ShutdownProfiler()
{
    s_isProfilerStarted.store( false, std::memory_order_seq_cst );
    s_profilerData->~ProfilerData();
    tracy_free( s_profilerData );
    s_profilerData = nullptr;
    rpmalloc_finalize();
    RpThreadInitDone = false;
    RpInitDone.store( 0, std::memory_order_release );
}
TRACY_API bool IsProfilerStarted()
{
    return s_isProfilerStarted.load( std::memory_order_seq_cst );
}
#  else
static std::atomic<int> profilerDataLock { 0 };
static std::atomic<ProfilerData*> profilerData { nullptr };

static ProfilerData& GetProfilerData()
{
    auto ptr = profilerData.load( std::memory_order_acquire );
    if( !ptr )
    {
        int expected = 0;
        while( !profilerDataLock.compare_exchange_weak( expected, 1, std::memory_order_release, std::memory_order_relaxed ) ) { expected = 0; YieldThread(); }
        ptr = profilerData.load( std::memory_order_acquire );
        if( !ptr )
        {
            ptr = (ProfilerData*)tracy_malloc( sizeof( ProfilerData ) );
            new (ptr) ProfilerData();
            profilerData.store( ptr, std::memory_order_release );
        }
        profilerDataLock.store( 0, std::memory_order_release );
    }
    return *ptr;
}
#  endif

// GCC prior to 8.4 had a bug with function-inline thread_local variables. Versions of glibc beginning with
// 2.18 may attempt to work around this issue, which manifests as a crash while running static destructors
// if this function is compiled into a shared object. Unfortunately, centos7 ships with glibc 2.17. If running
// on old GCC, use the old-fashioned way as a workaround
// See: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=85400
#if !defined(__clang__) && defined(__GNUC__) && ((__GNUC__ < 8) || ((__GNUC__ == 8) && (__GNUC_MINOR__ < 4)))
struct ProfilerThreadDataKey
{
public:
    ProfilerThreadDataKey()
    {
        int val = pthread_key_create(&m_key, sDestructor);
        static_cast<void>(val); // unused
        assert(val == 0);
    }
    ~ProfilerThreadDataKey()
    {
        int val = pthread_key_delete(m_key);
        static_cast<void>(val); // unused
        assert(val == 0);
    }
    ProfilerThreadData& get()
    {
        void* p = pthread_getspecific(m_key);
        if (!p)
        {
            p = (ProfilerThreadData*)tracy_malloc( sizeof( ProfilerThreadData ) );
            new (p) ProfilerThreadData(GetProfilerData());
            pthread_setspecific(m_key, p);
        }
        return *static_cast<ProfilerThreadData*>(p);
    }
private:
    pthread_key_t m_key;

    static void sDestructor(void* p)
    {
        ((ProfilerThreadData*)p)->~ProfilerThreadData();
        tracy_free(p);
    }
};

static ProfilerThreadData& GetProfilerThreadData()
{
    static ProfilerThreadDataKey key;
    return key.get();
}
#else
static ProfilerThreadData& GetProfilerThreadData()
{
    thread_local ProfilerThreadData data( GetProfilerData() );
    return data;
}
#endif

TRACY_API moodycamel::ConcurrentQueue<QueueItem>::ExplicitProducer* GetToken() { return GetProfilerThreadData().token.ptr; }
TRACY_API Profiler& GetProfiler() { return GetProfilerData().profiler; }
TRACY_API moodycamel::ConcurrentQueue<QueueItem>& GetQueue() { return GetProfilerData().queue; }
TRACY_API int64_t GetInitTime() { return GetProfilerData().initTime; }
TRACY_API std::atomic<uint32_t>& GetLockCounter() { return GetProfilerData().lockCounter; }
TRACY_API std::atomic<uint8_t>& GetGpuCtxCounter() { return GetProfilerData().gpuCtxCounter; }
TRACY_API GpuCtxWrapper& GetGpuCtx() { return GetProfilerThreadData().gpuCtx; }
TRACY_API uint32_t GetThreadHandle() { return detail::GetThreadHandleImpl(); }

#  ifdef TRACY_ON_DEMAND
TRACY_API LuaZoneState& GetLuaZoneState() { return GetProfilerThreadData().luaZoneState; }
#  endif

#  ifndef TRACY_MANUAL_LIFETIME
namespace
{
    const auto& __profiler_init = GetProfiler();
}
#  endif

#else

// MSVC static initialization order solution. gcc/clang uses init_order() to avoid all this.

// 1a. But s_queue is needed for initialization of variables in point 2.
extern moodycamel::ConcurrentQueue<QueueItem> s_queue;

// 2. If these variables would be in the .CRT$XCB section, they would be initialized only in main thread.
thread_local moodycamel::ProducerToken init_order(107) s_token_detail( s_queue, detail::GetThreadHandleImpl() );
thread_local ProducerWrapper init_order(108) s_token { s_queue.get_explicit_producer( s_token_detail ) };
thread_local ThreadHandleWrapper init_order(104) s_threadHandle { detail::GetThreadHandleImpl() };

#  ifdef _MSC_VER
// 1. Initialize these static variables before all other variables.
#    pragma warning( disable : 4075 )
#    pragma init_seg( ".CRT$XCB" )
#  endif

static InitTimeWrapper init_order(101) s_initTime { SetupHwTimer() };
std::atomic<int> init_order(102) RpInitDone( 0 );
std::atomic<int> init_order(102) RpInitLock( 0 );
thread_local bool RpThreadInitDone = false;
thread_local bool RpThreadShutdown = false;
moodycamel::ConcurrentQueue<QueueItem> init_order(103) s_queue( QueuePrealloc );
std::atomic<uint32_t> init_order(104) s_lockCounter( 0 );
std::atomic<uint8_t> init_order(104) s_gpuCtxCounter( 0 );

thread_local GpuCtxWrapper init_order(104) s_gpuCtx { nullptr };

#  ifdef TRACY_ON_DEMAND
thread_local LuaZoneState init_order(104) s_luaZoneState { 0, false };
#  endif

static Profiler init_order(105) s_profiler;

TRACY_API moodycamel::ConcurrentQueue<QueueItem>::ExplicitProducer* GetToken() { return s_token.ptr; }
TRACY_API Profiler& GetProfiler() { return s_profiler; }
TRACY_API moodycamel::ConcurrentQueue<QueueItem>& GetQueue() { return s_queue; }
TRACY_API int64_t GetInitTime() { return s_initTime.val; }
TRACY_API std::atomic<uint32_t>& GetLockCounter() { return s_lockCounter; }
TRACY_API std::atomic<uint8_t>& GetGpuCtxCounter() { return s_gpuCtxCounter; }
TRACY_API GpuCtxWrapper& GetGpuCtx() { return s_gpuCtx; }
TRACY_API uint32_t GetThreadHandle() { return s_threadHandle.val; }

#  ifdef TRACY_ON_DEMAND
TRACY_API LuaZoneState& GetLuaZoneState() { return s_luaZoneState; }
#  endif
#endif

TRACY_API bool ProfilerAvailable() { return s_instance != nullptr; }
TRACY_API bool ProfilerAllocatorAvailable() { return !RpThreadShutdown; }

constexpr static size_t SafeSendBufferSize = 65536;

TRACY_API void RequestListenAndBroadcast()
{
    GetProfiler().RequestListenAndBroadcast();
}


TRACY_API void ShutdownAndWait()
{
    Profiler &p = GetProfiler();
    p.Disconnect();
    p.RequestShutdown();
    p.StopWorkerThreads();
    while( !p.HasShutdownFinished() )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    };
}


#define LockAssert(cond) assert(cond)


// TODO(sev): Currently, tracy only supports uint8_t threads. per lock.
// When running certain apps, we created a tremendous amount of threads that all hammer the same lock.
// This simply does not work for these situations so we may need to switch to a thread locking list instead of a
// fixed array. *however* this also won't work correctly on the tracy server side where we also have the same limit
enum { TracyMaxLockThreads = (std::numeric_limits<uint8_t>::max() + 1) };

struct LockString
{
    LockString()
        : str( nullptr )
        , len( 0 )
        , hash( 0 )
    {
    }

    LockString( const char *str, size_t len )
        : str( str )
        , len( len )
        , hash( std::hash<std::string_view>{}( std::string_view( str, len ) ) )
    {
    }

    const char *str;
    size_t len;
    size_t hash;

    bool operator==( const LockString &rhs ) const
    {
        const std::string_view l( str, len );
        const std::string_view r( rhs.str, rhs.len );
        const bool result = ( l == r );
        return result;
    }
};


static tracy_force_inline uint64_t SendLockWait( uint32_t lockId, uint32_t tid )
{
    QueueItem *wait = Profiler::QueueSerial();
    uint64_t time = Profiler::GetTime();
    MemWrite( &wait->hdr.type, QueueType::LockWait );
    MemWrite( &wait->lockWait.thread, tid );
    MemWrite( &wait->lockWait.id, lockId );
    MemWrite( &wait->lockWait.time, time );
    Profiler::QueueSerialFinish();
    return time;
}


static tracy_force_inline void SendLockMark( uint32_t lockId, uint32_t tid, const LockString& file, int32_t line )
{
    QueueItem *mark = Profiler::QueueSerial();
    MemWrite( &mark->hdr.type, QueueType::LockMarkFileLine );
    MemWrite( &mark->lockMarkFileLine.thread, tid );
    MemWrite( &mark->lockMarkFileLine.id, lockId );
    MemWrite( &mark->lockMarkFileLine.file, (uint64_t)file.str );
    MemWrite( &mark->lockMarkFileLine.line, line );
    Profiler::QueueSerialFinish();
}


static tracy_force_inline uint64_t SendLockObtain( uint32_t lockId, uint32_t tid )
{
    QueueItem *obtain = Profiler::QueueSerial();
    uint64_t time = Profiler::GetTime();
    MemWrite( &obtain->hdr.type, QueueType::LockObtain );
    MemWrite( &obtain->lockObtain.thread, tid );
    MemWrite( &obtain->lockObtain.id, lockId );
    MemWrite( &obtain->lockObtain.time, time );
    Profiler::QueueSerialFinish();
    return time;
}


static tracy_force_inline uint64_t SendLockRelease( uint32_t lockId )
{
    QueueItem *release = Profiler::QueueSerial();
    uint64_t time = Profiler::GetTime();
    MemWrite( &release->hdr.type, QueueType::LockRelease );
    MemWrite( &release->lockRelease.id, lockId );
    MemWrite( &release->lockRelease.time, time );
    Profiler::QueueSerialFinish();
    return time;
}


struct LockStringHash
{
    size_t operator()(const LockString& key) const {
        return key.hash;
    }
};


struct LockMutex
{
    tracy_force_inline void lock()   { mutex.lock(); }
    tracy_force_inline void unlock() { mutex.unlock(); }

#if defined _MSC_VER
    tracy_force_inline void lock_shared()   { mutex.lock_shared(); }
    tracy_force_inline void unlock_shared() { mutex.unlock_shared(); }
#else
    tracy_force_inline void lock_shared()   { mutex.lock(); }
    tracy_force_inline void unlock_shared() { mutex.unlock(); }
#endif

    TracyMutex mutex;
};


struct LockItem
{
    LockItem( uint32_t id, uint32_t connectId, int64_t time, const LockString& str, uint64_t srcloc, bool active )
        : id32( id )
        , syncConnectionId(connectId)
        , time(time)
        , name(str)
        , srcloc(srcloc)
        , type(LockType::Lockable)
        , active(active)
#ifdef TRACY_ON_DEMAND
        , lockThread(0)
        , lockCount(0)
        , lockTime(0)
        , lockfile( nullptr )
        , lockline( 0 )
        , waitEvents( {0} )
#endif // ifdef TRACY_ON_DEMAND
    {
    }

    const uint32_t id32;
    uint32_t syncConnectionId;
    const int64_t time;
    LockString name;
    const uint64_t srcloc;
    const LockType type;
    bool active;

#ifdef TRACY_ON_DEMAND
    enum LockEventType : uint8_t
    {
        Wait,
        WaitShared,
        Obtain,
        ObtainShared
    };

    struct LockEvent
    {
        LockEventType type;
        uint32_t lockId;
        uint32_t tid;
        int64_t time;
        const char *lockfile;
        int lockline;
    };

    uint32_t lockThread;
    std::atomic<uint32_t> lockCount;
    int64_t lockTime;
    const char *lockfile;
    int lockline;

    struct WaitAndSrcloc
    {
        int64_t time;
    };
    // Element at index 0 is a sentinel
    std::array<WaitAndSrcloc, TracyMaxLockThreads + 1> waitEvents;
#endif
};


class ProfilerSyncState
{
    uint32_t m_mode;
public:
    enum SyncMode
    {
        None,
        Locks = ( 1 << 0 ),
        Zones = ( 1 << 1 ),
    };

    ProfilerSyncState()
        : m_mode( None )
        , m_threadCountOverflow( false )
        , m_globalThreadCount(0)
        , m_indexToTidLut( {0} )
    {
        // Make sure we bump the count to 1 before using it. 0 is our sentinel value
        m_globalThreadCount.fetch_add( 1 );

        const uint32_t tid = GetThreadHandle();
        GetThreadIndex( tid );
    }

    ~ProfilerSyncState()
    {
        for ( LockString str : m_lockStrings )
        {
            tracy_free( (void*)str.str );
        }
    }

    void SetSyncModeEnabled( SyncMode mode, bool enable )
    {
        if ( enable )
        {
            m_mode |= mode;
        }
        else
        {
            m_mode &= ~mode;
        }
    }

    bool HasSyncMode( SyncMode mode ) const
    {
        return ( ( m_mode & mode ) != 0 );
    }

    void Clear()
    {
#if defined(TRACY_HAS_BG_SUPPORT)
        m_threadZoneStack.clear();
        m_contextZoneStack.clear();
        m_frames.clear();
#endif // if defined(TRACY_HAS_BG_SUPPORT)
    }

    void SyncBegin()
    {
        m_LockItemMutex.lock();
    }

    void SyncEnd()
    {
        m_LockItemMutex.unlock();
    }


    bool LockTrackingEnabled() const
    {
        return HasSyncMode( Locks ) && ( m_threadCountOverflow.load() == false );
    }

    // NOTE: lock tracking api
    void LockAnnounce( uint32_t connId, uint64_t lockId64, uint32_t lockId32, const SourceLocationData* pSrcloc, const char *name, size_t len );
    void LockTerminate( uint32_t connId, uint64_t lockId64 );
    void LockSetName( uint32_t connId, uint64_t lockId64, const char* name, size_t len );
    void LockWaitBegin( uint32_t connId, uint64_t lockId64, const char *file, int line );
    void LockAcquired( uint32_t connId, uint64_t lockId64, const char *file, int line );
    void LockAcquiredTry( uint32_t connId, uint64_t lockId64, const char *file, int line );
    void LockReleased( uint32_t connId, uint64_t lockId64, const char *file, int line );

    bool SynchronizeLocks( Profiler& p, Profiler::RefTimes& refTimes, uint32_t connId );

    // NOTE: lock syncing api
    bool HasActiveLocks() const
    {
        return !m_activeLocks.empty();
    }

    size_t GetActiveLockCount() const
    {
        return m_activeLocks.size();
    }

    using ActiveLockMap = TracyUnorderedMap< uint64_t, LockItem >;
    tracy_force_inline const ActiveLockMap& GetActiveLocks() const
    {
        return m_activeLocks;
    }

    uint32_t GetTidForWaitingLockIndex( size_t waitIndex ) const
    {
        return m_indexToTidLut[ waitIndex ];
    }

private:
    std::atomic< bool > m_threadCountOverflow;
    LockMutex m_LockItemMutex;
    ActiveLockMap m_activeLocks;

    uint32_t GetThreadIndex( uint32_t tid );
    LockItem *GetLockItem( uint64_t lockId64 );

    std::atomic<uint32_t> m_globalThreadCount;
    // Element at index 0 is a sentinel
    std::array<uint32_t, TracyMaxLockThreads + 1> m_indexToTidLut;

    LockString StoreAndGetLockString( const char *name, size_t len );

    // TODO(sev): That probably isn't needed anymore since we now shut down tracy before unloading any dlls
    LockMutex m_LockStringMutex;
    TracyUnorderedSet< LockString, LockStringHash > m_lockStrings;

    struct LockTls
    {
        uint32_t m_globalThreadIndex;
        uint64_t lockId64;
        LockItem* pLockItem;
    };
    static inline thread_local LockTls m_Tls;


#if defined(TRACY_HAS_BG_SUPPORT)
public:
    void UpdateSyncInfo( QueueType type, QueueItem *item, const Profiler::RefTimes &refTimes );

    bool Synchronize( Profiler& p, const Profiler::RefTimes& syncRefTimes, Profiler::RefTimes& refTimes, uint32_t connId );

    bool SynchronizeZones( Profiler& p, const Profiler::RefTimes& syncRefTimes, Profiler::RefTimes& refTimes, uint32_t connId );
    bool SynchronizeContexts( Profiler& p, const Profiler::RefTimes& syncRefTimes, Profiler::RefTimes& refTimes, uint32_t connId );
    bool SynchronizeFrames( Profiler& p, Profiler::RefTimes& refTimes, uint32_t connId );

private:
    struct ZoneStackItem
    {
        uint32_t validationId;
        QueueType type;
        QueueItem item;
    };

    struct ZoneInfo
    {
        uint32_t id = 0;
        uint32_t validationId = 0;
        TracyVector< ZoneStackItem > stack;
    };

    using FrameMap = TracyUnorderedMap< uint64_t, QueueItem >;
    using ZoneStackMap = TracyUnorderedMap< uint32_t, ZoneInfo >;

    void PushFrame( QueueType type, QueueItem *item )
    {
        assert( type == QueueType::FrameMarkMsgStart );

        uint64_t namePtr = MemRead<uint64_t>( &item->frameMark.name );
        assert( !m_frames.contains( namePtr ) );
        m_frames[ namePtr ] = *item;
    }

    void PopFrame( QueueType type, QueueItem *item )
    {
        assert( type == QueueType::FrameMarkMsgEnd );

        uint64_t namePtr = MemRead<uint64_t>( &item->frameMark.name );
        assert( m_frames.contains( namePtr ) );
        m_frames.erase( namePtr );
    }

    void SetZoneValidation( uint32_t tid, QueueType type, QueueItem *item )
    {
        ProfilerSyncState::SetZoneValidation( m_threadZoneStack, tid, type, item );
    }

    void ClearZoneValidation( uint32_t tid )
    {
        ProfilerSyncState::ClearZoneValidation( m_threadZoneStack, tid );
    }

    void PushThreadZone( uint32_t tid, QueueType type, QueueItem *item )
    {
        ProfilerSyncState::PushZone( m_threadZoneStack, tid, type, item );
    }

    void PopThreadZone( uint32_t tid, QueueType type, QueueItem *item )
    {
        ProfilerSyncState::PopZone( m_threadZoneStack, tid, type, item );
    }

    void PushContextZone( uint32_t cid, QueueType type, QueueItem *item )
    {
        ProfilerSyncState::PushZone( m_contextZoneStack, cid, type, item );
    }

    void PopContextZone( uint32_t id, QueueType type, QueueItem *item )
    {
        ProfilerSyncState::PopZone( m_contextZoneStack, id, type, item );
    }

    static void SetZoneValidation( ZoneStackMap& rMap, uint32_t id, QueueType type, QueueItem *item )
    {
#ifndef TRACY_NO_VERIFY
        assert( type == QueueType::ZoneValidation );
        ZoneInfo& rInfo = rMap[ id ];
        rInfo.id = id;
        rInfo.validationId = MemRead<uint32_t>( &item->zoneValidation.id );
#endif // ifndef TRACY_NO_VERIFY
    }


    static void ClearZoneValidation( ZoneStackMap& rMap, uint32_t id )
    {
#ifndef TRACY_NO_VERIFY
        ZoneInfo& rInfo = rMap[ id ];
        assert( rInfo.stack.empty() || (rInfo.validationId == rInfo.stack.back().validationId) );
        rInfo.validationId = 0;
#endif // ifndef TRACY_NO_VERIFY
    }

    static void PushZoneItem( ZoneInfo& rInfo, uint32_t id, QueueType type, QueueItem* item )
    {
        ZoneStackItem stackItem = { .validationId = rInfo.validationId, .type = type, .item = *item};
        rInfo.stack.push_back(stackItem);
        rInfo.validationId = 0;
    }


    static void PopZoneItem( ZoneInfo& rInfo, QueueType type, QueueItem* item )
    {
        assert(!rInfo.stack.empty());
        if ( !rInfo.stack.empty() )
        {
#ifndef TRACY_NO_VERIFY
            assert( rInfo.validationId == rInfo.stack.back().validationId );
#endif // ifndef TRACY_NO_VERIFY

            rInfo.stack.pop_back();
        }

        rInfo.validationId = 0;
    }

    static void PushZone( ZoneStackMap& rMap, uint32_t id, QueueType type, QueueItem* item )
    {
        assert(    (type == QueueType::ZoneBegin)
                || (type == QueueType::ZoneBeginCallstack)
                || (type == QueueType::GpuZoneBegin)
                || (type == QueueType::GpuZoneBeginCallstack)
                || (type == QueueType::GpuZoneBeginSerial )
                || (type == QueueType::GpuZoneBeginCallstackSerial) );

        ZoneInfo& rInfo = rMap[ id ];
        rInfo.id = id;
        PushZoneItem( rInfo, id, type, item );
    }

    static void PopZone( ZoneStackMap& rMap, uint32_t id, QueueType type, QueueItem* item )
    {
        assert( (type == QueueType::ZoneEnd) || (type == QueueType::GpuZoneEnd) || (type == QueueType::GpuZoneEndSerial) );
        assert( rMap.find(id) != rMap.end() );
        ZoneInfo& rInfo = rMap[id];
        rInfo.id = id;
        PopZoneItem( rInfo, type, item );
    }

    FrameMap m_frames;
    ZoneStackMap m_threadZoneStack;
    ZoneStackMap m_contextZoneStack;
#endif // if defined(TRACY_HAS_BG_SUPPORT)
};


bool ProfilerSyncState::SynchronizeLocks( Profiler& p, Profiler::RefTimes& refTimes, uint32_t connId )
{
    bool result = true;

#ifdef TRACY_ON_DEMAND
#define AppendItem( item )                                              \
    if ( !p.AppendData( &(item), QueueDataSize[ (item).hdr.idx ] ) ) {  \
        result = false;                                                 \
        goto done;                                                      \
    }

    if ( HasSyncMode( ProfilerSyncState::Locks ) && !m_activeLocks.empty() )
    {
        const size_t activeLockCount = m_activeLocks.size();
        {
            QueueItem globalLockSyncBegin;
            MemWrite( &globalLockSyncBegin.hdr.type, QueueType::GlobalLockSyncBegin );
            MemWrite( &globalLockSyncBegin.globalLockSyncBegin.count, GetLockCounter().load( std::memory_order_relaxed ) );
            MemWrite( &globalLockSyncBegin.globalLockSyncBegin.active, activeLockCount );
            AppendItem( globalLockSyncBegin );
        }

        {
            TracyVector< LockItem::LockEvent > sendLockEvents;
            sendLockEvents.reserve( activeLockCount );

            for ( ActiveLockMap::reference entry : m_activeLocks )
            {
                LockItem& lockitem = entry.second;
                lockitem.active = true;
                LockAssert( lockitem.syncConnectionId < connId );
                LockAssert( lockitem.time >= GetInitTime() );
                lockitem.syncConnectionId = connId;

                const uint32_t id32 = lockitem.id32;

                {
                    QueueItem item;
                    MemWrite( &item.hdr.type, QueueType::LockAnnounce );
                    MemWrite( &item.lockAnnounce.id, id32 );
                    MemWrite( &item.lockAnnounce.time, lockitem.time );
                    MemWrite( &item.lockAnnounce.lckloc, lockitem.srcloc );
                    MemWrite( &item.lockAnnounce.type, lockitem.type );
                    AppendItem( item );
                }

                if ( lockitem.name.str )
                {
                    p.SendSingleString( lockitem.name.str, lockitem.name.len );
                    QueueItem nameitem;
                    MemWrite( &nameitem.hdr.type, QueueType::LockName );
                    MemWrite( &nameitem.lockNameFat.id, id32 );
                    MemWrite( &nameitem.lockNameFat.name, ( uint64_t )lockitem.name.str );
                    MemWrite( &nameitem.lockNameFat.size, ( uint16_t )lockitem.name.len );
                    AppendItem( nameitem );
                }

                const size_t lockCount = lockitem.lockCount.load();
                for ( size_t count = 0; count < lockCount; count++ )
                {
                    sendLockEvents.push_back( LockItem::LockEvent{LockItem::Obtain, id32, lockitem.lockThread, lockitem.lockTime, lockitem.lockfile, lockitem.lockline });
                }

                for ( size_t waitIndex = 0; waitIndex < lockitem.waitEvents.size(); waitIndex++ )
                {
                    const int64_t time = lockitem.waitEvents[ waitIndex ].time;
                    if ( time > 0 )
                    {
                        const uint32_t tid = m_indexToTidLut[ waitIndex ];
                        LockAssert( time >= lockitem.time );
                        const char *lockfile = lockitem.lockfile;
                        int lockline = lockitem.lockline;
                        sendLockEvents.push_back( LockItem::LockEvent{LockItem::Wait, id32, tid, time, lockfile, lockline });
                    }
                }
            }

            std::sort( sendLockEvents.begin(), sendLockEvents.end(),
                       [] ( const LockItem::LockEvent &lhs, const LockItem::LockEvent &rhs )
                       {
                           return lhs.time < rhs.time;
                       } );

            for ( size_t index = 0; index < sendLockEvents.size(); index++  )
            {
                const LockItem::LockEvent *lev = &sendLockEvents[ index ];
                switch ( lev->type )
                {
                    case LockItem::Wait:
                    case LockItem::WaitShared:
                    {
                        {
                            QueueType type = ( ( lev->type == LockItem::Wait ) ? QueueType::LockWait : QueueType::LockSharedWait );
                            int64_t t = lev->time;
                            int64_t dt = t - refTimes.m_refTimeSerial;
                            refTimes.m_refTimeSerial = t;

                            QueueItem lockItem;
                            MemWrite( &lockItem.hdr.type, type );
                            MemWrite( &lockItem.lockWait.id, lev->lockId );
                            MemWrite( &lockItem.lockWait.thread, lev->tid );
                            MemWrite( &lockItem.lockWait.time, dt );
                            AppendItem( lockItem );
                        }

                        if ( lev->lockfile )
                        {
                            QueueItem markItem;
                            MemWrite( &markItem.hdr.type, QueueType::LockMarkFileLine );
                            MemWrite( &markItem.lockMarkFileLine.thread, lev->tid );
                            MemWrite( &markItem.lockMarkFileLine.id, lev->lockId );
                            MemWrite( &markItem.lockMarkFileLine.file, lev->lockfile);
                            MemWrite( &markItem.lockMarkFileLine.line, lev->lockline);
                            AppendItem( markItem );
                        }
                    } break;

                    case LockItem::Obtain:
                    case LockItem::ObtainShared:
                    {
                        {
                            // NOTE(sev): since we clear the wait upon obtain, make sure we send a wait nonetheless to correctly
                            // update the state on the server side.
                            QueueType type = ( ( lev->type == LockItem::Obtain ) ? QueueType::LockWait : QueueType::LockSharedWait );
                            int64_t t = lev->time;
                            int64_t dt = t - refTimes.m_refTimeSerial;
                            refTimes.m_refTimeSerial = t;

                            QueueItem lockItem;
                            MemWrite( &lockItem.hdr.type, type );
                            MemWrite( &lockItem.lockWait.id, lev->lockId );
                            MemWrite( &lockItem.lockWait.thread, lev->tid );
                            MemWrite( &lockItem.lockWait.time, dt );
                            AppendItem( lockItem );
                        }

                        if ( lev->lockfile )
                        {
                            QueueItem markItem;
                            MemWrite( &markItem.hdr.type, QueueType::LockMarkFileLine );
                            MemWrite( &markItem.lockMarkFileLine.thread, lev->tid );
                            MemWrite( &markItem.lockMarkFileLine.id, lev->lockId );
                            MemWrite( &markItem.lockMarkFileLine.file, lev->lockfile);
                            MemWrite( &markItem.lockMarkFileLine.line, lev->lockline);
                            AppendItem( markItem );
                        }

                        {
                            QueueType type = ( ( lev->type == LockItem::Obtain ) ? QueueType::LockObtain : QueueType::LockSharedObtain );

                            int64_t t = lev->time;
                            int64_t dt = t - refTimes.m_refTimeSerial;
                            refTimes.m_refTimeSerial = t;

                            QueueItem lockItem;
                            MemWrite( &lockItem.hdr.type, type );
                            MemWrite( &lockItem.lockObtain.id, lev->lockId );
                            MemWrite( &lockItem.lockObtain.thread, lev->tid );
                            MemWrite( &lockItem.lockObtain.time, dt );
                            AppendItem( lockItem );
                        }

                        if ( lev->lockfile )
                        {
                            QueueItem markItem;
                            MemWrite( &markItem.hdr.type, QueueType::LockMarkFileLine );
                            MemWrite( &markItem.lockMarkFileLine.thread, lev->tid );
                            MemWrite( &markItem.lockMarkFileLine.id, lev->lockId );
                            MemWrite( &markItem.lockMarkFileLine.file, lev->lockfile);
                            MemWrite( &markItem.lockMarkFileLine.line, lev->lockline);
                            AppendItem( markItem );
                        }
                    } break;
                }
            }
        }

        {
            QueueItem globalLockSyncEnd;
            MemWrite( &globalLockSyncEnd.hdr.type, QueueType::GlobalLockSyncEnd );
            MemWrite( &globalLockSyncEnd.globalLockSyncEnd.count, GetLockCounter().load( std::memory_order_relaxed ) );
            MemWrite( &globalLockSyncEnd.globalLockSyncEnd.active, activeLockCount );
            AppendItem( globalLockSyncEnd );
        }

        if ( !p.CommitPendingData() )
        {
            result = false;
            goto done;
        }
    }


done:

#undef AppendItem
#endif // TRACY_ON_DEMAND

    return result;
}


void ProfilerSyncState::LockAnnounce( uint32_t connId, uint64_t lockId64, uint32_t lockId32, const SourceLocationData* pSrcloc, const char *name, size_t len )
{
    LockAssert( LockTrackingEnabled() );

    LockString nameStr = StoreAndGetLockString( name, len );
    const uint64_t srcloc = (uint64_t)pSrcloc;
    const int64_t time = Profiler::GetTime();

    // NOTE: We grab the lock item mutex first, before we check the connection.
    // This solves the problem where we would end up in lock announce, not yet connected
    // but we'll miss sending the announce event since the lock item is not net in the sync list.
    m_LockItemMutex.lock();

#ifdef TRACY_ON_DEMAND
    const bool active = ( connId != 0 );
#else
    const bool active = true;
#endif

    LockAssert( !m_activeLocks.contains( lockId64 ) );
#if (__cplusplus >= 201703L)
    m_activeLocks.try_emplace( lockId64, lockId32, connId, time, nameStr, srcloc, active );
#else // if (__cplusplus >= 201703L)
    m_activeLocks.emplace( std::piecewise_construct,
                           std::forward_as_tuple(lockId64),
                           std::forward_as_tuple(lockId32, connId, time, nameStr, srcloc, active) );
#endif // if (__cplusplus >= 201703L)
    m_LockItemMutex.unlock();

    if ( active )
    {
        {
            QueueItem *item = Profiler::QueueSerial();
            MemWrite( &item->hdr.type, QueueType::LockAnnounce );
            MemWrite( &item->lockAnnounce.id, lockId32 );
            MemWrite( &item->lockAnnounce.time, time );
            MemWrite( &item->lockAnnounce.lckloc, srcloc );
            MemWrite( &item->lockAnnounce.type, LockType::Lockable );
            Profiler::QueueSerialFinish();
        }

        if ( nameStr.str )
        {
            QueueItem *nameitem = Profiler::QueueSerial();
            MemWrite( &nameitem->hdr.type, QueueType::LockName );
            MemWrite( &nameitem->lockNameFat.id, lockId32 );
            MemWrite( &nameitem->lockNameFat.name, nameStr.str );
            MemWrite( &nameitem->lockNameFat.size, nameStr.len );
            Profiler::QueueSerialFinish();
        }
    }
}


void ProfilerSyncState::LockTerminate( uint32_t connId, uint64_t lockId64 )
{
    LockAssert( LockTrackingEnabled() );

    LockItem *pLockItem = GetLockItem( lockId64 );
    bool wasActive = pLockItem->active;
    LockAssert( !pLockItem->active || (pLockItem->syncConnectionId == connId) );
    uint32_t id32 = pLockItem->id32;

    assert( m_Tls.lockId64 != lockId64 );

    {
        m_LockItemMutex.lock();
        m_activeLocks.erase( lockId64 );
        m_LockItemMutex.unlock();
    }

#ifdef TRACY_ON_DEMAND
    const bool active = wasActive && ( connId != 0 );
#else
    const bool active = true;
#endif

    if ( active )
    {
        QueueItem* item = Profiler::QueueSerial();
        MemWrite( &item->hdr.type, QueueType::LockTerminate );
        MemWrite( &item->lockTerminate.id, ( uint32_t ) id32 );
        MemWrite( &item->lockTerminate.time, Profiler::GetTime() );
        Profiler::QueueSerialFinish();
    }
}


void ProfilerSyncState::LockSetName( uint32_t connId, uint64_t lockId64, const char* name, size_t len )
{
    LockAssert( LockTrackingEnabled() );

    LockString nameStr = StoreAndGetLockString( name, len );
    LockItem *pLockItem = GetLockItem( lockId64 );
    LockAssert( !pLockItem->active || (pLockItem->syncConnectionId == connId) );

    pLockItem->name = nameStr;

#ifdef TRACY_ON_DEMAND
    const bool active = pLockItem->active && ( connId != 0 ) && ( pLockItem->syncConnectionId == connId );
#else
    const bool active = true;
#endif

    if ( active )
    {
        QueueItem *nameitem = Profiler::QueueSerial();
        MemWrite( &nameitem->hdr.type, QueueType::LockName );
        MemWrite( &nameitem->lockNameFat.id, pLockItem->id32 );
        MemWrite( &nameitem->lockNameFat.name, ( uint64_t ) nameStr.str );
        MemWrite( &nameitem->lockNameFat.size, ( uint16_t ) nameStr.len );
        Profiler::QueueSerialFinish();
    }
}


void ProfilerSyncState::LockWaitBegin( uint32_t connId, uint64_t lockId64, const char *file, int line )
{
    LockAssert( LockTrackingEnabled() );

    LockItem *pLockItem = GetLockItem( lockId64 );
    if ( pLockItem )
    {
        m_Tls.lockId64 = lockId64;
        m_Tls.pLockItem = pLockItem;

        const uint32_t id32 = pLockItem->id32;
        const uint32_t tid = GetThreadHandle();
        int64_t time = 0;

        const bool isConnected = ( connId != 0 );
        const bool wasActive = isConnected && pLockItem->active;
        LockAssert( !pLockItem->active || (pLockItem->syncConnectionId == connId) );

#ifdef TRACY_ON_DEMAND
    const bool active = wasActive;
#else
    const bool active = true;
#endif

        if ( active )
        {
            // NOTE: Unfortunately we can't use file directly. The string, while static
            // could be in the mapped memory of a dll which, when unloaded will be unmapped.
            // However the tracy server may at any later point in time request the value for
            // this string at which point the memory would be inaccessible. So we must
            // copy the string here...
            // Also, we don't copy it all the time because we don't want to incur additional
            // overhead when no server is connected.
            const size_t len = ( file ? strlen( file ) : 0 );
            LockString fileStr = StoreAndGetLockString( file, len );
            time = SendLockWait( id32, tid );
            SendLockMark( id32, tid, fileStr, line );
        }
        else
        {
            time = Profiler::GetTime();
        }

#ifdef TRACY_ON_DEMAND
        const uint32_t index = GetThreadIndex( tid );
        assert( index < pLockItem->waitEvents.size() );
        LockAssert( time >= GetInitTime() );
        LockAssert( time >= pLockItem->time );
        pLockItem->waitEvents[ index ] = { time };
#endif
    }
}


void ProfilerSyncState::LockAcquired( uint32_t connId, uint64_t lockId64, const char *file, int line )
{
    LockAssert( LockTrackingEnabled() );

    LockItem *pLockItem = GetLockItem( lockId64 );
    if ( pLockItem )
    {
        const uint32_t tid = GetThreadHandle();
        const uint32_t id32 = pLockItem->id32;
        int64_t time = 0;

        const bool isConnected = ( connId != 0 );
        pLockItem->active = isConnected;
        LockAssert( !pLockItem->active || (pLockItem->syncConnectionId == connId) );

#ifdef TRACY_ON_DEMAND
        const bool active = pLockItem->active;
#else
        const bool active = true;
#endif

        if ( active )
        {
            // NOTE: Unfortunately we can't use file directly. The string, while static
            // could be in the mapped memory of a dll which, when unloaded will be unmapped.
            // However the tracy server may at any later point in time request the value for
            // this string at which point the memory would be inaccessible. So we must
            // copy the string here...
            // Also, we don't copy it all the time because we don't want to incur additional
            // overhead when no server is connected.
            const size_t len = ( file ? strlen( file ) : 0 );
            LockString fileStr = StoreAndGetLockString( file, len );
#ifdef TRACY_ON_DEMAND
            pLockItem->lockfile = fileStr.str;
#endif // ifdef TRACY_ON_DEMAND
            time = SendLockObtain( id32, tid );
            SendLockMark( id32, tid, fileStr, line );
        }
        else
        {
            time = Profiler::GetTime();
        }

#ifdef TRACY_ON_DEMAND
        pLockItem->lockThread = tid;
        pLockItem->lockCount.fetch_add( 1 );
        pLockItem->lockTime = time;
        pLockItem->lockline = line;

        LockAssert( time >= GetInitTime() );
        LockAssert( pLockItem->lockThread > 0);
        LockAssert( pLockItem->lockCount.load() > 0);

        const uint32_t index = GetThreadIndex( tid );
        assert( index < pLockItem->waitEvents.size() );
        LockAssert( pLockItem->waitEvents[ index ].time != 0 );
        pLockItem->waitEvents[ index ] = { 0 };
#endif
    }
}


void ProfilerSyncState::LockAcquiredTry( uint32_t connId, uint64_t lockId64, const char *file, int line )
{
    LockAssert( LockTrackingEnabled() );

    LockItem *pLockItem = GetLockItem( lockId64 );
    if ( pLockItem )
    {
        m_Tls.lockId64 = lockId64;
        m_Tls.pLockItem = pLockItem;

        const uint32_t tid = GetThreadHandle();
        const uint32_t id32 = pLockItem->id32;
        int64_t time = 0;

        const bool isConnected = ( connId != 0 );
        pLockItem->active = isConnected;
        LockAssert( !pLockItem->active || (pLockItem->syncConnectionId == connId) );

#ifdef TRACY_ON_DEMAND
        const bool active = pLockItem->active;
#else
        const bool active = true;
#endif

        if ( active )
        {
            // NOTE: Unfortunately we can't use file directly. The string, while static
            // could be in the mapped memory of a dll which, when unloaded will be unmapped.
            // However the tracy server may at any later point in time request the value for
            // this string at which point the memory would be inaccessible. So we must
            // copy the string here...
            // Also, we don't copy it all the time because we don't want to incur additional
            // overhead when no server is connected.
            const size_t len = ( file ? strlen( file ) : 0 );
            LockString fileStr = StoreAndGetLockString( file, len );
#ifdef TRACY_ON_DEMAND
            pLockItem->lockfile = fileStr.str;
#endif // ifdef TRACY_ON_DEMAND

            SendLockWait( id32, tid );
            time = SendLockObtain( id32, tid );
            SendLockMark( id32, tid, fileStr, line );
        }
        else
        {
            time = Profiler::GetTime();
        }

#ifdef TRACY_ON_DEMAND
        pLockItem->lockThread = tid;
        pLockItem->lockCount.fetch_add( 1 );
        pLockItem->lockTime = time;
        pLockItem->lockline = line;
        LockAssert( time >= GetInitTime() );

        LockAssert( pLockItem->lockThread > 0);
        LockAssert( pLockItem->lockCount.load() > 0);
#endif
    }
}


void ProfilerSyncState::LockReleased( uint32_t connId, uint64_t lockId64, const char *file, int line )
{
    LockAssert( LockTrackingEnabled() );

    LockItem *pLockItem = GetLockItem( lockId64 );
    if ( pLockItem )
    {
        const uint32_t id32 = pLockItem->id32;

        m_Tls.lockId64 = 0;
        m_Tls.pLockItem = nullptr;

        const bool isConnected = ( connId != 0 );
        const bool wasActive = isConnected && pLockItem->active;
        LockAssert( !pLockItem->active || (pLockItem->syncConnectionId == connId) );

#ifdef TRACY_ON_DEMAND
        LockAssert( pLockItem->lockThread > 0);
        LockAssert( pLockItem->lockCount.load() > 0);
        pLockItem->lockCount.fetch_sub( 1 );

        const bool active = wasActive;
#else
        const bool active = true;
#endif

        if ( active )
        {
            SendLockRelease( id32 );
        }
    }
}


tracy_force_inline uint32_t ProfilerSyncState::GetThreadIndex( uint32_t tid )
{
    uint32_t index = m_Tls.m_globalThreadIndex;
    if ( index == 0 )
    {
        index = m_globalThreadCount.fetch_add( 1 );
        if ( index < m_indexToTidLut.size() )
        {
            m_Tls.m_globalThreadIndex = index;
            m_indexToTidLut[ index ] = tid;
        }
        else
        {
            m_threadCountOverflow.store( true );
            index = 0;
        }
    }

    return index;
}


tracy_force_inline LockItem* ProfilerSyncState::GetLockItem( uint64_t lockId64 )
{
    LockItem* result = nullptr;
    if ( m_Tls.lockId64 == lockId64 )
    {
        result = m_Tls.pLockItem;
    }
    else
    {
        m_LockItemMutex.lock_shared();
        auto it = m_activeLocks.find( lockId64 );
        if ( it != m_activeLocks.end() )
        {
            result = &it->second;
        }
        m_LockItemMutex.unlock_shared();
    }

    return result;
}


LockString ProfilerSyncState::StoreAndGetLockString( const char *str, size_t len )
{
    LockString result;
    if ( str )
    {
        // Since we will delete the LockItem from the list when we terminate, we need to keep the
        // name stored somewhere. The server can request the name from us at any time and we need
        // a valid pointer for that.
        // Given that we have a potentially huge number of lock objects with a fairly small set of
        // different names, we don't want to just heap alloc the name for every lock object.

        const LockString findStr( str, len );
        {
            m_LockStringMutex.lock_shared();
            auto findIt = m_lockStrings.find( findStr );
            if ( findIt != m_lockStrings.end() )
            {
                result = *findIt;
            }
            m_LockStringMutex.unlock_shared();
        }

        if ( result.hash == 0 )
        {
            m_LockStringMutex.lock();
            auto findIt = m_lockStrings.find( findStr );
            if ( findIt == m_lockStrings.end() )
            {
                char *ptr = ( char * ) tracy_malloc( len + 1 );
                memcpy( ptr, str, len );
                ptr[ len ] = 0;
                auto insertIt = m_lockStrings.emplace( LockString( ptr, len ) );
                result = *insertIt.first;
            }
            else
            {
                result = *findIt;
            }
            m_LockStringMutex.unlock();
        }
    }

    return result;
}


#if defined(TRACY_HAS_BG_SUPPORT)
void ProfilerSyncState::UpdateSyncInfo( QueueType type, QueueItem *item, const Profiler::RefTimes &refTimes )
{
    static SourceLocationData allocSrcLocation =
    {
        "TracySyncZone",
        __func__,
        __FILE__,
        (uint32_t)__LINE__,
        0
    };
    static SourceLocationData* pAllocSrcLocation = &allocSrcLocation ;
    static uint64_t allocSrcLoc = (uint64_t)pAllocSrcLocation;

    switch ( type )
    {
        case QueueType::ZoneValidation:
        {
            SetZoneValidation( refTimes.m_threadCtx, type, item );
        } break;

        case QueueType::ZoneBegin:
        case QueueType::ZoneBeginCallstack:
        {
            MemWrite( &item->zoneBegin.time, refTimes.m_refTimeThread );
            PushThreadZone( refTimes.m_threadCtx, item->hdr.type, item );
        } break;

        case QueueType::ZoneBeginAllocSrcLoc:
        {
            // NOTE: The source location has been deallocated at this point.
            // Rewrite the item type to a non-alloc version and clear the source location pointer
            MemWrite( &item->hdr.idx, QueueType::ZoneBegin );
            MemWrite( &item->zoneBegin.time, refTimes.m_refTimeThread );
            MemWrite( &item->zoneBegin.srcloc, allocSrcLoc );
            PushThreadZone( refTimes.m_threadCtx, item->hdr.type, item );
        } break;

        case QueueType::ZoneBeginAllocSrcLocCallstack:
        {
            // NOTE: The source location has been deallocated at this point.
            // Rewrite the item type to a non-alloc version and clear the source location pointer
            MemWrite( &item->hdr.idx, QueueType::ZoneBeginCallstack );
            MemWrite( &item->zoneBegin.time, refTimes.m_refTimeThread );
            MemWrite( &item->zoneBegin.srcloc, allocSrcLoc );
            PushThreadZone( refTimes.m_threadCtx, item->hdr.type, item );
        } break;

        case QueueType::ZoneEnd:
        {
            MemWrite( &item->zoneEnd.time, refTimes.m_refTimeThread );
            PopThreadZone( refTimes.m_threadCtx, item->hdr.type, item );
        } break;

        case QueueType::GpuZoneBegin:
        case QueueType::GpuZoneBeginCallstack:
        {
            MemWrite( &item->gpuZoneBegin.cpuTime, refTimes.m_refTimeThread );
            PushThreadZone( refTimes.m_threadCtx, item->hdr.type, item );
        } break;

        case QueueType::GpuZoneBeginAllocSrcLoc:
        {
            // NOTE: The source location has been deallocated at this point.
            // Rewrite the item type to a non-alloc version and clear the source location pointer
            MemWrite( &item->hdr.idx, QueueType::GpuZoneBegin );
            MemWrite( &item->gpuZoneBegin.cpuTime, refTimes.m_refTimeThread );
            MemWrite( &item->gpuZoneBegin.srcloc, allocSrcLoc );
            PushThreadZone( refTimes.m_threadCtx, item->hdr.type, item );
        } break;

        case QueueType::GpuZoneBeginAllocSrcLocCallstack:
        {
            // NOTE: The source location has been deallocated at this point.
            // Rewrite the item type to a non-alloc version and clear the source location pointer
            MemWrite( &item->hdr.idx, QueueType::GpuZoneBeginCallstack );
            MemWrite( &item->gpuZoneBegin.cpuTime, refTimes.m_refTimeThread );
            MemWrite( &item->gpuZoneBegin.srcloc, allocSrcLoc );
            PushThreadZone( refTimes.m_threadCtx, item->hdr.type, item );
        } break;

        case QueueType::GpuZoneEnd:
        {
            MemWrite( &item->gpuZoneEnd.cpuTime, refTimes.m_refTimeThread );
            PopThreadZone( refTimes.m_threadCtx, item->hdr.type, item );
        } break;

        case QueueType::ZoneText:
        case QueueType::ZoneName:
        case QueueType::ZoneColor:
        case QueueType::ZoneValue:
        {
            ClearZoneValidation( refTimes.m_threadCtx );
        } break;

        case QueueType::GpuZoneBeginSerial:
        case QueueType::GpuZoneBeginCallstackSerial:
        {
            MemWrite( &item->gpuZoneBegin.cpuTime, refTimes.m_refTimeSerial );
            PushContextZone( item->gpuZoneBegin.context, item->hdr.type, item );
        } break;

        case QueueType::GpuZoneBeginAllocSrcLocSerial:
        {
            // NOTE: The source location has been deallocated at this point.
            // Rewrite the item type to a non-alloc version and clear the source location pointer
            MemWrite( &item->hdr.idx, QueueType::GpuZoneBeginSerial );
            MemWrite( &item->gpuZoneBegin.cpuTime, refTimes.m_refTimeSerial );
            MemWrite( &item->gpuZoneBegin.srcloc, allocSrcLoc );
            PushContextZone( item->gpuZoneBegin.context, item->hdr.type, item );
        } break;

        case QueueType::GpuZoneBeginAllocSrcLocCallstackSerial:
        {
            // NOTE: The source location has been deallocated at this point.
            // Rewrite the item type to a non-alloc version and clear the source location pointer
            MemWrite( &item->hdr.idx, QueueType::GpuZoneBeginCallstackSerial );
            MemWrite( &item->gpuZoneBegin.cpuTime, refTimes.m_refTimeSerial );
            MemWrite( &item->gpuZoneBegin.srcloc, allocSrcLoc );
            PushContextZone( item->gpuZoneBegin.context, item->hdr.type, item );
        } break;

        case QueueType::GpuZoneEndSerial:
        {
            MemWrite( &item->gpuZoneEnd.cpuTime, refTimes.m_refTimeSerial );
            PopContextZone( item->gpuZoneEnd.context, item->hdr.type, item );
        } break;

        case QueueType::FrameMarkMsgStart:
        {
            PushFrame( type, item );
        } break;

        case QueueType::FrameMarkMsgEnd:
        {
            PopFrame( type, item );
        } break;

        default:
        {
        } break;
    }
}


bool ProfilerSyncState::Synchronize( Profiler& p, const Profiler::RefTimes& syncRefTimes, Profiler::RefTimes& refTimes, uint32_t connId )
{
    bool result = true;

#ifdef TRACY_ON_DEMAND
#define AppendItem( item )                                              \
    if ( !p.AppendData( &(item), QueueDataSize[ (item).hdr.idx ] ) ) {  \
        result = false;                                                 \
        goto done;                                                      \
    }

    if ( HasSyncMode( ProfilerSyncState::Zones ) )
    {
        if ( !SynchronizeZones( p, syncRefTimes, refTimes, p.ConnectionId() ) )
        {
            result = false;
            goto done;
        }

        if ( !SynchronizeContexts( p, syncRefTimes, refTimes, p.ConnectionId() ) )
        {
            result = false;
            goto done;
        }

        if ( !SynchronizeFrames( p, refTimes, p.ConnectionId() ) )
        {
            result = false;
            goto done;
        }

        {
            QueueItem syncValidation;
            MemWrite( &syncValidation.hdr.type, QueueType::SyncValidation );
            uint8_t flags = ( QueueSyncValidation::ThreadCtx | QueueSyncValidation::TimeThread | QueueSyncValidation::TimeSerial );
            MemWrite( &syncValidation.syncValidation.flags, flags );
            MemWrite( &syncValidation.syncValidation.threadCtx, syncRefTimes.m_threadCtx );
            MemWrite( &syncValidation.syncValidation.refTimeThread, syncRefTimes.m_refTimeThread );
            MemWrite( &syncValidation.syncValidation.refTimeSerial, syncRefTimes.m_refTimeSerial );
            MemWrite( &syncValidation.syncValidation.refTimeCtx, 0 );
            AppendItem( syncValidation );
        }
    }

done:

#undef AppendItem
#endif // ifdef TRACY_ON_DEMAND

    return result;
}


bool ProfilerSyncState::SynchronizeZones( Profiler& p, const Profiler::RefTimes& syncRefTimes, Profiler::RefTimes& refTimes, uint32_t connId )
{
    bool result = true;

#ifdef TRACY_ON_DEMAND
#define AppendItem( item )                                              \
    if ( !p.AppendData( &(item), QueueDataSize[ (item).hdr.idx ] ) ) {  \
        result = false;                                                 \
        goto done;                                                      \
    }

    static SourceLocationData syncSrcLocation =
    {
        "TracySyncZone",
        __func__,
        __FILE__,
        (uint32_t)__LINE__,
        0
    };
    static SourceLocationData* pSyncSrcLocation = &syncSrcLocation;
    static uint64_t syncSrcLoc = (uint64_t)pSyncSrcLocation;


    if ( !m_threadZoneStack.empty() )
    {
        const ZoneInfo* curThreadStack = nullptr;
        FastVector< const ZoneInfo*> cpuTimeSync( m_threadZoneStack.size() );

        for ( const ZoneStackMap::value_type& entry : m_threadZoneStack )
        {
            uint32_t threadId = entry.first;
            const ZoneInfo &info = entry.second;

            // If this stack is for the thread that was active when we called into synchronize, we just save it and don't process it ATM.
            // In order for all data to match up we must be sure that the currect active thread will be the active one at the end.
            // However we also must take special care about the ref time, because switching thread causes the reftime in the worker to be
            // reset to 0!
            if ( syncRefTimes.m_threadCtx == threadId )
            {
                curThreadStack = &info;
            }
            else
            {
                const ZoneInfo **pNext = cpuTimeSync.push_next();
                *pNext = &info;
            }
        }

        if ( curThreadStack )
        {
            const ZoneInfo **pNext = cpuTimeSync.push_next();
            *pNext = curThreadStack;
        }

        for ( const ZoneInfo *pInfo : cpuTimeSync )
        {
            uint32_t threadId = pInfo->id;
            const TracyVector< ZoneStackItem > &stack = pInfo->stack;

            // This resets the thread ref time to 0!!
            {
                QueueItem item;
                MemWrite( &item.hdr.type, QueueType::ThreadContext );
                MemWrite( &item.threadCtx.thread, threadId );
                refTimes.m_threadCtx = threadId;
                refTimes.m_refTimeThread = 0;
                AppendItem( item );
            }

            for ( TracyVector< ZoneStackItem >::const_iterator it = stack.begin(), end = stack.end(); it != end; ++it )
            {
                const ZoneStackItem &stackItem = *it;
                QueueItem copy = stackItem.item;
                QueueItem *item = &copy;

#ifndef TRACY_NO_VERIFY
                if ( stackItem.validationId )
                {
                    QueueItem zoneValid;
                    MemWrite( &zoneValid.hdr.type, QueueType::ZoneValidation );
                    MemWrite( &zoneValid.zoneValidation.id, stackItem.validationId );
                    AppendItem( zoneValid );
                }
#endif // ifndef TRACY_NO_VERIFY

                switch ( item->hdr.type )
                {
                    case QueueType::ZoneBegin:
                    {
                        int64_t t = MemRead<int64_t>( &item->zoneBegin.time );
                        int64_t dt = t - refTimes.m_refTimeThread;
                        refTimes.m_refTimeThread = t;
                        MemWrite( &item->zoneBegin.time, dt );
                    } break;

                    case QueueType::ZoneEnd:
                    {
                        int64_t t = MemRead<int64_t>( &item->zoneEnd.time );
                        int64_t dt = t - refTimes.m_refTimeThread;
                        refTimes.m_refTimeThread = t;
                        MemWrite( &item->zoneEnd.time, dt );
                    } break;

                    case QueueType::GpuZoneBegin:
                    {
                        int64_t t = MemRead<int64_t>( &item->gpuZoneBegin.cpuTime );
                        int64_t dt = t - refTimes.m_refTimeThread;
                        refTimes.m_refTimeThread = t;
                        MemWrite( &item->gpuZoneBegin.cpuTime, dt );
                    } break;

                    case QueueType::GpuZoneEnd:
                    {
                        int64_t t = MemRead<int64_t>( &item->gpuZoneEnd.cpuTime );
                        int64_t dt = t - refTimes.m_refTimeThread;
                        refTimes.m_refTimeThread = t;
                        MemWrite( &item->gpuZoneEnd.cpuTime, dt );
                    } break;

                    default:
                    {
                        // unexpected event found
                        assert( false );
                    } break;
                }

                AppendItem( *item );
            }

            if ( ( syncRefTimes.m_threadCtx == threadId ) && ( refTimes.m_refTimeThread != syncRefTimes.m_refTimeThread ) )
            {
                if ( syncRefTimes.m_refTimeThread != 0 )
                {
                    // NOTE: if the thread time is not zero and we are not there yet, inject a time sync zone
                    QueueItem zoneValid;
                    MemWrite( &zoneValid.hdr.type, QueueType::ZoneValidation );
                    MemWrite( &zoneValid.zoneValidation.id, 0xdeafbeef );

                    // NOTE(sev): make sure m_refTimeThread is correctly synced for the current thread.
                    // We could still be out of sync if we have had a zone enter/exit on that thread which would not
                    // have been on the stack, so we wouldn't have adjusted the time
                    {
                        AppendItem( zoneValid );

                        int64_t t = syncRefTimes.m_refTimeThread;
                        int64_t dt = t - refTimes.m_refTimeThread;
                        refTimes.m_refTimeThread = t;

                        QueueItem syncItem;
                        MemWrite( &syncItem.hdr.type, QueueType::ZoneBegin );
                        MemWrite( &syncItem.zoneBegin.time, dt );
                        MemWrite( &syncItem.zoneBegin.srcloc, syncSrcLoc );
                        AppendItem( syncItem );
                    }

                    {
                        AppendItem( zoneValid );

                        int64_t t = syncRefTimes.m_refTimeThread;
                        int64_t dt = t - refTimes.m_refTimeThread;
                        refTimes.m_refTimeThread = t;

                        QueueItem syncItem;
                        MemWrite( &syncItem.hdr.type, QueueType::ZoneEnd );
                        MemWrite( &syncItem.zoneEnd.time, dt );
                        AppendItem( syncItem );
                    }
                }
                else
                {
                    // NOTE: if the thread time is zero, do nothing, because tracy does not handle a zone at zero well
                    refTimes.m_refTimeThread = 0;
                }
            }

#ifndef TRACY_NO_VERIFY
            if ( pInfo->validationId )
            {
                QueueItem zoneValid;
                MemWrite( &zoneValid.hdr.type, QueueType::ZoneValidation );
                MemWrite( &zoneValid.zoneValidation.id, pInfo->validationId );
                AppendItem( zoneValid );
            }
#endif // ifndef TRACY_NO_VERIFY

            if ( !p.CommitPendingData() )
            {
                result = false;
                goto done;
            }
        }
    }

    // NOTE: we *must* force a ThreadContext here. We could have have thread context switches that didn't generate any
    // information on the sync stack, but we must make sure our m_refTimeThread is correct.
    if ( (refTimes.m_threadCtx != syncRefTimes.m_threadCtx) || (syncRefTimes.m_refTimeThread == 0 ) )
    {
        QueueItem item;
        MemWrite( &item.hdr.type, QueueType::ThreadContext );
        MemWrite( &item.threadCtx.thread, syncRefTimes.m_threadCtx );
        refTimes.m_threadCtx = syncRefTimes.m_threadCtx;
        refTimes.m_refTimeThread = 0;
        AppendItem( item );
    }

    {
        for ( const ZoneStackMap::value_type& entry : m_threadZoneStack )
        {
            uint32_t threadId = entry.first;
            const ZoneInfo &info = entry.second;
            const TracyVector< ZoneStackItem > &stack = info.stack;
            QueueItem syncValidationThread;
            MemWrite( &syncValidationThread.hdr.type, QueueType::SyncValidationThread );
            MemWrite( &syncValidationThread.syncValidationThread.threadId, threadId );
            MemWrite( &syncValidationThread.syncValidationThread.stackDepth, (uint32_t)stack.size() );
            MemWrite( &syncValidationThread.syncValidationThread.validationId, info.validationId );
            AppendItem( syncValidationThread );
        }
    }

done:

#undef AppendItem
#endif // TRACY_ON_DEMAND

    return result;
}


bool ProfilerSyncState::SynchronizeContexts( Profiler& p, const Profiler::RefTimes& syncRefTimes, Profiler::RefTimes& refTimes, uint32_t connId )
{
    bool result = true;

#ifdef TRACY_ON_DEMAND
#define AppendItem( item )                                              \
    if ( !p.AppendData( &(item), QueueDataSize[ (item).hdr.idx ] ) ) {  \
        result = false;                                                 \
        goto done;                                                      \
    }

    static SourceLocationData syncSrcLocation =
    {
        "TracySyncContext",
        __func__,
        __FILE__,
        (uint32_t)__LINE__,
        0
    };
    static SourceLocationData* pSyncSrcLocation = &syncSrcLocation;
    static uint64_t syncSrcLoc = (uint64_t)pSyncSrcLocation;

    // Sync gpu zones
    if ( !m_contextZoneStack.empty() )
    {
        uint32_t nullThreadId = 0;
        uint16_t syncQueryId = 0;

        struct GpuSyncContextQuery
        {
            uint16_t queryId;
            uint8_t context;
        };

        FastVector<GpuSyncContextQuery> gpuTimeSync( m_contextZoneStack.size() * 2 + 2 );

        for ( const ZoneStackMap::value_type& entry : m_contextZoneStack )
        {
            const ZoneInfo &zoneInfo = entry.second;
            const TracyVector< ZoneStackItem > &stack = zoneInfo.stack;

            for ( TracyVector< ZoneStackItem >::const_iterator it = stack.begin(), end = stack.end(); it != end; ++it )
            {
                const ZoneStackItem &stackItem = *it;
                QueueItem copy = stackItem.item;
                QueueItem *item = &copy;

                switch ( item->hdr.type )
                {
                    case QueueType::GpuZoneBeginSerial:
                    case QueueType::GpuZoneBeginCallstackSerial:
                    {
                        int64_t t = MemRead<int64_t>( &item->gpuZoneBegin.cpuTime );
                        int64_t dt = t - refTimes.m_refTimeSerial;
                        refTimes.m_refTimeSerial = t;
                        MemWrite( &item->gpuZoneBegin.cpuTime, dt );
                        MemWrite( &item->gpuZoneBegin.queryId, syncQueryId++ );

                        GpuSyncContextQuery *q = gpuTimeSync.push_next();
                        q->queryId = item->gpuZoneBegin.queryId;
                        q->context = item->gpuZoneBegin.context;
                    } break;

                    case QueueType::GpuZoneEndSerial:
                    {
                        int64_t t = MemRead<int64_t>( &item->gpuZoneEnd.cpuTime );
                        int64_t dt = t - refTimes.m_refTimeSerial;
                        refTimes.m_refTimeSerial = t;
                        MemWrite( &item->gpuZoneEnd.cpuTime, dt );
                    } break;

                    default:
                    {
                        // unexpected event found
                        assert( false );
                    } break;
                }

                AppendItem( *item );
            }
        }

        if ( (syncRefTimes.m_refTimeSerial != 0) && (syncRefTimes.m_refTimeSerial != refTimes.m_refTimeSerial ))
        {
            uint8_t contextId = ( uint8_t ) m_contextZoneStack.begin()->first;

            {
                int64_t t = syncRefTimes.m_refTimeSerial;
                int64_t dt = t - refTimes.m_refTimeSerial;
                refTimes.m_refTimeSerial = t;

                QueueItem syncItem;
                MemWrite( &syncItem.hdr.type, QueueType::GpuZoneBeginSerial );
                MemWrite( &syncItem.gpuZoneBegin.cpuTime, dt );
                MemWrite( &syncItem.gpuZoneBegin.thread, nullThreadId );
                MemWrite( &syncItem.gpuZoneBegin.queryId, syncQueryId++ );
                MemWrite( &syncItem.gpuZoneBegin.context, contextId );
                MemWrite( &syncItem.gpuZoneBegin.srcloc, syncSrcLoc );

                GpuSyncContextQuery *q = gpuTimeSync.push_next();
                q->queryId = syncItem.gpuZoneBegin.queryId;
                q->context = syncItem.gpuZoneBegin.context;

                AppendItem( syncItem );
            }

            {
                int64_t t = syncRefTimes.m_refTimeSerial;
                int64_t dt = t - refTimes.m_refTimeSerial;
                refTimes.m_refTimeSerial = t;

                QueueItem syncItem;
                MemWrite( &syncItem.hdr.type, QueueType::GpuZoneEndSerial );
                MemWrite( &syncItem.gpuZoneEnd.cpuTime, dt );
                MemWrite( &syncItem.gpuZoneEnd.thread, nullThreadId );
                MemWrite( &syncItem.gpuZoneEnd.queryId, syncQueryId++ );
                MemWrite( &syncItem.gpuZoneEnd.context, contextId );

                GpuSyncContextQuery *q = gpuTimeSync.push_next();
                q->queryId = syncItem.gpuZoneEnd.queryId;
                q->context = syncItem.gpuZoneEnd.context;

                AppendItem( syncItem );
            }
        }

        {
            int64_t t = syncRefTimes.m_refTimeGpu;

            QueueItem gpuSync;
            MemWrite( &gpuSync.hdr.type, QueueType::GpuTime);

            for (size_t index = 0; index < gpuTimeSync.size(); index++)
            {
                GpuSyncContextQuery& sync = gpuTimeSync[index];
                int64_t dt = t - refTimes.m_refTimeGpu;
                refTimes.m_refTimeGpu = t;

                MemWrite( &gpuSync.gpuTime.gpuTime, dt );
                MemWrite( &gpuSync.gpuTime.queryId, sync.queryId);
                MemWrite( &gpuSync.gpuTime.context, sync.context);

                AppendItem( gpuSync );
            }

            if ( !p.CommitPendingData() )
            {
                result = false;
                goto done;
            }
        }

        for ( const ZoneStackMap::value_type& entry : m_contextZoneStack )
        {
            uint8_t contextId = (uint8_t)entry.first;
            uint32_t nullThreadId = 0;
            const ZoneInfo &info = entry.second;
            const TracyVector< ZoneStackItem > &stack = info.stack;
            QueueItem syncValidationContext;
            MemWrite( &syncValidationContext.hdr.type, QueueType::SyncValidationContext );
            MemWrite( &syncValidationContext.syncValidationContext.threadId, nullThreadId );
            MemWrite( &syncValidationContext.syncValidationContext.context, contextId );
            MemWrite( &syncValidationContext.syncValidationContext.stackDepth, (uint32_t)stack.size() );
            AppendItem( syncValidationContext );
        }
    }

done:

#undef AppendItem
#endif // TRACY_ON_DEMAND

    return result;
}


bool ProfilerSyncState::SynchronizeFrames( Profiler& p, Profiler::RefTimes& refTimes, uint32_t connId )
{
    bool result = true;

#ifdef TRACY_ON_DEMAND
#define AppendItem( item )                                              \
    if ( !p.AppendData( &(item), QueueDataSize[ (item).hdr.idx ] ) ) {  \
        result = false;                                                 \
        goto done;                                                      \
    }

    for ( const FrameMap::value_type& entry : m_frames )
    {
        const QueueItem &frame = entry.second;
        AppendItem( frame);
    }

done:

#undef AppendItem
#endif // TRACY_ON_DEMAND

    return result;
}
#endif // if defined(TRACY_HAS_BG_SUPPORT)


Profiler::Profiler()
    : m_timeBegin( 0 )
    , m_listenAndBroadcastRequested( false )
    , m_mainThread( detail::GetThreadHandleImpl() )
    , m_epoch( std::chrono::duration_cast<std::chrono::seconds>( std::chrono::system_clock::now().time_since_epoch() ).count() )
    , m_shutdown( false )
    , m_shutdownManual( false )
    , m_shutdownFinished( false )
    , m_pSyncState( nullptr )
    , m_pUiConnection( nullptr )
    , m_pBufferHandler( nullptr )
    , m_broadcaster()
    , m_noExit( false )
    , m_userPort( 0 )
    , m_zoneId( 1 )
    , m_samplingPeriod( 0 )
    , m_pExtWorker( nullptr )
    , m_serialQueue( 1024*1024 )
    , m_serialDequeue( 1024*1024 )
#ifndef TRACY_NO_FRAME_IMAGE
    , m_fiQueue( 16 )
    , m_fiDequeue( 16 )
#endif
    , m_symbolQueue( 10*1024 )
    , m_symbolDequeue( 10*1024 )
#if defined(TRACY_NEEDS_SYMBOL_WORKER)
    , m_requestSymbolLock()
    , m_symbolRequestQueue( 10*1024 )
    , m_cancelSymbolProcessing( false )
#endif
#ifdef TRACY_HAS_SYSTEM_TRACING
    , m_sysQueue( 1*1024 )
    , m_sysDequeue( 1*1024 )
#endif // ifdef TRACY_HAS_SYSTEM_TRACING
    , m_frameCount( 0 )
    , m_frameTime( 0 )
    , m_options( Options_None )
    , m_connectionId( 0 )
    , m_nextConnectionId( 1 )
    , m_listenPort( 0 )
#ifdef TRACY_ON_DEMAND
    , m_deferredQueue( 64*1024 )
#endif
    , m_paramCallback( nullptr )
    , m_sourceCallback( nullptr )
#if defined(TRACY_NEEDS_SYMBOL_WORKER)
    , m_queryImage( nullptr )
    , m_queryData( nullptr )
#endif
    , m_crashHandlerInstalled( false )
    , m_programName( nullptr )
{
    assert( !s_instance );
    s_instance = this;

    memset( &m_refTimes, 0, sizeof( m_refTimes ) );
    SetProgramName( GetProcessName() );

    memset( &m_broadcaster, 0, sizeof(m_broadcaster) );
    m_pSyncState = new( tracy_malloc( sizeof( *m_pSyncState ) ) ) ProfilerSyncState();

#ifdef TRACY_HAS_SYSTEM_TRACING
    if ( TracyHasCommandLineOption( "-tracy_cs" ) )
    {
        m_options |= Options_ContextSwitches;
    }

    if ( TracyHasCommandLineOption( "-tracy_sampling" ) )
    {
        m_options |= Options_Sampling;
    }

    if ( TracyHasCommandLineOption( "-tracy_hw_sampling" ) )
    {
        m_options |= Options_HardwareSampling;
        // TODO added context switches and sampling for now so that hardware counters
        // are usable in Tracy.
        m_options |= Options_ContextSwitches;
        m_options |= Options_Sampling;
    }

    if ( TracyHasCommandLineOption( "-tracy_hw_events" ) )
    {
        m_options |= Options_HardwareEvents;
        m_options |= Options_ContextSwitches;
    }

    if ( TracyHasCommandLineOption( "-tracy_vsync" ) )
    {
        m_options |= Options_Vsync;
    }

    if ( TracyHasCommandLineOption( "-tracy_sys" ) )
    {
        m_options |= Options_ContextSwitches;
        m_options |= Options_Sampling;
    }
#endif // ifdef TRACY_HAS_SYSTEM_TRACING

#if defined(TRACY_HAS_BG_SUPPORT)
    if ( TracyHasCommandLineOption( "-tracy_background" ) || TracyHasCommandLineOption( "-tracy_auto_spike_dump" ) )
    {
        m_options |= Options_Background;
    }
#endif // if defined(TRACY_HAS_BG_SUPPORT)

#if defined(TRACY_LOCK_SUPPORT_ENABLED)
    // TODO(sev): we enable lock syncing while we still have locks hidden behind a compile time switch
    //if ( TracyHasCommandLineOption( "-tracy_locks" ) )
    {
        m_pSyncState->SetSyncModeEnabled( ProfilerSyncState::Locks, true );
    }
#endif // if defined(TRACY_LOCK_SUPPORT_ENABLED)

#ifndef TRACY_DELAYED_INIT
#  ifdef _MSC_VER
    // 3. But these variables need to be initialized in main thread within the .CRT$XCB section. Do it here.
    s_token_detail = moodycamel::ProducerToken( s_queue, detail::GetThreadHandleImpl() );
    s_token = ProducerWrapper { s_queue.get_explicit_producer( s_token_detail ) };
    s_threadHandle = ThreadHandleWrapper { m_mainThread };
#  endif
#endif

    CalibrateTimer();
    CalibrateDelay();
    ReportTopology();

#ifdef __linux__
    m_kcore = (KCore*)tracy_malloc( sizeof( KCore ) );
    new(m_kcore) KCore();
#endif

#ifndef TRACY_NO_EXIT
    const char* noExitEnv = GetEnvVar( "TRACY_NO_EXIT" );
    if( noExitEnv && noExitEnv[0] == '1' )
    {
        m_noExit = true;
    }
#endif

    const char* userPort = GetEnvVar( "TRACY_PORT" );
    if( userPort )
    {
        m_userPort = atoi( userPort );
    }

    m_safeSendBuffer = (char*)tracy_malloc( SafeSendBufferSize );

#if !defined(TRACY_HAS_BG_SUPPORT) && (!defined(TRACY_DELAYED_INIT) || !defined(TRACY_MANUAL_LIFETIME))

    DbgHelpLoaderFunc *pLoader = nullptr;
#if defined( _WIN32 ) && defined( TRACY_HAS_CALLSTACK ) && defined( TRACY_HAS_CUSTOM_DBG_HELP_LOADER )
    extern void *TracyCustomDbgHelpLoader();
    pLoader = &TracyCustomDbgHelpLoader;
#endif

    SpawnWorkerThreads( nullptr, pLoader );
#endif
}

Profiler::~Profiler()
{
    m_shutdown.store( true, std::memory_order_relaxed );

    RemoveCrashHandler();

    StopWorkerThreads();

#ifdef __linux__
    m_kcore->~KCore();
    tracy_free( m_kcore );
#endif

    if ( m_broadcaster.broadcast )
    {
        m_broadcaster.broadcast->~UdpBroadcast();
        tracy_free( m_broadcaster.broadcast );
        memset(&m_broadcaster, 0, sizeof(m_broadcaster));
    }

    UiConnection::Destroy( m_pUiConnection );

    if ( m_pSyncState )
    {
        m_pSyncState->~ProfilerSyncState();
        tracy_free( m_pSyncState );
        m_pSyncState = nullptr;
    }

#ifndef _WIN32
    close( m_pipe[0] );
    close( m_pipe[1] );
#endif
    tracy_free( m_safeSendBuffer );

    assert( s_instance );
    s_instance = nullptr;
}


bool Profiler::ShouldExit()
{
    return s_instance->m_shutdown.load( std::memory_order_relaxed );
}


uint32_t Profiler::GetOptions() const
{
    return m_options;
}


void Profiler::ClearOptions( uint32_t clearOpts )
{
    uint32_t clearMask = ~clearOpts;
    m_options &= clearMask;
}


void Profiler::InstallCrashHandler()
{

#if defined __linux__ && !defined TRACY_NO_CRASH_HANDLER
    struct sigaction threadFreezer = {};
    threadFreezer.sa_handler = ThreadFreezer;
    sigaction( TRACY_CRASH_SIGNAL, &threadFreezer, &m_prevSignal.pwr );

    struct sigaction crashHandler = {};
    crashHandler.sa_sigaction = CrashHandler;
    crashHandler.sa_flags = SA_SIGINFO;
    sigaction( SIGILL, &crashHandler, &m_prevSignal.ill );
    sigaction( SIGFPE, &crashHandler, &m_prevSignal.fpe );
    sigaction( SIGSEGV, &crashHandler, &m_prevSignal.segv );
    sigaction( SIGPIPE, &crashHandler, &m_prevSignal.pipe );
    sigaction( SIGBUS, &crashHandler, &m_prevSignal.bus );
    sigaction( SIGABRT, &crashHandler, &m_prevSignal.abrt );
#endif

#if defined _WIN32 && !defined TRACY_UWP && !defined TRACY_NO_CRASH_HANDLER
    // We cannot use Vectored Exception handling because it catches application-wide frame-based SEH blocks. We only
    // want to catch unhandled exceptions.
    m_prevHandler = SetUnhandledExceptionFilter( CrashFilter );
#endif

#ifndef TRACY_NO_CRASH_HANDLER
    m_crashHandlerInstalled = true;
#endif

}

void Profiler::RemoveCrashHandler()
{
#if defined _WIN32 && !defined TRACY_UWP && !defined TRACY_NO_CRASH_HANDLER
    if( m_crashHandlerInstalled )
    {
        auto prev = SetUnhandledExceptionFilter( (LPTOP_LEVEL_EXCEPTION_FILTER)m_prevHandler );
        if( prev != CrashFilter ) SetUnhandledExceptionFilter( prev ); // A different exception filter was installed over ours => put it back
    }
#endif

#if defined __linux__ && !defined TRACY_NO_CRASH_HANDLER
    if( m_crashHandlerInstalled )
    {
        auto restore = []( int signum, struct sigaction* prev ) {
            struct sigaction old;
            sigaction( signum, prev, &old );
            if( old.sa_sigaction != CrashHandler ) sigaction( signum, &old, nullptr ); // A different signal handler was installed over ours => put it back
        };
        restore( TRACY_CRASH_SIGNAL, &m_prevSignal.pwr );
        restore( SIGILL, &m_prevSignal.ill );
        restore( SIGFPE, &m_prevSignal.fpe );
        restore( SIGSEGV, &m_prevSignal.segv );
        restore( SIGPIPE, &m_prevSignal.pipe );
        restore( SIGBUS, &m_prevSignal.bus );
        restore( SIGABRT, &m_prevSignal.abrt );
    }
#endif
    m_crashHandlerInstalled = false;
}

void Profiler::SpawnWorkerThreads( IWorker *pExtWorker, DbgHelpLoaderFunc* pDbgHelpLoader )
{
#ifdef TRACY_HAS_CALLSTACK
    InitCallstackCritical();
#endif

    m_pExtWorker = pExtWorker;
    bool needsSymbolWorker = false;
    static_cast<void>(needsSymbolWorker); // potentially unused

#ifdef TRACY_HAS_SYSTEM_TRACING
    const char* noSysTrace = GetEnvVar( "TRACY_NO_SYS_TRACE" );
    const bool disableSystrace = (noSysTrace && noSysTrace[0] == '1');
    if( disableSystrace )
    {
        TracyDebug("TRACY: Sys Trace was disabled by 'TRACY_NO_SYS_TRACE=1'\n");
    }
    else if( SysTraceStart( m_samplingPeriod ) )
    {
        needsSymbolWorker = true;
        assert( s_sysTraceThread == nullptr );
        s_sysTraceThread = (Thread*)tracy_malloc( sizeof( Thread ) );
        new(s_sysTraceThread) Thread( SysTraceWorker, nullptr );
        std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }
#endif

    assert( s_thread == nullptr );
    s_thread = (Thread*)tracy_malloc( sizeof( Thread ) );
    new(s_thread) Thread( LaunchWorker, this );
#if defined _WIN32 && !defined TRACY_UWP && !defined TRACY_NO_CRASH_HANDLER
    s_profilerThreadId = GetThreadId( s_thread->Handle() );
#endif

#ifndef TRACY_NO_FRAME_IMAGE
    assert( s_compressThread == nullptr );
    s_compressThread = (Thread*)tracy_malloc( sizeof( Thread ) );
    new(s_compressThread) Thread( LaunchCompressWorker, this );
#endif

#ifdef TRACY_NEEDS_SYMBOL_WORKER
    if ( needsSymbolWorker )
    {
        s_pDbgHelpLoader = pDbgHelpLoader;
        assert( s_symbolThread == nullptr );
        s_symbolThread = (Thread*)tracy_malloc( sizeof( Thread ) );
        new(s_symbolThread) Thread( LaunchSymbolWorker, this );

#       if defined _WIN32 && !defined TRACY_UWP && !defined TRACY_NO_CRASH_HANDLER
        s_symbolThreadId = GetThreadId( pSymbolThread->Handle() );
#       endif
    }
#endif

    m_timeBegin.store( GetTime(), std::memory_order_relaxed );
}

void Profiler::StopWorkerThreads()
{
    RequestShutdown();
    RemoveCrashHandler();

    if ( s_thread )
    {
        s_thread->~Thread();
        tracy_free( s_thread );
        s_thread = nullptr;
    }

#ifdef TRACY_HAS_SYSTEM_TRACING
    if( s_sysTraceThread )
    {
        SysTraceStop();
        s_sysTraceThread->~Thread();
        tracy_free( s_sysTraceThread );
        s_sysTraceThread = nullptr;
    }
#endif

#ifdef TRACY_NEEDS_SYMBOL_WORKER
    {
        if ( s_symbolThread )
        {
            s_symbolThread->~Thread();
            tracy_free( s_symbolThread );
            s_symbolThread = nullptr;
        }
    }
#endif

#ifndef TRACY_NO_FRAME_IMAGE
    if ( s_compressThread )
    {
        s_compressThread->~Thread();
        tracy_free( s_compressThread );
        s_compressThread = nullptr;
    }
#endif

    SetBufferHandler( nullptr );
    m_pExtWorker = nullptr;
}


void Profiler::DumpBegin()
{
    ClearSymbol();
}


void Profiler::DumpEnd()
{
    ClearSymbol();
}


Profiler::IBufferHandler* Profiler::SetBufferHandler( IBufferHandler *pHandler )
{
    IBufferHandler* pPrev = m_pBufferHandler;
    if ( pPrev )
    {
        CommitPendingData();
    }

    m_pBufferHandler = pHandler;
    return pPrev;
}


bool Profiler::Synchronize( uint32_t syncMode )
{
    bool result = true;

#ifdef TRACY_ON_DEMAND
    assert( m_pSyncState != nullptr );
    m_pSyncState->SyncBegin();
    m_deferredLock.lock();

    RefTimes refTimes = {0};

#define AppendItem( item )                                              \
    if ( !AppendData( &(item), QueueDataSize[ (item).hdr.idx ] ) ) {    \
        result = false;                                                 \
    }

    // Start the sync
    if ( result )
    {
        QueueItem syncBegin;
        MemWrite( &syncBegin.hdr.type, QueueType::ConnectionSyncBegin );
        MemWrite( &syncBegin.connectionSyncBegin.time, GetTime() );
        AppendItem( syncBegin );
    }

    if ( result && ( syncMode & SyncMode_Tracy ) )
    {
        SynchronizeTracyNoLock( m_refTimes, refTimes );
    }

    if ( result && ( syncMode & SyncMode_Sys ) )
    {
        SynchronizeSysNoLock( m_refTimes, refTimes );
    }

    // Finish the sync
    if ( result && ( syncMode == ( SyncMode_Tracy | SyncMode_Sys ) ) )
    {
        QueueItem syncValidation;
        MemWrite( &syncValidation.hdr.type, QueueType::SyncValidation );
        MemWrite( &syncValidation.syncValidation.threadCtx, m_refTimes.m_threadCtx );
        MemWrite( &syncValidation.syncValidation.refTimeThread, m_refTimes.m_refTimeThread );
        MemWrite( &syncValidation.syncValidation.refTimeSerial, m_refTimes.m_refTimeSerial );
        MemWrite( &syncValidation.syncValidation.refTimeCtx, m_refTimes.m_refTimeCtx );
        AppendItem( syncValidation );
    }

    if ( result )
    {
        QueueItem syncEnd;
        MemWrite( &syncEnd.hdr.type, QueueType::ConnectionSyncEnd );
        MemWrite( &syncEnd.connectionSyncEnd.time, GetTime() );
        AppendItem( syncEnd );
    }

    m_deferredLock.unlock();
    m_pSyncState->SyncEnd();

#undef AppendItem

#endif // ifdef TRACY_ON_DEMAND

    return result;
}

bool Profiler::ProcessServerQueries( const ServerQueryPacket *pQueries, size_t count )
{
    bool result = true;
    if ( pQueries )
    {
        for ( size_t queryIndex = 0; result && !ShouldExit() && ( queryIndex < count ); queryIndex++ )
        {
            result = result && HandleServerQuery( pQueries[ queryIndex ] );
        }

        result = result && CommitPendingData();
    }

    return result;
}


void Profiler::PreConnect( const char *broadcastMsg )
{
    assert( !IsConnected() );

    if ( m_pUiConnection )
    {
        m_pUiConnection->ResetBuffer();
    }

    ClearConnectionData();

    m_broadcaster.broadcastMsg.flags |= BroadcastFlags_DenyConnection;
    SetBroadcastMessage( broadcastMsg );
    UpdateBroadcast( true );
}


void Profiler::PostConnect()
{
    assert( !IsConnected() );
    SetBufferHandler( nullptr );
    UiConnection::Destroy( m_pUiConnection );
    ClearConnectionData();

    SetBroadcastMessage( nullptr );
    m_broadcaster.broadcastMsg.flags &= ~BroadcastFlags_DenyConnection;
    UpdateBroadcast( true );
}


void Profiler::ConnectionUpdate()
{
    UpdateBroadcast();
}


uint32_t Profiler::Connect()
{
    uint32_t id = ( m_nextConnectionId++ );
    uint32_t connId = ( id << 1 );

#ifdef TRACY_ON_DEMAND
    if ( m_pUiConnection )
    {
        m_pUiConnection->id = id;
        connId |= 0x01;
    }
#else
        connId |= 0x01;
#endif // ifdef TRACY_ON_DEMAND


    {
        m_serialLock.lock();
        assert( m_serialQueue.empty() );
        assert( m_serialDequeue.empty() );
        m_serialLock.unlock();
    }

    m_connectionId.store( connId, std::memory_order_release );
    return id;
}


void Profiler::Disconnect()
{
    m_connectionId.store( 0, std::memory_order_release );
}


Profiler::DequeueStatus Profiler::ProcessData()
{
#ifndef TRACY_ON_DEMAND
    ProcessSysTime();
#endif

    DequeueStats stats = { 0 };

    const DequeueStatus threadStatus = ProcessDataThread( stats );
    const DequeueStatus serialStatus = DequeueSerial( stats );
    const DequeueStatus systemStatus = DequeueSys( stats );
    const DequeueStatus symbolStatus = DequeueSymbols();

    DequeueStatus result = DequeueStatus::DataDequeued;
    if(    ( threadStatus == DequeueStatus::ConnectionLost )
        || ( serialStatus == DequeueStatus::ConnectionLost )
        || ( systemStatus == DequeueStatus::ConnectionLost )
        || ( symbolStatus == DequeueStatus::ConnectionLost ) )
    {
        result = DequeueStatus::ConnectionLost;
    }
    else if(    ( threadStatus == DequeueStatus::QueueEmpty )
             && ( serialStatus == DequeueStatus::QueueEmpty )
             && ( systemStatus == DequeueStatus::QueueEmpty )
             && ( symbolStatus == DequeueStatus::QueueEmpty ) )
    {
        result = DequeueStatus::QueueEmpty;
    }

    return result;
}


Profiler::DequeueStatus Profiler::ProcessDataSerial( DequeueStats& rStats )
{
#ifndef TRACY_ON_DEMAND
    ProcessSysTime();
#endif

    DequeueStatus result = DequeueSerial( rStats );
    return result;
}


Profiler::DequeueStatus Profiler::ProcessDataThread( DequeueStats& rStats )
{
    moodycamel::ConsumerToken token(GetQueue());

    using namespace std::chrono;

    DequeueStatus result = DequeueStatus::QueueEmpty;

    const microseconds maxDequeueUs(1500);
    const auto start = high_resolution_clock::now();
    for ( ;; )
    {
        DequeueStatus status = Dequeue( token, rStats );
        if ( status == DequeueStatus::QueueEmpty )
        {
            break;
        }

        result = status;
        if (    ShouldExit()
             || ( status == DequeueStatus::ConnectionLost )
             || ( duration_cast< microseconds >( high_resolution_clock::now() - start ) < maxDequeueUs ) )
        {
            break;
        }
    }

    return result;
}


Profiler::DequeueStatus Profiler::ProcessDataSys( DequeueStats& rStats )
{
    DequeueStatus result = DequeueSys( rStats );
    return result;
}


Profiler::DequeueStatus Profiler::ProcessDataSymbols()
{
    DequeueStatus result = DequeueSymbols();
    return result;
}


bool Profiler::CommitPendingData()
{
    bool result = true;
    if ( m_pBufferHandler && ( m_pBufferHandler->m_buffer.offset != m_pBufferHandler->m_buffer.start ) )
    {
        result = CommitData( 0 );
    }
    return result;
}


#ifdef TRACY_HAS_SYSTEM_TRACING
bool Profiler::IsSysTraceRunning()
{
    return ( s_sysTraceThread && s_sysTraceThread->IsRunning() );
}
#endif // ifdef TRACY_HAS_SYSTEM_TRACING


#if defined(TRACY_NEEDS_SYMBOL_WORKER)
bool Profiler::IsSymbolResolutionRunning()
{
    return ( s_symbolThread && s_symbolThread->IsRunning() );
}
#endif // if defined(TRACY_NEEDS_SYMBOL_WORKER)


void Profiler::LockAnnounce( volatile void *pLockId, const SourceLocationData* pSrcloc, const char *name, size_t len )
{
    LockAssert( m_pSyncState );
    if ( m_pSyncState->LockTrackingEnabled() )
    {
        const uint32_t connId = ConnectionId();
        const uint32_t lockId32 = GetLockCounter().fetch_add( 1, std::memory_order_relaxed );
        const uint64_t lockId64 = (uint64_t)pLockId;
        m_pSyncState->LockAnnounce( connId, lockId64, lockId32, pSrcloc, name, len );
    }
}

void Profiler::LockTerminate( volatile void *pLockId )
{
    LockAssert( m_pSyncState );
    if ( m_pSyncState->LockTrackingEnabled() )
    {
        const uint32_t connId = ConnectionId();
        const uint64_t lockId64 = (uint64_t)pLockId;
        m_pSyncState->LockTerminate( connId, lockId64 );
    }
}

void Profiler::LockSetName( volatile void* pLockId, const char* name, size_t len )
{
    LockAssert( m_pSyncState );
    if ( m_pSyncState->LockTrackingEnabled() )
    {
        const uint32_t connId = ConnectionId();
        const uint64_t lockId64 = (uint64_t)pLockId;
        m_pSyncState->LockSetName( connId, lockId64, name, len );
    }
}


void Profiler::LockWaitBegin( volatile void *pLockId, const char *file, int line )
{
    LockAssert( m_pSyncState );
    if ( m_pSyncState->LockTrackingEnabled() )
    {
        const uint32_t connId = ConnectionId();
        const uint64_t lockId64 = (uint64_t)pLockId;
        m_pSyncState->LockWaitBegin( connId, lockId64, file, line );
    }
}


void Profiler::LockAcquired( volatile void *pLockId, const char *file, int line )
{
    LockAssert( m_pSyncState );
    if ( m_pSyncState->LockTrackingEnabled() )
    {
        const uint32_t connId = ConnectionId();
        const uint64_t lockId64 = (uint64_t)pLockId;
        m_pSyncState->LockAcquired( connId, lockId64, file, line );
    }
}


void Profiler::LockAcquiredTry( volatile void *pLockId, const char *file, int line )
{
    LockAssert( m_pSyncState );
    if ( m_pSyncState->LockTrackingEnabled() )
    {
        const uint32_t connId = ConnectionId();
        const uint64_t lockId64 = (uint64_t)pLockId;
        m_pSyncState->LockAcquiredTry( connId, lockId64, file, line );
    }
}


void Profiler::LockReleased( volatile void *pLockId, const char *file, int line )
{
    LockAssert( m_pSyncState );
    if ( m_pSyncState->LockTrackingEnabled() )
    {
        const uint32_t connId = ConnectionId();
        const uint64_t lockId64 = (uint64_t)pLockId;
        m_pSyncState->LockReleased( connId, lockId64, file, line );
    }
}


const WelcomeMessage& Profiler::WaitForStartup()
{
    while( m_timeBegin.load( std::memory_order_relaxed ) == 0 )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }

#ifdef TRACY_USE_RPMALLOC
    rpmalloc_thread_initialize();
#endif

    memset( &m_refTimes, 0, sizeof( m_refTimes ) );

    m_exectime = 0;
    const auto execname = GetProcessExecutablePath();
    if( execname )
    {
        struct stat st;
        if( stat( execname, &st ) == 0 )
        {
            m_exectime = (uint64_t)st.st_mtime;
        }
    }

    const auto hostinfo = GetHostInfo();
    const auto hisz = std::min<size_t>( strlen( hostinfo ), WelcomeMessageHostInfoSize - 1 );

    const uint64_t pid = GetPid();

    uint8_t flags = 0;

#ifdef TRACY_ON_DEMAND
    flags |= WelcomeFlag::OnDemand;
#endif
#ifdef __APPLE__
    flags |= WelcomeFlag::IsApple;
#endif
#ifndef TRACY_NO_CODE_TRANSFER
    flags |= WelcomeFlag::CodeTransfer;
#endif
#ifdef _WIN32
    flags |= WelcomeFlag::CombineSamples;
#  ifndef TRACY_NO_CONTEXT_SWITCH
    if ( ( m_options & Options_ContextSwitches ) != 0 )
    {
        flags |= WelcomeFlag::IdentifySamples;
    }
#  endif
#endif

#if defined __i386 || defined _M_IX86
    uint8_t cpuArch = CpuArchX86;
#elif defined __x86_64__ || defined _M_X64
    uint8_t cpuArch = CpuArchX64;
#elif defined __aarch64__
    uint8_t cpuArch = CpuArchArm64;
#elif defined __ARM_ARCH
    uint8_t cpuArch = CpuArchArm32;
#else
    uint8_t cpuArch = CpuArchUnknown;
#endif

#if defined __i386 || defined _M_IX86 || defined __x86_64__ || defined _M_X64
    uint32_t regs[4];
    char manufacturer[12];
    CpuId( regs, 0 );
    memcpy( manufacturer, regs+1, 4 );
    memcpy( manufacturer+4, regs+3, 4 );
    memcpy( manufacturer+8, regs+2, 4 );

    CpuId( regs, 1 );
    uint32_t cpuId = ( regs[0] & 0xFFF ) | ( ( regs[0] & 0xFFF0000 ) >> 4 );
#else
    const char manufacturer[12] = {};
    uint32_t cpuId = 0;
#endif

    int64_t initTime = GetInitTime();

    size_t programNameLen = std::min<size_t>( m_programNameLen, WelcomeMessageProgramNameSize - 1 );
    MemWrite( &m_welcome.timerMul, m_timerMul );
    MemWrite( &m_welcome.initBegin, initTime );
    MemWrite( &m_welcome.initEnd, m_timeBegin.load( std::memory_order_relaxed ) );
    MemWrite( &m_welcome.delay, m_delay );
    MemWrite( &m_welcome.resolution, m_resolution );
    MemWrite( &m_welcome.epoch, m_epoch );
    MemWrite( &m_welcome.exectime, m_exectime );
    MemWrite( &m_welcome.pid, pid );
    MemWrite( &m_welcome.samplingPeriod, m_samplingPeriod );
    MemWrite( &m_welcome.flags, flags );
    MemWrite( &m_welcome.cpuArch, cpuArch );
    memcpy( m_welcome.cpuManufacturer, manufacturer, 12 );
    MemWrite( &m_welcome.cpuId, cpuId );
    memcpy( m_welcome.programName, m_programName, programNameLen );
    memset( m_welcome.programName + programNameLen, 0, WelcomeMessageProgramNameSize - programNameLen );
    memcpy( m_welcome.hostInfo, hostinfo, hisz );
    memset( m_welcome.hostInfo + hisz, 0, WelcomeMessageHostInfoSize - hisz );

    if ( TracyHasCommandLineOption( "-tracy_enable" ) )
    {
        this->RequestListenAndBroadcast();
    }

    return m_welcome;
}


void Profiler::WaitForShutdown( ListenSocket &listen )
{
    moodycamel::ConsumerToken token(GetQueue());

#ifndef TRACY_ON_DEMAND
    // Client is no longer available here. Accept incoming connections, but reject handshake.
    while ( !ShouldExit() )
    {
        ClearQueues( token );

        if ( ConnectToUi( listen, false ) )
        {
            InitiateUiConnection( HandshakeNotAvailable );
            SetBufferHandler( nullptr );
            UiConnection::Destroy( m_pUiConnection );
        }
    }

    m_shutdownFinished.store( true, std::memory_order_relaxed );
    return;
#endif

    // Wait for symbols thread to terminate. Symbol resolution will continue in this thread.
#ifdef TRACY_NEEDS_SYMBOL_WORKER
#if !TRACY_PROCESS_REMAINING_QUERIES
    m_cancelSymbolProcessing.store( true );
#endif

    while( s_symbolThreadRunning.load( std::memory_order_acquire ) ) { YieldThread(); }
#endif

    // Client is exiting. Send items remaining in queues.
    while ( m_pUiConnection && m_pUiConnection->IsValid() )
    {
        DequeueStatus status = ProcessData();
        if ( status == DequeueStatus::ConnectionLost )
        {
            break;
        }
        else if ( status == DequeueStatus::QueueEmpty )
        {
            CommitPendingData();
            break;
        }

        if ( !ProcessServerQuery() )
        {
            break;
        }

#if defined( TRACY_NEEDS_SYMBOL_WORKER ) && TRACY_PROCESS_REMAINING_QUERIES
        FastVector<SymbolQueueItem> remainingRequestQueue( m_symbolRequestQueue.size() );
        m_requestSymbolLock.lock();
        remainingRequestQueue.swap( m_symbolRequestQueue );
        m_requestSymbolLock.unlock();
        ProcessSymbolQueueItems( remainingRequestQueue.data(), remainingRequestQueue.size() );
#endif
    }

    // Send client termination notice to the server
    QueueItem terminate;
    MemWrite( &terminate.hdr.type, QueueType::Terminate );
    if ( SendData( ( const char * ) &terminate, 1 ) )
    {
        // Handle remaining server queries
        for ( ;; )
        {
            if ( !ProcessServerQuery() )
            {
                break;
            }

#if defined( TRACY_NEEDS_SYMBOL_WORKER ) && TRACY_PROCESS_REMAINING_QUERIES
            FastVector<SymbolQueueItem> remainingRequestQueue( m_symbolRequestQueue.size() );
            m_requestSymbolLock.lock();
            remainingRequestQueue.swap( m_symbolRequestQueue );
            m_requestSymbolLock.unlock();
            ProcessSymbolQueueItems( remainingRequestQueue.data(), remainingRequestQueue.size() );
#endif

            DequeueStatus status = ProcessData();
            if ( status == DequeueStatus::ConnectionLost )
            {
                break;
            }

            if ( !CommitPendingData() )
            {
                break;
            }
        }
    }

    SetBufferHandler( nullptr );
    UiConnection::Destroy( m_pUiConnection );
    m_shutdownFinished.store( true, std::memory_order_relaxed );

#if TRACY_PROCESS_REMAINING_QUERIES
    EndCallstack();
#endif
}


bool Profiler::WaitForActivation()
{
    bool isActive = true;

    moodycamel::ConsumerToken token( GetQueue() );

    // Only start listening and broadcasting once the application tell us to do so
    while ( m_listenAndBroadcastRequested.load( std::memory_order_relaxed ) == 0 )
    {
        if ( ShouldExit() )
        {
            m_shutdownFinished.store( true, std::memory_order_relaxed );
            isActive = false;
            break;
        }

        ClearQueues( token );
        std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
    }

    return isActive;
}


void Profiler::WorkerLoopBegin()
{
    InstallCrashHandler();
}


void Profiler::WorkerLoopEnd()
{
    RemoveCrashHandler();
}


void Profiler::StartBroadcast( uint16_t broadcastPort, uint32_t dataPort )
{
    static_assert(std::is_trivial_v<Profiler::Broadcaster>, "Broadcaster can't be memset");
    memset(&m_broadcaster, 0, sizeof(m_broadcaster));

    int broadcastLen = 0;
    BroadcastMessage& broadcastMsg = GetBroadcastMessage( m_programName, m_programNameLen, broadcastLen, dataPort );

#ifndef TRACY_NO_BROADCAST
    m_broadcaster.broadcast = (UdpBroadcast*)tracy_malloc( sizeof( UdpBroadcast ) );
    new(m_broadcaster.broadcast) UdpBroadcast();
#  if 1			// was ifdef TRACY_ONLY_LOCALHOST
    const char* addr = "127.255.255.255";
#  else
    const char* addr = "255.255.255.255";
#  endif

    if( m_broadcaster.broadcast->Open( addr, broadcastPort ) )
    {
        m_broadcaster.isActive = true;

        m_broadcaster.broadcastAddr = addr;
        m_broadcaster.broadcastPort = broadcastPort;
        m_broadcaster.broadcastMsg = broadcastMsg;
        m_broadcaster.broadcastLen = broadcastLen;
        m_broadcaster.epoch = m_epoch;
    }
    else
    {
        if( m_broadcaster.broadcast )
        {
            m_broadcaster.broadcast->~UdpBroadcast();
            tracy_free( m_broadcaster.broadcast );
            memset(&m_broadcaster, 0, sizeof(m_broadcaster));
        }
    }
#endif
}


void Profiler::StopBroadcast()
{
    if( m_broadcaster.isActive )
    {
        m_broadcaster.lastBroadcast = 0;
        m_broadcaster.broadcastMsg.activeTime = -1;
        m_broadcaster.broadcast->Send( m_broadcaster.broadcastPort, &m_broadcaster.broadcastMsg, m_broadcaster.broadcastLen );
    }
}


void Profiler::SetBroadcastMessage( const char* message = nullptr )
{
    BroadcastMessage *pBcMsg = &m_broadcaster.broadcastMsg;
    if ( message && message[0] )
    {
        static_assert( std::numeric_limits<uint8_t>::max() >= ProfilerMessageSize );
        size_t len = strlen( message );
        size_t msgLen = std::min( len, (size_t)tracy::ProfilerMessageSize );
        pBcMsg->msgLen = msgLen;
        memcpy( pBcMsg->strBuffer + pBcMsg->nameLen, message, msgLen );
    }
    else
    {
        pBcMsg->msgLen = 0;
        memset( pBcMsg->strBuffer + pBcMsg->nameLen, 0, sizeof(m_broadcaster.broadcastMsg.strBuffer) - pBcMsg->nameLen );
    }
}


void Profiler::UpdateBroadcast( bool noCheckSend )
{
    if( m_broadcaster.isActive )
    {
        const auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        if( noCheckSend || ( (t - m_broadcaster.lastBroadcast) > 3000000000 ) )  // 3s
        {
            m_broadcaster.lastBroadcast = t;
            const auto ts = std::chrono::duration_cast<std::chrono::seconds>( std::chrono::system_clock::now().time_since_epoch() ).count();
            m_broadcaster.broadcastMsg.activeTime = int32_t( ts - m_broadcaster.epoch );
            assert( m_broadcaster.broadcastMsg.activeTime >= 0 );
            size_t strBufferLen = m_broadcaster.broadcastMsg.nameLen + m_broadcaster.broadcastMsg.msgLen;
            m_broadcaster.broadcastLen = int( offsetof( BroadcastMessage, strBuffer ) + strBufferLen );
            m_broadcaster.broadcast->Send( m_broadcaster.broadcastPort, &m_broadcaster.broadcastMsg, m_broadcaster.broadcastLen );
        }
    }
}


bool Profiler::StartListening(ListenSocket& listen, uint32_t& dataPort, bool dataPortSearch, uint16_t broadcastPort)
{
    bool shouldShutDown = false;

    bool isListening = false;
    if( !dataPortSearch )
    {
        isListening = listen.Listen( dataPort, 4 );
    }
    else
    {
        for( uint32_t i=0; i<32; i++ )
        {
            if( listen.Listen( dataPort+i, 4 ) )
            {
                dataPort += i;
                isListening = true;
                break;
            }
        }
    }

    if (isListening)
    {
        m_listenPort = dataPort;
        StartBroadcast(broadcastPort, dataPort);
    }
    else
    {
        moodycamel::ConsumerToken token( GetQueue() );
        for(;;)
        {
            if( ShouldExit() )
            {
                m_shutdownFinished.store( true, std::memory_order_relaxed );
                shouldShutDown = true;

                assert( !m_pUiConnection );
                assert( !m_pBufferHandler );
                assert( !IsConnected() );
                break;
            }

            ClearQueues( token );
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }
    }

    return shouldShutDown;
}


bool Profiler::StartListening( ListenSocket& listen )
{
#ifdef TRACY_DATA_PORT
    const bool dataPortSearch = false;
    auto dataPort = m_userPort != 0 ? m_userPort : TRACY_DATA_PORT;
#else
    const bool dataPortSearch = m_userPort == 0;
    auto dataPort = m_userPort != 0 ? m_userPort : 8086;
#endif
#ifdef TRACY_BROADCAST_PORT
    const auto broadcastPort = TRACY_BROADCAST_PORT;
#else
    const auto broadcastPort = 8086;
#endif

    return StartListening(listen, dataPort, dataPortSearch, broadcastPort);
}


bool Profiler::SwitchThreadCtx( uint32_t threadId, RefTimes& refTimes )
{
    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::ThreadContext );
    MemWrite( &item.threadCtx.thread, threadId );
    refTimes.m_threadCtx = threadId;
    refTimes.m_refTimeThread = 0;
    return AppendData( &item, QueueDataSize[(int)QueueType::ThreadContext] );
}


bool Profiler::SynchronizeTracyNoLock( const RefTimes& syncRefTimes, RefTimes& refTimes )
{
    assert( m_pSyncState );

    bool result = true;

#ifdef TRACY_ON_DEMAND
#define AppendItem( item )                                              \
    if ( !AppendData( &(item), QueueDataSize[ (item).hdr.idx ] ) ) {    \
        result = false;                                                 \
        goto done;                                                      \
    }

    if ( !m_pSyncState->SynchronizeLocks( *this, refTimes, ConnectionId() ) )
    {
        result = false;
        goto done;
    }

    // Sync on demand items now. (We needed to make sure the active locks are known before sending the lock names)
    {
        for( auto& item : m_deferredQueue )
        {
            uint64_t ptr;
            uint16_t size;
            const auto idx = MemRead<uint8_t>( &item.hdr.idx );
            switch( (QueueType)idx )
            {
                case QueueType::MessageAppInfo:
                    ptr = MemRead<uint64_t>( &item.messageFat.text );
                    size = MemRead<uint16_t>( &item.messageFat.size );
                    SendSingleString( (const char*)ptr, size );
                    break;
                case QueueType::LockName:
                    ptr = MemRead<uint64_t>( &item.lockNameFat.name );
                    size = MemRead<uint16_t>( &item.lockNameFat.size );
                    SendSingleString( (const char*)ptr, size );
                    break;
                case QueueType::GpuContextName:
                    ptr = MemRead<uint64_t>( &item.gpuContextNameFat.ptr );
                    size = MemRead<uint16_t>( &item.gpuContextNameFat.size );
                    SendSingleString( (const char*)ptr, size );
                    break;
                case QueueType::HwCounterConfig:
                {
                    const char *counterName = ( const char * ) MemRead<uint64_t>( &item.hwCounterConfig.name );
                    const char *counterDesc = ( const char * ) MemRead<uint64_t>( &item.hwCounterConfig.description );
                    SendSingleString( counterName );
                    SendSecondString( counterDesc );
                    break;
                }
                default:
                    break;
            }
            AppendItem( item );
        }
        if ( !CommitPendingData() )
        {
            result = false;
            goto done;
        }
    }

#if defined(TRACY_HAS_BG_SUPPORT)
    result = m_pSyncState->Synchronize( *this, syncRefTimes, refTimes, ConnectionId() );

    // Make sure we have synced to the expected thread and timestamps!
    assert( refTimes.m_threadCtx == syncRefTimes.m_threadCtx );
    assert( refTimes.m_refTimeThread == syncRefTimes.m_refTimeThread );
    assert( refTimes.m_refTimeSerial == syncRefTimes.m_refTimeSerial );
    assert( refTimes.m_refTimeGpu == syncRefTimes.m_refTimeGpu );
#endif // if defined(TRACY_HAS_BG_SUPPORT)

    result = true;
done:
    if ( !CommitPendingData() )
    {
        result = false;
    }

#undef AppendItem

#endif // ifdef TRACY_ON_DEMAND

    return result;
}


bool Profiler::SynchronizeSysNoLock( const RefTimes& syncRefTimes, RefTimes& refTimes )
{
    bool result = true;

#ifdef TRACY_ON_DEMAND

    // Sync context switches and sys trace info
#ifdef TRACY_HAS_SYSTEM_TRACING

#define AppendItem( item )                                              \
    if ( !AppendData( &(item), QueueDataSize[ (item).hdr.idx ] ) ) {    \
        result = false;                                                 \
    }

    if ( refTimes.m_refTimeCtx != syncRefTimes.m_refTimeCtx )
    {
        int64_t t = syncRefTimes.m_refTimeCtx;
        int64_t dt = t - refTimes.m_refTimeCtx;
        refTimes.m_refTimeCtx = t;

        QueueItem csSync;
        memset( &csSync, 0, sizeof( csSync ) );
        MemWrite( &csSync.hdr.type, QueueType::ContextSwitch );
        MemWrite( &csSync.contextSwitch.time, syncRefTimes.m_refTimeCtx );
        AppendItem( csSync );
    }

    {
        QueueItem syncValidation;
        MemWrite( &syncValidation.hdr.type, QueueType::SyncValidation );
        uint8_t flags = ( QueueSyncValidation::TimeCtx );
        MemWrite( &syncValidation.syncValidation.flags, flags );
        MemWrite( &syncValidation.syncValidation.threadCtx, 0 );
        MemWrite( &syncValidation.syncValidation.refTimeThread, 0 );
        MemWrite( &syncValidation.syncValidation.refTimeSerial, 0 );
        MemWrite( &syncValidation.syncValidation.refTimeCtx, m_refTimes.m_refTimeCtx );
        AppendItem( syncValidation );
    }

    assert( refTimes.m_refTimeCtx == syncRefTimes.m_refTimeCtx );

    if ( !CommitPendingData() )
    {
        result = false;
    }
#endif // ifdef TRACY_HAS_SYSTEM_TRACING

#endif // ifdef TRACY_ON_DEMAND

    return result;
}


void Profiler::UpdateSyncInfo( QueueType type, QueueItem *item, const RefTimes &refTimes )
{
#if defined(TRACY_HAS_BG_SUPPORT)
    assert( m_pSyncState != nullptr );
    if ( m_pSyncState->HasSyncMode( ProfilerSyncState::Zones ) )
    {
        m_pSyncState->UpdateSyncInfo( type, item, refTimes );
    }
#endif // if defined(TRACY_HAS_BG_SUPPORT)
}


bool Profiler::HandleKeepAlive( DequeueStatus status, int& keepAlive )
{
    bool valid = true;
    if( status == DequeueStatus::ConnectionLost )
    {
        valid = false;
    }
    else if( status == DequeueStatus::QueueEmpty )
    {
        valid = CommitPendingData();

        if( keepAlive == 500 )
        {
            QueueItem ka;
            ka.hdr.type = QueueType::KeepAlive;
            AppendData( &ka, QueueDataSize[ka.hdr.idx] );
            valid = CommitPendingData();
            keepAlive = 0;
        }
        else if( !m_pUiConnection || (m_pUiConnection->IsValid() && !m_pUiConnection->HasData()) )
        {
            keepAlive++;
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }
    }
    else
    {
        keepAlive = 0;
    }

    return valid;
}


bool Profiler::InitiateUiConnection( HandshakeStatus handshake )
{
    assert( m_pUiConnection );
    bool succeeded = false;
    if ( m_pUiConnection && m_pUiConnection->IsValid() )
    {
        // Handshake
        char shibboleth[HandshakeShibbolethSize];
        m_pUiConnection->ReadRaw( shibboleth, HandshakeShibbolethSize, 2000 );
        bool validShib = ( memcmp( shibboleth, HandshakeShibboleth, HandshakeShibbolethSize ) == 0 );

        uint32_t protocolVersion = 0;
        m_pUiConnection->ReadRaw( &protocolVersion, sizeof( protocolVersion ), 2000 );
        bool validProt = ( protocolVersion == ProtocolVersion );

        if( !validProt )
        {
            HandshakeStatus status = HandshakeProtocolMismatch;
            m_pUiConnection->Send( &status, sizeof( status ) );
        }

        bool validHandshake = m_pUiConnection->Send( &handshake, sizeof( handshake ) ) == sizeof( handshake );
        bool validInit = ( validShib && validProt && validHandshake && ( handshake == HandshakeWelcome ) );
        if ( validInit && m_pUiConnection->IsValid() )
        {
            m_pUiConnection->Send( &m_welcome, sizeof( m_welcome ) );
        }

        if ( validInit && m_pUiConnection->IsValid() )
        {
            StopBroadcast();
            succeeded = true;
        }
    }

    return succeeded;
}

bool Profiler::ConnectToUi(ListenSocket& listen, bool block)
{
    assert( !m_pUiConnection );
    assert( !m_pBufferHandler );
    bool success = false;
    Socket* sock = nullptr;
    do
    {
#ifndef TRACY_NO_EXIT
        if( !m_noExit && ShouldExit() )
        {
            StopBroadcast();
            m_shutdownFinished.store( true, std::memory_order_relaxed );
            break;
        }
#endif

        sock = listen.Accept();
        if (sock != nullptr)
        {
            break;
        }
        else
        {
#ifndef TRACY_ON_DEMAND
            ProcessSysTime();
#endif

            UpdateBroadcast();
        }
    } while (block);

    if (sock)
    {
        if ( ShouldExit() )
        {
            sock->~Socket();
            tracy_free( sock );
            sock = nullptr;
            assert( !m_pUiConnection );
            assert( m_pBufferHandler );
        }
        else
        {
            m_pUiConnection = UiConnection::Create( sock );
            success = true;

            if( m_broadcaster.isActive )
            {
                m_broadcaster.lastBroadcast = 0;
                m_broadcaster.broadcastMsg.activeTime = -1;
                m_broadcaster.broadcast->Send( m_broadcaster.broadcastPort, &m_broadcaster.broadcastMsg, m_broadcaster.broadcastLen );
            }
        }
    }

    return success;
}


void Profiler::Worker()
{
#if defined __linux__ && !defined TRACY_NO_CRASH_HANDLER
    s_profilerTid = syscall( SYS_gettid );
#endif

    ThreadExitHandler threadExitHandler;

    InitRpmalloc();
    SetThreadName( "Tracy Profiler" );

#ifdef TRACY_DATA_PORT
    const bool dataPortSearch = false;
    auto dataPort = m_userPort != 0 ? m_userPort : TRACY_DATA_PORT;
#else
    const bool dataPortSearch = m_userPort == 0;
    auto dataPort = m_userPort != 0 ? m_userPort : 8086;
#endif
#ifdef TRACY_BROADCAST_PORT
    const auto broadcastPort = TRACY_BROADCAST_PORT;
#else
    const auto broadcastPort = 8086;
#endif


    const WelcomeMessage& welcome = WaitForStartup();
    (void)welcome;

    if ( !WaitForActivation() )
    {
        return;
    }

    ListenSocket listen;
    bool shouldShutDown = StartListening(listen, dataPort, dataPortSearch, broadcastPort);
    if( shouldShutDown )
    {
        return;
    }

    WorkerLoopBegin();

#ifdef TRACY_ON_DEMAND
    const bool keepLooping = true;
#else
    const bool keepLooping = false;
#endif // ifndef TRACY_ON_DEMAND

    // Connections loop.
    // Each iteration of the loop handles whole connection. Multiple iterations will only
    // happen in the on-demand mode or when handshake fails.
    while ( keepLooping && !ShouldExit() )
    {
#if defined(TRACY_HAS_BG_SUPPORT)
        bool block = ( m_pExtWorker == nullptr );
#else
        bool block = true;
#endif

        if ( ConnectToUi( listen, block ) )
        {
            assert( m_pUiConnection );
            IBufferHandler *pPrevHandler = SetBufferHandler( m_pUiConnection );
            assert( pPrevHandler == nullptr );

            InitiateUiConnection( HandshakeWelcome );

            PreConnect( "" );

            OnDemandPayloadMessage onDemand;
            onDemand.frames = GetFrameCount();
            onDemand.currentTime = GetTime();

            m_pUiConnection->Send( &onDemand, sizeof( onDemand ) );
            Synchronize();
            while ( m_pUiConnection->IsValid() && m_pUiConnection->HasData() )
            {
                ProcessServerQuery();
                CommitPendingData();
            }

            if ( m_pUiConnection->IsValid() )
            {
                Connect();

                // Main communications loop
                int keepAlive = 0;
                while ( !ShouldExit() && m_pUiConnection->IsValid() )
                {
                    DequeueStatus status = ProcessData();
                    HandleKeepAlive( status, keepAlive );
                    ProcessServerQuery();
                    CommitPendingData();
                }

                Disconnect();
            }

            PostConnect();

            SetBufferHandler( pPrevHandler );
        }
#if defined(TRACY_HAS_BG_SUPPORT)
        else if ( m_pExtWorker )
        {
            m_pSyncState->SetSyncModeEnabled( ProfilerSyncState::Zones, true );
            m_pExtWorker->Connection( *this, welcome );
            m_pSyncState->SetSyncModeEnabled( ProfilerSyncState::Zones, false );
        }
#endif // if defined(TRACY_HAS_BG_SUPPORT)
    }
    // End of connections loop

    WorkerLoopEnd();

    WaitForShutdown( listen );
}

#ifndef TRACY_NO_FRAME_IMAGE
void Profiler::CompressWorker()
{
    ThreadExitHandler threadExitHandler;
    SetThreadName( "Tracy DXT1" );
    while( m_timeBegin.load( std::memory_order_relaxed ) == 0 ) std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );

#ifdef TRACY_USE_RPMALLOC
    rpmalloc_thread_initialize();
#endif

    for(;;)
    {
        const auto shouldExit = ShouldExit();

        {
            bool lockHeld = true;
            while( !m_fiLock.try_lock() )
            {
                if( m_shutdownManual.load( std::memory_order_relaxed ) )
                {
                    lockHeld = false;
                    break;
                }
            }
            if( !m_fiQueue.empty() ) m_fiQueue.swap( m_fiDequeue );
            if( lockHeld )
            {
                m_fiLock.unlock();
            }
        }

        const auto sz = m_fiDequeue.size();
        if( sz > 0 )
        {
            auto fi = m_fiDequeue.data();
            auto end = fi + sz;
            while( fi != end )
            {
                const auto w = fi->w;
                const auto h = fi->h;
                const auto csz = size_t( w * h / 2 );
                auto etc1buf = (char*)tracy_malloc( csz );
                CompressImageDxt1( (const char*)fi->image, etc1buf, w, h );
                tracy_free( fi->image );

                TracyLfqPrepare( QueueType::FrameImage );
                MemWrite( &item->frameImageFat.image, (uint64_t)etc1buf );
                MemWrite( &item->frameImageFat.frame, fi->frame );
                MemWrite( &item->frameImageFat.w, w );
                MemWrite( &item->frameImageFat.h, h );
                uint8_t flip = fi->flip;
                MemWrite( &item->frameImageFat.flip, flip );
                TracyLfqCommit;

                fi++;
            }
            m_fiDequeue.clear();
        }
        else
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
        }

        if( shouldExit )
        {
            return;
        }
    }
}
#endif

static void FreeAssociatedMemory( const QueueItem& item )
{
    if( item.hdr.idx >= (int)QueueType::Terminate ) return;

    uint64_t ptr;
    switch( item.hdr.type )
    {
    case QueueType::ZoneText:
    case QueueType::ZoneName:
        ptr = MemRead<uint64_t>( &item.zoneTextFat.text );
        tracy_free( (void*)ptr );
        break;
    case QueueType::MessageColor:
    case QueueType::MessageColorCallstack:
        ptr = MemRead<uint64_t>( &item.messageColorFat.text );
        tracy_free( (void*)ptr );
        break;
    case QueueType::Message:
    case QueueType::MessageCallstack:
#ifndef TRACY_ON_DEMAND
    case QueueType::MessageAppInfo:
#endif
        ptr = MemRead<uint64_t>( &item.messageFat.text );
        tracy_free( (void*)ptr );
        break;
    case QueueType::ZoneBeginAllocSrcLoc:
    case QueueType::ZoneBeginAllocSrcLocCallstack:
        ptr = MemRead<uint64_t>( &item.zoneBegin.srcloc );
        tracy_free( (void*)ptr );
        break;
    case QueueType::GpuZoneBeginAllocSrcLoc:
    case QueueType::GpuZoneBeginAllocSrcLocCallstack:
    case QueueType::GpuZoneBeginAllocSrcLocSerial:
    case QueueType::GpuZoneBeginAllocSrcLocCallstackSerial:
        ptr = MemRead<uint64_t>( &item.gpuZoneBegin.srcloc );
        tracy_free( (void*)ptr );
        break;
    case QueueType::CallstackSerial:
    case QueueType::Callstack:
        ptr = MemRead<uint64_t>( &item.callstackFat.ptr );
        tracy_free( (void*)ptr );
        break;
    case QueueType::CallstackAlloc:
        ptr = MemRead<uint64_t>( &item.callstackAllocFat.nativePtr );
        tracy_free( (void*)ptr );
        ptr = MemRead<uint64_t>( &item.callstackAllocFat.ptr );
        tracy_free( (void*)ptr );
        break;
    case QueueType::CallstackSample:
    case QueueType::CallstackSampleContextSwitch:
        ptr = MemRead<uint64_t>( &item.callstackSampleFat.ptr );
        tracy_free( (void*)ptr );
        break;
    case QueueType::FrameImage:
        ptr = MemRead<uint64_t>( &item.frameImageFat.image );
        tracy_free( (void*)ptr );
        break;
#ifdef TRACY_HAS_CALLSTACK
    case QueueType::CallstackFrameSize:
    {
        InitRpmalloc();
        auto size = MemRead<uint8_t>( &item.callstackFrameSizeFat.size );
        auto data = (const CallstackEntry*)MemRead<uint64_t>( &item.callstackFrameSizeFat.data );
        for( uint8_t i=0; i<size; i++ )
        {
            const auto& frame = data[i];
            tracy_free_fast( (void*)frame.name );
            tracy_free_fast( (void*)frame.file );
        }
        tracy_free_fast( (void*)data );
        break;
    }
    case QueueType::SymbolInformation:
    {
        uint8_t needFree = MemRead<uint8_t>( &item.symbolInformationFat.needFree );
        if( needFree )
        {
            ptr = MemRead<uint64_t>( &item.symbolInformationFat.fileString );
            tracy_free( (void*)ptr );
        }
        break;
    }
    case QueueType::SymbolCodeMetadata:
        ptr = MemRead<uint64_t>( &item.symbolCodeMetadata.ptr );
        tracy_free( (void*)ptr );
        break;
#endif
#ifndef TRACY_ON_DEMAND
    case QueueType::LockName:
        ptr = MemRead<uint64_t>( &item.lockNameFat.name );
        tracy_free( (void*)ptr );
        break;
    case QueueType::GpuContextName:
        ptr = MemRead<uint64_t>( &item.gpuContextNameFat.ptr );
        tracy_free( (void*)ptr );
        break;
#endif
#ifdef TRACY_ON_DEMAND
    case QueueType::MessageAppInfo:
    case QueueType::GpuContextName:
        // Don't free memory associated with deferred messages.
        break;
#endif
#ifdef TRACY_HAS_SYSTEM_TRACING
    case QueueType::ExternalNameMetadata:
        ptr = MemRead<uint64_t>( &item.externalNameMetadata.name );
        tracy_free( (void*)ptr );
        ptr = MemRead<uint64_t>( &item.externalNameMetadata.threadName );
        tracy_free_fast( (void*)ptr );
        break;
#endif
    case QueueType::SourceCodeMetadata:
        ptr = MemRead<uint64_t>( &item.sourceCodeMetadata.ptr );
        tracy_free( (void*)ptr );
        break;
    default:
        break;
    }
}

void Profiler::ClearConnectionData()
{
    moodycamel::ConsumerToken token( GetQueue() );
    ClearQueues( token );

    memset( &m_refTimes, 0, sizeof( m_refTimes ) );

    assert( m_pSyncState != nullptr );
    m_pSyncState->Clear();
}

void Profiler::ClearQueues( moodycamel::ConsumerToken& token )
{
    assert( !IsConnected() );

    while ( !ShouldExit() )
    {
        const auto sz = GetQueue().try_dequeue_bulk_single( token, [](const uint64_t&){}, []( QueueItem* item, size_t sz ) { assert( sz > 0 ); while( sz-- > 0 ) FreeAssociatedMemory( *item++ ); } );
        if( sz == 0 ) break;
    }

    ClearSerial();
    ClearSys();
    ClearSymbol();
}

void Profiler::ClearSerial()
{
    bool lockHeld = true;
    while( !m_serialLock.try_lock() )
    {
        if( m_shutdownManual.load( std::memory_order_relaxed ) )
        {
            lockHeld = false;
            break;
        }
    }
    for( auto& v : m_serialQueue ) FreeAssociatedMemory( v );
    m_serialQueue.clear();
    if( lockHeld )
    {
        m_serialLock.unlock();
    }

    for( auto& v : m_serialDequeue ) FreeAssociatedMemory( v );
    m_serialDequeue.clear();
}

void Profiler::ClearSys()
{
#ifdef TRACY_HAS_SYSTEM_TRACING
    bool lockHeld = true;
    while( !m_sysLock.try_lock() )
    {
        if( m_shutdownManual.load( std::memory_order_relaxed ) )
        {
            lockHeld = false;
            break;
        }
    }
    for( auto& v : m_sysQueue ) FreeAssociatedMemory( v );
    m_sysQueue.clear();
    if( lockHeld )
    {
        m_sysLock.unlock();
    }

    for( auto& v : m_sysDequeue ) FreeAssociatedMemory( v );
    m_sysDequeue.clear();
#endif // ifdef TRACY_HAS_SYSTEM_TRACING
}

void Profiler::ClearSymbol()
{
#if defined(TRACY_NEEDS_SYMBOL_WORKER)

    m_cancelSymbolProcessing.store( true );

#ifdef TRACY_ON_DEMAND
    m_requestSymbolLock.lock();
    m_symbolRequestQueue.clear();
    m_requestSymbolLock.unlock();
#endif // ifdef TRACY_ON_DEMAND

    bool lockHeld = true;
    while( !m_symbolLock.try_lock() )
    {
        if( m_shutdownManual.load( std::memory_order_relaxed ) )
        {
            lockHeld = false;
            break;
        }
    }
    for( auto& v : m_symbolQueue ) FreeAssociatedMemory( v );
    m_symbolQueue.clear();
    if( lockHeld )
    {
        m_symbolLock.unlock();
    }

    for( auto& v : m_symbolDequeue ) FreeAssociatedMemory( v );
    m_symbolDequeue.clear();
#endif // if defined(TRACY_NEEDS_SYMBOL_WORKER)
}


Profiler::DequeueStatus Profiler::Dequeue( moodycamel::ConsumerToken& token, DequeueStats& rStats )
{
    assert( m_pBufferHandler );
    bool connectionLost = false;
    const auto sz = GetQueue().try_dequeue_bulk_single( token,
        [this, &connectionLost] ( const uint32_t& threadId )
        {
            if( ThreadCtxCheck( threadId, m_refTimes ) == ThreadCtxStatus::ConnectionLost ) connectionLost = true;
        },
        [this, &connectionLost, &rStats] ( QueueItem* item, size_t sz )
        {
            if( connectionLost ) return;
            InitRpmalloc();
            assert( sz > 0 );

            RefTimes syncTimes = m_refTimes;
#           define refThread syncTimes.m_refTimeThread
#           define refCtx syncTimes.m_refTimeCtx
#           define refGpu syncTimes.m_refTimeGpu

            while( sz-- > 0 )
            {
                uint64_t ptr;
                uint16_t size;
                auto idx = MemRead<uint8_t>( &item->hdr.idx );
                if( idx < (int)QueueType::Terminate )
                {
                    switch( (QueueType)idx )
                    {
                    case QueueType::ZoneText:
                    case QueueType::ZoneName:
                        ptr = MemRead<uint64_t>( &item->zoneTextFat.text );
                        size = MemRead<uint16_t>( &item->zoneTextFat.size );
                        SendSingleString( (const char*)ptr, size );
                        tracy_free_fast( (void*)ptr );
                        break;
                    case QueueType::Message:
                    case QueueType::MessageCallstack:
                        ptr = MemRead<uint64_t>( &item->messageFat.text );
                        size = MemRead<uint16_t>( &item->messageFat.size );
                        SendSingleString( (const char*)ptr, size );
                        tracy_free_fast( (void*)ptr );
                        break;
                    case QueueType::MessageColor:
                    case QueueType::MessageColorCallstack:
                        ptr = MemRead<uint64_t>( &item->messageColorFat.text );
                        size = MemRead<uint16_t>( &item->messageColorFat.size );
                        SendSingleString( (const char*)ptr, size );
                        tracy_free_fast( (void*)ptr );
                        break;
                    case QueueType::MessageAppInfo:
                        ptr = MemRead<uint64_t>( &item->messageFat.text );
                        size = MemRead<uint16_t>( &item->messageFat.size );
                        SendSingleString( (const char*)ptr, size );
#ifndef TRACY_ON_DEMAND
                        tracy_free_fast( (void*)ptr );
#endif
                        break;
                    case QueueType::ZoneBeginAllocSrcLoc:
                    case QueueType::ZoneBeginAllocSrcLocCallstack:
                    {
                        int64_t t = MemRead<int64_t>( &item->zoneBegin.time );
                        int64_t dt = t - refThread;
                        refThread = t;
                        MemWrite( &item->zoneBegin.time, dt );
                        ptr = MemRead<uint64_t>( &item->zoneBegin.srcloc );
                        SendSourceLocationPayload( ptr );
                        tracy_free_fast( (void*)ptr );
                        break;
                    }
                    case QueueType::Callstack:
                        ptr = MemRead<uint64_t>( &item->callstackFat.ptr );
                        SendCallstackPayload( ptr );
                        tracy_free_fast( (void*)ptr );
                        break;
                    case QueueType::CallstackAlloc:
                        ptr = MemRead<uint64_t>( &item->callstackAllocFat.nativePtr );
                        if( ptr != 0 )
                        {
                            CutCallstack( (void*)ptr, "lua_pcall" );
                            SendCallstackPayload( ptr );
                            tracy_free_fast( (void*)ptr );
                        }
                        ptr = MemRead<uint64_t>( &item->callstackAllocFat.ptr );
                        SendCallstackAlloc( ptr );
                        tracy_free_fast( (void*)ptr );
                        break;
                    case QueueType::CallstackSample:
                    case QueueType::CallstackSampleContextSwitch:
                    {
                        ptr = MemRead<uint64_t>( &item->callstackSampleFat.ptr );
                        SendCallstackPayload64( ptr );
                        tracy_free_fast( (void*)ptr );
                        int64_t t = MemRead<int64_t>( &item->callstackSampleFat.time );
                        int64_t dt = t - refCtx;
                        refCtx = t;
                        MemWrite( &item->callstackSampleFat.time, dt );
                        break;
                    }
                    case QueueType::FrameImage:
                    {
                        ptr = MemRead<uint64_t>( &item->frameImageFat.image );
                        const auto w = MemRead<uint16_t>( &item->frameImageFat.w );
                        const auto h = MemRead<uint16_t>( &item->frameImageFat.h );
                        const auto csz = size_t( w * h / 2 );
                        SendLongString( ptr, (const char*)ptr, csz, QueueType::FrameImageData );
                        tracy_free_fast( (void*)ptr );
                        break;
                    }
                    case QueueType::ZoneBegin:
                    case QueueType::ZoneBeginCallstack:
                    {
                        int64_t t = MemRead<int64_t>( &item->zoneBegin.time );
                        int64_t dt = t - refThread;
                        refThread = t;
                        MemWrite( &item->zoneBegin.time, dt );
                        break;
                    }
                    case QueueType::ZoneEnd:
                    {
                        int64_t t = MemRead<int64_t>( &item->zoneEnd.time );
                        int64_t dt = t - refThread;
                        refThread = t;
                        MemWrite( &item->zoneEnd.time, dt );
                        break;
                    }
                    case QueueType::GpuZoneBegin:
                    case QueueType::GpuZoneBeginCallstack:
                    {
                        int64_t t = MemRead<int64_t>( &item->gpuZoneBegin.cpuTime );
                        int64_t dt = t - refThread;
                        refThread = t;
                        MemWrite( &item->gpuZoneBegin.cpuTime, dt );
                        break;
                    }
                    case QueueType::GpuZoneBeginAllocSrcLoc:
                    case QueueType::GpuZoneBeginAllocSrcLocCallstack:
                    {
                        int64_t t = MemRead<int64_t>( &item->gpuZoneBegin.cpuTime );
                        int64_t dt = t - refThread;
                        refThread = t;
                        MemWrite( &item->gpuZoneBegin.cpuTime, dt );
                        ptr = MemRead<uint64_t>( &item->gpuZoneBegin.srcloc );
                        SendSourceLocationPayload( ptr );
                        tracy_free_fast( (void*)ptr );
                        break;
                    }
                    case QueueType::GpuZoneEnd:
                    {
                        int64_t t = MemRead<int64_t>( &item->gpuZoneEnd.cpuTime );
                        int64_t dt = t - refThread;
                        refThread = t;
                        MemWrite( &item->gpuZoneEnd.cpuTime, dt );
                        break;
                    }
                    case QueueType::GpuContextName:
                        ptr = MemRead<uint64_t>( &item->gpuContextNameFat.ptr );
                        size = MemRead<uint16_t>( &item->gpuContextNameFat.size );
                        SendSingleString( (const char*)ptr, size );
#ifndef TRACY_ON_DEMAND
                        tracy_free_fast( (void*)ptr );
#endif
                        break;
                    case QueueType::PlotDataInt:
                    case QueueType::PlotDataFloat:
                    case QueueType::PlotDataDouble:
                    {
                        int64_t t = MemRead<int64_t>( &item->plotDataInt.time );
                        int64_t dt = t - refThread;
                        refThread = t;
                        MemWrite( &item->plotDataInt.time, dt );
                        break;
                    }
                    case QueueType::ContextSwitch:
                    {
                        int64_t t = MemRead<int64_t>( &item->contextSwitch.time );
                        int64_t dt = t - refCtx;
                        refCtx = t;
                        MemWrite( &item->contextSwitch.time, dt );
                        break;
                    }
                    case QueueType::ThreadWakeup:
                    {
                        int64_t t = MemRead<int64_t>( &item->threadWakeup.time );
                        int64_t dt = t - refCtx;
                        refCtx = t;
                        MemWrite( &item->threadWakeup.time, dt );
                        break;
                    }
                    case QueueType::GpuTime:
                    {
                        int64_t t = MemRead<int64_t>( &item->gpuTime.gpuTime );
                        int64_t dt = t - refGpu;
                        refGpu = t;
                        MemWrite( &item->gpuTime.gpuTime, dt );
                        break;
                    }
                    case QueueType::HwCounterConfig:
                    {
                        const char *counterName = ( const char * ) MemRead<uint64_t>( &item->hwCounterConfig.name );
                        const char *counterDesc = ( const char * ) MemRead<uint64_t>( &item->hwCounterConfig.description );
                        SendSingleString( counterName );
                        SendSecondString( counterDesc );
                        break;
                    }
                    default:
                        assert( false );
                        break;
                    }
                }

                if( !AppendData( item, QueueDataSize[idx] ) )
                {
                    connectionLost = true;
                    break;
                }

                UpdateCpuRanges( rStats.stats, refThread, refCtx );
                UpdateGpuRange( rStats.stats, refGpu );

                UpdateCpuRanges( m_pBufferHandler->m_stats, refThread, refCtx );
                UpdateGpuRange( m_pBufferHandler->m_stats, refGpu );

                // NOTE: This is unfortunate, but we need to update m_refTimes here in case we initiate Synchronize()
                // Ideally we'd only do this if we actually do so
                m_refTimes.m_refTimeThread = refThread;
                m_refTimes.m_refTimeCtx = refCtx;
                m_refTimes.m_refTimeGpu = refGpu;
                UpdateSyncInfo( (QueueType)idx, item, syncTimes );
                item++;
            }

            m_refTimes = syncTimes;
        }
    );
    if( connectionLost ) return DequeueStatus::ConnectionLost;
    return sz > 0 ? DequeueStatus::DataDequeued : DequeueStatus::QueueEmpty;

#undef refThread
#undef refCtx
#undef refGpu
}

Profiler::DequeueStatus Profiler::DequeueContextSwitches( tracy::moodycamel::ConsumerToken& token, int64_t& timeStop )
{
    const auto sz = GetQueue().try_dequeue_bulk_single( token, [] ( const uint64_t& ) {},
        [this, &timeStop] ( QueueItem* item, size_t sz )
        {
            assert( sz > 0 );
            int64_t refCtx = m_refTimes.m_refTimeCtx;
            while( sz-- > 0 )
            {
                FreeAssociatedMemory( *item );
                if( timeStop < 0 ) return;
                const auto idx = MemRead<uint8_t>( &item->hdr.idx );
                if( idx == (uint8_t)QueueType::ContextSwitch )
                {
                    const auto csTime = MemRead<int64_t>( &item->contextSwitch.time );
                    if( csTime > timeStop )
                    {
                        timeStop = -1;
                        m_refTimes.m_refTimeCtx = refCtx;
                        return;
                    }
                    int64_t dt = csTime - refCtx;
                    refCtx = csTime;
                    MemWrite( &item->contextSwitch.time, dt );
                    if( !AppendData( item, QueueDataSize[(int)QueueType::ContextSwitch] ) )
                    {
                        timeStop = -2;
                        m_refTimes.m_refTimeCtx = refCtx;
                        return;
                    }
                }
                else if( idx == (uint8_t)QueueType::ThreadWakeup )
                {
                    const auto csTime = MemRead<int64_t>( &item->threadWakeup.time );
                    if( csTime > timeStop )
                    {
                        timeStop = -1;
                        m_refTimes.m_refTimeCtx = refCtx;
                        return;
                    }
                    int64_t dt = csTime - refCtx;
                    refCtx = csTime;
                    MemWrite( &item->threadWakeup.time, dt );
                    if( !AppendData( item, QueueDataSize[(int)QueueType::ThreadWakeup] ) )
                    {
                        timeStop = -2;
                        m_refTimes.m_refTimeCtx = refCtx;
                        return;
                    }
                }
                item++;
            }

            m_refTimes.m_refTimeCtx = refCtx;
        }
    );

    if( timeStop == -2 ) return DequeueStatus::ConnectionLost;
    return ( timeStop == -1 || sz > 0 ) ? DequeueStatus::DataDequeued : DequeueStatus::QueueEmpty;
}

Profiler::DequeueStatus Profiler::DequeueSerial( DequeueStats& rStats )
{
    DequeueStatus result = DequeueStatus::QueueEmpty;

#   define ThreadCtxCheckSerial( _name )                                                                \
        uint32_t thread = MemRead<uint32_t>( &item->_name.thread );                                     \
        switch( ThreadCtxCheck( thread, refTimes ) )                                                    \
        {                                                                                               \
        case ThreadCtxStatus::Same: break;                                                              \
        case ThreadCtxStatus::Changed: assert( m_refTimes.m_refTimeThread == 0 ); refThread = 0; break; \
        case ThreadCtxStatus::ConnectionLost: result = DequeueStatus::ConnectionLost; break;            \
        default: assert( false ); break;                                                                \
        }

    {
        bool lockHeld = true;
        while( !m_serialLock.try_lock() )
        {
            if( m_shutdownManual.load( std::memory_order_relaxed ) )
            {
                lockHeld = false;
                break;
            }
        }
        if( !m_serialQueue.empty() ) m_serialQueue.swap( m_serialDequeue );
        if( lockHeld )
        {
            m_serialLock.unlock();
        }
    }

    assert( m_pBufferHandler );

    const auto sz = m_serialDequeue.size();
    if( sz > 0 )
    {
        result = DequeueStatus::DataDequeued;

        InitRpmalloc();

        RefTimes syncTimes = m_refTimes;
#       define refSerial syncTimes.m_refTimeSerial
#       define refGpu syncTimes.m_refTimeGpu

#ifdef TRACY_FIBERS
#       define refThread syncTimes.m_refTimeThread
#endif

        FrameInfo lastFrame = { 0 };
        auto item = m_serialDequeue.data();
        auto end = item + sz;
        while( item != end )
        {
            uint64_t ptr;
            auto idx = MemRead<uint8_t>( &item->hdr.idx );
            if( idx < (int)QueueType::Terminate )
            {
                switch( (QueueType)idx )
                {
                case QueueType::CallstackSerial:
                    ptr = MemRead<uint64_t>( &item->callstackFat.ptr );
                    SendCallstackPayload( ptr );
                    tracy_free_fast( (void*)ptr );
                    break;
                case QueueType::LockWait:
                case QueueType::LockSharedWait:
                {
                    int64_t t = MemRead<int64_t>( &item->lockWait.time );
                    int64_t dt = t - refSerial;
                    refSerial = t;
                    MemWrite( &item->lockWait.time, dt );
                    break;
                }
                case QueueType::LockObtain:
                case QueueType::LockSharedObtain:
                {
                    int64_t t = MemRead<int64_t>( &item->lockObtain.time );
                    int64_t dt = t - refSerial;
                    refSerial = t;
                    MemWrite( &item->lockObtain.time, dt );
                    break;
                }
                case QueueType::LockRelease:
                case QueueType::LockSharedRelease:
                {
                    int64_t t = MemRead<int64_t>( &item->lockRelease.time );
                    int64_t dt = t - refSerial;
                    refSerial = t;
                    MemWrite( &item->lockRelease.time, dt );
                    break;
                }
                case QueueType::LockName:
                {
                    ptr = MemRead<uint64_t>( &item->lockNameFat.name );
                    uint16_t size = MemRead<uint16_t>( &item->lockNameFat.size );
                    SendSingleString( (const char*)ptr, size );
#ifndef TRACY_ON_DEMAND
                    tracy_free_fast( (void*)ptr );
#endif
                    break;
                }
                case QueueType::MemAlloc:
                case QueueType::MemAllocNamed:
                case QueueType::MemAllocCallstack:
                case QueueType::MemAllocCallstackNamed:
                {
                    int64_t t = MemRead<int64_t>( &item->memAlloc.time );
                    int64_t dt = t - refSerial;
                    refSerial = t;
                    MemWrite( &item->memAlloc.time, dt );
                    break;
                }
                case QueueType::MemFree:
                case QueueType::MemFreeNamed:
                case QueueType::MemFreeCallstack:
                case QueueType::MemFreeCallstackNamed:
                {
                    int64_t t = MemRead<int64_t>( &item->memFree.time );
                    int64_t dt = t - refSerial;
                    refSerial = t;
                    MemWrite( &item->memFree.time, dt );
                    break;
                }
                case QueueType::MemDiscard:
                case QueueType::MemDiscardCallstack:
                {
                    int64_t t = MemRead<int64_t>( &item->memDiscard.time );
                    int64_t dt = t - refSerial;
                    refSerial = t;
                    MemWrite( &item->memDiscard.time, dt );
                    break;
                }
                case QueueType::GpuZoneBeginSerial:
                case QueueType::GpuZoneBeginCallstackSerial:
                {
                    int64_t t = MemRead<int64_t>( &item->gpuZoneBegin.cpuTime );
                    int64_t dt = t - refSerial;
                    refSerial = t;
                    MemWrite( &item->gpuZoneBegin.cpuTime, dt );
                    break;
                }
                case QueueType::GpuZoneBeginAllocSrcLocSerial:
                case QueueType::GpuZoneBeginAllocSrcLocCallstackSerial:
                {
                    int64_t t = MemRead<int64_t>( &item->gpuZoneBegin.cpuTime );
                    int64_t dt = t - refSerial;
                    refSerial = t;
                    MemWrite( &item->gpuZoneBegin.cpuTime, dt );
                    ptr = MemRead<uint64_t>( &item->gpuZoneBegin.srcloc );
                    SendSourceLocationPayload( ptr );
                    tracy_free_fast( (void*)ptr );
                    break;
                }
                case QueueType::GpuZoneEndSerial:
                {
                    int64_t t = MemRead<int64_t>( &item->gpuZoneEnd.cpuTime );
                    int64_t dt = t - refSerial;
                    refSerial = t;
                    MemWrite( &item->gpuZoneEnd.cpuTime, dt );
                    break;
                }
                case QueueType::GpuTime:
                {
                    int64_t t = MemRead<int64_t>( &item->gpuTime.gpuTime );
                    int64_t dt = t - refGpu;
                    refGpu = t;
                    MemWrite( &item->gpuTime.gpuTime, dt );
                    break;
                }
                case QueueType::GpuContextName:
                {
                    ptr = MemRead<uint64_t>( &item->gpuContextNameFat.ptr );
                    uint16_t size = MemRead<uint16_t>( &item->gpuContextNameFat.size );
                    SendSingleString( (const char*)ptr, size );
#ifndef TRACY_ON_DEMAND
                    tracy_free_fast( (void*)ptr );
#endif
                    break;
                }
                case QueueType::HwCounterConfig:
                {
                    const char *counterName = ( const char * ) MemRead<uint64_t>( &item->hwCounterConfig.name );
                    const char *counterDesc = ( const char * ) MemRead<uint64_t>( &item->hwCounterConfig.description );
                    SendSingleString( counterName );
                    SendSecondString( counterDesc );
                    break;
                }
#ifdef TRACY_FIBERS
                case QueueType::ZoneBegin:
                case QueueType::ZoneBeginCallstack:
                {
                    ThreadCtxCheckSerial( zoneBeginThread );
                    int64_t t = MemRead<int64_t>( &item->zoneBegin.time );
                    int64_t dt = t - refThread;
                    refThread = t;
                    MemWrite( &item->zoneBegin.time, dt );
                    break;
                }
                case QueueType::ZoneBeginAllocSrcLoc:
                case QueueType::ZoneBeginAllocSrcLocCallstack:
                {
                    ThreadCtxCheckSerial( zoneBeginThread );
                    int64_t t = MemRead<int64_t>( &item->zoneBegin.time );
                    int64_t dt = t - refThread;
                    refThread = t;
                    MemWrite( &item->zoneBegin.time, dt );
                    ptr = MemRead<uint64_t>( &item->zoneBegin.srcloc );
                    SendSourceLocationPayload( ptr );
                    tracy_free_fast( (void*)ptr );
                    break;
                }
                case QueueType::ZoneEnd:
                {
                    ThreadCtxCheckSerial( zoneEndThread );
                    int64_t t = MemRead<int64_t>( &item->zoneEnd.time );
                    int64_t dt = t - refThread;
                    refThread = t;
                    MemWrite( &item->zoneEnd.time, dt );
                    break;
                }
                case QueueType::ZoneText:
                case QueueType::ZoneName:
                {
                    ThreadCtxCheckSerial( zoneTextFatThread );
                    ptr = MemRead<uint64_t>( &item->zoneTextFat.text );
                    uint16_t size = MemRead<uint16_t>( &item->zoneTextFat.size );
                    SendSingleString( (const char*)ptr, size );
                    tracy_free_fast( (void*)ptr );
                    break;
                }
                case QueueType::Message:
                case QueueType::MessageCallstack:
                {
                    ThreadCtxCheckSerial( messageFatThread );
                    ptr = MemRead<uint64_t>( &item->messageFat.text );
                    uint16_t size = MemRead<uint16_t>( &item->messageFat.size );
                    SendSingleString( (const char*)ptr, size );
                    tracy_free_fast( (void*)ptr );
                    break;
                }
                case QueueType::MessageColor:
                case QueueType::MessageColorCallstack:
                {
                    ThreadCtxCheckSerial( messageColorFatThread );
                    ptr = MemRead<uint64_t>( &item->messageColorFat.text );
                    uint16_t size = MemRead<uint16_t>( &item->messageColorFat.size );
                    SendSingleString( (const char*)ptr, size );
                    tracy_free_fast( (void*)ptr );
                    break;
                }
                case QueueType::Callstack:
                {
                    ThreadCtxCheckSerial( callstackFatThread );
                    ptr = MemRead<uint64_t>( &item->callstackFat.ptr );
                    SendCallstackPayload( ptr );
                    tracy_free_fast( (void*)ptr );
                    break;
                }
                case QueueType::CallstackAlloc:
                {
                    ThreadCtxCheckSerial( callstackAllocFatThread );
                    ptr = MemRead<uint64_t>( &item->callstackAllocFat.nativePtr );
                    if( ptr != 0 )
                    {
                        CutCallstack( (void*)ptr, "lua_pcall" );
                        SendCallstackPayload( ptr );
                        tracy_free_fast( (void*)ptr );
                    }
                    ptr = MemRead<uint64_t>( &item->callstackAllocFat.ptr );
                    SendCallstackAlloc( ptr );
                    tracy_free_fast( (void*)ptr );
                    break;
                }
                case QueueType::FiberEnter:
                {
                    ThreadCtxCheckSerial( fiberEnter );
                    int64_t t = MemRead<int64_t>( &item->fiberEnter.time );
                    int64_t dt = t - refThread;
                    refThread = t;
                    MemWrite( &item->fiberEnter.time, dt );
                    break;
                }
                case QueueType::FiberLeave:
                {
                    ThreadCtxCheckSerial( fiberLeave );
                    int64_t t = MemRead<int64_t>( &item->fiberLeave.time );
                    int64_t dt = t - refThread;
                    refThread = t;
                    MemWrite( &item->fiberLeave.time, dt );
                    break;
                }
#endif
                default:
                    assert( false );
                    break;
                }
            }
            else
            {
                switch( (QueueType)idx )
                {
                    case QueueType::FrameMarkMsg:
                    {
                        lastFrame.id = MemRead<uint64_t>( &item->frameMark.id );
                        lastFrame.time = MemRead<uint64_t>( &item->frameMark.time );

                        if ( rStats.pFrameHistory )
                        {
                            if ( rStats.pFrameHistory->first.time == 0 )
                            {
                                rStats.pFrameHistory->first = lastFrame;
                            }

                            if ( rStats.pFrameHistory->last.time == 0 )
                            {
                                rStats.pFrameHistory->last = lastFrame;
                            }

                            rStats.pFrameHistory->first.id = std::min( rStats.pFrameHistory->first.id, lastFrame.id );
                            rStats.pFrameHistory->first.time = std::min( rStats.pFrameHistory->first.time, lastFrame.time );

                            rStats.pFrameHistory->last.id = std::max( rStats.pFrameHistory->last.id, lastFrame.id );
                            rStats.pFrameHistory->last.time = std::max( rStats.pFrameHistory->last.time, lastFrame.time );

                            size_t frameIndex = ( rStats.pFrameHistory->count % rStats.pFrameHistory->size );
                            rStats.pFrameHistory->pBuffer[ frameIndex ] = lastFrame;
                            rStats.pFrameHistory->count++;
                        }
                    } break;

#ifdef TRACY_FIBERS
                case QueueType::ZoneColor:
                {
                    ThreadCtxCheckSerial( zoneColorThread );
                    break;
                }
                case QueueType::ZoneValue:
                {
                    ThreadCtxCheckSerial( zoneValueThread );
                    break;
                }
                case QueueType::ZoneValidation:
                {
                    ThreadCtxCheckSerial( zoneValidationThread );
                    break;
                }
                case QueueType::MessageLiteral:
                case QueueType::MessageLiteralCallstack:
                {
                    ThreadCtxCheckSerial( messageLiteralThread );
                    break;
                }
                case QueueType::MessageLiteralColor:
                case QueueType::MessageLiteralColorCallstack:
                {
                    ThreadCtxCheckSerial( messageColorLiteralThread );
                    break;
                }
                case QueueType::CrashReport:
                {
                    ThreadCtxCheckSerial( crashReportThread );
                    break;
                }
#endif // ifdef TRACY_FIBERS

                default:
                    break;
                }
            }
            if ( (result == DequeueStatus::ConnectionLost) || !AppendData( item, QueueDataSize[ idx ] ) )
            {
                result = DequeueStatus::ConnectionLost;
                break;
            }

            UpdateCpuRanges( rStats.stats, lastFrame.time, refSerial );
            UpdateGpuRange( rStats.stats, refGpu );
            if ( rStats.stats.firstFrame.time == 0 )
            {
                rStats.stats.firstFrame = lastFrame;
            }
            if ( lastFrame.time != 0 )
            {
                rStats.stats.lastFrame = lastFrame;
            }

            UpdateCpuRanges( m_pBufferHandler->m_stats, lastFrame.time, refSerial );
            UpdateGpuRange( m_pBufferHandler->m_stats, refGpu );
            if ( m_pBufferHandler->m_stats.firstFrame.time == 0 )
            {
                m_pBufferHandler->m_stats.firstFrame = lastFrame;
            }
            if ( lastFrame.time != 0 )
            {
                m_pBufferHandler->m_stats.lastFrame = lastFrame;
            }

            // NOTE: This is unfortunate, but we need to update m_refTimes here in case we initiate Synchronize()
            // Ideally we'd only do this if we actually do so
            m_refTimes.m_refTimeSerial = refSerial;
            m_refTimes.m_refTimeGpu = refGpu;

#ifdef TRACY_FIBERS
            m_refTimes.m_refTimeThread = refThread;
#endif
            UpdateSyncInfo( (QueueType)idx, item, syncTimes );
            item++;
        }

        m_refTimes = syncTimes;
        m_serialDequeue.clear();
    }

    return result;

#undef refSerial
#undef refGpu

#ifdef TRACY_FIBERS
#undef refThread
#endif

#   undef ThreadCtxCheckSerial
}


Profiler::DequeueStatus Profiler::DequeueSys( DequeueStats& rStats )
{
    DequeueStatus result = DequeueStatus::QueueEmpty;

#ifdef TRACY_HAS_SYSTEM_TRACING
    {
        bool lockHeld = true;
        while( !m_sysLock.try_lock() )
        {
            if( m_shutdownManual.load( std::memory_order_relaxed ) )
            {
                lockHeld = false;
                break;
            }
        }

        if( !m_sysQueue.empty() )
        {
            m_sysQueue.swap( m_sysDequeue );
        }

        if( lockHeld )
        {
            m_sysLock.unlock();
        }
    }

    bool connectionLost = false;
    assert( m_pBufferHandler );
    const auto sz = m_sysDequeue.size();
    if( sz > 0 )
    {
        result = DequeueStatus::DataDequeued;

        RefTimes syncTimes = m_refTimes;
#       define refCtx syncTimes.m_refTimeCtx

        InitRpmalloc();

        auto item = m_sysDequeue.data();
        auto end = item + sz;
        while( item != end )
        {
            uint64_t ptr;

            uint8_t idx = MemRead<uint8_t>( &item->hdr.idx );
            assert( idx < (int)QueueType::NUM_TYPES );
            switch( (QueueType)idx )
            {
                case QueueType::ContextSwitch:
                {
                    int64_t t = MemRead<int64_t>( &item->contextSwitch.time );
                    int64_t dt = t - refCtx;
                    refCtx = t;
                    MemWrite( &item->contextSwitch.time, dt );
                } break;

                case QueueType::ThreadWakeup:
                {
                    int64_t t = MemRead<int64_t>( &item->threadWakeup.time );
                    int64_t dt = t - refCtx;
                    refCtx = t;
                    MemWrite( &item->threadWakeup.time, dt );
                } break;

                case QueueType::CallstackSample:
                case QueueType::CallstackSampleContextSwitch:
                {
                    ptr = MemRead<uint64_t>( &item->callstackSampleFat.ptr );
                    SendCallstackPayload64( ptr );
                    tracy_free_fast( (void*)ptr );
                    int64_t t = MemRead<int64_t>( &item->callstackSampleFat.time );
                    int64_t dt = t - refCtx;
                    refCtx = t;
                    MemWrite( &item->callstackSampleFat.time, dt );
                } break;
            }

            if( !AppendData( item, QueueDataSize[idx] ) )
            {
                result = DequeueStatus::ConnectionLost;
                break;
            }

            UpdateCpuRange( rStats.stats, refCtx );

            UpdateCpuRange( m_pBufferHandler->m_stats, refCtx );

            // NOTE: This is unfortunate, but we need to update m_refTimes here in case we initiate Synchronize()
            // Ideally we'd only do this if we actually do so
            m_refTimes.m_refTimeCtx = refCtx;
            UpdateSyncInfo( (QueueType)idx, item, syncTimes );
            item++;
        }

        m_refTimes = syncTimes;
        m_sysDequeue.clear();
    }
#endif // ifdef TRACY_HAS_SYSTEM_TRACING

#undef refCtx

    return result;
}

Profiler::DequeueStatus Profiler::DequeueSymbols()
{
    DequeueStatus result = DequeueStatus::QueueEmpty;

#ifdef TRACY_NEEDS_SYMBOL_WORKER
    {
        bool lockHeld = true;
        while( !m_symbolLock.try_lock() )
        {
            if( m_shutdownManual.load( std::memory_order_relaxed ) )
            {
                lockHeld = false;
                break;
            }
        }

        if( !m_symbolQueue.empty() )
        {
            m_symbolQueue.swap( m_symbolDequeue );
        }

        if( lockHeld )
        {
            m_symbolLock.unlock();
        }
    }

    const auto sz = m_symbolDequeue.size();
    if( sz > 0 )
    {
        result = DequeueStatus::DataDequeued;
        InitRpmalloc();
        auto item = m_symbolDequeue.data();
        auto end = item + sz;
        while( item != end )
        {
            auto idx = MemRead<uint8_t>( &item->hdr.idx );
            switch( (QueueType)idx )
            {
#ifdef TRACY_HAS_CALLSTACK
                case QueueType::CallstackFrameSize:
                {
                    auto data = (const CallstackEntry*)MemRead<uint64_t>( &item->callstackFrameSizeFat.data );
                    auto datasz = MemRead<uint8_t>( &item->callstackFrameSizeFat.size );
                    auto imageName = (const char*)MemRead<uint64_t>( &item->callstackFrameSizeFat.imageName );
                    SendSingleString( imageName );
                    AppendData( item++, QueueDataSize[idx] );

                    for( uint8_t i=0; i<datasz; i++ )
                    {
                        const auto& frame = data[i];

                        SendSingleString( frame.name );
                        SendSecondString( frame.file );

                        QueueItem item;
                        MemWrite( &item.hdr.type, QueueType::CallstackFrame );
                        MemWrite( &item.callstackFrame.line, frame.line );
                        MemWrite( &item.callstackFrame.symAddr, frame.symAddr );
                        MemWrite( &item.callstackFrame.symLen, frame.symLen );

                        AppendData( &item, QueueDataSize[(int)QueueType::CallstackFrame] );

                        tracy_free_fast( (void*)frame.name );
                        tracy_free_fast( (void*)frame.file );
                    }
                    tracy_free_fast( (void*)data );
                    continue;
                } break;

                case QueueType::SymbolInformation:
                {
                    auto fileString = (const char*)MemRead<uint64_t>( &item->symbolInformationFat.fileString );
                    auto needFree = MemRead<uint8_t>( &item->symbolInformationFat.needFree );
                    SendSingleString( fileString );
                    if( needFree )
                    {
                        tracy_free_fast( (void*)fileString );
                    }
                } break;

                case QueueType::SymbolCodeMetadata:
                {
                    auto symbol = MemRead<uint64_t>( &item->symbolCodeMetadata.symbol );
                    auto ptr = (const char*)MemRead<uint64_t>( &item->symbolCodeMetadata.ptr );
                    auto size = MemRead<uint32_t>( &item->symbolCodeMetadata.size );
                    SendLongString( symbol, ptr, size, QueueType::SymbolCode );
                    tracy_free_fast( (void*)ptr );
                    ++item;
                    continue;
                } break;
#endif // TRACY_HAS_CALLSTACK

#ifdef TRACY_HAS_SYSTEM_TRACING
                case QueueType::ExternalNameMetadata:
                {
                    auto thread = MemRead<uint64_t>( &item->externalNameMetadata.thread );
                    auto name = (const char*)MemRead<uint64_t>( &item->externalNameMetadata.name );
                    auto threadName = (const char*)MemRead<uint64_t>( &item->externalNameMetadata.threadName );
                    SendString( thread, threadName, QueueType::ExternalThreadName );
                    SendString( thread, name, QueueType::ExternalName );
                    tracy_free_fast( (void*)threadName );
                    tracy_free_fast( (void*)name );
                    ++item;
                    continue;
                } break;
#endif // TRACY_HAS_SYSTEM_TRACING

                case QueueType::SourceCodeMetadata:
                {
                    auto ptr = (const char*)MemRead<uint64_t>( &item->sourceCodeMetadata.ptr );
                    auto size = MemRead<uint32_t>( &item->sourceCodeMetadata.size );
                    auto id = MemRead<uint32_t>( &item->sourceCodeMetadata.id );
                    SendLongString( (uint64_t)id, ptr, size, QueueType::SourceCode );
                    tracy_free_fast( (void*)ptr );
                    ++item;
                    continue;
                } break;

                default:
                {
                } break;
            }

            if( !AppendData( item, QueueDataSize[idx] ) )
            {
                result = DequeueStatus::ConnectionLost;
                break;
            }
            item++;
        }

        m_symbolDequeue.clear();
    }

#endif // ifdef TRACY_NEEDS_SYMBOL_WORKER

    return result;
}

Profiler::ThreadCtxStatus Profiler::ThreadCtxCheck( uint32_t threadId, RefTimes& rRefTimes )
{
    if( m_refTimes.m_threadCtx == threadId ) return ThreadCtxStatus::Same;
    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::ThreadContext );
    MemWrite( &item.threadCtx.thread, threadId );
    if( !AppendData( &item, QueueDataSize[(int)QueueType::ThreadContext] ) ) return ThreadCtxStatus::ConnectionLost;
    m_refTimes.m_threadCtx = threadId;
    m_refTimes.m_refTimeThread = 0;
    return ThreadCtxStatus::Changed;
}

bool Profiler::CommitData( size_t pendingSize )
{
    assert( m_pBufferHandler );
    bool result = m_pBufferHandler->Commit( pendingSize );
    return result;
}

bool Profiler::AppendData( const QueueItem *item, size_t len )
{
    const bool result = NeedDataSize( len );
    if ( result )
    {
        AppendDataUnsafe( item, len );
    }
    return result;
}

bool Profiler::NeedDataSize( size_t len )
{
    assert( m_pBufferHandler );
    bool result = true;
    if( m_pBufferHandler->m_buffer.offset - m_pBufferHandler->m_buffer.start + len > m_pBufferHandler->m_buffer.commitLimit )
    {
        result = CommitData( len );
    }

    return result;
}

void Profiler::AppendDataUnsafe( const void *data, size_t len )
{
    assert( m_pBufferHandler );
    assert( (m_pBufferHandler->m_buffer.offset + len) <= m_pBufferHandler->m_buffer.size );
    memcpy( m_pBufferHandler->m_buffer.pBuffer + m_pBufferHandler->m_buffer.offset, data, len );
    m_pBufferHandler->m_buffer.offset += len;
}

char* Profiler::SafeCopyProlog( const char* data, size_t size )
{
    bool success = true;
    char* buf = m_safeSendBuffer;
#ifndef NDEBUG
    assert( !m_inUse.exchange(true) );
#endif

    if( size > SafeSendBufferSize ) buf = (char*)tracy_malloc( size );

#ifdef _WIN32
    __try
    {
        memcpy( buf, data, size );
    }
    __except( 1 /*EXCEPTION_EXECUTE_HANDLER*/ )
    {
        success = false;
    }
#else
    // Send through the pipe to ensure safe reads
    for( size_t offset = 0; offset != size; /*in loop*/ )
    {
        size_t sendsize = size - offset;
        ssize_t result1, result2;
        while( ( result1 = write( m_pipe[1], data + offset, sendsize ) ) < 0 && errno == EINTR ) { /* retry */ }
        if( result1 < 0 )
        {
            success = false;
            break;
        }
        while( ( result2 = read( m_pipe[0], buf + offset, result1 ) ) < 0 && errno == EINTR ) { /* retry */ }
        if( result2 != result1 )
        {
            success = false;
            break;
        }
        offset += result1;
    }
#endif

    if( success ) return buf;

    SafeCopyEpilog( buf );
    return nullptr;
}

void Profiler::SafeCopyEpilog( char* buf )
{
    if( buf != m_safeSendBuffer ) tracy_free( buf );

#ifndef NDEBUG
    m_inUse.store( false );
#endif
}

bool Profiler::SendData( const char* data, size_t len )
{
    bool result = false;
    if ( m_pUiConnection )
    {
        result = m_pUiConnection->SendCompressed( data, ( int ) len ) != -1;
    }
    return result;
}

void Profiler::SendString( uint64_t str, const char* ptr, size_t len, QueueType type )
{
    assert( type == QueueType::StringData ||
            type == QueueType::ThreadName ||
            type == QueueType::PlotName ||
            type == QueueType::FrameName ||
            type == QueueType::ExternalName ||
            type == QueueType::ExternalThreadName ||
            type == QueueType::FiberName );

    QueueItem item;
    MemWrite( &item.hdr.type, type );
    MemWrite( &item.stringTransfer.ptr, str );

    assert( len <= std::numeric_limits<uint16_t>::max() );
    auto l16 = uint16_t( len );

    NeedDataSize( QueueDataSize[(int)type] + sizeof( l16 ) + l16 );

    AppendDataUnsafe( &item, QueueDataSize[(int)type] );
    AppendDataUnsafe( &l16, sizeof( l16 ) );
    AppendDataUnsafe( ptr, l16 );
}

void Profiler::SendSingleString( const char* ptr, size_t len )
{
    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::SingleStringData );

    assert( len <= std::numeric_limits<uint16_t>::max() );
    auto l16 = uint16_t( len );

    NeedDataSize( QueueDataSize[(int)QueueType::SingleStringData] + sizeof( l16 ) + l16 );

    AppendDataUnsafe( &item, QueueDataSize[(int)QueueType::SingleStringData] );
    AppendDataUnsafe( &l16, sizeof( l16 ) );
    AppendDataUnsafe( ptr, l16 );
}

void Profiler::SendSecondString( const char* ptr, size_t len )
{
    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::SecondStringData );

    assert( len <= std::numeric_limits<uint16_t>::max() );
    auto l16 = uint16_t( len );

    NeedDataSize( QueueDataSize[(int)QueueType::SecondStringData] + sizeof( l16 ) + l16 );

    AppendDataUnsafe( &item, QueueDataSize[(int)QueueType::SecondStringData] );
    AppendDataUnsafe( &l16, sizeof( l16 ) );
    AppendDataUnsafe( ptr, l16 );
}

void Profiler::SendLongString( uint64_t str, const char* ptr, size_t len, QueueType type )
{
    assert( type == QueueType::FrameImageData ||
            type == QueueType::SymbolCode ||
            type == QueueType::SourceCode );

    QueueItem item;
    MemWrite( &item.hdr.type, type );
    MemWrite( &item.stringTransfer.ptr, str );

    assert( len <= std::numeric_limits<uint32_t>::max() );
    assert( QueueDataSize[(int)type] + sizeof( uint32_t ) + len <= TargetFrameSize );
    auto l32 = uint32_t( len );

    NeedDataSize( QueueDataSize[(int)type] + sizeof( l32 ) + l32 );

    AppendDataUnsafe( &item, QueueDataSize[(int)type] );
    AppendDataUnsafe( &l32, sizeof( l32 ) );
    AppendDataUnsafe( ptr, l32 );
}

void Profiler::SendSourceLocation( uint64_t ptr )
{
    auto srcloc = (const SourceLocationData*)ptr;
    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::SourceLocation );
    MemWrite( &item.srcloc.name, (uint64_t)srcloc->name );
    MemWrite( &item.srcloc.file, (uint64_t)srcloc->file );
    MemWrite( &item.srcloc.function, (uint64_t)srcloc->function );
    MemWrite( &item.srcloc.line, srcloc->line );
    MemWrite( &item.srcloc.b, uint8_t( ( srcloc->color       ) & 0xFF ) );
    MemWrite( &item.srcloc.g, uint8_t( ( srcloc->color >> 8  ) & 0xFF ) );
    MemWrite( &item.srcloc.r, uint8_t( ( srcloc->color >> 16 ) & 0xFF ) );
    AppendData( &item, QueueDataSize[(int)QueueType::SourceLocation] );
}

void Profiler::SendSourceLocationPayload( uint64_t _ptr )
{
    auto ptr = (const char*)_ptr;

    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::SourceLocationPayload );
    MemWrite( &item.stringTransfer.ptr, _ptr );

    uint16_t len;
    memcpy( &len, ptr, sizeof( len ) );
    assert( len > 2 );
    len -= 2;
    ptr += 2;

    NeedDataSize( QueueDataSize[(int)QueueType::SourceLocationPayload] + sizeof( len ) + len );

    AppendDataUnsafe( &item, QueueDataSize[(int)QueueType::SourceLocationPayload] );
    AppendDataUnsafe( &len, sizeof( len ) );
    AppendDataUnsafe( ptr, len );
}

void Profiler::SendCallstackPayload( uint64_t _ptr )
{
    auto ptr = (uintptr_t*)_ptr;

    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::CallstackPayload );
    MemWrite( &item.stringTransfer.ptr, _ptr );

    const auto sz = *ptr++;
    const auto len = sz * sizeof( uint64_t );
    const auto l16 = uint16_t( len );

    NeedDataSize( QueueDataSize[(int)QueueType::CallstackPayload] + sizeof( l16 ) + l16 );

    AppendDataUnsafe( &item, QueueDataSize[(int)QueueType::CallstackPayload] );
    AppendDataUnsafe( &l16, sizeof( l16 ) );

    if( compile_time_condition<sizeof( uintptr_t ) == sizeof( uint64_t )>::value )
    {
        AppendDataUnsafe( ptr, sizeof( uint64_t ) * sz );
    }
    else
    {
        for( uintptr_t i=0; i<sz; i++ )
        {
            const auto val = uint64_t( *ptr++ );
            AppendDataUnsafe( &val, sizeof( uint64_t ) );
        }
    }
}

void Profiler::SendCallstackPayload64( uint64_t _ptr )
{
    auto ptr = (uint64_t*)_ptr;

    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::CallstackPayload );
    MemWrite( &item.stringTransfer.ptr, _ptr );

    const auto sz = *ptr++;
    const auto len = sz * sizeof( uint64_t );
    const auto l16 = uint16_t( len );

    NeedDataSize( QueueDataSize[(int)QueueType::CallstackPayload] + sizeof( l16 ) + l16 );

    AppendDataUnsafe( &item, QueueDataSize[(int)QueueType::CallstackPayload] );
    AppendDataUnsafe( &l16, sizeof( l16 ) );
    AppendDataUnsafe( ptr, sizeof( uint64_t ) * sz );
}

void Profiler::SendCallstackAlloc( uint64_t _ptr )
{
    auto ptr = (const char*)_ptr;

    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::CallstackAllocPayload );
    MemWrite( &item.stringTransfer.ptr, _ptr );

    uint16_t len;
    memcpy( &len, ptr, 2 );
    ptr += 2;

    NeedDataSize( QueueDataSize[(int)QueueType::CallstackAllocPayload] + sizeof( len ) + len );

    AppendDataUnsafe( &item, QueueDataSize[(int)QueueType::CallstackAllocPayload] );
    AppendDataUnsafe( &len, sizeof( len ) );
    AppendDataUnsafe( ptr, len );
}

void Profiler::QueueCallstackFrame( uint64_t ptr )
{
#ifdef TRACY_HAS_CALLSTACK
    if ( s_symbolThreadRunning.load( std::memory_order_acquire ) )
    {
        m_requestSymbolLock.lock();
        SymbolQueueItem* item = m_symbolRequestQueue.push_next();
        *item = SymbolQueueItem{ ConnectionId(), SymbolQueueItemType::CallstackFrame, ptr };
        m_requestSymbolLock.unlock();
        return;
    }
#endif

    AckServerQuery();
}

void Profiler::QueueSymbolQuery( uint64_t symbol )
{
#ifdef TRACY_HAS_CALLSTACK
    if ( s_symbolThreadRunning.load( std::memory_order_acquire ) )
    {
        // Special handling for kernel frames
        if ( symbol >> 63 != 0 )
        {
            SendSingleString( "<kernel>" );
            QueueItem item;
            MemWrite( &item.hdr.type, QueueType::SymbolInformation );
            MemWrite( &item.symbolInformation.line, 0 );
            MemWrite( &item.symbolInformation.symAddr, symbol );
            AppendData( &item, QueueDataSize[ ( int ) QueueType::SymbolInformation ] );
        }
        else
        {
            m_requestSymbolLock.lock();
            SymbolQueueItem* item = m_symbolRequestQueue.push_next();
            *item = SymbolQueueItem{ ConnectionId(), SymbolQueueItemType::SymbolQuery, symbol };
            m_requestSymbolLock.unlock();
        }
        return;
    }
#endif

    AckServerQuery();
}

void Profiler::QueueExternalName( uint64_t ptr )
{
#ifdef TRACY_HAS_SYSTEM_TRACING
    if ( s_symbolThreadRunning.load( std::memory_order_acquire ) )
    {
        m_requestSymbolLock.lock();
        SymbolQueueItem* item = m_symbolRequestQueue.push_next();
        *item = SymbolQueueItem { ConnectionId(), SymbolQueueItemType::ExternalName, ptr };
        m_requestSymbolLock.unlock();
        return;
    }
#endif

    static const char* pEmpty = "";
    SendString( ptr, pEmpty, QueueType::ExternalThreadName );
    SendString( ptr, pEmpty, QueueType::ExternalName );
}

void Profiler::QueueKernelCode( uint64_t symbol, uint32_t size )
{
    assert( symbol >> 63 != 0 );
#ifdef TRACY_HAS_CALLSTACK
    if ( s_symbolThreadRunning.load( std::memory_order_acquire ) )
    {
        m_requestSymbolLock.lock();
        SymbolQueueItem* item = m_symbolRequestQueue.push_next();
        *item = SymbolQueueItem { ConnectionId(), SymbolQueueItemType::KernelCode, symbol, size };
        m_requestSymbolLock.unlock();
        return;
    }
#endif

    AckSymbolCodeNotAvailable();
}

void Profiler::QueueSourceCodeQuery( uint32_t id )
{
#if defined(TRACY_NEEDS_SYMBOL_WORKER)
    if ( s_symbolThreadRunning.load( std::memory_order_acquire ) )
    {
        assert( m_exectime != 0 );
        assert( m_queryData );
        m_requestSymbolLock.lock();
        SymbolQueueItem* item = m_symbolRequestQueue.push_next();
        *item = SymbolQueueItem { ConnectionId(), SymbolQueueItemType::SourceCode, uint64_t( m_queryData ), uint64_t( m_queryImage ), id };
        m_requestSymbolLock.unlock();
        m_queryData = nullptr;
        m_queryImage = nullptr;
        return;
    }
#endif

    QueueItem item;
    MemWrite( &item.hdr.type, QueueType::AckSourceCodeNotAvailable );
    MemWrite( &item.sourceCodeNotAvailable.id, id );
    NeedDataSize( QueueDataSize[(int)QueueType::AckSourceCodeNotAvailable] );
    AppendDataUnsafe( &item, QueueDataSize[(int)QueueType::AckSourceCodeNotAvailable] );
}

#ifdef TRACY_NEEDS_SYMBOL_WORKER
void Profiler::HandleSymbolQueueItem( const SymbolQueueItem& si )
{
    switch( si.type )
    {
#ifdef TRACY_HAS_CALLSTACK
    case SymbolQueueItemType::CallstackFrame:
    {
        const auto frameData = DecodeCallstackPtr( si.ptr );
        auto data = tracy_malloc_fast( sizeof( CallstackEntry ) * frameData.size );
        memcpy( data, frameData.data, sizeof( CallstackEntry ) * frameData.size );
        QueueItem* item = QueueSymbol( QueueType::CallstackFrameSize );
        MemWrite( &item->callstackFrameSizeFat.ptr, si.ptr );
        MemWrite( &item->callstackFrameSizeFat.size, frameData.size );
        MemWrite( &item->callstackFrameSizeFat.data, (uint64_t)data );
        MemWrite( &item->callstackFrameSizeFat.imageName, (uint64_t)frameData.imageName );
        QueueSymbolFinish();
        break;
    }
    case SymbolQueueItemType::SymbolQuery:
    {
#ifdef __ANDROID__
        // On Android it's common for code to be in mappings that are only executable
        // but not readable.
        if( !EnsureReadable( si.ptr ) )
        {
            QueueItem* item = QueueSymbol( QueueType::AckServerQueryNoop );
            QueueSymbolFinish();
            break;
        }
#endif
        const auto sym = DecodeSymbolAddress( si.ptr );
        QueueItem* item = QueueSymbol( QueueType::SymbolInformation );
        MemWrite( &item->symbolInformationFat.line, sym.line );
        MemWrite( &item->symbolInformationFat.symAddr, si.ptr );
        MemWrite( &item->symbolInformationFat.fileString, (uint64_t)sym.file );
        MemWrite( &item->symbolInformationFat.needFree, (uint8_t)sym.needFree );
        QueueSymbolFinish();
        break;
    }
#endif

#ifdef TRACY_HAS_SYSTEM_TRACING
    case SymbolQueueItemType::ExternalName:
    {
        const char* threadName;
        const char* name;
        SysTraceGetExternalName( si.ptr, threadName, name );
        QueueItem* item = QueueSymbol( QueueType::ExternalNameMetadata );
        MemWrite( &item->externalNameMetadata.thread, si.ptr );
        MemWrite( &item->externalNameMetadata.name, (uint64_t)name );
        MemWrite( &item->externalNameMetadata.threadName, (uint64_t)threadName );
        QueueSymbolFinish();
        break;
    }
#endif

#ifdef TRACY_HAS_CALLSTACK
    case SymbolQueueItemType::KernelCode:
    {
#ifdef _WIN32
        const void* code = GetKernelCode( si.ptr, (uint32_t)si.extra );
        if ( code )
        {
            QueueItem* item = QueueSymbol( QueueType::SymbolCodeMetadata );
            MemWrite( &item->symbolCodeMetadata.symbol, si.ptr );
            MemWrite( &item->symbolCodeMetadata.ptr, (uint64_t)code );
            MemWrite( &item->symbolCodeMetadata.size, (uint32_t)si.extra );
            QueueSymbolFinish();
            break;
        }
#elif defined __linux__
        void* data = m_kcore->Retrieve( si.ptr, si.extra );
        if( data )
        {
            QueueItem* item = QueueSymbol( QueueType::SymbolCodeMetadata );
            MemWrite( &item->symbolCodeMetadata.symbol, si.ptr );
            MemWrite( &item->symbolCodeMetadata.ptr, (uint64_t)data );
            MemWrite( &item->symbolCodeMetadata.size, (uint32_t)si.extra );
            QueueSymbolFinish();
            break;
        }
#endif
        QueueItem* item = QueueSymbol( QueueType::AckSymbolCodeNotAvailable );
        QueueSymbolFinish();
        break;
    }
#endif // ifdef TRACY_HAS_CALLSTACK

    case SymbolQueueItemType::SourceCode:
        HandleSourceCodeQuery( (char*)si.ptr, (char*)si.extra, si.id );
        break;
    default:
        assert( false );
        break;
    }
}

void Profiler::SymbolWorker()
{
#if defined __linux__ && !defined TRACY_NO_CRASH_HANDLER
    s_symbolTid = syscall( SYS_gettid );
#endif

    ThreadExitHandler threadExitHandler;
    SetThreadName( "Tracy Symbol Worker" );
#ifdef TRACY_USE_RPMALLOC
    InitRpmalloc();
#endif

#ifdef TRACY_HAS_CALLSTACK
    InitCallstack( s_pDbgHelpLoader );
#endif

    while( m_timeBegin.load( std::memory_order_relaxed ) == 0 ) std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );

    // NOTE(sev): set the thread running only after we've initialized the callstack which can take ages
    s_symbolThreadRunning.store( true, std::memory_order_release );

    FastVector<SymbolQueueItem> requestQueue( 10 * 1024 );

    for(;;)
    {
#ifdef TRACY_ON_DEMAND
        if( !IsConnected() || m_cancelSymbolProcessing.exchange( false ) )
        {
            if( ShouldExit() )
            {
                break;
            }
            m_requestSymbolLock.lock();
            m_symbolRequestQueue.clear();
            m_requestSymbolLock.unlock();

            std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
            continue;
        }
#endif // ifdef TRACY_ON_DEMAND

        if ( !m_cancelSymbolProcessing.exchange( false ) )
        {
            if ( m_requestSymbolLock.try_lock() )
            {
                requestQueue.swap( m_symbolRequestQueue );
                m_requestSymbolLock.unlock();
            }

            if ( !requestQueue.empty() )
            {
                ProcessSymbolQueueItems( requestQueue.data(), requestQueue.size() );
                requestQueue.clear();
            }
            else if ( !ShouldExit() )
            {
                std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
            }
        }

        if( ShouldExit() )
        {
            break;
        }
    }

#if defined( TRACY_HAS_CALLSTACK ) && !TRACY_PROCESS_REMAINING_QUERIES
    EndCallstack();
#endif

    m_requestSymbolLock.lock();
    m_symbolRequestQueue.clear();
    m_requestSymbolLock.unlock();
    m_cancelSymbolProcessing.store( false );

    s_symbolThreadRunning.store( false, std::memory_order_release );
}

void Profiler::ProcessSymbolQueueItems( const SymbolQueueItem *symbolRequests, size_t count )
{
    for ( size_t index = 0; (index < count) && !ShouldExit() && !m_cancelSymbolProcessing.exchange( false ); index++ )
    {
        const SymbolQueueItem& si = symbolRequests[ index ];
        if ( si.connectionId == ConnectionId() )
        {
            HandleSymbolQueueItem( si );
        }
    }
}
#endif // if TRACY_NEEDS_SYMBOL_WORKER

bool Profiler::ProcessServerQuery()
{
    assert( m_pUiConnection );
    bool connActive = m_pUiConnection->IsValid();
    if (connActive)
    {
        while( m_pUiConnection->HasData() && !ShouldExit() )
        {
            connActive = HandleServerQuery();
            if( !connActive )
            {
                break;
            }
        }
    }
    return connActive;
}


bool Profiler::HandleServerQuery()
{
    assert( m_pUiConnection );

    ServerQueryPacket payload;
    if( !m_pUiConnection->Read( &payload, sizeof( payload ), 10 ) )
    {
        return false;
    }

    return HandleServerQuery( payload );
}


bool Profiler::HandleServerQuery( const ServerQueryPacket& payload )
{
    uint8_t type;
    uint64_t ptr;
    memcpy( &type, &payload.type, sizeof( payload.type ) );
    memcpy( &ptr, &payload.ptr, sizeof( payload.ptr ) );

    switch( type )
    {
    case ServerQueryString:
        SendString( ptr, (const char*)ptr, QueueType::StringData );
        break;
    case ServerQueryThreadString:
        if( ptr == m_mainThread )
        {
            SendString( ptr, "Main thread", 11, QueueType::ThreadName );
        }
        else
        {
            uint32_t threadId = (uint32_t)ptr;
            TracyThreadName name;
            GetThreadName( threadId, &name );
            SendString( ptr, name.str, name.len, QueueType::ThreadName );
            if( name.groupHint != 0 )
            {
                TracyLfqPrepare( QueueType::ThreadGroupHint );
                MemWrite( &item->threadGroupHint.thread, (uint32_t)ptr );
                MemWrite( &item->threadGroupHint.groupHint, name.groupHint );
                TracyLfqCommit;
            }
        }
        break;
    case ServerQuerySourceLocation:
        SendSourceLocation( ptr );
        break;
    case ServerQueryPlotName:
        SendString( ptr, (const char*)ptr, QueueType::PlotName );
        break;
    case ServerQueryTerminate:
        return false;
    case ServerQueryCallstackFrame:
        QueueCallstackFrame( ptr );
        break;
    case ServerQueryFrameName:
        SendString( ptr, (const char*)ptr, QueueType::FrameName );
        break;
    case ServerQueryDisconnect:
        HandleDisconnect();
        return false;
#ifdef TRACY_HAS_SYSTEM_TRACING
    case ServerQueryExternalName:
        QueueExternalName( ptr );
        break;
#endif
    case ServerQueryParameter:
        HandleParameter( ptr );
        break;
    case ServerQuerySymbol:
        QueueSymbolQuery( ptr );
        break;
#ifndef TRACY_NO_CODE_TRANSFER
    case ServerQuerySymbolCode:
        HandleSymbolCodeQuery( ptr, payload.extra );
        break;
#endif
    case ServerQuerySourceCode:
        QueueSourceCodeQuery( uint32_t( ptr ) );
        break;
    case ServerQueryDataTransfer:
#if defined(TRACY_NEEDS_SYMBOL_WORKER)
        if( m_queryData )
        {
            assert( !m_queryImage );
            m_queryImage = m_queryData;
        }
        m_queryDataPtr = m_queryData = (char*)tracy_malloc( ptr + 11 );
#endif
        AckServerQuery();
        break;
    case ServerQueryDataTransferPart:
#if defined(TRACY_NEEDS_SYMBOL_WORKER)
        memcpy( m_queryDataPtr, &ptr, 8 );
        memcpy( m_queryDataPtr+8, &payload.extra, 4 );
        m_queryDataPtr += 12;
#endif
        AckServerQuery();
        break;
#ifdef TRACY_FIBERS
    case ServerQueryFiberName:
        SendString( ptr, (const char*)ptr, QueueType::FiberName );
        break;
#endif
    default:
        assert( false );
        break;
    }

    return true;
}

void Profiler::HandleDisconnect()
{
    if ( m_pUiConnection )
    {
        moodycamel::ConsumerToken token( GetQueue() );

#ifdef TRACY_HAS_SYSTEM_TRACING
        if ( s_sysTraceThread )
        {
            auto timestamp = GetTime();
            for ( ;;)
            {
                const auto status = DequeueContextSwitches( token, timestamp );
                if ( status == DequeueStatus::ConnectionLost )
                {
                    return;
                }
                else if ( status == DequeueStatus::QueueEmpty )
                {
                    if ( !CommitPendingData() ) return;
                }
                if ( timestamp < 0 )
                {
                    if ( !CommitPendingData() ) return;
                    break;
                }
                ClearSerial();
                ClearSys();
                ClearSymbol();

                if ( m_pUiConnection->HasData() )
                {
                    if ( !ProcessServerQuery() )
                    {
                        return;
                    }
                    if ( !CommitPendingData() )
                    {
                        return;
                    }
                }
                else
                {
                    if ( !CommitPendingData() )
                    {
                        return;
                    }
                    std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
                }
            }
        }
#endif

        QueueItem terminate;
        MemWrite( &terminate.hdr.type, QueueType::Terminate );
        if( !SendData( (const char*)&terminate, 1 ) ) return;
        for (;;)
        {
            ClearQueues( token );
            if ( m_pUiConnection->HasData() )
            {
                if ( !ProcessServerQuery() )
                {
                    return;
                }
                if ( !CommitPendingData() )
                {
                    return;
                }
            }
            else
            {
                if ( !CommitPendingData() )
                {
                    return;
                }
                std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
            }
        }
    }
}

static bool CheckShouldSkipCalibration()
{
    const char *pszSkipCalibration = GetEnvVar( "TRACY_SKIP_CALIBRATION" );
    return pszSkipCalibration && pszSkipCalibration[0] == '1';
}

static bool ShouldSkipCalibration()
{
    static bool cachedResult = CheckShouldSkipCalibration();
    return cachedResult;
}

void Profiler::CalibrateTimer()
{
    m_timerMul = 1.;
    m_initTime = GetInitTime();

#ifdef TRACY_HW_TIMER

    if ( ShouldSkipCalibration() )
        return;

#  if !defined TRACY_TIMER_QPC && defined TRACY_TIMER_FALLBACK
    const bool needCalibration = HardwareSupportsInvariantTSC();
#  else
    const bool needCalibration = true;
#  endif
    if( needCalibration )
    {
        std::atomic_signal_fence( std::memory_order_acq_rel );
        const auto t0 = std::chrono::high_resolution_clock::now();
        const auto r0 = GetTime();
        std::atomic_signal_fence( std::memory_order_acq_rel );

        // Busy loop with a known good timer. Sleeping will result in a *very* bad estimate for the frequecy depending on the OS
        while ( std::chrono::duration_cast< std::chrono::milliseconds >( std::chrono::high_resolution_clock::now() - t0 ).count() < 200 )
        {
            ;
        }

        std::atomic_signal_fence( std::memory_order_acq_rel );
        const auto t1 = std::chrono::high_resolution_clock::now();
        const auto r1 = GetTime();
        std::atomic_signal_fence( std::memory_order_acq_rel );

        const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>( t1 - t0 ).count();
        const auto dr = r1 - r0;

        m_timerMul = double( dt ) / double( dr );
    }
#endif
}

void Profiler::CalibrateDelay()
{
    constexpr int MaxIterations = 50000;

    int Iterations = MaxIterations;
	
    // Do a really half-assed job if we're supposed to skip calibration.
    if ( ShouldSkipCalibration() )
    {
        Iterations = 50;
    }

    auto mindiff = std::numeric_limits<int64_t>::max();
    for( int i=0; i<Iterations * 10; i++ )
    {
        const auto t0i = GetTime();
        const auto t1i = GetTime();
        const auto dti = t1i - t0i;
        if( dti > 0 && dti < mindiff ) mindiff = dti;
    }
    m_resolution = mindiff;

#ifdef TRACY_DELAYED_INIT
    m_delay = m_resolution;
#else
    constexpr int MaxEvents = MaxIterations * 2;   // start + end
    static_assert( MaxEvents < QueuePrealloc, "Delay calibration loop will allocate memory in queue" );
    int Events = Iterations * 2;

    static const SourceLocationData __tracy_source_location { nullptr, TracyFunction,  TracyFile, (uint32_t)TracyLine, 0 };
    const auto t0 = GetTime();
    for( int i=0; i<Iterations; i++ )
    {
        {
            TracyLfqPrepare( QueueType::ZoneBegin );
            MemWrite( &item->zoneBegin.time, Profiler::GetTime() );
            MemWrite( &item->zoneBegin.srcloc, (uint64_t)&__tracy_source_location );
            TracyLfqCommit;
        }
        {
            TracyLfqPrepare( QueueType::ZoneEnd );
            MemWrite( &item->zoneEnd.time, GetTime() );
            TracyLfqCommit;
        }
    }
    const auto t1 = GetTime();
    const auto dt = t1 - t0;
    m_delay = dt / Events;

    moodycamel::ConsumerToken token( GetQueue() );
    int left = Events;
    while( left != 0 )
    {
        const auto sz = GetQueue().try_dequeue_bulk_single( token, [](const uint64_t&){}, [](QueueItem* item, size_t sz){} );
        assert( sz > 0 );
        left -= (int)sz;
    }
    assert( GetQueue().size_approx() == 0 );
#endif
}


#if defined _WIN32
struct Win32ProcInfoEx
{
    size_t size;
    void* list;
    void* end;
};

struct Win32ProcInfoIt
{
    Win32ProcInfoEx* info;
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* cur;
};


static Win32ProcInfoEx ReadProcInfo( t_GetLogicalProcessorInformationEx _GetLogicalProcessorInformationEx, LOGICAL_PROCESSOR_RELATIONSHIP rel )
{
    Win32ProcInfoEx result = { 0 };
    DWORD size = 0;
    _GetLogicalProcessorInformationEx( rel, nullptr, &size );
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* infoBuffer = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)tracy_malloc( size );
    BOOL success = _GetLogicalProcessorInformationEx( rel, infoBuffer, &size );
    if ( success )
    {
        result.size = (size_t)size;
        result.list = infoBuffer;
        result.end = (char*)result.list + size;
    }
    return result;
}


static void FreeProcInfo( Win32ProcInfoEx* pProcInfo )
{
    if ( pProcInfo )
    {
        if ( pProcInfo->list )
        {
            tracy_free( pProcInfo->list );
        }
        memset( pProcInfo, 0, sizeof( *pProcInfo ) );
    }
}


static bool IsValidProcInfo( Win32ProcInfoEx* pProcInfo )
{
    bool valid = ( ( pProcInfo->list != nullptr ) && ( pProcInfo->list < pProcInfo->end ) );
    return valid;
}


static Win32ProcInfoIt IterateProcInfo( Win32ProcInfoEx* pInfo )
{
    Win32ProcInfoIt result = { 0 };
    if ( pInfo )
    {
        result.info = pInfo;
        result.cur = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)pInfo->list;
    }

    return result;
}


static SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* GetProcInfo( Win32ProcInfoIt it )
{
    return it.cur;
}


static void AdvanceProcInfo( Win32ProcInfoIt* pIt )
{
    pIt->cur = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)((char*)pIt->cur + pIt->cur->Size);
    if ( (void*)pIt->cur >= pIt->info->end )
    {
        pIt->cur = nullptr;
    }
}
#endif


void Profiler::ReportTopology()
{
#ifndef TRACY_DELAYED_INIT
    struct CpuData
    {
        uint64_t cpuThreadMask;
        uint64_t coreInGroupMask;
        uint16_t package;
        uint16_t die;
        uint16_t group;
        uint32_t core;
        uint32_t thread;
        CpuType type;
    };

    struct CacheTopoData
    {
        uint64_t coreInGroupMask;
        uint16_t package;
        uint16_t group;
        uint32_t size;
        uint16_t linesize;
        uint8_t level;
        CacheType type;
    };

    CpuData* cpuData = nullptr;
    CacheTopoData* cacheData = nullptr;

    uint32_t cpuCount = 0;
    uint32_t cacheCount = 0;

#if defined _WIN32
#  ifdef TRACY_UWP
    t_GetLogicalProcessorInformationEx _GetLogicalProcessorInformationEx = &::GetLogicalProcessorInformationEx;
#  else
    t_GetLogicalProcessorInformationEx _GetLogicalProcessorInformationEx = (t_GetLogicalProcessorInformationEx)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "GetLogicalProcessorInformationEx" );
#  endif
    if( !_GetLogicalProcessorInformationEx ) return;

    Win32ProcInfoEx packageInfo = ReadProcInfo( _GetLogicalProcessorInformationEx, RelationProcessorPackage );
    Win32ProcInfoEx dieInfo = ReadProcInfo( _GetLogicalProcessorInformationEx, RelationProcessorDie );
    Win32ProcInfoEx coreInfo = ReadProcInfo( _GetLogicalProcessorInformationEx, RelationProcessorCore );
    Win32ProcInfoEx cacheInfo = ReadProcInfo( _GetLogicalProcessorInformationEx, RelationCache );

    uint32_t coreCount = 0;

    if ( IsValidProcInfo( &packageInfo ) && IsValidProcInfo( &dieInfo ) && IsValidProcInfo( &coreInfo ) && IsValidProcInfo( &cacheInfo ) )
    {
        for ( Win32ProcInfoIt it = IterateProcInfo( &coreInfo ); GetProcInfo( it ); AdvanceProcInfo( &it ) )
        {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* pCore = GetProcInfo( it );
            assert( pCore->Relationship == RelationProcessorCore );
            coreCount++;

            if ( pCore->Processor.GroupCount == 1 )
            {
                KAFFINITY mask = pCore->Processor.GroupMask[ 0 ].Mask;
                while ( mask != 0 )
                {
                    cpuCount++;
                    mask = mask & (mask - 1);
                }
            }
        }

        for ( Win32ProcInfoIt it = IterateProcInfo( &cacheInfo ); GetProcInfo( it ); AdvanceProcInfo( &it ) )
        {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* pCache = GetProcInfo( it );
            assert( pCache->Relationship == RelationCache );
            cacheCount++;
        }

        struct CoreInfoNode
        {
            CoreInfoNode* next;
            PROCESSOR_RELATIONSHIP* core;
        };

        size_t coreInfoNodeSize = coreCount * sizeof( CoreInfoNode );
        CoreInfoNode* coreInfoNodes = (CoreInfoNode*)tracy_malloc( coreInfoNodeSize );
        memset( coreInfoNodes, 0, coreInfoNodeSize );

        {
            CoreInfoNode* coreInfoList = NULL;

            CoreInfoNode* coreNodePtr = coreInfoNodes;
            const CoreInfoNode* const coreNodeEnd = coreInfoNodes + coreCount;

            for ( Win32ProcInfoIt it = IterateProcInfo( &coreInfo ); GetProcInfo( it ); AdvanceProcInfo( &it ) )
            {
                SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* pCore = GetProcInfo( it );
                assert( pCore->Relationship == RelationProcessorCore );
                assert( coreNodePtr < coreNodeEnd );
                if ( coreNodePtr < coreNodeEnd )
                {
                    coreNodePtr->core = (PROCESSOR_RELATIONSHIP*)&pCore->Processor;

                    CoreInfoNode** core = &coreInfoList;
                    while ( *core ) {
                        if ( (*core)->core->GroupMask[ 0 ].Group > coreNodePtr->core->GroupMask[ 0 ].Group ) {
                            break;
                        } else if (    ( (*core)->core->GroupMask[ 0 ].Group == coreNodePtr->core->GroupMask[ 0 ].Group )
                                    && ( (*core)->core->GroupMask[ 0 ].Mask > coreNodePtr->core->GroupMask[ 0 ].Mask ) ) {
                            break;
                        }

                        core = &(*core)->next;
                    }

                    coreNodePtr->next = (*core);
                    (*core) = coreNodePtr;

                    coreNodePtr++;
                }
            }
        }

        if ( cpuCount )
        {
            size_t cpuDataSize = sizeof( CpuData ) * cpuCount;
            cpuData = (CpuData*)tracy_malloc( cpuDataSize );
            if ( cpuData )
            {
                memset( cpuData, 0, cpuDataSize );
            }

            CpuData* cpuDataPtr = cpuData;
            const CpuData* const cpuDataEnd = (cpuData ? (cpuData + cpuCount) : nullptr);

            uint32_t coreId = 0;
            uint32_t threadId = 0;

            for ( CoreInfoNode* coreNodePtr = coreInfoNodes; coreNodePtr; coreNodePtr = coreNodePtr->next )
            {
                if ( coreNodePtr->core->GroupCount == 1 )
                {
                    uint16_t dieId = 0;
                    uint16_t groupId = coreNodePtr->core->GroupMask[ 0 ].Group;
                    uint32_t packageId = 0;

                    bool foundGroup = false;
                    for ( Win32ProcInfoIt it = IterateProcInfo( &packageInfo ); !foundGroup && GetProcInfo( it ); AdvanceProcInfo( &it ) )
                    {
                        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* pPackage = GetProcInfo( it );
                        assert( pPackage->Relationship == RelationProcessorPackage );
                        for ( WORD groupIndex = 0; groupIndex < pPackage->Processor.GroupCount; groupIndex++ )
                        {
                            if ( pPackage->Processor.GroupMask[ groupIndex ].Group == groupId )
                            {
                                foundGroup = true;
                                break;
                            }
                        }

                        if ( !foundGroup )
                        {
                            packageId++;
                        }
                    }

                    uint64_t cpuThreadMask = 1;
                    KAFFINITY mask = coreNodePtr->core->GroupMask[ 0 ].Mask;
                    static_assert( sizeof( cpuDataPtr->coreInGroupMask ) >= sizeof( coreNodePtr->core->GroupMask[ 0 ].Mask ), "Group mask does not fit" );
                    while ( mask != 0 )
                    {
                        if ( ( mask & 0x01 ) && ( cpuDataPtr < cpuDataEnd ) )
                        {
                            dieId = 0;
                            bool foundDie = false;
                            for ( Win32ProcInfoIt dieIt = IterateProcInfo( &dieInfo ); !foundDie && GetProcInfo( dieIt ); AdvanceProcInfo( &dieIt ) )
                            {
                                SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* pDie = GetProcInfo( dieIt );
                                assert( pDie->Relationship == RelationProcessorDie );

                                for ( WORD groupIndex = 0; groupIndex < pDie->Processor.GroupCount; groupIndex++ )
                                {
                                    KAFFINITY mask = pDie->Processor.GroupMask[ groupIndex ].Mask;
                                    if ( pDie->Processor.GroupMask[ groupIndex ].Group == groupId )
                                    {
                                        if ( ( pDie->Processor.GroupMask[ groupIndex ].Mask & cpuThreadMask ) != 0 )
                                        {
                                            foundDie = true;
                                            break;
                                        }
                                    }
                                }

                                if ( !foundDie )
                                {
                                    dieId++;
                                }
                            }

                            cpuDataPtr->cpuThreadMask = cpuThreadMask;
                            cpuDataPtr->coreInGroupMask = coreNodePtr->core->GroupMask[ 0 ].Mask;
                            cpuDataPtr->package = packageId;
                            cpuDataPtr->die = dieId;
                            cpuDataPtr->group = groupId;
                            cpuDataPtr->core = coreId;
                            cpuDataPtr->thread = threadId;
                            cpuDataPtr->type = CpuType::Normal;
                            cpuDataPtr++;

                            threadId++;
                        }

                        mask >>= 1u;
                        cpuThreadMask <<= 1u;
                    }

                    coreId++;
                }
            }
        }

        if ( cacheCount )
        {
            size_t cacheDataSize = sizeof( CacheTopoData ) * cacheCount;
            cacheData = (CacheTopoData*)tracy_malloc( cacheDataSize );
            if ( cacheData )
            {
                memset( cacheData, 0, cacheDataSize );
            }

            CacheTopoData* cacheDataPtr = cacheData;
            const CacheTopoData* const cacheDataEnd = (cacheData ? (cacheData + cacheCount) : nullptr);

            for ( Win32ProcInfoIt it = IterateProcInfo( &cacheInfo ); GetProcInfo( it ); AdvanceProcInfo( &it ) )
            {
                SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* pCache = GetProcInfo( it );
                assert( pCache->Relationship == RelationCache );
                GROUP_AFFINITY* pGroupAffinity = nullptr;
                if ( pCache->Cache.GroupCount == 1 )
                {
                    pGroupAffinity = &pCache->Cache.GroupMasks[ 0 ];
                }
                else if ( pCache->Cache.GroupCount == 0 )
                {
                    pGroupAffinity = &pCache->Cache.GroupMask;
                }

                if ( pGroupAffinity && ( cacheDataPtr < cacheDataEnd ) )
                {
                    uint16_t groupId = pGroupAffinity->Group;
                    uint32_t packageId = 0;

                    bool foundGroup = false;
                    for ( Win32ProcInfoIt it = IterateProcInfo( &packageInfo ); !foundGroup && GetProcInfo( it ); AdvanceProcInfo( &it ) )
                    {
                        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* pPackage = GetProcInfo( it );
                        assert( pPackage->Relationship == RelationProcessorPackage );
                        for ( WORD groupIndex = 0; groupIndex < pPackage->Processor.GroupCount; groupIndex++ )
                        {
                            if ( pPackage->Processor.GroupMask[ groupIndex ].Group == groupId )
                            {
                                foundGroup = true;
                                break;
                            }
                        }

                        if ( !foundGroup )
                        {
                            packageId++;
                        }
                    }

                    cacheDataPtr->coreInGroupMask = pGroupAffinity->Mask;
                    cacheDataPtr->package = packageId;
                    cacheDataPtr->group = groupId;
                    cacheDataPtr->size = pCache->Cache.CacheSize;
                    cacheDataPtr->linesize = pCache->Cache.LineSize;
                    cacheDataPtr->level = pCache->Cache.Level;

                    switch ( pCache->Cache.Type )
                    {
                        case CacheUnified: {
                            cacheDataPtr->type = CacheType::Unified;
                        } break;

                        case CacheInstruction: {
                            cacheDataPtr->type = CacheType::Instruction;
                        } break;

                        case CacheData: {
                            cacheDataPtr->type = CacheType::Data;
                        } break;

                        default: {
                            cacheDataPtr->type = CacheType::Unknown;
                        } break;
                    }

                    cacheDataPtr++;
                }
            }
        }

#       if defined __i386 || defined _M_IX86 || defined __x86_64__ || defined _M_X64
        uint32_t regs[4];
        char manufacturer[12];
        CpuId( regs, 0 );
        memcpy( manufacturer, regs+1, 4 );
        memcpy( manufacturer+4, regs+3, 4 );
        memcpy( manufacturer+8, regs+2, 4 );

        if ( memcmp( manufacturer, "GenuineIntel", sizeof("GenuineIntel") - 1 ) == 0 )
        {
            bool isHybridCpu = false;
            uint32_t regs[4];
            CpuId( regs, 0 );
            uint32_t maxLeaf = regs[CpuidRegister_eax];
            if ( maxLeaf >= 0x1a )
            {
                CpuId( regs, 0x7 );
                bool hybridCpu = ( ( regs[CpuidRegister_edx] & (1u << 15) ) != 0 );
                CpuId( regs, 0x1a );

                if ( hybridCpu && ( regs[CpuidRegister_eax] != 0 ) )
                {
                    isHybridCpu = true;

                    for ( Win32ProcInfoIt it = IterateProcInfo( &packageInfo ); GetProcInfo( it ); AdvanceProcInfo( &it ) )
                    {
                        SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* pPackage = GetProcInfo( it );
                        assert( pPackage->Relationship == RelationProcessorPackage );
                        for ( WORD groupIndex = 0; groupIndex < pPackage->Processor.GroupCount; groupIndex++ )
                        {
                            // Set the groups affinity so this thread can run on all cores in the group
                            const GROUP_AFFINITY* activeGroup = &pPackage->Processor.GroupMask[ groupIndex ];
                            GROUP_AFFINITY prevGroup = { 0 };
                            SetThreadGroupAffinity( GetCurrentThread(), activeGroup, &prevGroup );

                            DWORD_PTR affinityMask = 1;
                            while ( affinityMask )
                            {
                                // Make sure we run cpu id on that specific processor core
                                if ( affinityMask & activeGroup->Mask )
                                {
                                    SetThreadAffinityMask( GetCurrentThread(), affinityMask );

                                    for ( uint32_t cpuIndex = 0; cpuIndex < cpuCount; cpuIndex++ )
                                    {
                                        CpuData* cpu = ( cpuData + cpuIndex );
                                        static_assert( sizeof( cpu->cpuThreadMask ) >= sizeof( affinityMask ), "affinity mask does not fit" );
                                        if ( ( cpu->group == activeGroup->Group ) && ( cpu->cpuThreadMask == affinityMask ) )
                                        {
                                            CpuId( regs, 0x1a );
                                            //Bits 31-24: Core type*
                                            //    10H: Reserved
                                            //    20H: Intel Atom
                                            //    30H: Reserved
                                            //    40H: Intel Core
                                            uint32_t coreType = regs[CpuidRegister_eax];
                                            coreType = (coreType >> 24);

                                            if ( coreType == 0x20 )
                                            {
                                                cpu->type = CpuType::IntelECore;
                                            }
                                            else if ( coreType == 0x40 )
                                            {
                                                cpu->type = CpuType::IntelPCore;
                                            }

                                            break;
                                        }
                                    }
                                }

                                affinityMask <<= 1;
                            }

                            // Reset the groups's affinity
                            SetThreadGroupAffinity( GetCurrentThread(), &prevGroup, nullptr );
                        }
                    }
                }
            }
        }
#       endif // if defined __i386 || defined _M_IX86 || defined __x86_64__ || defined _M_X64
    }
    else
    {
        cpuCount = 0;
        cacheCount = 0;
    }

    FreeProcInfo( &packageInfo );
    FreeProcInfo( &dieInfo );
    FreeProcInfo( &coreInfo );
    FreeProcInfo( &cacheInfo );

#elif defined __linux__
    cpuCount = (uint32_t)std::thread::hardware_concurrency();
    size_t cpuDataSize = sizeof( CpuData ) * cpuCount;
    cpuData = (CpuData*)tracy_malloc( cpuDataSize );

    if ( cpuData )
    {
        memset( cpuData, 0, cpuDataSize );

        const char* basePath = "/sys/devices/system/cpu/cpu";
        for( uint32_t i=0; i<cpuCount; i++ )
        {
            char path[1024];
            sprintf( path, "%s%u/topology/physical_package_id", basePath, i );
            char buf[1024];
            FILE* f = fopen( path, "rb" );
            if( f )
            {
                auto read = fread( buf, 1, 1024, f );
                buf[read] = '\0';
                fclose( f );

                uint32_t packageId = uint32_t( atoi( buf ) );
                uint32_t threadId = (uint32_t)i;

                sprintf( path, "%s%u/topology/core_id", basePath, i );
                f = fopen( path, "rb" );
                if ( f )
                {
                    read = fread( buf, 1, 1024, f );
                    buf[read] = '\0';
                    fclose( f );

                    uint32_t coreId = uint32_t( atoi( buf ) );
                    cpuData[i].package = packageId;
                    cpuData[i].thread = threadId;
                    cpuData[i].core = coreId;
                }
            }

            sprintf( path, "%s%i/topology/die_id", basePath, i );
            f = fopen( path, "rb" );
            if( f )
            {
                auto read = fread( buf, 1, 1024, f );
                buf[read] = '\0';
                fclose( f );
                cpuData[i].die = uint32_t( atoi( buf ) );
            }
        }
    }
#endif

    for ( uint32_t i = 0; i < cpuCount; i++ )
    {
        CpuData* pCpu = &cpuData[i];

        TracyLfqPrepare( QueueType::CpuTopology );
        MemWrite( &item->cpuTopology.coreInGroupMask, pCpu->coreInGroupMask );
        MemWrite( &item->cpuTopology.package, pCpu->package );
        MemWrite( &item->cpuTopology.die, pCpu->die );
        MemWrite( &item->cpuTopology.group, pCpu->group );
        MemWrite( &item->cpuTopology.core, pCpu->core );
        MemWrite( &item->cpuTopology.thread, pCpu->thread );
        MemWrite( &item->cpuTopology.type, pCpu->type );

#ifdef TRACY_ON_DEMAND
        DeferItem( *item );
#endif

        TracyLfqCommit;
    }

    for ( uint32_t i = 0; i < cacheCount; i++ )
    {
        CacheTopoData* pCache = &cacheData[i];

        TracyLfqPrepare( QueueType::CacheTopology );
        MemWrite( &item->cacheTopology.coreInGroupMask, pCache->coreInGroupMask );
        MemWrite( &item->cacheTopology.package, pCache->package );
        MemWrite( &item->cacheTopology.group, pCache->group );
        MemWrite( &item->cacheTopology.size, pCache->size );
        MemWrite( &item->cacheTopology.linesize, pCache->linesize );
        MemWrite( &item->cacheTopology.level, pCache->level );
        MemWrite( &item->cacheTopology.type, pCache->type );

#ifdef TRACY_ON_DEMAND
        DeferItem( *item );
#endif

        TracyLfqCommit;
    }

    if ( cacheData )
    {
        tracy_free( cacheData );
        cacheData = nullptr;
        cacheCount = 0;
    }

    if ( cpuData )
    {
        tracy_free( cpuData );
        cpuData = nullptr;
        cpuCount = 0;
    }
#endif // ifndef TRACY_DELAYED_INIT
}

void Profiler::SendCallstack( int32_t depth, const char* skipBefore )
{
#ifdef TRACY_HAS_CALLSTACK
    auto ptr = Callstack( depth );
    CutCallstack( ptr, skipBefore );

    TracyQueuePrepare( QueueType::Callstack );
    MemWrite( &item->callstackFat.ptr, (uint64_t)ptr );
    TracyQueueCommit( callstackFatThread );
#endif
}

void Profiler::CutCallstack( void* callstack, const char* skipBefore )
{
#ifdef TRACY_HAS_CALLSTACK
    auto data = (uintptr_t*)callstack;
    const auto sz = *data++;
    uintptr_t i;
    for( i=0; i<sz; i++ )
    {
        auto name = DecodeCallstackPtrFast( uint64_t( data[i] ) );
        const bool found = strcmp( name, skipBefore ) == 0;
        if( found )
        {
            i++;
            break;
        }
    }

    if( i != sz )
    {
        memmove( data, data + i, ( sz - i ) * sizeof( uintptr_t* ) );
        *--data = sz - i;
    }
#endif
}

void Profiler::ProcessSysTime()
{
#ifdef TRACY_HAS_SYSTIME
    if( m_shutdown.load( std::memory_order_relaxed ) ) return;
    auto t = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    if( t - m_sysTimeLast > 100000000 )    // 100 ms
    {
        auto sysTime = m_sysTime.Get();
        if( sysTime >= 0 )
        {
            m_sysTimeLast = t;

            TracyLfqPrepare( QueueType::SysTimeReport );
            MemWrite( &item->sysTime.time, GetTime() );
            MemWrite( &item->sysTime.sysTime, sysTime );
            TracyLfqCommit;
        }
    }

#ifdef TRACY_HAS_SYSPOWER
    m_sysPower.Tick();
#endif // ifdef TRACY_HAS_SYSPOWER

#endif
}

void Profiler::HandleParameter( uint64_t payload )
{
    assert( m_paramCallback );
    const auto idx = uint32_t( payload >> 32 );
    const auto val = int32_t( payload & 0xFFFFFFFF );
    m_paramCallback( m_paramCallbackData, idx, val );
    AckServerQuery();
}

void Profiler::HandleSymbolCodeQuery( uint64_t symbol, uint32_t size )
{
    if( symbol >> 63 != 0 )
    {
        QueueKernelCode( symbol, size );
    }
    else
    {
        auto&& lambda = [ this, symbol ]( const char* buf, size_t size ) {
            SendLongString( symbol, buf, size, QueueType::SymbolCode );
        };

        // 'symbol' may have come from a module that has since unloaded, perform a safe copy before sending
        if( !WithSafeCopy( (const char*)symbol, size, lambda ) ) AckSymbolCodeNotAvailable();
    }
}

void Profiler::HandleSourceCodeQuery( char* data, char* image, uint32_t id )
{
    bool ok = false;
#if defined(TRACY_NEEDS_SYMBOL_WORKER)
    FILE* f = fopen( data, "rb" );
    if( f )
    {
        struct stat st;
        if( fstat( fileno( f ), &st ) == 0 && (uint64_t)st.st_mtime < m_exectime && st.st_size < ( TargetFrameSize - 16 ) )
        {
            auto ptr = (char*)tracy_malloc_fast( st.st_size );
            auto rd = fread( ptr, 1, st.st_size, f );
            if( rd == (size_t)st.st_size )
            {
                QueueItem* item = QueueSymbol( QueueType::SourceCodeMetadata );
                MemWrite( &item->sourceCodeMetadata.ptr, (uint64_t)ptr );
                MemWrite( &item->sourceCodeMetadata.size, (uint32_t)rd );
                MemWrite( &item->sourceCodeMetadata.id, id );
                QueueSymbolFinish();
                ok = true;
            }
            else
            {
                tracy_free_fast( ptr );
            }
        }
        fclose( f );
    }

#ifdef TRACY_DEBUGINFOD
    else if( image && data[0] == '/' )
    {
        size_t size;
        auto buildid = GetBuildIdForImage( image, size );
        if( buildid )
        {
            auto d = debuginfod_find_source( GetDebuginfodClient(), buildid, size, data, nullptr );
            TracyDebug( "DebugInfo source query: %s, fn: %s, image: %s\n", d >= 0 ? " ok " : "fail", data, image );
            if( d >= 0 )
            {
                struct stat st;
                fstat( d, &st );
                if( st.st_size < ( TargetFrameSize - 16 ) )
                {
                    lseek( d, 0, SEEK_SET );
                    auto ptr = (char*)tracy_malloc_fast( st.st_size );
                    auto rd = read( d, ptr, st.st_size );
                    if( rd == (size_t)st.st_size )
                    {
                        QueueItem* item = QueueSymbol( QueueType::SourceCodeMetadata );
                        MemWrite( &item->sourceCodeMetadata.ptr, (uint64_t)ptr );
                        MemWrite( &item->sourceCodeMetadata.size, (uint32_t)rd );
                        MemWrite( &item->sourceCodeMetadata.id, id );
                        QueueSymbolFinish();
                        ok = true;
                    }
                    else
                    {
                        tracy_free_fast( ptr );
                    }
                }
                close( d );
            }
        }
    }
    else
    {
        TracyDebug( "DebugInfo invalid query fn: %s, image: %s\n", data, image );
    }
#endif

    if( !ok && m_sourceCallback )
    {
        size_t sz;
        char* ptr = m_sourceCallback( m_sourceCallbackData, data, sz );
        if( ptr )
        {
            if( sz < ( TargetFrameSize - 16 ) )
            {
                QueueItem* item = QueueSymbol( QueueType::SourceCodeMetadata );
                MemWrite( &item->sourceCodeMetadata.ptr, (uint64_t)ptr );
                MemWrite( &item->sourceCodeMetadata.size, (uint32_t)sz );
                MemWrite( &item->sourceCodeMetadata.id, id );
                QueueSymbolFinish();
                ok = true;
            }
            else
            {
                tracy_free_fast( ptr );
            }
        }
    }
#endif // if defined(TRACY_NEEDS_SYMBOL_WORKER)

    if( !ok )
    {
        QueueItem* item = QueueSymbol( QueueType::AckSourceCodeNotAvailable );
        MemWrite( &item->sourceCodeNotAvailable, id );
        QueueSymbolFinish();
    }

    tracy_free_fast( data );
    tracy_free_fast( image );
}

#if defined _WIN32 && defined TRACY_TIMER_QPC
int64_t Profiler::GetTimeQpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter( &t );
    return t.QuadPart;
}
#endif

#undef LockAssert

}

static bool TracyCheckConnectionId( TracyCZoneCtx ctx )
{
#ifdef TRACY_ON_DEMAND
    bool isActive = ( (ctx.active != 0) && (ctx.active == tracy::GetProfiler().ConnectionId()) );
#else // ifdef TRACY_ON_DEMAND
    bool isActive = ( ctx.active != 0 );
#endif // ifdef TRACY_ON_DEMAND

    return isActive;
}


#ifdef __cplusplus
extern "C" {
#endif

TRACY_API TracyCZoneCtx ___tracy_emit_zone_begin( const struct ___tracy_source_location_data* srcloc, int32_t active )
{
    ___tracy_c_zone_context ctx;
#ifdef TRACY_ON_DEMAND
    ctx.active = (active ? tracy::GetProfiler().ConnectionId() : 0);
#else
    ctx.active = (uint32_t)active;
#endif
    if( !ctx.active ) return ctx;
    const auto id = tracy::GetProfiler().GetNextZoneId();
    ctx.id = id;

#ifndef TRACY_NO_VERIFY
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneValidation );
        tracy::MemWrite( &item->zoneValidation.id, id );
        TracyQueueCommitC( zoneValidationThread );
    }
#endif
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneBegin );
        tracy::MemWrite( &item->zoneBegin.time, tracy::Profiler::GetTime() );
        tracy::MemWrite( &item->zoneBegin.srcloc, (uint64_t)srcloc );
        TracyQueueCommitC( zoneBeginThread );
    }
    return ctx;
}

TRACY_API TracyCZoneCtx ___tracy_emit_zone_begin_callstack( const struct ___tracy_source_location_data* srcloc, int32_t depth, int32_t active )
{
    ___tracy_c_zone_context ctx;
#ifdef TRACY_ON_DEMAND
    ctx.active = (active ? tracy::GetProfiler().ConnectionId() : 0);
#else
    ctx.active = (uint32_t)active;
#endif
    if( !ctx.active ) return ctx;
    const auto id = tracy::GetProfiler().GetNextZoneId();
    ctx.id = id;

#ifndef TRACY_NO_VERIFY
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneValidation );
        tracy::MemWrite( &item->zoneValidation.id, id );
        TracyQueueCommitC( zoneValidationThread );
    }
#endif
    auto zoneQueue = tracy::QueueType::ZoneBegin;
    if( depth > 0 && tracy::has_callstack() )
    {
        tracy::GetProfiler().SendCallstack( depth );
        zoneQueue = tracy::QueueType::ZoneBeginCallstack;
    }
    TracyQueuePrepareC( zoneQueue );
    tracy::MemWrite( &item->zoneBegin.time, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->zoneBegin.srcloc, (uint64_t)srcloc );
    TracyQueueCommitC( zoneBeginThread );

    return ctx;
}

TRACY_API TracyCZoneCtx ___tracy_emit_zone_begin_alloc( uint64_t srcloc, int32_t active )
{
    ___tracy_c_zone_context ctx;
#ifdef TRACY_ON_DEMAND
    ctx.active = (active ? tracy::GetProfiler().ConnectionId() : 0);
#else
    ctx.active = (uint32_t)active;
#endif
    if( !ctx.active )
    {
        tracy::tracy_free( (void*)srcloc );
        return ctx;
    }
    const auto id = tracy::GetProfiler().GetNextZoneId();
    ctx.id = id;

#ifndef TRACY_NO_VERIFY
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneValidation );
        tracy::MemWrite( &item->zoneValidation.id, id );
        TracyQueueCommitC( zoneValidationThread );
    }
#endif
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneBeginAllocSrcLoc );
        tracy::MemWrite( &item->zoneBegin.time, tracy::Profiler::GetTime() );
        tracy::MemWrite( &item->zoneBegin.srcloc, srcloc );
        TracyQueueCommitC( zoneBeginThread );
    }
    return ctx;
}

TRACY_API TracyCZoneCtx ___tracy_emit_zone_begin_alloc_callstack( uint64_t srcloc, int32_t depth, int32_t active )
{
    ___tracy_c_zone_context ctx;
#ifdef TRACY_ON_DEMAND
    ctx.active = (active ? tracy::GetProfiler().ConnectionId() : 0);
#else
    ctx.active = (uint32_t)active;
#endif
    if( !ctx.active )
    {
        tracy::tracy_free( (void*)srcloc );
        return ctx;
    }
    const auto id = tracy::GetProfiler().GetNextZoneId();
    ctx.id = id;

#ifndef TRACY_NO_VERIFY
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneValidation );
        tracy::MemWrite( &item->zoneValidation.id, id );
        TracyQueueCommitC( zoneValidationThread );
    }
#endif
    auto zoneQueue = tracy::QueueType::ZoneBeginAllocSrcLoc;
    if( depth > 0 && tracy::has_callstack() )
    {
        tracy::GetProfiler().SendCallstack( depth );
        zoneQueue = tracy::QueueType::ZoneBeginAllocSrcLocCallstack;
    }
    TracyQueuePrepareC( zoneQueue );
    tracy::MemWrite( &item->zoneBegin.time, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->zoneBegin.srcloc, srcloc );
    TracyQueueCommitC( zoneBeginThread );

    return ctx;
}

TRACY_API void ___tracy_emit_zone_end( TracyCZoneCtx ctx )
{
    if ( !TracyCheckConnectionId( ctx ) ) return;

#ifndef TRACY_NO_VERIFY
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneValidation );
        tracy::MemWrite( &item->zoneValidation.id, ctx.id );
        TracyQueueCommitC( zoneValidationThread );
    }
#endif
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneEnd );
        tracy::MemWrite( &item->zoneEnd.time, tracy::Profiler::GetTime() );
        TracyQueueCommitC( zoneEndThread );
    }
}

TRACY_API void ___tracy_emit_zone_text( TracyCZoneCtx ctx, const char* txt, size_t size )
{
    assert( size < std::numeric_limits<uint16_t>::max() );
    if ( !TracyCheckConnectionId( ctx ) ) return;

    auto ptr = (char*)tracy::tracy_malloc( size );
    memcpy( ptr, txt, size );
#ifndef TRACY_NO_VERIFY
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneValidation );
        tracy::MemWrite( &item->zoneValidation.id, ctx.id );
        TracyQueueCommitC( zoneValidationThread );
    }
#endif
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneText );
        tracy::MemWrite( &item->zoneTextFat.text, (uint64_t)ptr );
        tracy::MemWrite( &item->zoneTextFat.size, (uint16_t)size );
        TracyQueueCommitC( zoneTextFatThread );
    }
}

TRACY_API void ___tracy_emit_zone_name( TracyCZoneCtx ctx, const char* txt, size_t size )
{
    assert( size < std::numeric_limits<uint16_t>::max() );
    if ( !TracyCheckConnectionId( ctx ) ) return;

    auto ptr = (char*)tracy::tracy_malloc( size );
    memcpy( ptr, txt, size );
#ifndef TRACY_NO_VERIFY
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneValidation );
        tracy::MemWrite( &item->zoneValidation.id, ctx.id );
        TracyQueueCommitC( zoneValidationThread );
    }
#endif
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneName );
        tracy::MemWrite( &item->zoneTextFat.text, (uint64_t)ptr );
        tracy::MemWrite( &item->zoneTextFat.size, (uint16_t)size );
        TracyQueueCommitC( zoneTextFatThread );
    }
}

TRACY_API void ___tracy_emit_zone_color( TracyCZoneCtx ctx, uint32_t color )
{
    if ( !TracyCheckConnectionId( ctx ) ) return;

#ifndef TRACY_NO_VERIFY
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneValidation );
        tracy::MemWrite( &item->zoneValidation.id, ctx.id );
        TracyQueueCommitC( zoneValidationThread );
    }
#endif
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneColor );
        tracy::MemWrite( &item->zoneColor.b, uint8_t( ( color       ) & 0xFF ) );
        tracy::MemWrite( &item->zoneColor.g, uint8_t( ( color >> 8  ) & 0xFF ) );
        tracy::MemWrite( &item->zoneColor.r, uint8_t( ( color >> 16 ) & 0xFF ) );
        TracyQueueCommitC( zoneColorThread );
    }
}

TRACY_API void ___tracy_emit_zone_value( TracyCZoneCtx ctx, uint64_t value )
{
    if ( !TracyCheckConnectionId( ctx ) ) return;

#ifndef TRACY_NO_VERIFY
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneValidation );
        tracy::MemWrite( &item->zoneValidation.id, ctx.id );
        TracyQueueCommitC( zoneValidationThread );
    }
#endif
    {
        TracyQueuePrepareC( tracy::QueueType::ZoneValue );
        tracy::MemWrite( &item->zoneValue.value, value );
        TracyQueueCommitC( zoneValueThread );
    }
}

TRACY_API void ___tracy_emit_memory_alloc( const void* ptr, size_t size, int32_t secure ) { tracy::Profiler::MemAlloc( ptr, size, secure != 0 ); }
TRACY_API void ___tracy_emit_memory_alloc_callstack( const void* ptr, size_t size, int32_t depth, int32_t secure )
{
    if( depth > 0 && tracy::has_callstack() )
    {
        tracy::Profiler::MemAllocCallstack( ptr, size, depth, secure != 0 );
    }
    else
    {
        tracy::Profiler::MemAlloc( ptr, size, secure != 0 );
    }
}
TRACY_API void ___tracy_emit_memory_free( const void* ptr, int32_t secure ) { tracy::Profiler::MemFree( ptr, secure != 0 ); }
TRACY_API void ___tracy_emit_memory_free_callstack( const void* ptr, int32_t depth, int32_t secure )
{
    if( depth > 0 && tracy::has_callstack() )
    {
        tracy::Profiler::MemFreeCallstack( ptr, depth, secure != 0 );
    }
    else
    {
        tracy::Profiler::MemFree( ptr, secure != 0 );
    }
}
TRACY_API void ___tracy_emit_memory_discard( const char* name, int32_t secure ) { tracy::Profiler::MemDiscard( name, secure != 0 ); }
TRACY_API void ___tracy_emit_memory_discard_callstack( const char* name, int32_t secure, int32_t depth )
{
    if( depth > 0 && tracy::has_callstack() )
    {
        tracy::Profiler::MemDiscardCallstack( name, secure != 0, depth );
    }
    else
    {
        tracy::Profiler::MemDiscard( name, secure != 0 );
    }
}
TRACY_API void ___tracy_emit_memory_alloc_named( const void* ptr, size_t size, int32_t secure, const char* name ) { tracy::Profiler::MemAllocNamed( ptr, size, secure != 0, name ); }
TRACY_API void ___tracy_emit_memory_alloc_callstack_named( const void* ptr, size_t size, int32_t depth, int32_t secure, const char* name )
{
    if( depth > 0 && tracy::has_callstack() )
    {
        tracy::Profiler::MemAllocCallstackNamed( ptr, size, depth, secure != 0, name );
    }
    else
    {
        tracy::Profiler::MemAllocNamed( ptr, size, secure != 0, name );
    }
}
TRACY_API void ___tracy_emit_memory_free_named( const void* ptr, int32_t secure, const char* name ) { tracy::Profiler::MemFreeNamed( ptr, secure != 0, name ); }
TRACY_API void ___tracy_emit_memory_free_callstack_named( const void* ptr, int32_t depth, int32_t secure, const char* name )
{
    if( depth > 0 && tracy::has_callstack() )
    {
        tracy::Profiler::MemFreeCallstackNamed( ptr, depth, secure != 0, name );
    }
    else
    {
        tracy::Profiler::MemFreeNamed( ptr, secure != 0, name );
    }
}
TRACY_API void ___tracy_emit_frame_mark( const char* name ) { tracy::Profiler::SendFrameMark( name ); }
TRACY_API void ___tracy_emit_frame_mark_start( const char* name ) { tracy::Profiler::SendFrameMark( name, tracy::QueueType::FrameMarkMsgStart ); }
TRACY_API void ___tracy_emit_frame_mark_end( const char* name ) { tracy::Profiler::SendFrameMark( name, tracy::QueueType::FrameMarkMsgEnd ); }
TRACY_API void ___tracy_emit_frame_image( const void* image, uint16_t w, uint16_t h, uint8_t offset, int32_t flip ) { tracy::Profiler::SendFrameImage( image, w, h, offset, flip != 0 ); }
TRACY_API void ___tracy_emit_plot( const char* name, double val ) { tracy::Profiler::PlotData( name, val ); }
TRACY_API void ___tracy_emit_plot_float( const char* name, float val ) { tracy::Profiler::PlotData( name, val ); }
TRACY_API void ___tracy_emit_plot_int( const char* name, int64_t val ) { tracy::Profiler::PlotData( name, val ); }
TRACY_API void ___tracy_emit_plot_config( const char* name, int32_t type, int32_t step, int32_t fill, uint32_t color ) { tracy::Profiler::ConfigurePlot( name, tracy::PlotFormatType(type), step != 0, fill != 0, color ); }
TRACY_API void ___tracy_emit_message( const char* txt, size_t size, int32_t callstack_depth ) { tracy::Profiler::Message( txt, size, callstack_depth ); }
TRACY_API void ___tracy_emit_messageL( const char* txt, int32_t callstack_depth ) { tracy::Profiler::Message( txt, callstack_depth ); }
TRACY_API void ___tracy_emit_messageC( const char* txt, size_t size, uint32_t color, int32_t callstack_depth ) { tracy::Profiler::MessageColor( txt, size, color, callstack_depth ); }
TRACY_API void ___tracy_emit_messageLC( const char* txt, uint32_t color, int32_t callstack_depth ) { tracy::Profiler::MessageColor( txt, color, callstack_depth ); }
TRACY_API void ___tracy_emit_message_appinfo( const char* txt, size_t size ) { tracy::Profiler::MessageAppInfo( txt, size ); }

TRACY_API uint64_t ___tracy_alloc_srcloc( uint32_t line, const char* source, size_t sourceSz, const char* function, size_t functionSz, uint32_t color ) {
    return tracy::Profiler::AllocSourceLocation( line, source, sourceSz, function, functionSz, color );
}

TRACY_API uint64_t ___tracy_alloc_srcloc_name( uint32_t line, const char* source, size_t sourceSz, const char* function, size_t functionSz, const char* name, size_t nameSz, uint32_t color ) {
    return tracy::Profiler::AllocSourceLocation( line, source, sourceSz, function, functionSz, name, nameSz, color );
}

TRACY_API void ___tracy_emit_gpu_zone_begin( const struct ___tracy_gpu_zone_begin_data data )
{
    TracyLfqPrepareC( tracy::QueueType::GpuZoneBegin );
    tracy::MemWrite( &item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuZoneBegin.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->gpuZoneBegin.srcloc, data.srcloc );
    tracy::MemWrite( &item->gpuZoneBegin.queryId, data.queryId );
    tracy::MemWrite( &item->gpuZoneBegin.context, data.context );
    TracyLfqCommitC;
}

TRACY_API void ___tracy_emit_gpu_zone_begin_callstack( const struct ___tracy_gpu_zone_begin_callstack_data data )
{
    tracy::GetProfiler().SendCallstack( data.depth );
    TracyLfqPrepareC( tracy::QueueType::GpuZoneBeginCallstack );
    tracy::MemWrite( &item->gpuZoneBegin.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuZoneBegin.queryId, data.queryId );
    tracy::MemWrite( &item->gpuZoneBegin.context, data.context );
    tracy::MemWrite( &item->gpuZoneBegin.srcloc, data.srcloc );
    TracyLfqCommitC;
}

TRACY_API void ___tracy_emit_gpu_zone_begin_alloc( const struct ___tracy_gpu_zone_begin_data data )
{
    TracyLfqPrepareC( tracy::QueueType::GpuZoneBeginAllocSrcLoc  );
    tracy::MemWrite( &item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuZoneBegin.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->gpuZoneBegin.srcloc, data.srcloc );
    tracy::MemWrite( &item->gpuZoneBegin.queryId, data.queryId );
    tracy::MemWrite( &item->gpuZoneBegin.context, data.context );
    TracyLfqCommitC;
}

TRACY_API void ___tracy_emit_gpu_zone_begin_alloc_callstack( const struct ___tracy_gpu_zone_begin_callstack_data data )
{
    tracy::GetProfiler().SendCallstack( data.depth );
    TracyLfqPrepareC( tracy::QueueType::GpuZoneBeginAllocSrcLocCallstack  );
    tracy::MemWrite( &item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuZoneBegin.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->gpuZoneBegin.srcloc, data.srcloc );
    tracy::MemWrite( &item->gpuZoneBegin.queryId, data.queryId );
    tracy::MemWrite( &item->gpuZoneBegin.context, data.context );
    TracyLfqCommitC;
}

TRACY_API void ___tracy_emit_gpu_time( const struct ___tracy_gpu_time_data data )
{
    TracyLfqPrepareC( tracy::QueueType::GpuTime );
    tracy::MemWrite( &item->gpuTime.gpuTime, data.gpuTime );
    tracy::MemWrite( &item->gpuTime.queryId, data.queryId );
    tracy::MemWrite( &item->gpuTime.context, data.context );
    TracyLfqCommitC;
}

TRACY_API void ___tracy_emit_gpu_zone_end( const struct ___tracy_gpu_zone_end_data data )
{
    TracyLfqPrepareC( tracy::QueueType::GpuZoneEnd );
    tracy::MemWrite( &item->gpuZoneEnd.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuZoneEnd.thread, 0 );
    tracy::MemWrite( &item->gpuZoneEnd.queryId, data.queryId );
    tracy::MemWrite( &item->gpuZoneEnd.context, data.context );
    TracyLfqCommitC;
}

TRACY_API void ___tracy_emit_gpu_new_context( ___tracy_gpu_new_context_data data )
{
    TracyLfqPrepareC( tracy::QueueType::GpuNewContext );
    tracy::MemWrite( &item->gpuNewContext.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuNewContext.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->gpuNewContext.gpuTime, data.gpuTime );
    tracy::MemWrite( &item->gpuNewContext.period, data.period );
    tracy::MemWrite( &item->gpuNewContext.context, data.context );
    tracy::MemWrite( &item->gpuNewContext.flags, data.flags );
    tracy::MemWrite( &item->gpuNewContext.type, data.type );

#ifdef TRACY_ON_DEMAND
    tracy::GetProfiler().DeferItem( *item );
#endif

    TracyLfqCommitC;
}

TRACY_API void ___tracy_emit_gpu_context_name( const struct ___tracy_gpu_context_name_data data )
{
    auto ptr = (char*)tracy::tracy_malloc( data.len );
    memcpy( ptr, data.name, data.len );

    TracyLfqPrepareC( tracy::QueueType::GpuContextName );
    tracy::MemWrite( &item->gpuContextNameFat.context, data.context );
    tracy::MemWrite( &item->gpuContextNameFat.ptr, (uint64_t)ptr );
    tracy::MemWrite( &item->gpuContextNameFat.size, data.len );

#ifdef TRACY_ON_DEMAND
    tracy::GetProfiler().DeferItem( *item );
#endif

    TracyLfqCommitC;
}

TRACY_API void ___tracy_emit_gpu_calibration( const struct ___tracy_gpu_calibration_data data )
{
    TracyLfqPrepareC( tracy::QueueType::GpuCalibration );
    tracy::MemWrite( &item->gpuCalibration.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuCalibration.gpuTime, data.gpuTime );
    tracy::MemWrite( &item->gpuCalibration.cpuDelta, data.cpuDelta );
    tracy::MemWrite( &item->gpuCalibration.context, data.context );
    TracyLfqCommitC;
}

TRACY_API void ___tracy_emit_gpu_time_sync( const struct ___tracy_gpu_time_sync_data data )
{
    TracyLfqPrepareC( tracy::QueueType::GpuTimeSync );
    tracy::MemWrite( &item->gpuTimeSync.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuTimeSync.gpuTime, data.gpuTime );
    tracy::MemWrite( &item->gpuTimeSync.context, data.context );
    TracyLfqCommitC;
}

TRACY_API void ___tracy_emit_gpu_zone_begin_serial( const struct ___tracy_gpu_zone_begin_data data )
{
    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuZoneBeginSerial );
    tracy::MemWrite( &item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuZoneBegin.srcloc, data.srcloc );
    tracy::MemWrite( &item->gpuZoneBegin.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->gpuZoneBegin.queryId, data.queryId );
    tracy::MemWrite( &item->gpuZoneBegin.context, data.context );
    tracy::Profiler::QueueSerialFinish();
}

#ifdef TRACY_HAS_CALLSTACK

TRACY_API void ___tracy_emit_gpu_zone_begin_callstack_serial( const struct ___tracy_gpu_zone_begin_callstack_data data )
{
    auto item = tracy::Profiler::QueueSerialCallstack( tracy::Callstack( data.depth ) );
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuZoneBeginCallstackSerial );
    tracy::MemWrite( &item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuZoneBegin.srcloc, data.srcloc );
    tracy::MemWrite( &item->gpuZoneBegin.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->gpuZoneBegin.queryId, data.queryId );
    tracy::MemWrite( &item->gpuZoneBegin.context, data.context );
    tracy::Profiler::QueueSerialFinish();
}

#endif

TRACY_API void ___tracy_emit_gpu_zone_begin_alloc_serial( const struct ___tracy_gpu_zone_begin_data data )
{
    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuZoneBeginAllocSrcLocSerial );
    tracy::MemWrite( &item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuZoneBegin.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->gpuZoneBegin.srcloc, data.srcloc );
    tracy::MemWrite( &item->gpuZoneBegin.queryId, data.queryId );
    tracy::MemWrite( &item->gpuZoneBegin.context, data.context );
    tracy::Profiler::QueueSerialFinish();
}

#ifdef TRACY_HAS_CALLSTACK

TRACY_API void ___tracy_emit_gpu_zone_begin_alloc_callstack_serial( const struct ___tracy_gpu_zone_begin_callstack_data data )
{
    auto item = tracy::Profiler::QueueSerialCallstack( tracy::Callstack( data.depth ) );
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuZoneBeginAllocSrcLocCallstackSerial );
    tracy::MemWrite( &item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuZoneBegin.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->gpuZoneBegin.srcloc, data.srcloc );
    tracy::MemWrite( &item->gpuZoneBegin.queryId, data.queryId );
    tracy::MemWrite( &item->gpuZoneBegin.context, data.context );
    tracy::Profiler::QueueSerialFinish();
}

#endif

TRACY_API void ___tracy_emit_gpu_time_serial( const struct ___tracy_gpu_time_data data )
{
    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuTime );
    tracy::MemWrite( &item->gpuTime.gpuTime, data.gpuTime );
    tracy::MemWrite( &item->gpuTime.queryId, data.queryId );
    tracy::MemWrite( &item->gpuTime.context, data.context );
    tracy::Profiler::QueueSerialFinish();
}

TRACY_API void ___tracy_emit_gpu_zone_end_serial( const struct ___tracy_gpu_zone_end_data data )
{
    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuZoneEndSerial );
    tracy::MemWrite( &item->gpuZoneEnd.cpuTime, tracy::Profiler::GetTime() );
    memset( &item->gpuZoneEnd.thread, 0, sizeof( item->gpuZoneEnd.thread ) );
    tracy::MemWrite( &item->gpuZoneEnd.queryId, data.queryId );
    tracy::MemWrite( &item->gpuZoneEnd.context, data.context );
    tracy::Profiler::QueueSerialFinish();
}

TRACY_API void ___tracy_emit_gpu_new_context_serial( ___tracy_gpu_new_context_data data )
{
    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuNewContext );
    tracy::MemWrite( &item->gpuNewContext.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuNewContext.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->gpuNewContext.gpuTime, data.gpuTime );
    tracy::MemWrite( &item->gpuNewContext.period, data.period );
    tracy::MemWrite( &item->gpuNewContext.context, data.context );
    tracy::MemWrite( &item->gpuNewContext.flags, data.flags );
    tracy::MemWrite( &item->gpuNewContext.type, data.type );

#ifdef TRACY_ON_DEMAND
    tracy::GetProfiler().DeferItem( *item );
#endif

    tracy::Profiler::QueueSerialFinish();
}

TRACY_API void ___tracy_emit_gpu_context_name_serial( const struct ___tracy_gpu_context_name_data data )
{
    auto ptr = (char*)tracy::tracy_malloc( data.len );
    memcpy( ptr, data.name, data.len );

    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuContextName );
    tracy::MemWrite( &item->gpuContextNameFat.context, data.context );
    tracy::MemWrite( &item->gpuContextNameFat.ptr, (uint64_t)ptr );
    tracy::MemWrite( &item->gpuContextNameFat.size, data.len );

#ifdef TRACY_ON_DEMAND
    tracy::GetProfiler().DeferItem( *item );
#endif

    tracy::Profiler::QueueSerialFinish();
}

TRACY_API void ___tracy_emit_gpu_calibration_serial( const struct ___tracy_gpu_calibration_data data )
{
    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuCalibration );
    tracy::MemWrite( &item->gpuCalibration.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuCalibration.gpuTime, data.gpuTime );
    tracy::MemWrite( &item->gpuCalibration.cpuDelta, data.cpuDelta );
    tracy::MemWrite( &item->gpuCalibration.context, data.context );
    tracy::Profiler::QueueSerialFinish();
}

TRACY_API void ___tracy_emit_gpu_time_sync_serial( const struct ___tracy_gpu_time_sync_data data )
{
    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuTimeSync );
    tracy::MemWrite( &item->gpuTimeSync.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuTimeSync.gpuTime, data.gpuTime );
    tracy::MemWrite( &item->gpuTimeSync.context, data.context );
    tracy::Profiler::QueueSerialFinish();
}

struct __tracy_lockable_context_data
{
    uint32_t m_id;
#ifdef TRACY_ON_DEMAND
    std::atomic<uint32_t> m_lockCount;
    std::atomic<bool> m_active;
#endif
};

TRACY_API struct __tracy_lockable_context_data* ___tracy_announce_lockable_ctx( const struct ___tracy_source_location_data* srcloc )
{
    struct __tracy_lockable_context_data *lockdata = (__tracy_lockable_context_data*)tracy::tracy_malloc( sizeof( __tracy_lockable_context_data ) );
    lockdata->m_id =tracy:: GetLockCounter().fetch_add( 1, std::memory_order_relaxed );
#ifdef TRACY_ON_DEMAND
    new(&lockdata->m_lockCount) std::atomic<uint32_t>( 0 );
    new(&lockdata->m_active) std::atomic<bool>( false );
#endif
    assert( lockdata->m_id != (std::numeric_limits<uint32_t>::max)() );

    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::LockAnnounce );
    tracy::MemWrite( &item->lockAnnounce.id, lockdata->m_id );
    tracy::MemWrite( &item->lockAnnounce.time, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->lockAnnounce.lckloc, (uint64_t)srcloc );
    tracy::MemWrite( &item->lockAnnounce.type, tracy::LockType::Lockable );
#ifdef TRACY_ON_DEMAND
    tracy::GetProfiler().DeferItem( *item );
#endif
    tracy::Profiler::QueueSerialFinish();

    return lockdata;
}

TRACY_API void ___tracy_terminate_lockable_ctx( struct __tracy_lockable_context_data* lockdata )
{
    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::LockTerminate );
    tracy::MemWrite( &item->lockTerminate.id, lockdata->m_id );
    tracy::MemWrite( &item->lockTerminate.time, tracy::Profiler::GetTime() );
#ifdef TRACY_ON_DEMAND
    tracy::GetProfiler().DeferItem( *item );
#endif
    tracy::Profiler::QueueSerialFinish();

#ifdef TRACY_ON_DEMAND
    lockdata->m_lockCount.~atomic();
    lockdata->m_active.~atomic();
#endif
    tracy::tracy_free((void*)lockdata);
}

TRACY_API int ___tracy_before_lock_lockable_ctx( struct __tracy_lockable_context_data* lockdata )
{
#ifdef TRACY_ON_DEMAND
    bool queue = false;
    const auto locks = lockdata->m_lockCount.fetch_add( 1, std::memory_order_relaxed );
    const auto active = lockdata->m_active.load( std::memory_order_relaxed );
    if( locks == 0 || active )
    {
        const bool connected = tracy::GetProfiler().IsConnected();
        if( active != connected ) lockdata->m_active.store( connected, std::memory_order_relaxed );
        if( connected ) queue = true;
    }
    if( !queue ) return false;
#endif

    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::LockWait );
    tracy::MemWrite( &item->lockWait.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->lockWait.id, lockdata->m_id );
    tracy::MemWrite( &item->lockWait.time, tracy::Profiler::GetTime() );
    tracy::Profiler::QueueSerialFinish();
    return true;
}

TRACY_API void ___tracy_after_lock_lockable_ctx( struct __tracy_lockable_context_data* lockdata )
{
    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::LockObtain );
    tracy::MemWrite( &item->lockObtain.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->lockObtain.id, lockdata->m_id );
    tracy::MemWrite( &item->lockObtain.time, tracy::Profiler::GetTime() );
    tracy::Profiler::QueueSerialFinish();
}

TRACY_API void ___tracy_after_unlock_lockable_ctx( struct __tracy_lockable_context_data* lockdata )
{
#ifdef TRACY_ON_DEMAND
    lockdata->m_lockCount.fetch_sub( 1, std::memory_order_relaxed );
    if( !lockdata->m_active.load( std::memory_order_relaxed ) ) return;
    if( !tracy::GetProfiler().IsConnected() )
    {
        lockdata->m_active.store( false, std::memory_order_relaxed );
        return;
    }
#endif

    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::LockRelease );
    tracy::MemWrite( &item->lockRelease.id, lockdata->m_id );
    tracy::MemWrite( &item->lockRelease.time, tracy::Profiler::GetTime() );
    tracy::Profiler::QueueSerialFinish();
}

TRACY_API void ___tracy_after_try_lock_lockable_ctx( struct __tracy_lockable_context_data* lockdata, int acquired )
{
#ifdef TRACY_ON_DEMAND
    if( !acquired ) return;

    bool queue = false;
    const auto locks = lockdata->m_lockCount.fetch_add( 1, std::memory_order_relaxed );
    const auto active = lockdata->m_active.load( std::memory_order_relaxed );
    if( locks == 0 || active )
    {
        const bool connected = tracy::GetProfiler().IsConnected();
        if( active != connected ) lockdata->m_active.store( connected, std::memory_order_relaxed );
        if( connected ) queue = true;
    }
    if( !queue ) return;
#endif

    if( acquired )
    {
        auto item = tracy::Profiler::QueueSerial();
        tracy::MemWrite( &item->hdr.type, tracy::QueueType::LockObtain );
        tracy::MemWrite( &item->lockObtain.thread, tracy::GetThreadHandle() );
        tracy::MemWrite( &item->lockObtain.id, lockdata->m_id );
        tracy::MemWrite( &item->lockObtain.time, tracy::Profiler::GetTime() );
        tracy::Profiler::QueueSerialFinish();
    }
}

TRACY_API void ___tracy_mark_lockable_ctx( struct __tracy_lockable_context_data* lockdata, const struct ___tracy_source_location_data* srcloc )
{
#ifdef TRACY_ON_DEMAND
    const auto active = lockdata->m_active.load( std::memory_order_relaxed );
    if( !active ) return;
    const auto connected = tracy::GetProfiler().IsConnected();
    if( !connected )
    {
        if( active ) lockdata->m_active.store( false, std::memory_order_relaxed );
        return;
    }
#endif

    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::LockMark );
    tracy::MemWrite( &item->lockMark.thread, tracy::GetThreadHandle() );
    tracy::MemWrite( &item->lockMark.id, lockdata->m_id );
    tracy::MemWrite( &item->lockMark.srcloc, (uint64_t)srcloc );
    tracy::Profiler::QueueSerialFinish();
}

TRACY_API void ___tracy_custom_name_lockable_ctx( struct __tracy_lockable_context_data* lockdata, const char* name, size_t nameSz )
{
    assert( nameSz < (std::numeric_limits<uint16_t>::max)() );
    auto ptr = (char*)tracy::tracy_malloc( nameSz );
    memcpy( ptr, name, nameSz );
    auto item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::LockName );
    tracy::MemWrite( &item->lockNameFat.id, lockdata->m_id );
    tracy::MemWrite( &item->lockNameFat.name, (uint64_t)ptr );
    tracy::MemWrite( &item->lockNameFat.size, (uint16_t)nameSz );
#ifdef TRACY_ON_DEMAND
    tracy::GetProfiler().DeferItem( *item );
#endif
    tracy::Profiler::QueueSerialFinish();
}

TRACY_API int ___tracy_connected( void )
{
    return tracy::GetProfiler().IsConnected();
}

#ifdef TRACY_FIBERS
TRACY_API void ___tracy_fiber_enter( const char* fiber ){ tracy::Profiler::EnterFiber( fiber, 0 ); }
TRACY_API void ___tracy_fiber_leave( void ){ tracy::Profiler::LeaveFiber(); }
#endif

#  ifdef TRACY_MANUAL_LIFETIME
TRACY_API void ___tracy_startup_profiler( void )
{
    tracy::StartupProfiler();
}

TRACY_API void ___tracy_shutdown_profiler( void )
{
    tracy::ShutdownProfiler();
}

TRACY_API int ___tracy_profiler_started( void )
{
    return tracy::s_isProfilerStarted.load( std::memory_order_seq_cst );
}
#  endif

TRACY_API void SetupTracyExternalApi( TracyExternalAPI_t *pApi )
{
    pApi->_emitZoneBegin = (TracyExternalEmitZoneBeginPtr)___tracy_emit_zone_begin;
    pApi->_emitZoneEnd = (TracyExternalEmitZoneEndPtr)___tracy_emit_zone_end;
    pApi->_setThreadName = ___tracy_set_thread_name;
}

#ifdef __cplusplus
}
#endif

#endif
