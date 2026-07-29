#include <assert.h>
#include <memory>
#include <type_traits>

#ifdef _WIN32
#  include <stdio.h>
#else
#  include <unistd.h>
#endif

#include "../ini.h"

#include "TracyStorage.hpp"
#include "TracyUserData.hpp"
#include "TracyViewData.hpp"
#include "TracyEvent.hpp"
#include "TracyWorker.hpp"

#define RAPIDJSON_HAS_STDSTRING 1
#include "rapidjson/document.h"
#include "rapidjson/reader.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/error/en.h"

namespace tracy
{

static bool LoadJsonDocument( const char *file, rapidjson::Document &d )
{
    bool result = false;

    FILE* f = fopen( file, "r" );
    if ( f )
    {
        d.SetObject();
        char readBuffer[ 4096 ];
        rapidjson::FileReadStream is( f, readBuffer, sizeof( readBuffer ) );
        rapidjson::ParseResult pr = d.ParseStream( is );
        result = ( !pr.IsError() && (pr.Code() != rapidjson::kParseErrorDocumentEmpty) );
        fclose( f );
    }

    return result;
}


static void SaveJsonDocument( const char *file, const rapidjson::Document &d )
{
    FILE* f = fopen( file, "w" );
    if ( f )
    {
        char writeBuffer[ 4096 ];
        rapidjson::FileWriteStream os( f, writeBuffer, sizeof( writeBuffer ) );
        rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer( os );
        d.Accept( writer );
        fclose( f );
    }
}


static void ReadJsonValue_( const rapidjson::Value &src, const char* name, int8_t& dst)
{
    int32_t tmp = src[ name ].Get<int32_t>();
    dst = ( int8_t ) tmp;
}

static void ReadJsonValue_( const rapidjson::Value &src, const char* name, uint8_t& dst)
{
    uint32_t tmp = src[ name ].Get<uint32_t>();
    dst = ( uint8_t ) tmp;
}

static void ReadJsonValue_( const rapidjson::Value &src, const char *name, int16_t &dst )
{
    int32_t tmp = src[ name ].Get<int32_t>();
    dst = ( int16_t ) tmp;
}

static void ReadJsonValue_( const rapidjson::Value &src, const char *name, uint16_t &dst )
{
    uint32_t tmp = src[ name ].Get<uint32_t>();
    dst = ( uint16_t ) tmp;
}


template < typename T >
static void ReadJsonValue_( const rapidjson::Value &src, const char* name, T& dst)
{
    dst = src[ name ].Get<T>();
}


static void LoadCommonJson( const rapidjson::Value& v, ViewDataCommon& data )
{
#define ReadJsonValue( val, name, data ) if ( (val).HasMember( #name ) ) { ReadJsonValue_( (val), #name, (data).name ); }
#define ReadJsonValueTyped( val, name, data, type ) if ( (val).HasMember( #name ) ) { ReadJsonValue_( (val), #name, (type&)(data).name ); }

    using namespace rapidjson;
    ReadJsonValue( v, drawGpuZones, data );
    ReadJsonValue( v, drawZones, data );
    ReadJsonValue( v, drawLocks, data );
    ReadJsonValue( v, drawMergedLocks, data );
    ReadJsonValue( v, drawPlots, data );
    ReadJsonValue( v, onlyContendedLocks, data );
    ReadJsonValue( v, lockDrawFlags, data );
    ReadJsonValue( v, drawEmptyLabels, data );
    ReadJsonValue( v, drawFrameTargets, data );
    ReadJsonValue( v, drawContextSwitches, data );
    ReadJsonValue( v, darkenContextSwitches, data );
    ReadJsonValue( v, drawCpuData, data );
    ReadJsonValue( v, drawCpuUsageGraph, data );
    ReadJsonValue( v, drawSamples, data );
    ReadJsonValue( v, dynamicColors, data );
    ReadJsonValue( v, forceColors, data );
    ReadJsonValue( v, ghostZones, data );
    ReadJsonValue( v, plotHeight, data );
    ReadJsonValue( v, frameTarget, data );
    ReadJsonValue( v, flFrameHeight, data );
    ReadJsonValue( v, frameOverviewMaxTimeMS, data );
    ReadJsonValueTyped( v, shortenName, data, uint8_t );
    ReadJsonValue( v, stackCollapseMode, data );
    ReadJsonValue( v, stackCollapseClamp, data );
    ReadJsonValue( v, uiControlLoc, data );
    ReadJsonValue( v, coreCollapseMode, data );
    ReadJsonValue( v, coreCollapseClamp, data );
    ReadJsonValue( v, viewContextSwitchStack, data );
    ReadJsonValue( v, drawMousePosTime, data );
    ReadJsonValue( v, darkenOutsideExport, data );
    ReadJsonValue( v, autoZoneStats, data );

#undef ReadJsonValueTyped
#undef ReadJsonValue
}


static void SaveCommonJson( rapidjson::Document::AllocatorType& alloc, rapidjson::Value& v, const ViewDataCommon& data )
{
#define WriteJsonValue( val, name, data ) ( val ).AddMember( #name, Value( ( data ).name ), alloc )
#define WriteJsonValueTyped( val, name, data, type ) ( val ).AddMember( #name, Value( (const type&)( data ).name ), alloc )

    using namespace rapidjson;

    WriteJsonValue( v, drawGpuZones, data );
    WriteJsonValue( v, drawZones, data );
    WriteJsonValue( v, drawLocks, data );
    WriteJsonValue( v, drawMergedLocks, data );
    WriteJsonValue( v, drawPlots, data );
    WriteJsonValue( v, onlyContendedLocks, data );
    WriteJsonValue( v, lockDrawFlags, data );
    WriteJsonValue( v, drawEmptyLabels, data );
    WriteJsonValue( v, drawFrameTargets, data );
    WriteJsonValue( v, drawContextSwitches, data );
    WriteJsonValue( v, darkenContextSwitches, data );
    WriteJsonValue( v, drawCpuData, data );
    WriteJsonValue( v, drawCpuUsageGraph, data );
    WriteJsonValue( v, drawSamples, data );
    WriteJsonValue( v, dynamicColors, data );
    WriteJsonValue( v, forceColors, data );
    WriteJsonValue( v, ghostZones, data );
    WriteJsonValue( v, plotHeight, data );
    WriteJsonValue( v, frameTarget, data );
    WriteJsonValue( v, flFrameHeight, data );
    WriteJsonValue( v, frameOverviewMaxTimeMS, data );
    WriteJsonValueTyped( v, shortenName, data, uint8_t );
    WriteJsonValue( v, stackCollapseMode, data );
    WriteJsonValue( v, stackCollapseClamp, data );
    WriteJsonValue( v, uiControlLoc, data );
    WriteJsonValue( v, coreCollapseMode, data );
    WriteJsonValue( v, coreCollapseClamp, data );
    WriteJsonValue( v, viewContextSwitchStack, data );
    WriteJsonValue( v, drawMousePosTime, data );
    WriteJsonValue( v, darkenOutsideExport, data );
    WriteJsonValue( v, autoZoneStats, data );

#undef WriteJsonValueTyped
#undef WriteJsonValue
}


template < typename T, size_t N >
static void LoadTracks( T& trackMap, bool* ploaded, const rapidjson::Value& v, const char (&trackstr)[N] )
{
#define ReadJsonValue( val, name, data ) if ( (val).HasMember( #name ) ) { ReadJsonValue_( (val), #name, (data).name ); }

    using namespace rapidjson;

    using CvKeyType = T::key_type;
    using KeyType = std::remove_cv_t<std::remove_reference_t<CvKeyType>>;

    struct KeyConverter
    {
        static void FromString( uint64_t& dst, const char* src )
        {
            dst = strtoull( src, nullptr, 10);
        }

        static void FromString( std::string& dst, const char* src )
        {
            dst = src;
        }
    };

    bool loaded = false;
    if ( v.HasMember( trackstr ) )
    {
        const Value& vtracks = v[ trackstr ];
        for (Value::ConstMemberIterator it = vtracks.MemberBegin(); it != vtracks.MemberEnd(); ++it)
        {
            KeyType key;
            KeyConverter::FromString( key, it->name.GetString() );

            ViewData::Track track;
            track.visible = true;

            const Value &vtrack = it->value;
            if ( vtrack.GetType() == kObjectType )
            {
                ReadJsonValue( vtrack, priority, track );
                ReadJsonValue( vtrack, visible, track );

                const auto uiit = vtrack.FindMember( "trackUiSettings" );
                if ( uiit != vtrack.MemberEnd() )
                {
                    const Value &vui = uiit->value;
                    ReadJsonValue( vui, shouldOverride, track.ui );
                    ReadJsonValue( vui, stackCollapseClamp, track.ui );
                    ReadJsonValue( vui, stackCollapseMode, track.ui );
                }
                else
                {
                    ReadJsonValue( vtrack, shouldOverride, track.ui );
                    ReadJsonValue( vtrack, stackCollapseClamp, track.ui );
                    ReadJsonValue( vtrack, stackCollapseMode, track.ui );
                }
            }
            else
            {
                track.priority = vtrack.GetInt();
            }

            trackMap[ key ] = track;
            loaded = true;
        }
    }

    if ( ploaded )
    {
        *ploaded = loaded;
    }

#undef WriteJsonValue
}


template < typename T, size_t N >
static void SaveTracks( rapidjson::Document::AllocatorType& alloc, rapidjson::Value& v, const T& trackMap, const char (&trackstr)[N] )
{
#define WriteJsonValue( val, name, data ) (val).AddMember( #name, Value( (data).name ), alloc )

    using namespace rapidjson;

    using CvKeyType = T::key_type;
    using KeyType = std::remove_cv_t<std::remove_reference_t<CvKeyType>>;

    struct KeyConverter
    {
        static void ToString( const uint64_t src, std::string& dst )
        {
            char buffer[ 32 ];
            int written = sprintf( buffer, "%llu", src );
            assert( written >= 0 );
            dst = std::string( buffer, ( size_t ) written );
        }

        static void ToString( const std::string& src, std::string& dst )
        {
            dst = src;
        }
    };

    if ( !trackMap.empty() )
    {
        v.AddMember( trackstr, Value( kObjectType ), alloc );
        Value& vtracks = v[ trackstr ];
        for ( auto& it : trackMap )
        {
            const ViewData::Track &track = it.second;

            std::string name;
            KeyConverter::ToString( it.first, name );
            vtracks.AddMember( Value( kStringType ).Set( name, alloc), Value(kObjectType), alloc);
            Value &vtrack = vtracks[ name ];
            WriteJsonValue( vtrack, priority, track );
            WriteJsonValue( vtrack, visible, track );

            vtrack.AddMember( "trackUiSettings", Value( kObjectType ), alloc );
            Value &vui = vtrack[ "trackUiSettings" ];
            WriteJsonValue( vui, shouldOverride, track.ui );
            WriteJsonValue( vui, stackCollapseClamp, track.ui );
            WriteJsonValue( vui, stackCollapseMode, track.ui );
        }
    }
#undef WriteJsonValue
}


template < typename T, size_t N >
static void LoadPlots( T& plotMap, bool* ploaded, const rapidjson::Value& v, const char (&plotstr)[N] )
{
#define ReadJsonValue( val, name, data ) if ( (val).HasMember( #name ) ) { ReadJsonValue_( (val), #name, (data).name ); }

    using namespace rapidjson;

    bool loaded = false;
    if ( v.HasMember( plotstr ) )
    {
        const Value& vplots = v[ plotstr ];
        for (Value::ConstMemberIterator it = vplots.MemberBegin(); it != vplots.MemberEnd(); ++it)
        {
            const Value &vplot = it->value;
            if ( vplot.GetType() == kObjectType )
            {
                ViewData::Plot plot;
                ReadJsonValue( vplot, visible, plot );
                plotMap[ it->name.GetString() ] = plot;
                loaded = true;
            }
        }
    }

    if ( ploaded )
    {
        *ploaded = loaded;
    }

#undef WriteJsonValue
}


template < typename T, size_t N >
static void SavePlots( rapidjson::Document::AllocatorType& alloc, rapidjson::Value& v, const T& plotMap, const char (&plotstr)[N] )
{
#define WriteJsonValue( val, name, data ) (val).AddMember( #name, Value( (data).name ), alloc )

    using namespace rapidjson;

    if ( !plotMap.empty() )
    {
        v.AddMember( plotstr, Value( kObjectType ), alloc );
        Value &vplots = v[ plotstr ];
        for ( auto &it : plotMap )
        {
            const ViewData::Plot& plot = it.second;
            vplots.AddMember( Value( kStringType ).SetString( it.first, alloc ), Value( kObjectType ), alloc );
            Value &vplot = vplots[ it.first ];
            WriteJsonValue( vplot, visible, plot );
        }
    }

#undef WriteJsonValue
}


void SyncViewSettings( ViewData& vd, Worker& worker )
{
    {
        const Vector<ThreadData*>& threadData = worker.GetThreadData();

        std::unordered_map< const char*, size_t > uniqueThreadNames;
        for ( const ThreadData* td : threadData )
        {
            const char* pname = worker.GetThreadName( td->id );
            if ( pname && (strcmp( pname, "???" ) != 0) )
            {
                uniqueThreadNames[ pname ]++;
            }
        }

        for ( auto it = uniqueThreadNames.begin(), end = uniqueThreadNames.end(); it != end; )
        {
            if ( it->second > 1 )
            {
                it = uniqueThreadNames.erase( it );
            }
            else
            {
                ++it;
            }
        }

        for ( const ThreadData* td : threadData )
        {
            ViewData::Track defaultlts = {0};
            defaultlts.visible = true;
            auto localit = vd.threads.emplace( td->id, defaultlts );
            if ( localit.second )
            {
                vd.threadsChanged = true;
            }

            ViewData::Track& lts = localit.first->second;

            const bool manualChange = ( ( lts.flags & ViewData::Flags_Manual ) != 0 );
            const bool appliedGlobal = ( ( lts.flags & ViewData::Flags_AppliedGlobal ) != 0 );

            const char* pname = worker.GetThreadName( td->id );
            if ( pname && (strcmp( pname, "???" ) != 0) )
            {
                const std::string name( pname );
                if ( (!worker.IsDataStatic() || localit.second) &&
                     !manualChange &&
                     !appliedGlobal )
                {
                    // NOTE: if a name is not unique, we don't try to get the data we have in
                    // our global settings
                    if ( uniqueThreadNames.contains( pname ) )
                    {
                        auto it = vd.globalThreads.find( name );
                        if ( it != vd.globalThreads.end() )
                        {
                            lts.ui = it->second.ui;
                            lts.priority = it->second.priority;
                            lts.visible = it->second.visible;
                        }
                    }
                    lts.flags |= ViewData::Flags_AppliedGlobal;
                }

                ViewData::Track gts;
                gts.ui = lts.ui;
                gts.priority = lts.priority;
                gts.visible = lts.visible;
                vd.globalThreads[ name ] = gts;
            }
        }
    }

    {
        const CpuData *cpuData = worker.GetCpuData();
        const int cpuDataCount = worker.GetCpuDataCpuCount();
        if ( cpuData )
        {
            for ( int i = 0; i < cpuDataCount; i++ )
            {
                const CpuData& cd = cpuData[ i ];

                ViewData::Track defaultlts = {0};
                defaultlts.visible = true;
                auto localit = vd.cores.emplace( i, defaultlts );
                if ( localit.second )
                {
                    vd.coresChanged = true;
                }
            }
        }
    }

    for ( const PlotData* pd : worker.GetPlots() )
    {
        if ( ( pd->type == PlotType::User ) || ( pd->type == PlotType::SysTime ) )
        {
            const char* pname =   (pd->type == PlotType::SysTime)
                                ? "__SysTime_CPU_usage__"
                                : ( pd->name.active ? worker.GetString( pd->name ) : nullptr );
            if ( pname && (strcmp( pname, "???" ) != 0) )
            {
                const std::string name( pname );
                ViewData::Plot defaultlps = {0};
                if ( name == std::string_view( "GPU time (ms)" ) )
                {
                    defaultlps.visible = true;
                }

                auto localit = vd.plots.emplace( name, defaultlps );
                if ( localit.second )
                {
                    vd.plotsChanged = true;
                }

                ViewData::Plot& lps = localit.first->second;

                const bool manualChange = ( ( lps.flags & ViewData::Flags_Manual ) != 0 );
                const bool appliedGlobal = ( ( lps.flags & ViewData::Flags_AppliedGlobal ) != 0 );
                if ( !worker.IsDataStatic() &&
                     !manualChange &&
                     !appliedGlobal )
                {
                    auto it = vd.globalPlots.find( name );
                    if ( it != vd.globalPlots.end() )
                    {
                        lps.visible = it->second.visible;
                    }
                    lps.flags |= ViewData::Flags_AppliedGlobal;
                }

                ViewData::Plot gps;
                gps.visible = lps.visible;
                vd.globalPlots[ name ] = gps;
            }
        }
    }
}

constexpr auto FileDescription = "description";
constexpr auto FileTimeline = "timeline";
constexpr auto FileOptions = "options";
constexpr auto FileAnnotations = "annotations";
constexpr auto FileSourceSubstitutions = "srcsub";
constexpr auto FileGlobalOptionsJson = "globaloptions.json";
constexpr auto FileOptionsJson = "options.json";
constexpr auto FileZonePlotsJson = "zoneplots.json";


enum : uint32_t { VersionTimeline = 0 };
enum : uint32_t { VersionOptions = 7 };
enum : uint32_t { VersionAnnotations = 0 };
enum : uint32_t { VersionSourceSubstitutions = 0 };

UserData::UserData()
    : m_preserveState( false )
    , m_time( 0 )
{
}

UserData::UserData( const char* program, uint64_t time )
    : m_program( program )
    , m_time( time )
{
    if( m_program.empty() ) m_program = "_";

    FILE* f = OpenFile( FileDescription, false );
    if( f )
    {
        fseek( f, 0, SEEK_END );
        const auto sz = ftell( f );
        fseek( f, 0, SEEK_SET );
        auto buf = std::unique_ptr<char[]>( new char[sz] );
        fread( buf.get(), 1, sz, f );
        fclose( f );
        m_description.assign( buf.get(), buf.get() + sz );
    }
}

void UserData::Init( const char* program, uint64_t time )
{
    assert( !Valid() );
    m_program = program;
    m_time = time;

    if( m_program.empty() ) m_program = "_";
}

bool UserData::SetDescription( const char* description )
{
    assert( Valid() );

    m_description = description;
    const auto sz = m_description.size();

    FILE* f = OpenFile( FileDescription, true );
    if( !f ) return false;

    fwrite( description, 1, sz, f );
    fclose( f );
    return true;
}


void UserData::LoadStateJson( ViewData &data )
{
#define ReadJsonValue( val, name, data )                        \
            if ( (val).HasMember( #name ) )                     \
            {                                                   \
                ReadJsonValue_( (val), #name, (data).name );    \
            }

    using namespace rapidjson;

    Document d;
    if ( LoadJsonDocument( GetSavePath( FileGlobalOptionsJson ), d ) )
    {
        // Load legacy members
        LoadTracks( data.globalThreads, nullptr, d, "ThreadOrder" );
        LoadPlots( data.globalPlots, nullptr, d, "PlotSettings" );

        if ( d.HasMember( "viewData" ) )
        {
            const Value& rViewData = d[ "viewData" ];
            LoadCommonJson( rViewData, data );
            LoadTracks( data.globalThreads, nullptr, rViewData, "Threads" );
            LoadPlots( data.globalPlots, nullptr, rViewData, "Plots" );
        }
    }

    if ( Valid() && LoadJsonDocument( GetSavePath( m_program.c_str(), m_time, FileOptionsJson, false ), d ) )
    {
        if ( d.HasMember( "viewData" ) )
        {
            const Value& rViewData = d[ "viewData" ];
            ReadJsonValue( rViewData, zvStart, data);
            ReadJsonValue( rViewData, zvEnd, data);
            ReadJsonValue( rViewData, frameScale, data);
            ReadJsonValue( rViewData, frameStart, data);

            LoadCommonJson( rViewData, data );

            // Read the legacy file format. We can remove this once we are sure that we don't have any old options left.
            LoadTracks( data.threads, &data.threadsChanged, rViewData, "perThreadSettings" );
            LoadTracks( data.cores, &data.coresChanged, rViewData, "perCoreSettings" );
            LoadPlots( data.plots, &data.plotsChanged, rViewData, "PlotSettings" );

            LoadTracks( data.threads, &data.threadsChanged, rViewData, "Threads" );
            LoadTracks( data.cores, &data.coresChanged, rViewData, "Cores" );
            LoadPlots( data.plots, &data.plotsChanged, rViewData, "Plots" );
        }
    }

#undef ReadJsonValue
}


void UserData::SaveStateJson( const ViewData &data, bool global )
{
#define WriteJsonValue( val, name, data ) (val).AddMember( #name, Value( (data).name ),  alloc )

    using namespace rapidjson;

    if ( global )
    {
        Document d;
        d.SetObject();

        rapidjson::Document::AllocatorType& alloc = d.GetAllocator();

        d.AddMember( "viewData", Value( kObjectType ), alloc );
        Value &viewData = d[ "viewData" ];
        SaveCommonJson( alloc, viewData, data );
        SaveTracks( alloc, viewData, data.globalThreads, "Threads" );
        SavePlots( alloc, viewData, data.globalPlots, "Plots" );

        SaveJsonDocument( GetSavePath( FileGlobalOptionsJson ), d );
    }
    else
    {
        if( !m_preserveState ) return;
        assert( Valid() );

        Document d;
        d.SetObject();

        rapidjson::Document::AllocatorType& alloc = d.GetAllocator();

        d.AddMember( "viewData", Value( kObjectType ), alloc );
        Value &viewData = d[ "viewData" ];
        WriteJsonValue( viewData, zvStart, data);
        WriteJsonValue( viewData, zvEnd, data);
        WriteJsonValue( viewData, frameScale, data);
        WriteJsonValue( viewData, frameStart, data);

        SaveCommonJson( alloc, viewData, data );
        SaveTracks( alloc, viewData, data.threads, "Threads" );
        SaveTracks( alloc, viewData, data.cores, "Cores" );
        SavePlots( alloc, viewData, data.plots, "Plots" );

        SaveJsonDocument( GetSavePath( m_program.c_str(), m_time, FileOptionsJson, true ), d );
    }

#undef WriteJsonValue
}


void UserData::LoadState( ViewData& data )
{
    assert( Valid() );
    FILE* f = OpenFile( FileTimeline, false );
    if( f )
    {
        uint32_t ver;
        fread( &ver, 1, sizeof( ver ), f );
        if( ver == VersionTimeline )
        {
            fread( &data.zvStart, 1, sizeof( data.zvStart ), f );
            fread( &data.zvEnd, 1, sizeof( data.zvEnd ), f );
            //fread( &data.zvHeight, 1, sizeof( data.zvHeight ), f );
            fseek( f, sizeof( float ), SEEK_CUR );
            //fread( &data.zvScroll, 1, sizeof( data.zvScroll ), f );
            fseek( f, sizeof( float ), SEEK_CUR );
            fread( &data.frameScale, 1, sizeof( data.frameScale ), f );
            fread( &data.frameStart, 1, sizeof( data.frameStart ), f );
        }
        fclose( f );
    }

    f = OpenFile( FileOptions, false );
    if( f )
    {
        uint32_t ver;
        fread( &ver, 1, sizeof( ver ), f );
        // TODO: remove in future
        if( ver == VersionOptions )
        {
            fread( &data.drawGpuZones, 1, sizeof( data.drawGpuZones ), f );
            fread( &data.drawZones, 1, sizeof( data.drawZones ), f );
            fread( &data.drawLocks, 1, sizeof( data.drawLocks ), f );
            fread( &data.drawPlots, 1, sizeof( data.drawPlots ), f );
            fread( &data.onlyContendedLocks, 1, sizeof( data.onlyContendedLocks ), f );
            fread( &data.drawEmptyLabels, 1, sizeof( data.drawEmptyLabels ), f );
            fread( &data.drawFrameTargets, 1, sizeof( data.drawFrameTargets ), f );
            fread( &data.drawContextSwitches, 1, sizeof( data.drawContextSwitches ), f );
            fread( &data.darkenContextSwitches, 1, sizeof( data.darkenContextSwitches ), f );
            fread( &data.drawCpuData, 1, sizeof( data.drawCpuData ), f );
            fread( &data.drawCpuUsageGraph, 1, sizeof( data.drawCpuUsageGraph ), f );
            fread( &data.drawSamples, 1, sizeof( data.drawSamples ), f );
            fread( &data.dynamicColors, 1, sizeof( data.dynamicColors ), f );
            fread( &data.forceColors, 1, sizeof( data.forceColors ), f );
            fread( &data.ghostZones, 1, sizeof( data.ghostZones ), f );
            fread( &data.frameTarget, 1, sizeof( data.frameTarget ), f );
            fclose( f );
        }
        else
        {
        	fclose( f );
            const auto path = GetSavePath( m_program.c_str(), m_time, FileOptions, false );
            assert( path );
            auto ini = ini_load( path );
            if( ini )
            {
                int v;
                if( ini_sget( ini, "options", "drawGpuZones", "%d", &v ) ) data.drawGpuZones = v;
                if( ini_sget( ini, "options", "drawZones", "%d", &v ) ) data.drawZones = v;
                if( ini_sget( ini, "options", "drawLocks", "%d", &v ) ) data.drawLocks = v;
                if( ini_sget( ini, "options", "drawPlots", "%d", &v ) ) data.drawPlots = v;
                if( ini_sget( ini, "options", "onlyContendedLocks", "%d", &v ) ) data.onlyContendedLocks = v;
                if( ini_sget( ini, "options", "drawEmptyLabels", "%d", &v ) ) data.drawEmptyLabels = v;
                if( ini_sget( ini, "options", "drawFrameTargets", "%d", &v ) ) data.drawFrameTargets = v;
                if( ini_sget( ini, "options", "drawContextSwitches", "%d", &v ) ) data.drawContextSwitches = v;
                if( ini_sget( ini, "options", "darkenContextSwitches", "%d", &v ) ) data.darkenContextSwitches = v;
                if( ini_sget( ini, "options", "drawCpuData", "%d", &v ) ) data.drawCpuData = v;
                if( ini_sget( ini, "options", "drawCpuUsageGraph", "%d", &v ) ) data.drawCpuUsageGraph = v;
                if( ini_sget( ini, "options", "drawSamples", "%d", &v ) ) data.drawSamples = v;
                if( ini_sget( ini, "options", "dynamicColors", "%d", &v ) ) data.dynamicColors = v;
                if( ini_sget( ini, "options", "inheritParentColors", "%d", &v ) ) data.inheritParentColors = v;
                if( ini_sget( ini, "options", "forceColors", "%d", &v ) ) data.forceColors = v;
                if( ini_sget( ini, "options", "ghostZones", "%d", &v ) ) data.ghostZones = v;
                if( ini_sget( ini, "options", "frameTarget", "%d", &v ) ) data.frameTarget = v;
                if( ini_sget( ini, "options", "shortenName", "%d", &v ) ) data.shortenName = (ShortenName)v;
                if( ini_sget( ini, "options", "plotHeight", "%d", &v ) ) data.plotHeight = v;
                ini_free( ini );
            }
        }
    }


}

void UserData::SaveState( const ViewData& data )
{
    if( !m_preserveState ) return;
    assert( Valid() );
    FILE* f = OpenFile( FileTimeline, true );
    if( f )
    {
        uint32_t ver = VersionTimeline;
        fwrite( &ver, 1, sizeof( ver ), f );
        fwrite( &data.zvStart, 1, sizeof( data.zvStart ), f );
        fwrite( &data.zvEnd, 1, sizeof( data.zvEnd ), f );
        //fwrite( &data.zvHeight, 1, sizeof( data.zvHeight ), f );
        float zero = 0;
        fwrite( &zero, 1, sizeof( zero ), f );
        //fwrite( &data.zvScroll, 1, sizeof( data.zvScroll ), f );
        fwrite( &zero, 1, sizeof( zero ), f );
        fwrite( &data.frameScale, 1, sizeof( data.frameScale ), f );
        fwrite( &data.frameStart, 1, sizeof( data.frameStart ), f );
        fclose( f );
    }

    f = OpenFile( FileOptions, true );
    if( f )
    {
        fprintf( f, "[options]\n" );
        fprintf( f, "drawGpuZones = %d\n", data.drawGpuZones );
        fprintf( f, "drawZones = %d\n", data.drawZones );
        fprintf( f, "drawLocks = %d\n", data.drawLocks );
        fprintf( f, "drawPlots = %d\n", data.drawPlots );
        fprintf( f, "onlyContendedLocks = %d\n", data.onlyContendedLocks );
        fprintf( f, "drawEmptyLabels = %d\n", data.drawEmptyLabels );
        fprintf( f, "drawFrameTargets = %d\n", data.drawFrameTargets );
        fprintf( f, "drawContextSwitches = %d\n", data.drawContextSwitches );
        fprintf( f, "darkenContextSwitches = %d\n", data.darkenContextSwitches );
        fprintf( f, "drawCpuData = %d\n", data.drawCpuData );
        fprintf( f, "drawCpuUsageGraph = %d\n", data.drawCpuUsageGraph );
        fprintf( f, "drawSamples = %d\n", data.drawSamples );
        fprintf( f, "dynamicColors = %d\n", data.dynamicColors );
        fprintf( f, "inheritParentColors = %d\n", data.inheritParentColors );
        fprintf( f, "forceColors = %d\n", data.forceColors );
        fprintf( f, "ghostZones = %d\n", data.ghostZones );
        fprintf( f, "frameTarget = %d\n", data.frameTarget );
        fprintf( f, "shortenName = %d\n", (int)data.shortenName );
        fprintf( f, "plotHeight = %d\n", data.plotHeight );
        fclose( f );
    }
}

void UserData::StateShouldBePreserved()
{
    m_preserveState = true;
}

void UserData::LoadAnnotations( std::vector<std::unique_ptr<Annotation>>& data )
{
    assert( Valid() );
    FILE* f = OpenFile( FileAnnotations, false );
    if( f )
    {
        uint32_t ver;
        fread( &ver, 1, sizeof( ver ), f );
        if( ver == VersionAnnotations )
        {
            uint32_t sz;
            fread( &sz, 1, sizeof( sz ), f );
            for( uint32_t i=0; i<sz; i++ )
            {
                auto ann = std::make_unique<Annotation>();

                uint32_t tsz;
                fread( &tsz, 1, sizeof( tsz ), f );
                if( tsz != 0 )
                {
                    char buf[1024];
                    assert( tsz < 1024 );
                    fread( buf, 1, tsz, f );
                    ann->text.assign( buf, tsz );
                }
                fread( &ann->range.min, 1, sizeof( ann->range.min ), f );
                fread( &ann->range.max, 1, sizeof( ann->range.max ), f );
                fread( &ann->color, 1, sizeof( ann->color ), f );
                ann->range.active = true;

                data.emplace_back( std::move( ann ) );
            }
        }
        fclose( f );
    }
}

void UserData::SaveAnnotations( const std::vector<std::unique_ptr<Annotation>>& data )
{
    if( !m_preserveState ) return;
    if( data.empty() )
    {
        Remove( FileAnnotations );
        return;
    }
    assert( Valid() );
    FILE* f = OpenFile( FileAnnotations, true );
    if( f )
    {
        uint32_t ver = VersionAnnotations;
        fwrite( &ver, 1, sizeof( ver ), f );
        uint32_t sz = uint32_t( data.size() );
        fwrite( &sz, 1, sizeof( sz ), f );
        for( auto& ann : data )
        {
            sz = uint32_t( ann->text.size() );
            fwrite( &sz, 1, sizeof( sz ), f );
            if( sz != 0 )
            {
                fwrite( ann->text.c_str(), 1, sz, f );
            }
            fwrite( &ann->range.min, 1, sizeof( ann->range.min ), f );
            fwrite( &ann->range.max, 1, sizeof( ann->range.max ), f );
            fwrite( &ann->color, 1, sizeof( ann->color ), f );
        }
        fclose( f );
    }
}

bool UserData::LoadSourceSubstitutions( std::vector<SourceRegex>& data )
{
    assert( Valid() );
    bool regexValid = true;
    FILE* f = OpenFile( FileSourceSubstitutions, false );
    if( f )
    {
        uint32_t ver;
        fread( &ver, 1, sizeof( ver ), f );
        if( ver == VersionSourceSubstitutions )
        {
            uint32_t sz;
            fread( &sz, 1, sizeof( sz ), f );
            for( uint32_t i=0; i<sz; i++ )
            {
                std::string pattern, target;
                uint32_t tsz;
                fread( &tsz, 1, sizeof( tsz ), f );
                if( tsz != 0 )
                {
                    char buf[1024];
                    assert( tsz < 1024 );
                    fread( buf, 1, tsz, f );
                    pattern.assign( buf, tsz );
                }
                fread( &tsz, 1, sizeof( tsz ), f );
                if( tsz != 0 )
                {
                    char buf[1024];
                    assert( tsz < 1024 );
                    fread( buf, 1, tsz, f );
                    target.assign( buf, tsz );
                }
                std::regex regex;
                try
                {
                    regex.assign( pattern );
                }
                catch( std::regex_error& )
                {
                    regexValid = false;
                }
                data.emplace_back( SourceRegex { std::move( pattern ), std::move( target ), std::move( regex ) } );
            }
        }
        fclose( f );
    }
    return regexValid;
}

void UserData::SaveSourceSubstitutions( const std::vector<SourceRegex>& data )
{
    if( !m_preserveState ) return;
    if( data.empty() )
    {
        Remove( FileSourceSubstitutions );
        return;
    }
    assert( Valid() );
    FILE* f = OpenFile( FileSourceSubstitutions, true );
    if( f )
    {
        uint32_t ver = VersionSourceSubstitutions;
        fwrite( &ver, 1, sizeof( ver ), f );
        uint32_t sz = uint32_t( data.size() );
        fwrite( &sz, 1, sizeof( sz ), f );
        for( auto& v : data )
        {
            sz = uint32_t( v.pattern.size() );
            fwrite( &sz, 1, sizeof( sz ), f );
            if( sz != 0 )
            {
                fwrite( v.pattern.c_str(), 1, sz, f );
            }
            sz = uint32_t( v.target.size() );
            fwrite( &sz, 1, sizeof( sz ), f );
            if( sz != 0 )
            {
                fwrite( v.target.c_str(), 1, sz, f );
            }
        }
        fclose( f );
    }
}

void UserData::LoadZonePlotsJson( Worker &worker )
{
#ifndef TRACY_NO_STATISTICS
    using namespace rapidjson;

    FILE *f = OpenFile( FileZonePlotsJson, false );
    if ( f )
    {
        char readBuffer[ 4096 ];
        FileReadStream is( f, readBuffer, sizeof( readBuffer ) );
        Document d;
        d.ParseStream( is );

        if ( !d.HasParseError() && d.HasMember( "plots" ) )
        {
            const Value &zonePlots = d[ "plots" ];

            assert( zonePlots.IsArray() );
            int nZonePlots = zonePlots.Size();

            ZonePlotDef zpd;
            for ( int i = 0; i < nZonePlots; ++i )
            {
                const Value &zonePlot = zonePlots[ i ];
                assert( zonePlot.IsArray() );
                int nComponentPlots = zonePlot.Size();
                PlotData *pPrevPlot = nullptr;
                for ( int j = 0; j < nComponentPlots; ++j )
                {
                    memset( &zpd, 0, sizeof( zpd ) );
                    const Value &nextPlot = zonePlot[ j ];
                    
					if ( nextPlot.HasMember( "srcloc" ) ) ReadJsonValue_( nextPlot, "srcloc", zpd.srcloc );
					if ( nextPlot.HasMember( "filterType" ) ) ReadJsonValue_( nextPlot, "filterType", *((uint8_t*)&zpd.filterType) );
					if ( nextPlot.HasMember( "filterId" ) ) ReadJsonValue_( nextPlot, "filterId", zpd.filterId );
					if ( nextPlot.HasMember( "frameSum" ) ) ReadJsonValue_( nextPlot, "frameSum", zpd.aggregatePerFrame );

					uint8_t drawType = ( uint8_t ) ( zpd.aggregatePerFrame ? PlotDrawType::Step : PlotDrawType::Line ); // default value if 'drawType' missing from json file
					if ( nextPlot.HasMember( "drawType" ) ) ReadJsonValue_( nextPlot, "drawType", drawType );
					
					// backward compatibility
					if ( nextPlot.HasMember( "name" ) && nextPlot.HasMember( "nameOnly" ) )
                    {
                        const Value &jsonBaseName = nextPlot[ "name" ];
						StringRef basename;
                        if ( jsonBaseName.HasMember( "data" ) ) ReadJsonValue_( jsonBaseName, "data", basename.__data );
                        if ( jsonBaseName.HasMember( "str" ) ) ReadJsonValue_( jsonBaseName, "str", basename.str );

						bool specificZoneName;
						ReadJsonValue_( nextPlot, "nameOnly", specificZoneName );

						if ( specificZoneName && basename.isidx )
						{
							zpd.filterType = PlotFilterType::ZoneName;
							zpd.filterId = basename.str;
						}
                    }

					pPrevPlot = worker.CreatePlotForSourceLocation( zpd.srcloc, zpd.filterType, zpd.filterId, zpd.aggregatePerFrame, ( PlotDrawType ) drawType, pPrevPlot);
                }
            }
        }

        fclose( f );
    }
#endif
}

void UserData::SaveZonePlotsJson( Worker &worker, const Vector<PlotData *> &plots )
{
#ifndef TRACY_NO_STATISTICS
    using namespace rapidjson;

    if ( !m_preserveState ) return;

    int nZonePlots = 0;
    for ( int i = 0; i < plots.size(); ++i )
    {
        if ( plots[ i ]->type == PlotType::Zone )
        {
            nZonePlots++;
        }
    }

    if ( nZonePlots > 0 )
    {
        Document d;
        d.SetObject();

        d.AddMember( "plots", Value( kArrayType ), d.GetAllocator() );
        Value &zonePlots = d[ "plots" ];
        zonePlots.Reserve( nZonePlots, d.GetAllocator() );

        int iOut = 0;
        for ( int i = 0; i < plots.size(); ++i )
        {
            if ( plots[ i ]->type == PlotType::Zone )
            {
                zonePlots.PushBack( Value( kArrayType ), d.GetAllocator() );
                Value &componentPlots = zonePlots[ zonePlots.Size() - 1 ];
                PlotData *pNext = plots[ i ];

                while ( pNext )
                {
                    ZonePlotDef *pZpd = pNext->zonePlotDef;
                    assert( pNext->type == PlotType::Zone && pZpd != nullptr );
                    componentPlots.PushBack( Value( kObjectType ), d.GetAllocator() );
                    Value &nextPlot = componentPlots[ componentPlots.Size() - 1 ];

					std::string debugPlotName = worker.GetString( pNext->name );
					nextPlot.AddMember( "name__DEBUG__", debugPlotName, d.GetAllocator() );
                    nextPlot.AddMember( "srcloc", Value( pZpd->srcloc ), d.GetAllocator() );
					nextPlot.AddMember( "filterType", Value( ( uint8_t ) pZpd->filterType ), d.GetAllocator() );
					nextPlot.AddMember( "filterId", Value( pZpd->filterId ), d.GetAllocator() );
                    nextPlot.AddMember( "frameSum", Value( pZpd->aggregatePerFrame ), d.GetAllocator() );
					nextPlot.AddMember( "drawType", Value( (uint8_t)pNext->drawType ), d.GetAllocator() );
                    pNext = pNext->nextPlot;
                }
                iOut++;
            }
        }
        FILE *f = OpenFile( FileZonePlotsJson, true );
        if ( f )
        {
            char writeBuffer[ 4096 ];
            FileWriteStream os( f, writeBuffer, sizeof( writeBuffer ) );
            PrettyWriter<FileWriteStream> writer( os );
            d.Accept( writer );
            fclose( f );
        }
    }
    else
    {
        Remove( FileZonePlotsJson );
    }
#endif
}

FILE* UserData::OpenFile( const char* filename, bool write )
{
    const auto path = GetSavePath( m_program.c_str(), m_time, filename, write );
    if( !path ) return nullptr;
    FILE* f = fopen( path, write ? "wb" : "rb" );
    return f;
}

void UserData::Remove( const char* filename )
{
    const auto path = GetSavePath( m_program.c_str(), m_time, filename, false );
    if( !path ) return;
    unlink( path );
}

const char* UserData::GetConfigLocation() const
{
    assert( Valid() );
    return GetSavePath( m_program.c_str(), m_time, nullptr, false );
}

}
