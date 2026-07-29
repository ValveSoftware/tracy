#include <inttypes.h>
#include <random>

#include "TracyFilesystem.hpp"
#include "TracyImGui.hpp"
#include "TracyMouse.hpp"
#include "TracyPrint.hpp"
#include "TracyTimelineItemGpu.hpp"
#include "TracyUtility.hpp"
#include "TracyView.hpp"
#include "tracy_pdqsort.h"

namespace tracy
{

static bool SmallButtonWithSize(const char* label, float diffToMaxX )
{
    ImGuiStyle& style = ImGui::GetStyle();
    float prevPadding = style.FramePadding.x;
    style.FramePadding.x += (diffToMaxX * 0.5f);
    bool pressed = ImGui::SmallButton( label );
    style.FramePadding.x = prevPadding;
    return pressed;
}


void View::DrawOptions()
{
    ImGui::Begin( "Options", &m_showOptions, ImGuiWindowFlags_AlwaysAutoResize );
    if( ImGui::GetCurrentWindowRead()->SkipItems ) { ImGui::End(); return; }

    const ViewData orig = GetViewData();

    const auto scale = GetScale();

    bool saveGlobalSettings = false;
    if ( m_worker.IsDataStatic() )
    {
        saveGlobalSettings = ImGui::Button( "Export to global settings" );
        TooltipIfHovered( "Tracy file loaded. Click here if UI setting changes should be saved into the global settings." );
        ImGui::Separator();
    }

    ImGui::SetNextItemWidth( 90 * scale );
    ImGui::SliderFloat( "##fheight", &m_vd.flFrameHeight, MIN_FRAMES_HEIGHT, MAX_FRAMES_HEIGHT, "%.3fx", ImGuiSliderFlags_AlwaysClamp );
    ImGui::SameLine();
    ImGui::Text( "Frames Height" );

    ImGui::SetNextItemWidth( 90 * scale );
    if ( ImGui::InputInt( "##Max", &m_vd.frameOverviewMaxTimeMS ) )
    {
        if ( m_vd.frameOverviewMaxTimeMS < 1 ) m_vd.frameOverviewMaxTimeMS = 1;
    }
    ImGui::SameLine();
    ImGui::Text( "Frames Max (ms)" );

    ImGui::Separator();

    bool val = m_vd.drawEmptyLabels;
    ImGui::Checkbox( ICON_FA_EXPAND " Draw empty labels", &val );
    m_vd.drawEmptyLabels = val;
    val = m_vd.drawFrameTargets;
    ImGui::Checkbox( ICON_FA_FLAG_CHECKERED " Draw frame targets", &val );
    m_vd.drawFrameTargets = val;
    ImGui::Indent();
    int tmp = m_vd.frameTarget;
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    ImGui::SetNextItemWidth( 90 * scale );
    if( ImGui::InputInt( "Target FPS", &tmp ) )
    {
        if( tmp < 1 ) tmp = 1;
        m_vd.frameTarget = tmp;
    }
    ImGui::SameLine();
    TextDisabledUnformatted( TimeToString( 1000*1000*1000 / tmp ) );
    ImGui::PopStyleVar();
    ImGui::PushFont( m_smallFont );
    SmallColorBox( 0xFF2222DD );
    ImGui::SameLine( 0, 0 );
    ImGui::Text( "  <  %i  <  ", tmp / 2 );
    ImGui::SameLine( 0, 0 );
    SmallColorBox( 0xFF22DDDD );
    ImGui::SameLine( 0, 0 );
    ImGui::Text( "  <  %i  <  ", tmp );
    ImGui::SameLine( 0, 0 );
    SmallColorBox( 0xFF22DD22 );
    ImGui::SameLine( 0, 0 );
    ImGui::Text( "  <  %i  <  ", tmp * 2 );
    ImGui::SameLine( 0, 0 );
    SmallColorBox( 0xFFDD9900 );
    ImGui::PopFont();
    ImGui::Unindent();

    val = m_vd.drawMousePosTime;
    ImGui::Checkbox( ICON_FA_CLOCK " Draw time at mouse position", &val );
    m_vd.drawMousePosTime = val;

    val = m_vd.darkenOutsideExport;
    ImGui::Checkbox( ICON_FA_CIRCLE_HALF_STROKE " Darken outside export range", &val );
    m_vd.darkenOutsideExport = val;

    if( m_worker.HasContextSwitches() )
    {
        ImGui::Separator();
        val = m_vd.drawContextSwitches;
        ImGui::Checkbox( ICON_FA_PERSON_HIKING " Draw context switches", &val );
        m_vd.drawContextSwitches = val;
        ImGui::Indent();
        val = m_vd.darkenContextSwitches;
        SmallCheckbox( ICON_FA_MOON " Darken inactive threads", &val );
        m_vd.darkenContextSwitches = val;
        ImGui::Unindent();
        val = m_vd.drawCpuData;
        ImGui::Checkbox( ICON_FA_SLIDERS " Draw CPU data", &val );
        m_vd.drawCpuData = val;
        ImGui::Indent();
        val = m_vd.drawCpuUsageGraph;
        SmallCheckbox( ICON_FA_SIGNATURE " Draw CPU usage graph", &val );
        m_vd.drawCpuUsageGraph = val;
        ImGui::Unindent();
        ImGui::Indent();
        val = m_vd.viewContextSwitchStack;
        SmallCheckbox( ICON_FA_LAYER_GROUP " Draw context switch thread stack", &val );
        m_vd.viewContextSwitchStack = val;
        ImGui::Unindent();
    }

    if( m_worker.GetCallstackSampleCount() != 0 )
    {
        val = m_vd.drawSamples;
        ImGui::Checkbox( ICON_FA_EYE_DROPPER " Draw stack samples", &val );
        m_vd.drawSamples = val;
    }

    if ( m_worker.GetHwCounterCount() )
    {
        static char tmpBuf[ 1024 ];
        const char *hwCounterName = m_worker.GetHwCounterName().Active() ? m_worker.GetString( m_worker.GetHwCounterName() ) : "L2 cache misses";
        
        ImGui::Separator();
        val = m_vd.drawHwCounter;
        sprintf( tmpBuf, ICON_FA_MEMORY " Hardware counter - %s", hwCounterName );
        ImGui::Checkbox( tmpBuf, &val );
        m_vd.drawHwCounter = val;

        ImGui::Indent();

        // draw mode
        int ival = ( ( int )m_vd.hwCounterDrawMode ) % ( (int)ViewData::EHwCounterDrawMode::DrawModeCount );
        ImGui::TextUnformatted( ICON_FA_CHART_COLUMN " Graph type" );
        ImGui::Indent();
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        ImGui::RadioButton( hwCounterName, &ival, (uint8_t)ViewData::EHwCounterDrawMode::BarGraph_Count );
        sprintf( tmpBuf, "%s/\xce\xbcs", hwCounterName );
        ImGui::RadioButton( tmpBuf, &ival, (uint8_t)ViewData::EHwCounterDrawMode::BarGraph_Rate );
        ImGui::RadioButton( "Text mode", &ival, ( uint8_t ) ViewData::EHwCounterDrawMode::TextMode );
        ImGui::PopStyleVar();
        ImGui::Unindent();
        m_vd.hwCounterDrawMode = (ViewData::EHwCounterDrawMode )ival;

        // Y axis
        val = m_vd.hwCounterYAxisSameScale;
        ImGui::Checkbox( "Y axis - all tracks using same scale", &val );
        m_vd.hwCounterYAxisSameScale = val;

        // target rate
        ival = m_vd.hwCounterRateTarget;
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        ImGui::SetNextItemWidth( 90 * scale );
        sprintf( tmpBuf, "'%s/\xce\xbcs' target rate", hwCounterName );
        if ( ImGui::InputInt( tmpBuf, &ival ) )
        {
            if( ival < 1 ) ival = 1;
            m_vd.hwCounterRateTarget = ival;
        }
        ImGui::PopStyleVar();

        ImGui::Indent();
        ImGui::PushFont( m_smallFont );
        SmallColorBox( 0xFF22DD22 );
        ImGui::SameLine( 0, 0 );
        ImGui::Text( "  <  %i  <  ", ival );
        ImGui::SameLine( 0, 0 );
        SmallColorBox( 0xFF00FFFF );
        ImGui::SameLine( 0, 0 );
        ImGui::Text( "  <  %i  <  ", int(ival * 1.5f) );
        ImGui::SameLine( 0, 0 );
        SmallColorBox( 0xFF0088FF );        
        ImGui::SameLine( 0, 0 );
        ImGui::Text( "  <  %i  <  ", ival * 2 );
        ImGui::SameLine( 0, 0 );
        SmallColorBox( 0xFF0000FF );
        ImGui::PopFont();
        ImGui::Unindent();

        ImGui::Unindent();
    }

    const auto& gpuData = m_worker.GetGpuData();
    if( !gpuData.empty() )
    {
        ImGui::Separator();
        val = m_vd.drawGpuZones;
        ImGui::Checkbox( ICON_FA_EYE " Draw GPU zones", &val );
        m_vd.drawGpuZones = val;
        const auto expand = ImGui::TreeNode( "GPU zones" );
        ImGui::SameLine();
        size_t visibleGpu = 0;
        for( const auto& gd : gpuData ) if( m_tc.GetItem( gd ).IsVisible() ) visibleGpu++;
        if( visibleGpu == gpuData.size() )
        {
        ImGui::TextDisabled( "(%zu)", gpuData.size() );
        }
        else
        {
            ImGui::TextDisabled( "(%zu/%zu)", visibleGpu, gpuData.size() );
        }
        if( expand )
        {
            for( size_t i=0; i<gpuData.size(); i++ )
            {
                const auto& timeline = gpuData[i]->threadData.begin()->second.timeline;
                m_tc.GetItem( gpuData[i] ).VisibilityCheckbox();
                ImGui::SameLine();
                if( gpuData[i]->threadData.size() == 1 )
                {
                    ImGui::TextDisabled( "%s top level zones", RealToString( timeline.size() ) );
                }
                else
                {
                    ImGui::TextDisabled( "%s threads", RealToString( gpuData[i]->threadData.size() ) );
                }
                if( gpuData[i]->name.Active() )
                {
                    char buf[64];
                    auto& item = (TimelineItemGpu&)( m_tc.GetItem( gpuData[i] ) );
                    sprintf( buf, "%s context %i", GpuContextNames[(int)gpuData[i]->type], item.GetIdx() );
                    ImGui::PushFont( m_smallFont );
                    ImGui::TextUnformatted( buf );
                    ImGui::PopFont();
                }
                if( !gpuData[i]->hasCalibration )
                {
                    ImGui::TreePush( (void*)nullptr );
                    auto& drift = GpuDrift( gpuData[i] );
                    ImGui::SetNextItemWidth( 120 * scale );
                    ImGui::PushID( i );
                    ImGui::InputInt( "Drift (ns/s)", &drift );
                    ImGui::PopID();
                    if( timeline.size() > 1 )
                    {
                        ImGui::SameLine();
                        if( ImGui::Button( ICON_FA_ROBOT " Auto" ) )
                        {
                            size_t lastidx = 0;
                            if( timeline.is_magic() )
                            {
                                auto& tl = *((Vector<GpuEvent>*)&timeline);
                                for( size_t j=tl.size()-1; j > 0; j-- )
                                {
                                    if( tl[j].GpuEnd() >= 0 )
                                    {
                                        lastidx = j;
                                        break;
                                    }
                                }
                            }
                            else
                            {
                                for( size_t j=timeline.size()-1; j > 0; j-- )
                                {
                                    if( timeline[j]->GpuEnd() >= 0 )
                                    {
                                        lastidx = j;
                                        break;
                                    }
                                }
                            }

                            enum { NumSlopes = 10000 };
                            std::random_device rd;
                            std::default_random_engine gen( rd() );
                            std::uniform_int_distribution<size_t> dist( 0, lastidx - 1 );
                            float slopes[NumSlopes];
                            size_t idx = 0;
                            if( timeline.is_magic() )
                            {
                                auto& tl = *((Vector<GpuEvent>*)&timeline);
                                do
                                {
                                    const auto p0 = dist( gen );
                                    const auto p1 = dist( gen );
                                    if( p0 != p1 )
                                    {
                                        slopes[idx++] = float( 1.0 - double( tl[p1].GpuStart() - tl[p0].GpuStart() ) / double( tl[p1].CpuStart() - tl[p0].CpuStart() ) );
                                    }
                                }
                                while( idx < NumSlopes );
                            }
                            else
                            {
                                do
                                {
                                    const auto p0 = dist( gen );
                                    const auto p1 = dist( gen );
                                    if( p0 != p1 )
                                    {
                                        slopes[idx++] = float( 1.0 - double( timeline[p1]->GpuStart() - timeline[p0]->GpuStart() ) / double( timeline[p1]->CpuStart() - timeline[p0]->CpuStart() ) );
                                    }
                                }
                                while( idx < NumSlopes );
                            }
                            pdqsort_branchless( slopes, slopes+NumSlopes );
                            drift = int( 1000000000 * -slopes[NumSlopes/2] );
                        }
                    }
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
    }

    ImGui::Separator();
    val = m_vd.drawZones;
    ImGui::Checkbox( ICON_FA_MICROCHIP " Draw CPU zones", &val );
    ImGui::Indent();
    m_vd.drawZones = val;

#ifndef TRACY_NO_STATISTICS
    if( m_worker.AreGhostZonesReady() && m_worker.GetGhostZonesCount() != 0 )
    {
        val = m_vd.ghostZones;
        SmallCheckbox( ICON_FA_GHOST " Draw ghost zones", &val );
        m_vd.ghostZones = val;
    }
#endif

    int ival = m_vd.dynamicColors;
    ImGui::TextUnformatted( ICON_FA_PALETTE " Zone colors" );
    ImGui::SameLine();
    bool forceColors = m_vd.forceColors;
    if( SmallCheckbox( "Ignore custom", &forceColors ) ) m_vd.forceColors = forceColors;
    ImGui::SameLine();
    bool inheritColors = m_vd.inheritParentColors;
    if( SmallCheckbox( "Inherit parent colors", &inheritColors ) ) m_vd.inheritParentColors = inheritColors;
    ImGui::Indent();
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    ImGui::RadioButton( "Static", &ival, 0 );
    ImGui::RadioButton( "Thread dynamic", &ival, 1 );
    ImGui::RadioButton( "Source location dynamic", &ival, 2 );
    ImGui::PopStyleVar();
    ImGui::Unindent();
    m_vd.dynamicColors = ival;
    ival = std::clamp( ( int ) m_vd.shortenName, ( int ) ShortenName::Never, ( int ) ShortenName::NoSpaceAndNormalize );
    ImGui::TextUnformatted( ICON_FA_RULER_HORIZONTAL " Zone name shortening" );
    ImGui::Indent();
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    ImGui::RadioButton( "Disabled", &ival, (uint8_t)ShortenName::Never );
    ImGui::RadioButton( "Minimal length", &ival, (uint8_t)ShortenName::Always );
    ImGui::RadioButton( "Only normalize", &ival, (uint8_t)ShortenName::OnlyNormalize );
    ImGui::RadioButton( "As needed", &ival, (uint8_t)ShortenName::NoSpace );
    ImGui::RadioButton( "As needed + normalize", &ival, (uint8_t)ShortenName::NoSpaceAndNormalize );
    ImGui::PopStyleVar();
    ImGui::Unindent();
    m_vd.shortenName = (ShortenName)ival;

    if ( !m_showCoreView )
    {
        ival = std::clamp( ( int ) m_vd.stackCollapseMode, 0, 2 );
        ImGui::TextUnformatted( ICON_FA_UP_DOWN " Thread stack clamping" );
        ImGui::Indent();
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        ImGui::RadioButton( "Dynamic##stackCollapseClamp", &ival, ViewData::CollapseDynamic );
        ImGui::RadioButton( "Max##stackCollapseClamp", &ival, ViewData::CollapseMax );
        ImGui::RadioButton( "Limit##stackCollapseClamp", &ival, ViewData::CollapseLimit );
        m_vd.stackCollapseMode = ival;

        ImGui::SameLine();
        tmp = m_vd.stackCollapseClamp;
        ImGui::SetNextItemWidth( 90 * scale );
        if( ImGui::InputInt( "##stackCollapseClampLimit", &tmp ) )
        {
            m_vd.stackCollapseClamp = std::clamp( tmp, 0, 256 );
            m_vd.stackCollapseMode = ViewData::CollapseLimit;
        }
        ImGui::PopStyleVar();
        ImGui::Unindent();
    }
    else if ( m_worker.HasContextSwitches() )
    {
        ival = std::clamp( ( int ) m_vd.coreCollapseMode, 0, 2 );
        ImGui::TextUnformatted( ICON_FA_UP_DOWN " Core stack clamping" );
        ImGui::Indent();
        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
        ImGui::RadioButton( "Dynamic##coreCollapseClamp", &ival, ViewData::CollapseDynamic );
        ImGui::RadioButton( "Max##coreCollapseClamp", &ival, ViewData::CollapseMax );
        ImGui::RadioButton( "Limit##coreCollapseClamp", &ival, ViewData::CollapseLimit );
        m_vd.coreCollapseMode = ival;

        ImGui::SameLine();
        tmp = m_vd.coreCollapseClamp;
        ImGui::SetNextItemWidth( 90 * scale );
        if( ImGui::InputInt( "##coreCollapseClampLimit", &tmp ) )
        {
            m_vd.coreCollapseClamp = std::clamp( tmp, 0, 256 );
            m_vd.coreCollapseMode = ViewData::CollapseLimit;
        }
        ImGui::PopStyleVar();
        ImGui::Unindent();
    }

    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 0, 0 ) );
    ImGui::TextUnformatted( ICON_FA_SLIDERS " Ui Controls Position" );
    ImGui::Indent();
    ImGui::SetNextItemWidth( 80 * scale );
    if ( ImGui::BeginCombo( "Location##uicontrolsloc", m_vd.ppszUiControlLoc[ m_vd.uiControlLoc ] ) )
    {
        if ( ImGui::Selectable( m_vd.ppszUiControlLoc[ ViewData::UiCtrlLocLeft ] ) )
        {
            m_vd.uiControlLoc = ViewData::UiCtrlLocLeft;
        }
        else if ( ImGui::Selectable( m_vd.ppszUiControlLoc[ ViewData::UiCtrlLocRight ] ) )
        {
            m_vd.uiControlLoc = ViewData::UiCtrlLocRight;
        }
        else if ( ImGui::Selectable( m_vd.ppszUiControlLoc[ ViewData::UiCtrlLocHidden ] ) )
        {
            m_vd.uiControlLoc = ViewData::UiCtrlLocHidden;
        }
        ImGui::EndCombo();
    }
    ImGui::Unindent();
    ImGui::PopStyleVar();

    ImGui::Unindent();

    if( !m_worker.GetLockMap().empty() )
    {
        size_t lockCnt = 0;
        size_t singleCnt = 0;
        size_t singleTerminatedCnt = 0;
        size_t multiCntCont = 0;
        size_t multiCntUncont = 0;
        for( const auto& l : m_worker.GetLockMap() )
        {
            if( l.second->valid && !l.second->timeline.empty() )
            {
                lockCnt++;
                if( l.second->threadList.size() == 1 )
                {
                    if ( ( l.second->isTerminated ) || ( l.second->timeTerminate > 0 ) )
                    {
                        singleTerminatedCnt++;
                    }
                    else
                    {
                    singleCnt++;
                }
                }
                else if( l.second->isContended )
                {
                    multiCntCont++;
                }
                else
                {
                    multiCntUncont++;
                }
            }
        }

        ImGui::Separator();
        if ( !m_worker.IsDataStatic() )
        {
            ImGui::TextDisabled( "Global Lock Object Count (%zu)", m_worker.GetTotalLockObjectCount() );
            ImGui::TextDisabled( "Active Lock Count (%zu)", m_worker.GetActiveLockMap().size() );
        }
        ImGui::TextDisabled( "Lock Count (%zu)", m_worker.GetLockMap().size() );
        ImGui::TextDisabled( "Single Lock Count (active) (%zu)", singleCnt );
        ImGui::TextDisabled( "Single Lock Count (terminated) (%zu)", singleTerminatedCnt );
        ImGui::TextDisabled( "Multi Contended Count (%zu)", multiCntCont );
        ImGui::TextDisabled( "Multi Uncontended Count (%zu)", multiCntUncont );

        ImGui::Separator();
        val = m_vd.drawLocks;
        ImGui::Checkbox( ICON_FA_LOCK " Draw locks", &val );
        m_vd.drawLocks = val;
        ImGui::SameLine();
        val = m_vd.onlyContendedLocks;
        ImGui::Checkbox( "Only contended", &val );
        TooltipIfHovered( "Toggle whether to also show uncontended events for contended locks" );
        m_vd.onlyContendedLocks = val;

        if ( (m_worker.IsDataStatic() && singleTerminatedCnt) || (!m_worker.IsDataStatic() && m_vd.keepSingleThreadLocks) )
        {
            ImGui::SameLine();
            val = m_vd.drawMergedLocks;
            ImGui::Checkbox( ICON_FA_LOCK " Merge single terminated locks", &val );
            TooltipIfHovered( "Multiple non-overlapping terminated single thread locks are being merged into one entry on the timeline" );
            m_vd.drawMergedLocks = val;
        }

        const auto expand = ImGui::TreeNode( "Locks" );
        ImGui::SameLine();
        size_t visibleLocks = 0;
        for( const auto& l : m_worker.GetLockMap() ) if( Vis( l.second ) ) visibleLocks++;
        if( visibleLocks == lockCnt )
        {
        ImGui::TextDisabled( "(%zu)", lockCnt );
        }
        else
        {
            ImGui::TextDisabled( "(%zu/%zu)", visibleLocks, lockCnt );
        }
        TooltipIfHovered( "Locks with no recorded events are counted, but not listed." );
        if( expand )
        {
            ImGui::SameLine();
            if( ImGui::SmallButton( "Select all" ) )
            {
                for( const auto& l : m_worker.GetLockMap() )
                {
                    Vis( l.second ) = true;
                }
            }
            ImGui::SameLine();
            if( ImGui::SmallButton( "Unselect all" ) )
            {
                for( const auto& l : m_worker.GetLockMap() )
                {
                    Vis( l.second ) = false;
                }
            }
            ImGui::SameLine();
            DrawHelpMarker( "Right click on lock name to open lock information window." );

            const char *drawLockButtonLabels[] =
            {
                ICON_FA_EYE,
                ICON_FA_EYE_SLASH
            };

            const float lockButtonLabelWidths[] =
            {
                ImGui::CalcTextSize(drawLockButtonLabels[0], NULL, true).x,
                ImGui::CalcTextSize(drawLockButtonLabels[1], NULL, true).x,
            };

            const float maxLockLabelWidth = std::max( lockButtonLabelWidths[0], lockButtonLabelWidths[1] );

            ImVec4 drawLockButtonColor[] =
            {
                ImVec4( 0.6f, 0.9f, 0.6f, 1.0f ),
                ImVec4( 0.5f, 0.5f, 0.5f, 1.0f ),
            };

            {
                ImGui::PushID( "##lockDrawContended" );
                const int lockButtonIndex = ( ( ( m_vd.lockDrawFlags & ViewData::ELockDrawVisFlags::Contended ) != 0 ) ? 0 : 1);
                ImGui::PushStyleColor( ImGuiCol_Text, drawLockButtonColor[lockButtonIndex]);
                const float labelWidthDiff = ( maxLockLabelWidth - lockButtonLabelWidths[ lockButtonIndex ] );
                if ( SmallButtonWithSize( drawLockButtonLabels[ lockButtonIndex ], labelWidthDiff ) )
                {
                    m_vd.lockDrawFlags ^= ViewData::ELockDrawVisFlags::Contended;
                }
                ImGui::PopStyleColor( 1 );
                ImGui::PopID();

                ImGui::SameLine();
            }

            const bool multiExpand = ImGui::TreeNodeEx( "Contended locks present in multiple threads", 0 );
            if ( ImGui::IsItemHovered() )
            {
                ImGui::BeginTooltip();
                ImGui::Text( "The locks in this list are contended in multiple threads" );
                ImGui::EndTooltip();
            }

            ImGui::SameLine();
            size_t visibleMultiCntCont = 0;
            for( const auto& l : m_worker.GetLockMap() ) if( l.second->threadList.size() != 1 && l.second->isContended && Vis( l.second ) ) visibleMultiCntCont++;
            if( visibleMultiCntCont == multiCntCont )
            {
            ImGui::TextDisabled( "(%zu)", multiCntCont );
            }
            else
            {
                ImGui::TextDisabled( "(%zu/%zu)", visibleMultiCntCont, multiCntCont );
            }
            if( multiExpand )
            {
                ImGui::SameLine();
                if( ImGui::SmallButton( "Select all" ) )
                {
                    for( const auto& l : m_worker.GetLockMap() )
                    {
                        if( l.second->threadList.size() != 1 && l.second->isContended ) Vis( l.second ) = true;
                    }
                }
                ImGui::SameLine();
                if( ImGui::SmallButton( "Unselect all" ) )
                {
                    for( const auto& l : m_worker.GetLockMap() )
                    {
                        if( l.second->threadList.size() != 1 && l.second->isContended ) Vis( l.second ) = false;
                    }
                }

                std::vector<std::pair<uint32_t, const LockMap*>> locks;
                locks.reserve( multiCntCont );

                for( const auto& l : m_worker.GetLockMap() )
                {
                    if( l.second->valid && !l.second->timeline.empty() && l.second->threadList.size() != 1 && l.second->isContended )
                    {
                        locks.push_back( std::make_pair(l.first, l.second) );
                    }
                }

                std::sort( locks.begin(), locks.end(),
                          []( const std::pair<uint32_t, const LockMap*>& lhs, const std::pair<uint32_t, const LockMap*>& rhs)
                          {
                            return lhs.first < rhs.first;
                          } );

                for ( const std::pair<uint32_t, const LockMap*>& l : locks )
                {
                    {
                        auto& sl = m_worker.GetSourceLocation( l.second->srcloc );
                        auto fileName = m_worker.GetString( sl.file );

                        char buf[1024];
                        if( l.second->customName.Active() )
                        {
                            sprintf( buf, "%" PRIu32 ": %s", l.first, m_worker.GetString( l.second->customName ) );
                        }
                        else
                        {
                            sprintf( buf, "%" PRIu32 ": %s", l.first, m_worker.GetString( m_worker.GetSourceLocation( l.second->srcloc ).function ) );
                        }
                        SmallCheckbox( buf, &Vis( l.second ) );
                        if( ImGui::IsItemHovered() )
                        {
                            m_lockHoverHighlight = l.first;

                            if( ImGui::IsItemClicked( 1 ) )
                            {
                                m_lockInfoWindow = l.first;
                            }
                        }
                        if( m_optionsLockBuzzAnim.Match( l.second->srcloc ) )
                        {
                            const auto time = m_optionsLockBuzzAnim.Time();
                            const auto indentVal = sin( time * 60.f ) * 10.f * time;
                            ImGui::SameLine( 0, ImGui::GetStyle().ItemSpacing.x + indentVal );
                        }
                        else
                        {
                            ImGui::SameLine();
                        }
                        ImGui::TextDisabled( "(%s) %s", RealToString( l.second->timeline.size() ), LocationToString( fileName, sl.line ) );
                        if( ImGui::IsItemHovered() )
                        {
                            DrawSourceTooltip( fileName, sl.line, 1, 1 );
                            if( ImGui::IsItemClicked( 1 ) )
                            {
                                if( SourceFileValid( fileName, m_worker.GetCaptureTime(), *this, m_worker ) )
                                {
                                    ViewSource( fileName, sl.line );
                                }
                                else
                                {
                                    m_optionsLockBuzzAnim.Enable( l.second->srcloc, 0.5f );
                                }
                            }
                        }
                    }
                }
                ImGui::TreePop();
            }

            {
                ImGui::PushID( "##lockDrawUncontended" );
                const int lockButtonIndex = ( ( ( m_vd.lockDrawFlags & ViewData::ELockDrawVisFlags::Uncontended ) != 0 ) ? 0 : 1);
                ImGui::PushStyleColor( ImGuiCol_Text, drawLockButtonColor[lockButtonIndex]);
                const float labelWidthDiff = ( maxLockLabelWidth - lockButtonLabelWidths[ lockButtonIndex ] );
                if ( SmallButtonWithSize( drawLockButtonLabels[ lockButtonIndex ], labelWidthDiff ) )
                {
                    m_vd.lockDrawFlags ^= ViewData::ELockDrawVisFlags::Uncontended;
                }
                ImGui::PopStyleColor( 1 );
                ImGui::PopID();

                ImGui::SameLine();
            }
            ImGui::SameLine();

            const bool multiUncontExpand = ImGui::TreeNodeEx( "Uncontended locks present in multiple threads", 0 );
            if ( ImGui::IsItemHovered() )
            {
                ImGui::BeginTooltip();
                ImGui::Text( "The locks in this list are used in multiple threads, but are not contended" );
                ImGui::EndTooltip();
            }

            ImGui::SameLine();
            uint64_t visibleMultiCntUncont = 0;
            for( const auto& l : m_worker.GetLockMap() ) if( l.second->threadList.size() != 1 && !l.second->isContended && Vis( l.second ) ) visibleMultiCntUncont++;
            if( visibleMultiCntUncont == multiCntUncont )
            {
            ImGui::TextDisabled( "(%zu)", multiCntUncont );
            }
            else
            {
                ImGui::TextDisabled( "(%zu/%zu)", visibleMultiCntUncont, multiCntUncont );
            }
            if( multiUncontExpand )
            {
                ImGui::SameLine();
                if( ImGui::SmallButton( "Select all" ) )
                {
                    for( const auto& l : m_worker.GetLockMap() )
                    {
                        if( l.second->threadList.size() != 1 && !l.second->isContended ) Vis( l.second ) = true;
                    }
                }
                ImGui::SameLine();
                if( ImGui::SmallButton( "Unselect all" ) )
                {
                    for( const auto& l : m_worker.GetLockMap() )
                    {
                        if( l.second->threadList.size() != 1 && !l.second->isContended ) Vis( l.second ) = false;
                    }
                }

                std::vector<std::pair<uint32_t, const LockMap*>> locks;
                locks.reserve( multiCntUncont );

                for( const auto& l : m_worker.GetLockMap() )
                {
                    if( l.second->valid && !l.second->timeline.empty() && l.second->threadList.size() != 1 && !l.second->isContended )
                    {
                        locks.push_back( std::make_pair(l.first, l.second) );
                    }
                }

                std::sort( locks.begin(), locks.end(),
                          []( const std::pair<uint32_t, const LockMap*>& lhs, const std::pair<uint32_t, const LockMap*>& rhs)
                          {
                            return lhs.first < rhs.first;
                          } );

                for ( const std::pair<uint32_t, const LockMap*>& l : locks )
                {
                    {
                        auto& sl = m_worker.GetSourceLocation( l.second->srcloc );
                        auto fileName = m_worker.GetString( sl.file );

                        char buf[1024];
                        if( l.second->customName.Active() )
                        {
                            sprintf( buf, "%" PRIu32 ": %s", l.first, m_worker.GetString( l.second->customName ) );
                        }
                        else
                        {
                            sprintf( buf, "%" PRIu32 ": %s", l.first, m_worker.GetString( m_worker.GetSourceLocation( l.second->srcloc ).function ) );
                        }
                        SmallCheckbox( buf, &Vis( l.second ) );
                        if( ImGui::IsItemHovered() )
                        {
                            m_lockHoverHighlight = l.first;

                            if( ImGui::IsItemClicked( 1 ) )
                            {
                                m_lockInfoWindow = l.first;
                            }
                        }
                        if( m_optionsLockBuzzAnim.Match( l.second->srcloc ) )
                        {
                            const auto time = m_optionsLockBuzzAnim.Time();
                            const auto indentVal = sin( time * 60.f ) * 10.f * time;
                            ImGui::SameLine( 0, ImGui::GetStyle().ItemSpacing.x + indentVal );
                        }
                        else
                        {
                            ImGui::SameLine();
                        }
                        ImGui::TextDisabled( "(%s) %s", RealToString( l.second->timeline.size() ), LocationToString( fileName, sl.line ) );
                        if( ImGui::IsItemHovered() )
                        {
                            DrawSourceTooltip( fileName, sl.line, 1, 1 );
                            if( ImGui::IsItemClicked( 1 ) )
                            {
                                if( SourceFileValid( fileName, m_worker.GetCaptureTime(), *this, m_worker ) )
                                {
                                    ViewSource( fileName, sl.line );
                                }
                                else
                                {
                                    m_optionsLockBuzzAnim.Enable( l.second->srcloc, 0.5f );
                                }
                            }
                        }
                    }
                }
                ImGui::TreePop();
            }

            {
                ImGui::PushID( "##lockDrawSingleThread" );
                const int lockButtonIndex = ( ( ( m_vd.lockDrawFlags & ViewData::ELockDrawVisFlags::SingleThread ) != 0 ) ? 0 : 1);
                ImGui::PushStyleColor( ImGuiCol_Text, drawLockButtonColor[lockButtonIndex]);
                const float labelWidthDiff = ( maxLockLabelWidth - lockButtonLabelWidths[ lockButtonIndex ] );
                if ( SmallButtonWithSize( drawLockButtonLabels[ lockButtonIndex ], labelWidthDiff ) )
                {
                    m_vd.lockDrawFlags ^= ViewData::ELockDrawVisFlags::SingleThread;
                }
                ImGui::PopStyleColor( 1 );
                ImGui::PopID();

                ImGui::SameLine();
            }
            ImGui::SameLine();

            const auto singleExpand = ImGui::TreeNodeEx( "Locks present in a single thread (active)", 0 );
            if ( ImGui::IsItemHovered() )
            {
                ImGui::BeginTooltip();
                ImGui::Text( "The locks in this list are only ever used in a single thread" );
                ImGui::EndTooltip();
            }

            ImGui::SameLine();
            uint64_t visibleSingleCnt = 0;
            for( const auto& l : m_worker.GetLockMap() ) if( l.second->threadList.size() == 1 && Vis( l.second ) ) visibleSingleCnt++;
            if( visibleSingleCnt == singleCnt )
            {
            ImGui::TextDisabled( "(%zu)", singleCnt );
            }
            else
            {
                ImGui::TextDisabled( "(%zu/%zu)", visibleSingleCnt, singleCnt );
            }
            if( singleExpand )
            {
                ImGui::SameLine();
                if( ImGui::SmallButton( "Select all" ) )
                {
                    for( const auto& l : m_worker.GetLockMap() )
                    {
                        if ( l.second->threadList.size() == 1 )
                        {
                            const bool isDead = ( ( l.second->timeTerminate > 0 ) || ( l.second->isTerminated ) );
                            if ( !isDead )
                            {
                                Vis( l.second ) = true;
                            }
                        }
                    }
                }
                ImGui::SameLine();
                if( ImGui::SmallButton( "Unselect all" ) )
                {
                    for( const auto& l : m_worker.GetLockMap() )
                    {
                        if ( l.second->threadList.size() == 1 )
                        {
                            const bool isDead = ( ( l.second->timeTerminate > 0 ) || ( l.second->isTerminated ) );
                            if ( !isDead )
                            {
                                Vis( l.second ) = false;
                            }
                        }
                    }
                }

                std::vector<std::pair<uint32_t, const LockMap*>> locks;
                locks.reserve( singleCnt );

                for( const auto& l : m_worker.GetLockMap() )
                {
                    const bool isDead = ( ( l.second->timeTerminate > 0 ) || ( l.second->isTerminated ) );

                    if( !isDead && l.second->valid && !l.second->timeline.empty() && l.second->threadList.size() == 1 )
                    {
                        locks.push_back( std::make_pair(l.first, l.second) );
                    }
                }

                std::sort( locks.begin(), locks.end(),
                          []( const std::pair<uint32_t, const LockMap*>& lhs, const std::pair<uint32_t, const LockMap*>& rhs)
                          {
                            return lhs.first < rhs.first;
                          } );

                for ( const std::pair<uint32_t, const LockMap*>& l : locks )
                {
                    {
                        auto& sl = m_worker.GetSourceLocation( l.second->srcloc );
                        auto fileName = m_worker.GetString( sl.file );

                        char buf[1024];
                        if( l.second->customName.Active() )
                        {
                            sprintf( buf, "%" PRIu32 ": %s", l.first, m_worker.GetString( l.second->customName ) );
                        }
                        else
                        {
                            sprintf( buf, "%" PRIu32 ": %s", l.first, m_worker.GetString( m_worker.GetSourceLocation( l.second->srcloc ).function ) );
                        }
                        SmallCheckbox( buf, &Vis( l.second ) );
                        if( ImGui::IsItemHovered() )
                        {
                            m_lockHoverHighlight = l.first;

                            if( ImGui::IsItemClicked( 1 ) )
                            {
                                m_lockInfoWindow = l.first;
                            }
                        }
                        if( m_optionsLockBuzzAnim.Match( l.second->srcloc ) )
                        {
                            const auto time = m_optionsLockBuzzAnim.Time();
                            const auto indentVal = sin( time * 60.f ) * 10.f * time;
                            ImGui::SameLine( 0, ImGui::GetStyle().ItemSpacing.x + indentVal );
                        }
                        else
                        {
                            ImGui::SameLine();
                        }
                        ImGui::TextDisabled( "(%s) %s", RealToString( l.second->timeline.size() ), LocationToString( fileName, sl.line ) );
                        if( ImGui::IsItemHovered() )
                        {
                            DrawSourceTooltip( fileName, sl.line, 1, 1 );
                            if( ImGui::IsItemClicked( 1 ) )
                            {
                                if( SourceFileValid( fileName, m_worker.GetCaptureTime(), *this, m_worker ) )
                                {
                                    ViewSource( fileName, sl.line );
                                }
                                else
                                {
                                    m_optionsLockBuzzAnim.Enable( l.second->srcloc, 0.5f );
                                }
                            }
                        }
                    }
                }
                ImGui::TreePop();
            }

            {
                ImGui::PushID( "##lockDrawSingleThreadDead" );
                const int lockButtonIndex = ( ( ( m_vd.lockDrawFlags & ViewData::ELockDrawVisFlags::SingleTerminated ) != 0 ) ? 0 : 1 );
                ImGui::PushStyleColor( ImGuiCol_Text, drawLockButtonColor[ lockButtonIndex ] );
                const float labelWidthDiff = ( maxLockLabelWidth - lockButtonLabelWidths[ lockButtonIndex ] );
                if ( SmallButtonWithSize( drawLockButtonLabels[ lockButtonIndex ], labelWidthDiff ) )
                {
                    m_vd.lockDrawFlags ^= ViewData::ELockDrawVisFlags::SingleTerminated;
                }
                ImGui::PopStyleColor( 1 );
                ImGui::PopID();

                ImGui::SameLine();
            }
            ImGui::SameLine();

            const auto singleExpandDead = ImGui::TreeNodeEx( "Locks present in a single thread (terminated)", 0 );
            if ( ImGui::IsItemHovered() )
            {
                ImGui::BeginTooltip();
                if ( !m_worker.IsDataStatic() && !m_vd.keepSingleThreadLocks )
                {
                    ImGui::TextColored( ImVec4( 0.9f, 0.2f, 0.2f, 1.0f ), "No information for these locks is being saved!" );
                    ImGui::TextColored( ImVec4( 0.4f, 0.4f, 0.4f, 1.0f ), "If you need this information, enable the feature in the global settings and re-connect" );
                }
                ImGui::Text( "The locks in this list have been destroyed and were only ever used in a single thread" );
                ImGui::EndTooltip();
            }

            ImGui::SameLine();
            ImGui::TextDisabled( "(%zu)", singleTerminatedCnt );
            if ( singleExpandDead )
            {
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Select all" ) )
                {
                    for ( const auto &l : m_worker.GetLockMap() )
                    {
                        if ( l.second->threadList.size() == 1 )
                        {
                            const bool isDead = ( ( l.second->timeTerminate > 0 ) || ( l.second->isTerminated ) );
                            if ( isDead )
                            {
                                Vis( l.second ) = true;
                            }
                        }
                    }
                }
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Unselect all" ) )
                {
                    for ( const auto &l : m_worker.GetLockMap() )
                    {
                        if ( l.second->threadList.size() == 1 )
                        {
                            const bool isDead = ( ( l.second->timeTerminate > 0 ) || ( l.second->isTerminated ) );
                            if ( isDead )
                            {
                                Vis( l.second ) = false;
                            }
                        }
                    }
                }

                std::vector<std::pair<uint32_t, const LockMap *>> locks;
                locks.reserve( singleTerminatedCnt );

                for ( const auto &l : m_worker.GetLockMap() )
                {
                    const bool isDead = ( ( l.second->timeTerminate > 0 ) || ( l.second->isTerminated ) );

                    if ( isDead && l.second->valid && !l.second->timeline.empty() && l.second->threadList.size() == 1 )
                    {
                        locks.push_back( std::make_pair( l.first, l.second ) );
                    }
                }

                std::sort( locks.begin(), locks.end(),
                            [] ( const std::pair<uint32_t, const LockMap *> &lhs, const std::pair<uint32_t, const LockMap *> &rhs )
                            {
                                return lhs.first < rhs.first;
                            } );

                for ( const std::pair<uint32_t, const LockMap *> &l : locks )
                {
                    {
                        auto& sl = m_worker.GetSourceLocation( l.second->srcloc );
                        auto fileName = m_worker.GetString( sl.file );

                        char buf[1024];
                        if( l.second->customName.Active() )
                        {
                            sprintf( buf, "%" PRIu32 ": %s", l.first, m_worker.GetString( l.second->customName ) );
                        }
                        else
                        {
                            sprintf( buf, "%" PRIu32 ": %s", l.first, m_worker.GetString( m_worker.GetSourceLocation( l.second->srcloc ).function ) );
                        }
                        SmallCheckbox( buf, &Vis( l.second ) );
                        if( ImGui::IsItemHovered() )
                        {
                            m_lockHoverHighlight = l.first;

                            if( ImGui::IsItemClicked( 1 ) )
                            {
                                m_lockInfoWindow = l.first;
                            }
                        }
                        if( m_optionsLockBuzzAnim.Match( l.second->srcloc ) )
                        {
                            const auto time = m_optionsLockBuzzAnim.Time();
                            const auto indentVal = sin( time * 60.f ) * 10.f * time;
                            ImGui::SameLine( 0, ImGui::GetStyle().ItemSpacing.x + indentVal );
                        }
                        else
                        {
                            ImGui::SameLine();
                        }
                        ImGui::TextDisabled( "(%s) %s", RealToString( l.second->timeline.size() ), LocationToString( fileName, sl.line ) );
                        if( ImGui::IsItemHovered() )
                        {
                            DrawSourceTooltip( fileName, sl.line, 1, 1 );
                            if( ImGui::IsItemClicked( 1 ) )
                            {
                                if( SourceFileValid( fileName, m_worker.GetCaptureTime(), *this, m_worker ) )
                                {
                                    ViewSource( fileName, sl.line );
                                }
                                else
                                {
                                    m_optionsLockBuzzAnim.Enable( l.second->srcloc, 0.5f );
                                }
                            }
                        }
                    }
                }
                ImGui::TreePop();
            }
            ImGui::TreePop();
        }
    }

    if( !m_worker.GetPlots().empty() )
    {
        ImGui::Separator();

        ImGui::Text( ICON_FA_SIGNATURE "Draw plots:" );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 140 * scale );
        
        if ( ImGui::BeginCombo( "##drawplots", m_vd.ppszPlotViz[m_vd.drawPlots ]) )
        {
			if ( ImGui::Selectable( m_vd.ppszPlotViz[ m_vd.Disable ] ) ) m_vd.drawPlots = m_vd.Disable;
			if ( ImGui::Selectable( m_vd.ppszPlotViz[ m_vd.Top ] ) ) m_vd.drawPlots = m_vd.Top;
			if ( ImGui::Selectable( m_vd.ppszPlotViz[ m_vd.Bottom ] ) ) m_vd.drawPlots = m_vd.Bottom;
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        int pH = m_vd.plotHeight;
        ImGui::SliderInt("Plot heights", &pH, 30, 200);
        m_vd.plotHeight = pH;

        const auto expand = ImGui::TreeNode( "Plots" );
        ImGui::SameLine();
        size_t visiblePlots = 0;
        for( const auto& p : m_worker.GetPlots() ) if( m_tc.GetItem( p ).IsVisible() ) visiblePlots++;
        if( visiblePlots == m_worker.GetPlots().size() )
        {
        ImGui::TextDisabled( "(%zu)", m_worker.GetPlots().size() );
        }
        else
        {
            ImGui::TextDisabled( "(%zu/%zu)", visiblePlots, m_worker.GetPlots().size() );
        }
        if( expand )
        {
            bool settingsChanged = false;
            ImGui::SameLine();
            if( ImGui::SmallButton( "Select all" ) )
            {
                for( const auto& p : m_worker.GetPlots() )
                {
                    m_tc.GetItem( p ).SetVisible( true );
                }
                settingsChanged = true;
            }
            ImGui::SameLine();
            if( ImGui::SmallButton( "Unselect all" ) )
            {
                for( const auto& p : m_worker.GetPlots() )
                {
                    m_tc.GetItem( p ).SetVisible( false );
                }
                settingsChanged = true;
            }

            for( const auto& p : m_worker.GetPlots() )
            {
                SmallColorBox( GetPlotColor( *p, m_worker ) );
                ImGui::SameLine();
                settingsChanged |= m_tc.GetItem( p ).VisibilityCheckbox();
                ImGui::SameLine();
                ImGui::TextDisabled( "%s data points", RealToString( p->data.size() ) );
            }
            ImGui::TreePop();

            if (settingsChanged)
            {
                for( const auto& p : m_worker.GetPlots() )
                {
                    if ( ( p->type == PlotType::User ) || ( p->type == PlotType::SysTime ) )
                    {
                        const char* pname =   (p->type == PlotType::SysTime)
                                            ? "__SysTime_CPU_usage__"
                                            : ( p->name.active ? m_worker.GetString( p->name ) : nullptr );
                        if ( pname && (strcmp( pname, "???" ) != 0) )
                        {
                            uint8_t flags = 0;
                            const std::string name( pname );
                            const bool visible = m_tc.GetItem( p ).IsVisible();

                            auto it = m_vd.plots.find( name );
                            if ( it != m_vd.plots.end() )
                            {
                                if ( visible != it->second.visible )
                                {
                                    flags |= ViewData::Flags_Manual;
                                }
                            }

                            m_vd.plots[ name ].visible = visible;
                        }
                    }
                }

                m_vd.plotsChanged = true;
            }
        }
    }

    if( m_showCoreView && m_worker.HasContextSwitches() )
    {
        const CpuData *cpuData = m_worker.GetCpuData();
        const int cpuDataCount = m_worker.GetCpuDataCpuCount();

        if ( cpuData && ( cpuDataCount > 0 ) )
        {
            struct IdAndData
            {
                int id;
                const CpuData *pData;
            };

            std::vector< IdAndData > allCpuThreads;
            allCpuThreads.reserve( cpuDataCount );

            for( int i = 0; i < cpuDataCount; i++ )
            {
                const CpuData *pPerCpuData = &cpuData[ i ];
                allCpuThreads.push_back( IdAndData{ i, pPerCpuData } );
            }

            ImGui::Separator();
            bool expand = ImGui::TreeNode( ICON_FA_SHUFFLE " Visible cores:" );
            ImGui::SameLine();
            ImGui::TextDisabled( "(%zu)", allCpuThreads.size() );
            if( expand )
            {
                bool settingsChanged = false;

                ImGui::SameLine();
                if( ImGui::SmallButton( "Select all" ) )
                {
                    for( const IdAndData& t : allCpuThreads )
                    {
                        m_tc.GetItem( t.pData ).SetVisible( true );
                    }
                    settingsChanged = true;
                }
                ImGui::SameLine();
                if( ImGui::SmallButton( "Unselect all" ) )
                {
                    for( const IdAndData& t : allCpuThreads )
                    {
                        m_tc.GetItem( t.pData ).SetVisible( false );
                    }
                    settingsChanged = true;
                }

                for ( const IdAndData& t : allCpuThreads )
                {
                    settingsChanged |= m_tc.GetItem( t.pData ).VisibilityCheckbox();
                }
                ImGui::TreePop();

                if (settingsChanged)
                {
                    for (int32_t i = 0; i < allCpuThreads.size(); i++)
                    {
                        uint8_t flags = 0;

                        const IdAndData& t = allCpuThreads[ i ];
                        const bool visible = m_tc.GetItem( t.pData ).IsVisible();
                        auto it = m_vd.cores.find( t.id );
                        if ( it != m_vd.cores.end() )
                        {
                            if ( ( i != it->second.priority) || ( visible != it->second.visible ) )
                            {
                                flags |= ViewData::Flags_Manual;
                            }
                        }

                        m_vd.cores[ t.id ].priority = i;
                        m_vd.cores[ t.id ].visible = visible;
                        m_vd.cores[ t.id ].flags = flags;
                    }

                    m_vd.coresChanged = true;
                }
            }
        }
    }
    else
    {
        ImGui::Separator();
        auto expand = ImGui::TreeNode( ICON_FA_SHUFFLE " Visible threads:" );
        ImGui::SameLine();
        size_t visibleThreads = 0;
        for( const auto& t : m_threadOrder ) if( m_tc.GetItem( t ).IsVisible() ) visibleThreads++;
        if( visibleThreads == m_threadOrder.size() )
        {
        ImGui::TextDisabled( "(%zu)", m_threadOrder.size() );
        }
        else
        {
            ImGui::TextDisabled( "(%zu/%zu)", visibleThreads, m_threadOrder.size() );
        }
        if( expand )
        {
            bool settingsChanged = false;
            auto& crash = m_worker.GetCrashEvent();

            ImGui::SameLine();
            if( ImGui::SmallButton( "Select all" ) )
            {
                for( const auto& t : m_threadOrder )
                {
                    m_tc.GetItem( t ).SetVisible( true );
                }
                settingsChanged = true;
            }
            ImGui::SameLine();
            if( ImGui::SmallButton( "Unselect all" ) )
            {
                for( const auto& t : m_threadOrder )
                {
                    m_tc.GetItem( t ).SetVisible( false );
                }
                settingsChanged = true;
            }
            ImGui::SameLine();
            if( ImGui::SmallButton( "Sort" ) )
            {
                pdqsort_branchless( m_threadOrder.begin(), m_threadOrder.end(), [this] ( const auto& lhs, const auto& rhs ) {
                    if( lhs->groupHint != rhs->groupHint ) return lhs->groupHint < rhs->groupHint;
                    return strcmp( m_worker.GetThreadName( lhs->id ), m_worker.GetThreadName( rhs->id ) ) < 0;
                } );
            }

            const auto wposx = ImGui::GetCursorScreenPos().x;
            m_threadDnd.clear();
            int idx = 0;

            for( const auto& t : m_threadOrder )
            {
                m_threadDnd.push_back( ImGui::GetCursorScreenPos().y );
                ImGui::PushID( idx );
                const auto threadName = m_worker.GetThreadName( t->id );
                const auto threadColor = GetThreadColor( t->id, 0 );
                SmallColorBox( threadColor );
                ImGui::SameLine();
                settingsChanged |= m_tc.GetItem( t ).VisibilityCheckbox();
                if( ImGui::BeginDragDropSource( ImGuiDragDropFlags_SourceNoHoldToOpenOthers ) )
                {
                    ImGui::SetDragDropPayload( "ThreadOrder", &idx, sizeof(int) );
                    ImGui::TextUnformatted( ICON_FA_SHUFFLE );
                    ImGui::SameLine();
                    SmallColorBox( threadColor );
                    ImGui::SameLine();
                    ImGui::TextUnformatted( threadName );
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::TextDisabled( "(%s)", RealToString( t->id ) );
                if( crash.thread == t->id )
                {
                    ImGui::SameLine();
                    TextColoredUnformatted( ImVec4( 1.f, 0.2f, 0.2f, 1.f ), ICON_FA_SKULL );
                    if( ImGui::IsItemHovered() )
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted( "Crashed" );
                        ImGui::EndTooltip();
                        if( IsMouseClicked( 0 ) )
                        {
                            m_showInfo = true;
                        }
                        if( IsMouseClicked( 2 ) )
                        {
                            CenterAtTime( crash.time );
                        }
                    }
                }
                if( t->isFiber )
                {
                    ImGui::SameLine();
                    TextColoredUnformatted( ImVec4( 0.2f, 0.6f, 0.2f, 1.f ), "Fiber" );
                }
                ImGui::SameLine();
                ImGui::TextDisabled( "%s top level zones", RealToString( t->timeline.size() ) );
                if ( m_worker.IsDataStatic() )
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled( "Max stack depth: %s", RealToString( t->maxDepth ) );
                }
                idx++;
            }
            if( m_threadDnd.size() > 1 )
            {
                const auto w = ImGui::GetContentRegionAvail().x;
                const auto dist = m_threadDnd[1] - m_threadDnd[0];
                const auto half = dist * 0.5f;
                m_threadDnd.push_back( m_threadDnd.back() + dist );

                int target = -1;
                int source;
                for( size_t i=0; i<m_threadDnd.size(); i++ )
                {
                    if( ImGui::BeginDragDropTargetCustom( ImRect( wposx, m_threadDnd[i] - half, wposx + w, m_threadDnd[i] + half ), i+1 ) )
                    {
                        auto draw = ImGui::GetWindowDrawList();
                        draw->AddLine( ImVec2( wposx, m_threadDnd[i] ), ImVec2( wposx + w, m_threadDnd[i] ), ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.f );
                        if( auto payload = ImGui::AcceptDragDropPayload( "ThreadOrder", ImGuiDragDropFlags_AcceptNoDrawDefaultRect ) )
                        {
                            target = (int)i;
                            source = *(int*)payload->Data;
                        }
                        ImGui::EndDragDropTarget();
                    }
                }
                if( target >= 0 && target != source )
                {
                    settingsChanged = true;

                    const auto srcval = m_threadOrder[source];
                    if( target < source )
                    {
                        assert( source < (int)m_threadOrder.size() );
                        m_threadOrder.erase( m_threadOrder.begin() + source );
                        m_threadOrder.insert( m_threadOrder.begin() + target, srcval );
                    }
                    else
                    {
                        assert( target <= (int)m_threadOrder.size() );
                        m_threadOrder.insert( m_threadOrder.begin() + target, srcval );
                        m_threadOrder.erase( m_threadOrder.begin() + source );
                    }
                }
            }
            ImGui::TreePop();

            if (settingsChanged)
            {
                for (int32_t i = 0; i < m_threadOrder.size(); i++)
                {
                    uint8_t flags = 0;
                    const ThreadData *td = m_threadOrder[ i ];
                    const bool visible = m_tc.GetItem( td ).IsVisible();
                    auto it = m_vd.threads.find( td->id );
                    if ( it != m_vd.threads.end() )
                    {
                        if ( ( i != it->second.priority) || ( visible != it->second.visible ) )
                        {
                            flags |= ViewData::Flags_Manual;
                        }
                    }

                    m_vd.threads[ td->id ].priority = i;
                    m_vd.threads[ td->id ].visible = visible;
                    m_vd.threads[ td->id ].flags = flags;
                }

                m_vd.threadsChanged = true;
            }
        }
    }


    if( m_worker.AreFramesUsed() )
    {
        ImGui::Separator();
        auto expand = ImGui::TreeNode( ICON_FA_IMAGES " Visible frame sets:" );
        ImGui::SameLine();
        uint64_t visibleFrames = 0;
        for( const auto& fd : m_worker.GetFrames() ) if( Vis( fd ) ) visibleFrames++;
        if( visibleFrames == m_worker.GetFrames().size() )
        {
            ImGui::TextDisabled( "(%zu)", m_worker.GetFrames().size() );
        }
        else
        {
            ImGui::TextDisabled( "(%zu/%zu)", visibleFrames, m_worker.GetFrames().size() );
        }
        if( expand )
        {
            ImGui::SameLine();
            if( ImGui::SmallButton( "Select all" ) )
            {
                for( const auto& fd : m_worker.GetFrames() )
                {
                    Vis( fd ) = true;
                }
            }
            ImGui::SameLine();
            if( ImGui::SmallButton( "Unselect all" ) )
            {
                for( const auto& fd : m_worker.GetFrames() )
                {
                    Vis( fd ) = false;
                }
            }

            int idx = 0;
            for( const auto& fd : m_worker.GetFrames() )
            {
                ImGui::PushID( idx++ );
                SmallCheckbox( GetFrameSetName( *fd ), &Vis( fd ) );
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::TextDisabled( "%s %sframes", RealToString( fd->frames.size() ), fd->continuous ? "" : "discontinuous " );
            }
            ImGui::TreePop();
        }
    }
    ImGui::End();

    if ( m_vd != orig )
    {
        if ( (ViewDataCommon&)m_vd != orig )
        {
            m_vd.flags |= ViewData::Flags_Manual;
        }

        SyncViewSettings( m_vd, m_worker );
        if ( m_worker.IsDataStatic() )
        {
            m_userData.SaveStateJson( m_vd, false );
        }
        else
        {
            saveGlobalSettings = true;
        }
    }

    if ( saveGlobalSettings )
    {
        m_userData.SaveStateJson( m_vd, true );
    }
}

}
