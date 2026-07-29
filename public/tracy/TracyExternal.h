#ifndef __TRACYEXTERNAL_HPP__
#define __TRACYEXTERNAL_HPP__

#include <stdint.h>

#ifdef _WIN32
	#define TRACY_EXT_CALL __cdecl
#else
	#define TRACY_EXT_CALL
#endif


#ifdef __cplusplus
extern "C"
{
#endif

// Need to ensure this struct matches
//		* ___tracy_source_location_data from tracyC.h
//		* SourceLocationData from TracyProfiler.hpp
typedef struct TracyExternalSourceLocationData_t
{
    const char* name;
    const char* function;
    const char* file;
    uint32_t line;
    uint32_t color;
} TracyExternalSourceLocationData_t;

// Need to ensure this struct matches
//		* ___tracy_c_zone_context from tracyC.h
typedef struct TracyExternalZoneCtx_t
{
    uint32_t id;
    int active;
} TracyExternalZoneCtx_t;

typedef TracyExternalZoneCtx_t (TRACY_EXT_CALL *TracyExternalEmitZoneBeginPtr)( const TracyExternalSourceLocationData_t *srcloc, int active );
typedef void (TRACY_EXT_CALL* TracyExternalEmitZoneEndPtr)( TracyExternalZoneCtx_t ctx );
typedef void( TRACY_EXT_CALL* TracyExternalSetThreadNamePtr )( const char* name );

typedef struct TracyExternalAPI_t
{
    TracyExternalEmitZoneBeginPtr _emitZoneBegin;
    TracyExternalEmitZoneEndPtr _emitZoneEnd;
    TracyExternalSetThreadNamePtr _setThreadName;
} TracyExternalAPI_t;

#ifdef __cplusplus
}
#endif

namespace tracy
{
	class CTracyExternalScopeZone
	{
	public:
		CTracyExternalScopeZone( const CTracyExternalScopeZone& ) = delete;
		CTracyExternalScopeZone( CTracyExternalScopeZone&& ) = delete;
		CTracyExternalScopeZone& operator=( const CTracyExternalScopeZone& ) = delete;
		CTracyExternalScopeZone& operator=( CTracyExternalScopeZone&& ) = delete;

		CTracyExternalScopeZone( TracyExternalAPI_t* pApi, const TracyExternalSourceLocationData_t* srcloc, int active )
			: m_pApi( pApi )
		{
			if ( m_pApi )
			{
				m_ctx = m_pApi->_emitZoneBegin( srcloc, active );
			}
		}

		~CTracyExternalScopeZone()
		{
			if ( m_pApi )
			{
				m_pApi->_emitZoneEnd( m_ctx );
			}
		}

	private:
		TracyExternalAPI_t* m_pApi;
		TracyExternalZoneCtx_t m_ctx;
	};
	}

#ifndef TracyExternalFunction
#    define TracyExternalFunction __FUNCTION__
#endif

#ifndef TracyExternalFile
#    define TracyExternalFile __FILE__
#endif

#ifndef TracyExternalLine
#    define TracyExternalLine __LINE__
#endif

#ifndef TracyExternalConcat
#    define TracyExternalConcat( x, y ) TracyExternalConcatIndirect( x, y )
#endif
#ifndef TracyExternalConcatIndirect
#    define TracyExternalConcatIndirect( x, y ) x##y
#endif

// TODO : Add more macros to support dynamic/runtime built strings as per main tracy macros ( should error here otherwise )
#define TracyExternalZoneNamed( pApi, varname, active ) static constexpr TracyExternalSourceLocationData_t TracyExternalConcat(__tracy_source_location,TracyExternalLine) { nullptr, TracyExternalFunction,  TracyExternalFile, (uint32_t)TracyExternalLine, 0 }; tracy::CTracyExternalScopeZone varname( pApi,  &TracyExternalConcat(__tracy_source_location,TracyExternalLine), active )
#define TracyExternalZoneNamedN( pApi, varname, name, active ) static constexpr TracyExternalSourceLocationData_t TracyExternalConcat(__tracy_source_location,TracyExternalLine) { name, TracyExternalFunction,  TracyExternalFile, (uint32_t)TracyExternalLine, 0 }; tracy::CTracyExternalScopeZone varname( pApi, &TracyExternalConcat(__tracy_source_location,TracyExternalLine), active )

#endif // __TRACYEXTERNAL_HPP__