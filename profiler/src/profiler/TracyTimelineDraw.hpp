#ifndef __TRACYTIMELINEDRAW_HPP__
#define __TRACYTIMELINEDRAW_HPP__

#include <stdint.h>

#include "TracyEvent.hpp"
#include "TracyShortPtr.hpp"

namespace tracy
{

enum class TimelineDrawType : uint8_t
{
    Folded,
    Zone,
    GhostFolded,
    Ghost
};

enum TimelineDrawSubType : uint8_t
{
    Thread,
    Core,
};

struct TimelineDraw
{
    TimelineDrawType type;
    TimelineDrawSubType subtype;
    uint16_t depth;
    short_ptr<void*> ev;
    Int48 rstart;
    Int48 rend;
    uint16_t comprTid;
    uint32_t num;
    uint32_t inheritedColor;
};


enum class ContextSwitchDrawType : uint8_t
{
    Waiting,
    Folded,
    Running
};

struct ContextSwitchDraw
{
    ContextSwitchDrawType type;
    uint32_t idx;
    uint32_t data;                  // Folded: number of items -OR- Waiting: wait stack
	uint32_t readyingStack;			// only valid in 'Waiting' case
};


struct SamplesDraw
{
    uint32_t num;
    uint32_t idx;
};


struct MessagesDraw
{
    short_ptr<MessageData> msg;
    bool highlight;
    uint32_t num;
};


struct CpuUsageDraw
{
    int own;
    int other;
};


struct CpuCtxDraw
{
    uint32_t idx;
    uint32_t num;
};



struct LockState
{
    enum Type : uint8_t
    {
        Nothing         = 1 << 0,
        HasLock         = 1 << 1,   // green
        HasBlockingLock = 1 << 2,   // yellow
        WaitLock        = 1 << 3    // red
    };
};

struct LockDrawItem
{
    Int48 t1;
    LockState::Type state;
    uint32_t condensed;
    uint32_t lockId;
    short_ptr<LockEventPtr> ptr, next;
};

struct LockDraw
{
    uint32_t id;
    bool forceDraw;
    uint8_t thread;
    bool terminated;
    bool merged;
    const LockMap* mergedLockMap;
    std::vector<LockDrawItem> data;
};

struct HwCounterDrawItem
{
    uint64_t count = 0;
    Int48 tstart = 0;
    Int48 tend = 0;
    float rate = 0.0f;  // # per microsecond
};

struct HwCounterDraw
{
    StringIdx m_name;
    StringIdx m_description;
    uint64_t m_maxCount;
    float m_maxRate;
    std::vector<HwCounterDrawItem> m_items;

    void Reset()
    {
        m_name.SetIdx( 0 );
        m_description.SetIdx( 0 );
        m_maxCount = 0;
        m_maxRate = 0.0f;
        m_items.clear();
    }

    // Note that durationNs can be less than ( tend - tstart ) in case we merge
    // hw counter data to a single item (to reduce number of draw calls ...)
    void AddDrawItem( Int48 tstart, Int48 tend, uint64_t nCount, uint64_t nDurationNs )
    {
        HwCounterDrawItem counterDraw;
        counterDraw.tstart = tstart;
        counterDraw.tend = tend;
        counterDraw.count = nCount;
        counterDraw.rate = 0.0f;

        double flRangeUs = ( double ) ( nDurationNs ) / 1000.0;
        if ( flRangeUs > 0 )
        {
            counterDraw.rate = counterDraw.count / flRangeUs;
        }

        if ( counterDraw.count > m_maxCount ) { m_maxCount = counterDraw.count; }
        if ( counterDraw.rate > m_maxRate ) { m_maxRate = counterDraw.rate; }

        m_items.emplace_back( counterDraw );
    }
};

}

#endif
