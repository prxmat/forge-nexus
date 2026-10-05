#include "timers.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.hpp"

namespace timers {
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
float Number(const json& value, float fallback = 0) {
    if (value.is_number()) return value.get<float>();
    if (value.is_string()) { try { return std::stof(value.get<std::string>()); } catch (...) {} }
    return fallback;
}

std::string Text(const json& object, const char* key) {
    auto it = object.find(key);
    if (it == object.end()) return "";
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    if (it->is_number()) { std::ostringstream out; out << it->get<double>(); return out.str(); }
    return "";
}

bool Flag(const json& object, const char* key) {
    auto it = object.find(key);
    if (it == object.end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0;
    if (it->is_string()) { std::string text = it->get<std::string>(); return text == "true" || text == "1"; }
    return false;
}

// Blish's [x, y, z] has z up, MumbleLink has y up: swap. [x, y] is the ground (MumbleLink's x and z).
bool ReadPoint(const json& value, Point& out) {
    if (!value.is_array() || value.size() < 2) return false;
    for (const auto& component : value) if (!component.is_number()) return false;
    if (value.size() >= 3) { out.v = { value[0].get<float>(), value[2].get<float>(), value[1].get<float>() }; out.flat = false; }
    else { out.v = { value[0].get<float>(), 0, value[1].get<float>() }; out.flat = true; }
    return true;
}

Colour ReadColour(const json& object, const char* key) {
    Colour colour;
    auto it = object.find(key);
    if (it == object.end() || !it->is_array() || it->size() < 3) return colour;
    auto channel = [](const json& value) { return (uint8_t)std::max(0.0f, std::min(255.0f, Number(value, 255))); };
    colour.r = channel((*it)[0]);
    colour.g = channel((*it)[1]);
    colour.b = channel((*it)[2]);
    colour.set = true;
    return colour;
}

Trigger ReadTrigger(const json& object) {
    Trigger trigger;
    if (!object.is_object()) return trigger;
    trigger.present = true;
    trigger.key = Text(object, "type") == "key";
    std::string key = Text(object, "keyBind");
    if (key.empty()) key = Text(object, "keyBinds");
    if (!key.empty()) { try { trigger.keyIndex = std::max(0, std::min(7, std::stoi(key))); } catch (...) { trigger.keyIndex = 0; } }
    if (auto it = object.find("position"); it != object.end()) trigger.hasPosition = ReadPoint(*it, trigger.position);
    if (auto it = object.find("antipode"); it != object.end()) trigger.hasAntipode = ReadPoint(*it, trigger.antipode);
    if (auto it = object.find("radius"); it != object.end() && (it->is_number() || it->is_string())) { trigger.radius = Number(*it); trigger.hasRadius = true; }
    trigger.requireCombat = Flag(object, "requireCombat");
    trigger.requireOutOfCombat = Flag(object, "requireOutOfCombat");
    trigger.requireEntry = Flag(object, "requireEntry");
    trigger.requireDeparture = Flag(object, "requireDeparture");
    return trigger;
}

std::vector<float> Timestamps(const json& object) {
    std::vector<float> out;
    auto it = object.find("timestamps");
    if (it == object.end()) return out;
    if (it->is_array()) { for (const auto& value : *it) if (value.is_number() || value.is_string()) out.push_back(Number(value)); }
    else if (it->is_number()) out.push_back(it->get<float>());
    return out;
}

float Seconds(const json& object, const char* key, bool& has) {
    auto it = object.find(key);
    has = it != object.end() && (it->is_number() || it->is_string());
    return has ? Number(*it) : 0;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t start = line.find_first_not_of(' ');
        if (start == std::string::npos) continue;
        out.push_back(line.substr(start));
    }
    return out;
}

void NoteKeys(TimerFile& file, const Trigger& trigger) {
    if (!trigger.present || !trigger.key) return;
    file.usesKeys = true;
    if (std::find(file.keys.begin(), file.keys.end(), trigger.keyIndex) == file.keys.end()) file.keys.push_back(trigger.keyIndex);
}

// Icons are relative to the pack's root: the file's folder, or one of its parents when the timers sit in sub-folders.
std::string ResolveIcon(const fs::path& folder, const std::string& icon) {
    if (icon.empty()) return "";
    std::error_code error;
    fs::path at = folder;
    for (int depth = 0; depth < 4 && !at.empty(); depth++) {
        fs::path candidate = at / fs::path(icon);
        if (fs::is_regular_file(candidate, error)) return candidate.string();
        if (!at.has_parent_path() || at.parent_path() == at) break;
        at = at.parent_path();
    }
    return "";
}
} // namespace

std::string TimerFile::Area() const {
    std::vector<std::string> lines = Lines(name);
    return lines.size() > 1 ? lines.front() : category;
}

std::string TimerFile::Title() const {
    std::vector<std::string> lines = Lines(name);
    if (lines.empty()) return id;
    if (lines.size() == 1) return lines.front();
    std::string out;
    for (size_t index = 1; index < lines.size(); index++) out += (index > 1 ? " · " : "") + lines[index];
    return out;
}

bool Parse(const std::string& raw, TimerFile& out, std::string& error) {
    std::string text = raw;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
    json data = json::parse(text, nullptr, false, true, true);
    if (data.is_discarded() || !data.is_object()) { error = "JSON illisible"; return false; }
    out.id = Text(data, "id");
    out.name = Text(data, "name");
    out.category = Text(data, "category");
    out.description = Text(data, "description");
    out.author = Text(data, "author");
    out.icon = Text(data, "icon");
    if (auto it = data.find("map"); it != data.end()) out.map = (uint32_t)Number(*it, 0);
    if (auto it = data.find("reset"); it != data.end()) out.reset = ReadTrigger(*it);
    NoteKeys(out, out.reset);
    auto phases = data.find("phases");
    if (phases == data.end() || !phases->is_array() || phases->empty()) { error = "aucune phase"; return false; }
    for (const auto& item : *phases) {
        if (!item.is_object()) continue;
        Phase phase;
        phase.name = Text(item, "name");
        if (auto it = item.find("start"); it != item.end()) phase.start = ReadTrigger(*it);
        if (auto it = item.find("finish"); it != item.end()) phase.finish = ReadTrigger(*it);
        NoteKeys(out, phase.start);
        NoteKeys(out, phase.finish);
        if (auto it = item.find("alerts"); it != item.end() && it->is_array()) {
            for (const auto& entry : *it) {
                if (!entry.is_object()) continue;
                Alert alert;
                bool warningSeconds = false, alertSeconds = false;
                alert.warning = Text(entry, "warning");
                alert.alert = Text(entry, "alert");
                alert.warningDuration = Seconds(entry, "warningDuration", warningSeconds);
                alert.alertDuration = Seconds(entry, "alertDuration", alertSeconds);
                // As TaimiHUD: a warning (an alert) needs both its text and its duration.
                alert.hasWarning = entry.contains("warning") && warningSeconds && alert.warningDuration > 0;
                alert.hasAlert = entry.contains("alert") && alertSeconds && alert.alertDuration > 0;
                alert.icon = Text(entry, "icon");
                alert.set = Text(entry, "set");
                alert.warningColour = ReadColour(entry, "warningColor");
                alert.alertColour = ReadColour(entry, "alertColor");
                alert.fill = ReadColour(entry, "fillColor");
                alert.timestamps = Timestamps(entry);
                if ((alert.hasWarning || alert.hasAlert) && !alert.timestamps.empty()) phase.alerts.push_back(alert);
            }
        }
        if (auto it = item.find("sounds"); it != item.end() && it->is_array()) {
            for (const auto& entry : *it) {
                if (!entry.is_object()) continue;
                Sound sound;
                sound.text = Text(entry, "text");
                sound.set = Text(entry, "set");
                sound.timestamps = Timestamps(entry);
                if (!sound.text.empty() && !sound.timestamps.empty()) phase.sounds.push_back(sound);
            }
        }
        if (auto it = item.find("actions"); it != item.end() && it->is_array()) {
            for (const auto& entry : *it) {
                if (!entry.is_object()) continue;
                std::string type = Text(entry, "type");
                if (!type.empty() && type != "skipTime") continue;
                Action action;
                action.name = Text(entry, "name");
                if (auto sets = entry.find("sets"); sets != entry.end() && sets->is_array()) for (const auto& set : *sets) if (set.is_string()) action.sets.push_back(set.get<std::string>());
                if (auto trigger = entry.find("trigger"); trigger != entry.end()) action.trigger = ReadTrigger(*trigger);
                if (auto time = entry.find("time"); time != entry.end()) action.skip = Number(*time);
                NoteKeys(out, action.trigger);
                if (action.trigger.present && action.skip > 0) phase.actions.push_back(action);
            }
        }
        out.phases.push_back(phase);
    }
    if (out.phases.empty()) { error = "aucune phase lisible"; return false; }
    if (out.id.empty()) { error = "pas d'id"; return false; }
    if (!out.map) { error = "pas de carte"; return false; }
    std::sort(out.keys.begin(), out.keys.end());
    return true;
}

void LoadFolder(const std::string& folder, const std::string& source, std::vector<std::shared_ptr<const TimerFile>>& out, std::vector<std::string>& errors) {
    std::error_code error;
    fs::path root(folder);
    if (folder.empty() || !fs::is_directory(root, error)) return;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
    for (; !error && it != end; it.increment(error)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError)) continue;
        const fs::path& path = it->path();
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (extension != ".bhtimer") continue;
        std::ifstream file(path, std::ios::binary);
        if (!file) { errors.push_back(path.filename().string() + " : illisible"); continue; }
        std::stringstream buffer;
        buffer << file.rdbuf();
        auto timer = std::make_shared<TimerFile>();
        std::string reason;
        if (!Parse(buffer.str(), *timer, reason)) { errors.push_back(path.filename().string() + " : " + reason); continue; }
        timer->path = path.string();
        timer->folder = path.parent_path().string();
        timer->source = source;
        timer->icon = ResolveIcon(path.parent_path(), timer->icon);
        for (auto& phase : timer->phases) for (auto& alert : phase.alerts) alert.icon = ResolveIcon(path.parent_path(), alert.icon);
        out.push_back(timer);
    }
}

bool Within(const Trigger& trigger, const Vec3& pos) {
    if (trigger.hasPosition && trigger.hasRadius) {
        const Point& centre = trigger.position;
        float dx = pos.x - centre.v.x, dz = pos.z - centre.v.z, dy = centre.flat ? 0.0f : pos.y - centre.v.y;
        return std::sqrt(dx * dx + dy * dy + dz * dz) < trigger.radius;
    }
    if (trigger.hasPosition && trigger.hasAntipode) {
        const Vec3& a = trigger.position.v;
        const Vec3& b = trigger.antipode.v;
        bool flat = trigger.position.flat || trigger.antipode.flat;
        if (pos.x < std::min(a.x, b.x) || pos.x > std::max(a.x, b.x)) return false;
        if (pos.z < std::min(a.z, b.z) || pos.z > std::max(a.z, b.z)) return false;
        if (!flat && (pos.y < std::min(a.y, b.y) || pos.y > std::max(a.y, b.y))) return false;
        return true;
    }
    return false;
}

// TaimiHUD's check, in its order: no zone fails; a key trigger uses up the press even when the rest fails.
bool Machine::Check(const Trigger& trigger, const Vec3& pos) {
    if (!trigger.present) return false;
    if (!(trigger.hasPosition && (trigger.hasRadius || trigger.hasAntipode))) return false;
    bool keyOk = true;
    if (trigger.key) {
        uint32_t flag = 1u << trigger.keyIndex;
        keyOk = (keys & flag) != 0;
        keys &= ~flag;
    }
    bool inside = Within(trigger, pos);
    bool combatOk = (!trigger.requireCombat || combat == Combat::Entered) && (!trigger.requireOutOfCombat || combat == Combat::Exited);
    bool entryOk = !trigger.requireEntry || inside;
    bool departureOk = !trigger.requireDeparture || !inside;
    return keyOk && combatOk && entryOk && departureOk;
}

void Machine::Reset() {
    state = State::OnMap;
    phase = -1;
    combat = Combat::Outside;
    skip.clear();
    lastSkip.clear();
}

void Machine::StartPhase(int index, double now) {
    state = State::OnPhase;
    phase = index;
    phaseStart = now;
    skip.clear();
    lastSkip.clear();
    lastNow = now - 0.001; // Timestamps at 0 fire on this very tick.
}

double Machine::Skipped(const std::map<std::string, double>& skips, const std::string& set) const {
    auto it = skips.find(set == "default" ? "" : set);
    return it == skips.end() ? 0.0 : it->second;
}

void Machine::Tick(double now, const Vec3& pos, float warnAt, std::vector<Bar>& bars, std::vector<Shout>& shouts) {
    if (state != State::OnMap && Check(file->reset, pos)) {
        Reset();
        Shout shout;
        shout.kind = Shout::Reset;
        shout.timer = file.get();
        shout.text = file->Title();
        shout.duration = 2.0f;
        shouts.push_back(shout);
        lastNow = now;
        return;
    }
    const int count = (int)file->phases.size();
    switch (state) {
    case State::OnMap:
        if (count && Check(file->phases[0].start, pos)) StartPhase(0, now);
        break;
    case State::OnPhase: {
        const Phase& current = file->phases[phase];
        if (current.finish.present && Check(current.finish, pos)) { state = phase + 1 < count ? State::BetweenPhases : State::Finished; break; }
        for (const Action& action : current.actions) {
            if (!Check(action.trigger, pos)) continue;
            if (action.sets.empty()) skip[""] += action.skip;
            else for (const auto& set : action.sets) skip[set == "default" ? "" : set] += action.skip;
        }
        break;
    }
    case State::BetweenPhases:
        if (phase + 1 < count && Check(file->phases[phase + 1].start, pos)) StartPhase(phase + 1, now);
        break;
    case State::Finished:
        break;
    }
    if (state == State::OnPhase) Emit(now, warnAt, bars, shouts);
    lastNow = now;
    lastSkip = skip;
}

// The phase's bars now, and what its timeline crossed since the last tick. A jump (skipTime) passes over its events
// quietly: only a crossing less than 1.5 s old is announced.
void Machine::Emit(double now, float warnAt, std::vector<Bar>& bars, std::vector<Shout>& shouts) {
    const Phase& current = file->phases[phase];
    auto elapsed = [&](const std::string& set) { return now - phaseStart + Skipped(skip, set); };
    auto crossed = [&](const std::string& set, double at) {
        double before = lastNow - phaseStart + Skipped(lastSkip, set), after = elapsed(set);
        return before < at && at <= after && after - at < 1.5;
    };
    for (const Alert& alert : current.alerts) {
        double t = elapsed(alert.set);
        for (float stamp : alert.timestamps) {
            if (alert.hasWarning) {
                if (t >= stamp - alert.warningDuration && t < stamp) {
                    Bar bar;
                    bar.timer = file.get(); bar.text = alert.warning; bar.icon = alert.icon; bar.fill = alert.fill; bar.colour = alert.warningColour;
                    bar.remaining = (float)(stamp - t); bar.duration = alert.warningDuration; bar.warning = true;
                    bars.push_back(bar);
                }
                if (warnAt > 0) {
                    double lead = std::min<double>(warnAt, alert.warningDuration);
                    if (crossed(alert.set, stamp - lead)) {
                        Shout shout;
                        shout.kind = Shout::Warning; shout.timer = file.get(); shout.text = alert.warning; shout.icon = alert.icon; shout.colour = alert.warningColour; shout.duration = (float)lead;
                        shouts.push_back(shout);
                    }
                }
                // The last ten whole seconds inside the warning's own window, for the countdown sounds.
                for (int left = 1; left <= 10 && stamp - left >= stamp - alert.warningDuration - 0.001; left++) {
                    if (!crossed(alert.set, stamp - left)) continue;
                    Shout shout;
                    shout.kind = Shout::Tick; shout.timer = file.get(); shout.text = alert.warning; shout.icon = alert.icon; shout.seconds = left;
                    shouts.push_back(shout);
                }
                if (crossed(alert.set, stamp)) {
                    Shout shout;
                    shout.kind = Shout::Due; shout.timer = file.get(); shout.text = alert.warning; shout.icon = alert.icon;
                    shouts.push_back(shout);
                }
            }
            if (alert.hasAlert) {
                if (t >= stamp && t < stamp + alert.alertDuration) {
                    Bar bar;
                    bar.timer = file.get(); bar.text = alert.alert; bar.icon = alert.icon; bar.fill = alert.fill; bar.colour = alert.alertColour;
                    bar.remaining = (float)(stamp + alert.alertDuration - t); bar.duration = alert.alertDuration; bar.warning = false;
                    bars.push_back(bar);
                }
                if (crossed(alert.set, stamp)) {
                    Shout shout;
                    shout.kind = Shout::Alert; shout.timer = file.get(); shout.text = alert.alert; shout.icon = alert.icon; shout.colour = alert.alertColour; shout.duration = alert.alertDuration;
                    shouts.push_back(shout);
                }
            }
        }
    }
    for (const Sound& sound : current.sounds) {
        for (float stamp : sound.timestamps) {
            if (!crossed(sound.set, stamp)) continue;
            Shout shout;
            shout.kind = Shout::Sound; shout.timer = file.get(); shout.text = sound.text; shout.duration = 3.0f;
            shouts.push_back(shout);
        }
    }
}

} // namespace timers
