// The player's own boons, followed from arcdps' buff events: what a WeakAura shows on a support's screen.
// arcdps sends them in realtime "limited to visible, must have previous squad to squad application": the player's
// own boons qualify once a squad member (the player counts) has applied that boon.
#pragma once
#include <cstdint>
#include <map>

struct BoonState {
    int stacks = 0;            // Intensity boons: active stacks. Duration boons: 1 while it runs.
    uint32_t remainingMs = 0;  // Until it runs out (intensity: the longest stack).
    uint32_t peakMs = 0;       // The longest it ran since it last started: for the sweep.
};

// Nexus' EV_ARCDPS_COMBATEVENT_SQUAD_RAW payload.
void AurasOnEvent(void* payload);
// The tracked boons now, by buff id.
std::map<uint32_t, BoonState> AurasSnapshot();
// A buff event about the player arrived since the addon loaded.
bool AurasEverBuff();
// What arcdps sent about the player's boons since load, and the removes that named a stack no longer followed.
struct AuraCounts { uint32_t events = 0, unknownRemoves = 0; };
AuraCounts AurasCounts();
// A map change: the game clears boons without telling arcdps.
void AurasClear();
bool AuraIntensity(uint32_t buff);
