#include "TracyImGui.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyTimelineItemPlot.hpp"
#include "TracyUtility.hpp"
#include "TracyView.hpp"
#include "TracyWorker.hpp"
#include "tracy_pdqsort.h"

namespace tracy
{

constexpr int PlotHeightPx = 100;
constexpr int MinVisSize = 3;

static uint32_t s_plotColors[] = { 0xFF44FF44, 0xFFfc3f8a, 0xFFffb133, 0xFF797d00, 0xFFb67eff, 0xFF564dfa, 0xfff1f1ff, 0xFF6cdc4f, 0xFFff5925, 0xFF7127d1, 0xFF06a1d2, 0xFFbabd08, 0xFFffe6ba, 0xFF004eba, 0xFFffbbd4 };
static int s_numPlotColors = ( int ) sizeof( s_plotColors ) / sizeof( s_plotColors[ 0 ] );

TimelineItemPlot::TimelineItemPlot( View& view, Worker& worker, PlotData* plot )
    : TimelineItem( view, worker, plot, true )
    , m_plot( plot )
{
    SetVisible( m_plot->type == PlotType::Zone );

    const float defaultHeight = 100.0f;
    m_resizeBar.id = 0;
    m_resizeBar.wasActive = false;
    m_resizeBar.thickness = 8.0f;
    m_resizeBar.defaultHeight = defaultHeight;
    m_resizeBar.minHeight = 30.0f;
    m_resizeBar.maxHeight = 500.0f;
    m_resizeBar.height = defaultHeight;
    m_resizeBar.color = ImVec4( 0.5f, 0.5f, 0.5f, 1.0f );
    m_resizeBar.colActive = ImVec4( 0.9f, 1.0f, 0.9f, 1.0f );
    m_resizeBar.colHover = ImVec4( 0.8f, 0.8f, 0.8f, 1.0f );
}

bool TimelineItemPlot::IsEmpty() const
{
    for ( PlotData *pd = m_plot; pd != nullptr; pd = pd->nextPlot )
    {
        if ( !pd->data.empty() )
            return false;
    }
    return true;
}

void TimelineItemPlot::HeaderLabelPrefix( const TimelineContext &ctx, int yOffset, float &xOffset )
{
    auto draw = ImGui::GetWindowDrawList();
    const auto ty = ImGui::GetTextLineHeight();
    float yPos = 0.0f;
    float rounding = 0.17f * ( ty - 1 );

    if ( m_plot->nextPlot != nullptr )
    {
        for ( PlotData *plot = m_plot; plot != nullptr; plot = plot->nextPlot )
        {
            draw->AddRectFilled( ctx.wpos + ImVec2( xOffset + 1.0f, yOffset + yPos + 1.0f ), ctx.wpos + ImVec2( xOffset + ty - 1.0f, yOffset + yPos + ty - 1.0f ), GetPlotColor( *plot, m_worker ), rounding );
            yPos += ty;
        }
        xOffset += ty + 4;
    }
}

const char* TimelineItemPlot::HeaderLabel() const
{
    static char tmp[1024];
    tmp[ 0 ] = 0;
    for ( PlotData *plot = m_plot; plot != nullptr; plot = plot->nextPlot )
    {
        if ( tmp[ 0 ] ) strcat( tmp, "\n" );
        switch ( plot->type )
        {
            case PlotType::User:
            case PlotType::Zone:
                strcat( tmp, m_worker.GetString( plot->name ) );
                break;
            case PlotType::Memory:
                if ( !plot->name.active  )
                {
                    return ICON_FA_MEMORY " Memory usage";
                }
                else
                {
                    sprintf( tmp, ICON_FA_MEMORY " %s", m_worker.GetString( plot->name ) );
                    return tmp;
                }
            case PlotType::SysTime:
                return ICON_FA_GAUGE_HIGH " CPU usage";
            case PlotType::Power:
                sprintf( tmp, ICON_FA_BOLT " %s", m_worker.GetString( plot->name ) );
                return tmp;
            default:
                assert( false );
                return nullptr;
        }
    }
    return tmp;
}

void TimelineItemPlot::HeaderTooltip( const char* label ) const
{
    ImGui::BeginTooltip();

    char *pLabel = strdup( label );
    char *pTokenInit = pLabel;
    char *pToken = nullptr;
    for ( PlotData *plot = m_plot; plot != nullptr; plot = plot->nextPlot )
    {
        SmallColorBox( GetPlotColor( *plot, m_worker ) );
        ImGui::SameLine();
        pToken = strtok( pTokenInit, "\n" );
        pTokenInit = nullptr;
        TextFocused( "Plot", pToken ? pToken : label );
        ImGui::Separator();

        const auto first = RangeBegin();
        const auto last = RangeEnd();
        const auto activity = last - first;
        const auto traceLen = m_worker.GetLastTime() - m_worker.GetFirstTime();

        TextFocused( "Appeared at", TimeToString( first ) );
        TextFocused( "Last event at", TimeToString( last ) );
        TextFocused( "Activity time:", TimeToString( activity ) );
        ImGui::SameLine();
        char buf[64];
        PrintStringPercent( buf, activity / double( traceLen ) * 100 );
        TextDisabledUnformatted( buf );
        ImGui::Separator();
        TextFocused( "Data points:", RealToString( plot->data.size() ) );
        TextFocused( "Data range:", FormatPlotValue( plot->max - plot->min, plot->format ) );
        TextFocused( "Min value:", FormatPlotValue( plot->min, plot->format ) );
        TextFocused( "Max value:", FormatPlotValue( plot->max, plot->format ) );
        TextFocused( "Avg value:", FormatPlotValue( plot->sum / plot->data.size(), plot->format ) );
        TextFocused( "Data/second:", RealToString( double( plot->data.size() ) / activity * 1000000000ll ) );

        const auto it = std::lower_bound( plot->data.begin(), plot->data.end(), last - 1000000000ll * 10, [] ( const auto &l, const auto &r ) { return l.time.Val() < r; } );
        const auto tr10 = last - it->time.Val();
        if( tr10 != 0 )
        {
            TextFocused( "D/s (10s):", RealToString( double( std::distance( it, plot->data.end() ) ) / tr10 * 1000000000ll ) );
        }
    }
    free(pLabel);
    ImGui::EndTooltip();
}

void TimelineItemPlot::HeaderExtraContents( const TimelineContext& ctx, int offset, float &xOffset )
{
    auto draw = ImGui::GetWindowDrawList();
    const float ItemSpacing = ImGui::GetStyle().ItemSpacing.x;

    char tmp[1024];
    tmp[ 0 ] = 0;
    size_t tmpSize = sizeof( tmp );
    for ( PlotData *plot = m_plot; plot != nullptr; plot = plot->nextPlot )
    {
        if( tmp[0] ) strncat( tmp, "\n", tmpSize );
        strncat( tmp, "( y - range: ", tmpSize );
        strncat( tmp, FormatPlotValue( plot->rMax - plot->rMin, plot->format ), tmpSize );
        strncat( tmp, ", visible data points: ", tmpSize );
        strncat( tmp, RealToString( plot->num ), tmpSize );
        strncat( tmp, ")", tmpSize );
    }
    tmp[ tmpSize - 1 ] = 0;
    draw->AddText( ctx.wpos + ImVec2( xOffset, offset ), 0xFF226E6E, tmp );
    xOffset += ImGui::CalcTextSize( tmp ).x + ItemSpacing;
}

int64_t TimelineItemPlot::RangeBegin() const
{
    int64_t retVal = m_plot->data.front().time.Val();

    for ( PlotData *plot = m_plot->nextPlot; plot != nullptr; plot = plot->nextPlot )
    {
        retVal = std::min( retVal, plot->data.front().time.Val() );
    }
    return retVal;
}

int64_t TimelineItemPlot::RangeEnd() const
{
    int64_t retVal = m_plot->data.back().time.Val();
    for ( PlotData *plot = m_plot->nextPlot; plot != nullptr; plot = plot->nextPlot )
    {
         retVal = std::max( retVal, plot->data.back().time.Val() );
    }
    return retVal;
}

bool TimelineItemPlot::DrawContents( const TimelineContext& ctx, int& offset )
{
    uint32_t flags = View::DrawPlotFlags::AddBackground;
    if ( m_plot->nextPlot != nullptr )
    {
        int initialOffset = offset;
        flags |= View::DrawPlotFlags::MultiPlot;
        int nPlot = 0;

        for ( PlotData *plot = m_plot; plot != nullptr; plot = plot->nextPlot, nPlot++ )
        {
            offset = initialOffset;
            m_view.DrawPlot( ctx, *plot, m_draw, m_plotLines[ nPlot ].m_begin, m_plotLines[ nPlot ].m_end, offset, m_rightEnd, m_bUseFixedMax ? m_userMax : m_max, flags, m_resizeBar.height );
            flags &= ~View::DrawPlotFlags::AddBackground;
        }
    }
    else
    {
        m_view.DrawPlot( ctx, *m_plot, m_draw, m_plotLines[ 0 ].m_begin, m_plotLines[ 0 ].m_end, offset, m_rightEnd, m_bUseFixedMax ? m_userMax : m_max, flags, m_resizeBar.height );
    }
    return true;
}


bool TimelineItemPlot::PreventScrolling() const
{
    return m_resizeBar.wasActive;
};

void TimelineItemPlot::DrawUiControls( const TimelineContext &ctx, int start, int &offset, float xOffset )
{
    const uint8_t uiControlLoc = m_view.GetViewData().uiControlLoc;
    if ( uiControlLoc == ViewData::UiCtrlLocHidden )
    {
        return;
    }

    const char *controlPopupName = "PlotUiControlsPopup";
    ImVec2 startPos( xOffset, start);
    ImGui::SetCursorPos( startPos );
    const ImVec2 wndContentRegionMax = ImGui::GetWindowContentRegionMax();
    bool renderControls = false;
    bool isPopup = false;
    bool closePopup = true;

    if ( uiControlLoc == ViewData::UiCtrlLocRight )
    {
        float framePaddingWidth = ImGui::GetStyle().FramePadding.x;
        const float ctrlButtonWidth = ImGui::CalcTextSize( ICON_FA_BARS ).x + framePaddingWidth * 2.0f;
        ImGui::SetCursorPosX( wndContentRegionMax.x - ctrlButtonWidth );
    }

    if ( ImGui::SmallButton( ICON_FA_BARS ) )
    {
        ImGui::OpenPopup( controlPopupName );
    }

    if ( ImGui::BeginPopup( controlPopupName ) )
    {
        isPopup = true;
        renderControls = true;
    }

    if ( renderControls )
    {
        if ( m_plot->type == PlotType::Zone )
        {
            if ( !isPopup )
                ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );

            if ( ImGui::Button( ICON_FA_SKULL_CROSSBONES " Delete" ) )
            {
                ImGui::OpenPopup( "Confirm Delete Plot" );
            }

            if ( !isPopup )
                ImGui::PopStyleVar(); //ImGuiStyleVar_FramePadding

            if ( ImGui::BeginPopupModal( "Confirm Delete Plot", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            {
                bool bClosePopups = false;
                ImGui::Text( HeaderLabel() );
                ImGui::Separator();
                ImGui::Text( "Are you sure you want to delete this plot?" );
                if ( ImGui::Button( "Yes" ) )
                {
                    for ( PlotData *plot = m_plot; plot != nullptr; plot = plot->nextPlot )
                    {
                        m_worker.QueuePlotForDelete( plot );
                    }
                    bClosePopups = true;
                }
                ImGui::SameLine();
                if ( ImGui::Button( "No" ) )
                {
                    bClosePopups = true;
                }
                if ( bClosePopups )
                    ImGui::CloseCurrentPopup(); // Confirm delete popup

                ImGui::EndPopup();

                if ( bClosePopups )
                    ImGui::CloseCurrentPopup(); // Parent popup
            }

            isPopup ? ImGui::Separator() : ImGui::SameLine();
        }

        bool isTable = false;
        const ImVec4 border_color = ImVec4( 1.0f, 1.0f, 1.0f, 0.8f );
        if ( !isPopup )
        {
            ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
            if ( m_plot->type == PlotType::Zone )
            {
                isTable = true; // Use ImGui tables to distinguish groups of buttons in expanded view
                ImGui::PushStyleColor( ImGuiCol_TableBorderLight, border_color );
                ImGui::PushStyleColor( ImGuiCol_TableBorderStrong, border_color );
                ImGui::BeginTable( "table_yaxis", 1, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PreciseWidths | ImGuiTableFlags_NoHostExtendX );
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
            }
        }

        ImGui::Text( ICON_FA_RULER_VERTICAL " Y axis Max:" );
        isPopup ? ImGui::Indent() : ImGui::SameLine();

        bool bRadioChanged = ImGui::RadioButton( "Auto", &m_bUseFixedMax, 0 );
        if ( !isPopup ) ImGui::SameLine();
        bRadioChanged = ImGui::RadioButton( "Fixed Value", &m_bUseFixedMax, 1 ) || bRadioChanged;

        if ( !m_bUseFixedMax )
        {
            ImGui::BeginDisabled(); // Disable instead of skipping rendering, otherwise if right-aligned the popup position doesn't adjust properly when the content width changes.
        }
        ImGui::SameLine();
        if ( m_userMax < 0.0 ) m_userMax = m_max;
        const auto scale = GetScale();
        ImGui::SetNextItemWidth( 90 * scale );
        float fUserMax = ( float ) m_userMax;
        if( ImGui::SliderFloat( "##FixedInput", &fUserMax, 0.0f, ( float ) m_max, "%.3f" ) )     // If v_min >= v_max we have no bound
            m_userMax = ( double ) fUserMax;
        if ( !m_bUseFixedMax )
        {
            ImGui::EndDisabled();
        }

        if ( isPopup )
        {
            ImGui::Unindent();
            ImGui::Separator();
        }

        if ( m_plot->type == PlotType::Zone )
        {
            if ( isTable )
            {
                ImGui::EndTable();
                ImGui::SameLine();
                ImGui::BeginTable( "table_drawtype", 1, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PreciseWidths | ImGuiTableFlags_NoHostExtendX );
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
            }
            int drawType = ( int ) m_plot->drawType;
            ImGui::RadioButton( ICON_FA_CHART_LINE, &drawType, ( int ) PlotDrawType::Line );
            ImGui::SameLine();
            ImGui::RadioButton( ICON_FA_STAIRS, &drawType, ( int ) PlotDrawType::Step );
            ImGui::SameLine();
            ImGui::RadioButton( ICON_FA_CHART_COLUMN, &drawType, ( int ) PlotDrawType::Bar );
            for ( PlotData *plot = m_plot; plot != nullptr; plot = plot->nextPlot )
            {
                plot->drawType = ( PlotDrawType ) drawType;
            }
            if ( isTable )
            {
                ImGui::EndTable();
                ImGui::PopStyleColor(2);
            }
        }

        if( !isPopup )
            ImGui::PopStyleVar(); //ImGuiStyleVar_FramePadding
    }

    if ( isPopup )
    {
        ImGui::EndPopup();
    }

    ImGui::SetCursorPosY( offset );
    if ( m_resizeBar.height == 0.0f )
    {
        m_resizeBar.height = GetHeight();
    }

    float diff = UpdateAndDrawResizeBar( m_resizeBar );
    offset += ((int)diff + m_resizeBar.uiHeight );

    ImGui::SetCursorPosY( offset );
}

void TimelineItemPlot::DrawFinished()
{
    m_draw.clear();
    m_plotLines.clear();
}

void TimelineItemPlot::Preprocess( const TimelineContext& ctx, TaskDispatch& td, bool visible, int yPos )
{
    assert( m_draw.empty() );
    assert( m_plotLines.empty() );

    for ( PlotData *p = m_plot; p != nullptr; p = p->nextPlot )
    {
        m_plotLines.push_next();
    }

    if( !visible ) return;
    if( yPos > ctx.yMax ) return;
    if( IsEmpty() ) return;
    const auto PlotHeight = int( round( PlotHeightPx * GetScale() ) );
    if( yPos + PlotHeight < ctx.yMin ) return;

    td.Queue( [this, &ctx] {
        const auto vStart = ctx.vStart;
        const auto vEnd = ctx.vEnd;
        const auto nspx = ctx.nspx;
        const auto MinVisNs = int64_t( round( MinVisSize * nspx ) );
        m_max = 0;

        int nPlot = 0;
        for ( PlotData *p = m_plot; p != nullptr; p = p->nextPlot, ++nPlot )
        {
            auto &vec = p->data;
            auto &line = m_plotLines[nPlot];
            p->color = s_plotColors[ std::min( s_numPlotColors-1, nPlot ) ];

            vec.ensure_sorted();
            if( vec.front().time.Val() > vEnd )
            {
                p->rMin = 0;
                p->rMax = 0;
                p->num = 0;
                m_rightEnd = false;
                continue;
            }
            else if( vec.back().time.Val() < vStart )
            {
                const auto lastTime = m_worker.GetLastTime();
                const auto val = vec.back().val;
                p->rMin = val - 1;
                p->rMax = val + 1;
                p->num = lastTime < vStart ? 0 : 1;
                m_rightEnd = vec.back().time.Val() < lastTime;
                continue;
            }

            auto it = std::lower_bound( vec.begin(), vec.end(), vStart, [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );
            auto end = std::lower_bound( it, vec.end(), vEnd, [] ( const auto& l, const auto& r ) { return l.time.Val() < r; } );

            m_rightEnd = end == vec.end() && vec.back().time.Val() < m_worker.GetLastTime();

            if( end != vec.end() ) end++;
            if( it != vec.begin() ) it--;

            double min = it->val;
            double max = it->val;
            const auto num = end - it;
            if( num > 1000000 )
            {
                    min = p->min;
                    max = p->max;
            }
            else
            {
                auto tmp = it;
                while( ++tmp < end )
                {
                    if( tmp->val < min ) min = tmp->val;
                    else if( tmp->val > max ) max = tmp->val;
                }
            }

            m_max = nPlot == 0 ? max : std::max( max, m_max );

            if( min == max )
            {
                if ( p->nextPlot == nullptr && m_max == max )
                {
                    m_max++; // Only adjust overall maximum on the last plot of a multi-line plot. 
                }
                min--;
                max++;
            }

            p->rMin = min;
            p->rMax = max;
            p->num = num;

            line.m_begin = m_draw.size();
            m_draw.emplace_back( 0 );
            m_draw.emplace_back( it - vec.begin() );

            ++it;
            while( it < end )
            {
                auto next = std::upper_bound( it, end, int64_t( it->time.Val() + MinVisNs ), [] ( const auto& l, const auto& r ) { return l < r.time.Val(); } );
                assert( next > it );
                const auto rsz = uint32_t( next - it );
                if( rsz < 4 )
                {
                    for ( uint32_t i = 0; i < rsz; i++ )
                    {
                        m_draw.emplace_back( 0 );
                        m_draw.emplace_back( it - vec.begin() );
                        ++it;
                    }
                }
                else
                {
                    // Sync with View::DrawPlot()!
                    constexpr int NumSamples = 256;
                    uint32_t samples[NumSamples];
                    uint32_t cnt = 0;
                    uint32_t offset = it - vec.begin();
                    if( rsz < NumSamples )
                    {
                        for( cnt=0; cnt<rsz; cnt++ )
                        {
                            samples[cnt] = offset + cnt;
                        }
                    }
                    else
                    {
                        const auto skip = ( rsz + NumSamples - 1 ) / NumSamples;
                        const auto limit = rsz / skip;
                        for( cnt=0; cnt<limit; cnt++ )
                        {
                            samples[cnt] = offset + cnt * skip;
                        }
                        if( cnt == limit ) cnt--;
                        samples[cnt++] = offset + rsz - 1;
                    }
                    it = next;

                    pdqsort_branchless( samples, samples+cnt, [&vec] ( const auto& l, const auto& r ) { return vec[l].val < vec[r].val; } );

                    assert( rsz > 0 );
                    m_draw.emplace_back( rsz );
                    m_draw.emplace_back( offset );
                    m_draw.emplace_back( samples[0] );
                    m_draw.emplace_back( samples[cnt-1] );
                }
            }
            line.m_end = m_draw.size();
        }
    } );
}

}
