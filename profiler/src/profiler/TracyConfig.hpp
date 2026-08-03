#ifndef __TRACYCONFIG_HPP__
#define __TRACYCONFIG_HPP__

#include <string>

#include "TracyUtility.hpp"

namespace tracy
{

struct Config
{
    bool threadedRendering = true;
    bool focusLostLimit = true;
    int targetFps = 60;
    bool drawFrameTargets = false;
    double horizontalScrollMultiplier = 1.0;
    double verticalScrollMultiplier = 1.0;
    bool memoryLimit = false;
    int memoryLimitPercent = 80;
    bool achievements = false;
    bool achievementsAsked = true;
    int dynamicColors = 1;
    bool forceColors = false;
    bool ghostZones = true;
    int shortenName = (int)ShortenName::NoSpaceAndNormalize;
    bool drawSamples = true;
    bool drawContextSwitches = true;
    int plotHeight = 100;
    bool keepSingleThreadLocks = false;
    bool saveUserScale = false;
    float userScale = 1.0f;

    // LLM assistant settings
#ifdef __EMSCRIPTEN__
    const bool llm = false;
#else
    const bool llm = false;
#endif
    std::string llmAddress = "http://localhost:11434";
    std::string llmModel;
    std::string llmEmbeddingsModel;
    std::string llmUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36";
    std::string llmSearchIdentifier;
    std::string llmSearchApiKey;
};

extern Config s_config;

void LoadConfig();
bool SaveConfig();

}

#endif
