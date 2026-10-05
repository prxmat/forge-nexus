// Text-to-speech (Windows SAPI) and short system sounds, on a thread of their own: the game never waits for a voice.
#pragma once
#include <string>
#include <vector>

struct VoiceInfo { std::string id, name, language; }; // language: "en", "fr", "de", "es", or "".

void SpeechStart();
void SpeechStop();
// The installed voices, once the speech thread listed them (empty before, or without SAPI).
std::vector<VoiceInfo> SpeechVoices();
// The first installed voice for a language, or "" (the system's default voice).
std::string SpeechVoiceFor(const std::string& language);
// Queued and read in order; a line still waiting after 4 s is dropped (it would come too late). volume 0–100, rate −10–10.
void Speak(const std::string& text, const std::string& voiceId, int volume, int rate);
// 0: soft (Windows' "Asterisk"), 1: sharp ("Exclamation"). Returns at once.
void PlayAlertSound(int kind);

// An alert sound: a synthesized one (tones.h id), or "file:<path>" relative to the sounds folder (.ogg like
// WeakAuras' sounds, or .wav). Volume 0–100. One at a time: a new one cuts the previous; the voice is apart.
void PlayTone(const std::string& id, int volume);
void SetSoundFolder(const std::string& folder);
// The .ogg and .wav files of the sounds folder and its sub-folders, as paths relative to it ("WeakAuras/AirHorn.ogg").
// Read again at most every 5 s, or at the next call after RefreshSoundFiles().
std::vector<std::string> SoundFiles();
void RefreshSoundFiles();
// Stops what plays (before the addon unloads: Windows reads the sound from our memory).
void StopTones();
