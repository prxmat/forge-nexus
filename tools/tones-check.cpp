// Native check of the synthesized alert sounds (src/tones.cpp): each one renders, has the length and the pitch it
// should, and makes a valid WAV; they are written to build/native/tones/ to be listened to. From the root:
// It also decodes the WeakAuras sounds of sounds/weakauras (Ogg Vorbis, WAV) the way the addon plays them.
//   clang -O2 -c -DSTB_VORBIS_NO_STDIO src/thirdparty/stb_vorbis.c -o build/native/stb_vorbis.o
//   clang++ -std=c++17 -Isrc tools/tones-check.cpp src/tones.cpp src/soundfile.cpp build/native/stb_vorbis.o -o build/native/tones-check && build/native/tones-check
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "soundfile.h"
#include "tones.h"

static int failures = 0;
static void Expect(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (!ok) failures++;
}

// The dominant pitch of a stretch, by zero crossings.
static double Pitch(const std::vector<float>& samples, double from, double to) {
    size_t a = (size_t)(from * 44100), b = std::min(samples.size(), (size_t)(to * 44100));
    int crossings = 0;
    for (size_t i = a + 1; i < b; i++) if ((samples[i - 1] < 0) != (samples[i] < 0)) crossings++;
    return crossings / 2.0 / ((b - a) / 44100.0);
}

int main() {
    std::filesystem::create_directories("build/native/tones");
    for (const auto& sound : tones::All()) {
        std::vector<float> samples = tones::Samples(sound.id);
        float peak = 0;
        for (float s : samples) peak = std::max(peak, std::fabs(s));
        std::vector<uint8_t> wav = tones::Wav(sound.id, 80);
        bool header = wav.size() == 44 + samples.size() * 2 && std::string(wav.begin(), wav.begin() + 4) == "RIFF" && std::string(wav.begin() + 8, wav.begin() + 12) == "WAVE";
        Expect(!samples.empty() && peak > 0.2f && header, std::string(sound.label) + ": " + std::to_string(samples.size() * 1000 / 44100) + " ms, peak " + std::to_string(peak));
        Expect(std::fabs(samples.front()) < 0.01f && std::fabs(samples.back()) < 0.05f, std::string(sound.label) + ": starts and ends quiet (no click)");
        std::ofstream(std::string("build/native/tones/") + sound.id + ".wav", std::ios::binary).write((const char*)wav.data(), (std::streamsize)wav.size());
    }
    std::vector<float> siren = tones::Samples("sirene");
    double start = Pitch(siren, 0.02, 0.08), end = Pitch(siren, 0.24, 0.30);
    Expect(start > 820 && start < 980 && end > 480 && end < 620, "the siren falls: " + std::to_string((int)start) + " Hz then " + std::to_string((int)end) + " Hz");
    Expect(std::fabs(Pitch(tones::Samples("bip"), 0.01, 0.08) - 1250) < 40, "the beep sits at 1250 Hz");
    Expect(tones::Wav("nope", 80).empty() && tones::Label("nope") == nullptr, "an unknown sound is nothing");
    std::vector<uint8_t> quiet = tones::Wav("bip", 0);
    bool silent = true;
    for (size_t i = 44; i < quiet.size(); i++) if (quiet[i]) silent = false;
    Expect(silent, "volume 0 is silent");
    // The WeakAuras set, as the addon decodes it.
    int decoded = 0, unreadable = 0;
    double airHorn = 0, warningSiren = 0, sheep = 0, glass = 0;
    for (const auto& entry : std::filesystem::directory_iterator("sounds/weakauras")) {
        std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".ogg" && entry.path().extension() != ".wav") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<uint8_t> wav = soundfile::ToWav(bytes, 80);
        double seconds = soundfile::Seconds(wav);
        if (seconds > 0.1 && seconds < 15) decoded++; else { unreadable++; std::printf("  unreadable: %s\n", name.c_str()); }
        if (name == "AirHorn.ogg") airHorn = seconds;
        if (name == "WarningSiren.ogg") warningSiren = seconds;
        if (name == "SheepBleat.ogg") sheep = seconds;
        if (name == "Glass.wav") glass = seconds;
    }
    Expect(decoded >= 64 && unreadable == 0, "WeakAuras: " + std::to_string(decoded) + " sounds decoded");
    Expect(std::fabs(airHorn - 1.899) < 0.02 && std::fabs(warningSiren - 4.255) < 0.02 && std::fabs(sheep - 1.325) < 0.02 && glass > 0.5, "WeakAuras: lengths as ffprobe reads them (AirHorn " + std::to_string(airHorn) + " s, 96 kHz stereo SheepBleat " + std::to_string(sheep) + " s)");
    {
        std::ifstream in("sounds/weakauras/AirHorn.ogg", std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<uint8_t> full = soundfile::ToWav(bytes, 100), half = soundfile::ToWav(bytes, 50);
        auto peak = [](const std::vector<uint8_t>& wav) { int top = 0; for (size_t i = 44; i + 1 < wav.size(); i += 2) top = std::max(top, std::abs((int)(int16_t)(wav[i] | (wav[i + 1] << 8)))); return top; };
        Expect(std::abs(peak(full) / 2 - peak(half)) <= 1, "WeakAuras: volume 50 halves the sound");
        Expect(soundfile::ToWav({ 'n', 'o', 'p', 'e' }, 80).empty(), "a file that is no sound gives nothing");
    }
    std::printf(failures ? "%d FAILED\n" : "all good\n", failures);
    return failures ? 1 : 0;
}
