#include "TracyTimelineContext.hpp"
#include "TracyTimelineDraw.hpp"
#include "TracyView.hpp"
#include "TracyImGui.hpp"
#include "TracyPrint.hpp"
#include "TracyColor.hpp"
#include "TracyMouse.hpp"

#include <algorithm>

namespace tracy
{

constexpr float MinCounterTxtSize = 15.0f;

constexpr float TxtModeTShapePaddingLeftRight = 2.0f;
constexpr float TxtModeTShapePaddingTopBottom = 1.0f;
constexpr float TxtModeTextMargin = 3.0f;
constexpr float TxtModeTextPadding = 1.0f;
constexpr float TxtModeReservedSize = 2.0f * ( TxtModeTextMargin + TxtModeTextPadding );

static uint32_t GetCounterColor( const HwCounterDrawItem &counter, float flTargetRate )
{
    if ( counter.rate > flTargetRate * 2.0f )
    {
        // always red
        return 0xFF0000FF;
    }
    else if ( counter.rate > flTargetRate )
    {
        // gradient from yellow to red

        // 'counter.rate / flTargetRate' currently in the range [1.0 , 2.0]
        const float ratio = ( counter.rate / flTargetRate ) - 1.0f;
        const int g = int( 255.0f * ( 1.0f - ratio ) );
        return 0xFF0000FF | ( g << 8 );
    }
    else
    {
        // always green
        return 0xFF22DD22;
    }
}

void View::DrawHwCounterList( const TimelineContext &ctx, const HwCounterDraw &drawList, int offset, int height, int endZoneOffset )
{    
    static char tmpStrBuf[ 1024 ];
    const char *hwCounterName = drawList.m_name.Active() ? m_worker.GetString( drawList.m_name ) : "L2 cache misses";
    
    auto draw = ImGui::GetWindowDrawList();
    const auto& wpos = ctx.wpos;
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );
    const auto w = ctx.w;
    const auto pxns = ctx.pxns;
    const auto ty = ctx.ty;
    const bool hover = ctx.hover;
    const float yBase = offset + height;
    const float yBase05 = offset + round( height * 0.5f );

    const bool bDrawRate = ( m_vd.hwCounterDrawMode == ViewData::EHwCounterDrawMode::BarGraph_Rate );
    const bool bTextMode = ( m_vd.hwCounterDrawMode == ViewData::EHwCounterDrawMode::TextMode );

    const uint32_t txtColor   = IM_COL32( 197, 200, 198, 255 );
    const uint32_t zigzagColor = IM_COL32( 197, 200, 198, 102 );
    const uint32_t overlayColor = 0x2DFF8888;

    uint64_t maxCount = drawList.m_maxCount;
    float maxRate = drawList.m_maxRate;
    if ( m_vd.hwCounterYAxisSameScale )
    {
        maxCount = m_hwCounterMaxCount;
        maxRate = m_hwCounterMaxRate;
    }
    maxCount = std::max( maxCount, 1ull );
    maxRate = std::max( maxRate, 1.0f );
    float revrange = 1.0 / ( bDrawRate ? maxRate : maxCount );

    char hwCounterStr[ 128 ];
    int zigzagStart = -1;
    int zigzagEnd = -1;

    for ( auto &counterDraw : drawList.m_items )
    {
        double xStart = ( counterDraw.tstart.Val() - m_vd.zvStart ) * pxns;
        double xEnd = ( counterDraw.tend.Val() - m_vd.zvStart ) * pxns;

        const uint32_t color = GetCounterColor( counterDraw, (float)m_vd.hwCounterRateTarget );

        if ( !bTextMode )
        {
            const uint32_t borderColor = DarkenColor( color );
            
            float barHeight = ( bDrawRate ? counterDraw.rate : counterDraw.count );
            barHeight *= revrange * height;

            // Snap to pixels
            barHeight = floor( barHeight );
            xStart = floor( xStart );
            xEnd = floor( xEnd );

            if ( barHeight > 2.0f )
            {
                draw->AddRectFilled( wpos + ImVec2( xStart, yBase ), wpos + ImVec2( xEnd, yBase - barHeight ), color );
                draw->AddRect( wpos + ImVec2( xStart, yBase ), wpos + ImVec2( xEnd, yBase - barHeight ), borderColor );
            }
        }
        else
        {
            double width = xEnd - xStart;

            if ( width < MinCounterTxtSize )
            {
                if ( ( zigzagStart != -1 ) && ( ( xStart - zigzagEnd ) > MinCounterTxtSize ) )
                {
                    DrawZigZag( draw, wpos + ImVec2( 0, yBase05 ), zigzagStart, zigzagEnd, height * 0.25, zigzagColor );
                    zigzagStart = -1;
                }

                zigzagEnd = xEnd;
                if ( zigzagStart == -1 )
                {
                    zigzagStart = xStart;
                }                
                continue;
            }

            if ( ( zigzagStart != -1 ) )
            {
                DrawZigZag( draw, wpos + ImVec2( 0, yBase05 ), zigzagStart, zigzagEnd, height * 0.25, zigzagColor );
                zigzagStart = -1;
            }
            
            sprintf( hwCounterStr, "%s %s (%.1f %s/\xce\xbcs)", RealToString( counterDraw.count ), hwCounterName, counterDraw.rate, hwCounterName );
            float tx = ImGui::CalcTextSize( hwCounterStr ).x;
            if ( width - TxtModeReservedSize <= tx )
            {
                sprintf( hwCounterStr, "%s (%.1f/\xce\xbcs)", RealToString( counterDraw.count ), counterDraw.rate );
                tx = ImGui::CalcTextSize( hwCounterStr ).x;
            }
            if ( width - TxtModeReservedSize <= tx )
            {
                sprintf( hwCounterStr, "%lld (%.1f)", counterDraw.count, counterDraw.rate );
                tx = ImGui::CalcTextSize( hwCounterStr ).x;
            }
            
            if ( counterDraw.tstart.Val() >= m_vd.zvStart )
            {
                DrawLine( draw, dpos + ImVec2( xStart + TxtModeTShapePaddingLeftRight, offset + TxtModeTShapePaddingTopBottom ), dpos + ImVec2( xStart + TxtModeTShapePaddingLeftRight, yBase - TxtModeTShapePaddingTopBottom ), color );
            }
            if ( counterDraw.tend.Val() <= m_vd.zvEnd )
            {
                DrawLine( draw, dpos + ImVec2( xEnd - TxtModeTShapePaddingLeftRight, offset + TxtModeTShapePaddingTopBottom ), dpos + ImVec2( xEnd - TxtModeTShapePaddingLeftRight, yBase - TxtModeTShapePaddingTopBottom ), color );
            }

            if ( width - TxtModeReservedSize > tx )
            {
                const auto x0 = xStart + TxtModeTextMargin;
                const auto x1 = xEnd - TxtModeTextMargin;
                const auto te = x1 - tx;

                auto tpos = ( x0 + te ) / 2;
                if ( tpos < 0 )
                {
                    tpos = std::min( std::min( 0., te - tpos ), te );
                }
                else if ( tpos > w - tx )
                {
                    tpos = std::max( double( w - tx ), x0 );
                }
                tpos = round( tpos );

                DrawLine( draw, dpos + ImVec2( xStart + TxtModeTShapePaddingLeftRight, yBase05 ), dpos + ImVec2( tpos - TxtModeTextPadding, yBase05 ), color );
                DrawLine( draw, dpos + ImVec2( tpos + tx + TxtModeTextPadding, yBase05 ), dpos + ImVec2( xEnd - TxtModeTShapePaddingLeftRight, yBase05 ), color );
                draw->AddText( wpos + ImVec2( tpos, offset ), txtColor, hwCounterStr );
            }
            else
            {
                DrawLine( draw, dpos + ImVec2( xStart + TxtModeTShapePaddingLeftRight, yBase05 ), dpos + ImVec2( xEnd - TxtModeTShapePaddingLeftRight, yBase05 ), color );
            }
        }

        if ( hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( xStart, offset ), wpos + ImVec2( xEnd, yBase ) ) )
        {
            // Middle mouse button to zoom
            if ( !m_zoomAnim.active && IsMouseClicked( ImGuiMouseButton_Middle ) )
            {
                ZoomToRange( counterDraw.tstart.Val(), counterDraw.tend.Val() );
            }
            
            // Draw overlay to highlight the region where hw counters were recorded.
            draw->AddRectFilled( wpos + ImVec2( xStart, offset ), wpos + ImVec2( xEnd, endZoneOffset ), overlayColor );
            //DrawStripedRect( draw, wpos, xStart, wpos.y + offset, xEnd, wpos.y + endZoneOffset, 5 * GetScale(), 0x55DD8888, true, false );
            DrawLine( draw, dpos + ImVec2( xStart, offset ), dpos + ImVec2( xStart, endZoneOffset ), IM_COL32_WHITE );
            DrawLine( draw, dpos + ImVec2( xEnd, offset ), dpos + ImVec2( xEnd, endZoneOffset ), IM_COL32_WHITE );
            
            // Tooltip
            ImGui::BeginTooltip();
            sprintf( tmpStrBuf, "%s:", hwCounterName );
            TextFocused( tmpStrBuf, RealToString( counterDraw.count ) );
            sprintf( tmpStrBuf, "%s/\xce\xbcs:", hwCounterName );
            TextFocused( tmpStrBuf, RealToString( counterDraw.rate ) );
            ImGui::Separator();
            TextFocused( "Start time:", TimeToStringExact( counterDraw.tstart.Val() ) );
            TextFocused( "End time:", TimeToStringExact( counterDraw.tend.Val() ) );
            TextFocused( "Range duration:", TimeToString( counterDraw.tend.Val() - counterDraw.tstart.Val() ) );
            if ( drawList.m_description.Active() )
            {
                ImGui::Separator();
                ImGui::PushTextWrapPos( 450.0f * ty / 15.f );
                TextDisabledUnformatted( m_worker.GetString( drawList.m_description ) );
                ImGui::PopTextWrapPos();
            }
            ImGui::EndTooltip();
        }
    }

    if ( !bTextMode )
    {
        if ( bDrawRate )
        {
            sprintf( tmpStrBuf, "Max %s/\xce\xbcs: %s", hwCounterName, RealToString( maxRate ) );
        }
        else
        {
            sprintf( tmpStrBuf, "Max %s: %s", hwCounterName, RealToString( maxCount ) );
        }
        DrawTextSuperContrast( draw, wpos + ImVec2( 5, offset ), txtColor, tmpStrBuf );
        DrawTextSuperContrast( draw, wpos + ImVec2( 5, yBase - ty ), txtColor, "0" );

        DrawLine( draw, dpos + ImVec2( 0, yBase ), dpos + ImVec2( w, yBase ), IM_COL32( 128, 128, 128, 255 ) );
    }
    else
    {
        if ( ( zigzagStart != -1 ) )
        {
            DrawZigZag( draw, wpos + ImVec2( 0, yBase05 ), zigzagStart, zigzagEnd, height * 0.25, zigzagColor );
            zigzagStart = -1;
        }
    }

    // Left click to cycle through draw mode
    if ( hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( 0, offset ), wpos + ImVec2( w, yBase ) ) )
    {
        if ( IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            m_vd.hwCounterDrawMode = ( ViewData::EHwCounterDrawMode ) ( ( ( int ) m_vd.hwCounterDrawMode + 1 ) % ( int ) ViewData::EHwCounterDrawMode::DrawModeCount );
        }
    }
}

}