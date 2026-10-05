// Native check of the synthesized alert sounds (src/tones.cpp): each one renders, has the length and the pitch it
// should, and makes a valid WAV; they are written to build/native/tones/ to be listened to. From the root:
//   clang++ -std=c++17 -Isrc tools/tones-check.cpp src/tones.cpp -o build/native/tones-check && build/native/tones-check
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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
    std::printf(failures ? "%d FAILED\n" : "all good\n", failures);
    return failures ? 1 : 0;
}
