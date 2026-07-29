#ifndef __TRACYGPUS2_HPP__
#define __TRACYGPUS2_HPP__

#ifndef TRACY_ENABLE

#define TracyGPUS2Context(device,queue) nullptr
#define TracyGPUS2Destroy(ctx)
#define TracyGPUS2ContextName(ctx, name, size)

#define TracyGPUS2NewFrame(ctx)

#define TracyGPUS2Zone(ctx, name)
#define TracyGPUS2ZoneC(ctx, name, color)
#define TracyGPUS2NamedZone(ctx, varname, name, active)
#define TracyGPUS2NamedZoneC(ctx, varname, name, color, active)
#define TracyD3D12ZoneTransient(ctx, varname, name, active)

#define TracyGPUS2ZoneS(ctx, name, depth)
#define TracyGPUS2ZoneCS(ctx, name, color, depth)
#define TracyGPUS2NamedZoneS(ctx, varname, name, depth, active)
#define TracyGPUS2NamedZoneCS(ctx, varname, name, color, depth, active)
#define TracyD3D12ZoneTransientS(ctx, varname, name, depth, active)

#define TracyGPUS2Collect(ctx)

namespace tracy
{
    class GPUS2ZoneScope {};
}

using TracyGPUS2Ctx = void *;

#else

#include <atomic>
#include <assert.h>
#include <stdlib.h>

#include "Tracy.hpp"
#include "../client/TracyProfiler.hpp"
#include "../client/TracyCallstack.hpp"
#include "../common/TracyAlign.hpp"
#include "../common/TracyAlloc.hpp"

namespace tracy
{

    class GPUS2Ctx
    {
        friend class GPUS2ZoneScope;

        enum { QueryCount = 0xFFFE };

    public:
        GPUS2Ctx( int64_t tcpu, int64_t tgpu )
            : m_context( GetGpuCtxCounter().fetch_add( 1, std::memory_order_relaxed ) )
            , m_idx( 0 )
        {
            assert( m_context != 255 );

            uint8_t flags = 0;

            const float period = 1.f;
            auto *item = Profiler::QueueSerial();
            MemWrite( &item->hdr.type, QueueType::GpuNewContext );
            MemWrite( &item->gpuNewContext.cpuTime, tcpu );
            MemWrite( &item->gpuNewContext.gpuTime, tgpu );
            memset( &item->gpuNewContext.thread, 0, sizeof( item->gpuNewContext.thread ) );
            MemWrite( &item->gpuNewContext.period, period );
            MemWrite( &item->gpuNewContext.context, m_context );
            MemWrite( &item->gpuNewContext.flags, flags );
            MemWrite( &item->gpuNewContext.type, GpuContextType::GPUS2 );

#ifdef TRACY_ON_DEMAND
            GetProfiler().DeferItem( *item );
#endif

            Profiler::QueueSerialFinish();
        }

        ~GPUS2Ctx()
        {
        }

        void Name( const char *name, uint16_t len )
        {
            auto ptr = ( char * ) tracy_malloc( len );
            memcpy( ptr, name, len );

            auto item = Profiler::QueueSerial();
            MemWrite( &item->hdr.type, QueueType::GpuContextName );
            MemWrite( &item->gpuContextNameFat.context, m_context );
            MemWrite( &item->gpuContextNameFat.ptr, ( uint64_t ) ptr );
            MemWrite( &item->gpuContextNameFat.size, len );

#ifdef TRACY_ON_DEMAND
            GetProfiler().DeferItem( *item );
#endif

            Profiler::QueueSerialFinish();
        }

    
        tracy_force_inline uint16_t NextQueryId()
        {
            m_idx = ( m_idx + 1 );
            if ( m_idx >= QueryCount ) m_idx = 0;
            return m_idx;
        }


        tracy_force_inline uint8_t GetId() const
        {
            return m_context;
        }


        uint8_t m_context;
        uint16_t m_idx = 0xFFFF;

    };


    static inline GPUS2Ctx *CreateGPUS2Context( int64_t tcpu, int64_t tgpu )
    {
        auto ctx = ( GPUS2Ctx * ) tracy_malloc( sizeof( GPUS2Ctx ) );
        new( ctx ) GPUS2Ctx( tcpu, tgpu );
        return ctx;
    }

    static inline void DestroyGPUS2Context( GPUS2Ctx *ctx )
    {
        ctx->~GPUS2Ctx();
        tracy_free( ctx );
    }
}

extern tracy::GPUS2Ctx *g_tracyCtx;

#define TracyGPUS2Context( tcpu, tgpu ) tracy::CreateGPUS2Context( tcpu, tgpu );
#define TracyGPUS2Destroy(ctx) tracy::DestroyGPUS2Context(ctx);
#define TracyGPUS2ContextName(ctx, name, size) ctx->Name(name, size);

tracy_force_inline void TracyGPUS2StartQuery( SceneViewTimestampQuery_t *pTimestampQuery, uint32_t line, const char *source, size_t sourceSz, const char *function, size_t functionSz, const char *name, size_t nameSz )
{
    tracy::Profiler& p = tracy::GetProfiler();

#ifdef TRACY_ON_DEMAND
    if ( !p.IsConnected() || !g_tracyCtx )
    {
        pTimestampQuery->m_nTracyStartQueryId = 0xFFFF;
        pTimestampQuery->m_nTracyConnectionId = 0;
        return;
    }
#endif

    auto *item = tracy::Profiler::QueueSerial();
    const uint64_t sourceLocation = tracy::Profiler::AllocSourceLocation( line, source, sourceSz, function, functionSz, name, nameSz );

    const uint16_t queryId = g_tracyCtx->NextQueryId();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuZoneBeginAllocSrcLocSerial );
    tracy::MemWrite( &item->gpuZoneBegin.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuZoneBegin.srcloc, sourceLocation );
    tracy::MemWrite( &item->gpuZoneBegin.thread, 0 );
    tracy::MemWrite( &item->gpuZoneBegin.queryId, queryId );
    tracy::MemWrite( &item->gpuZoneBegin.context, g_tracyCtx->GetId() );

    pTimestampQuery->m_nTracyStartQueryId = queryId;
    pTimestampQuery->m_nTracyFrameStartTime = p.GetFrameTime();
    pTimestampQuery->m_nTracyConnectionId = p.ConnectionId();

    tracy::Profiler::QueueSerialFinish();

    return;
}

tracy_force_inline void TracyGPUS2EndQuery( SceneViewTimestampQuery_t *pTimestampQuery )
{
    if ( !tracy::GetProfiler().IsConnectedTo( pTimestampQuery->m_nTracyConnectionId ) || !g_tracyCtx )
    {
        pTimestampQuery->m_nTracyStartQueryId = 0xFFFF;
        pTimestampQuery->m_nTracyConnectionId = 0;
        return;
    }

    auto *item = tracy::Profiler::QueueSerial();

    const auto queryId = g_tracyCtx->NextQueryId();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuZoneEndSerial );
    tracy::MemWrite( &item->gpuZoneEnd.cpuTime, tracy::Profiler::GetTime() );
    tracy::MemWrite( &item->gpuZoneEnd.thread, 0 );
    tracy::MemWrite( &item->gpuZoneEnd.queryId, uint16_t( queryId ) );
    tracy::MemWrite( &item->gpuZoneEnd.context, g_tracyCtx->GetId() );

    pTimestampQuery->m_nTracyEndQueryId = queryId;

    tracy::Profiler::QueueSerialFinish();
}

tracy_force_inline void TracyGPUS2CollectQuery( uint32_t nConnId, uint16_t queryId, int64_t time )
{
#ifdef TRACY_ON_DEMAND
    if ( queryId == 0xFFFF || !g_tracyCtx || !tracy::GetProfiler().IsConnectedTo( nConnId ) )
    {
        return;
    }
#endif

    auto *item = tracy::Profiler::QueueSerial();
    tracy::MemWrite( &item->hdr.type, tracy::QueueType::GpuTime );
    tracy::MemWrite( &item->gpuTime.gpuTime, time );
    tracy::MemWrite( &item->gpuTime.queryId, queryId );
    tracy::MemWrite( &item->gpuTime.context, g_tracyCtx->GetId());
    tracy::Profiler::QueueSerialFinish();
}

#endif

#endif
