#ifndef __TRACYCPUZONEBUFFER_HPP__
#define __TRACYCPUZONEBUFFER_HPP__


#include <stdint.h>
#include <array>
#include <vector>
#include "TracyVector.hpp"
#include "TracyShortPtr.hpp"
#include "TracyEvent.hpp"
#include "TracyWorker.hpp"


namespace tracy
{


struct ZoneEvent;
struct ContextSwitchCpu;
struct ThreadData;


struct ContextSwitchCpuRange
{
    size_t beg;
    size_t end;
};


struct CpuZoneRange
{
    size_t beg;
    size_t end;
};


struct ContextSwitchIndexRange
{
    size_t first;
    size_t last;
};


struct CpuZoneInfo
{
    const ZoneEvent *pEvent;
    size_t csIndex;
};


struct CpuZoneDist
{
    int64_t distToNext;
};


struct CpuZonePool
{
    enum { ZonePoolItemCount = 32 * 1024 };
    static_assert( ZonePoolItemCount != 0 && (ZonePoolItemCount & (ZonePoolItemCount - 1)) == 0, "ZonePoolItemCount must be a power of 2!" );

    // The last element in the dist pool is a sentinel.
    // It is set to various magic values that all will be larger than the maximum allowed valid distance between zones.
    typedef std::array<CpuZoneDist, ZonePoolItemCount + 1> ZoneDistPool;
    typedef std::array<CpuZoneInfo, ZonePoolItemCount> ZoneInfoPool;

    ZoneDistPool distPool;
    ZoneInfoPool infoPool;
    size_t fillCount;
    ContextSwitchIndexRange csIndexRange;
};


struct CpuZoneBuffer
{
public:
    // Special sentinel values for the dist pool
    static const int64_t s_NextPool   = ( INT64_MAX - 0 );
    static const int64_t s_EndOfPool  = ( INT64_MAX - 1 );
    static const int64_t s_EndOfZones = ( INT64_MAX - 2 );
    static const int64_t s_EndOfRange = ( INT64_MAX - 3 );

    ~CpuZoneBuffer();

    void Clear();
    int Update( Worker &worker, const Vector<const ThreadData *> &threadLut, const Vector<ContextSwitchCpu> &cslist, bool shouldTimeSlice );

    bool HasMoreDataToProcess() const;
    size_t BufferCount() const;
    size_t GetZoneCount() const;

    size_t GetProcessedCount() const;

    ContextSwitchCpuRange FindContextSwitchCpuRange( int64_t start, int64_t end, const Vector<ContextSwitchCpu> &cslist );
    CpuZoneRange FindCpuZoneRange( int64_t start, int64_t end, ContextSwitchCpuRange ContextSwitchCpuRange );

    CpuZoneInfo *ZoneInfo( size_t globalIndex );
    CpuZoneDist *ZoneDist( size_t globalIndex );

    const CpuZoneInfo *ZoneInfo( size_t globalIndex ) const;
    const CpuZoneDist *ZoneDist( size_t globalIndex ) const;

    const CpuZonePool *GetBuffer( size_t bufferIndex ) const;

private:
    CpuZonePool *GetBuffer( size_t bufferIndex );
    CpuZonePool *AddNewCpuZoneBuffer();

    template<typename Adapter, typename V>
    bool BuildZoneListAt( Worker &worker, const V &vec, int64_t start, int64_t end, size_t csIndex, int64_t &rPrevEnd );
    bool BuildZoneListAt( Worker &worker, const Vector<short_ptr<ZoneEvent>> &vec, int64_t start, int64_t end, size_t csIndex, int64_t &rPrevEnd );

    std::vector<CpuZonePool *> m_buffers;
    size_t m_lastCsIndex = 0;
    size_t m_zoneCount = 0;
    size_t m_validCsCount = 0;
    int m_maxDepth = 0;
};


}


#endif
