#ifndef __TRACY_FEATURE_DEFINES_HPP__
#define __TRACY_FEATURE_DEFINES_HPP__

#pragma once


#ifdef TRACY_ENABLE


// NOTE: Enable tracy server api
#define TRACY_HAS_SERVER_API_SUPPORT

// NOTE: Enable tracy profiler background mode support
#define TRACY_HAS_BG_SUPPORT


// NOTE: We override the dbghelp loading code to match the library version loaded in the game
#define TRACY_HAS_CUSTOM_DBG_HELP_LOADER


// NOTE: Don't resolve callstacks for inlines
#define TRACY_NO_CALLSTACK_INLINES


#endif // ifdef TRACY_ENABLE

#endif // __TRACY_FEATURE_DEFINES_HPP__
