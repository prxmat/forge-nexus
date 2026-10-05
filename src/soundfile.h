// Sound files for the alerts: Ogg Vorbis (WeakAuras' format, decoded by stb_vorbis) and 16-bit PCM WAV, turned
// into a playable WAV in memory at a volume. Portable: tools/tones-check decodes the WeakAuras set natively.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace soundfile {

// A file's bytes as a 16-bit PCM WAV scaled to a volume 0–100. Empty when it is neither Ogg Vorbis nor 16-bit PCM WAV.
std::vector<uint8_t> ToWav(const std::vector<uint8_t>& file, int volume);

// The length of a WAV made by ToWav, in seconds (0 when empty).
double Seconds(const std::vector<uint8_t>& wav);

} // namespace soundfile
