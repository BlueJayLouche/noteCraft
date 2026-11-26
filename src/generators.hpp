// Effects that play notes of their own from the keys held: Arpeggiator, Latch.
#pragma once

#include "timing.hpp" // transportTempo

namespace notecraft {

inline constexpr const char* kArpModes[] = {"Up", "Down", "Up/Down", "Random", "As played"};
inline constexpr const char* kArpRates[] = {"1/4", "1/8", "1/8 T", "1/16", "1/16 T", "1/32"};
inline constexpr double kArpRateBeats[] = {1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125};

// Arpeggiator: plays the held keys one at a time at Rate, over Octaves, each for Gate of a step. While the host
// plays, steps fall on its beat grid; while it is stopped, the first key starts the steps at once.
struct Arpeggiator : ScheduledEffect<Arpeggiator, 4> {
    static constexpr const char* id = "com.notecraft.arpeggiator";
    static constexpr const char* name = "Arpeggiator";
    static constexpr const char* description = "Plays the held notes one after another, in time with the host.";
    enum { Mode, Rate, Octaves, Gate };
    static constexpr std::array<Param, 4> params{{
        {"Mode", 0.0, 4.0, 0.0, Unit::Choice, kArpModes},
        {"Rate", 0.0, 5.0, 3.0, Unit::Choice, kArpRates},
        {"Octaves", 1.0, 4.0, 1.0, Unit::Number},
        {"Gate", 0.05, 1.0, 0.5, Unit::Percent},
    }};

    Rng rng;
    std::array<Note, 32> held{}; // in the order they were pressed
    std::size_t heldCount = 0;
    std::uint32_t step = 0;
    double nextStep = -1.0; // absolute sample of the next step; negative while no key is held
    double lastStep = -1.0e18;
    double samplesPerBeat = 24000.0;
    bool synced = false;     // the host plays and gives its beat position
    double beatAtBlock = 0.0; // the host's beat at this block's start

    void clearState()
    {
        heldCount = 0;
        nextStep = -1.0;
        lastStep = -1.0e18;
        step = 0;
    }
    void releaseAllNotes() { clearState(); }

    double rateBeats(const Values<4>& p) const { return kArpRateBeats[std::clamp(choice(p, Rate), 0, 5)]; }

    // The first step of the host's grid at or after t; one that would come within half a step of the last step
    // played is the step already played, so the next one is taken.
    double gridAfter(double t, const Values<4>& p) const
    {
        const double rate = rateBeats(p);
        const double beat = beatAtBlock + (t - static_cast<double>(now)) / samplesPerBeat;
        double at = t + (std::ceil(beat / rate - 1e-9) * rate - beat) * samplesPerBeat;
        if (at < lastStep + 0.5 * rate * samplesPerBeat) at += rate * samplesPerBeat;
        return at;
    }

    void block(const clap_event_transport_t* t, const Values<4>& p)
    {
        samplesPerBeat = 60.0 / transportTempo(t) * sampleRate;
        synced = t != nullptr && (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0 &&
                 (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0;
        if (synced) {
            beatAtBlock = static_cast<double>(t->song_pos_beats) / static_cast<double>(CLAP_BEATTIME_FACTOR);
            if (heldCount > 0) nextStep = gridAfter(static_cast<double>(now), p); // follows tempo changes and jumps
        }
    }

    void advance(std::int64_t until, const Values<4>& p)
    {
        if (heldCount == 0 || nextStep < 0.0) return;
        const double length = std::max(rateBeats(p) * samplesPerBeat, 1.0);
        while (nextStep < static_cast<double>(until)) {
            play(static_cast<std::int64_t>(nextStep), length, p);
            lastStep = nextStep;
            nextStep += length;
        }
    }

    void play(std::int64_t at, double length, const Values<4>& p)
    {
        const int mode = std::clamp(choice(p, Mode), 0, 4);
        std::array<Note, 32> base{};
        std::copy(held.begin(), held.begin() + static_cast<std::ptrdiff_t>(heldCount), base.begin());
        auto* last = base.begin() + static_cast<std::ptrdiff_t>(heldCount);
        if (mode != 4) {
            std::stable_sort(base.begin(), last, [](const Note& a, const Note& b) { return a.key < b.key; });
        }
        const int octaves = std::clamp(choice(p, Octaves), 1, 4);
        std::array<Note, 256> order{};
        std::size_t count = 0;
        for (int o = 0; o < octaves; ++o) {
            for (auto* n = base.begin(); n != last; ++n) {
                if (n->key + 12 * o > 127) continue;
                order[count] = *n;
                order[count++].key = static_cast<std::int16_t>(n->key + 12 * o);
            }
        }
        if (count == 0) return;
        if (mode == 1) std::reverse(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(count));
        if (mode == 2) { // up, then down without repeating the top and bottom notes
            for (std::size_t i = count - 1; i > 1 && count < order.size(); --i) order[count++] = order[i - 1];
        }
        Note n = order[mode == 3 ? static_cast<std::size_t>(rng.below(static_cast<int>(count))) : step % count];
        ++step;
        n.id = -1;
        if (out.on(at, n)) {
            out.off(at + std::max<std::int64_t>(1, static_cast<std::int64_t>(length * get(p, Gate))), n);
        }
    }

    void noteOn(std::int64_t t, const Note& n, const Values<4>& p)
    {
        for (std::size_t i = 0; i < heldCount; ++i) {
            if (held[i].channel == n.channel && held[i].key == n.key) {
                held[i].velocity = n.velocity;
                return;
            }
        }
        if (heldCount == held.size()) return;
        held[heldCount++] = n;
        if (heldCount == 1) {
            step = 0;
            nextStep = synced ? gridAfter(static_cast<double>(t), p) : static_cast<double>(t);
        }
    }

    void noteOff(std::int64_t, const Note& n, const Values<4>&)
    {
        auto* last = held.begin() + static_cast<std::ptrdiff_t>(heldCount);
        auto* end = std::remove_if(held.begin(), last, [&n](const Note& h) { return h.channel == n.channel && h.key == n.key; });
        heldCount = static_cast<std::size_t>(end - held.begin());
        if (heldCount == 0) nextStep = -1.0;
    }
};

inline constexpr const char* kLatchModes[] = {"Hold", "Toggle"};

// Latch: Hold keeps the last chord sounding after its keys are let go, until a key is pressed with none held;
// Toggle starts a note with one press of its key and ends it with the next. An all-notes-off ends them all.
struct Latch : ScheduledEffect<Latch, 1> {
    static constexpr const char* id = "com.notecraft.latch";
    static constexpr const char* name = "Latch";
    static constexpr const char* description = "Keeps notes sounding after their keys are let go.";
    enum { Mode };
    static constexpr std::array<Param, 1> params{{
        {"Mode", 0.0, 1.0, 0.0, Unit::Choice, kLatchModes},
    }};

    std::array<bool, 2048> down{};
    std::array<bool, 2048> latched{};
    std::array<Note, 2048> notes{};
    int downCount = 0;

    void clearState()
    {
        down.fill(false);
        latched.fill(false);
        downCount = 0;
    }
    void releaseAllNotes() { clearState(); }

    void noteOn(std::int64_t t, const Note& n, const Values<1>& p)
    {
        const int k = slot(n.channel, n.key);
        if (choice(p, Mode) == 1) {
            if (latched[k]) {
                out.off(t, notes[k]);
                latched[k] = false;
            } else if (out.on(t, n)) {
                latched[k] = true;
                notes[k] = n;
            }
            return;
        }
        if (downCount == 0) { // a new chord ends the one held
            for (std::size_t j = 0; j < latched.size(); ++j) {
                if (latched[j]) out.off(t, notes[j]);
                latched[j] = false;
            }
        }
        if (!down[k]) {
            down[k] = true;
            ++downCount;
        }
        if (!latched[k] && out.on(t, n)) {
            latched[k] = true;
            notes[k] = n;
        }
    }

    void noteOff(std::int64_t, const Note& n, const Values<1>&)
    {
        const int k = slot(n.channel, n.key);
        if (down[k]) {
            down[k] = false;
            --downCount;
        }
    }
};

} // namespace notecraft
