#include <algorithm>
#include <chrono>

#include "TracyTimelineItemCore.hpp"

#include "TracyImGui.hpp"
#include "TracyLockHelpers.hpp"
#include "TracyEvent.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyTimelinePreprocessor.hpp"
#include "TracyView.hpp"
#include "TracyWorker.hpp"


namespace tracy
{

constexpr float MinVisSize = 3;
constexpr float MinCtxSize = 4;
constexpr float MinHwCounterVisSize = 3;


TimelineItemCore::TimelineItemCore( View& view, Worker& worker, const CpuData* cpuData )
    : TimelineItem( view, worker, cpuData, true )
{
    assert( cpuData );
    const CpuData *allCpuData = m_worker.GetCpuData();
    ptrdiff_t index = ( cpuData - allCpuData );
    assert( index >= 0 );
    uint32_t cpuThread = (uint32_t)index;

    const Worker::CpuThreadTopology *threadTopo = m_worker.GetThreadTopology( CpuThreadId{ cpuThread } );
    m_coreInfo.package = threadTopo ? threadTopo->package : CpuPackageId{ 0 };
    m_coreInfo.core = threadTopo ? threadTopo->core : CpuCoreId{ 0 };
    m_coreInfo.cpuThread = CpuThreadId{ cpuThread };
    m_coreInfo.cpuData = cpuData;

    m_Name = m_worker.GetFormattedCpuName( cpuThread );

    m_maxDepth = 0;
    m_depth = 0;
    m_keepActive = false;

    ViewData &vd = view.GetViewData();
    if ( vd.cores.contains( cpuThread ) )
    {
        ViewData::Track& coreSettings = vd.cores[ cpuThread ];
        m_trackUiData.settings = coreSettings.ui;
        SetVisible( coreSettings.visible );
    }
    else
    {
        m_trackUiData.settings.shouldOverride = false;
        m_trackUiData.settings.stackCollapseMode = vd.coreCollapseMode;
        m_trackUiData.settings.stackCollapseClamp = vd.coreCollapseClamp;
    }

    m_trackUiData.preventScroll = 0;
}


bool TimelineItemCore::IsEmpty() const
{
    return ( ( m_coreInfo.cpuData == nullptr ) || m_coreInfo.cpuData->cs.empty() );
}


uint32_t TimelineItemCore::HeaderColor() const
{
    return 0xFFFFFFFF;
}


uint32_t TimelineItemCore::HeaderColorInactive() const
{
    return 0xFF888888;
}


uint32_t TimelineItemCore::HeaderLineColor() const
{
    return 0x33FFFFFF;
}


const char* TimelineItemCore::HeaderLabel() const
{
    return m_Name.c_str();
}


int64_t TimelineItemCore::RangeBegin() const
{
    return -1;
}


int64_t TimelineItemCore::RangeEnd() const
{
    return -1;
}


void TimelineItemCore::HeaderTooltip( const char* label ) const
{
    ImGui::BeginTooltip();
    SmallColorBox( GetThreadColor(  m_coreInfo.cpuThread.val, 0, m_view.GetViewData().dynamicColors ) );
    ImGui::SameLine();
    ImGui::TextUnformatted( label );

    const Vector<ContextSwitchCpu>& cs = m_coreInfo.cpuData->cs;
    if ( !cs.empty() )
    {
        const ContextSwitchCpu &first = cs.front();
        const ContextSwitchCpu &last = cs.back();
        const int64_t firstTime = first.Start();
        const int64_t lastTime = (last.IsEndValid() ? last.End() : last.Start());
        const int64_t count = cs.size();

        ImGui::Separator();
        TextFocused( "Context Switch Count", RealToString( count ) );
        TextFocused( "Processed context switches", RealToString( m_cpuZoneBuffer.GetProcessedCount() ));
        TextFocused( "First Context Switch at", TimeToStringExact( firstTime ) );
        TextFocused( "Last Context Switch at:", TimeToStringExact( lastTime ) );
    }

    if ( m_cpuZoneBuffer.GetZoneCount() )
    {
        const size_t memUsage = ( m_cpuZoneBuffer.BufferCount() * sizeof( CpuZonePool ) );

        ImGui::Separator();
        TextFocused( "Top-level zones:", RealToString( m_cpuZoneBuffer.GetZoneCount() ) );
        TextFocused( "Zone buffer count:", RealToString( m_cpuZoneBuffer.BufferCount() ) );
        TextFocused( "Total zone buffer memory:", MemSizeToString( memUsage ) );
    }
    ImGui::EndTooltip();
}


bool TimelineItemCore::DrawContents( const TimelineContext& ctx, int& offset )
{
    uint8_t stackCollapseMode = m_view.GetViewData().coreCollapseMode;
    int32_t stackCollapseClamp = m_view.GetViewData().coreCollapseClamp;
    if ( m_trackUiData.settings.shouldOverride )
    {
        stackCollapseMode = m_trackUiData.settings.stackCollapseMode;
        stackCollapseClamp = m_trackUiData.settings.stackCollapseClamp;
    }

    int depth = m_depth;
    if ( stackCollapseMode == ViewData::CollapseMax )
    {
        depth = m_maxDepth;
    }
    else if ( stackCollapseMode == ViewData::CollapseLimit )
    {
        depth = std::min(stackCollapseClamp, m_maxDepth);
    }

    m_view.DrawCpuTrack( ctx, m_coreInfo.cpuThread.val, m_draw, m_ctxDraw, m_hwCounterDraw, offset, depth);
    return true;
}


bool TimelineItemCore::PreventScrolling() const
{
    return (m_trackUiData.preventScroll > 0);
}


void TimelineItemCore::DrawUiControls( const TimelineContext &ctx, int start, int &offset,float xOffset )
{
    ViewData::Track& rCoreSettings = m_view.GetViewData().cores[ m_coreInfo.cpuThread.val ];
    m_view.DrawTrackUiControls( ctx, HeaderLabel(), m_maxDepth, m_depth, m_trackUiData, rCoreSettings, start, offset, xOffset );
}


void TimelineItemCore::DrawFinished()
{
    m_draw.clear();
    m_ctxDraw.clear();
    m_hwCounterDraw.Reset();

    if ( m_keepActive && m_cpuZoneBuffer.HasMoreDataToProcess() )
    {
        s_wasActive = true;
    }
}


void TimelineItemCore::Preprocess( const TimelineContext& ctx, TaskDispatch& td, bool visible, int yPos )
{
    assert( m_draw.empty() );
    assert( m_ctxDraw.empty() );
    assert( m_hwCounterDraw.m_items.empty() );

    m_keepActive = visible && m_worker.IsDataStatic();

    if( !visible )
    {
        return;
    }

    if ( m_coreInfo.cpuData && !m_coreInfo.cpuData->cs.empty() )
    {
        const Vector<ContextSwitchCpu>& cslist = m_coreInfo.cpuData->cs;
        td.Queue( [this, &td, &ctx, &cslist, yPos ] {
            BuildThreadDataLut();
            int maxDepth = m_cpuZoneBuffer.Update( m_worker, m_threadLut, cslist, true );
            m_maxDepth = std::max( maxDepth, m_maxDepth );

            ContextSwitchCpuRange ctxRange = m_cpuZoneBuffer.FindContextSwitchCpuRange( ctx.vStart, ctx.vEnd, cslist );
            CpuZoneRange zoneRange = m_cpuZoneBuffer.FindCpuZoneRange( ctx.vStart, ctx.vEnd, ctxRange );

            uint8_t stackCollapseMode = m_view.GetViewData().coreCollapseMode;
            int32_t stackCollapseClamp = m_view.GetViewData().coreCollapseClamp;
            if ( m_trackUiData.settings.shouldOverride )
            {
                stackCollapseMode = m_trackUiData.settings.stackCollapseMode;
                stackCollapseClamp = m_trackUiData.settings.stackCollapseClamp;
            }

            bool visible = true;
            if ( stackCollapseMode == ViewData::CollapseLimit )
            {
                maxDepth = std::min(stackCollapseClamp, maxDepth);
                visible = ( maxDepth > 0 );
            }

            TimelinePreprocessor preproc( m_worker, maxDepth, stackCollapseMode );
            m_depth = PreprocessZones( ctx, cslist, zoneRange, preproc, visible );
            m_maxDepth = std::max(m_maxDepth, m_depth);

            PreprocessCpuCtxSwitches( ctx, cslist, ctxRange );

            PreprocessHwCounter( ctx, cslist, ctxRange );
        } );
    }
}


void TimelineItemCore::BuildThreadDataLut()
{
    const size_t comprThreadCount = m_worker.GetExternalCompressedThreadCount();
    const uint16_t tcount = (uint16_t)std::min( comprThreadCount, (size_t)UINT16_MAX );
    if ( tcount > m_threadLut.size() )
    {
        m_threadLut.reserve_and_use( tcount );

        for ( uint16_t comprThreadIndex = 0; comprThreadIndex < tcount; comprThreadIndex++ )
        {
            m_threadLut[ comprThreadIndex ] = nullptr;
            const uint64_t tid = m_worker.DecompressThreadExternal( comprThreadIndex );
            const bool isLocal = m_worker.IsThreadLocal( tid );
            if ( isLocal )
            {
                const ThreadData *td = m_worker.GetThreadData( tid );
                m_threadLut[ comprThreadIndex ] = td;
            }
        }
    }
}


int TimelineItemCore::PreprocessZones( const TimelineContext &ctx, const Vector<ContextSwitchCpu> &cslist, const CpuZoneRange &zoneRange, TimelinePreprocessor &preproc, bool visible )
{
    if ( zoneRange.beg >= zoneRange.end )
    {
        return 0;
    }

    CpuZoneDist prevDistValue = { 0 };
    CpuZoneDist *pDistOverride = nullptr;

    if ( (zoneRange.beg < zoneRange.end) && (zoneRange.end < m_cpuZoneBuffer.GetZoneCount() ) )
    {
        pDistOverride = m_cpuZoneBuffer.ZoneDist( zoneRange.end );
        prevDistValue = *pDistOverride;
        pDistOverride->distToNext = CpuZoneBuffer::s_EndOfRange;
    }

    const auto MinVisNs = int64_t( round( GetScale() * MinVisSize * ctx.nspx ) );
    TimelineContext clampedCtx = ctx;

    int maxdepth = 1;
    size_t globalZoneIndex = zoneRange.beg;
    while ( globalZoneIndex < zoneRange.end )
    {
        size_t next = ( globalZoneIndex + 1 );
        const CpuZoneInfo *curinfo = m_cpuZoneBuffer.ZoneInfo( globalZoneIndex );
        const ContextSwitchCpu &cs = cslist[ curinfo->csIndex ];

        assert( cs.IsEndValid() );
        const int64_t csStart = cs.Start();
        const int64_t csEnd = cs.End();
        const int64_t start = std::max( csStart, curinfo->pEvent->Start() );
        const int64_t end = std::min( csEnd, curinfo->pEvent->End() );
        const int64_t size = ( end - start );

        if ( size < MinVisNs )
        {
            int64_t startTime = start;
            int64_t lastTime = end;

            const size_t startZoneIndex = globalZoneIndex;
            const CpuZoneDist *dist = m_cpuZoneBuffer.ZoneDist( startZoneIndex );
            const CpuZoneInfo *startZone = m_cpuZoneBuffer.ZoneInfo( startZoneIndex );

            size_t endZoneIndex = startZoneIndex;
            while ( dist->distToNext < MinVisNs )
            {
                while ( dist->distToNext < MinVisNs )
                {
                    endZoneIndex++;
                    dist++;
                }

                if ( dist->distToNext == CpuZoneBuffer::s_NextPool )
                {
                    dist = m_cpuZoneBuffer.ZoneDist( endZoneIndex );
                }
            }

            assert( endZoneIndex < m_cpuZoneBuffer.GetZoneCount() );
            const CpuZoneInfo *endZone = m_cpuZoneBuffer.ZoneInfo( endZoneIndex );
            const int64_t endZoneEnd = endZone->pEvent->End();

            const ContextSwitchCpu &endCs = cslist[ endZone->csIndex ];
            const int64_t endCsEnd = endCs.End();

            const uint32_t count = (uint32_t)(( endZoneIndex - startZoneIndex ) + 1);
            assert( count <= m_cpuZoneBuffer.GetZoneCount() );

            lastTime = std::min( endZoneEnd, endCsEnd );
            assert( startTime <= lastTime );

            const ZoneEvent *pEvent = startZone->pEvent;
            if ( pEvent && visible )
            {
                const uint16_t comprTid = ((count <= 1) ? cs.Thread() : 0);
                const int64_t mergeStart = startTime;
                const int64_t mergeEnd = lastTime;
                m_draw.emplace_back( TimelineDraw{ TimelineDrawType::Folded, TimelineDrawSubType::Core, 0, ( void ** ) pEvent, mergeStart, mergeEnd, comprTid, count } );
            }

            next = (endZoneIndex + 1);
        }
        else
        {
            const uint16_t cstid = cs.Thread();
            const ThreadData *td = m_threadLut[ cstid ];
            if ( td && visible )
            {
                clampedCtx.vStart = std::max( ctx.vStart, start );
                clampedCtx.vEnd = std::min( ctx.vEnd, end );
                const size_t drawStartIndex = m_draw.size();
                const int d = preproc.PreprocessZoneLevel( clampedCtx, td->timeline, TimelineDrawSubType::Core, cstid, visible, m_draw );
                for ( size_t drawIndex = drawStartIndex; drawIndex < m_draw.size(); drawIndex++ )
                {
                    TimelineDraw &draw = m_draw[ drawIndex ];
                    draw.rstart = std::max( draw.rstart.Val(), csStart );
                    draw.rend = std::min( draw.rend.Val(), csEnd );
                }

                if ( d > maxdepth ) maxdepth = d;
            }
        }

        globalZoneIndex = next;
    }

    if ( pDistOverride != nullptr )
    {
        *pDistOverride = prevDistValue;
    }

    return maxdepth;
}


void TimelineItemCore::PreprocessCpuCtxSwitches( const TimelineContext &ctx, const Vector<ContextSwitchCpu> &cslist, const ContextSwitchCpuRange &ctxRange )
{
    if ( ctxRange.beg == ctxRange.end )
    {
        // The view is focused on the cpu being idle. Make sure we draw the idle CS in that case.
        if ( (ctxRange.beg > 0) && (ctxRange.beg < cslist.size()) )
        {
            // If we don't have a previous CS, don't add a wait since we can't determine the wait duration
            m_ctxDraw.emplace_back( ContextSwitchDraw{ ContextSwitchDrawType::Waiting, (uint32_t)ctxRange.beg, 1, 0 } );
        }
        return;
    }

    const auto MinCtxNs = int64_t( round( GetScale() * MinCtxSize * ctx.nspx ) );
    int64_t prevEnd = 0;
    auto it = cslist.begin() + ctxRange.beg;
    const auto rangeEnd = cslist.begin() + ctxRange.end;
    if ( it < rangeEnd )
    {
        const ContextSwitchCpu& curcs = *it;
        const int64_t curStart = curcs.Start();
        prevEnd = curStart;
        if ( ( it != cslist.begin() ) && (curStart > ctx.vStart) )
        {
            const ContextSwitchCpu &prev = *( it - 1 );
            prevEnd = prev.IsEndValid() ? prev.End() : 0;
        }
    }

    while ( it < rangeEnd )
    {
        auto next = it + 1;

        const ContextSwitchCpu& cs = *it;
        const size_t index = ( it - cslist.begin() );

        const int64_t start = cs.Start();
        const int64_t end = it->IsEndValid() ? it->End() : start;
        const int64_t zsz = end - start;
        const uint16_t cstid = cs.Thread();
        const uint64_t tid = m_worker.DecompressThreadExternal( cstid );
        if ( prevEnd != start )
        {
            m_ctxDraw.emplace_back( ContextSwitchDraw { ContextSwitchDrawType::Waiting, (uint32_t)index, 1, 0 } );
        }

        prevEnd = end;
        if ( zsz < MinCtxNs )
        {
            auto nextTime = end + MinCtxNs;
            for ( ;; )
            {
                next = std::lower_bound( next, rangeEnd, nextTime, [] ( const auto &l, const auto &r ) { return ( l.IsEndValid() ? l.End() : l.Start() ) < r; } );
                if ( next == rangeEnd )
                {
                    break;
                }
                auto prev = next - 1;
                const auto pt = prev->IsEndValid() ? prev->End() : prev->Start();
                const auto nt = next->IsEndValid() ? next->End() : next->Start();
                if ( nt - pt >= MinCtxNs )
                {
                    break;
                }
                nextTime = nt + MinCtxNs;
            }

            const size_t count = ( next - it );
            m_ctxDraw.emplace_back( ContextSwitchDraw { ContextSwitchDrawType::Folded, (uint32_t)index, (uint32_t)count, 0 } );
        }
        else
        {
            ContextSwitchDrawType type = ( ( tid != 0 ) ? ContextSwitchDrawType::Running : ContextSwitchDrawType::Waiting );
            m_ctxDraw.emplace_back( ContextSwitchDraw { type, (uint32_t)index, 1, 0});
        }

        it = next;
    }

    if ( ( it != cslist.end() ) && ( prevEnd < ctx.vEnd ) )
    {
        const size_t index = ( it - cslist.begin() );
        const ContextSwitchCpu& next = *it;
        const int64_t nextstart = next.Start();

        const uint16_t cstid = next.Thread();
        const uint64_t tid = ( ( prevEnd == nextstart ) ? m_worker.DecompressThreadExternal( cstid ) : 0 );
        ContextSwitchDrawType type = ( ( tid != 0 ) ? ContextSwitchDrawType::Running : ContextSwitchDrawType::Waiting );
        m_ctxDraw.emplace_back( ContextSwitchDraw { type, (uint32_t)index, 1, 0});
    }
}

void TimelineItemCore::PreprocessHwCounter( const TimelineContext &ctx, const Vector<ContextSwitchCpu> &cslist, const ContextSwitchCpuRange &ctxRange )
{
    // TODO Remove. Only testing on core 0 !!!
    //if ( m_coreInfo.core != 0 )
    //    return;
    
    const int64_t vStart = ctx.vStart;
    const int64_t vEnd = ctx.vEnd;
    const auto nspx = ctx.nspx;
    const auto MinVisNs = int64_t( round( GetScale() * MinHwCounterVisSize * nspx ) );
    const Vector<HwCounterData>& hwCounter = m_coreInfo.cpuData->hwCounter;
    m_hwCounterDraw.m_maxCount = 0;

    // TODO Do we need to sort the hw counter vector. Assuming sorted for now ...
    if ( hwCounter.empty() || ( hwCounter.front().time.Val() > vEnd ) || ( hwCounter.back().time.Val() < vStart ) )
        return;

    // Using context switch data to only display hw counter for the current process.
    auto ctxIt = cslist.begin() + ctxRange.beg;
    const auto ctxEndIt = cslist.begin() + ctxRange.end;
    ctxIt = FindFirstLocalCtxSwitch( ctxIt, ctxEndIt );
    if ( ctxIt == ctxEndIt )
    {
        // The view is focused on the cpu being idle or external program. Nothing to do
        return;
    }
    int64_t currentCtxStartTime = ctxIt->Start();
    int64_t currentCtxEndTime = ctxIt->IsEndValid() ? ctxIt->End() : ctxIt->Start();

    auto beginIt = std::lower_bound( hwCounter.begin(), hwCounter.end(), vStart, [] ( const auto &l, const auto &r ) { return l.time.Val() < r; } );
    auto endIt = std::lower_bound( beginIt, hwCounter.end(), vEnd, [] ( const auto &l, const auto &r ) { return l.time.Val() < r; } );

    if ( beginIt != hwCounter.begin() ) beginIt--;

    auto it = beginIt;
    HwCounterDrawItem counterDraw;
    int64_t nAccDurationNs = 0;
    while ( it < endIt )
    {
        auto next = it + 1;

        // TODO what happend if next == endId ???? crash ????
        int64_t durationNs = next->time.Val() - it->time.Val();
        int64_t midpointTime = it->time.Val() + ( durationNs / 2 );

        // Only display hw counter for the current process
        if ( midpointTime > currentCtxStartTime && midpointTime < currentCtxEndTime )
        {
            // Merge hw counter data if necessary (too small to be visualized)
            if ( ( next->time.Val() - counterDraw.tstart.Val() ) < MinVisNs )
            {
                counterDraw.tend = next->time;
                counterDraw.count += ( next->count - it->count );
                nAccDurationNs += durationNs;
            }
            else
            {
                // draw merged hw counter
                if ( counterDraw.count > 0 )
                {
                    m_hwCounterDraw.AddDrawItem( counterDraw.tstart, counterDraw.tend, counterDraw.count, nAccDurationNs );
                }

                // start a new draw command
                counterDraw.tstart = it->time;
                counterDraw.tend = next->time;
                counterDraw.count = next->count - it->count;
                counterDraw.rate = 0.0f;
                nAccDurationNs = durationNs;
            }
        }
        
        // Find the next 'ContextSwitchCpu' for the current process
        while ( midpointTime >= currentCtxEndTime && ctxIt != ctxEndIt )
        {
            ctxIt = FindFirstLocalCtxSwitch( ctxIt + 1, ctxEndIt );
            if ( ctxIt != ctxEndIt )
            {
                currentCtxStartTime = ctxIt->Start();
                currentCtxEndTime = ctxIt->IsEndValid() ? ctxIt->End() : ctxIt->Start();
            }
        }

        it = next;
    }

    if ( counterDraw.count > 0 )
    {
        m_hwCounterDraw.AddDrawItem( counterDraw.tstart, counterDraw.tend, counterDraw.count, nAccDurationNs );
    }

    m_hwCounterDraw.m_name = m_worker.GetHwCounterName();
    m_hwCounterDraw.m_description = m_worker.GetHwCounterDescription();

    m_view.UpdateHwCounterMaxValues( m_hwCounterDraw.m_maxCount, m_hwCounterDraw.m_maxRate );
}

const ContextSwitchCpu *TimelineItemCore::FindFirstLocalCtxSwitch( const ContextSwitchCpu *beginIt, const ContextSwitchCpu *endIt )
{
    while ( beginIt < endIt )
    {
        const uint16_t cstid = beginIt->Thread();
        const ThreadData *td = m_threadLut[ cstid ];
        if ( td )
        {
            return beginIt;
        }

        beginIt++;
    }
    
    return endIt;
}


}
