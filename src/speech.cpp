#include "speech.h"

#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>

#include "soundfile.h"
#include "tones.h"

namespace {
struct Line { std::wstring text; std::string voice; int volume = 100, rate = 0; ULONGLONG at = 0; };

std::mutex mutex;
std::condition_variable wake;
std::deque<Line> queue;
std::vector<VoiceInfo> voices;
bool running = false;
std::thread worker;

std::wstring Widen(const std::string& text) {
    if (text.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size);
    return out;
}

std::string Narrow(const wchar_t* text) {
    if (!text || !*text) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string out(size > 0 ? size - 1 : 0, '\0');
    if (size > 1) WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], size, nullptr, nullptr);
    return out;
}

// The voice's language from its token: Attributes\Language is a hex LCID list ("409;9").
std::string LanguageOf(ISpObjectToken* token) {
    std::string out;
    ISpDataKey* attributes = nullptr;
    if (SUCCEEDED(token->OpenKey(L"Attributes", &attributes)) && attributes) {
        WCHAR* language = nullptr;
        if (SUCCEEDED(attributes->GetStringValue(L"Language", &language)) && language) {
            unsigned long lcid = wcstoul(language, nullptr, 16);
            switch (lcid & 0x3FF) {
            case 0x09: out = "en"; break;
            case 0x0C: out = "fr"; break;
            case 0x07: out = "de"; break;
            case 0x0A: out = "es"; break;
            default: break;
            }
            CoTaskMemFree(language);
        }
        attributes->Release();
    }
    return out;
}

// Voices of one registry category (the classic SAPI one, then Windows 10's OneCore one).
void List(const wchar_t* category, const char* suffix, std::map<std::string, ISpObjectToken*>& tokens, std::vector<VoiceInfo>& out) {
    ISpObjectTokenCategory* list = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory, (void**)&list)) || !list) return;
    if (SUCCEEDED(list->SetId(category, FALSE))) {
        IEnumSpObjectTokens* items = nullptr;
        if (SUCCEEDED(list->EnumTokens(nullptr, nullptr, &items)) && items) {
            ISpObjectToken* token = nullptr;
            while (items->Next(1, &token, nullptr) == S_OK && token) {
                WCHAR* id = nullptr;
                WCHAR* name = nullptr;
                token->GetId(&id);
                token->GetStringValue(nullptr, &name);
                VoiceInfo voice;
                voice.id = Narrow(id);
                voice.name = Narrow(name ? name : id) + suffix;
                voice.language = LanguageOf(token);
                if (id) CoTaskMemFree(id);
                if (name) CoTaskMemFree(name);
                bool known = voice.id.empty() || tokens.count(voice.id);
                for (const auto& other : out) if (other.name + suffix == voice.name || other.name == voice.name) known = true;
                if (known) token->Release();
                else { tokens[voice.id] = token; out.push_back(voice); }
                token = nullptr;
            }
            items->Release();
        }
    }
    list->Release();
}

void Run() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ISpVoice* voice = nullptr;
    CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, (void**)&voice);
    std::map<std::string, ISpObjectToken*> tokens;
    std::vector<VoiceInfo> found;
    List(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices", "", tokens, found);
    List(L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices", " (OneCore)", tokens, found);
    { std::lock_guard<std::mutex> lock(mutex); voices = found; }
    std::string current = "\x01";
    for (;;) {
        Line line;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [] { return !running || !queue.empty(); });
            if (!running) break;
            line = queue.front();
            queue.pop_front();
        }
        if (!voice || GetTickCount64() - line.at > 4000) continue;
        if (line.voice != current) {
            auto it = tokens.find(line.voice);
            if (it == tokens.end() || FAILED(voice->SetVoice(it->second))) voice->SetVoice(nullptr);
            current = line.voice;
        }
        voice->SetVolume((USHORT)std::max(0, std::min(100, line.volume)));
        voice->SetRate(std::max(-10, std::min(10, line.rate)));
        voice->Speak(line.text.c_str(), SPF_IS_NOT_XML, nullptr); // Synchronous: this thread only reads.
    }
    for (auto& entry : tokens) entry.second->Release();
    if (voice) voice->Release();
    CoUninitialize();
}
} // namespace

void SpeechStart() {
    std::lock_guard<std::mutex> lock(mutex);
    if (running) return;
    running = true;
    worker = std::thread(Run);
}

void SpeechStop() {
    StopTones();
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running) return;
        running = false;
        queue.clear();
    }
    wake.notify_all();
    if (worker.joinable()) worker.join();
}

std::vector<VoiceInfo> SpeechVoices() {
    std::lock_guard<std::mutex> lock(mutex);
    return voices;
}

std::string SpeechVoiceFor(const std::string& language) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& voice : voices) if (voice.language == language) return voice.id;
    return "";
}

void Speak(const std::string& text, const std::string& voiceId, int volume, int rate) {
    if (text.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!running) return;
        if (queue.size() > 8) queue.pop_front();
        Line line;
        line.text = Widen(text);
        line.voice = voiceId;
        line.volume = volume;
        line.rate = rate;
        line.at = GetTickCount64();
        queue.push_back(line);
    }
    wake.notify_one();
}

void PlayAlertSound(int kind) {
    PlaySoundW(kind ? L"SystemExclamation" : L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
}

namespace {
std::mutex toneMutex;
// Every sound made, by id and volume, kept for the session: Windows reads it from here while it plays.
std::map<std::string, std::vector<uint8_t>> toneCache;
std::filesystem::path soundFolder;
}

void SetSoundFolder(const std::string& folder) {
    std::lock_guard<std::mutex> lock(toneMutex);
    soundFolder = std::filesystem::path(folder);
}

std::vector<std::string> SoundFiles() {
    std::filesystem::path folder;
    { std::lock_guard<std::mutex> lock(toneMutex); folder = soundFolder; }
    std::vector<std::string> names;
    std::error_code error;
    if (folder.empty() || !std::filesystem::is_directory(folder, error)) return names;
    std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, error), end;
    for (; !error && it != end; it.increment(error)) {
        if (it.depth() > 1) { it.disable_recursion_pending(); continue; }
        std::error_code fileError;
        if (!it->is_regular_file(fileError)) continue;
        std::wstring extension = it->path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (extension != L".wav" && extension != L".ogg") continue;
        std::string relative = std::filesystem::relative(it->path(), folder, fileError).u8string();
        std::replace(relative.begin(), relative.end(), '\\', '/');
        if (!fileError && !relative.empty()) names.push_back(relative);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void PlayTone(const std::string& id, int volume) {
    if (id.empty()) return;
    const bool file = id.rfind("file:", 0) == 0;
    std::filesystem::path path;
    const uint8_t* data = nullptr;
    {
        std::lock_guard<std::mutex> lock(toneMutex);
        if (file) {
            if (soundFolder.empty()) return;
            path = soundFolder / std::filesystem::u8path(id.substr(5));
        }
        std::string key = id + "|" + std::to_string(volume);
        auto it = toneCache.find(key);
        if (it == toneCache.end()) {
            std::vector<uint8_t> wav;
            if (file) {
                // Ogg Vorbis or 16-bit WAV, decoded once and scaled to the volume.
                std::ifstream in(path, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                wav = soundfile::ToWav(bytes, volume);
            } else wav = tones::Wav(id, volume);
            it = toneCache.emplace(key, std::move(wav)).first;
        }
        if (!it->second.empty()) data = it->second.data();
    }
    if (data) PlaySoundW((LPCWSTR)data, nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    // A WAV the decoder does not read (24-bit, float…): Windows plays the file itself, at its own volume.
    else if (file) PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

void StopTones() { PlaySoundW(nullptr, nullptr, 0); }
