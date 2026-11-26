// Velocity Curve and Humanize. Header only, so the tests drive the same code the plug-in runs.
#pragma once

#include "common.hpp"

namespace notecraft {

// ---------------------------------------------------------------------------------------------------------------
// Velocity Curve: note-on velocity through a line from L (input 0) to M (input at Mid position) to H (input 1),
// plus a random spread. Everything else passes unchanged, in order, at its own offset.

struct VelocityCurve {
    static constexpr const char* id = "com.notecraft.velocity-curve";
    static constexpr const char* name = "Velocity Curve";
    static constexpr const char* description = "Shapes note-on velocity through three points, with a random spread.";
    enum { Lo, Mid, Hi, MidPosition, Random };
    static constexpr std::array<Param, 5> params{{
        {"Lo", 0.0, 1.0, 0.0, Unit::Percent},
        {"Mid", 0.0, 1.0, 0.5, Unit::Percent},
        {"Hi", 0.0, 1.0, 1.0, Unit::Percent},
        {"Mid position", 0.0, 1.0, 0.5, Unit::Percent},
        {"Random", 0.0, 1.0, 0.0, Unit::Percent}, // up to +-25 % of full scale
    }};

    Rng rng;

    static double curve(double v, double lo, double mid, double hi, double midX)
    {
        if (v <= midX) {
            return midX <= 0.0 ? mid : lo + (mid - lo) * (v / midX);
        }
        return midX >= 1.0 ? mid : mid + (hi - mid) * ((v - midX) / (1.0 - midX));
    }

    double map(double v, const Values<5>& p)
    {
        const double shaped = curve(v, get(p, Lo), get(p, Mid), get(p, Hi), get(p, MidPosition));
        return noteOnVelocity(shaped + (rng.next() * 2.0 - 1.0) * 0.25 * get(p, Random));
    }

    void activate(double) {}
    void reset() {}

    void process(const clap_process_t& process, Values<5>& p)
    {
        const clap_input_events_t* in = process.in_events;
        const clap_output_events_t* out = process.out_events;
        const std::uint32_t count = in->size(in);
        for (std::uint32_t i = 0; i < count; ++i) {
            const clap_event_header_t* h = in->get(in, i);
            if (h == nullptr || applyParam(h, p, params) || !isNoteStream(h)) {
                continue;
            }
            if (isCore(h, CLAP_EVENT_NOTE_ON, sizeof(clap_event_note_t))) {
                clap_event_note_t e;
                std::memcpy(&e, h, sizeof e);
                e.header.size = sizeof e;
                e.velocity = map(e.velocity, p);
                out->try_push(out, &e.header);
            } else if (isCore(h, CLAP_EVENT_MIDI, sizeof(clap_event_midi_t)) &&
                       isMidiNoteOn(*reinterpret_cast<const clap_event_midi_t*>(h))) {
                clap_event_midi_t e;
                std::memcpy(&e, h, sizeof e);
                e.header.size = sizeof e;
                e.data[2] = midiVelocity(map((e.data[2] & 0x7F) / 127.0, p));
                out->try_push(out, &e.header);
            } else {
                out->try_push(out, h);
            }
        }
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Humanize: each note plays with probability Chance (a dropped note's off is dropped with it). A note that
// plays is moved late by up to Timing and its velocity by up to +-Velocity/2. A note's release and expressions
// keep its delay, so its length is kept and its off never leaves before its on.
//
// ponytail: late only. Early notes need the host to compensate a note effect's latency (#671 leaves that
// open); then report Timing as latency and spread +-Timing around it.

struct Humanize {
    static constexpr const char* id = "com.notecraft.humanize";
    static constexpr const char* name = "Humanize";
    static constexpr const char* description = "Drops notes by chance, moves the rest late and varies their velocity.";
    enum { Chance, Timing, Velocity };
    static constexpr std::array<Param, 3> params{{
        {"Chance", 0.0, 1.0, 1.0, Unit::Percent},
        {"Timing", 0.0, 100.0, 10.0, Unit::Ms},
        {"Velocity", 0.0, 1.0, 0.2, Unit::Percent},
    }};

    // ponytail: fixed queue, O(n) insert. Releases keep a reserve, so a flood of note-ons and controllers
    // cannot crowd out an off; past the reserve too an event is dropped. Grow kCapacity if that is ever reached.
    static constexpr std::size_t kCapacity = 4096;
    static constexpr std::size_t kReleaseReserve = 1024;

    union Copy {
        clap_event_header_t header;
        clap_event_note_t note;
        clap_event_note_expression_t expression;
        clap_event_midi_t midi;
    };
    struct Pending {
        std::int64_t at;                 // absolute sample
        const clap_event_header_t* ref;  // the host's event, passed as is (never delayed, so valid this call)
        Copy copy;                       // a note event that may be delayed
    };

    Rng rng;
    double sampleRate = 48000.0;
    std::int64_t now = 0; // absolute sample at the block start
    std::array<Pending, kCapacity> queue{};
    std::size_t size = 0;
    std::array<std::int64_t, 16 * 128> delayOf{};  // the delay given to each key's last note-on
    std::array<std::int64_t, 16 * 128> lastOnAt{}; // when each key's last note-on leaves
    std::array<std::int64_t, 16 * 128> lastOffAt{}; // when each key's last note-off leaves
    std::int64_t latestOn = 0;                     // when the last scheduled note-on leaves
    std::array<std::uint16_t, 16 * 128> dropped{}; // each key's notes dropped by Chance whose offs are to come

    void activate(double rate)
    {
        sampleRate = rate > 0.0 ? rate : 48000.0;
        reset();
    }

    void reset()
    {
        now = 0;
        size = 0;
        delayOf.fill(0);
        lastOnAt.fill(0);
        lastOffAt.fill(0);
        latestOn = 0;
        dropped.fill(0);
    }

    // Whether a note-on of key k plays. A note whose key is out of range always plays: its off could not be
    // matched. ponytail: dropped notes are counted per key, so with overlapping notes on one key the off
    // swallowed may be the other note's; that note then ends at the later off, nothing hangs.
    bool plays(int k, const Values<3>& p)
    {
        if (k < 0 || rng.next() < get(p, Chance)) return true;
        dropped[k] = static_cast<std::uint16_t>(std::min(dropped[k] + 1, 0xFFFF));
        return false;
    }

    // Whether an off of key k belongs to a dropped note. A release of every note forgets the dropped ones, whose
    // own offs may never come.
    bool swallow(int k)
    {
        if (k < 0) {
            dropped.fill(0);
            return false;
        }
        if (dropped[k] == 0) return false;
        --dropped[k];
        return true;
    }

    // A note-on at t that plays: its delay and velocity. Returns when it leaves: never before the off of the
    // key's note before, which a longer delay may have pushed past it.
    std::int64_t scheduleOn(int k, std::int64_t t, double& velocity, const Values<3>& p)
    {
        auto delay = static_cast<std::int64_t>(rng.next() * get(p, Timing) * 0.001 * sampleRate);
        velocity = noteOnVelocity(velocity + (rng.next() * 2.0 - 1.0) * 0.5 * get(p, Velocity));
        if (k >= 0) delay = std::max(t + delay, lastOffAt[k]) - t;
        const std::int64_t at = t + delay;
        if (k >= 0) {
            delayOf[k] = delay;
            lastOnAt[k] = at;
        }
        latestOn = std::max(latestOn, at);
        return at;
    }

    // A release or expression of key k at t: its note's delay, never before the note's on. A release of
    // every note (a wildcard, CC 120 or 123) leaves after every note-on scheduled so far.
    std::int64_t follow(int k, std::int64_t t) const
    {
        return k >= 0 ? std::max(t + delayOf[k], lastOnAt[k]) : std::max(t, latestOn);
    }

    std::int64_t scheduleOff(int k, std::int64_t t)
    {
        const std::int64_t at = follow(k, t);
        if (k >= 0) lastOffAt[k] = std::max(lastOffAt[k], at);
        return at;
    }

    bool push(std::int64_t at, const clap_event_header_t* ref, const Copy* copy, bool release)
    {
        if (size >= (release ? kCapacity : kCapacity - kReleaseReserve)) {
            return false;
        }
        // After every queued event at the same time: equal times keep arrival order.
        auto* end = queue.data() + size;
        auto* pos = std::upper_bound(queue.data(), end, at, [](std::int64_t a, const Pending& e) { return a < e.at; });
        std::move_backward(pos, end, end + 1);
        pos->at = at;
        pos->ref = ref;
        if (copy != nullptr) {
            pos->copy = *copy;
        }
        ++size;
        return true;
    }

    void take(const clap_event_header_t* h, std::int64_t t, const Values<3>& p)
    {
        Copy c{};
        if (isCore(h, CLAP_EVENT_NOTE_ON, sizeof(clap_event_note_t))) {
            std::memcpy(&c.note, h, sizeof c.note);
            c.header.size = sizeof c.note;
            const int k = slot(c.note.channel, c.note.key);
            if (plays(k, p)) {
                push(scheduleOn(k, t, c.note.velocity, p), nullptr, &c, false);
            }
        } else if (isCore(h, CLAP_EVENT_NOTE_OFF, sizeof(clap_event_note_t)) ||
                   isCore(h, CLAP_EVENT_NOTE_CHOKE, sizeof(clap_event_note_t))) {
            std::memcpy(&c.note, h, sizeof c.note);
            c.header.size = sizeof c.note;
            const int k = slot(c.note.channel, c.note.key);
            if (!swallow(k)) {
                push(scheduleOff(k, t), nullptr, &c, true);
            }
        } else if (isCore(h, CLAP_EVENT_NOTE_EXPRESSION, sizeof(clap_event_note_expression_t))) {
            std::memcpy(&c.expression, h, sizeof c.expression);
            c.header.size = sizeof c.expression;
            const int k = slot(c.expression.channel, c.expression.key);
            push(k >= 0 ? follow(k, t) : t, nullptr, &c, false);
        } else if (isCore(h, CLAP_EVENT_MIDI, sizeof(clap_event_midi_t))) {
            std::memcpy(&c.midi, h, sizeof c.midi);
            c.header.size = sizeof c.midi;
            const int status = c.midi.data[0] & 0xF0;
            const int k = slot(c.midi.data[0] & 0x0F, c.midi.data[1] & 0x7F);
            if (isMidiNoteOn(c.midi)) {
                if (plays(k, p)) {
                    double v = (c.midi.data[2] & 0x7F) / 127.0;
                    const std::int64_t at = scheduleOn(k, t, v, p);
                    c.midi.data[2] = midiVelocity(v);
                    push(at, nullptr, &c, false);
                }
            } else if (status == 0x80 || status == 0x90) {
                if (!swallow(k)) {
                    push(scheduleOff(k, t), nullptr, &c, true);
                }
            } else if (status == 0xA0) { // polyphonic aftertouch follows its note
                push(follow(k, t), nullptr, &c, false);
            } else if (status == 0xB0 && (c.midi.data[1] == 120 || c.midi.data[1] == 123)) {
                swallow(-1);
                push(follow(-1, t), nullptr, &c, true);
            } else {
                push(t, h, nullptr, false);
            }
        } else {
            push(t, h, nullptr, false);
        }
    }

    void process(const clap_process_t& process, Values<3>& p)
    {
        const clap_input_events_t* in = process.in_events;
        const clap_output_events_t* out = process.out_events;
        const std::uint32_t frames = process.frames_count;
        const std::uint32_t count = in->size(in);
        if (frames == 0) { // nothing can be scheduled into no time: pass through
            for (std::uint32_t i = 0; i < count; ++i) {
                const clap_event_header_t* h = in->get(in, i);
                if (h != nullptr && !applyParam(h, p, params) && isNoteStream(h)) {
                    out->try_push(out, h);
                }
            }
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const clap_event_header_t* h = in->get(in, i);
            if (h == nullptr || applyParam(h, p, params) || !isNoteStream(h)) {
                continue;
            }
            take(h, now + std::min<std::uint32_t>(h->time, frames - 1), p);
        }
        const std::int64_t end = now + frames;
        std::size_t due = 0;
        for (; due < size && queue[due].at < end; ++due) {
            Pending& e = queue[due];
            if (e.ref != nullptr) {
                out->try_push(out, e.ref);
            } else {
                e.copy.header.time = static_cast<std::uint32_t>(e.at - now);
                out->try_push(out, &e.copy.header);
            }
        }
        std::move(queue.begin() + static_cast<std::ptrdiff_t>(due), queue.begin() + static_cast<std::ptrdiff_t>(size),
                  queue.begin());
        size -= due;
        now = end;
    }
};

} // namespace notecraft
