#ifndef __TRACYPROTOCOL_HPP__
#define __TRACYPROTOCOL_HPP__

#include <limits>
#include <stdint.h>

namespace tracy
{

constexpr unsigned Lz4CompressBound( unsigned isize ) { return isize + ( isize / 255 ) + 16; }

// We have a different protocol from upstream so I chnage the code to (hopefully) force a conflict if
// the protocol changes. Make sure update the protocol number with the diverging offset we have.
// Therefore we always need to stay ahead of the upstream protocol but also make sure we don't inadvertently
// end up with a version we have already "burned/used up".
// When making changes to our protocol, increase the "offset" value, when upstream changes the protocol simply add
// our current offset value!
//
// Update the comment when updating the protocol!
// Upstream base protocol version: 74
// Our current offset from upstream is: 1
//
enum : uint32_t { ForceProtocolVersionConflict = 0 };
enum : uint32_t { ProtocolVersion = ForceProtocolVersionConflict + 75 };
enum : uint16_t { BroadcastVersion = 4 };

using lz4sz_t = uint32_t;

enum { TargetFrameSize = 256 * 1024 };
enum { LZ4Size = Lz4CompressBound( TargetFrameSize ) };
static_assert( LZ4Size <= (std::numeric_limits<lz4sz_t>::max)(), "LZ4Size greater than lz4sz_t" );
static_assert( TargetFrameSize * 2 >= 64 * 1024, "Not enough space for LZ4 stream buffer" );

enum { HandshakeShibbolethSize = 8 };
static const char HandshakeShibboleth[HandshakeShibbolethSize] = { 'T', 'r', 'a', 'c', 'y', 'P', 'r', 'f' };

enum HandshakeStatus : uint8_t
{
    HandshakePending,
    HandshakeWelcome,
    HandshakeProtocolMismatch,
    HandshakeNotAvailable,
    HandshakeDropped
};

enum { WelcomeMessageProgramNameSize = 64 };
enum { WelcomeMessageHostInfoSize = 1024 };
enum { ProfilerMessageSize = 64 };

#pragma pack( push, 1 )

// Must increase left query space after handling!
enum ServerQuery : uint8_t
{
    ServerQueryTerminate,
    ServerQueryString,
    ServerQueryThreadString,
    ServerQuerySourceLocation,
    ServerQueryPlotName,
    ServerQueryFrameName,
    ServerQueryParameter,
    ServerQueryFiberName,
    ServerQueryExternalName,
    // Items above are high priority. Split order must be preserved. See IsQueryPrio().
    ServerQueryDisconnect,
    ServerQueryCallstackFrame,
    ServerQuerySymbol,
    ServerQuerySymbolCode,
    ServerQuerySourceCode,
    ServerQueryDataTransfer,
    ServerQueryDataTransferPart
};

struct ServerQueryPacket
{
    ServerQuery type;
    uint64_t ptr;
    uint32_t extra;
};

enum { ServerQueryPacketSize = sizeof( ServerQueryPacket ) };


enum CpuArchitecture : uint8_t
{
    CpuArchUnknown,
    CpuArchX86,
    CpuArchX64,
    CpuArchArm32,
    CpuArchArm64
};


struct WelcomeFlag
{
    enum _t : uint8_t
    {
        OnDemand        = 1 << 0,
        IsApple         = 1 << 1,
        CodeTransfer    = 1 << 2,
        CombineSamples  = 1 << 3,
        IdentifySamples = 1 << 4,
    };
};

struct WelcomeMessage
{
    double timerMul;
    int64_t initBegin;
    int64_t initEnd;
    uint64_t delay;
    uint64_t resolution;
    uint64_t epoch;
    uint64_t exectime;
    uint64_t pid;
    int64_t samplingPeriod;
    uint8_t flags;
    uint8_t cpuArch;
    char cpuManufacturer[12];
    uint32_t cpuId;
    char programName[WelcomeMessageProgramNameSize];
    char hostInfo[WelcomeMessageHostInfoSize];
};

enum { WelcomeMessageSize = sizeof( WelcomeMessage ) };


struct OnDemandPayloadMessage
{
    uint64_t frames;
    uint64_t currentTime;
};

enum { OnDemandPayloadMessageSize = sizeof( OnDemandPayloadMessage ) };


enum BroadcastFlags
{
    BroadcastFlags_None = 0,
    BroadcastFlags_DenyConnection = 1 << 0,
};


static_assert( std::numeric_limits<uint8_t>::max() >= WelcomeMessageProgramNameSize );
static_assert( std::numeric_limits<uint8_t>::max() >= ProfilerMessageSize );

struct BroadcastMessage
{
    uint16_t broadcastVersion;
    uint16_t listenPort;
    uint32_t protocolVersion;
    uint64_t pid;
    int32_t activeTime;        // in seconds
    uint64_t flags;
    uint8_t nameLen;
    uint8_t msgLen;
    char strBuffer[int( WelcomeMessageProgramNameSize) + int( ProfilerMessageSize) ];
};

struct BroadcastMessage_v3
{
    uint16_t broadcastVersion;
    uint16_t listenPort;
    uint32_t protocolVersion;
    uint64_t pid;
    int32_t activeTime;        // in seconds
    char programName[WelcomeMessageProgramNameSize];
};

struct BroadcastMessage_v2
{
    uint16_t broadcastVersion;
    uint16_t listenPort;
    uint32_t protocolVersion;
    int32_t activeTime;
    char programName[WelcomeMessageProgramNameSize];
};

struct BroadcastMessage_v1
{
    uint32_t broadcastVersion;
    uint32_t protocolVersion;
    uint32_t listenPort;
    uint32_t activeTime;
    char programName[WelcomeMessageProgramNameSize];
};

struct BroadcastMessage_v0
{
    uint32_t broadcastVersion;
    uint32_t protocolVersion;
    uint32_t activeTime;
    char programName[WelcomeMessageProgramNameSize];
};

enum { BroadcastMessageSize = sizeof( BroadcastMessage ) };
enum { BroadcastMessageSize_v3 = sizeof( BroadcastMessage_v3 ) };
enum { BroadcastMessageSize_v2 = sizeof( BroadcastMessage_v2 ) };
enum { BroadcastMessageSize_v1 = sizeof( BroadcastMessage_v1 ) };
enum { BroadcastMessageSize_v0 = sizeof( BroadcastMessage_v0 ) };

#pragma pack( pop )

}

#endif
