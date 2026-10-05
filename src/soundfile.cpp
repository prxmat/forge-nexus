#include "soundfile.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "thirdparty/stb_vorbis.c"

namespace soundfile {

namespace {
uint32_t Read32(const uint8_t* at) { return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24); }
uint16_t Read16(const uint8_t* at) { return (uint16_t)(at[0] | (at[1] << 8)); }
void Put16(std::vector<uint8_t>& out, uint16_t value) { out.push_back(value & 0xFF); out.push_back(value >> 8); }
void Put32(std::vector<uint8_t>& out, uint32_t value) { for (int shift = 0; shift < 32; shift += 8) out.push_back((value >> shift) & 0xFF); }

// The samples of a RIFF WAVE file, when it is 16-bit PCM.
bool ReadWav(const std::vector<uint8_t>& file, std::vector<int16_t>& pcm, int& channels, int& rate) {
    if (file.size() < 12 || memcmp(file.data(), "RIFF", 4) != 0 || memcmp(file.data() + 8, "WAVE", 4) != 0) return false;
    bool format = false;
    size_t at = 12;
    while (at + 8 <= file.size()) {
        const uint8_t* chunk = file.data() + at;
        uint32_t size = Read32(chunk + 4);
        size_t body = at + 8, end = std::min(file.size(), body + size);
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            if (Read16(file.data() + body) != 1 || Read16(file.data() + body + 14) != 16) return false; // PCM, 16 bits only.
            channels = Read16(file.data() + body + 2);
            rate = (int)Read32(file.data() + body + 4);
            format = channels > 0 && rate > 0;
        } else if (memcmp(chunk, "data", 4) == 0 && format) {
            pcm.resize((end - body) / 2);
            memcpy(pcm.data(), file.data() + body, pcm.size() * 2);
            return !pcm.empty();
        }
        at = body + size + (size & 1); // Chunks are padded to an even size.
    }
    return false;
}
} // namespace

std::vector<uint8_t> ToWav(const std::vector<uint8_t>& file, int volume) {
    std::vector<int16_t> pcm;
    int channels = 0, rate = 0;
    if (file.size() >= 4 && memcmp(file.data(), "OggS", 4) == 0) {
        short* decoded = nullptr;
        int frames = stb_vorbis_decode_memory(file.data(), (int)file.size(), &channels, &rate, &decoded);
        if (frames > 0 && decoded && channels > 0) pcm.assign(decoded, decoded + (size_t)frames * channels);
        free(decoded);
    } else ReadWav(file, pcm, channels, rate);
    if (pcm.empty() || channels <= 0 || rate <= 0) return {};
    const double gain = std::max(0, std::min(100, volume)) / 100.0;
    const uint32_t bytes = (uint32_t)pcm.size() * 2;
    std::vector<uint8_t> out;
    out.reserve(44 + bytes);
    out.insert(out.end(), { 'R', 'I', 'F', 'F' });
    Put32(out, 36 + bytes);
    out.insert(out.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
    Put32(out, 16);
    Put16(out, 1);
    Put16(out, (uint16_t)channels);
    Put32(out, (uint32_t)rate);
    Put32(out, (uint32_t)rate * channels * 2);
    Put16(out, (uint16_t)(channels * 2));
    Put16(out, 16);
    out.insert(out.end(), { 'd', 'a', 't', 'a' });
    Put32(out, bytes);
    for (int16_t sample : pcm) Put16(out, (uint16_t)(int16_t)std::lround(sample * gain));
    return out;
}

double Seconds(const std::vector<uint8_t>& wav) {
    if (wav.size() < 44) return 0;
    uint32_t perSecond = Read32(wav.data() + 28);
    return perSecond ? (double)Read32(wav.data() + 40) / perSecond : 0;
}

} // namespace soundfile
