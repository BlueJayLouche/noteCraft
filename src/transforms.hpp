// Effects that turn each note into other notes when it starts: Scale, Chord, Note Filter, Random Pitch.
#pragma once

#include "common.hpp"

namespace notecraft {

// The base of these effects: D::map picks up to four notes for a note-on, and that note's off ends exactly
// those, whatever the parameters are by then. A key played again ends its notes first; a stray off is dropped.
template <class D, std::size_t N>
struct Mapper : ScheduledEffect<D, N> {
    struct Held {
        bool active = false;
        std::uint8_t count = 0;
        std::array<std::int16_t, 4> keys{};
    };
    std::array<Held, 2048> held{};
    Rng rng;

    void clearState() { held.fill(Held{}); }
    void releaseAllNotes() { clearState(); }

    void noteOn(std::int64_t t, const Note& n, const Values<N>& p)
    {
        const int k = slot(n.channel, n.key);
        if (held[k].active) noteOff(t, n, p);
        std::array<Note, 4> notes{};
        const int count = static_cast<D&>(*this).map(n, notes, p);
        Held h;
        h.active = true;
        for (int i = 0; i < count; ++i) {
            if (slot(notes[i].channel, notes[i].key) >= 0 && this->out.on(t, notes[i])) {
                h.keys[h.count++] = notes[i].key;
            }
        }
        held[k] = h;
    }

    void noteOff(std::int64_t t, const Note& n, const Values<N>&)
    {
        Held& h = held[slot(n.channel, n.key)];
        for (int i = 0; i < h.count; ++i) {
            Note e = n;
            e.key = h.keys[static_cast<std::size_t>(i)];
            this->out.off(t, e);
        }
        h = Held{};
    }
};

// A note moved to another key is a new note to the receiver: it carries no host note id.
inline Note moved(const Note& n, int key)
{
    Note e = n;
    if (key != n.key) {
        e.key = static_cast<std::int16_t>(key);
        e.id = -1;
    }
    return e;
}

inline constexpr const char* kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
inline constexpr const char* kScaleTypes[] = {"Major", "Minor", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Locrian",
                                              "Harmonic minor", "Melodic minor", "Major pentatonic", "Minor pentatonic",
                                              "Blues", "Chromatic"};
template <class... I>
constexpr std::uint16_t degrees(I... semitones)
{
    return static_cast<std::uint16_t>(((1u << semitones) | ...));
}
inline constexpr std::uint16_t kScaleMasks[] = {
    degrees(0, 2, 4, 5, 7, 9, 11), degrees(0, 2, 3, 5, 7, 8, 10), degrees(0, 2, 3, 5, 7, 9, 10),
    degrees(0, 1, 3, 5, 7, 8, 10), degrees(0, 2, 4, 6, 7, 9, 11), degrees(0, 2, 4, 5, 7, 9, 10),
    degrees(0, 1, 3, 5, 6, 8, 10), degrees(0, 2, 3, 5, 7, 8, 11), degrees(0, 2, 3, 5, 7, 9, 11),
    degrees(0, 2, 4, 7, 9),        degrees(0, 3, 5, 7, 10),        degrees(0, 3, 5, 6, 7, 10),
    0x0FFF,
};
inline constexpr const char* kScaleActions[] = {"Nearest", "Up", "Down", "Drop"};

// Scale: a note outside the scale moves to the nearest note in it (a tie goes down), the next one up or down,
// or is dropped.
struct Scale : Mapper<Scale, 3> {
    static constexpr const char* id = "com.notecraft.scale";
    static constexpr const char* name = "Scale";
    static constexpr const char* description = "Moves notes into a key and scale.";
    enum { Root, Type, Action };
    static constexpr std::array<Param, 3> params{{
        {"Root", 0.0, 11.0, 0.0, Unit::Choice, kNoteNames},
        {"Scale", 0.0, 12.0, 0.0, Unit::Choice, kScaleTypes},
        {"Action", 0.0, 3.0, 0.0, Unit::Choice, kScaleActions},
    }};

    // The key a note goes to, or -1 to drop it.
    static int snap(int key, int root, std::uint16_t mask, int action)
    {
        const auto in = [&](int k) { return k >= 0 && k <= 127 && ((mask >> ((k - root + 120) % 12)) & 1) != 0; };
        if (in(key)) return key;
        if (action == 3) return -1;
        for (int d = 1; d < 12; ++d) {
            if (action != 1 && in(key - d)) return key - d;
            if (action != 2 && in(key + d)) return key + d;
        }
        return -1;
    }

    int map(const Note& n, std::array<Note, 4>& notes, const Values<3>& p)
    {
        const int key = snap(n.key, std::clamp(choice(p, Root), 0, 11), kScaleMasks[std::clamp(choice(p, Type), 0, 12)],
                             choice(p, Action));
        if (key < 0) return 0;
        notes[0] = moved(n, key);
        return 1;
    }
};

// Chord: each note plays with up to three more, Shift semitones away (0 is off), at Velocity times its own.
struct Chord : Mapper<Chord, 4> {
    static constexpr const char* id = "com.notecraft.chord";
    static constexpr const char* name = "Chord";
    static constexpr const char* description = "Adds up to three notes to every note.";
    enum { Shift1, Shift2, Shift3, Velocity };
    static constexpr std::array<Param, 4> params{{
        {"Shift 1", -24.0, 24.0, 4.0, Unit::Semitones},
        {"Shift 2", -24.0, 24.0, 7.0, Unit::Semitones},
        {"Shift 3", -24.0, 24.0, 0.0, Unit::Semitones},
        {"Velocity", 0.0, 1.0, 1.0, Unit::Percent},
    }};

    int map(const Note& n, std::array<Note, 4>& notes, const Values<4>& p)
    {
        notes[0] = n;
        int count = 1;
        for (int s = Shift1; s <= Shift3; ++s) {
            const int key = n.key + choice(p, static_cast<std::size_t>(s));
            const bool repeated = std::any_of(notes.begin(), notes.begin() + count, [key](const Note& e) { return e.key == key; });
            if (key < 0 || key > 127 || repeated) continue;
            notes[static_cast<std::size_t>(count)] = moved(n, key);
            notes[static_cast<std::size_t>(count)].velocity = n.velocity * get(p, Velocity);
            ++count;
        }
        return count;
    }
};

// Note Filter: only notes within a key range and a velocity range pass (a range set upside down reads the other
// way round).
struct NoteFilter : Mapper<NoteFilter, 4> {
    static constexpr const char* id = "com.notecraft.note-filter";
    static constexpr const char* name = "Note Filter";
    static constexpr const char* description = "Passes only notes within a key and velocity range.";
    enum { LowKey, HighKey, LowVelocity, HighVelocity };
    static constexpr std::array<Param, 4> params{{
        {"Lowest key", 0.0, 127.0, 0.0, Unit::Key},
        {"Highest key", 0.0, 127.0, 127.0, Unit::Key},
        {"Lowest velocity", 1.0, 127.0, 1.0, Unit::Number},
        {"Highest velocity", 1.0, 127.0, 127.0, Unit::Number},
    }};

    static bool within(long v, long a, long b) { return v >= std::min(a, b) && v <= std::max(a, b); }

    int map(const Note& n, std::array<Note, 4>& notes, const Values<4>& p)
    {
        const long velocity = std::lround(noteOnVelocity(n.velocity) * 127.0);
        if (!within(n.key, choice(p, LowKey), choice(p, HighKey)) ||
            !within(velocity, choice(p, LowVelocity), choice(p, HighVelocity))) {
            return 0;
        }
        notes[0] = n;
        return 1;
    }
};

inline constexpr const char* kPitchDirections[] = {"Both", "Up", "Down"};

// Random Pitch: with probability Chance a note moves by 1 to Range semitones, up, down or either way. A note
// that would leave the keyboard stays where it is.
struct RandomPitch : Mapper<RandomPitch, 3> {
    static constexpr const char* id = "com.notecraft.random-pitch";
    static constexpr const char* name = "Random Pitch";
    static constexpr const char* description = "Moves notes up or down by a random interval.";
    enum { Chance, Range, Direction };
    static constexpr std::array<Param, 3> params{{
        {"Chance", 0.0, 1.0, 0.5, Unit::Percent},
        {"Range", 1.0, 24.0, 12.0, Unit::Semitones},
        {"Direction", 0.0, 2.0, 0.0, Unit::Choice, kPitchDirections},
    }};

    int map(const Note& n, std::array<Note, 4>& notes, const Values<3>& p)
    {
        notes[0] = n;
        if (rng.next() < get(p, Chance)) {
            const int range = std::max(choice(p, Range), 1);
            int shift = 1 + rng.below(range);
            const int direction = choice(p, Direction);
            if (direction == 2 || (direction == 0 && rng.next() < 0.5)) shift = -shift;
            const int key = n.key + shift;
            if (key >= 0 && key <= 127) notes[0] = moved(n, key);
        }
        return 1;
    }
};

} // namespace notecraft
