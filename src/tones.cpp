#include "tones.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tones {

namespace {
constexpr int RATE = 44100;
constexpr double PI = 3.14159265358979323846;

// Linear attack, flat, linear release: no click at either end.
double Envelope(double t, double length, double attack, double release) {
    if (t < attack) return t / attack;
    if (t > length - release) return std::max(0.0, (length - t) / release);
    return 1.0;
}

template <typename Sample>
std::vector<float> Render(double length, Sample&& sample) {
    std::vector<float> out((size_t)(length * RATE));
    for (size_t i = 0; i < out.size(); i++) out[i] = (float)sample((double)i / RATE);
    return out;
}

// The countdown "wuh": a buzzy tone falling from 950 to 520 Hz in a third of a second, like a raid warning.
std::vector<float> Siren() {
    const double length = 0.32;
    double phase = 0;
    return Render(length, [&](double t) {
        double frequency = 950.0 * std::pow(520.0 / 950.0, t / length);
        phase += 2 * PI * frequency / RATE;
        return std::tanh(2.4 * std::sin(phase)) * 0.75 * Envelope(t, length, 0.012, 0.09);
    });
}

std::vector<float> Beep(double frequency, double length) {
    return Render(length, [&](double t) { return std::sin(2 * PI * frequency * t) * Envelope(t, length, 0.003, 0.035); });
}

// A two-note horn (a minor third), rich in harmonics: the "honk" for the moment itself.
std::vector<float> Horn() {
    const double length = 0.5;
    return Render(length, [&](double t) {
        double vibrato = 1.0 + 0.004 * std::sin(2 * PI * 6.0 * t);
        double value = 0;
        for (double base : { 311.13, 369.99 })
            for (int harmonic = 1; harmonic <= 7; harmonic++) value += std::sin(2 * PI * base * vibrato * harmonic * t) / harmonic;
        return value * 0.38 * Envelope(t, length, 0.015, 0.12);
    });
}

// A struck bell: four partials, each fading at its own rate.
std::vector<float> Bell() {
    const double length = 1.2;
    const double partials[][3] = { { 523.25, 1.0, 3.2 }, { 1049.6, 0.55, 4.8 }, { 1566.9, 0.35, 6.5 }, { 2113.0, 0.22, 9.0 } };
    return Render(length, [&](double t) {
        double value = 0;
        for (const auto& p : partials) value += p[1] * std::exp(-p[2] * t) * std::sin(2 * PI * p[0] * t);
        return value * 0.45 * Envelope(t, length, 0.002, 0.05);
    });
}

// Two tones taking turns every 80 ms, like a building alarm.
std::vector<float> Alarm() {
    const double length = 0.48, step = 0.08;
    return Render(length, [&](double t) {
        int index = (int)(t / step);
        double local = t - index * step;
        double frequency = index % 2 ? 784.0 : 988.0;
        return std::tanh(1.8 * std::sin(2 * PI * frequency * t)) * 0.7 * Envelope(local, step, 0.004, 0.004) * Envelope(t, length, 0.002, 0.02);
    });
}

// A drop: a quick rise from 500 to 1800 Hz that dies out.
std::vector<float> Drop() {
    const double length = 0.16;
    double phase = 0;
    return Render(length, [&](double t) {
        double frequency = t < 0.045 ? 500.0 * std::pow(1800.0 / 500.0, t / 0.045) : 1800.0;
        phase += 2 * PI * frequency / RATE;
        return std::sin(phase) * std::exp(-22.0 * t) * Envelope(t, length, 0.002, 0.02);
    });
}

const std::vector<Info> SOUNDS = {
    { "sirene", "Sirène (wuh)" },
    { "bip", "Bip" },
    { "bip-aigu", "Bip aigu" },
    { "klaxon", "Klaxon" },
    { "cloche", "Cloche" },
    { "alarme", "Alarme" },
    { "goutte", "Goutte" },
};

void Put16(std::vector<uint8_t>& out, uint16_t value) { out.push_back(value & 0xFF); out.push_back(value >> 8); }
void Put32(std::vector<uint8_t>& out, uint32_t value) { for (int shift = 0; shift < 32; shift += 8) out.push_back((value >> shift) & 0xFF); }
} // namespace

const std::vector<Info>& All() { return SOUNDS; }

const char* Label(const std::string& id) {
    for (const auto& sound : SOUNDS) if (id == sound.id) return sound.label;
    return nullptr;
}

std::vector<float> Samples(const std::string& id) {
    if (id == "sirene") return Siren();
    if (id == "bip") return Beep(1250.0, 0.09);
    if (id == "bip-aigu") return Beep(1760.0, 0.18);
    if (id == "klaxon") return Horn();
    if (id == "cloche") return Bell();
    if (id == "alarme") return Alarm();
    if (id == "goutte") return Drop();
    return {};
}

std::vector<uint8_t> Wav(const std::string& id, int volume) {
    std::vector<float> samples = Samples(id);
    if (samples.empty()) return {};
    float peak = 0;
    for (float s : samples) peak = std::max(peak, std::fabs(s));
    // Peaks at 90 % of full scale at volume 100.
    const double gain = (peak > 0 ? 0.9 / peak : 0) * std::max(0, std::min(100, volume)) / 100.0;
    const uint32_t bytes = (uint32_t)samples.size() * 2;
    std::vector<uint8_t> out;
    out.reserve(44 + bytes);
    out.insert(out.end(), { 'R', 'I', 'F', 'F' });
    Put32(out, 36 + bytes);
    out.insert(out.end(), { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
    Put32(out, 16);
    Put16(out, 1);         // PCM
    Put16(out, 1);         // mono
    Put32(out, RATE);
    Put32(out, RATE * 2);  // bytes per second
    Put16(out, 2);         // block align
    Put16(out, 16);        // bits per sample
    out.insert(out.end(), { 'd', 'a', 't', 'a' });
    Put32(out, bytes);
    for (float s : samples) Put16(out, (uint16_t)(int16_t)std::lround(std::max(-1.0, std::min(1.0, s * gain)) * 32767.0));
    return out;
}

} // namespace tones
