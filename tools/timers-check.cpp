// Native check of the timers engine against a timer pack (Hero's Timers): every file must load, and three fights
// are played through the machine. Build from the repository root:
//   clang++ -std=c++17 -Isrc tools/timers-check.cpp src/timers.cpp src/text.cpp -o build/native/timers-check
//   build/native/timers-check /path/to/pack
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "timers.h"

using namespace timers;

static int failures = 0;
static void Expect(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (!ok) failures++;
}

static std::shared_ptr<const TimerFile> Find(const std::vector<std::shared_ptr<const TimerFile>>& all, const std::string& id) {
    for (const auto& file : all) if (file->id == id) return file;
    return nullptr;
}

// The middle of a trigger's zone, in MumbleLink's frame.
static Vec3 Inside(const Trigger& trigger) {
    if (trigger.hasRadius) return trigger.position.v;
    Vec3 a = trigger.position.v, b = trigger.antipode.v;
    return { (a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2 };
}

struct Run {
    std::vector<Bar> bars;
    std::vector<Shout> shouts;
};
static Run Step(Machine& machine, double now, const Vec3& pos, float warnAt = 3) {
    Run run;
    machine.Tick(now, pos, warnAt, run.bars, run.shouts);
    return run;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: timers-check <pack folder>\n"); return 2; }
    std::vector<std::shared_ptr<const TimerFile>> all;
    std::vector<std::string> errors;
    LoadFolder(argv[1], "test", all, errors);
    std::printf("%zu timers, %zu errors\n", all.size(), errors.size());
    for (const auto& error : errors) std::printf("  error: %s\n", error.c_str());
    int alerts = 0, sounds = 0, actions = 0, keyed = 0, icons = 0, missingIcons = 0;
    std::map<uint32_t, int> maps;
    for (const auto& file : all) {
        maps[file->map]++;
        if (file->usesKeys) keyed++;
        for (const auto& phase : file->phases) {
            alerts += (int)phase.alerts.size();
            sounds += (int)phase.sounds.size();
            actions += (int)phase.actions.size();
            for (const auto& alert : phase.alerts) (alert.icon.empty() ? missingIcons : icons)++;
        }
    }
    std::printf("%d alerts (%d with an icon found, %d without), %d sounds, %d skipTime actions, %d timers with trigger keys, %zu maps\n", alerts, icons, missingIcons, sounds, actions, keyed, maps.size());
    Expect(errors.empty(), "every file parses");
    Expect(all.size() >= 50, "the whole pack loads");

    // Slothasor: automatic. Combat in the arena starts it; the mushroom warning counts down to 50 s.
    if (auto file = Find(all, "htp.raid.w2.b1")) {
        std::printf("%s / %s (map %u)\n", file->Area().c_str(), file->Title().c_str(), file->map);
        Machine machine(file);
        Vec3 arena = Inside(file->phases[0].start);
        Run run = Step(machine, 0, arena);
        Expect(machine.GetState() == Machine::State::OnMap, "no combat yet: waits");
        machine.SetCombat(Combat::Entered);
        run = Step(machine, 1, arena);
        Expect(machine.GetState() == Machine::State::OnPhase, "combat in the arena: phase 1 starts");
        bool sawStartAlert = false;
        for (const auto& shout : run.shouts) if (shout.kind == Shout::Alert && shout.text == "Timer: Global Mechanics") sawStartAlert = true;
        Expect(sawStartAlert, "the alert at 0 s fires on the first tick");
        Run at10 = Step(machine, 11, arena);
        const Bar* mushroom = nullptr;
        for (const auto& bar : at10.bars) if (bar.warning && bar.text == "Next: Mushroom #2") mushroom = &bar;
        Expect(mushroom && std::fabs(mushroom->remaining - 40.0f) < 0.01f, "at 10 s, « Next: Mushroom #2 » shows 40 s left");
        Expect(mushroom && !mushroom->icon.empty(), "the mushroom icon is found in the pack");
        // Read out at 3 s before 50 s, once.
        int warned = 0, due = 0, spawned = 0;
        std::vector<int> ticks;
        for (double t = 11.1; t <= 52.0; t += 0.1) {
            Run step = Step(machine, t, arena);
            for (const auto& shout : step.shouts) {
                if (shout.kind == Shout::Tick && shout.text == "Next: Mushroom #2") ticks.push_back(shout.seconds);
                if (shout.kind == Shout::Warning && shout.text == "Next: Mushroom #2") warned++;
                if (shout.kind == Shout::Due && shout.text == "Next: Mushroom #2") due++;
                if (shout.kind == Shout::Alert && shout.text == "Spawned: Mushroom #2") spawned++;
            }
        }
        Expect(warned == 1 && due == 1 && spawned == 1, "mushroom #2: one read-out 3 s before, one beep when due, one alert");
        Expect(ticks == std::vector<int>({ 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 }), "mushroom #2: one countdown tick per second, 10 down to 1");
        // The fight ends out of combat at the finish point, then the reset zone rearms it.
        machine.SetCombat(Combat::Exited);
        Step(machine, 60, Inside(file->phases[0].finish));
        Expect(machine.GetState() == Machine::State::Finished, "out of combat at the finish point: finished");
        Run reset = Step(machine, 61, Inside(file->reset));
        bool sawReset = false;
        for (const auto& shout : reset.shouts) if (shout.kind == Shout::Reset) sawReset = true;
        Expect(machine.GetState() == Machine::State::OnMap && sawReset, "the reset zone rearms the timer");
    } else Expect(false, "Slothasor timer present");

    // Deimos oils: a key timer. In combat in the zone, nothing until trigger key 0.
    if (auto file = Find(all, "htp.raid.w4.b4.oils")) {
        Machine machine(file);
        Vec3 zone = Inside(file->phases[0].start);
        machine.SetCombat(Combat::Entered);
        Step(machine, 0, zone);
        Expect(machine.GetState() == Machine::State::OnMap, "Deimos: waits for the key");
        machine.Press(1);
        Step(machine, 1, zone);
        Expect(machine.GetState() == Machine::State::OnMap, "Deimos: another key does nothing");
        machine.Release(1);
        machine.Press(0);
        Run started = Step(machine, 2, zone);
        Expect(machine.GetState() == Machine::State::OnPhase, "Deimos: key 0 starts the oils");
        // « Move! » at 2.5 s lasts 2.5 s: its countdown has 2 and 1 only.
        std::vector<int> moveTicks;
        for (double t = 2.05; t <= 5.0; t += 0.05) {
            Run step = Step(machine, t, zone);
            for (const auto& shout : step.shouts) if (shout.kind == Shout::Tick && shout.text == "Move!") moveTicks.push_back(shout.seconds);
        }
        Expect(moveTicks == std::vector<int>({ 2, 1 }), "Deimos: a 2.5 s warning counts down 2, 1 only");
        // The press was used up: holding it does not finish the phase at once.
        Step(machine, 5.1, zone);
        Expect(machine.GetState() == Machine::State::OnPhase, "Deimos: the press is used up by the start");
        machine.Release(0);
        machine.Press(0);
        Step(machine, 30, zone);
        Expect(machine.GetState() == Machine::State::Finished, "Deimos: key 0 again ends it");
    } else Expect(false, "Deimos oils timer present");

    // Temple of Febe CM: skipTime on key 4 moves Cerus' set ahead by a second.
    std::shared_ptr<const TimerFile> febe;
    for (const auto& file : all) for (const auto& phase : file->phases) for (const auto& action : phase.actions) if (!febe && action.trigger.key) febe = file;
    if (febe) {
        Machine machine(febe);
        const Phase& first = febe->phases[0];
        const Action* action = nullptr;
        for (const auto& candidate : first.actions) if (!action) action = &candidate;
        Vec3 zone = Inside(first.start);
        machine.SetCombat(Combat::Entered);
        // Start: as the file wants it (key or not).
        if (first.start.key) machine.Press(first.start.keyIndex);
        Step(machine, 0, zone);
        if (first.start.key) machine.Release(first.start.keyIndex);
        Expect(machine.GetState() == Machine::State::OnPhase, febe->Title() + ": phase 1 starts");
        if (action) {
            std::string set = action->sets.empty() ? "" : action->sets.front();
            const Alert* alert = nullptr;
            for (const auto& candidate : first.alerts) if (!alert && candidate.hasWarning && candidate.set == set) alert = &candidate;
            if (alert) {
                float stamp = alert->timestamps.front();
                double probe = std::max(0.5, stamp - alert->warningDuration + 0.5);
                Run before = Step(machine, probe, zone);
                float left = -1;
                for (const auto& bar : before.bars) if (bar.text == alert->warning && bar.warning) left = bar.remaining;
                machine.Press(action->trigger.keyIndex);
                Run after = Step(machine, probe + 0.1, Inside(action->trigger));
                machine.Release(action->trigger.keyIndex);
                float leftAfter = -1;
                for (const auto& bar : after.bars) if (bar.text == alert->warning && bar.warning) leftAfter = bar.remaining;
                Expect(left > 0 && leftAfter > 0 && std::fabs((left - leftAfter) - (0.1f + action->skip)) < 0.05f, "skipTime moves « " + alert->warning + " » " + std::to_string(action->skip) + " s ahead");
            } else std::printf("  (no warning in set « %s » to probe)\n", set.c_str());
        }
    } else std::printf("  (no timer with a key action in this pack)\n");

    std::printf(failures ? "%d FAILED\n" : "all good\n", failures);
    return failures ? 1 : 0;
}
