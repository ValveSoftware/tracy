#ifndef __TRACYVIEWDATA_HPP__
#define __TRACYVIEWDATA_HPP__

#include <stdint.h>
#include <regex>
#include <unordered_map>
#include <string>

#include "TracyUtility.hpp"

namespace tracy
{

struct ZoneEvent;

struct Range
{
    void StartFrame() { hiMin = hiMax = false; }

    int64_t min = 0;
    int64_t max = 0;
    bool active = false;
    bool hiMin = false;
    bool hiMax = false;
    bool modMin = false;
    bool modMax = false;
};

struct RangeSlim
{
    bool operator==( const Range& other ) const { return other.active == active && other.min == min && other.max == max; }
    bool operator!=( const Range& other ) const { return !(*this == other); }
    void operator=( const Range& other ) { active = other.active; min = other.min; max = other.max; }

    int64_t min, max;
    bool active = false;
	const ZoneEvent *pZone = nullptr;
};

const float MIN_FRAMES_HEIGHT=50.0f;
const float MAX_FRAMES_HEIGHT=500.0f;


struct ViewDataCommon
{
    bool operator==( const ViewDataCommon &rhs ) const = default;
    bool operator!=( const ViewDataCommon &rhs ) const = default;

    enum EThreadUiControlLoc { UiCtrlLocLeft, UiCtrlLocRight, UiCtrlLocHidden };
    static inline const char *const ppszUiControlLoc[ 3 ] = { "Left", "Right", "Hidden" };

    enum EThreadStackCollapse { CollapseDynamic = 0, CollapseMax = 1, CollapseLimit = 2 };
    enum EPlotViz { Disable = 0, Top, Bottom };
    static inline const char *const ppszPlotViz[ 3 ] = { "Disable Plots", "Plots Above Zones", "Plots Below Zone" };

    enum ELockDrawVisFlags { None = 0, Contended = (1 << 0), Uncontended = ( 1 << 1 ), SingleThread = ( 1 << 2 ), SingleTerminated = ( 1 << 3 ) };

    enum class EHwCounterDrawMode : uint8_t { BarGraph_Count = 0, BarGraph_Rate, TextMode, DrawModeCount };

    uint8_t drawGpuZones = true;
    uint8_t drawZones = true;
    uint8_t drawLocks = true;
    uint8_t drawMergedLocks = true;
    uint8_t keepSingleThreadLocks = false;
    uint8_t drawPlots = EPlotViz::Top;
    uint8_t onlyContendedLocks = true;
    uint8_t lockDrawFlags = ELockDrawVisFlags::Contended;
    uint8_t drawEmptyLabels = false;
    uint8_t drawFrameTargets = false;
    uint8_t drawContextSwitches = true;
    uint8_t darkenContextSwitches = true;
    uint8_t drawCpuData = true;
    uint8_t drawCpuUsageGraph = true;
    uint8_t drawSamples = true;
    uint8_t dynamicColors = 1;
    uint8_t inheritParentColors = true;
    uint8_t forceColors = false;
    uint8_t ghostZones = true;

    uint32_t plotHeight = 100;

    uint32_t frameTarget = 60;
    float flFrameHeight = MIN_FRAMES_HEIGHT;
    int32_t frameOverviewMaxTimeMS = 15;

    ShortenName shortenName = ShortenName::NoSpaceAndNormalize;
    uint8_t stackCollapseMode = CollapseDynamic;
    int32_t stackCollapseClamp = 0;
    uint8_t uiControlLoc = UiCtrlLocRight;

    uint8_t coreCollapseMode = CollapseDynamic;
    int32_t coreCollapseClamp = 0;

    uint8_t viewContextSwitchStack = false;
    uint8_t drawMousePosTime = false;
    uint8_t darkenOutsideExport = true;

    bool autoZoneStats = false;

    uint8_t drawHwCounter = true;
    uint8_t hwCounterYAxisSameScale = true;
    EHwCounterDrawMode hwCounterDrawMode = EHwCounterDrawMode::BarGraph_Count;
    uint32_t hwCounterRateTarget = 100;   //  target # / microsecond
};


struct TrackUiSettings
{
    bool operator==( const TrackUiSettings &rhs ) const = default;
    bool operator!=( const TrackUiSettings &rhs ) const = default;

    bool shouldOverride = false;
    uint8_t stackCollapseMode = 0;
    int32_t stackCollapseClamp = 0;
};


struct ViewData : public ViewDataCommon
{
    enum Flags
    {
        Flags_None = 0,
        Flags_Manual = ( 1 << 0 ),
        Flags_AppliedGlobal = ( 1 << 1 ),
    };

    struct Track
    {
        bool operator==( const Track &rhs ) const
        {
            const bool result = (
                ui == rhs.ui &&
                priority == rhs.priority && 
                visible == rhs.visible );

            return result;
        }

        bool operator!=( const Track &rhs ) const
        {
            return !( *this == rhs );
        }

        TrackUiSettings ui;
        int32_t priority = INT32_MAX;
        bool visible = false;
        uint8_t flags = 0;
    };

    struct Plot
    {
        bool operator==( const Plot &rhs ) const
        {
            const bool result = ( visible == rhs.visible );
            return result;
        }

        bool operator!=( const Plot &rhs ) const
        {
            return !( *this == rhs );
        }

        bool visible = false;
        uint8_t flags = 0;
    };

    bool operator==( const ViewData &rhs ) const
    {
        const bool result = (
            (ViewDataCommon::operator==(rhs)) &&
            zvStart == rhs.zvStart &&
            zvEnd == rhs.zvEnd &&
            frameScale == rhs.frameScale &&
            frameStart == rhs.frameStart &&
            threads == rhs.threads && cores == rhs.cores &&
            plots == rhs.plots );
        return result;
    }

    bool operator!=( const ViewData &rhs ) const
    {
        return !( *this == rhs );
    }

    int64_t zvStart = 0;
    int64_t zvEnd = 0;
    int32_t frameScale = 0;
    int32_t frameStart = 0;

    std::unordered_map<uint64_t, Track> threads;
    bool threadsChanged = false;
    std::unordered_map<uint64_t, Track> cores;
    bool coresChanged = false;
    std::unordered_map<std::string, Plot> plots;
    bool plotsChanged = false;

    uint8_t flags = 0;


    typedef std::unordered_map < std::string, Track > GlobalThreadMap;
    typedef std::unordered_map < std::string, Plot > GlobalPlotMap;
    GlobalThreadMap globalThreads;
    GlobalPlotMap globalPlots;
};


void SyncViewSettings( ViewData& vd, Worker& worker );


struct Annotation
{
    std::string text;
    Range range;
    uint32_t color;
};

struct SourceRegex
{
    std::string pattern;
    std::string target;
    std::regex regex;
};

}

#endif
