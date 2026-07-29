#include <algorithm>

#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyTimelineItem.hpp"
#include "TracyView.hpp"

namespace tracy
{

TimelineItem::TimelineItem( View& view, Worker& worker, const void* key, bool wantPreprocess )
    : m_visible( true )
    , m_showFull( true )
    , m_height( 0 )
    , m_wantPreprocess( wantPreprocess )
    , m_key( key )
    , m_view( view )
    , m_worker( worker )
{
}

void TimelineItem::Draw( bool firstFrame, const TimelineContext& ctx, int yOffset )
{
    const auto yBegin = yOffset;
    auto yEnd = yOffset;

    if( !IsVisible() )
    {
        DrawFinished();
        if( m_height != 0 ) AdjustThreadHeight( firstFrame, yBegin, yEnd );
        return;
    }
    if( IsEmpty() )
    {
        DrawFinished();
        return;
    }

    const auto label = HeaderLabel();
    ImVec2 labelSize = ImGui::CalcTextSize( label );
    labelSize.y += ImGui::GetStyle().FramePadding.y * 2.0f;

    const auto w = ctx.w;
    const auto ty = ctx.ty;
    const auto ostep = labelSize.y + 1;
    const auto& wpos = ctx.wpos;
    const auto yPos = wpos.y + yBegin;
    const auto dpos = wpos + ImVec2( 0.5f, 0.5f );
    auto draw = ImGui::GetWindowDrawList();

    ImGui::PushID( this );
    ImGui::PushClipRect( wpos + ImVec2( 0, yBegin ), wpos + ImVec2( w, yBegin + m_height ), true );

    yEnd += ostep;
    if( m_showFull )
    {
        if( !DrawContents( ctx, yEnd ) && !m_view.GetViewData().drawEmptyLabels )
        {
            DrawFinished();
            yEnd = yBegin;
            AdjustThreadHeight( firstFrame, yBegin, yEnd );
            ImGui::PopClipRect();
            ImGui::PopID();
            return;
        }
    }

    DrawOverlay( wpos + ImVec2( 0, yBegin ), wpos + ImVec2( w, yEnd ) );
    ImGui::PopClipRect();

    float xOffset = 0.0f;
    const float ItemSpacing = ImGui::GetStyle().ItemSpacing.x;
    const auto hdrOffset = yBegin;
    const bool drawHeader = yPos + labelSize.y >= ctx.yMin && yPos <= ctx.yMax;

    const ViewData& vd = m_view.GetViewData();
    if ( vd.darkenOutsideExport )
    {
        // NOTE: if we have an export range, darken the areas that are outside said range
        int64_t exportStart = 0;
        int64_t exportEnd = 0;
        if ( m_worker.GetExportRange( exportStart, exportEnd ) )
        {
            const int64_t beforeStart = std::min( exportStart, vd.zvStart );
            const int64_t beforeEnd = std::max( exportStart, vd.zvStart );

            const ImVec2 beforeMin( ctx.wpos.x + ( beforeStart - vd.zvStart ) * ctx.pxns, ctx.wpos.y + yBegin );
            const ImVec2 beforeMax( ctx.wpos.x + ( beforeEnd - vd.zvStart ) * ctx.pxns, ctx.wpos.y + yEnd );

            const int64_t afterStart = std::min( exportEnd, vd.zvEnd );
            const int64_t afterEnd = std::max( exportEnd, vd.zvEnd );

            const ImVec2 afterMin( ctx.wpos.x + ( afterStart - vd.zvStart ) * ctx.pxns, ctx.wpos.y + yBegin );
            const ImVec2 afterMax( ctx.wpos.x + ( afterEnd - vd.zvStart ) * ctx.pxns, ctx.wpos.y + yEnd );

            auto draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled( beforeMin, beforeMax, 0x55000000 );
            draw->AddRectFilled( afterMin, afterMax, 0x55000000 );

            if ( ImGui::IsMouseHoveringRect( beforeMin, beforeMax ) || ImGui::IsMouseHoveringRect( afterMin, afterMax ) )
            {
                if ( ImGui::BeginTooltip() )
                {
                    ImGui::Text( "Area ouside the exported range.\nZones may be incomplete!" );
                    ImGui::EndTooltip();
                }
            }
        }
    }

    if( drawHeader )
    {
        const auto color = HeaderColor();
        const auto colorInactive = HeaderColorInactive();

        if( m_showFull )
        {
            DrawTextContrast( draw, wpos + ImVec2( 0, hdrOffset ), color, ICON_FA_CARET_DOWN );
        }
        else
        {
            DrawTextContrast( draw, wpos + ImVec2( 0, hdrOffset ), colorInactive, ICON_FA_CARET_RIGHT );
        }
        xOffset = ty;
        HeaderLabelPrefix( ctx, hdrOffset, xOffset );
        DrawTextContrast( draw, wpos + ImVec2( xOffset, hdrOffset ), m_showFull ? color : colorInactive, label );
        xOffset += labelSize.x + ItemSpacing;

        if( ctx.hover && ImGui::IsMouseHoveringRect( wpos + ImVec2( 0, hdrOffset ), wpos + ImVec2( xOffset, hdrOffset + labelSize.y ) ) )
        {
            HeaderTooltip( label );

            if( IsMouseClicked( 0 ) )
            {
                m_showFull = !m_showFull;
            }
            if( IsMouseClicked( 2 ) )
            {
                const auto t0 = RangeBegin();
                const auto t1 = RangeEnd();
                if( t0 < t1 )
                {
                    m_view.ZoomToRange( t0, t1 );
                }
            }
            if( IsMouseClicked( 1 ) )
            {
                ImGui::OpenPopup( "menuPopup" );
            }
        }

        if ( m_showFull )
        {
            DrawLine( draw, dpos + ImVec2( 0, hdrOffset + labelSize.y - 1 ), dpos + ImVec2( w, hdrOffset + labelSize.y - 1 ), HeaderLineColor() );
            HeaderExtraContents( ctx, hdrOffset, xOffset );
        }

    }

    if( ImGui::BeginPopup( "menuPopup" ) )
    {
        if( ImGui::MenuItem( ICON_FA_EYE_SLASH " Hide" ) )
        {
            SetVisible( false );
            ImGui::CloseCurrentPopup();
        }
        DrawExtraPopupItems();
        ImGui::EndPopup();
    }

    yEnd += 0.2f * ostep;
    DrawUiControls( ctx, yBegin, yEnd, xOffset );
    AdjustThreadHeight( firstFrame, yBegin, yEnd );
    DrawFinished();

    ImGui::PopID();
}

void TimelineItem::AdjustThreadHeight( bool firstFrame, int yBegin, int yEnd )
{
    const bool instant = PreventScrolling();
    const auto speed = 4.0;
    const auto baseMove = 1.0;

    const auto newHeight = yEnd - yBegin;
    if( firstFrame )
    {
        m_height = newHeight;
    }
    else if( m_height != newHeight )
    {
        const auto diff = newHeight - m_height;
        const auto preClampMove = diff * ( instant ? 1.0f : speed * ImGui::GetIO().DeltaTime );
        if( diff > 0 )
        {
            const auto move = preClampMove + baseMove;
            m_height = int( std::min<double>( m_height + move, newHeight ) );
        }
        else
        {
            const auto move = preClampMove - baseMove;
            m_height = int( std::max<double>( m_height + move, newHeight ) );
        }
        s_wasActive = true;
    }
}

bool TimelineItem::VisibilityCheckbox()
{
    return SmallCheckbox( HeaderLabel(), &m_visible );
}

}
