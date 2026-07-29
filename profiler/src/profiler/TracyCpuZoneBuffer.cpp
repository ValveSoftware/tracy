#include <algorithm>
#include <chrono>

#include "TracyCpuZoneBuffer.hpp"

#include "TracyTimelineContext.hpp"
#include "TracyTimelinePreprocessor.hpp"


namespace tracy
{

//#define TRACY_CORE_ZONE_VALIDATION


#if defined( TRACY_CORE_ZONE_VALIDATION )
#   define ValidateCondition(cond) assert(cond)
#else
#   define ValidateCondition(...)
#endif


static const int64_t s_MaxZoneListTimeThreshold = 10;


#if defined(TRACY_CORE_ZONE_VALIDATION)
static void ValidateZoneBuffer( const CpuZoneBuffer& zoneBuffer, const Vector<ContextSwitchCpu> &cslist )
{
    size_t fullCalc = 0;
    for ( size_t index = 0, count = zoneBuffer.BufferCount(); index < count; index++ )
    {
        const CpuZonePool *zb = zoneBuffer.GetBuffer( index );
        fullCalc += zb->fillCount;
    }
    assert( fullCalc == zoneBuffer.GetZoneCount() );

    for ( size_t index = 0, count = zoneBuffer.BufferCount(); index < count; index++ )
    {
        const CpuZonePool *zb = zoneBuffer.GetBuffer( index );
        if ( ( index + 1 ) < count )
        {
            assert( zb->distPool[ CpuZonePool::ZonePoolItemCount ].distToNext == CpuZoneBuffer::s_NextPool );
        }
        else
        {
            assert( zb->distPool[ CpuZonePool::ZonePoolItemCount ].distToNext == CpuZoneBuffer::s_EndOfPool );
        }

        const CpuZoneInfo &first = zb->infoPool[ 0 ];
        const CpuZoneInfo &last = zb->infoPool[ zb->fillCount - 1 ];
        assert( zb->csIndexRange.first == first.csIndex );
        assert( zb->csIndexRange.last == last.csIndex );
    }

    if ( zoneBuffer.BufferCount() > 0 )
    {
        size_t prevGlobalIndex = 0;
        size_t prevPoolIndex = 0;
        size_t prevElemIndex = 0;
        for ( size_t globalIndex = 1; globalIndex < zoneBuffer.GetZoneCount(); globalIndex++ )
        {
            assert( prevPoolIndex < zoneBuffer.BufferCount() );
            assert( prevElemIndex < CpuZonePool::ZonePoolItemCount );

            const CpuZoneInfo *prevInfo = zoneBuffer.ZoneInfo( prevGlobalIndex );
            const CpuZoneDist *prevDist = zoneBuffer.ZoneDist( prevGlobalIndex );
            const ContextSwitchCpu *prevCs = &cslist[ prevInfo->csIndex ];

            const int64_t prevZoneStart = prevInfo->pEvent->Start();
            const int64_t prevZoneEnd = prevInfo->pEvent->End();
            const int64_t prevCsStart = prevCs->Start();
            const int64_t prevCsEnd = prevCs->End();
            const int64_t prevClampedStart = std::max( prevZoneStart, prevCsStart );
            const int64_t prevClampedEnd = std::min( prevZoneEnd, prevCsEnd );

            const size_t curPoolIndex = ( globalIndex / CpuZonePool::ZonePoolItemCount );
            const size_t curElemIndex = ( globalIndex % CpuZonePool::ZonePoolItemCount );

            assert( curPoolIndex < zoneBuffer.BufferCount() );
            assert( curElemIndex < CpuZonePool::ZonePoolItemCount );

            const CpuZoneInfo *curInfo = zoneBuffer.ZoneInfo( globalIndex );
            const ContextSwitchCpu *curCs = &cslist[ curInfo->csIndex ];

            const int64_t curZoneStart = curInfo->pEvent->Start();
            const int64_t curZoneEnd = curInfo->pEvent->End();
            const int64_t curCsStart = curCs->Start();
            const int64_t curCsEnd = curCs->End();
            const int64_t curClampedStart = std::max( curZoneStart, curCsStart );
            const int64_t curClampedEnd = std::min( curZoneEnd, curCsEnd );

            const int64_t diff = (curClampedEnd - prevClampedEnd);

            assert( prevClampedStart <= prevClampedEnd );
            assert( curClampedStart <= curClampedEnd );
            assert( prevClampedEnd <= curClampedEnd );
            assert( prevInfo->csIndex <= curInfo->csIndex );
            assert( ( prevInfo->csIndex == curInfo->csIndex ) || (prevCsEnd <= curCsStart) );
            assert( prevClampedEnd <= curClampedStart );
            assert( diff == prevDist->distToNext );

            prevGlobalIndex = globalIndex;
            prevPoolIndex = curPoolIndex;
            prevElemIndex = curElemIndex;
        }
    }
}


static void ValidateZoneBufferRange( const CpuZoneBuffer& zoneBuffer, const Vector<ContextSwitchCpu> &cslist, size_t prevIndex, size_t curIndex )
{
    if ( prevIndex != curIndex )
    {
        ValidateZoneBuffer( zoneBuffer, cslist );
    }
}
#endif // if defined(TRACY_CORE_ZONE_VALIDATION)


CpuZoneBuffer::~CpuZoneBuffer()
{
    Clear();
}


void CpuZoneBuffer::Clear()
{
    m_lastCsIndex = 0;
    m_zoneCount = 0;
    m_validCsCount = 0;
    for ( CpuZonePool *buf : m_buffers )
    {
        delete buf;
    }

    m_buffers.clear();
}


int CpuZoneBuffer::Update( Worker& worker, const Vector<const ThreadData *> &threadLut, const Vector<ContextSwitchCpu> &cslist, bool shouldTimeSlice )
{
#if defined( TRACY_CORE_ZONE_VALIDATION )
    size_t oldCheckIndex = m_lastCsIndex;
#endif

    int depth = 0;

    if ( cslist.empty() )
    {
        return depth;
    }

    auto it = cslist.begin() + m_lastCsIndex;
    const auto eit = cslist.end();
    if ( it != eit )
    {
        // Make sure we also update the currently active buffer since it may not have
        // been full yet, and the cs range can change
        size_t prevZoneLinkBufferCount = ( m_buffers.empty() ? 0 : ( m_buffers.size() - 1 ) );

        int64_t prevEnd = 0;
        if ( !m_buffers.empty() )
        {
            const CpuZonePool *zb = m_buffers.back();
            ValidateCondition( zb );
            const CpuZoneInfo &last = zb->infoPool[ zb->fillCount - 1 ];
            const ContextSwitchCpu &lastCs = cslist[ last.csIndex ];

            const int64_t zoneEnd = last.pEvent->End();
            const int64_t csEnd = lastCs.End();
            prevEnd = std::min( zoneEnd, csEnd );
        }

        const auto buildStart = std::chrono::high_resolution_clock::now();
        while ( ( it < eit ) && it->IsEndValid() )
        {
            const ContextSwitchCpu &cs = *it;
            const size_t csIndex = ( it - cslist.begin() );
            const uint16_t comprTid = cs.Thread();
            const ThreadData *td = threadLut[ comprTid ];

            if ( td )
            {
                const int64_t start = cs.Start();
                const int64_t end = cs.End();

                TimelinePreprocessor preproc( worker, 0, 0 );
                int d = preproc.CalculateMaxZoneDepthInRange( td->timeline, start, end );
                depth = std::max( d, depth );

                if ( !BuildZoneListAt( worker, td->timeline, start, end, csIndex, prevEnd ) )
                {
                    break;
                }
            }

            it++;

            if ( shouldTimeSlice )
            {
                // Make sure we don't process for too long, but also force at least some progress
                const auto now = std::chrono::high_resolution_clock::now();
                const int64_t ms = std::chrono::duration_cast< std::chrono::milliseconds >( now - buildStart ).count();
                if ( ( ms > s_MaxZoneListTimeThreshold ) && ( csIndex > m_lastCsIndex ) )
                {
                    break;
                }
            }
        }

        m_lastCsIndex = ( it - cslist.begin() );

        for ( size_t bufferIndex = prevZoneLinkBufferCount, bufferCount = m_buffers.size(); bufferIndex < bufferCount; bufferIndex++ )
        {
            CpuZonePool *zb = GetBuffer( bufferIndex );
            const CpuZoneInfo &first = zb->infoPool[ 0 ];
            const CpuZoneInfo &last = zb->infoPool[ zb->fillCount - 1 ];
            ContextSwitchIndexRange range = { first.csIndex, last.csIndex };
            zb->csIndexRange = range;
        }

        m_zoneCount = 0;
        if ( !m_buffers.empty() )
        {
            m_zoneCount = ( ( ( m_buffers.size() - 1 ) * CpuZonePool::ZonePoolItemCount ) + m_buffers.back()->fillCount );
        }

        auto validCsIt = cslist.begin() + m_validCsCount;
        while ( ( validCsIt != eit ) && ( validCsIt->IsEndValid() ) )
        {
            m_validCsCount++;
            validCsIt++;
        }
    }

#if defined( TRACY_CORE_ZONE_VALIDATION )
    ValidateZoneBufferRange( *this, cslist, oldCheckIndex, m_lastCsIndex );
#endif // if defined( TRACY_CORE_ZONE_VALIDATION )

    m_maxDepth = std::max( depth, m_maxDepth );
    return m_maxDepth;
}


bool CpuZoneBuffer::HasMoreDataToProcess() const
{
    return ( m_validCsCount > m_lastCsIndex );
}


size_t CpuZoneBuffer::BufferCount() const
{
    return m_buffers.size();
}


size_t CpuZoneBuffer::GetZoneCount() const
{
    return m_zoneCount;
}


size_t CpuZoneBuffer::GetProcessedCount() const
{
    return m_lastCsIndex;
}


ContextSwitchCpuRange CpuZoneBuffer::FindContextSwitchCpuRange( int64_t start, int64_t end, const Vector<ContextSwitchCpu> &cslist )
{
    ContextSwitchCpuRange result;
    result.beg = result.end = 0;

    if ( cslist.empty() )
    {
        return result;
    }

    const auto begIt = std::lower_bound( cslist.begin(), cslist.end(), std::max<int64_t>( 0, start ), [] ( const auto& l, const auto& r ) { return ( l.IsEndValid() ? l.End() : l.Start() ) < r; } );
    if( begIt == cslist.end() )
    {
        return result;
    }

    const auto endIt = std::lower_bound( begIt, cslist.end(), end, [] ( const auto& l, const auto& r ) { return l.Start() < r; } );

    const size_t begIndex = (begIt - cslist.begin());
    const size_t endIndex = (endIt - cslist.begin());

    if ( m_validCsCount > begIndex )
    {
        result.beg = begIndex;
        result.end = std::min( endIndex, m_validCsCount );
    }

    result.beg = begIndex;
    result.end = endIndex;
    return result;
}


CpuZoneRange CpuZoneBuffer::FindCpuZoneRange( int64_t start, int64_t end, ContextSwitchCpuRange csRange )
{
    CpuZoneRange result = { m_zoneCount, m_zoneCount };
    if ( csRange.beg == csRange.end )
    {
        return result;
    }

    {
        auto rangeStart = std::lower_bound( m_buffers.begin(), m_buffers.end(), csRange.beg,
                                            [] ( const CpuZonePool *l, const size_t &r )
                                            {
                                                return l->csIndexRange.last < r;
                                            } );

        if ( rangeStart != m_buffers.end() )
        {
            size_t rangePoolIndex = ( rangeStart - m_buffers.begin() );
            const CpuZonePool *zb = GetBuffer( rangePoolIndex );
            auto elemIt = std::lower_bound( zb->infoPool.begin(), zb->infoPool.begin() + zb->fillCount, csRange.beg,
                                            [] ( const CpuZoneInfo &l, const size_t &r )
                                            {
                                                return l.csIndex < r;
                                            } );

            result.beg = ( rangePoolIndex * CpuZonePool::ZonePoolItemCount );
            if ( elemIt != zb->infoPool.end() )
            {
                size_t elemIndex = ( elemIt - zb->infoPool.begin() );
                result.beg += elemIndex;
            }

            while ( result.beg < m_zoneCount )
            {
                const CpuZoneInfo *info = ZoneInfo( result.beg );
                if ( info->pEvent->End() >= start )
                {
                    break;
                }

                result.beg++;
            }
        }
    }

    {
        const size_t ctxEnd = std::min( csRange.end, m_lastCsIndex );
        auto rangeEnd = std::lower_bound( m_buffers.begin(), m_buffers.end(), ctxEnd,
                                          [] ( const CpuZonePool *l, const size_t &r )
                                          {
                                              return l->csIndexRange.last < r;
                                          } );

        if ( rangeEnd != m_buffers.end() )
        {
            size_t rangePoolIndex = ( rangeEnd - m_buffers.begin() );
            CpuZonePool *zb = GetBuffer( rangePoolIndex );
            auto elemIt = std::lower_bound( zb->infoPool.begin(), zb->infoPool.begin() + zb->fillCount, ctxEnd,
                                            [] ( const CpuZoneInfo &l, const size_t &r )
                                            {
                                                return l.csIndex < r;
                                            } );

            size_t elemIndex = ( elemIt - zb->infoPool.begin() );
            ValidateCondition( elemIndex < zb->distPool.size() );

            size_t globalIndex = ( rangePoolIndex * CpuZonePool::ZonePoolItemCount );
            if ( elemIt != zb->infoPool.end() )
            {
                globalIndex += elemIndex;
            }

            for ( ; globalIndex < m_zoneCount; globalIndex++ )
            {
                const size_t zonePool = ( globalIndex / CpuZonePool::ZonePoolItemCount );
                const size_t zoneElem = ( globalIndex % CpuZonePool::ZonePoolItemCount );
                zb = GetBuffer( zonePool );
                const CpuZoneInfo &info = zb->infoPool[ zoneElem ];
                if ( info.csIndex != ctxEnd )
                {
                    break;
                }
            }

            result.end = globalIndex;
        }

        if ( result.beg < result.end )
        {
            while ( (result.end - 1) > result.beg )
            {
                const CpuZoneInfo *info = ZoneInfo( result.end - 1 );
                if ( info->pEvent->Start() <= end )
                {
                    break;
                }

                result.end--;
            }
        }

        if ( result.beg > result.end )
        {
            result.beg = result.end = 0;
        }
    }

    return result;
}


CpuZoneInfo *CpuZoneBuffer::ZoneInfo( size_t globalIndex )
{
    const size_t zonePool = ( globalIndex / CpuZonePool::ZonePoolItemCount );
    const size_t zoneElem = ( globalIndex % CpuZonePool::ZonePoolItemCount );
    ValidateCondition( zonePool < m_buffers.size() );
    ValidateCondition( zoneElem < m_buffers[ zonePool ]->fillCount );
    return &m_buffers[ zonePool ]->infoPool[ zoneElem ];
}


CpuZoneDist *CpuZoneBuffer::ZoneDist( size_t globalIndex )
{
    const size_t zonePool = ( globalIndex / CpuZonePool::ZonePoolItemCount );
    const size_t zoneElem = ( globalIndex % CpuZonePool::ZonePoolItemCount );
    ValidateCondition( zonePool < m_buffers.size() );
    ValidateCondition( zoneElem < m_buffers[ zonePool ]->fillCount );
    return &m_buffers[ zonePool ]->distPool[ zoneElem ];
}


const CpuZoneInfo *CpuZoneBuffer::ZoneInfo( size_t globalIndex ) const
{
    const size_t zonePool = ( globalIndex / CpuZonePool::ZonePoolItemCount );
    const size_t zoneElem = ( globalIndex % CpuZonePool::ZonePoolItemCount );
    ValidateCondition( zonePool < m_buffers.size() );
    ValidateCondition( zoneElem < m_buffers[ zonePool ]->fillCount );
    return &m_buffers[ zonePool ]->infoPool[ zoneElem ];
}


const CpuZoneDist *CpuZoneBuffer::ZoneDist( size_t globalIndex ) const
{
    const size_t zonePool = ( globalIndex / CpuZonePool::ZonePoolItemCount );
    const size_t zoneElem = ( globalIndex % CpuZonePool::ZonePoolItemCount );
    ValidateCondition( zonePool < m_buffers.size() );
    ValidateCondition( zoneElem < m_buffers[ zonePool ]->fillCount );
    return &m_buffers[ zonePool ]->distPool[ zoneElem ];
}


const CpuZonePool *CpuZoneBuffer::GetBuffer( size_t bufferIndex ) const
{
    ValidateCondition( bufferIndex < m_buffers.size() );
    return m_buffers[ bufferIndex ];
}


CpuZonePool *CpuZoneBuffer::GetBuffer( size_t bufferIndex )
{
    ValidateCondition( bufferIndex < m_buffers.size() );
    return m_buffers[ bufferIndex ];
}


CpuZonePool *CpuZoneBuffer::AddNewCpuZoneBuffer()
{
    CpuZonePool *result = new CpuZonePool;
    memset( result, 0, sizeof( *result ) );

    if ( !m_buffers.empty() )
    {
        m_buffers.back()->distPool[CpuZonePool::ZonePoolItemCount].distToNext = CpuZoneBuffer::s_NextPool;
    }

    // Sentinel for loop comparison. This will always fail, so it acts both
    // as an end of list and end of buffer sentinel
    result->distPool[ CpuZonePool::ZonePoolItemCount ].distToNext = CpuZoneBuffer::s_EndOfPool;
    m_buffers.push_back( result );
    return result;
}


template<typename Adapter, typename V>
bool CpuZoneBuffer::BuildZoneListAt( Worker& worker, const V &vec, int64_t start, int64_t end, size_t csIndex, int64_t& rPrevEnd )
{
    bool result = true;
    auto it = std::lower_bound( vec.begin(), vec.end(), start, [&worker] ( const auto& l, const auto& r ) { Adapter a; return worker.GetZoneEnd( a(l) ) < r; } );
    if( it == vec.end() )
    {
        return result;
    }

    const auto zitend = std::lower_bound( it, vec.end(), end, [] ( const auto& l, const auto& r ) { Adapter a; return a(l).Start() < r; } );

    if( it == zitend )
    {
        return result;
    }

    Adapter a;
    if ( !a( *it ).IsEndValid() || !a(*(zitend-1)).IsEndValid() )
    {
        result = false;
        return result;
    }

    ZoneEvent nullevent;
    memset( &nullevent, 0, sizeof(nullevent) );
    CpuZoneInfo nullinfo = {0};
    nullinfo.pEvent = &nullevent;

    CpuZoneDist nulldist = {0};

    CpuZonePool* zb = nullptr;
    CpuZoneDist* prev = &nulldist;
    if ( !m_buffers.empty() )
    {
        zb = m_buffers.back();
        prev = &zb->distPool[ zb->fillCount - 1 ];
    }

    if ( m_buffers.empty() || ( m_buffers.back()->fillCount == CpuZonePool::ZonePoolItemCount ) )
    {
        zb = AddNewCpuZoneBuffer();
    }

    ValidateCondition( zb && ( zb->fillCount < CpuZonePool::ZonePoolItemCount ) );

    int64_t prevEnd = rPrevEnd;
    while ( it < zitend )
    {
        const ZoneEvent &ev = a( *it );
        ValidateCondition( ev.IsEndValid() );

        if ( zb->fillCount == CpuZonePool::ZonePoolItemCount )
        {
            zb = AddNewCpuZoneBuffer();
        }

        CpuZoneInfo *info = &zb->infoPool[ zb->fillCount ];
        CpuZoneDist *dist = &zb->distPool[ zb->fillCount ];
        zb->fillCount++;

        const int64_t clampedEnd = std::min( ev.End(), end );
        dist->distToNext = CpuZoneBuffer::s_EndOfZones;
        info->pEvent = &ev;
        info->csIndex = csIndex;

        const int64_t diff = ( clampedEnd - prevEnd );
        prev->distToNext = diff;
        prev = dist;
        prevEnd = clampedEnd;

        it++;
    }

    rPrevEnd = prevEnd;
    return result;
}


bool CpuZoneBuffer::BuildZoneListAt( Worker& worker, const Vector<short_ptr<ZoneEvent>> &vec, int64_t start, int64_t end, size_t csIndex, int64_t& rPrevEnd )
{
    if ( vec.is_magic() )
    {
        return BuildZoneListAt<VectorAdapterDirect<ZoneEvent>>( worker, *(const Vector<ZoneEvent>*)( &vec ), start, end, csIndex, rPrevEnd );
    }
    else
    {
        return BuildZoneListAt<VectorAdapterPointer<ZoneEvent>>( worker, vec, start, end, csIndex, rPrevEnd );
    }
}


}
