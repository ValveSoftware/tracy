#include <algorithm>
#include <limits>

#include "TracyTimelinePreprocessor.hpp"
#include "TracyImGui.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyViewData.hpp"
#include "TracyWorker.hpp"


namespace tracy
{

constexpr float MinVisSize = 3;


TimelinePreprocessor::TimelinePreprocessor( Worker &worker, int maxDrawDepth, uint8_t stackCollapseMode )
    : m_worker( worker )
    , m_maxDrawDepth(maxDrawDepth)
    , m_stackCollapseMode(stackCollapseMode)
{
}


int TimelinePreprocessor::PreprocessZoneLevel( const TimelineContext& ctx, const Vector<short_ptr<ZoneEvent>>& vec, TimelineDrawSubType subtype, uint16_t comprTid, bool visible, std::vector<TimelineDraw> &outDraw )
{
	return PreprocessZoneLevel( ctx, vec, subtype, comprTid, 0, visible, outDraw );
}


int TimelinePreprocessor::CalculateMaxZoneDepth( const Vector<short_ptr<ZoneEvent>>& vec )
{
	return CalculateMaxZoneDepthInRange( vec, 0, INT64_MAX );
}


int TimelinePreprocessor::CalculateMaxZoneDepthInRange( const Vector<short_ptr<ZoneEvent>> &vec, int64_t rangeStart, int64_t rangeEnd )
{
	return CalculateMaxZoneDepthInRange( vec, rangeStart, rangeEnd, 0 );
}


int TimelinePreprocessor::PreprocessZoneLevel( const TimelineContext& ctx, const Vector<short_ptr<ZoneEvent>>& vec, TimelineDrawSubType subtype, uint16_t comprTid, int depth, bool visible, std::vector<TimelineDraw> &outDraw )
{
	if ( m_stackCollapseMode == ViewData::CollapseLimit )
	{
		visible = ( depth < m_maxDrawDepth );
	}

	if ( vec.is_magic() )
	{
		return PreprocessZoneLevel<VectorAdapterDirect<ZoneEvent>>( ctx, *(Vector<ZoneEvent>*)( &vec ), subtype, comprTid, depth, visible, outDraw );
	}
	else
	{
		return PreprocessZoneLevel<VectorAdapterPointer<ZoneEvent>>( ctx, vec, subtype, comprTid, depth, visible, outDraw );
	}
}


template<typename Adapter, typename V>
int TimelinePreprocessor::PreprocessZoneLevel( const TimelineContext& ctx, const V& vec, TimelineDrawSubType subtype, uint16_t comprTid, int depth, bool visible, std::vector<TimelineDraw> &outDraw )
{
	const auto vStart = ctx.vStart;
	const auto vEnd = ctx.vEnd;
	const auto nspx = ctx.nspx;

	const auto MinVisNs = int64_t( round( GetScale() * MinVisSize * nspx ) );

	auto it = std::lower_bound( vec.begin(), vec.end(), vStart, [this] ( const auto& l, const auto& r ) { Adapter a; return m_worker.GetZoneEnd( a(l) ) < r; } );
	if( it == vec.end() ) return depth;

	const auto zitend = std::lower_bound( it, vec.end(), vEnd, [] ( const auto& l, const auto& r ) { Adapter a; return a(l).Start() < r; } );
	if( it == zitend ) return depth;
	Adapter a;
	if( !a(*it).IsEndValid() && m_worker.GetZoneEnd( a(*it) ) < vStart ) return depth;
	if( m_worker.GetZoneEnd( a(*(zitend-1)) ) < vStart ) return depth;

	int maxdepth = depth + 1;

	while( it < zitend )
	{
		auto& ev = a(*it);
		const auto end = m_worker.GetZoneEnd( ev );
		const auto zsz = end - ev.Start();
		if( zsz < MinVisNs )
		{
			auto nextTime = end + MinVisNs;
			auto next = it + 1;
			for(;;)
			{
				next = std::lower_bound( next, zitend, nextTime, [this] ( const auto& l, const auto& r ) { Adapter a; return m_worker.GetZoneEnd( a(l) ) < r; } );
				if( next == zitend ) break;
				auto prev = next - 1;
				const auto pt = m_worker.GetZoneEnd( a(*prev) );
				const auto nt = m_worker.GetZoneEnd( a(*next) );
				if( nt - pt >= MinVisNs ) break;
				nextTime = nt + MinVisNs;
			}
			if( visible ) outDraw.emplace_back( TimelineDraw { TimelineDrawType::Folded, subtype, uint16_t( depth ), (void**)&ev, ev.Start(), m_worker.GetZoneEnd( a(*(next-1)) ), comprTid, uint32_t( next - it ) });
			it = next;
		}
		else
		{
			if( ev.HasChildren() )
			{
				const auto d = PreprocessZoneLevel( ctx, m_worker.GetZoneChildren( ev.Child() ), subtype, comprTid, depth + 1, visible, outDraw );
				if( d > maxdepth ) maxdepth = d;
			}

			if( visible ) outDraw.emplace_back( TimelineDraw { TimelineDrawType::Zone, subtype, uint16_t( depth ), (void**)&ev, ev.Start(), end, comprTid, 0});
			++it;
		}
	}

	return maxdepth;
}


int TimelinePreprocessor::CalculateMaxZoneDepthInRange( const Vector<short_ptr<ZoneEvent>> &vec, int64_t rangeStart, int64_t rangeEnd, int depth )
{
	assert( rangeStart <= rangeEnd );
	if ( vec.is_magic() )
    {
        return CalculateMaxZoneDepthInRange<VectorAdapterDirect<ZoneEvent>>( *(Vector<ZoneEvent>*)( &vec ), rangeStart, rangeEnd, depth );
    }
    else
    {
        return CalculateMaxZoneDepthInRange<VectorAdapterPointer<ZoneEvent>>( vec, rangeStart, rangeEnd, depth );
    }
}


template<typename Adapter, typename V>
int TimelinePreprocessor::CalculateMaxZoneDepthInRange( const V& vec, int64_t rangeStart, int64_t rangeEnd, int depth )
{
    auto it = std::lower_bound( vec.begin(), vec.end(), rangeStart, [this] ( const auto& l, const auto& r ) { Adapter a; return m_worker.GetZoneEnd( a(l) ) < r; } );
    if( it == vec.end() ) return depth;

    const auto zitend = std::lower_bound( it, vec.end(), rangeEnd, [this] ( const auto& l, const auto& r ) { Adapter a; return a(l).Start() < r; } );
    if( it == zitend ) return depth;
    Adapter a;
    if( !a(*it).IsEndValid() && m_worker.GetZoneEnd( a(*it) ) < rangeStart ) return depth;
    if( m_worker.GetZoneEnd( a(*(zitend-1)) ) < rangeStart ) return depth;

    int maxdepth = depth + 1;

    while( it < zitend )
    {
        const auto &ev = a( *it );
        const int64_t start = std::max( ev.Start(), rangeStart );
        const int64_t end = std::min( m_worker.GetZoneEnd( ev ), rangeEnd );
        if( ev.HasChildren() )
        {
            const int d = CalculateMaxZoneDepthInRange( m_worker.GetZoneChildren( ev.Child() ), start, end, depth + 1 );
            if( d > maxdepth ) maxdepth = d;
        }
        ++it;
    }

    return maxdepth;
}


}
