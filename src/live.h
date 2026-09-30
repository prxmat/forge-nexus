// The combat as it happens, from arcdps' events relayed by Nexus' ArcDPS Integration addon: the same
// picture a log gives, while the fight is still on. The log, when it lands, takes over.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include "evtc.h"

// WvW team id → colour, learned from logs (statechange 74) and kept for the live fights of the same match-up.
extern std::mutex g_teamColoursMutex;
extern std::map<uint32_t, std::string> g_teamColours;

// Nexus callback for EV_ARCDPS_COMBATEVENT_LOCAL_RAW and _SQUAD_RAW.
void LiveOnEvent(void* payload);
// Each frame: Mumble's combat flag and whether the map is WvW; ends a fight left behind.
void LiveTick(bool inCombat, bool wvwMap);
// A log of the fight arrived: the live view steps aside.
void LiveDismiss();
// Any event since load: the Integration addon is there.
bool LiveEverEvent();
// The current fight as a ParsedFight; false when none is on.
bool LiveSnapshot(ParsedFight& out);
