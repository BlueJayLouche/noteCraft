// Effects that move notes in time: Strum, Note Echo, Note Length.
#pragma once

#include "common.hpp"

namespace notecraft {

inline constexpr const char* kStrumDirections[] = {"Up", "Down", "Alternate", "Random"};

// Strum: notes that start together (a chord) leave one after another, Time apart, lowest first (Up), highest
// first (Down), switching each chord (Alternate) or shuffled. Each note's off keeps its delay.
struct Strum : ScheduledEffect<Strum, 2> {
    static constexpr const char* id = "com.notecraft.strum";
    static constexpr const char* name = "Strum";
    static constexpr const char* description = "Spreads the notes of a chord out in time.";
    enum { Time, Direction };
    static constexpr std::array<Param, 2> params{{
        {"Time", 0.0, 100.0, 30.0, Unit::Ms},
        {"Direction", 0.0, 3.0, 0.0, Unit::Choice, kStrumDirections},
    }};

    Rng rng;
    std::array<Note, 128> chord{};
    std::size_t chordSize = 0;
    std::int64_t chordAt = 0;
    std::array<std::int64_t, 2048> delayOf{};  // the delay given to each key's last note-on
    std::array<std::int64_t, 2048> lastOnAt{}; // when each key's last note-on leaves
    std::array<std::uint16_t, 2048> taken{};   // each key's notes taken, whose off is still to come
    bool down = false;                          // the next Alternate chord goes down

    void clearState()
    {
        chordSize = 0;
        delayOf.fill(0);
        lastOnAt.fill(0);
        taken.fill(0);
        down = false;
    }
    void releaseAllNotes() { clearState(); }

    // A chord is complete once time moves past it.
    void advance(std::int64_t t, const Values<2>& p)
    {
        if (chordSize > 0 && t > chordAt) strum(p);
    }

    void noteOn(std::int64_t t, const Note& n, const Values<2>& p)
    {
        if (chordSize == chord.size()) strum(p);
        if (chordSize == 0) chordAt = t;
        chord[chordSize++] = n;
    }

    void noteOff(std::int64_t t, const Note& n, const Values<2>& p)
    {
        if (chordSize > 0) strum(p); // a note ending where it starts: strum it first
        const int k = slot(n.channel, n.key);
        if (taken[k] == 0) return;
        --taken[k];
        out.off(std::max(t + delayOf[k], lastOnAt[k]), n);
    }

    void strum(const Values<2>& p)
    {
        Note* first = chord.data();
        Note* last = first + chordSize;
        int direction = choice(p, Direction);
        if (direction == 2) {
            direction = down ? 1 : 0;
            if (chordSize > 1) down = !down;
        }
        if (direction == 3) {
            for (std::size_t i = chordSize; i > 1; --i) std::swap(chord[i - 1], chord[static_cast<std::size_t>(rng.below(static_cast<int>(i)))]);
        } else {
            std::stable_sort(first, last, [direction](const Note& a, const Note& b) {
                return direction == 0 ? a.key < b.key : a.key > b.key;
            });
        }
        const std::int64_t step = samples(get(p, Time));
        for (std::size_t i = 0; i < chordSize; ++i) {
            const int k = slot(chord[i].channel, chord[i].key);
            const std::int64_t delay = static_cast<std::int64_t>(i) * step;
            if (out.on(chordAt + delay, chord[i])) {
                delayOf[k] = delay;
                lastOnAt[k] = chordAt + delay;
                ++taken[k];
            }
        }
        chordSize = 0;
    }
};

inline constexpr const char* kSyncs[] = {"Off", "1/1", "1/2", "1/4", "1/8", "1/8 T", "1/16", "1/16 T", "1/32"};
inline constexpr double kSyncBeats[] = {0.0, 4.0, 2.0, 1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125};

inline double transportTempo(const clap_event_transport_t* t)
{
    const bool known = t != nullptr && (t->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0 && std::isfinite(t->tempo);
    return known ? std::clamp(t->tempo, 20.0, 999.0) : 120.0;
}

// Note Echo: each note repeats Repeats times, Time apart (or a note value of the host's tempo), each repeat
// Feedback times as loud as the one before and Pitch semitones higher. Each repeat is as long as the note.
struct NoteEcho : ScheduledEffect<NoteEcho, 5> {
    static constexpr const char* id = "com.notecraft.note-echo";
    static constexpr const char* name = "Note Echo";
    static constexpr const char* description = "Repeats each note, fading and optionally rising in pitch.";
    enum { Repeats, Time, Sync, Feedback, Pitch };
    static constexpr std::array<Param, 5> params{{
        {"Repeats", 1.0, 16.0, 3.0, Unit::Number},
        {"Time", 10.0, 2000.0, 250.0, Unit::Ms},
        {"Sync", 0.0, 8.0, 0.0, Unit::Choice, kSyncs},
        {"Feedback", 0.0, 1.0, 0.7, Unit::Percent},
        {"Pitch", -12.0, 12.0, 0.0, Unit::Semitones},
    }};

    struct Held {
        int count = 0; // notes taken: the note and its first count - 1 repeats
        std::int64_t interval = 0;
        int pitch = 0;
    };
    std::array<Held, 2048> held{};
    double tempo = 120.0;

    void clearState() { held.fill(Held{}); }
    void releaseAllNotes() { clearState(); }
    void block(const clap_event_transport_t* t, const Values<5>&) { tempo = transportTempo(t); }

    std::int64_t interval(const Values<5>& p) const
    {
        const int sync = std::clamp(choice(p, Sync), 0, 8);
        const double length = sync > 0 ? kSyncBeats[sync] * 60.0 / tempo * sampleRate : get(p, Time) * 0.001 * sampleRate;
        return std::max<std::int64_t>(1, static_cast<std::int64_t>(length));
    }

    void noteOn(std::int64_t t, const Note& n, const Values<5>& p)
    {
        const int k = slot(n.channel, n.key);
        if (held[k].count > 0) noteOff(t, n, p); // the same key again ends the note before
        Held h{0, interval(p), choice(p, Pitch)};
        const int repeats = choice(p, Repeats);
        double velocity = n.velocity;
        for (int i = 0; i <= repeats; ++i) {
            Note e = n;
            e.key = static_cast<std::int16_t>(n.key + i * h.pitch);
            if (i > 0) {
                velocity *= get(p, Feedback);
                e.id = -1;
            }
            e.velocity = velocity;
            if (e.key < 0 || e.key > 127 || velocity < 1.0 / 127.0 || !out.on(t + i * h.interval, e)) break;
            h.count = i + 1;
        }
        held[k] = h;
    }

    void noteOff(std::int64_t t, const Note& n, const Values<5>&)
    {
        Held& h = held[slot(n.channel, n.key)];
        for (int i = 0; i < h.count; ++i) {
            Note e = n;
            e.key = static_cast<std::int16_t>(n.key + i * h.pitch);
            out.off(t + i * h.interval, e);
        }
        h = Held{};
    }
};

inline constexpr const char* kLengthModes[] = {"Fixed", "Stretch"};

// Note Length: every note lasts Length (Fixed: the played off is ignored), or its played length times Stretch.
// ponytail: Stretch only lengthens. Shortening would need the note's end before it is played.
struct NoteLength : ScheduledEffect<NoteLength, 3> {
    static constexpr const char* id = "com.notecraft.note-length";
    static constexpr const char* name = "Note Length";
    static constexpr const char* description = "Sets every note to a fixed length, or stretches it.";
    enum { Mode, Length, Stretch };
    static constexpr std::array<Param, 3> params{{
        {"Mode", 0.0, 1.0, 0.0, Unit::Choice, kLengthModes},
        {"Length", 10.0, 4000.0, 250.0, Unit::Ms},
        {"Stretch", 1.0, 4.0, 2.0, Unit::Percent},
    }};

    enum class Held : std::uint8_t { None, Fixed, Stretched };
    std::array<Held, 2048> held{};
    std::array<std::int64_t, 2048> onAt{};

    void clearState() { held.fill(Held::None); }
    void releaseAllNotes() { clearState(); }

    void noteOn(std::int64_t t, const Note& n, const Values<3>& p)
    {
        const int k = slot(n.channel, n.key);
        if (held[k] == Held::Stretched) noteOff(t, n, p); // the same key again: the note before gets its off now
        held[k] = Held::None;
        if (!out.on(t, n)) return;
        if (choice(p, Mode) == 0) {
            out.off(t + std::max<std::int64_t>(1, samples(get(p, Length))), n);
            held[k] = Held::Fixed;
        } else {
            held[k] = Held::Stretched;
            onAt[k] = t;
        }
    }

    void noteOff(std::int64_t t, const Note& n, const Values<3>& p)
    {
        const int k = slot(n.channel, n.key);
        if (held[k] == Held::Stretched) {
            out.off(onAt[k] + static_cast<std::int64_t>(static_cast<double>(t - onAt[k]) * get(p, Stretch)), n);
        }
        held[k] = Held::None;
    }
};

} // namespace notecraft
