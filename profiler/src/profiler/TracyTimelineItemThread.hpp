#ifndef __TRACYTIMELINEITEMTHREAD_HPP__
#define __TRACYTIMELINEITEMTHREAD_HPP__

#include "TracyEvent.hpp"
#include "TracyTimelineDraw.hpp"
#include "TracyTimelineItem.hpp"
#include "TracyView.hpp"
#include "TracyViewData.hpp"

namespace tracy
{

class TimelineItemThread final : public TimelineItem
{
public:
    TimelineItemThread( View& view, Worker& worker, const ThreadData* plot );

    bool PreventScrolling() const override;

protected:
    uint32_t HeaderColor() const override;
    uint32_t HeaderColorInactive() const override;
    uint32_t HeaderLineColor() const override;
    const char* HeaderLabel() const override;

    int64_t RangeBegin() const override;
    int64_t RangeEnd() const override;

    void HeaderTooltip( const char* label ) const override;
    void HeaderExtraContents( const TimelineContext& ctx, int offset, float &xOffset ) override;

    bool DrawContents( const TimelineContext& ctx, int& offset ) override;
    void DrawOverlay( const ImVec2& ul, const ImVec2& dr ) override;
    void DrawUiControls( const TimelineContext &ctx, int start, int &offset, float xOffset ) override;
    void DrawExtraPopupItems() override;
    void DrawFinished() override;

    bool IsEmpty() const override;

    void Preprocess( const TimelineContext& ctx, TaskDispatch& td, bool visible, int yPos ) override;

private:
#ifndef TRACY_NO_STATISTICS
    int PreprocessGhostLevel( const TimelineContext& ctx, const Vector<GhostZone>& vec, int depth, bool visible );
#endif

    void PreprocessContextSwitches( const TimelineContext& ctx, const ContextSwitch& ctxSwitch, bool visible );
    void PreprocessHwCounter( const TimelineContext &ctx, const ContextSwitch &ctxSwitch, const CpuData *pCpuData, int cpuDataCount, bool visible );
    void PreprocessSamples( const TimelineContext& ctx, const Vector<SampleData>& vec, bool visible, int yPos );
    void PreprocessMessages( const TimelineContext& ctx, const Vector<short_ptr<MessageData>>& vec, uint64_t tid, bool visible, int yPos );
    void PreprocessLocks( const TimelineContext &ctx, const unordered_flat_set<uint32_t> &lockIds, const unordered_flat_map<uint32_t, LockMap *> &locks, uint32_t tid, TaskDispatch &td, bool visible );

    void PreprocessLockTimeline( const TimelineContext &ctx,
                                 LockType type,
                                 bool onlyContended,
                                 const Vector<LockEventPtr>& timeline,
                                 const LockTimeRange range,
                                 uint8_t threadIndex,
                                 uint32_t lockId,
                                 bool visible,
                                 LockDraw *drawPtr,
                                 int64_t MinVisNs ) const;

    void PreprocessMergedLocks( const TimelineContext &ctx, const unordered_flat_map<uint32_t, LockMap*>& locks, uint32_t tid, int64_t MinVisNs, bool visible );

    const ThreadData* m_thread;
    bool m_ghost;

    std::vector<SamplesDraw> m_samplesDraw;
    std::vector<ContextSwitchDraw> m_ctxDraw;
    HwCounterDraw m_hwCounterDraw;
    std::vector<TimelineDraw> m_draw;
    std::vector<MessagesDraw> m_msgDraw;
    std::vector<std::unique_ptr<LockDraw>> m_lockDraw;

    std::vector<std::unique_ptr<LockMap>> m_mergedLockMaps;

    std::atomic<int32_t> m_mergeJobCounter;
    std::vector<std::unique_ptr<LockDraw>> m_mergeLockDrawTmp;

    int m_maxDepth;
    int m_maxZoneDepth;
    int m_maxGhostDepth;
    int m_depth;
    bool m_hasCtxSwitch;
    bool m_hasSamples;
    bool m_hasMessages;
    bool m_hasHwCounter;

    TrackUiData m_trackUiData;
};

}

#endif
