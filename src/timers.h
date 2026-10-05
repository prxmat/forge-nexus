// Encounter timers in Blish HUD's format (.bhtimer), the one Hero's Timers ship and TaimiHUD reads: the files, and
// one machine per timer that follows the player through the fight's phases and says what to show and to read out.
// Portable (no Windows): tools/timers-check builds it natively against a timer pack.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace timers {

struct Vec3 { float x = 0, y = 0, z = 0; };

// A trigger point in MumbleLink's frame (x, y up, z). A flat one only has the ground: x and z.
struct Point { Vec3 v; bool flat = false; };

struct Trigger {
    bool present = false;
    bool key = false;    // "type": "key": also needs a press of trigger key keyIndex.
    int keyIndex = 0;
    bool hasPosition = false, hasAntipode = false, hasRadius = false;
    Point position, antipode;
    float radius = 0;
    bool requireCombat = false, requireOutOfCombat = false, requireEntry = false, requireDeparture = false;
};

struct Colour { uint8_t r = 255, g = 255, b = 255; bool set = false; };

// A warning counts down to each timestamp; an alert shows from the timestamp on, without a countdown.
struct Alert {
    std::string warning, alert, icon, set;
    bool hasWarning = false, hasAlert = false;
    float warningDuration = 0, alertDuration = 0;
    Colour warningColour, alertColour, fill;
    std::vector<float> timestamps;
};

// Blish reads these out (text-to-speech) at each timestamp.
struct Sound { std::string text, set; std::vector<float> timestamps; };

// skipTime: the phase's timeline jumps ahead, for the sets named (none: the default set).
struct Action { std::string name; std::vector<std::string> sets; Trigger trigger; float skip = 0; };

struct Phase {
    std::string name;
    Trigger start, finish;
    std::vector<Alert> alerts;
    std::vector<Sound> sounds;
    std::vector<Action> actions;
};

struct TimerFile {
    std::string id, name, category, description, author, icon;
    std::string path, folder, source; // source: where it was found ("Forge", "TaimiHUD").
    uint32_t map = 0;
    Trigger reset;
    std::vector<Phase> phases;
    bool usesKeys = false; // Some trigger waits for a trigger key.
    std::vector<int> keys; // Which ones.
    // The name's first line is the area; the rest says the boss and what the timer covers.
    std::string Area() const;
    std::string Title() const;
};

// One .bhtimer (lenient JSON: comments and trailing commas pass). False with the reason when it is not a timer.
bool Parse(const std::string& text, TimerFile& out, std::string& error);
// Every .bhtimer under a folder, recursively. Icons are resolved to absolute paths when the file exists.
void LoadFolder(const std::string& folder, const std::string& source, std::vector<std::shared_ptr<const TimerFile>>& out, std::vector<std::string>& errors);

enum class Combat { Outside, Entered, Exited };

// What a running phase shows this frame.
struct Bar {
    const TimerFile* timer = nullptr;
    std::string text, icon;
    Colour fill, colour;
    float remaining = 0; // Warning: seconds to the timestamp. Alert: seconds of display left.
    float duration = 0;
    bool warning = true;
};

// Something that happened this frame, once: read it out, flash it, sound it. Tick: a whole second of a warning's
// last ten (seconds = how many are left), for countdown sounds; Due: the warning's moment.
struct Shout {
    enum Kind { Sound, Alert, Warning, Tick, Due, Reset };
    Kind kind = Sound;
    const TimerFile* timer = nullptr;
    std::string text, icon;
    Colour colour;
    float duration = 0;  // How long it stays on screen.
    int seconds = 0;     // Tick: seconds left.
};

class Machine {
public:
    enum class State { OnMap, OnPhase, BetweenPhases, Finished };
    explicit Machine(std::shared_ptr<const TimerFile> file) : file(std::move(file)) {}
    const TimerFile& File() const { return *file; }
    std::shared_ptr<const TimerFile> Shared() const { return file; }
    State GetState() const { return state; }
    int PhaseIndex() const { return phase; }
    double Elapsed(double now) const { return state == State::OnPhase ? now - phaseStart : 0; }
    Combat GetCombat() const { return combat; }
    void SetCombat(Combat value) { combat = value; }
    void Press(int key) { if (key >= 0 && key < 8) keys |= 1u << key; }
    void Release(int key) { if (key >= 0 && key < 8) keys &= ~(1u << key); }
    void Reset();
    // now: seconds on a steady clock; pos: the player in MumbleLink's frame (metres). warnAt: a warning is read out
    // this many seconds before its timestamp (0: never). Adds the frame's bars and what happened.
    void Tick(double now, const Vec3& pos, float warnAt, std::vector<Bar>& bars, std::vector<Shout>& shouts);
private:
    bool Check(const Trigger& trigger, const Vec3& pos);
    void StartPhase(int index, double now);
    void Emit(double now, float warnAt, std::vector<Bar>& bars, std::vector<Shout>& shouts);
    double Skipped(const std::map<std::string, double>& skips, const std::string& set) const;
    std::shared_ptr<const TimerFile> file;
    State state = State::OnMap;
    int phase = -1;
    double phaseStart = 0, lastNow = 0;
    Combat combat = Combat::Outside;
    uint32_t keys = 0;
    std::map<std::string, double> skip, lastSkip;
};

// Whether a point is inside a trigger's zone (sphere, or box between position and antipode). No zone: false.
bool Within(const Trigger& trigger, const Vec3& pos);

} // namespace timers
