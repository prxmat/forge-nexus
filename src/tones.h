// The alert sounds, synthesized: a WeakAuras-like set (a "wuh" siren for countdowns, beeps, a horn, a bell…) made
// in code, so the addon ships no sound file and borrows none. Portable: tools/tones-check builds it natively.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tones {

struct Info { const char* id; const char* label; };

// Every sound, in the order the menus list them. The id is what the settings keep.
const std::vector<Info>& All();
const char* Label(const std::string& id);

// A playable WAV file (RIFF, 16-bit mono, 44.1 kHz) of that sound at a volume 0–100. Empty for an unknown id.
std::vector<uint8_t> Wav(const std::string& id, int volume);

// The raw samples (−1…1), for checks.
std::vector<float> Samples(const std::string& id);

} // namespace tones
