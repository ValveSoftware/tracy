#include <algorithm>
#include <limits>

#include "TracyColor.hpp"
#include "TracyImGui.hpp"
#include "TracyLockHelpers.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyTimelinePreprocessor.hpp"
#include "TracyTimelineItemThread.hpp"
#include "TracyView.hpp"
#include "TracyWorker.hpp"

namespace tracy
{

constexpr float MinVisSize = 3;
constexpr float MinCtxSize = 4;


TimelineItemThread::TimelineItemThread( View& view, Worker& worker, const ThreadData* thread )
    : TimelineItem( view, worker, thread, true )
    , m_thread( thread )
    , m_ghost( false )
{
    ViewData &vd = view.GetViewData();
    if ( vd.threads.contains( thread->id ) )
    {
        ViewData::Track &threadSettings = vd.threads[ thread->id ];
        m_trackUiData.settings = threadSettings.ui;
        SetVisible( threadSettings.visible );
    }
    else
    {
        m_trackUiData.settings.shouldOverride = false;
        m_trackUiData.settings.stackCollapseMode = view.GetViewData().stackCollapseMode;
        m_trackUiData.settings.stackCollapseClamp = view.GetViewData().stackCollapseClamp;
    }

    m_trackUiData.preventScroll = 0;

    m_maxZoneDepth = m_thread->maxDepth;
    m_maxGhostDepth = 0;
    m_maxDepth = m_maxZoneDepth;
    auto name = worker.GetThreadName( thread->id );
    if( strncmp( name, "Tracy ", 6 ) == 0 )
    {
        m_showFull = false;
    }
}

bool TimelineItemThread::IsEmpty() const
{
    auto& crash = m_worker.GetCrashEvent();
    return crash.thread != m_thread->id &&
        m_thread->timeline.empty() &&
        m_thread->messages.empty() &&
        m_thread->ghostZones.empty();
}

uint32_t TimelineItemThread::HeaderColor() const
{
    auto& crash = m_worker.GetCrashEvent();
    if( crash.thread == m_thread->id ) return 0xFF2222FF;
    if( m_thread->isFiber ) return 0xFF88FF88;
    return 0xFFFFFFFF;
}

uint32_t TimelineItemThread::HeaderColorInactive() const
{
    auto& crash = m_worker.GetCrashEvent();
    if( crash.thread == m_thread->id ) return 0xFF111188;
    if( m_thread->isFiber ) return 0xFF448844;
    return 0xFF888888;
}

uint32_t TimelineItemThread::HeaderLineColor() const
{
    return 0x33FFFFFF;
}

const char* TimelineItemThread::HeaderLabel() const
{
    return m_worker.GetThreadName( m_thread->id );
}

int64_t TimelineItemThread::RangeBegin() const
{
    int64_t first = std::numeric_limits<int64_t>::max();
    const auto ctx = m_worker.GetContextSwitchData( m_thread->id );
    if( ctx && !ctx->v.empty() )
    {
        first = ctx->v.begin()->Start();
    }
    if( !m_thread->timeline.empty() )
    {
        if( m_thread->timeline.is_magic() )
        {
            auto& tl = *((Vector<ZoneEvent>*)&m_thread->timeline);
            first = std::min( first, tl.front().Start() );
        }
        else
        {
            first = std::min( first, m_thread->timeline.front()->Start() );
        }
    }
    if( !m_thread->messages.empty() )
    {
        first = std::min( first, m_thread->messages.front()->time );
    }

    const unordered_flat_map<uint32_t, LockMap*>& locks = m_worker.GetLockMap();
    const unordered_flat_set<uint32_t> &lockIds = m_worker.GetLockIdsForThread( m_thread->id );
    for( uint32_t lockId : lockIds )
    {
        const auto lockit = locks.find( lockId );
        if ( lockit == locks.end() ) continue;
        const auto &lockmap = *lockit->second;
        if( !lockmap.valid ) continue;
        auto it = lockmap.threadMap.find( m_thread->id );
        if( it == lockmap.threadMap.end() ) continue;
        const auto thread = it->second;
        auto lptr = lockmap.timeline.data();
        while( lptr->ptr->thread != thread ) lptr++;
        if( lptr->ptr->Time() < first ) first = lptr->ptr->Time();
    }
    return first;
}

int64_t TimelineItemThread::RangeEnd() const
{
    int64_t last = -1;
    const auto ctx = m_worker.GetContextSwitchData( m_thread->id );
    if( ctx && !ctx->v.empty() )
    {
        const auto& back = ctx->v.back();
        last = back.IsEndValid() ? back.End() : back.Start();
    }
    if( !m_thread->timeline.empty() )
    {
        if( m_thread->timeline.is_magic() )
        {
            auto& tl = *((Vector<ZoneEvent>*)&m_thread->timeline);
            last = std::max( last, m_worker.GetZoneEnd( tl.back() ) );
        }
        else
        {
            last = std::max( last, m_worker.GetZoneEnd( *m_thread->timeline.back() ) );
        }
    }
    if( !m_thread->messages.empty() )
    {
        last = std::max( last, m_thread->messages.back()->time );
    }

    const unordered_flat_map<uint32_t, LockMap*>& locks = m_worker.GetLockMap();
    const unordered_flat_set<uint32_t> &lockIds = m_worker.GetLockIdsForThread( m_thread->id );
    for( uint32_t lockId : lockIds )
    {
        const auto lockit = locks.find( lockId );
        if ( lockit == locks.end() ) continue;
        const auto &lockmap = *lockit->second;
        if( !lockmap.valid ) continue;
        auto it = lockmap.threadMap.find( m_thread->id );
        if( it == lockmap.threadMap.end() ) continue;
        const auto thread = it->second;
        auto eptr = lockmap.timeline.data() + lockmap.timeline.size() - 1;
        while( eptr->ptr->thread != thread ) eptr--;
        if( eptr->ptr->Time() > last ) last = eptr->ptr->Time();
    }
    return last;
}

void TimelineItemThread::HeaderTooltip( const char* label ) const
{
    m_view.HighlightThread( m_thread->id );

    ImGui::BeginTooltip();
    SmallColorBox( GetThreadColor( m_thread->id, 0, m_view.GetViewData().dynamicColors ) );
    ImGui::SameLine();
    ImGui::TextUnformatted( m_worker.GetThreadName( m_thread->id ) );
    ImGui::SameLine();
    ImGui::TextDisabled( "(%s)", RealToString( m_thread->id ) );
    auto& crash = m_worker.GetCrashEvent();
    if( crash.thread == m_thread->id )
    {
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 1.f, 0.2f, 0.2f, 1.f ), ICON_FA_SKULL " Crashed" );
    }
    if( m_thread->isFiber )
    {
        ImGui::SameLine();
        TextColoredUnformatted( ImVec4( 0.2f, 0.6f, 0.2f, 1.f ), "Fiber" );
    }

    const auto ctx = m_worker.GetContextSwitchData( m_thread->id );
    const auto first = RangeBegin();
    const auto last = RangeEnd();

    ImGui::Separator();

    size_t lockCnt = 0;
    for( const auto& lock : m_worker.GetLockMap() )
    {
        const auto& lockmap = *lock.second;
        if( !lockmap.valid ) continue;
        auto it = lockmap.threadMap.find( m_thread->id );
        if( it == lockmap.threadMap.end() ) continue;
        lockCnt++;
    }

    if( last >= 0 )
    {
        const auto lifetime = last - first;
        const auto traceLen = m_worker.GetLastTime() - m_worker.GetFirstTime();

        TextFocused( "Appeared at", TimeToString( first ) );
        TextFocused( "Last event at", TimeToString( last ) );
        TextFocused( "Lifetime:", TimeToString( lifetime ) );
        ImGui::SameLine();
        char buf[64];
        PrintStringPercent( buf, lifetime / double( traceLen ) * 100 );
        TextDisabledUnformatted( buf );

        if( ctx )
        {
            TextFocused( "Time in running state:", TimeToString( ctx->runningTime ) );
            ImGui::SameLine();
            PrintStringPercent( buf, ctx->runningTime / double( lifetime ) * 100 );
            TextDisabledUnformatted( buf );
        }
    }

    ImGui::Separator();
    if( !m_thread->timeline.empty() )
    {
        TextFocused( "Zone count:", RealToString( m_thread->count ) );
        TextFocused( "Top-level zones:", RealToString( m_thread->timeline.size() ) );
    }
    if( !m_thread->messages.empty() )
    {
        TextFocused( "Messages:", RealToString( m_thread->messages.size() ) );
    }
    if( lockCnt != 0 )
    {
        TextFocused( "Locks:", RealToString( lockCnt ) );
    }
    if( ctx )
    {
        TextFocused( "Running state regions:", RealToString( ctx->v.size() ) );
    }
    if( !m_thread->samples.empty() )
    {
        TextFocused( "Call stack samples:", RealToString( m_thread->samples.size() ) );
        if( m_thread->kernelSampleCnt != 0 )
        {
            TextFocused( "Kernel samples:", RealToString( m_thread->kernelSampleCnt ) );
            ImGui::SameLine();
            ImGui::TextDisabled( "(%.2f%%)", 100.f * m_thread->kernelSampleCnt / m_thread->samples.size() );
        }
    }
    ImGui::EndTooltip();
}

void TimelineItemThread::HeaderExtraContents( const TimelineContext& ctx, int offset, float &xOffset )
{
    int messageOffset = offset;
    if ( !m_msgDraw.empty() )
    {
        const auto label = HeaderLabel();
        ImVec2 labelSize = ImGui::CalcTextSize( label );
        messageOffset += labelSize.y;
    }
    m_view.DrawThreadMessagesList( ctx, m_msgDraw, messageOffset, m_thread->id );

#ifndef TRACY_NO_STATISTICS
    const bool hasGhostZones = m_worker.AreGhostZonesReady() && !m_thread->ghostZones.empty();
    if( hasGhostZones && !m_thread->timeline.empty() )
    {
        auto draw = ImGui::GetWindowDrawList();
        const auto ty = ImGui::GetTextLineHeight();
        const float ItemSpacing = ImGui::GetStyle().ItemSpacing.x;

        const auto color = m_ghost ? 0xFFAA9999 : 0x88AA7777;
        draw->AddText( ctx.wpos + ImVec2( xOffset, offset ), color, ICON_FA_GHOST );
        float ghostSz = ImGui::CalcTextSize( ICON_FA_GHOST ).x;

        if( ctx.hover && ImGui::IsMouseHoveringRect( ctx.wpos + ImVec2( xOffset, offset ), ctx.wpos + ImVec2( xOffset + ghostSz, offset + ty ) ) )
        {
            if( IsMouseClicked( 0 ) )
            {
                m_ghost = !m_ghost;
            }
        }
        xOffset += ghostSz + ItemSpacing;
    }
#endif
}

bool TimelineItemThread::DrawContents( const TimelineContext& ctx, int& offset )
{
    uint8_t stackCollapseMode = m_view.GetViewData().stackCollapseMode;
    int32_t stackCollapseClamp = m_view.GetViewData().stackCollapseClamp;
    if ( m_trackUiData.settings.shouldOverride )
    {
        stackCollapseMode = m_trackUiData.settings.stackCollapseMode;
        stackCollapseClamp = m_trackUiData.settings.stackCollapseClamp;
    }

    bool hasMessageCheck = m_hasMessages;
    int depth = m_depth;
    if ( stackCollapseMode == ViewData::CollapseMax )
    {
        depth = m_maxDepth;
        hasMessageCheck = !m_thread->messages.empty();
    }
    else if ( stackCollapseMode == ViewData::CollapseLimit )
    {
        depth = std::min(stackCollapseClamp, m_maxDepth);
        hasMessageCheck = !m_thread->messages.empty();
    }

    bool hasLocksCheck = !m_lockDraw.empty();
    if ( hasMessageCheck || hasLocksCheck )
    {
        const auto label = HeaderLabel();
        ImVec2 labelSize = ImGui::CalcTextSize( label );
        offset += labelSize.y;
    }

    std::sort( m_lockDraw.begin(), m_lockDraw.end(), [] ( const auto &lhs, const auto &rhs ) { return ( lhs->id < rhs->id ); } );

    m_view.DrawThread( ctx, *m_thread, m_draw, m_ctxDraw, m_samplesDraw, m_lockDraw, m_hwCounterDraw, offset, depth, m_hasCtxSwitch, m_hasSamples, m_hasHwCounter );

    if( (depth == 0) && !hasMessageCheck && !hasLocksCheck )
    {
        auto& crash = m_worker.GetCrashEvent();
        return crash.thread == m_thread->id;
    }
    return true;
}

void TimelineItemThread::DrawOverlay( const ImVec2& ul, const ImVec2& dr )
{
    m_view.DrawThreadOverlays( *m_thread, ul, dr );
}

bool TimelineItemThread::PreventScrolling() const
{
    return (m_trackUiData.preventScroll > 0);
}

void TimelineItemThread::DrawUiControls( const TimelineContext &ctx, int start, int &offset, float xOffset )
{
    ViewData::Track& rThreadSettings = m_view.GetViewData().threads[ m_thread->id ];
    m_view.DrawTrackUiControls( ctx, HeaderLabel(), m_maxDepth, m_depth, m_trackUiData, rThreadSettings, start, offset, xOffset );
}

void TimelineItemThread::DrawExtraPopupItems()
{
    if( m_view.GetSelectThread() == m_thread->id )
    {
        if( ImGui::MenuItem( ICON_FA_TIMELINE " Unselect in CPU timeline" ) )
        {
            m_view.SelectThread( 0 );
        }
    }
    else if( m_view.GetViewData().drawCpuData && ImGui::MenuItem( ICON_FA_TIMELINE " Select in CPU timeline" ) )
    {
        m_view.SelectThread( m_thread->id );
    }
 }

void TimelineItemThread::DrawFinished()
{
    m_samplesDraw.clear();
    m_ctxDraw.clear();
    m_hwCounterDraw.Reset();
    m_draw.clear();
    m_msgDraw.clear();
    m_lockDraw.clear();

    m_mergeLockDrawTmp.clear();
    m_mergedLockMaps.clear();
}

void TimelineItemThread::Preprocess( const TimelineContext& ctx, TaskDispatch& td, bool visible, int yPos )
{
    assert( m_samplesDraw.empty() );
    assert( m_ctxDraw.empty() );
    assert( m_hwCounterDraw.m_items.empty() );
    assert( m_draw.empty() );
    assert( m_msgDraw.empty() );
    assert( m_lockDraw.empty() );

    assert( m_mergeJobCounter.load() == 0 );
    assert( m_mergeLockDrawTmp.empty() );
    assert( m_mergedLockMaps.empty() );

	const uint16_t comprTid = m_worker.CompressThread( m_thread->id );
	td.Queue( [this, &ctx, comprTid, visible] {
#ifndef TRACY_NO_STATISTICS
        if( m_worker.AreGhostZonesReady() && ( m_ghost || ( m_view.GetViewData().ghostZones && m_thread->timeline.empty() ) ) )
        {
            m_depth = PreprocessGhostLevel( ctx, m_thread->ghostZones, 0, visible );
            m_maxGhostDepth = std::max(m_maxGhostDepth, m_depth);
            m_maxDepth = m_maxGhostDepth;
        }
        else
#endif
        {
            int maxDepth = m_maxZoneDepth;
            uint8_t stackCollapseMode = m_view.GetViewData().stackCollapseMode;
            int32_t stackCollapseClamp = m_view.GetViewData().stackCollapseClamp;
            if ( m_trackUiData.settings.shouldOverride )
            {
                stackCollapseMode = m_trackUiData.settings.stackCollapseMode;
                stackCollapseClamp = m_trackUiData.settings.stackCollapseClamp;
            }

            if ( stackCollapseMode == ViewData::CollapseLimit )
            {
                maxDepth = std::min(stackCollapseClamp, maxDepth);
            }

            TimelinePreprocessor preproc( m_worker, maxDepth, stackCollapseMode );
            m_depth = preproc.PreprocessZoneLevel( ctx, m_thread->timeline, TimelineDrawSubType::Thread, comprTid, visible, m_draw );
            m_maxZoneDepth = std::max(m_maxZoneDepth, m_depth);
            m_maxDepth = m_maxZoneDepth;
        }
    } );

    const auto& vd = m_view.GetViewData();

    m_hasCtxSwitch = false;
    if( vd.drawContextSwitches )
    {
        auto ctxSwitch = m_worker.GetContextSwitchData( m_thread->id );
        if( ctxSwitch )
        {
            // There is no yPos passed here to enable more granular visibility check,
            // as context switch shadows will usually be projected down onto zones.
            td.Queue( [this, &ctx, ctxSwitch, visible] {
                PreprocessContextSwitches( ctx, *ctxSwitch, visible );
            } );
        }
    }

    m_hasHwCounter = false;
    if ( vd.drawHwCounter )
    {
        auto ctxSwitch = m_worker.GetContextSwitchData( m_thread->id );
        auto cpuData = m_worker.GetCpuData();
        const auto cpuCnt = m_worker.GetCpuDataCpuCount();
        if ( ctxSwitch && cpuCnt )
        {
            td.Queue( [this, &ctx, ctxSwitch, cpuData, cpuCnt, visible] {
                PreprocessHwCounter( ctx, *ctxSwitch, cpuData, cpuCnt, visible );
            } );
        }
    }

    m_hasSamples = false;
    if( vd.drawSamples && !m_thread->samples.empty() )
    {
        td.Queue( [this, &ctx, visible, yPos] {
            PreprocessSamples( ctx, m_thread->samples, visible, yPos );
        } );
    }

    m_hasMessages = false;
    td.Queue( [this, &ctx, visible, yPos] {
        PreprocessMessages( ctx, m_thread->messages, m_thread->id, visible, yPos );
    } );

    if( vd.drawLocks )
    {
        m_mergeJobCounter.store( 1 );
        const uint8_t activeDrawFlags = (ViewData::ELockDrawVisFlags::Contended | ViewData::ELockDrawVisFlags::Uncontended | ViewData::ELockDrawVisFlags::SingleThread );
        const uint8_t terminatedDrawFlags = (ViewData::ELockDrawVisFlags::SingleTerminated);

        const auto& locks = m_worker.GetLockMap();
        if ( ( vd.lockDrawFlags & activeDrawFlags ) != 0 )
        {
            const unordered_flat_set<uint32_t> &lockIds = m_worker.GetLockIdsForThread( m_thread->id );
            if ( !lockIds.empty() )
            {
                PreprocessLocks( ctx, lockIds, locks, m_thread->id, td, visible );
            }
        }

        if ( (vd.lockDrawFlags & terminatedDrawFlags ) != 0 )
        {
            const unordered_flat_set<uint32_t> &singlelockIds = m_worker.GetTerminatedSingleThreadLockIdsForThread( m_thread->id );
            if( !singlelockIds.empty() )
            {
                PreprocessLocks( ctx, singlelockIds, locks, m_thread->id, td, visible );
            }
        }

        if ( m_mergeJobCounter.fetch_sub( 1 ) == 1 )
        {
            const auto nspx = ctx.nspx;
            const auto MinVisNs = int64_t( round( GetScale() * MinVisSize * nspx ) );
            uint32_t tid = m_thread->id;
            td.Queue( [ this, tid, &locks, &ctx, &vd, visible, MinVisNs ]
            {
                PreprocessMergedLocks( ctx, locks, tid, MinVisNs, visible );
            } );
        }
    }
}

#ifndef TRACY_NO_STATISTICS
int TimelineItemThread::PreprocessGhostLevel( const TimelineContext& ctx, const Vector<GhostZone>& vec, int depth, bool visible )
{
    uint8_t stackCollapseMode = m_view.GetViewData().stackCollapseMode;
    int32_t stackCollapseClamp = m_view.GetViewData().stackCollapseClamp;
    if ( m_trackUiData.settings.shouldOverride )
    {
        stackCollapseMode = m_trackUiData.settings.stackCollapseMode;
        stackCollapseClamp = m_trackUiData.settings.stackCollapseClamp;
    }

    if ( stackCollapseMode == ViewData::CollapseLimit )
    {
        int maxDepth = std::min(stackCollapseClamp, m_maxZoneDepth);
        visible = ( depth < maxDepth );
    }

    const auto nspx = ctx.nspx;
    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;

    const auto MinVisNs = int64_t( round( GetScale() * MinVisSize * nspx ) );

    auto it = std::lower_bound( vec.begin(), vec.end(), std::max<int64_t>( 0, vStart - 2 * MinVisNs ), [] ( const auto& l, const auto& r ) { return l.end.Val() < r; } );
    if( it == vec.end() ) return depth;

    const auto zitend = std::lower_bound( it, vec.end(), vEnd, [] ( const auto& l, const auto& r ) { return l.start.Val() < r; } );
    if( it == zitend ) return depth;
    if( (zitend-1)->end.Val() < vStart ) return depth;

    int maxdepth = depth + 1;

    while( it < zitend )
    {
        auto& ev = *it;
        const auto end = ev.end.Val();
        const auto zsz = end - ev.start.Val();
        if( zsz < MinVisNs )
        {
            auto nextTime = end + MinVisNs;
            auto next = it + 1;
            for(;;)
            {
                next = std::lower_bound( next, zitend, nextTime, [] ( const auto& l, const auto& r ) { return l.end.Val() < r; } );
                if( next == zitend ) break;
                auto prev = next - 1;
                const auto pt = prev->end.Val();
                const auto nt = next->end.Val();
                if( nt - pt >= MinVisNs ) break;
                nextTime = nt + MinVisNs;
            }
            if( visible ) m_draw.emplace_back( TimelineDraw { TimelineDrawType::GhostFolded, TimelineDrawSubType::Thread, uint16_t( depth ), (void**)&ev, it->start, (next-1)->end } );
            it = next;
        }
        else
        {
            if( ev.child >= 0 )
            {
                const auto d = PreprocessGhostLevel( ctx, m_worker.GetGhostChildren( ev.child ), depth + 1, visible );
                if( d > maxdepth ) maxdepth = d;
            }
            if( visible ) m_draw.emplace_back( TimelineDraw { TimelineDrawType::Ghost, TimelineDrawSubType::Thread, uint16_t( depth ), (void**)&ev, ev.start, ev.end } );
            ++it;
        }
    }

    return maxdepth;
}
#endif


void TimelineItemThread::PreprocessContextSwitches( const TimelineContext& ctx, const ContextSwitch& ctxSwitch, bool visible )
{
    const auto nspx = ctx.nspx;
    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;

    auto& vec = ctxSwitch.v;
    auto it = std::lower_bound( vec.begin(), vec.end(), std::max<int64_t>( 0, vStart ), [] ( const auto& l, const auto& r ) { return ( l.IsEndValid() ? l.End() : l.Start() ) < r; } );
    if( it == vec.end() ) return;
    if( it != vec.begin() ) --it;

    auto citend = std::lower_bound( it, vec.end(), vEnd, [] ( const auto& l, const auto& r ) { return l.Start() < r; } );
    if( it == citend ) return;
    if( citend != vec.end() ) ++citend;

    m_hasCtxSwitch = true;
    if( !visible ) return;

    const auto MinCtxNs = int64_t( round( GetScale() * MinCtxSize * nspx ) );
    const auto& sampleData = m_thread->samples;

    bool first = true;
    while( it < citend )
    {
        auto& ev = *it;
        if( first )
        {
            first = false;
        }
        else
        {
            uint32_t waitStack = 0;
            if( !sampleData.empty() )
            {
                auto sdit = std::lower_bound( sampleData.begin(), sampleData.end(), ev.Start(), [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );
                bool found = sdit != sampleData.end() && sdit->time.Val() == ev.Start();
                if( !found && it != vec.begin() )
                {
                    auto eit = it;
                    --eit;
                    sdit = std::lower_bound( sampleData.begin(), sampleData.end(), eit->End(), [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );
                    found = sdit != sampleData.end() && sdit->time.Val() == eit->End();
                }
                if( found ) waitStack = sdit->callstack.Val();
            }

			uint32_t readyingStack = 0;
			if ( ev.WakeupVal() != ev.Start() )
			{
				const ThreadData *pReadyingThreadData = m_view.GetThreadDataForCpu( ev.WakeupCpu(), ev.WakeupVal() );
				if ( pReadyingThreadData && !pReadyingThreadData->samples.empty() )
				{
					auto sdit = std::lower_bound( pReadyingThreadData->samples.begin(), pReadyingThreadData->samples.end(), ev.WakeupVal(), [] ( const auto &l, const auto &r ) { return l.time.Val() < r; } );
					if ( sdit != pReadyingThreadData->samples.end() && sdit->time.Val() == ev.WakeupVal() )
					{
						readyingStack = sdit->callstack.Val();
					}
				}
			}

            m_ctxDraw.emplace_back( ContextSwitchDraw { ContextSwitchDrawType::Waiting, uint32_t( it - vec.begin() ), waitStack, readyingStack } );
        }

        const auto end = ev.IsEndValid() ? ev.End() : ev.Start();
        const auto zsz = end - ev.Start();
        if( zsz < MinCtxNs )
        {
            auto nextTime = end + MinCtxNs;
            auto next = it + 1;
            for(;;)
            {
                next = std::lower_bound( next, citend, nextTime, [] ( const auto& l, const auto& r ) { return ( l.IsEndValid() ? l.End() : l.Start() ) < r; } );
                if( next == citend ) break;
                auto prev = next - 1;
                const auto pt = prev->IsEndValid() ? prev->End() : prev->Start();
                const auto nt = next->IsEndValid() ? next->End() : next->Start();
                if( nt - pt >= MinCtxNs ) break;
                nextTime = nt + MinCtxNs;
            }
            m_ctxDraw.emplace_back( ContextSwitchDraw { ContextSwitchDrawType::Folded, uint32_t( it - vec.begin() ), uint32_t( next - it ) } );
            it = next;
        }
        else
        {
            m_ctxDraw.emplace_back( ContextSwitchDraw { ContextSwitchDrawType::Running, uint32_t( it - vec.begin() ) } );
            ++it;
        }
    }
}

void TimelineItemThread::PreprocessHwCounter( const TimelineContext &ctx, const ContextSwitch &ctxSwitch, const CpuData *pCpuData, int cpuDataCount, bool visible )
{
    const auto nspx = ctx.nspx;
    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;

    auto &ctxSwitchVec = ctxSwitch.v;
    auto ctxSwitchIt = std::lower_bound( ctxSwitchVec.begin(), ctxSwitchVec.end(), std::max<int64_t>( 0, vStart ), [] ( const auto &l, const auto &r ) { return ( l.IsEndValid() ? l.End() : l.Start() ) < r; } );
    if ( ctxSwitchIt == ctxSwitchVec.end() ) return;
    if ( ctxSwitchIt != ctxSwitchVec.begin() ) --ctxSwitchIt;

    auto ctxSwitchItEnd = std::lower_bound( ctxSwitchIt, ctxSwitchVec.end(), vEnd, [] ( const auto &l, const auto &r ) { return l.Start() < r; } );
    if ( ctxSwitchIt == ctxSwitchItEnd ) return;
    if ( ctxSwitchItEnd != ctxSwitchVec.end() ) ++ctxSwitchItEnd;
    
    m_hwCounterDraw.m_maxCount = 0;

    if ( !visible ) return;

    // Given a thread, find on which core it is actually running in order to draw hw counter for that core.
    while ( ctxSwitchIt < ctxSwitchItEnd )
    {
        int64_t currentCtxStartTime = ctxSwitchIt->Start();
        int64_t currentCtxEndTime = ctxSwitchIt->IsEndValid() ? ctxSwitchIt->End() : ctxSwitchIt->Start();
        if ( currentCtxStartTime == currentCtxEndTime )
        {
            ++ctxSwitchIt;
            continue;
        }
        
        uint8_t cpu = ctxSwitchIt->Cpu();
        if ( ( cpu >= cpuDataCount ) || pCpuData[ cpu ].hwCounter.empty() )
        {
            ++ctxSwitchIt;
            continue;
        }

        const Vector<HwCounterData>& hwCounter = pCpuData[ cpu ].hwCounter;
        if ( ( hwCounter.front().time.Val() > currentCtxEndTime ) || ( hwCounter.back().time.Val() < currentCtxStartTime ) )
        {
            ++ctxSwitchIt;
            continue;
        }

        // Iterate hw counter vector for the given core & period of time
        auto beginIt = std::lower_bound( hwCounter.begin(), hwCounter.end(), currentCtxStartTime, [] ( const auto &l, const auto &r ) { return l.time.Val() < r; } );
        auto endIt = std::lower_bound( beginIt, hwCounter.end(), currentCtxEndTime, [] ( const auto &l, const auto &r ) { return l.time.Val() < r; } );

        auto it = beginIt;
        while ( it < endIt )
        {
            auto next = it + 1;
            // only draw hw counter count for the given viewing area
            if ( ( next->time.Val() < vStart ) || ( it->time.Val() > vEnd ) )
            {
                it = next;
                continue;
            }
            
            uint64_t count = next->count - it->count;
            m_hwCounterDraw.AddDrawItem( it->time, next->time, count, next->time.Val() - it->time.Val() );

            it = next;
        }
        
        ++ctxSwitchIt;
    }

    m_hwCounterDraw.m_name = m_worker.GetHwCounterName();
    m_hwCounterDraw.m_description = m_worker.GetHwCounterDescription();

    m_view.UpdateHwCounterMaxValues( m_hwCounterDraw.m_maxCount, m_hwCounterDraw.m_maxRate );

    m_hasHwCounter = !m_hwCounterDraw.m_items.empty();
}

void TimelineItemThread::PreprocessSamples( const TimelineContext& ctx, const Vector<SampleData>& vec, bool visible, int yPos )
{
    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;
    const auto nspx = ctx.nspx;
    const auto ty = ctx.ty;
    const auto ostep = ty + 1;
    const auto pos = yPos + ostep;

    const auto MinVis = 5 * GetScale();
    const auto MinVisNs = int64_t( round( MinVis * nspx ) );

    auto it = std::lower_bound( vec.begin(), vec.end(), vStart - MinVisNs, [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );
    if( it == vec.end() ) return;
    const auto itend = std::lower_bound( it, vec.end(), vEnd, [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );
    if( it == itend ) return;

    m_hasSamples = true;
    if( !visible ) return;

    const auto ty0375 = pos + round( ty * 0.375f );
    const auto ty02 = round( ty * 0.2f );
    const auto y0 = ty0375 - ty02 - 3;
    const auto y1 = ty0375 + ty02 - 1;
    if( y0 > ctx.yMax || y1 < ctx.yMin ) return;

    while( it < itend )
    {
        auto next = it + 1;
        if( next != itend )
        {
            const auto t0 = it->time.Val();
            auto nextTime = t0 + MinVisNs;
            for(;;)
            {
                next = std::lower_bound( next, itend, nextTime, [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );
                if( next == itend ) break;
                auto prev = next - 1;
                const auto pt = prev->time.Val();
                const auto nt = next->time.Val();
                if( nt - pt >= MinVisNs ) break;
                nextTime = nt + MinVisNs;
            }
        }
        m_samplesDraw.emplace_back( SamplesDraw { uint32_t( next - it - 1 ), uint32_t( it - vec.begin() ) } );
        it = next;
    }
}

void TimelineItemThread::PreprocessMessages( const TimelineContext& ctx, const Vector<short_ptr<MessageData>>& vec, uint64_t tid, bool visible, int yPos )
{
    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;
    const auto nspx = ctx.nspx;

    const auto MinVisNs = int64_t( round( GetScale() * MinVisSize * nspx ) );

    auto it = std::lower_bound( vec.begin(), vec.end(), vStart, [] ( const auto& lhs, const auto& rhs ) { return lhs->time < rhs; } );
    if( it == vec.end() ) return;
    auto end = std::lower_bound( it, vec.end(), vEnd+1, [] ( const auto& lhs, const auto& rhs ) { return lhs->time < rhs; } );
    if( it == end ) return;

    m_hasMessages = true;
    if( !visible ) return;
    if( yPos > ctx.yMax || yPos + ctx.ty < ctx.yMin ) return;

    const auto hMsg = m_view.GetMessageHighlight();
    const auto hThread = hMsg ? m_worker.DecompressThread( hMsg->thread ) : 0;

    while( it < end )
    {
        const auto msgTime = (*it)->time;
        const auto nextTime = msgTime + MinVisNs;
        const auto next = std::upper_bound( it, vec.end(), nextTime, [] ( const auto& lhs, const auto& rhs ) { return lhs < rhs->time; } );
        const auto num = next - it;
        bool hilite;
        if( num == 1 )
        {
            hilite = hMsg == *it;
        }
        else
        {
            if( hMsg && hThread == tid )
            {
                const auto hTime = hMsg->time;
                hilite = (*it)->time <= hTime && ( next == vec.end() || (*next)->time > hTime );
            }
            else
            {
                hilite = false;
            }
        }
        m_msgDraw.emplace_back( MessagesDraw { *it, hilite, uint32_t( num ) } );
        it = next;
    }
}

static Vector<LockEventPtr>::const_iterator GetNextLockEvent( const Vector<LockEventPtr>::const_iterator& it, const Vector<LockEventPtr>::const_iterator& end, LockState::Type& nextState, size_t threadBit )
{
    auto next = it;
    next++;

    switch( nextState )
    {
    case LockState::Nothing:
        while( next < end )
        {
            if( next->lockCount != 0 )
            {
                if( GetThreadBit( next->lockingThread ) == threadBit )
                {
                    nextState = AreOtherWaiting( next->waitList, threadBit ) ? LockState::HasBlockingLock : LockState::HasLock;
                    break;
                }
                else if( IsThreadWaiting( next->waitList, threadBit ) )
                {
                    nextState = LockState::WaitLock;
                    break;
                }
            }
            next++;
        }
        break;
    case LockState::HasLock:
        while( next < end )
        {
            if( next->lockCount == 0 )
            {
                nextState = LockState::Nothing;
                break;
            }
            if( next->waitList.Any() )
            {
                if( AreOtherWaiting( next->waitList, threadBit ) )
                {
                    nextState = LockState::HasBlockingLock;
                }
                break;
            }
            if( next->waitList != it->waitList || next->lockCount != it->lockCount )
            {
                break;
            }
            next++;
        }
        break;
    case LockState::HasBlockingLock:
        while( next < end )
        {
            if( next->lockCount == 0 )
            {
                nextState = LockState::Nothing;
                break;
            }
            if( next->waitList != it->waitList || next->lockCount != it->lockCount )
            {
                break;
            }
            next++;
        }
        break;
    case LockState::WaitLock:
        while( next < end )
        {
            if( GetThreadBit( next->lockingThread ) == threadBit )
            {
                nextState = AreOtherWaiting( next->waitList, threadBit ) ? LockState::HasBlockingLock : LockState::HasLock;
                break;
            }
            if( next->lockingThread != it->lockingThread )
            {
                break;
            }
            if( next->lockCount == 0 )
            {
                break;
            }
            next++;
        }
        break;
    default:
        assert( false );
        break;
    }

    return next;
}

static LockState::Type CombineLockState( LockState::Type state, LockState::Type next )
{
    return std::max( state, next );
}

static Vector<LockEventPtr>::const_iterator GetNextLockEventShared( const Vector<LockEventPtr>::const_iterator& it, const Vector<LockEventPtr>::const_iterator& end, LockState::Type& nextState, size_t threadBit )
{
    const auto itptr = (const LockEventShared*)(const LockEvent*)it->ptr;
    auto next = it;
    next++;

    switch( nextState )
    {
    case LockState::Nothing:
        while( next < end )
        {
            const auto ptr = (const LockEventShared*)(const LockEvent*)next->ptr;
            if( next->lockCount != 0 )
            {
                const ThreadWaitList wait = BitListUnion( next->waitList, ptr->waitShared );
                if( GetThreadBit( next->lockingThread ) == threadBit )
                {
                    nextState = AreOtherWaiting( wait, threadBit ) ? LockState::HasBlockingLock : LockState::HasLock;
                    break;
                }
                else if( IsThreadWaiting( wait, threadBit ) )
                {
                    nextState = LockState::WaitLock;
                    break;
                }
            }
            else if( IsThreadWaiting( ptr->sharedList, threadBit ) )
            {
                nextState = ( next->waitList.Any() ) ? LockState::HasBlockingLock : LockState::HasLock;
                break;
            }
            else if( ptr->sharedList.Any() && IsThreadWaiting( next->waitList, threadBit ) )
            {
                nextState = LockState::WaitLock;
                break;
            }
            next++;
        }
        break;
    case LockState::HasLock:
        while( next < end )
        {
            const auto ptr = (const LockEventShared*)(const LockEvent*)next->ptr;
            if( next->lockCount == 0 && !IsThreadWaiting( ptr->sharedList, threadBit ) )
            {
                nextState = LockState::Nothing;
                break;
            }
            if( next->waitList.Any() )
            {
                if( AreOtherWaiting( next->waitList, threadBit ) )
                {
                    nextState = LockState::HasBlockingLock;
                }
                break;
            }
            else if( !IsThreadWaiting( ptr->sharedList, threadBit ) && ptr->waitShared.Any() )
            {
                nextState = LockState::HasBlockingLock;
                break;
            }
            if( next->waitList != it->waitList || ptr->waitShared != itptr->waitShared || next->lockCount != it->lockCount || ptr->sharedList != itptr->sharedList )
            {
                break;
            }
            next++;
        }
        break;
    case LockState::HasBlockingLock:
        while( next < end )
        {
            const auto ptr = (const LockEventShared*)(const LockEvent*)next->ptr;
            if( next->lockCount == 0 && !IsThreadWaiting( ptr->sharedList, threadBit ) )
            {
                nextState = LockState::Nothing;
                break;
            }
            if( next->waitList != it->waitList || ptr->waitShared != itptr->waitShared || next->lockCount != it->lockCount || ptr->sharedList != itptr->sharedList )
            {
                break;
            }
            next++;
        }
        break;
    case LockState::WaitLock:
        while( next < end )
        {
            const auto ptr = (const LockEventShared*)(const LockEvent*)next->ptr;
            if( GetThreadBit( next->lockingThread ) == threadBit )
            {
                const ThreadWaitList wait = BitListUnion( next->waitList, ptr->waitShared );
                nextState = AreOtherWaiting( wait, threadBit ) ? LockState::HasBlockingLock : LockState::HasLock;
                break;
            }
            if( IsThreadWaiting( ptr->sharedList, threadBit ) )
            {
                nextState = ( next->waitList.Any() ) ? LockState::HasBlockingLock : LockState::HasLock;
                break;
            }
            if( next->lockingThread != it->lockingThread )
            {
                break;
            }
            if( next->lockCount == 0 && !IsThreadWaiting( ptr->waitShared, threadBit ) )
            {
                break;
            }
            next++;
        }
        break;
    default:
        assert( false );
        break;
    }

    return next;
}

void TimelineItemThread::PreprocessLocks( const TimelineContext& ctx, const unordered_flat_set<uint32_t> &lockIds, const unordered_flat_map<uint32_t, LockMap*>& locks, uint32_t tid, TaskDispatch& td, bool visible )
{
    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;
    const auto nspx = ctx.nspx;

    const auto& vd = m_view.GetViewData();
    const bool drawContendedLocks = ( ( vd.lockDrawFlags & ViewData::ELockDrawVisFlags::Contended ) != 0 );
    const bool drawUncontendedLocks = ( ( vd.lockDrawFlags & ViewData::ELockDrawVisFlags::Uncontended ) != 0 );
    const bool drawSingleThreadLocks = ( ( vd.lockDrawFlags & ViewData::ELockDrawVisFlags::SingleThread ) != 0 );
    const bool drawSingleThreadTermLocks = ( ( vd.lockDrawFlags & ViewData::ELockDrawVisFlags::SingleTerminated ) != 0 );

    const auto lockInfoWindow = m_view.GetLockInfoWindow();
    const auto lockInfoHighlight = m_view.GetLockHoverHighlight();

    const auto MinVisNs = int64_t( round( GetScale() * MinVisSize * nspx ) );

    const size_t lcount = lockIds.size();
    std::unique_ptr<LockDraw> highlightedFromMerged;
    m_mergeJobCounter.fetch_add( lcount );

    for( uint32_t lockId : lockIds )
    {
        if ( ( lockInfoWindow != View::InvalidId ) && ( lockInfoWindow != lockId ) )
        {
            m_mergeJobCounter.fetch_sub( 1 );
            continue;
        }

        const auto &lockmapit = locks.find( lockId );
        assert( lockmapit != locks.end() );
        const auto &lockmap = *lockmapit->second;

        if ( !lockmap.valid )
        {
            m_mergeJobCounter.fetch_sub( 1 );
            continue;
        }
        if ( !m_view.Vis( &lockmap ) )
        {
            m_mergeJobCounter.fetch_sub( 1 );
            continue;
        }

        const bool isTerminatedLock = ( ( lockmap.timeTerminate > 0 ) || ( lockmap.isTerminated ) );
        const bool isContendedLock = lockmap.isContended;
        const bool isMultiThread = lockmap.isMultiThread;

        const bool drawContended = ( isContendedLock && drawContendedLocks );
        const bool drawUncontended = ( !isContendedLock && isMultiThread && drawUncontendedLocks );
        const bool drawSingleThread = ( !isMultiThread && !isTerminatedLock && drawSingleThreadLocks );
        const bool drawSingleThreadTerm = ( !isMultiThread && isTerminatedLock && drawSingleThreadTermLocks );

        const bool drawLock = ( drawContended || drawUncontended || drawSingleThread || drawSingleThreadTerm );
        if ( !drawLock )
        {
            m_mergeJobCounter.fetch_sub( 1 );
            continue;
        }

        auto it = lockmap.threadMap.find( tid );
        if ( it == lockmap.threadMap.end() )
        {
            m_mergeJobCounter.fetch_sub( 1 );
            continue;
        }

        assert( !lockmap.timeline.empty() );
        const auto& range = lockmap.range[it->second];
        if( range.start > vEnd || range.end < vStart )
        {
            if ( lockInfoWindow == lockId )
            {
                m_lockDraw.emplace_back( std::make_unique<LockDraw>( LockDraw{ lockId, true, it->second } ) );
            }
            m_mergeJobCounter.fetch_sub( 1 );
            continue;
        }

        int64_t actualMinVisNs = MinVisNs;
        auto drawData = std::make_unique<LockDraw>( LockDraw{ lockId, false, it->second, isTerminatedLock } );
        LockDraw *drawPtr = drawData.get();

        if ( drawSingleThreadTerm && vd.drawMergedLocks && ( lockId != lockInfoHighlight ) )
        {
            actualMinVisNs = 0; // prevent PreprocessLockTimeline from condensing lock events just yet
            m_mergeLockDrawTmp.emplace_back( std::move( drawData ) );
        }
        else
        {
            if ( drawSingleThreadTerm && vd.drawMergedLocks && ( lockId == lockInfoHighlight ) )
            {
                highlightedFromMerged = std::move( drawData );
            }
            else
            {
                m_lockDraw.emplace_back( std::move( drawData ) );
            }
        }

        Worker &worker = m_worker;
        LockType type = lockmap.type;
        uint8_t threadIndex = it->second;
        const Vector<LockEventPtr> &timeline = lockmap.timeline;
        const bool onlyContended = (lockmap.isContended && vd.onlyContendedLocks);
        td.Queue( [ this, lockId, threadIndex, type, tid, onlyContended, &locks, &timeline, &ctx, &range, &vd, visible, drawPtr, MinVisNs, actualMinVisNs ]
                {
                    PreprocessLockTimeline( ctx, type, onlyContended, timeline, range, threadIndex, lockId, visible, drawPtr, actualMinVisNs );
                    if ( m_mergeJobCounter.fetch_sub( 1 ) == 1 )
                    {
                        PreprocessMergedLocks( ctx, locks, tid, MinVisNs, visible );
                    }
                } );
    }

    if ( highlightedFromMerged )
    {
        m_lockDraw.emplace_back( std::move( highlightedFromMerged ) );
    }
}

void TimelineItemThread::PreprocessLockTimeline( const TimelineContext& ctx,
                                                 LockType type,
                                                 bool onlyContended,
                                                 const Vector<LockEventPtr>& timeline,
                                                 const LockTimeRange range,
                                                 uint8_t threadIndex,
                                                 uint32_t lockId,
                                                 bool visible,
                                                 LockDraw* drawPtr,
                                                 int64_t MinVisNs ) const
{
    const auto vStart = ctx.vStart;
    const auto vEnd = ctx.vEnd;

    auto GetNextLockFunc = type == LockType::Lockable ? GetNextLockEvent : GetNextLockEventShared;
    const auto thread = threadIndex;
    const auto threadBit = GetThreadBit( thread );
    const auto& tl = timeline;

    auto vbegin = std::lower_bound( tl.begin(), tl.end(), std::max( range.start, vStart ), [] ( const auto& l, const auto& r ) { return l.ptr->Time() < r; } );
    const auto vend = std::lower_bound( vbegin, tl.end(), std::min( range.end, vEnd ), [] ( const auto& l, const auto& r ) { return l.ptr->Time() < r; } );

    if( vbegin > tl.begin() ) vbegin--;

    LockState::Type state = LockState::Nothing;
    if( type == LockType::Lockable )
    {
        if( vbegin->lockCount != 0 )
        {
            if( vbegin->lockingThread == thread )
            {
                state = AreOtherWaiting( vbegin->waitList, threadBit ) ? LockState::HasBlockingLock : LockState::HasLock;
            }
            else if( IsThreadWaiting( vbegin->waitList, threadBit ) )
            {
                state = LockState::WaitLock;
            }
        }
    }
    else
    {
        auto ptr = (const LockEventShared*)(const LockEvent*)vbegin->ptr;
        if( vbegin->lockCount != 0 )
        {
            if( vbegin->lockingThread == thread )
            {
                state = ( AreOtherWaiting( vbegin->waitList, threadBit ) || AreOtherWaiting( ptr->waitShared, threadBit ) ) ? LockState::HasBlockingLock : LockState::HasLock;
            }
            else if( IsThreadWaiting( vbegin->waitList, threadBit ) || IsThreadWaiting( ptr->waitShared, threadBit ) )
            {
                state = LockState::WaitLock;
            }
        }
        else if( IsThreadWaiting( ptr->sharedList, threadBit ) )
        {
            state = vbegin->waitList.Any() ? LockState::HasBlockingLock : LockState::HasLock;
        }
        else if( ptr->sharedList.Any() && IsThreadWaiting( vbegin->waitList, threadBit ) )
        {
            state = LockState::WaitLock;
        }
    }

    const uint8_t mask = onlyContended ? ( LockState::Nothing | LockState::HasLock ) : LockState::Nothing;
    if( !visible )
    {
        while( vbegin < vend && ( state & mask ) != 0 )
        {
            vbegin = GetNextLockFunc( vbegin, vend, state, threadBit );
        }
        drawPtr->forceDraw = vbegin < vend;
        return;
    }

    auto& dst = drawPtr->data;
    for(;;)
    {
        while( vbegin < vend && ( state & mask ) != 0 )
        {
            vbegin = GetNextLockFunc( vbegin, vend, state, threadBit );
        }
        if( vbegin >= vend ) break;
        assert( ( state & mask ) == 0 );

        LockState::Type drawState = state;
        auto next = GetNextLockFunc( vbegin, vend, state, threadBit );
        auto lastValid = next;

        const auto tStart = vbegin->ptr->Time();
        int64_t t0 = tStart;
        int64_t t1 = next == tl.end() ? m_worker.GetLastTime() : next->ptr->Time();
        uint32_t condensed = 0;

        for(;;)
        {
            if( next >= vend || t1 - t0 > MinVisNs ) break;
            auto n = next;
            auto ns = state;
            while( n < vend && ( ns & mask ) != 0 )
            {
                n = GetNextLockFunc( n, vend, ns, threadBit );
            }
            if( n >= vend ) break;
            if( n == next )
            {
                n = GetNextLockFunc( n, vend, ns, threadBit );
            }
            drawState = CombineLockState( drawState, state );
            const auto t2 = n == tl.end() ? m_worker.GetLastTime() : n->ptr->Time();
            if( t2 - t1 > MinVisNs ) break;
            if( drawState != ns && t2 - tStart > MinVisNs && ( ns & mask ) == 0 ) break;

            if ( state != LockState::Nothing )
            {
                // We are checking the size of both, lock "zones" and empty space between them.
                // We need to make sure that the next pointer properly points to the end of a zone
                // and not to the beginning, otherwise our t1 is incorrect if we don't end up
                // merging zones after all (e.g. small zone followed by a small empty space
                // followed by a large zone. In that case we need to make sure our end time
                // is the end of the zone, not the beginning of the next one)
                condensed++;
                lastValid = next;
            }

            t0 = t1;
            t1 = t2;
            next = n;
            state = ns;
        }

        if ( condensed <= 1 )
        {
            // If we didn't do any merging after all, but the distance between the current and next event is less than MinVisNs
            // t1 would point to the start time of the next lock event and therefore the tooltip would display the wrong time.
            t1 = lastValid == tl.end() ? m_worker.GetLastTime() : lastValid->ptr->Time();
        }

        dst.emplace_back( LockDrawItem { t1, drawState, condensed, lockId, vbegin, lastValid } );

        vbegin = next;
    }
}

void TimelineItemThread::PreprocessMergedLocks( const TimelineContext &ctx, const unordered_flat_map<uint32_t, LockMap*>& locks, uint32_t tid, int64_t MinVisNs, bool visible )
{
    if ( m_mergeLockDrawTmp.empty() )
    {
        return;
    }

    const StringIdx nameIdx = StringIdx( m_worker.StoreString( "Merged Locks", sizeof( "Merged Locks" ) - 1 ).idx );

    size_t allEventCount = 0;
    for ( const std::unique_ptr<LockDraw> &draw : m_mergeLockDrawTmp )
    {
        allEventCount += draw->data.size();
    }

    if ( allEventCount )
    {
        struct LockZones
        {
            const LockEventPtr *beg = nullptr;
            const LockEventPtr *end = nullptr;
            size_t mergeIndex = 0;
            uint32_t lockId = 0;
        };
        Vector<LockZones> allZones;
        allZones.reserve( allEventCount );

        for ( const std::unique_ptr<LockDraw> &drawData : m_mergeLockDrawTmp )
        {
            for ( const LockDrawItem &item : drawData->data )
            {
                LockZones& zone = allZones.push_next();
                zone.beg = item.ptr;
                zone.end = item.next;
                zone.lockId = item.lockId;
                zone.mergeIndex = 0;
            }
        }

        std::sort( allZones.begin(), allZones.end(),
                   []( const LockZones &lhs, const LockZones &rhs )
                   {
                       const int64_t l = lhs.beg->ptr->Time();
                       const int64_t r = rhs.beg->ptr->Time();
                       return l < r;
        } );

        struct MergeInfo
        {
            LockTimeRange range;
            size_t count;
            unordered_flat_set< uint32_t > lockIdSet;
        };

        Vector<MergeInfo> mergeRanges;
        mergeRanges.push_back( MergeInfo{ { allZones.front().beg->ptr->Time(), allZones.front().end->ptr->Time()}, 1, { allZones.front().lockId } } );

        for ( size_t index = 1, count = allZones.size(); index < count; index++ )
        {
            LockZones &cur = allZones[ index ];

            bool overlaps = true;
            for ( size_t mergeIndex = 0, mergeCount = mergeRanges.size(); mergeIndex < mergeCount; mergeIndex++ )
            {
                MergeInfo &info = mergeRanges[ mergeIndex ];
                const int64_t startTime = cur.beg->ptr->Time();
                if ( startTime >= info.range.end )
                {
                    info.range.end = cur.end->ptr->Time();
                    info.count++;
                    info.lockIdSet.emplace( cur.lockId );

                    cur.mergeIndex = mergeIndex;
                    overlaps = false;
                    break;
                }
            }

            if ( overlaps )
            {
                cur.mergeIndex = mergeRanges.size();
                mergeRanges.push_back( MergeInfo{ { cur.beg->ptr->Time(), cur.end->ptr->Time() }, 1, { cur.lockId } } );
            }
        }

        size_t mergeCount = mergeRanges.size();
        std::vector<std::vector<uint32_t>> mergeInfoLockIds;
        mergeInfoLockIds.resize( mergeCount );

        m_mergedLockMaps.resize( mergeCount );
        for ( size_t mergeIndex = 0; mergeIndex < mergeCount; mergeIndex++ )
        {
            MergeInfo &info = mergeRanges[ mergeIndex ];

            m_mergedLockMaps[ mergeIndex ] = std::make_unique<LockMap>();
            LockMap *lm = m_mergedLockMaps[ mergeIndex ].get();
            lm->customName = nameIdx;
            lm->srcloc = 0;
            lm->timeline.reserve( info.count * 2 );
            lm->threadMap.emplace( m_thread->id, 0 );
            lm->threadList.push_back( m_thread->id );
            lm->type = LockType::Lockable;
            lm->timeAnnounce = 0;
            lm->timeTerminate = 0;
            lm->valid = true;
            lm->isContended = false;
            lm->isTerminated = true;
            lm->lockCount = info.lockIdSet.size();
            lm->lockingThread = 0;
            mergeInfoLockIds[ mergeIndex ].reserve( info.count * 2 );
        }

        for ( LockZones &z : allZones )
        {
            LockMap *lm = m_mergedLockMaps[ z.mergeIndex ].get();
            lm->timeline.push_back( *z.beg );
            lm->timeline.push_back( *z.end );

            lm->range[ 0 ].start = std::min( lm->range[ 0 ].start, z.beg->ptr->Time() );
            lm->range[ 0 ].end = std::max( lm->range[ 0 ].end, z.end->ptr->Time() );
            lm->timeAnnounce = lm->range[ 0 ].start;
            lm->timeTerminate = lm->range[ 0 ].end;

            mergeInfoLockIds[ z.mergeIndex ].push_back( z.lockId );
            mergeInfoLockIds[ z.mergeIndex ].push_back( z.lockId );
        }

        for ( size_t mergeIndex = 0; mergeIndex < mergeCount; mergeIndex++ )
        {
            const MergeInfo &mi = mergeRanges[ mergeIndex ];
            const LockMap *lm = m_mergedLockMaps[ mergeIndex ].get();

            const std::vector<uint32_t>& lockIds = mergeInfoLockIds[ mergeIndex ];
            assert( lockIds.size() == lm->timeline.size() );

            const uint32_t lockId = std::numeric_limits<uint32_t>::max() - (uint32_t)mergeIndex;
            auto drawData = std::make_unique<LockDraw>( LockDraw{ lockId, false, 0, true, true, lm });
            auto drawPtr = drawData.get();
            m_lockDraw.emplace_back( std::move( drawData ) );

            auto it = lm->threadMap.find( tid );
            const auto &range = lm->range[ it->second ];

            const size_t vend = lm->timeline.size();
            Worker &worker = m_worker;
            LockType type = lm->type;
            uint8_t threadIndex = it->second;
            const Vector<LockEventPtr> &timeline = lm->timeline;
            PreprocessLockTimeline( ctx, type, false, timeline, range, threadIndex, 0, visible, drawPtr, MinVisNs );

            const size_t tlCount = timeline.size();
            size_t tlIndex = 0;
            for ( LockDrawItem &item : drawPtr->data )
            {
                while ( tlIndex < tlCount )
                {
                    if ( timeline[ tlIndex ].ptr == item.ptr->ptr )
                    {
                        item.lockId = lockIds[ tlIndex ];
                        break;
                    }

                    tlIndex++;
                }
            }
        }
    }
}

}
