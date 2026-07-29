#ifndef __TRACYTIMELINEITEMPLOT_HPP__
#define __TRACYTIMELINEITEMPLOT_HPP__

#include "TracyEvent.hpp"
#include "TracyTimelineDraw.hpp"
#include "TracyTimelineItem.hpp"
#include "TracyView.hpp"

namespace tracy
{

class TimelineItemPlot final : public TimelineItem
{
public:
    TimelineItemPlot( View& view, Worker& worker, PlotData* plot );
    bool PreventScrolling() const override;

protected:
    uint32_t HeaderColor() const override { return 0xFF44DDDD; }
    uint32_t HeaderColorInactive() const override { return 0xFF226E6E; }
    uint32_t HeaderLineColor() const override { return 0x8844DDDD; }
    const char* HeaderLabel() const override;

    int64_t RangeBegin() const override;
    int64_t RangeEnd() const override;

    void HeaderTooltip( const char* label ) const override;
    void HeaderLabelPrefix( const TimelineContext &ctx, int yOffset, float &xOffset ) override; // Return width of prefix
    void HeaderExtraContents( const TimelineContext& ctx, int offset, float &xOffset ) override;

    bool DrawContents( const TimelineContext& ctx, int& offset ) override;
    void DrawUiControls( const TimelineContext &ctx, int start, int &offset, float xOffset ) override;
    void DrawFinished() override;

    bool IsEmpty() const override;

    void Preprocess( const TimelineContext& ctx, TaskDispatch& td, bool visible, int yPos ) override;

private:
    PlotData *m_plot; // Linked list of PlotData for display
    std::vector<uint32_t> m_draw; // m_draw contains plot points to be drawn this frame for all component PlotData
    struct PlotLine
    {
        uint32_t m_begin = 0; // Index into m_draw
        uint32_t m_end = 0; // Index into m_draw
    };
    Vector<PlotLine> m_plotLines; 
    double m_max; // Calculated max visible plot value across all component plot lines
    double m_userMax = -1.0;
    int m_bUseFixedMax = 0; // Set to 1 to display plot using user-provided fixed maximum value (defined as int for use with ImGui::RadioButton())

    bool m_rightEnd;
    TimelineResizeBar m_resizeBar;
};

}

#endif
