// Alertes Forge: encounter timers (the .bhtimer files of TaimiHUD and Hero's Timers), boon auras and a voice, drawn
// over the game like WeakAuras. Display and sound only: nothing here presses a key or plays for the player.
#pragma once
#include <string>

#include "Mumble.h"
#include "Nexus.h"

void AlertsLoad(AddonAPI_t* api, NexusLinkData_t* nexus, Mumble::Data* mumble, const std::string& settingsPath);
void AlertsUnload();
// RT_Render: ticks the timers, draws the bars, the centre text and the auras, reads out what happened.
void AlertsRender();
// The "Alertes" tab of the Forge window. Called with g_state.mutex held.
void AlertsTab();
// Nexus' EV_ARCDPS_COMBATEVENT_SQUAD_RAW: the player's boons.
void AlertsOnArcEvent(void* payload);
