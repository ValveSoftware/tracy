#include <math.h>

#include "TracyColor.hpp"
#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineDraw.hpp"
#include "TracyTimelineItem.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyView.hpp"
#include "tracy_pdqsort.h"
#include "../Fonts.hpp"

constexpr float MinVisSize = 3;

namespace tracy
{

struct ThreadStackInfo
{
    ZoneEvent ev;
    int64_t count;
    bool merged;
};

static void ExtractThreadStack( Worker &worker, int64_t vAt, int64_t vStart, int64_t vEnd, double nsPerPixel, const Vector<short_ptr<ZoneEvent>> &vec, int depth, Vector<ThreadStackInfo> &rStack );

template<typename Adapter, typename V>
static void ExtractThreadStack( Worker& worker, int64_t vAt, int64_t vStart, int64_t vEnd, double nsPerPixel, const V& vec, int depth, Vector<ThreadStackInfo>& rStack )
{
    auto it = std::lower_bound( vec.begin(), vec.end(), vStart, [&worker] ( const auto& l, const auto& r ) { Adapter a; return worker.GetZoneEnd( a(l) ) <= r; } );
    if ( it == vec.end() )
    {
        return;
    }

    const auto zitend = std::lower_bound( it, vec.end(), vEnd, [] ( const auto& l, const auto& r ) { Adapter a; return a(l).Start() <= r; } );
    if ( it == zitend )
    {
        return;
    }

    Adapter a;
    if ( !a( *it ).IsEndValid() && worker.GetZoneEnd( a( *it ) ) < vStart )
    {
        return;
    }

    if ( worker.GetZoneEnd( a( *( zitend - 1 ) ) ) < vStart )
    {
        return;
    }

    const int64_t MinVisNs = int64_t( round( GetScale() * MinVisSize * nsPerPixel ) );

    assert( it != zitend );
    assert( worker.GetZoneEnd( a( *it ) ) >= vStart );
    assert( a( *(zitend - 1) ).Start() <= vEnd );

    const bool wasParentMerged = ( !rStack.empty() && ( rStack.back().merged ) );
    const int64_t totalEventCount = ( zitend - it );

    while( it < zitend )
    {
        const ZoneEvent &ev = a( *it );
        const int64_t start = ev.Start();
        const int64_t end = worker.GetZoneEnd( ev );
        const int64_t zsz = end - ev.Start();
        if ( wasParentMerged || ( ( zsz < MinVisNs ) && (totalEventCount > 1) ) )
        {
            const int64_t count = ( zitend - it );
            if ( rStack.size() <= depth )
            {
                rStack.push_back( ThreadStackInfo{ ZoneEvent(), 0, false});
            }
            ThreadStackInfo &rInfo = rStack[ depth ];

            rInfo.count += count;
            rInfo.merged = true;

            while ( it < zitend )
            {
                const ZoneEvent &innerev = a( *it );
                assert( innerev.Start() <= std::min( end + MinVisNs, vEnd ) );
                if ( innerev.HasChildren() )
                {
                    const Vector<short_ptr<ZoneEvent>> &children = worker.GetZoneChildren( innerev.Child() );
                    ExtractThreadStack( worker, vAt, vStart, vEnd, nsPerPixel, children, depth + 1, rStack );
                }

                it++;
            }

            break;
        }
        else
        {
            if ( vAt >= start && vAt <= end )
            {
                rStack.push_back( ThreadStackInfo{ ev, 1, false});
                if ( ev.HasChildren() )
                {
                    const Vector<short_ptr<ZoneEvent>> &children = worker.GetZoneChildren( ev.Child() );
                    ExtractThreadStack( worker, vAt, vStart, vEnd, nsPerPixel, children, depth + 1, rStack );
                }
                break;
            }
            ++it;
        }
    }
}


static void ExtractThreadStack( Worker &worker, int64_t vAt, int64_t vStart, int64_t vEnd, double nsPerPixel, const Vector<short_ptr<ZoneEvent>> &vec, int depth, Vector<ThreadStackInfo>& rStack )
{
    if ( vec.is_magic() )
    {
        ExtractThreadStack<VectorAdapterDirect<ZoneEvent>>( worker, vAt, vStart, vEnd, nsPerPixel, *(Vector<ZoneEvent>*)( &vec ), depth, rStack );
    }
    else
    {
        ExtractThreadStack<VectorAdapterPointer<ZoneEvent>>( worker, vAt, vStart, vEnd, nsPerPixel, vec, depth, rStack );
    }
}



bool View::DrawCpuData( const TimelineContext& ctx, const std::vector<CpuUsageDraw>& cpuDraw, const std::vector<std::vector<CpuCtxDraw>>& ctxDraw, int& offset, bool hasCpuData, bool drawThreadInteractions )
{
    auto cpuData = m_worker.GetCpuData();
    const auto cpuCnt = m_worker.GetCpuDataCpuCount();
    assert( cpuCnt != 0 );

    const auto& wpos = ctx.wpos;
    const auto w = ctx.w;
    const auto ty = ctx.ty;
    const auto sty = ctx.sty;
    const auto pxns = ctx.pxns;
    const auto nspx = ctx.nspx;
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );
    const auto yMin = ctx.yMin;
    const auto yMax = ctx.yMax;
    const auto hover = ctx.hover;
    const auto vStart = ctx.vStart;

    auto draw = ImGui::GetWindowDrawList();

    if( hasCpuData && m_vd.drawCpuUsageGraph )
    {
        const auto cpuUsageHeight = floor( 30.f * GetScale() );
        if( wpos.y + offset + cpuUsageHeight + 3 >= yMin && wpos.y + offset <= yMax )
        {
            const float cpuCntRev = 1.f / cpuCnt;
            int pos = 0;
            for( auto& v : cpuDraw )
            {
                float base;
                if( v.own != 0 )
                {
                    base = dpos.y + offset + ( 1.f - v.own * cpuCntRev ) * cpuUsageHeight;
                    DrawLine( draw, ImVec2( dpos.x + pos, dpos.y + offset + cpuUsageHeight ), ImVec2( dpos.x + pos, base ), 0xFF55BB55 );
                }
                else
                {
                    base = dpos.y + offset + cpuUsageHeight;
                }
                if( v.other != 0 )
                {
                    int usageTotal = v.own + v.other;
                    DrawLine( draw, ImVec2( dpos.x + pos, base ), ImVec2( dpos.x + pos, dpos.y + offset + ( 1.f - usageTotal * cpuCntRev ) * cpuUsageHeight ), 0xFF666666 );
                }
                pos++;
            }
            DrawLine( draw, dpos + ImVec2( 0, offset+cpuUsageHeight+2 ), dpos + ImVec2( w, offset+cpuUsageHeight+2 ), 0x22DD88DD );

            if( hover && ImGui::IsMouseHoveringRect( ImVec2( wpos.x, wpos.y + offset ), ImVec2( wpos.x + w, wpos.y + offset + cpuUsageHeight ), true ) )
            {
                ImGui::BeginTooltip();
                if( cpuDraw.size() > ( ImGui::GetIO().MousePos.x - wpos.x ) )
                {
                    const auto& usage = cpuDraw[ImGui::GetIO().MousePos.x - wpos.x];
                    TextFocused( "Cores used by profiled program:", RealToString( usage.own ) );
                    ImGui::SameLine();
                    char buf[64];
                    PrintStringPercent( buf, usage.own * cpuCntRev * 100 );
                    TextDisabledUnformatted( buf );
                    TextFocused( "Cores used by other programs:", RealToString( usage.other ) );
                    ImGui::SameLine();
                    PrintStringPercent( buf, usage.other * cpuCntRev * 100 );
                    TextDisabledUnformatted( buf );
                    TextFocused( "Number of cores:", RealToString( cpuCnt ) );
                    if( usage.own + usage.other != 0 )
                    {
                        const auto mt = m_vd.zvStart + ( ImGui::GetIO().MousePos.x - wpos.x ) * nspx;
                        ImGui::Separator();
                        for( int i=0; i<cpuCnt; i++ )
                        {
                            if( !cpuData[i].cs.empty() )
                            {
                                auto& cs = cpuData[i].cs;
                                auto it = std::lower_bound( cs.begin(), cs.end(), mt, [] ( const auto& l, const auto& r ) { return (uint64_t)l.End() < (uint64_t)r; } );
                                if( it != cs.end() && it->Start() <= mt && it->End() >= mt )
                                {
                                    const std::string cpuName = m_worker.GetFormattedCpuName( i );
                                    ImGui::TextDisabled( cpuName.c_str() );
                                    ImGui::SameLine();
                                    const auto thread = m_worker.DecompressThreadExternal( it->Thread() );
                                    bool local, untracked;
                                    const char* txt;
                                    auto label = GetThreadContextData( thread, local, untracked, txt );
                                    if( local || untracked )
                                    {
                                        uint32_t color;
                                        if( m_vd.dynamicColors != 0 )
                                        {
                                            color = local ? GetThreadColor( thread, 0 ) : ( untracked ? 0xFF663333 : 0xFF444444 );
                                        }
                                        else
                                        {
                                            color = local ? 0xFF334488 : ( untracked ? 0xFF663333 : 0xFF444444 );
                                        }
                                        TextColoredUnformatted( HighlightColor<75>( color ), label );
                                        ImGui::SameLine();
                                        ImGui::TextDisabled( "(%s)", RealToString( thread ) );
                                    }
                                    else
                                    {
                                        TextDisabledUnformatted( label );
                                    }
                                }
                            }
                        }
                    }
                }
                else
                {
                    TextFocused( "Cores used by profiled program:", "0" );
                    TextFocused( "Cores used by other programs:", "0" );
                    TextFocused( "Number of cores:", RealToString( cpuCnt ) );
                }
                ImGui::EndTooltip();
            }
        }
        offset += cpuUsageHeight + 3;
    }

    ImGui::PushFont( g_fonts.normal, FontSmall );
    const auto sstep = sty + 1;

    const auto origOffset = offset;
    for( int i=0; i<cpuCnt; i++ )
    {
        DrawLine( draw, dpos + ImVec2( 0, offset+sty ), dpos + ImVec2( w, offset+sty ), 0x22DD88DD );
        auto tt = m_worker.GetThreadTopology( CpuThreadId{ (uint32_t)i } );

        if( ( i < ctxDraw.size() ) && !ctxDraw[i].empty() && wpos.y + offset + sty >= yMin && wpos.y + offset <= yMax )
        {
            auto& cs = cpuData[i].cs;
            for( auto& v : ctxDraw[i] )
            {
                const auto& ev = cs[v.idx];
                const auto t0 = ev.Start();
                const auto px0 = ( t0 - vStart ) * pxns;
                if( v.num > 0 )
                {
                    const auto& eev = cs[v.idx + v.num - 1];
                    const auto t1 = eev.IsEndValid() ? eev.End() : eev.Start();
                    const auto px1 = ( t1 - vStart ) * pxns;
                    DrawZigZag( draw, wpos + ImVec2( 0, offset + sty/2 ), std::max( px0, -10.0 ), std::min( std::max( px1, px0+MinVisSize ), double( w + 10 ) ), sty/4, 0xFF888888 );

                    if( hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( px0, offset-1 ), wpos + ImVec2( std::max( px1, px0+MinVisSize ), offset + sty ) ) )
                    {
                        ImGui::PopFont();
                        ImGui::BeginTooltip();
                        TextFocused( "CPU:", RealToString( i ) );
                        if( tt )
                        {
                            ImGui::SameLine();
                            ImGui::Spacing();
                            ImGui::SameLine();
                            TextFocused( "Package:", RealToString( tt->package.val ) );
                            ImGui::SameLine();
                            TextFocused( "Group:", RealToString( tt->group.val ) );
                            ImGui::SameLine();
                            TextFocused( "Core:", RealToString( tt->core.val ) );
                        }
                        TextFocused( "Context switch regions:", RealToString( v.num ) );
                        ImGui::Separator();
                        TextFocused( "Start time:", TimeToString( t0 ) );
                        TextFocused( "End time:", TimeToString( t1 ) );
                        TextFocused( "Activity time:", TimeToString( t1 - t0 ) );
                        ImGui::EndTooltip();
                        ImGui::PushFont( g_fonts.normal, FontSmall );

                        if( IsMouseClicked( 2 ) )
                        {
                            ZoomToRange( t0, t1 );
                        }
                    }
                }
                else
                {
                    const auto end = ev.IsEndValid() ? ev.End() : ev.Start();
                    const auto px1 = ( end - vStart ) * pxns;

                    const auto thread = m_worker.DecompressThreadExternal( ev.Thread() );
                    bool local, untracked;
                    const char* txt;
                    auto label = GetThreadContextData( thread, local, untracked, txt );

                    auto getDisplayThreadColor = [this]( uint64_t thread, bool local, bool untracked ) {
                        if( m_vd.dynamicColors != 0 )
                        {
                            return local ? GetThreadColor( thread, 0 ) : ( untracked ? 0xFF663333 : 0xFF444444 );
                        }
                        else
                        {
                            return local ? 0xFF334488 : ( untracked ? 0xFF663333 : 0xFF444444 );
                        }
                    };
                    uint32_t color = getDisplayThreadColor( thread, local, untracked );

                    draw->AddRectFilled( wpos + ImVec2( px0, offset ), wpos + ImVec2( px1, offset + sty ), color );
                    if( m_drawThreadHighlight == thread )
                    {
                        draw->AddRect( wpos + ImVec2( px0, offset ), wpos + ImVec2( px1, offset + sty ), 0xFFFFFFFF );
                    }
                    else
                    {
                        const auto accentColor = HighlightColor( color );
                        const auto darkColor = DarkenColor( color );
                        DrawLine( draw, dpos + ImVec2( px0, offset + sty ), dpos + ImVec2( px0, offset ), dpos + ImVec2( px1-1, offset ), accentColor, 1.f );
                        DrawLine( draw, dpos + ImVec2( px0, offset + sty ), dpos + ImVec2( px1-1, offset + sty ), dpos + ImVec2( px1-1, offset ), darkColor, 1.f );
                    }

                    const auto zsz = px1 - px0;
                    auto tsz = ImGui::CalcTextSize( label );
                    if( tsz.x < zsz )
                    {
                        const auto x = ( ev.Start() - m_vd.zvStart ) * pxns + ( ( end - ev.Start() ) * pxns - tsz.x ) / 2;
                        if( x < 0 || x > w - tsz.x )
                        {
                            ImGui::PushClipRect( wpos + ImVec2( px0, offset ), wpos + ImVec2( px1, offset + tsz.y * 2 ), true );
                            DrawTextContrast( draw, wpos + ImVec2( std::max( std::max( 0., px0 ), std::min( double( w - tsz.x ), x ) ), offset-1 ), local ? 0xFFFFFFFF : 0xAAFFFFFF, label );
                            ImGui::PopClipRect();
                        }
                        else if( ev.Start() == ev.End() )
                        {
                            DrawTextContrast( draw, wpos + ImVec2( px0 + ( px1 - px0 - tsz.x ) * 0.5, offset-1 ), local ? 0xFFFFFFFF : 0xAAFFFFFF, label );
                        }
                        else
                        {
                            DrawTextContrast( draw, wpos + ImVec2( x, offset-1 ), local ? 0xFFFFFFFF : 0xAAFFFFFF, label );
                        }
                    }
                    else
                    {
                        ImGui::PushClipRect( wpos + ImVec2( px0, offset ), wpos + ImVec2( px1, offset + tsz.y * 2 ), true );
                        DrawTextContrast( draw, wpos + ImVec2( ( ev.Start() - vStart ) * pxns, offset-1 ), local ? 0xFFFFFFFF : 0xAAFFFFFF, label );
                        ImGui::PopClipRect();
                    }

                    if( hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( px0, offset-1 ), wpos + ImVec2( px1, offset + sty ) ) )
                    {
                        m_drawThreadHighlight = thread;
                        ImGui::PopFont();
                        ImGui::BeginTooltip();
                        TextFocused( "CPU:", RealToString( i ) );
                        if( tt )
                        {
                            ImGui::SameLine();
                            ImGui::Spacing();
                            ImGui::SameLine();
                            TextFocused( "Package:", RealToString( tt->package.val ) );
                            ImGui::SameLine();
                            TextFocused( "Die:", RealToString( tt->die.val ) );
                            ImGui::SameLine();
                            TextFocused( "Core:", RealToString( tt->core.val ) );
                        }
                        if( local )
                        {
                            TextFocused( "Program:", m_worker.GetCaptureProgram().c_str() );
                            ImGui::SameLine();
                            TextDisabledUnformatted( "(profiled program)" );
                            SmallColorBox( GetThreadColor( thread, 0 ) );
                            ImGui::SameLine();
                            TextFocused( "Thread:", m_worker.GetThreadName( thread ) );
                            ImGui::SameLine();
                            ImGui::TextDisabled( "(%s)", RealToString( thread ) );
                            m_drawThreadMigrations = thread;
                            m_cpuDataThread = thread;
                        }
                        else
                        {
                            if( untracked )
                            {
                                TextFocused( "Program:", m_worker.GetCaptureProgram().c_str() );
                            }
                            else
                            {
                                TextFocused( "Program:", txt );
                            }
                            ImGui::SameLine();
                            if( untracked )
                            {
                                TextDisabledUnformatted( "(untracked thread in profiled program)" );
                            }
                            else
                            {
                                TextDisabledUnformatted( "(external)" );
                            }
                            TextFocused( "Thread:", m_worker.GetExternalName( thread ).second );
                            ImGui::SameLine();
                            ImGui::TextDisabled( "(%s)", RealToString( thread ) );
                        }
                        if ( ev.WakeupVal() != -1 )
                        {
                            uint8_t wakeupCpu = ev.WakeupCpu();

                            TextFocused( "Readying CPU:", RealToString( wakeupCpu ) );

                            const ThreadData *pThreadData = GetThreadDataForCpu( wakeupCpu, ev.WakeupVal() );
                            if ( pThreadData )
                            {
                                TextDisabledUnformatted( "Readying thread:" );
                                ImGui::SameLine();

                                const auto thread = pThreadData->id;
                                bool local, untracked;
                                const char *txt;
                                auto label = GetThreadContextData( thread, local, untracked, txt );
                                if ( local || untracked )
                                {
                                    uint32_t color;
                                    if ( m_vd.dynamicColors != 0 )
                                    {
                                        color = local ? GetThreadColor( thread, 0 ) : ( untracked ? 0xFF663333 : 0xFF444444 );
                                    }
                                    else
                                    {
                                        color = local ? 0xFF334488 : ( untracked ? 0xFF663333 : 0xFF444444 );
                                    }
                                    TextColoredUnformatted( HighlightColor<75>( color ), label );
                                    ImGui::SameLine();
                                    ImGui::TextDisabled( "(%s)", RealToString( thread ) );
                                }
                                else
                                {
                                    TextDisabledUnformatted( label );
                                }
                            }
                        }
                        ImGui::Separator();
                        TextFocused( "Start time:", TimeToStringExact( ev.Start() ) );
                        TextFocused( "End time:", TimeToStringExact( end ) );
                        TextFocused( "Activity time:", TimeToString( end - ev.Start() ) );
                        
                        // Display data about the switch in
                        auto threadCtxSwitches = m_worker.GetContextSwitchData( thread );
                        if( threadCtxSwitches )
                        {
                            auto& v = threadCtxSwitches->v;
                            auto it = std::lower_bound( v.begin(), v.end(), ev.Start(), [](const auto& l, const auto& r) { return l.Start() < r; } );
                            // We should have the data, or something went wrong.
                            assert( it != v.end() && it->Start() == ev.Start() );

                            // Do we have information about the previous CSwitch?
                            if( it != v.begin() )
                            {
                                auto& prev = *( it - 1 );
                                    
                                ImGui::Separator();

                                TextFocused( "Wait reason:", DecodeContextSwitchReasonCode( prev.Reason() ) );
                                ImGui::SameLine();
                                ImGui::PushFont( g_fonts.normal, FontSmall );
                                ImGui::AlignTextToFramePadding();
                                TextDisabledUnformatted( DecodeContextSwitchReason( prev.Reason() ) );
                                ImGui::PopFont();
                                TextFocused( "Wait state:", DecodeContextSwitchStateCode( prev.State() ) );
                                TextFocused( "Waiting time:", TimeToString( it->WakeupVal() - prev.End() ) );
                            }
                            
                            // Do we have information about the readying thread?
                            if( it->Start() - it->WakeupVal() )
                            {
                                ImGui::Separator();
                                TextFocused( "WakeUp delay:", TimeToString( it->Start() - it->WakeupVal() ) );
                                assert( it->WakeupCpu() < cpuCnt );
                                const auto& wakeUpCpuCSwitches = cpuData[it->WakeupCpu()].cs;
                                auto wakeupit = std::lower_bound( wakeUpCpuCSwitches.begin(), wakeUpCpuCSwitches.end(), it->WakeupVal(), []( const auto& l, const auto& r ) { return l.End() < r; } );
                                if( wakeupit != wakeUpCpuCSwitches.end()
                                    && wakeupit->Start() < it->WakeupVal()
                                    && it->WakeupVal() < wakeupit->End() )
                                {
                                    TextDisabledUnformatted( "Woken up by:" );
                                    ImGui::SameLine();

                                    const auto wakeupThread = m_worker.DecompressThreadExternal( wakeupit->Thread() );
                                    bool wakeupThreadLocal, wakeupThreadUntracked;
                                    const char* wakeUpThreadProgram;
                                    auto wakeuplabel = GetThreadContextData( wakeupThread, wakeupThreadLocal, wakeupThreadUntracked, wakeUpThreadProgram );
                                    
                                    uint32_t wakeupThreadColor = getDisplayThreadColor( wakeupThread, wakeupThreadLocal, wakeupThreadUntracked );
                                    TextColoredUnformatted( HighlightColor<75>( wakeupThreadColor ), wakeuplabel );
                                    ImGui::SameLine();
                                    ImGui::TextDisabled( "(%s)", RealToString( wakeupThread ) );
                                }
                                else
                                {
                                    TextDisabledUnformatted( "Woken up by Kernel" );
                                }
                            }

                        }


                        ImGui::EndTooltip();
                        ImGui::PushFont( g_fonts.normal, FontSmall );

                        if( local && IsMouseClicked( 0 ) )
                        {
                            auto& item = m_tc.GetItem( m_worker.GetThreadData( thread ) );
                            item.SetVisible( true );
                            item.SetShowFull( true );
                        }
                        if( IsMouseClicked( 2 ) )
                        {
                            ZoomToRange( ev.Start(), end );
                        }
                    }
                }
            }
        }

        const std::string cpuName = m_worker.GetFormattedCpuName( i );
        const auto txtx = ImGui::CalcTextSize( cpuName.c_str() ).x;
        DrawTextSuperContrast( draw, wpos + ImVec2( ty, offset-1 ), 0xFFDD88DD, cpuName.c_str() );
        if( hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( 0, offset-1 ), wpos + ImVec2( sty + txtx, offset + sty ) ) )
        {
            ImGui::PopFont();
            ImGui::BeginTooltip();
            TextFocused( "CPU:", RealToString( i ) );
            if( tt )
            {
                ImGui::SameLine();
                ImGui::Spacing();
                ImGui::SameLine();
                TextFocused( "Package:", RealToString( tt->package.val ) );
                ImGui::SameLine();
                TextFocused( "Die:", RealToString( tt->die.val ) );
                ImGui::SameLine();
                TextFocused( "Core:", RealToString( tt->core.val ) );

                std::vector<const Worker::CpuCacheInfo*> caches;
                m_worker.GetCachesForCore( tt, caches );
                if ( !caches.empty() )
                {
                    static const ImVec4 s_CacheCols[] =
                    {
                        (ImVec4)ImColor( 1.0f, 1.0f, 1.0f ),
                        (ImVec4)ImColor( 0.6f, 1.0f, 0.6f ),
                        (ImVec4)ImColor( 1.0f, 0.6f, 0.25f ),
                        (ImVec4)ImColor( 1.0f, 0.34f, 0.34f ),
                        (ImVec4)ImColor( 1.0f, 0.0f, 0.0f ),
                    };

                    static const char s_CacheTypePrefix[] =
                    {
                        'x',
                        'U',
                        'I',
                        'D',
                    };

                    TextDisabledUnformatted( "Caches:" );
                    for ( const Worker::CpuCacheInfo* cache : caches )
                    {
                        const int levelIndex = std::min( (int)cache->level, IM_ARRAYSIZE( s_CacheCols ) );
                        const int typeIndex = std::min( (int)cache->type, IM_ARRAYSIZE( s_CacheTypePrefix ) );
                        ImGui::TextColored( s_CacheCols[levelIndex]
                                            , "   L%u %c$ %s"
                                            , cache->level
                                            , s_CacheTypePrefix[ typeIndex ]
                                            , MemSizeToString( cache->size )
                        );
                    }
                }
            }
            TextFocused( "Context switch regions:", RealToString( cpuData[i].cs.size() ) );
            ImGui::EndTooltip();
            ImGui::PushFont( g_fonts.normal, FontSmall );
        }

        offset += sstep;
    }

    // Drawing threads interactions
    if ( drawThreadInteractions )
    {
        offset = origOffset;
        for ( int i = 0; i < cpuCnt; i++ )
        {
            if ( ( i < ctxDraw.size() ) && !ctxDraw[ i ].empty() && wpos.y + offset + sty >= yMin && wpos.y + offset <= yMax )
            {
                auto &cs = cpuData[ i ].cs;
                for ( auto &v : ctxDraw[ i ] )
                {
                    const auto &ev = cs[ v.idx ];
                    if ( ev.WakeupVal() != -1 )
                    {
                        const auto readycolor = 0xFF2280A0; // 0xFF0000FF;
                        const auto bgSize = GetScale() * 4.f;
                        const auto lnSize = GetScale() * 2.f;
                        const auto arrowOffset = GetScale() * 5.f;

                        const auto readyt0 = ev.WakeupVal();
                        const auto readycpu0 = ev.WakeupCpu();

                        const auto readyt1 = ev.Start();
                        const auto readycpu1 = i;

                        const auto readypx0 = ( readyt0 - m_vd.zvStart ) * pxns;
                        const auto readypy0 = origOffset + sty * 0.5f + readycpu0 * sstep;

                        const auto readypx1 = ( readyt1 - m_vd.zvStart ) * pxns;
                        const auto readypy1 = origOffset + sty * 0.5f + readycpu1 * sstep;

                        if ( readypx1 - readypx0 < 2 )
                        {
                            if ( readypx1 - readypx0 < 1 )
                            {
                                // Too small, don't draw anything
                            }
                            else
                            {
                                DrawLine( draw, dpos + ImVec2( readypx0, readypy0 ), dpos + ImVec2( readypx0, readypy1 ), dpos + ImVec2( readypx1, readypy1 ), readycolor );
                            }
                        }
                        else
                        {
                            DrawLine( draw, dpos + ImVec2( readypx0, readypy0 ), dpos + ImVec2( readypx0, readypy1 ), dpos + ImVec2( readypx1, readypy1 ), 0xFF000000, bgSize );
                            DrawLine( draw, dpos + ImVec2( readypx1 - arrowOffset, readypy1 - arrowOffset ), dpos + ImVec2( readypx1, readypy1 ), dpos + ImVec2( readypx1 - arrowOffset, readypy1 + arrowOffset ), 0xFF000000, bgSize );

                            DrawLine( draw, dpos + ImVec2( readypx0, readypy0 ), dpos + ImVec2( readypx0, readypy1 ), dpos + ImVec2( readypx1, readypy1 ), readycolor, lnSize );
                            DrawLine( draw, dpos + ImVec2( readypx1 - arrowOffset, readypy1 - arrowOffset ), dpos + ImVec2( readypx1, readypy1 ), dpos + ImVec2( readypx1 - arrowOffset, readypy1 + arrowOffset ), readycolor, lnSize );
                        }
                    }
                }
            }

            offset += sstep;
        }
    }

    if( ImGui::IsMouseHoveringRect( wpos, wpos + ImVec2( w, offset ) ) && IsMouseClickReleased( ImGuiMouseButton_Left ) )
    {
        if( m_drawThreadHighlight != 0 )
        {
            m_selectedThread = m_drawThreadHighlight;
        }
        else
        {
            // Clicked anywhere in the CPUData timeline that is not a thread => Clear selected thread.
            m_selectedThread = 0;
        }
    }
    if( m_drawThreadMigrations != 0 )
    {
        DrawThreadMigrations( ctx, origOffset, m_drawThreadMigrations );
    }

    if( m_selectedThread != 0 )
    {
        DrawThreadMigrations( ctx, origOffset, m_selectedThread );
    }

    ImGui::PopFont();
    return true;
}

void View::DrawThreadMigrations( const TimelineContext& ctx, const int origOffset, uint64_t thread )
{
    const auto& wpos = ctx.wpos;
    const auto w = ctx.w;
    const auto ty = ctx.ty;
    const auto sty = ctx.sty;
    const auto pxns = ctx.pxns;
    const auto nspx = ctx.nspx;
    const auto dpos = wpos + ImVec2(0.5f, 0.5f);
    const auto yMin = ctx.yMin;
    const auto yMax = ctx.yMax;
    const auto hover = ctx.hover;
    const auto vStart = ctx.vStart;
    const auto sstep = sty + 1;

    auto draw = ImGui::GetWindowDrawList();
    auto ctxSwitch = m_worker.GetContextSwitchData( thread );
    if( ctxSwitch )
    {
        const auto color = HighlightColor( GetThreadColor( thread, -8 ) );

        auto& v = ctxSwitch->v;
        auto it = std::lower_bound( v.begin(), v.end(), m_vd.zvStart, [] ( const auto& l, const auto& r ) { return l.End() < r; } );
        if( it != v.begin() ) --it;
        auto end = std::lower_bound( it, v.end(), m_vd.zvEnd, [] ( const auto& l, const auto& r ) { return l.Start() < r; } );
        if( end == v.end() ) --end;

        const auto bgSize = GetScale() * 4.f;
        const auto lnSize = GetScale() * 2.f;
        const auto wakeupLineSize = GetScale() * 1.5f;


        auto computeScreenPos = [&]( int64_t t, uint8_t cpu ) {
            const auto px = ( t - m_vd.zvStart ) * pxns;
            return dpos + ImVec2( px, origOffset + sty * 0.5f + cpu * sstep );
        };

        auto drawWakeUp = [&]( int64_t start, ImVec2 startPos, int64_t wakeup, uint8_t wakeupcpu, uint32_t wakecolor, bool forceDraw ) {
            if( start != wakeup )
            {
                const auto pw = computeScreenPos( wakeup, wakeupcpu );
                const auto wakeupWidthPixels = startPos.x - pw.x;
                if( forceDraw || ( wakeupWidthPixels >= 0.5 ) )
                {

                    DrawLine( draw, pw, startPos, wakecolor, wakeupLineSize );
                    draw->AddCircleFilled( pw, bgSize, wakecolor );
                        
                    // Vertical line at beginning of thread to emphasize wakeup
                    if( wakeupWidthPixels >= 3 )
                    {
                        const float halfPx = GetScale() * 0.5f;
                        DrawLine( draw, ImVec2{ startPos.x, startPos.y - sty * 0.5f - halfPx }, ImVec2{ startPos.x , startPos.y + sty * 0.5f + halfPx }, 0xFF000000, lnSize * 2 );
                        DrawLine( draw, ImVec2{ startPos.x, startPos.y - sty * 0.5f - halfPx }, ImVec2{ startPos.x , startPos.y + sty * 0.5f + halfPx }, wakecolor, lnSize );
                    }
                }
            }
        };

        if( it != v.end() && it->Start() > m_vd.zvStart )
        {
            drawWakeUp( it->Start(), computeScreenPos( it->Start(), it->Cpu() ), it->WakeupVal(), it->WakeupCpu(), 0xFF444444, true);
        }
        while( it < end )
        {
            const auto t0 = it->End();
            const auto cpu0 = it->Cpu();
            const auto waitReason = it->Reason();
            const auto waitState = it->State();

            ++it;

                
            const auto t1 = it->Start();
            const auto cpu1 = it->Cpu();

            const auto p0 = computeScreenPos( t0, cpu0 );
            const auto p1 = computeScreenPos( t1, cpu1 );

            const auto migrationWidthPixels = p1.x - p0.x;
            if( migrationWidthPixels < 2 )
            {
                DrawLine( draw, p0, p1, color );
            }
            else
            {
                DrawLine( draw, p0, p1, 0xFF000000, bgSize );
                DrawLine( draw, p0, p1, color, lnSize );
            }

            const auto hue = 0.38f * float(waitReason); // Golden angle, gives new colors for each reason
            const auto wakecolor = ImColor::HSV(hue, 1.f, 1.f);
            drawWakeUp( t1, p1, it->WakeupVal(), it->WakeupCpu(), wakecolor, (migrationWidthPixels >= 30) );
        }
    }
}

void View::DrawCpuDataWindow()
{
    const auto scale = GetScale();
    ImGui::SetNextWindowSize( ImVec2( 700 * scale, 800 * scale ), ImGuiCond_FirstUseEver );
    ImGui::Begin( "CPU data", &m_showCpuDataWindow );
    if( ImGui::GetCurrentWindowRead()->SkipItems ) { ImGui::End(); return; }

    struct PidData
    {
        std::vector<uint64_t> tids;
        CpuThreadData data;
    };

    const auto& ctd = m_worker.GetCpuThreadData();
    unordered_flat_map<uint64_t, PidData> pids;
    for( auto& v : ctd )
    {
        uint64_t pid = m_worker.GetPidFromTid( v.first );
        auto it = pids.find( pid );
        if( it == pids.end() )
        {
            it = pids.emplace( pid, PidData {} ).first;
        }
        it->second.tids.emplace_back( v.first );
        it->second.data.runningTime += v.second.runningTime;
        it->second.data.runningRegions += v.second.runningRegions;
        it->second.data.migrations += v.second.migrations;
    }

    TextFocused( "Tracked threads:", RealToString( ctd.size() ) );
    ImGui::SameLine();
    TextFocused( "Tracked processes:", RealToString( pids.size() ) );
    ImGui::Separator();
    ImGui::BeginChild( "##cpudata" );
    if( ImGui::BeginTable( "##cpudata", 5, ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY ) )
    {
        ImGui::TableSetupScrollFreeze( 0, 1 );
        ImGui::TableSetupColumn( "PID/TID", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize );
        ImGui::TableSetupColumn( "Name" );
        ImGui::TableSetupColumn( "Running time", ImGuiTableColumnFlags_PreferSortDescending );
        ImGui::TableSetupColumn( "Slices", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize );
        ImGui::TableSetupColumn( "Core jumps", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize );
        ImGui::TableHeadersRow();

        std::vector<unordered_flat_map<uint64_t, PidData>::iterator> psort;
        psort.reserve( pids.size() );
        for( auto it = pids.begin(); it != pids.end(); ++it ) psort.emplace_back( it );
        const auto& sortspec = *ImGui::TableGetSortSpecs()->Specs;
        switch( sortspec.ColumnIndex )
        {
        case 0:
            if( sortspec.SortDirection == ImGuiSortDirection_Descending )
            {
                pdqsort_branchless( psort.begin(), psort.end(), [] ( const auto& l, const auto& r ) { return l->first > r->first; } );
            }
            else
            {
                pdqsort_branchless( psort.begin(), psort.end(), [] ( const auto& l, const auto& r ) { return l->first < r->first; } );
            }
            break;
        case 1:
            if( sortspec.SortDirection == ImGuiSortDirection_Descending )
            {
                pdqsort_branchless( psort.begin(), psort.end(), [this] ( const auto& l, const auto& r ) { return strcmp( m_worker.GetExternalName( l->second.tids[0] ).first, m_worker.GetExternalName( r->second.tids[0] ).first ) > 0; } );
            }
            else
            {
                pdqsort_branchless( psort.begin(), psort.end(), [this] ( const auto& l, const auto& r ) { return strcmp( m_worker.GetExternalName( l->second.tids[0] ).first, m_worker.GetExternalName( r->second.tids[0] ).first ) < 0; } );
            }
            break;
        case 2:
            if( sortspec.SortDirection == ImGuiSortDirection_Descending )
            {
                pdqsort_branchless( psort.begin(), psort.end(), [] ( const auto& l, const auto& r ) { return l->second.data.runningTime > r->second.data.runningTime; } );
            }
            else
            {
                pdqsort_branchless( psort.begin(), psort.end(), [] ( const auto& l, const auto& r ) { return l->second.data.runningTime < r->second.data.runningTime; } );
            }
            break;
        case 3:
            if( sortspec.SortDirection == ImGuiSortDirection_Descending )
            {
                pdqsort_branchless( psort.begin(), psort.end(), [] ( const auto& l, const auto& r ) { return l->second.data.runningRegions > r->second.data.runningRegions; } );
            }
            else
            {
                pdqsort_branchless( psort.begin(), psort.end(), [] ( const auto& l, const auto& r ) { return l->second.data.runningRegions < r->second.data.runningRegions; } );
            }
            break;
        case 4:
            if( sortspec.SortDirection == ImGuiSortDirection_Descending )
            {
                pdqsort_branchless( psort.begin(), psort.end(), [] ( const auto& l, const auto& r ) { return l->second.data.migrations > r->second.data.migrations; } );
            }
            else
            {
                pdqsort_branchless( psort.begin(), psort.end(), [] ( const auto& l, const auto& r ) { return l->second.data.migrations < r->second.data.migrations; } );
            }
            break;
        default:
            assert( false );
            break;
        }

        const auto thisPid = m_worker.GetPid();
        const auto rtimespan = 1.0 / ( m_worker.GetLastTime() - m_worker.GetFirstTime() );
        const auto ty = ImGui::GetTextLineHeight();

        auto& style = ImGui::GetStyle();
        const auto framePaddingY = style.FramePadding.y;
        bool drawSeparator = false;
        for( auto& pidit : psort )
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if( drawSeparator ) ImGui::Separator();

            char buf[128];
            auto& pid = *pidit;
            const auto pidMatch = thisPid != 0 && thisPid == pid.first;
            auto name = m_worker.GetExternalName( pid.second.tids[0] ).first;
            if( pidMatch )
            {
                name = m_worker.GetCaptureProgram().c_str();
                ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.2f, 1.0f, 0.2f, 1.0f ) );
            }
            const auto pidtxt = pid.first == 0 ? "Unknown" : RealToString( pid.first );
            const auto expand = ImGui::TreeNode( pidtxt );
            if( ImGui::IsItemHovered() )
            {
                if( pidMatch )
                {
                    m_drawThreadMigrations = pid.first;
                    m_cpuDataThread = pid.first;
                }
                m_drawThreadHighlight = pid.first;
            }
            const auto tsz = pid.second.tids.size();
            if( tsz > 1 )
            {
                ImGui::SameLine();
                ImGui::TextDisabled( "(%s)", RealToString( tsz ) );
            }
            ImGui::TableNextColumn();
            if( drawSeparator ) ImGui::Separator();
            ImGui::TextUnformatted( pid.first == 0 ? "???" : name );
            if( ImGui::IsItemHovered() )
            {
                if( pidMatch )
                {
                    m_drawThreadMigrations = pid.first;
                    m_cpuDataThread = pid.first;
                }
                m_drawThreadHighlight = pid.first;
            }
            ImGui::TableNextColumn();
            if( drawSeparator ) ImGui::Separator();
            PrintStringPercent( buf, TimeToString( pid.second.data.runningTime ), double( pid.second.data.runningTime ) * rtimespan * 100 );
            style.FramePadding.y = 0;
            ImGui::ProgressBar( double( pid.second.data.runningTime ) * rtimespan, ImVec2( -1, ty ), buf );
            style.FramePadding.y = framePaddingY;
            ImGui::TableNextColumn();
            if( drawSeparator ) ImGui::Separator();
            ImGui::TextUnformatted( RealToString( pid.second.data.runningRegions ) );
            ImGui::TableNextColumn();
            if( drawSeparator )
            {
                drawSeparator = false;
                ImGui::Separator();
            }
            ImGui::TextUnformatted( RealToString( pid.second.data.migrations ) );
            ImGui::SameLine();
            PrintStringPercent( buf, double( pid.second.data.migrations ) / pid.second.data.runningRegions * 100 );
            TextDisabledUnformatted( buf );
            if( expand )
            {
                switch( sortspec.ColumnIndex )
                {
                case 0:
                    if( sortspec.SortDirection == ImGuiSortDirection_Descending )
                    {
                        pdqsort_branchless( pid.second.tids.begin(), pid.second.tids.end(), []( const auto& l, const auto& r ) { return l > r; } );
                    }
                    else
                    {
                        pdqsort_branchless( pid.second.tids.begin(), pid.second.tids.end() );
                    }
                    break;
                case 1:
                    if( sortspec.SortDirection == ImGuiSortDirection_Descending )
                    {
                        pdqsort_branchless( pid.second.tids.begin(), pid.second.tids.end(), [this] ( const auto& l, const auto& r ) { return strcmp( m_worker.GetExternalName( l ).second, m_worker.GetExternalName( r ).second ) > 0; } );
                    }
                    else
                    {
                        pdqsort_branchless( pid.second.tids.begin(), pid.second.tids.end(), [this] ( const auto& l, const auto& r ) { return strcmp( m_worker.GetExternalName( l ).second, m_worker.GetExternalName( r ).second ) < 0; } );
                    }
                    break;
                case 2:
                    if( sortspec.SortDirection == ImGuiSortDirection_Descending )
                    {
                        pdqsort_branchless( pid.second.tids.begin(), pid.second.tids.end(), [&ctd] ( const auto& l, const auto& r ) { return ctd.find( l )->second.runningTime > ctd.find( r )->second.runningTime; } );
                    }
                    else
                    {
                        pdqsort_branchless( pid.second.tids.begin(), pid.second.tids.end(), [&ctd] ( const auto& l, const auto& r ) { return ctd.find( l )->second.runningTime < ctd.find( r )->second.runningTime; } );
                    }
                    break;
                case 3:
                    if( sortspec.SortDirection == ImGuiSortDirection_Descending )
                    {
                        pdqsort_branchless( pid.second.tids.begin(), pid.second.tids.end(), [&ctd] ( const auto& l, const auto& r ) { return ctd.find( l )->second.runningRegions > ctd.find( r )->second.runningRegions; } );
                    }
                    else
                    {
                        pdqsort_branchless( pid.second.tids.begin(), pid.second.tids.end(), [&ctd] ( const auto& l, const auto& r ) { return ctd.find( l )->second.runningRegions < ctd.find( r )->second.runningRegions; } );
                    }
                    break;
                case 4:
                    if( sortspec.SortDirection == ImGuiSortDirection_Descending )
                    {
                        pdqsort_branchless( pid.second.tids.begin(), pid.second.tids.end(), [&ctd] ( const auto& l, const auto& r ) { return ctd.find( l )->second.migrations > ctd.find( r )->second.migrations; } );
                    }
                    else
                    {
                        pdqsort_branchless( pid.second.tids.begin(), pid.second.tids.end(), [&ctd] ( const auto& l, const auto& r ) { return ctd.find( l )->second.migrations < ctd.find( r )->second.migrations; } );
                    }
                    break;
                default:
                    assert( false );
                    break;
                }
                drawSeparator = true;
                for( auto& tid : pid.second.tids )
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if( drawSeparator ) ImGui::Separator();

                    const auto tidMatch = pidMatch && m_worker.IsThreadLocal( tid );
                    const char* tname;
                    if( tidMatch )
                    {
                        tname = m_worker.GetThreadName( tid );
                        ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 1.0f, 1.0f, 0.2f, 1.0f ) );
                    }
                    else
                    {
                        tname = m_worker.GetExternalName( tid ).second;
                    }
                    const auto& tit = ctd.find( tid );
                    assert( tit != ctd.end() );
                    ImGui::TextUnformatted( RealToString( tid ) );
                    if( ImGui::IsItemHovered() )
                    {
                        if( tidMatch )
                        {
                            m_drawThreadMigrations = tid;
                            m_cpuDataThread = tid;
                        }
                        m_drawThreadHighlight = tid;
                    }
                    ImGui::TableNextColumn();
                    if( drawSeparator ) ImGui::Separator();
                    if( tidMatch )
                    {
                        SmallColorBox( GetThreadColor( tid, 0 ) );
                        ImGui::SameLine();
                    }
                    ImGui::TextUnformatted( tname );
                    if( ImGui::IsItemHovered() )
                    {
                        if( tidMatch )
                        {
                            m_drawThreadMigrations = tid;
                            m_cpuDataThread = tid;
                        }
                        m_drawThreadHighlight = tid;
                    }
                    ImGui::TableNextColumn();
                    if( drawSeparator ) ImGui::Separator();
                    PrintStringPercent( buf, TimeToString( tit->second.runningTime ), double( tit->second.runningTime ) * rtimespan * 100 );
                    style.FramePadding.y = 0;
                    ImGui::ProgressBar( double( tit->second.runningTime ) * rtimespan, ImVec2( -1, ty ), buf );
                    style.FramePadding.y = framePaddingY;
                    ImGui::TableNextColumn();
                    if( drawSeparator ) ImGui::Separator();
                    ImGui::TextUnformatted( RealToString( tit->second.runningRegions ) );
                    ImGui::TableNextColumn();
                    if( drawSeparator )
                    {
                        drawSeparator = false;
                        ImGui::Separator();
                    }
                    ImGui::TextUnformatted( RealToString( tit->second.migrations ) );
                    ImGui::SameLine();
                    PrintStringPercent( buf, double( tit->second.migrations ) / tit->second.runningRegions * 100 );
                    TextDisabledUnformatted( buf );
                    if( tidMatch )
                    {
                        ImGui::PopStyleColor();
                    }
                }
                ImGui::TreePop();
                drawSeparator = true;
            }
            if( pidMatch )
            {
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    ImGui::End();
}


}
