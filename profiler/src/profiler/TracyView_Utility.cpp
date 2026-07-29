#include <inttypes.h>

#include "TracyColor.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineContext.hpp"
#include "TracyUtility.hpp"
#include "TracyView.hpp"

namespace tracy
{

bool View::IsFrameExternal( const char* filename, const char* image )
{
    if( strncmp( filename, "/usr/", 5 ) == 0 || strncmp( filename, "/lib/", 5 ) == 0 || strcmp( filename, "[unknown]" ) == 0 || strcmp( filename, "<kernel>" ) == 0 ) return true;
    if( strncmp( filename, "C:\\Program Files\\", 17 ) == 0 || strncmp( filename, "d:\\a01\\_work\\", 13 ) == 0 ) return true;
    if( !image ) return false;
    return strncmp( image, "/usr/", 5 ) == 0 || strncmp( image, "/lib/", 5 ) == 0 || strncmp( image, "/lib64/", 7 ) == 0 || strcmp( image, "<kernel>" ) == 0;
}

uint32_t View::GetThreadColor( uint64_t thread, int depth )
{
    return tracy::GetThreadColor( thread, depth, m_vd.dynamicColors != 0 );
}

uint32_t View::GetRawSrcLocColor( const SourceLocation& srcloc, int depth )
{
    auto namehash = srcloc.namehash;
    if( namehash == 0 && srcloc.function.active )
    {
        const auto f = m_worker.GetString( srcloc.function );
        namehash = charutil::hash( f );
        if( namehash == 0 ) namehash++;
        srcloc.namehash = namehash;
    }
    if( namehash == 0 )
    {
        return GetHsvColor( uint64_t( &srcloc ), depth );
    }
    else
    {
        return GetHsvColor( namehash, depth );
    }
}

uint32_t View::GetSrcLocColor( const SourceLocation& srcloc, int depth )
{
    const auto color = srcloc.color;
    if( color != 0 && !m_vd.forceColors ) return color | 0xFF000000;
    if( m_vd.dynamicColors == 0 ) return 0xFFCC5555;
    return GetRawSrcLocColor( srcloc, depth );
}

uint32_t View::GetZoneColor( const ZoneEvent& ev, uint64_t thread, int depth )
{
    const auto sl = ev.SrcLoc();
    const auto& srcloc = m_worker.GetSourceLocation( sl );
    if( !m_vd.forceColors )
    {
        if( m_worker.HasZoneExtra( ev ) )
        {
            const auto custom_color = m_worker.GetZoneExtra( ev ).color.Val();
            if( custom_color != 0 ) return custom_color | 0xFF000000;
        }
        const auto color = srcloc.color;
        if( color != 0 ) return color | 0xFF000000;
    }
    switch( m_vd.dynamicColors )
    {
    case 0:
        return 0xFFCC5555;
    case 1:
        return GetHsvColor( thread, depth );
    case 2:
        return GetRawSrcLocColor( srcloc, depth );
    default:
        assert( false );
        return 0;
    }
}

uint32_t View::GetZoneColor( const GpuEvent& ev )
{
    const auto& srcloc = m_worker.GetSourceLocation( ev.SrcLoc() );
    const auto color = srcloc.color;
    return color != 0 ? ( color | 0xFF000000 ) : 0xFF222288;
}

View::ZoneColorData View::GetZoneColorData( const ZoneEvent& ev, uint64_t thread, int depth, uint32_t inheritedColor )
{
    ZoneColorData ret;
    const auto& srcloc = ev.SrcLoc();
    if( m_zoneInfoWindow == &ev )
    {
        ret.color = inheritedColor ? inheritedColor : GetZoneColor( ev, thread, depth );
        ret.accentColor = 0xFF44DD44;
        ret.thickness = 3.f;
        ret.highlight = true;
    }
    else if( m_zoneHighlight == &ev )
    {
        ret.color = inheritedColor ? inheritedColor : GetZoneColor( ev, thread, depth );
        ret.accentColor = 0xFF4444FF;
        ret.thickness = 3.f;
        ret.highlight = true;
    }
    else if( m_zoneSrcLocHighlight == srcloc )
    {
        ret.color = inheritedColor ? inheritedColor : GetZoneColor( ev, thread, depth );
        ret.accentColor = 0xFFEEEEEE;
        ret.thickness = 1.f;
        ret.highlight = true;
    }
    else if( m_findZone.show && !m_findZone.match.empty() && m_findZone.match[m_findZone.selMatch] == srcloc )
    {
        uint32_t color = 0xFF229999;
        if( m_findZone.highlight.active )
        {
            const auto zt = m_worker.GetZoneEnd( ev ) - ev.Start();
            if( zt >= m_findZone.highlight.start && zt <= m_findZone.highlight.end )
            {
                color = 0xFFFFCC66;
            }
        }
        ret.color = color;
        ret.accentColor = HighlightColor( color );
        ret.thickness = 3.f;
        ret.highlight = true;
    }
    else
    {
        const auto color = inheritedColor ? inheritedColor : GetZoneColor( ev, thread, depth );
        ret.color = color;
        ret.accentColor = HighlightColor( color );
        ret.thickness = 1.f;
        ret.highlight = false;
    }
    return ret;
}

View::ZoneColorData View::GetZoneColorData( const GpuEvent& ev )
{
    ZoneColorData ret;
    const auto color = GetZoneColor( ev );
    ret.color = color;
    if( m_gpuInfoWindow == &ev )
    {
        ret.accentColor = 0xFF44DD44;
        ret.thickness = 3.f;
        ret.highlight = true;
    }
    else if( m_gpuHighlight == &ev )
    {
        ret.accentColor = 0xFF4444FF;
        ret.thickness = 3.f;
        ret.highlight = true;
    }
    else
    {
        ret.accentColor = HighlightColor( color );
        ret.thickness = 1.f;
        ret.highlight = false;
    }
    return ret;
}


const ZoneEvent* View::FindZoneAtTime( uint64_t thread, int64_t time ) const
{
    // TODO add thread rev-map
    ThreadData* td = nullptr;
    for( const auto& t : m_worker.GetThreadData() )
    {
        if( t->id == thread )
        {
            td = t;
            break;
        }
    }
    if( !td ) return nullptr;

    const Vector<short_ptr<ZoneEvent>>* timeline = &td->timeline;
    if( timeline->empty() ) return nullptr;
    const ZoneEvent* ret = nullptr;
    for(;;)
    {
        if( timeline->is_magic() )
        {
            auto vec = (Vector<ZoneEvent>*)timeline;
            auto it = std::upper_bound( vec->begin(), vec->end(), time, [] ( const auto& l, const auto& r ) { return l < r.Start(); } );
            if( it != vec->begin() ) --it;
            if( it->Start() > time || ( it->IsEndValid() && it->End() < time ) ) return ret;
            ret = it;
            if( !it->HasChildren() ) return ret;
            timeline = &m_worker.GetZoneChildren( it->Child() );
        }
        else
        {
            auto it = std::upper_bound( timeline->begin(), timeline->end(), time, [] ( const auto& l, const auto& r ) { return l < r->Start(); } );
            if( it != timeline->begin() ) --it;
            if( (*it)->Start() > time || ( (*it)->IsEndValid() && (*it)->End() < time ) ) return ret;
            ret = *it;
            if( !(*it)->HasChildren() ) return ret;
            timeline = &m_worker.GetZoneChildren( (*it)->Child() );
        }
    }
}

const ZoneEvent* View::GetZoneChild( const ZoneEvent& zone, int64_t time ) const
{
    if( !zone.HasChildren() ) return nullptr;
    auto& children = m_worker.GetZoneChildren( zone.Child() );
    if( children.is_magic() )
    {
        auto& vec = *((Vector<ZoneEvent>*)&children);
        auto it = std::upper_bound( vec.begin(), vec.end(), time, [] ( const auto& l, const auto& r ) { return l < r.Start(); } );
        if( it != vec.begin() ) --it;
        if( it->Start() > time || ( it->IsEndValid() && it->End() < time ) ) return nullptr;
        return it;
    }
    else
    {
        auto it = std::upper_bound( children.begin(), children.end(), time, [] ( const auto& l, const auto& r ) { return l < r->Start(); } );
        if( it != children.begin() ) --it;
        if( (*it)->Start() > time || ( (*it)->IsEndValid() && (*it)->End() < time ) ) return nullptr;
        return *it;
    }
}

const ZoneEvent* View::GetZoneParent( const ZoneEvent& zone ) const
{
#ifndef TRACY_NO_STATISTICS
    if( m_worker.AreSourceLocationZonesReady() )
    {
        auto& slz = m_worker.GetZonesForSourceLocation( zone.SrcLoc() );
        if( !slz.zones.empty() && slz.zones.is_sorted() )
        {
            auto it = std::lower_bound( slz.zones.begin(), slz.zones.end(), zone.Start(), [] ( const auto& lhs, const auto& rhs ) { return lhs.Zone()->Start() < rhs; } );
            if( it != slz.zones.end() && it->Zone() == &zone )
            {
                return GetZoneParent( zone, m_worker.DecompressThread( it->Thread() ), m_worker );
            }
        }
    }
#endif

    for( const auto& thread : m_worker.GetThreadData() )
    {
        const ZoneEvent* parent = nullptr;
        const Vector<short_ptr<ZoneEvent>>* timeline = &thread->timeline;
        if( timeline->empty() ) continue;
        for(;;)
        {
            if( timeline->is_magic() )
            {
                auto vec = (Vector<ZoneEvent>*)timeline;
                auto it = std::upper_bound( vec->begin(), vec->end(), zone.Start(), [] ( const auto& l, const auto& r ) { return l < r.Start(); } );
                if( it != vec->begin() ) --it;
                if( zone.IsEndValid() && it->Start() > zone.End() ) break;
                if( it == &zone ) return parent;
                if( !it->HasChildren() ) break;
                parent = it;
                timeline = &m_worker.GetZoneChildren( parent->Child() );
            }
            else
            {
                auto it = std::upper_bound( timeline->begin(), timeline->end(), zone.Start(), [] ( const auto& l, const auto& r ) { return l < r->Start(); } );
                if( it != timeline->begin() ) --it;
                if( zone.IsEndValid() && (*it)->Start() > zone.End() ) break;
                if( *it == &zone ) return parent;
                if( !(*it)->HasChildren() ) break;
                parent = *it;
                timeline = &m_worker.GetZoneChildren( parent->Child() );
            }
        }
    }
    return nullptr;
}

const ZoneEvent* View::GetZoneParent( const ZoneEvent& zone, uint64_t tid, const Worker &worker ) const
{
    const auto thread = worker.GetThreadData( tid );
    const ZoneEvent* parent = nullptr;
    const Vector<short_ptr<ZoneEvent>>* timeline = &thread->timeline;
    if( timeline->empty() ) return nullptr;
    for(;;)
    {
        if( timeline->is_magic() )
        {
            auto vec = (Vector<ZoneEvent>*)timeline;
            auto it = std::upper_bound( vec->begin(), vec->end(), zone.Start(), [] ( const auto& l, const auto& r ) { return l < r.Start(); } );
            if( it != vec->begin() ) --it;
            if( zone.IsEndValid() && it->Start() > zone.End() ) break;
            if( it == &zone ) return parent;
            if( !it->HasChildren() ) break;
            parent = it;
            timeline = &worker.GetZoneChildren( parent->Child() );
        }
        else
        {
            auto it = std::upper_bound( timeline->begin(), timeline->end(), zone.Start(), [] ( const auto& l, const auto& r ) { return l < r->Start(); } );
            if( it != timeline->begin() ) --it;
            if( zone.IsEndValid() && (*it)->Start() > zone.End() ) break;
            if( *it == &zone ) return parent;
            if( !(*it)->HasChildren() ) break;
            parent = *it;
            timeline = &worker.GetZoneChildren( parent->Child() );
        }
    }
    return nullptr;
}

bool View::IsZoneReentry( const ZoneEvent& zone ) const
{
#ifndef TRACY_NO_STATISTICS
    if( m_worker.AreSourceLocationZonesReady() )
    {
        auto& slz = m_worker.GetZonesForSourceLocation( zone.SrcLoc() );
        if( !slz.zones.empty() && slz.zones.is_sorted() )
        {
            auto it = std::lower_bound( slz.zones.begin(), slz.zones.end(), zone.Start(), [] ( const auto& lhs, const auto& rhs ) { return lhs.Zone()->Start() < rhs; } );
            if( it != slz.zones.end() && it->Zone() == &zone )
            {
                return IsZoneReentry( zone, m_worker.DecompressThread( it->Thread() ) );
            }
        }
    }
#endif

    for( const auto& thread : m_worker.GetThreadData() )
    {
        const ZoneEvent* parent = nullptr;
        const Vector<short_ptr<ZoneEvent>>* timeline = &thread->timeline;
        if( timeline->empty() ) continue;
        for(;;)
        {
            if( timeline->is_magic() )
            {
                auto vec = (Vector<ZoneEvent>*)timeline;
                auto it = std::upper_bound( vec->begin(), vec->end(), zone.Start(), [] ( const auto& l, const auto& r ) { return l < r.Start(); } );
                if( it != vec->begin() ) --it;
                if( zone.IsEndValid() && it->Start() > zone.End() ) break;
                if( it == &zone ) return false;
                if( !it->HasChildren() ) break;
                parent = it;
                if (parent->SrcLoc() == zone.SrcLoc() ) return true;
                timeline = &m_worker.GetZoneChildren( parent->Child() );
            }
            else
            {
                auto it = std::upper_bound( timeline->begin(), timeline->end(), zone.Start(), [] ( const auto& l, const auto& r ) { return l < r->Start(); } );
                if( it != timeline->begin() ) --it;
                if( zone.IsEndValid() && (*it)->Start() > zone.End() ) break;
                if( *it == &zone ) return false;
                if( !(*it)->HasChildren() ) break;
                parent = *it;
                if (parent->SrcLoc() == zone.SrcLoc() ) return true;
                timeline = &m_worker.GetZoneChildren( parent->Child() );
            }
        }
    }
    return false;
}

bool View::IsZoneReentry( const ZoneEvent& zone, uint64_t tid ) const
{
    const auto thread = m_worker.GetThreadData( tid );
    const ZoneEvent* parent = nullptr;
    const Vector<short_ptr<ZoneEvent>>* timeline = &thread->timeline;
    if( timeline->empty() ) return false;
    for(;;)
    {
        if( timeline->is_magic() )
        {
            auto vec = (Vector<ZoneEvent>*)timeline;
            auto it = std::upper_bound( vec->begin(), vec->end(), zone.Start(), [] ( const auto& l, const auto& r ) { return l < r.Start(); } );
            if( it != vec->begin() ) --it;
            if( zone.IsEndValid() && it->Start() > zone.End() ) break;
            if( it == &zone ) return false;
            if( !it->HasChildren() ) break;
            parent = it;
            if (parent->SrcLoc() == zone.SrcLoc() ) return true;
            timeline = &m_worker.GetZoneChildren( parent->Child() );
        }
        else
        {
            auto it = std::upper_bound( timeline->begin(), timeline->end(), zone.Start(), [] ( const auto& l, const auto& r ) { return l < r->Start(); } );
            if( it != timeline->begin() ) --it;
            if( zone.IsEndValid() && (*it)->Start() > zone.End() ) break;
            if( *it == &zone ) return false;
            if( !(*it)->HasChildren() ) break;
            parent = *it;
            if (parent->SrcLoc() == zone.SrcLoc() ) return true;
            timeline = &m_worker.GetZoneChildren( parent->Child() );
        }
    }
    return false;
}

const GpuEvent* View::GetZoneParent( const GpuEvent& zone ) const
{
    for( const auto& ctx : m_worker.GetGpuData() )
    {
        for( const auto& td : ctx->threadData )
        {
            const GpuEvent* parent = nullptr;
            const Vector<short_ptr<GpuEvent>>* timeline = &td.second.timeline;
            if( timeline->empty() ) continue;
            for(;;)
            {
                if( timeline->is_magic() )
                {
                    auto vec = (Vector<GpuEvent>*)timeline;
                    auto it = std::upper_bound( vec->begin(), vec->end(), zone.GpuStart(), [] ( const auto& l, const auto& r ) { return (uint64_t)l < (uint64_t)r.GpuStart(); } );
                    if( it != vec->begin() ) --it;
                    if( zone.GpuEnd() >= 0 && it->GpuStart() > zone.GpuEnd() ) break;
                    if( it == &zone ) return parent;
                    if( it->Child() < 0 ) break;
                    parent = it;
                    timeline = &m_worker.GetGpuChildren( parent->Child() );
                }
                else
                {
                    auto it = std::upper_bound( timeline->begin(), timeline->end(), zone.GpuStart(), [] ( const auto& l, const auto& r ) { return (uint64_t)l < (uint64_t)r->GpuStart(); } );
                    if( it != timeline->begin() ) --it;
                    if( zone.GpuEnd() >= 0 && (*it)->GpuStart() > zone.GpuEnd() ) break;
                    if( *it == &zone ) return parent;
                    if( (*it)->Child() < 0 ) break;
                    parent = *it;
                    timeline = &m_worker.GetGpuChildren( parent->Child() );
                }
            }
        }
    }
    return nullptr;
}

const ThreadData* View::GetZoneThreadData( const ZoneEvent& zone ) const
{
#ifndef TRACY_NO_STATISTICS
    if( m_worker.AreSourceLocationZonesReady() )
    {
        auto& slz = m_worker.GetZonesForSourceLocation( zone.SrcLoc() );
        if( !slz.zones.empty() && slz.zones.is_sorted() )
        {
            auto it = std::lower_bound( slz.zones.begin(), slz.zones.end(), zone.Start(), [] ( const auto& lhs, const auto& rhs ) { return lhs.Zone()->Start() < rhs; } );
            if( it != slz.zones.end() && it->Zone() == &zone )
            {
                return m_worker.GetThreadData( m_worker.DecompressThread( it->Thread() ) );
            }
        }
    }
#endif

    for( const auto& thread : m_worker.GetThreadData() )
    {
        const Vector<short_ptr<ZoneEvent>>* timeline = &thread->timeline;
        if( timeline->empty() ) continue;
        for(;;)
        {
            if( timeline->is_magic() )
            {
                auto vec = (Vector<ZoneEvent>*)timeline;
                auto it = std::upper_bound( vec->begin(), vec->end(), zone.Start(), [] ( const auto& l, const auto& r ) { return l < r.Start(); } );
                if( it != vec->begin() ) --it;
                if( zone.IsEndValid() && it->Start() > zone.End() ) break;
                if( it == &zone ) return thread;
                if( !it->HasChildren() ) break;
                timeline = &m_worker.GetZoneChildren( it->Child() );
            }
            else
            {
                auto it = std::upper_bound( timeline->begin(), timeline->end(), zone.Start(), [] ( const auto& l, const auto& r ) { return l < r->Start(); } );
                if( it != timeline->begin() ) --it;
                if( zone.IsEndValid() && (*it)->Start() > zone.End() ) break;
                if( *it == &zone ) return thread;
                if( !(*it)->HasChildren() ) break;
                timeline = &m_worker.GetZoneChildren( (*it)->Child() );
            }
        }
    }
    return nullptr;
}

uint64_t View::GetZoneThread( const ZoneEvent& zone ) const
{
    auto threadData = GetZoneThreadData( zone );
    return threadData ? threadData->id : 0;
}

uint64_t View::GetZoneThread( const GpuEvent& zone ) const
{
    if( zone.Thread() == 0 )
    {
        for( const auto& ctx : m_worker.GetGpuData() )
        {
            if ( ctx->threadData.size() != 1 ) continue;
            const Vector<short_ptr<GpuEvent>>* timeline = &ctx->threadData.begin()->second.timeline;
            if( timeline->empty() ) continue;
            for(;;)
            {
                if( timeline->is_magic() )
                {
                    auto vec = (Vector<GpuEvent>*)timeline;
                    auto it = std::upper_bound( vec->begin(), vec->end(), zone.GpuStart(), [] ( const auto& l, const auto& r ) { return (uint64_t)l < (uint64_t)r.GpuStart(); } );
                    if( it != vec->begin() ) --it;
                    if( zone.GpuEnd() >= 0 && it->GpuStart() > zone.GpuEnd() ) break;
                    if( it == &zone ) return ctx->thread;
                    if( it->Child() < 0 ) break;
                    timeline = &m_worker.GetGpuChildren( it->Child() );
                }
                else
                {
                    auto it = std::upper_bound( timeline->begin(), timeline->end(), zone.GpuStart(), [] ( const auto& l, const auto& r ) { return (uint64_t)l < (uint64_t)r->GpuStart(); } );
                    if( it != timeline->begin() ) --it;
                    if( zone.GpuEnd() >= 0 && (*it)->GpuStart() > zone.GpuEnd() ) break;
                    if( *it == &zone ) return ctx->thread;
                    if( (*it)->Child() < 0 ) break;
                    timeline = &m_worker.GetGpuChildren( (*it)->Child() );
                }
            }
        }
        return 0;
    }
    else
    {
        return m_worker.DecompressThread( zone.Thread() );
    }
}

const GpuCtxData* View::GetZoneCtx( const GpuEvent& zone ) const
{
    for( const auto& ctx : m_worker.GetGpuData() )
    {
        for( const auto& td : ctx->threadData )
        {
            const Vector<short_ptr<GpuEvent>>* timeline = &td.second.timeline;
            if( timeline->empty() ) continue;
            for(;;)
            {
                if( timeline->is_magic() )
                {
                    auto vec = (Vector<GpuEvent>*)timeline;
                    auto it = std::upper_bound( vec->begin(), vec->end(), zone.GpuStart(), [] ( const auto& l, const auto& r ) { return (uint64_t)l < (uint64_t)r.GpuStart(); } );
                    if( it != vec->begin() ) --it;
                    if( zone.GpuEnd() >= 0 && it->GpuStart() > zone.GpuEnd() ) break;
                    if( it == &zone ) return ctx;
                    if( it->Child() < 0 ) break;
                    timeline = &m_worker.GetGpuChildren( it->Child() );
                }
                else
                {
                    auto it = std::upper_bound( timeline->begin(), timeline->end(), zone.GpuStart(), [] ( const auto& l, const auto& r ) { return (uint64_t)l < (uint64_t)r->GpuStart(); } );
                    if( it != timeline->begin() ) --it;
                    if( zone.GpuEnd() >= 0 && (*it)->GpuStart() > zone.GpuEnd() ) break;
                    if( *it == &zone ) return ctx;
                    if( (*it)->Child() < 0 ) break;
                    timeline = &m_worker.GetGpuChildren( (*it)->Child() );
                }
            }
        }
    }
    return nullptr;
}

int64_t View::GetZoneChildTimeClamped( const ZoneEvent &zone, int64_t start, int64_t end )
{
    int64_t time = 0;
    if( zone.HasChildren() )
    {
        auto& children = m_worker.GetZoneChildren( zone.Child() );
        if( children.is_magic() )
        {
            auto& vec = *(Vector<ZoneEvent>*)&children;
            for( auto& v : vec )
            {
                const int64_t vstart = v.Start();
                const int64_t vend = v.End();

                const int64_t clampedstart = std::max( vstart, start );
                const int64_t clampedend = std::min( vend, end );
                const auto childSpan = std::max( int64_t( 0 ), clampedend - clampedstart );
                time += childSpan;
            }
        }
        else
        {
            for( auto& v : children )
            {
                const int64_t vstart = v->Start();
                const int64_t vend = v->End();

                const int64_t clampedstart = std::max( vstart, start );
                const int64_t clampedend = std::min( vend, end );
                const auto childSpan = std::max( int64_t( 0 ), clampedend - clampedstart );
                time += childSpan;
            }
        }
    }
    return time;
}

int64_t View::GetZoneChildTime( const ZoneEvent& zone )
{
    const int64_t start = zone.Start();
    const int64_t end = m_worker.GetZoneEnd( zone );
    return GetZoneChildTimeClamped( zone, start, end );
}

int64_t View::GetZoneChildTime( const GpuEvent& zone )
{
    int64_t time = 0;
    if( zone.Child() >= 0 )
    {
        auto& children = m_worker.GetGpuChildren( zone.Child() );
        if( children.is_magic() )
        {
            auto& vec = *(Vector<GpuEvent>*)&children;
            for( auto& v : vec )
            {
                const auto childSpan = std::max( int64_t( 0 ), v.GpuEnd() - v.GpuStart() );
                time += childSpan;
            }
        }
        else
        {
            for( auto& v : children )
            {
                const auto childSpan = std::max( int64_t( 0 ), v->GpuEnd() - v->GpuStart() );
                time += childSpan;
            }
        }
    }
    return time;
}

int64_t View::GetZoneChildTimeFast( const ZoneEvent& zone )
{
    int64_t time = 0;
    if( zone.HasChildren() )
    {
        auto& children = m_worker.GetZoneChildren( zone.Child() );
        if( children.is_magic() )
        {
            auto& vec = *(Vector<ZoneEvent>*)&children;
            for( auto& v : vec )
            {
                assert( v.IsEndValid() );
                time += v.End() - v.Start();
            }
        }
        else
        {
            for( auto& v : children )
            {
                assert( v->IsEndValid() );
                time += v->End() - v->Start();
            }
        }
    }
    return time;
}

int64_t View::GetZoneChildTimeFastClamped( const ZoneEvent& zone, int64_t t0, int64_t t1 )
{
    int64_t time = 0;
    if( zone.HasChildren() )
    {
        auto& children = m_worker.GetZoneChildren( zone.Child() );
        if( children.is_magic() )
        {
            auto& vec = *(Vector<ZoneEvent>*)&children;
            auto it = std::lower_bound( vec.begin(), vec.end(), t0, [] ( const auto& l, const auto& r ) { return (uint64_t)l.End() < (uint64_t)r; } );
            if( it == vec.end() ) return 0;
            const auto zitend = std::lower_bound( it, vec.end(), t1, [] ( const auto& l, const auto& r ) { return l.Start() < r; } );
            if( it == zitend ) return 0;
            while( it < zitend )
            {
                const auto c0 = std::max<int64_t>( it->Start(), t0 );
                const auto c1 = std::min<int64_t>( it->End(), t1 );
                time += c1 - c0;
                ++it;
            }
        }
        else
        {
            auto it = std::lower_bound( children.begin(), children.end(), t0, [] ( const auto& l, const auto& r ) { return (uint64_t)l->End() < (uint64_t)r; } );
            if( it == children.end() ) return 0;
            const auto zitend = std::lower_bound( it, children.end(), t1, [] ( const auto& l, const auto& r ) { return l->Start() < r; } );
            if( it == zitend ) return 0;
            while( it < zitend )
            {
                const auto c0 = std::max<int64_t>( (*it)->Start(), t0 );
                const auto c1 = std::min<int64_t>( (*it)->End(), t1 );
                time += c1 - c0;
                ++it;
            }
        }
    }
    return time;
}

int64_t View::GetZoneSelfTimeClamped( const ZoneEvent &zone, int64_t start, int64_t end )
{
    const auto ztime = end - start;
    const auto selftime = ztime - GetZoneChildTimeClamped( zone, start, end );
    return selftime;
}

int64_t View::GetZoneSelfTime( const ZoneEvent& zone )
{
    if( m_cache.zoneSelfTime.first == &zone ) return m_cache.zoneSelfTime.second;
    if( m_cache.zoneSelfTime2.first == &zone ) return m_cache.zoneSelfTime2.second;
    const auto ztime = m_worker.GetZoneEnd( zone ) - zone.Start();
    const auto selftime = ztime - GetZoneChildTime( zone );
    if( zone.IsEndValid() )
    {
        m_cache.zoneSelfTime2 = m_cache.zoneSelfTime;
        m_cache.zoneSelfTime = std::make_pair( &zone, selftime );
    }
    return selftime;
}

int64_t View::GetZoneSelfTime( const GpuEvent& zone )
{
    if( m_cache.gpuSelfTime.first == &zone ) return m_cache.gpuSelfTime.second;
    if( m_cache.gpuSelfTime2.first == &zone ) return m_cache.gpuSelfTime2.second;
    const auto ztime = m_worker.GetZoneEnd( zone ) - zone.GpuStart();
    const auto selftime = ztime - GetZoneChildTime( zone );
    if( zone.GpuEnd() >= 0 )
    {
        m_cache.gpuSelfTime2 = m_cache.gpuSelfTime;
        m_cache.gpuSelfTime = std::make_pair( &zone, selftime );
    }
    return selftime;
}

bool View::GetZoneRunningTime( const ContextSwitch* ctx, const ZoneEvent& ev, int64_t& time, uint64_t& cnt )
{
    auto it = std::lower_bound( ctx->v.begin(), ctx->v.end(), ev.Start(), [] ( const auto& l, const auto& r ) { return (uint64_t)l.End() < (uint64_t)r; } );
    if( it == ctx->v.end() ) return false;
    const auto end = m_worker.GetZoneEnd( ev );
    const auto eit = std::upper_bound( it, ctx->v.end(), end, [] ( const auto& l, const auto& r ) { return l < r.Start(); } );
    if( eit == ctx->v.end() ) return false;
    cnt = std::distance( it, eit );
    if( cnt == 0 ) return false;
    if( cnt == 1 )
    {
        time = end - ev.Start();
    }
    else
    {
        int64_t running = it->End() - ev.Start();
        ++it;
        for( uint64_t i=0; i<cnt-2; i++ )
        {
            running += it->End() - it->Start();
            ++it;
        }
        running += end - it->Start();
        time = running;
    }
    return true;
}

bool View::GetZoneRunningTime( const ContextSwitch* ctx, const ZoneEvent& ev, const RangeSlim& range, int64_t& time, uint64_t& cnt )
{
    const auto start = std::max( ev.Start(), range.min );
    auto it = std::lower_bound( ctx->v.begin(), ctx->v.end(), start, [] ( const auto& l, const auto& r ) { return (uint64_t)l.End() < (uint64_t)r; } );
    if( it == ctx->v.end() ) return false;
    const auto end = std::min( m_worker.GetZoneEnd( ev ), range.max );
    const auto eit = std::upper_bound( it, ctx->v.end(), end, [] ( const auto& l, const auto& r ) { return l < r.Start(); } );
    if( eit == ctx->v.end() ) return false;
    cnt = std::distance( it, eit );
    if( cnt == 0 ) return false;
    if( cnt == 1 )
    {
        time = end - start;
    }
    else
    {
        int64_t running = it->End() - start;
        ++it;
        for( uint64_t i=0; i<cnt-2; i++ )
        {
            running += it->End() - it->Start();
            ++it;
        }
        running += end - it->Start();
        time = running;
    }
    return true;
}

const char* View::SourceSubstitution( const char* srcFile ) const
{
    if( !m_sourceRegexValid || m_sourceSubstitutions.empty() ) return srcFile;
    static std::string res, tmp;
    res.assign( srcFile );
    for( auto& v : m_sourceSubstitutions )
    {
        tmp = std::regex_replace( res, v.regex, v.target );
        std::swap( tmp, res );
    }
    return res.c_str();
}

int64_t View::AdjustGpuTime( int64_t time, int64_t begin, int drift )
{
    if( time < 0 ) return time;
    const auto t = time - begin;
    return time + t / 1000000000 * drift;
}

int64_t View::GetFrameNumber( const FrameData& fd, int i ) const
{
    if( fd.name == 0 )
    {
        const int64_t frameOffset = (int64_t)m_worker.GetFrameOffset();
        const int64_t frameAdjust = ( m_worker.IsOnDemand() ? 3 : 2 );
		return i + frameOffset - frameAdjust;
    }
    else
    {
        return i + 1;
    }
}

const char* View::GetFrameText( const FrameData& fd, int i, uint64_t ftime ) const
{
    const auto fnum = GetFrameNumber( fd, i );
    static char buf[1024];
    if( fd.name == 0 )
    {
        if( i == 0 )
        {
            sprintf( buf, "Tracy init (%s)", TimeToString( ftime ) );
        }
        else if( i != 1 || !m_worker.IsOnDemand() )
        {
            sprintf( buf, "Frame %s (%s)", RealToString( fnum ), TimeToString( ftime ) );
        }
        else
        {
            sprintf( buf, "Missed frames (%s)", TimeToString( ftime ) );
        }
    }
    else
    {
        sprintf( buf, "%s %s (%s)", GetFrameSetName( fd ), RealToString( fnum ), TimeToString( ftime ) );
    }
    return buf;
}

const char* View::GetFrameSetName( const FrameData& fd ) const
{
    return GetFrameSetName( fd, m_worker );
}

const char* View::GetFrameSetName( const FrameData& fd, const Worker& worker )
{
    enum { Pool = 4 };
    static char bufpool[Pool][64];
    static int bufsel = 0;

    if( fd.name == 0 )
    {
        return "Frames";
    }
    else if( fd.name >> 63 != 0 )
    {
        char* buf = bufpool[bufsel];
        bufsel = ( bufsel + 1 ) % Pool;
        sprintf( buf, "[%" PRIu32 "] Vsync", uint32_t( fd.name ) );
        return buf;
    }
    else
    {
        return worker.GetString( fd.name );
    }
}

const char* View::GetThreadContextData( uint64_t thread, bool& _local, bool& _untracked, const char*& program )
{
    static char buf[256];
    const auto local = m_worker.IsThreadLocal( thread );
    auto txt = local ? m_worker.GetThreadName( thread ) : m_worker.GetExternalName( thread ).first;
    auto label = txt;
    bool untracked = false;
    if( !local )
    {
        if( m_worker.GetPid() == 0 )
        {
            untracked = strcmp( txt, m_worker.GetCaptureProgram().c_str() ) == 0;
        }
        else
        {
            const auto pid = m_worker.GetPidFromTid( thread );
            untracked = pid == m_worker.GetPid();
            if( untracked )
            {
                label = txt = m_worker.GetExternalName( thread ).second;
            }
            else
            {
                const auto ttxt = m_worker.GetExternalName( thread ).second;
                if( strcmp( ttxt, "???" ) != 0 && strcmp( ttxt, txt ) != 0 )
                {
                    snprintf( buf, 256, "%s (%s)", txt, ttxt );
                    label = buf;
                }
            }
        }
    }
    _local = local;
    _untracked = untracked;
    program = txt;
    return label;
}

void View::Attention( bool& alreadyDone )
{
    if( !alreadyDone )
    {
        alreadyDone = true;
        m_acb();
    }
}

void View::UpdateTitle()
{
    auto captureName = m_worker.GetCaptureName().c_str();
    const auto& desc = m_userData.GetDescription();
    if( !desc.empty() )
    {
        char buf[1024];
        snprintf( buf, 1024, "%s (%s)", captureName, desc.c_str() );
        m_stcb( buf );
    }
    else if( !m_filename.empty() )
    {
        auto fptr = m_filename.c_str() + m_filename.size() - 1;
        while( fptr > m_filename.c_str() && *fptr != '/' && *fptr != '\\' ) fptr--;
        if( *fptr == '/' || *fptr == '\\' ) fptr++;

        char buf[1024];
        snprintf( buf, 1024, "%s (%s)", captureName, fptr );
        m_stcb( buf );
    }
    else
    {
        m_stcb( captureName );
    }
}

const ThreadData *View::GetThreadDataForCpu( uint8_t cpu, int64_t time )
{
	const auto cpuData = m_worker.GetCpuData();
	const auto cpuCnt = m_worker.GetCpuDataCpuCount();
	if ( ( cpu < cpuCnt ) && !cpuData[ cpu ].cs.empty() )
	{
		auto &cs = cpuData[ cpu ].cs;
		auto it = std::lower_bound( cs.begin(), cs.end(), time, [] ( const auto &l, const auto &r ) { return ( uint64_t ) l.End() < ( uint64_t ) r; } );
		if ( it != cs.end() && it->Start() <= time && it->End() >= time )
		{
			return m_worker.GetThreadData( m_worker.DecompressThreadExternal( it->Thread() ) );
		}
	}

	return nullptr;
}

float UpdateAndDrawResizeBar( TimelineResizeBar &resizeBar )
{
    float startY = ImGui::GetCursorPosY();

    float oldHeight = resizeBar.height;
    float newHeight = resizeBar.height;

    // Unfortuantely ImGui only uses the active color, if the item is active *and* hovered
    // Otherwise, if it is hovered, it uses the hovered color.
    // Otherwise it uses the button color.
    // This code unfortunately lags behind so we can manage to "unhover" the button while dragging
    // which would result in visual color glitches.
    const ImVec4 colButton = ( resizeBar.wasActive ? resizeBar.colActive : resizeBar.color );
    ImGui::PushStyleColor( ImGuiCol_Button, colButton );
    ImGui::PushStyleColor( ImGuiCol_ButtonActive, resizeBar.colActive );
    ImGui::PushStyleColor( ImGuiCol_ButtonHovered, resizeBar.colHover );
    ImVec2 buttonSize( -1.0f, resizeBar.thickness );

    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 0 ) );
    ImGui::PushStyleVar( ImGuiStyleVar_ItemInnerSpacing, ImVec2( 0, 0 ) );
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    float origFontScale = window->FontWindowScale;
    float scale = ( resizeBar.thickness / ImGui::GetFontSize() );
    ImGui::SetWindowFontScale( scale );
#define GL ICON_FA_GRIP_LINES
    ImGui::Button(GL GL GL GL GL GL GL GL GL "##ResizeBar", buttonSize);
#undef GL
    ImGui::SetWindowFontScale( origFontScale );
    ImGui::PopStyleVar( 3 );
    ImGui::PopStyleColor(3);

    resizeBar.id = ImGui::GetItemID();

    float endY = ImGui::GetCursorPosY();
    float rawUiHeight = ( endY - startY );
    resizeBar.uiHeight = rawUiHeight + ImGui::GetStyle().ItemSpacing.y * 2;
    assert( resizeBar.uiHeight >= 0.0f );

    bool isResizeBarActive = ImGui::IsItemActive();
    bool isResizeBarHovered = ImGui::IsItemHovered();
    resizeBar.wasActive = isResizeBarActive;

    if ( isResizeBarActive )
    {
        ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeNS );
        ImVec2 origMouseDelta = ImGui::GetIO().MouseDelta;
        const float adjustedHeight = ( resizeBar.height + origMouseDelta.y );
        const float newHeightClamped = ImClamp( adjustedHeight, resizeBar.minHeight, resizeBar.maxHeight );
        const float mouseDelta = ( newHeightClamped - resizeBar.height );
        newHeight = newHeightClamped;
    }

    if ( isResizeBarHovered )
    {
        ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeNS );
        ImGui::SetTooltip( "Drag to resize\n"
                           "Double click to resize to default height: %d",
                           (int)resizeBar.defaultHeight );

        if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
        {
            newHeight = resizeBar.defaultHeight;
        }
    }

    resizeBar.height = ImClamp( newHeight, resizeBar.minHeight, resizeBar.maxHeight );
    assert( resizeBar.height > 0.0f );

    float diff = ( newHeight - oldHeight );
    return diff;
}


void View::DrawTrackUiControls( const TimelineContext &ctx, const char* label, int maxDepth, int depth, TrackUiData& rData, ViewData::Track& rVdTrackSettings, int start, int &offset, float xOffset )
{
    if ( rData.preventScroll > 0 )
    {
        rData.preventScroll--;
    }

    const uint8_t uiControlLoc = m_vd.uiControlLoc;
    if ( uiControlLoc == ViewData::UiCtrlLocHidden )
    {
        return;
    }

    const float inputItemWidth = 90.0f;

    const char *controlPopupName = "UiControlsPopup";
    const char *radioDynamicLabel = "Dynamic##uiCtrlStackCollapseClamp";
    const char *radioMaxLabel = "Max##uiCtrlStackCollapseClamp";
    const char *radioLimitLabel = "Limit##uiCtrlStackCollapseClamp";
    const char *inputLabel = "##uiCtrlStackCollapseClampLimit";

    ImVec2 startPos( xOffset, start);
    ImGui::SetCursorPos( startPos );

    const bool isDisabled = !rData.settings.shouldOverride;

    ImVec4 bgcol = ImGui::GetStyleColorVec4( ImGuiCol_FrameBgActive );
    ImVec4 hovcol = ImGui::GetStyleColorVec4( ImGuiCol_FrameBgHovered );
    ImVec4 framecol = ImGui::GetStyleColorVec4( ImGuiCol_FrameBg );
    ImVec4 fontcol = ImGui::GetStyleColorVec4( ImGuiCol_Text );

    bgcol.w = 1.0f;
    hovcol.w = 1.0f;
    framecol.w = 1.0f;

    ImVec4 xbgcol = bgcol;
    ImVec4 xhovcol = hovcol;
    ImVec4 xframecol = framecol;
    ImVec4 xfontcol = fontcol;

    if ( isDisabled )
    {
        xbgcol = ImVec4( 0.2f, 0.2f, 0.2f, 1.0f );
        xhovcol = ImVec4( 0.2f, 0.2f, 0.2f, 1.0f );
        xframecol = ImVec4( 0.2f, 0.2f, 0.2f, 1.0f );
        xfontcol = ImVec4( 0.4f, 0.4f, 0.4f, 1.0f );
    }

    ImVec4 lockColor( 1.0f, 0.85f, 0.85f, 1.0f );
    const char *pButtonText = ICON_FA_LOCK;
    bool shouldOverride = rData.settings.shouldOverride;
    if ( shouldOverride )
    {
        lockColor = ImVec4( 0.85f, 1.0f, 0.85f, 1.0f );
        pButtonText = ICON_FA_LOCK_OPEN;
    }

    const ImVec2 wndContentRegionMax = ImGui::GetWindowContentRegionMax();

    const auto GetLabelWidth = [] ( const char *label ) -> float
    {
        float framePaddingWdith = ImGui::GetStyle().FramePadding.x;
        const float width = ImGui::CalcTextSize(label, NULL, true).x + framePaddingWdith * 2.0f;
        return width;
    };

    // lock button width
    const float lockButtonWidth = GetLabelWidth(pButtonText);

    // radio button sizewidth
    const float innerSpacingWidth = ImGui::GetStyle().ItemInnerSpacing.x;
    const float radioDynWidth = GetLabelWidth(radioDynamicLabel) + innerSpacingWidth + ImGui::GetFrameHeight();
    const float radioMaxWidth = GetLabelWidth(radioMaxLabel) + innerSpacingWidth + ImGui::GetFrameHeight();
    const float radioLimitWidth = GetLabelWidth(radioLimitLabel) + innerSpacingWidth + ImGui::GetFrameHeight();

    // input width
    ImGui::PushItemWidth( inputItemWidth );
    const float buttonSize = ImGui::GetFrameHeight();
    const float inputFieldWidth = ImMax(1.0f, ImGui::CalcItemWidth() - (buttonSize + innerSpacingWidth) * 2);
    const float incButtonWidth = innerSpacingWidth + ImMax(buttonSize, GetLabelWidth("+")); // 19
    const float decButtonWidth = innerSpacingWidth + ImMax(buttonSize, GetLabelWidth("-"));
    const float inputWidth = inputFieldWidth + incButtonWidth + decButtonWidth;
    ImGui::PopItemWidth();

    // total ui control width
    const float itemSpacingWidth = ImGui::GetStyle().ItemSpacing.x;
    const float uiSingleLineElementsWidth =   lockButtonWidth 
                                            + itemSpacingWidth + radioDynWidth 
                                            + itemSpacingWidth + radioMaxWidth 
                                            + itemSpacingWidth + radioLimitWidth 
                                            + itemSpacingWidth + inputWidth;

    bool renderControls = false;
    bool isPopup = false;

    const float ctrlButtonWidth = GetLabelWidth( ICON_FA_BARS );

    if ( uiControlLoc == ViewData::UiCtrlLocRight )
    {
        ImGui::SetCursorPosX( wndContentRegionMax.x - ctrlButtonWidth );
    }

    {
        ImVec4 curCol = ImGui::GetStyle().Colors[ImGuiCol_Text];
        const ImVec4 overrideCol( 0.75f, 1.0f, 0.75f, 1.0f );
        const ImVec4 buttonColor = shouldOverride ? overrideCol : curCol;
        ImGui::PushStyleColor( ImGuiCol_Text, buttonColor );
        if( ImGui::SmallButton( ICON_FA_BARS ) )
        {
            ImGui::OpenPopup( controlPopupName );
        }
        ImGui::PopStyleColor( 1 );
    }

    if ( ImGui::BeginPopup( controlPopupName ) )
    {
        if ( ( label != nullptr ) && ( label[ 0 ] != 0 ) )
        {
            ImGui::Text( label );
        }
        else
        {
            ImGui::Text( "unknown core" );
        }
        isPopup = true;
        renderControls = true;
    }

    if ( renderControls )
    {
        if ( !isPopup )
            ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );

        TrackUiSettings origSettings = rData.settings;

        ImGui::PushStyleColor( ImGuiCol_Text, lockColor );
        if ( ImGui::Button( pButtonText ) )
        {
            rData.settings.shouldOverride = !rData.settings.shouldOverride;
        }
        ImGui::SetItemTooltip( "Click to override global thread collapse settings" );
        ImGui::PopStyleColor( 1 );

        ImGui::PushItemFlag( ImGuiItemFlags_Disabled, isDisabled );

        ImGui::PushStyleColor( ImGuiCol_FrameBgActive, xbgcol );
        ImGui::PushStyleColor( ImGuiCol_FrameBgHovered, xhovcol );
        ImGui::PushStyleColor( ImGuiCol_FrameBg, xframecol );
        ImGui::PushStyleColor( ImGuiCol_Text, xfontcol );

        int ival = std::clamp( ( int ) rData.settings.stackCollapseMode, 0, 2 );

        if ( !isPopup )
        {
            ImGui::SameLine();
        }
        ImGui::RadioButton( radioDynamicLabel, &ival, ViewData::CollapseDynamic );
        if ( !isPopup )
        {
            ImGui::SameLine();
        }
        ImGui::RadioButton( radioMaxLabel, &ival, ViewData::CollapseMax );
        if ( !isPopup )
        {
            ImGui::SameLine();
        }
        ImGui::RadioButton( radioLimitLabel, &ival, ViewData::CollapseLimit );

        rData.settings.stackCollapseMode = ival;

        ImGui::SetNextItemWidth( inputItemWidth );

        if ( !isPopup )
        {
            ImGui::SameLine();
        }

        int tmp = ( ( rData.settings.stackCollapseMode == ViewData::CollapseMax ) ? maxDepth : rData.settings.stackCollapseClamp );
        if ( ImGui::InputInt( inputLabel, &tmp ) )
        {
            rData.settings.stackCollapseClamp = tmp;
            rData.settings.stackCollapseMode = ViewData::CollapseLimit;
        }

        if ( !rData.settings.shouldOverride )
        {
            rData.settings.stackCollapseMode = m_vd.stackCollapseMode;
            rData.settings.stackCollapseClamp = m_vd.stackCollapseClamp;
        }

        if ( maxDepth >= 1 )
        {
            rData.settings.stackCollapseClamp = std::clamp( rData.settings.stackCollapseClamp, 1, maxDepth );
        }

        if ( rData.settings.stackCollapseMode == ViewData::CollapseMax )
        {
            rData.settings.stackCollapseClamp = maxDepth;
        }
        else if ( rData.settings.stackCollapseMode == ViewData::CollapseDynamic )
        {
            rData.settings.stackCollapseClamp = depth;
        }

        ImGui::PopStyleColor( 4 );
        ImGui::PopItemFlag();

        if ( !isPopup )
            ImGui::PopStyleVar(); //ImGuiStyleVar_FramePadding

        if ( origSettings != rData.settings )
        {
            // We need to prevent scrolling for two frames, since the height of this item
            // is only evaluated to the new height in the next Preprocess() step
            rData.preventScroll = 2;
        }
    }

    if ( isPopup )
    {
        ImGui::EndPopup();
    }

    ImGui::SetCursorPosY( offset );

    if ( rData.settings != rVdTrackSettings.ui )
    {
        rVdTrackSettings.flags |= ViewData::Flags_Manual;
        rVdTrackSettings.ui = rData.settings;
        RequestSaveSettings();
    }
}


}
