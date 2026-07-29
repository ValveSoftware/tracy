#ifndef __TRACYTIMELINEPREPROCESSOR_HPP__
#define __TRACYTIMELINEPREPROCESSOR_HPP__

#include "TracyEvent.hpp"
#include "TracyTimelineDraw.hpp"
#include <vector>


namespace tracy
{

struct TimelineContext;
class Worker;

class TimelinePreprocessor
{
public:
    TimelinePreprocessor( Worker &worker, int m_maxDrawDepth, uint8_t stackCollapseMode );

	int PreprocessZoneLevel( const TimelineContext &ctx, const Vector<short_ptr<ZoneEvent>> &vec, TimelineDrawSubType subtype, uint16_t comprTid, bool visible, std::vector<TimelineDraw> &outDraw );

	int CalculateMaxZoneDepth( const Vector<short_ptr<ZoneEvent>> &vec );
    int CalculateMaxZoneDepthInRange( const Vector<short_ptr<ZoneEvent>> &vec, int64_t rangeStart, int64_t rangeEnd );

private:
	int PreprocessZoneLevel( const TimelineContext &ctx, const Vector<short_ptr<ZoneEvent>> &vec, TimelineDrawSubType subtype, uint16_t comprTid, int depth, bool visible, std::vector<TimelineDraw> &outDraw );

	template<typename Adapter, typename V>
	int PreprocessZoneLevel( const TimelineContext &ctx, const V &vec, TimelineDrawSubType subtype, uint16_t comprTid, int depth, bool visible, std::vector<TimelineDraw> &outDraw );

    int CalculateMaxZoneDepthInRange( const Vector<short_ptr<ZoneEvent>> &vec, int64_t rangeStart, int64_t rangeEnd, int depth );

    template<typename Adapter, typename V>
    int CalculateMaxZoneDepthInRange( const V& vec, int64_t rangeStart, int64_t rangeEnd, int depth );

    Worker &m_worker;
    int m_maxDrawDepth;
    uint8_t m_stackCollapseMode;
};


}

#endif
