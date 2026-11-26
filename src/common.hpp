// What every effect shares: parameters, random numbers, reading notes, and the scheduler that writes them.
#pragma once

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace notecraft {

enum class Unit { Percent, Ms, Semitones, Number, Choice, Key };

struct Param {
    const char* name;
    double min;
    double max;
    double defaultValue;
    Unit unit;
    const char* const* labels = nullptr; // Unit::Choice: one per step from min
};

template <std::size_t N>
using Values = std::array<std::atomic<double>, N>;

// A parameter change addressed to the plug-in (its id is the index). True when the event was one.
template <std::size_t N>
bool applyParam(const clap_event_header_t* h, Values<N>& values, const std::array<Param, N>& params)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE ||
        h->size < sizeof(clap_event_param_value_t)) {
        return false;
    }
    const auto* p = reinterpret_cast<const clap_event_param_value_t*>(h);
    if (p->param_id < N && std::isfinite(p->value)) {
        const auto& info = params[p->param_id];
        values[p->param_id].store(std::clamp(p->value, info.min, info.max), std::memory_order_relaxed);
    }
    return true;
}

template <std::size_t N>
double get(const Values<N>& values, std::size_t i)
{
    return values[i].load(std::memory_order_relaxed);
}

template <std::size_t N>
int choice(const Values<N>& values, std::size_t i)
{
    return static_cast<int>(std::lround(get(values, i)));
}

// xorshift64*: the audio thread's random numbers, no allocation, no lock.
struct Rng {
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    double next() // [0, 1)
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return static_cast<double>((state * 2685821657736338717ull) >> 11) * 0x1.0p-53;
    }
    int below(int n) { return n > 0 ? std::min(static_cast<int>(next() * n), n - 1) : 0; }
};

// A note-on never leaves below one MIDI step: velocity 0 reads as a note-off downstream. NaN becomes the minimum.
inline double noteOnVelocity(double v)
{
    constexpr double minimum = 1.0 / 127.0;
    return v >= minimum ? std::min(v, 1.0) : minimum;
}

inline double releaseVelocity(double v) { return v >= 0.0 ? std::min(v, 1.0) : 0.0; }

inline std::uint8_t midiVelocity(double v)
{
    return static_cast<std::uint8_t>(std::lround(noteOnVelocity(v) * 127.0));
}

inline bool isCore(const clap_event_header_t* h, std::uint16_t type, std::size_t size)
{
    return h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == type && h->size >= size;
}

// Notes and MIDI pass on; the rest (transport, parameter events) is the host's to this plug-in, never output.
inline bool isNoteStream(const clap_event_header_t* h)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return false;
    switch (h->type) {
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE:
    case CLAP_EVENT_NOTE_EXPRESSION:
    case CLAP_EVENT_MIDI:
    case CLAP_EVENT_MIDI_SYSEX:
    case CLAP_EVENT_MIDI2:
        return true;
    default:
        return false;
    }
}

inline bool isMidiNoteOn(const clap_event_midi_t& m)
{
    return (m.data[0] & 0xF0) == 0x90 && (m.data[2] & 0x7F) > 0;
}

inline int slot(int channel, int key)
{
    return channel >= 0 && channel < 16 && key >= 0 && key < 128 ? channel * 128 + key : -1;
}

// A note in either dialect: a CLAP note event or a MIDI 1.0 note message, written back the way it came.
struct Note {
    bool midi = false;
    std::int16_t port = 0;
    std::int16_t channel = 0;
    std::int16_t key = 0;
    std::int32_t id = -1; // CLAP note id; -1 for notes an effect makes or moves to another key
    double velocity = 0.0;
};

enum class Input { Other, On, Off, ReleaseAll };

// Reads a note-on or note-off of one key; an off of every key (a wildcard, CC 120 or 123) is a release of all.
inline Input readNote(const clap_event_header_t* h, Note& n)
{
    if (isCore(h, CLAP_EVENT_NOTE_ON, sizeof(clap_event_note_t)) ||
        isCore(h, CLAP_EVENT_NOTE_OFF, sizeof(clap_event_note_t)) ||
        isCore(h, CLAP_EVENT_NOTE_CHOKE, sizeof(clap_event_note_t))) {
        clap_event_note_t e;
        std::memcpy(&e, h, sizeof e);
        n = Note{false, e.port_index, e.channel, e.key, e.note_id, e.velocity};
        const bool oneKey = slot(e.channel, e.key) >= 0;
        if (h->type == CLAP_EVENT_NOTE_ON) return oneKey ? Input::On : Input::Other;
        return oneKey ? Input::Off : Input::ReleaseAll;
    }
    if (isCore(h, CLAP_EVENT_MIDI, sizeof(clap_event_midi_t))) {
        clap_event_midi_t m;
        std::memcpy(&m, h, sizeof m);
        const int status = m.data[0] & 0xF0;
        n = Note{true, static_cast<std::int16_t>(m.port_index), static_cast<std::int16_t>(m.data[0] & 0x0F),
                 static_cast<std::int16_t>(m.data[1] & 0x7F), -1, (m.data[2] & 0x7F) / 127.0};
        if (status == 0x90 && n.velocity > 0.0) return Input::On;
        if (status == 0x80 || status == 0x90) return Input::Off;
        if (status == 0xB0 && (n.key == 120 || n.key == 123)) return Input::ReleaseAll;
    }
    return Input::Other;
}

// The output side of an effect: a time-ordered queue, and the voice count of every key, kept as the events
// leave. A note-on on a key that sounds ends the sounding note first; an off leaves only when the key's last
// note ends. So whatever an effect schedules, every on gets its off and no off arrives without an on.
//
// A note-on is taken only while there is room for it and for the off of every note taken so far, so an off is
// never refused: a flood of notes drops new notes, never an off. An effect pushes one off per accepted on.
class Scheduler {
public:
    static constexpr std::size_t kCapacity = 4096;

    void clear()
    {
        head_ = size_ = 0;
        outstanding_ = 0;
        counts_.fill(0);
    }

    bool on(std::int64_t at, const Note& n) { return push({at, nullptr, n, Kind::On}); }
    bool off(std::int64_t at, const Note& n) { return push({at, nullptr, n, Kind::Off}); }
    bool pass(std::int64_t at, const clap_event_header_t* h) { return push({at, h, Note{}, Kind::Pass}); }

    // Writes, in time order, everything due before `limit`.
    void emitUntil(std::int64_t limit, std::int64_t blockStart, const clap_output_events_t* out)
    {
        for (; head_ < size_ && items_[head_].at < limit; ++head_) {
            emit(items_[head_], blockStart, out);
        }
    }

    // At `at`: drops the pending notes (passed events stay) and ends every sounding note.
    void releaseAll(std::int64_t at, std::int64_t blockStart, const clap_output_events_t* out)
    {
        std::size_t kept = head_;
        for (std::size_t i = head_; i < size_; ++i) {
            if (items_[i].kind == Kind::Pass) items_[kept++] = items_[i];
        }
        size_ = kept;
        outstanding_ = 0;
        for (std::size_t k = 0; k < counts_.size(); ++k) {
            if (counts_[k] > 0) {
                counts_[k] = 0;
                write(last_[k], false, static_cast<std::uint32_t>(at - blockStart), out);
            }
        }
    }

    // Moves what is pending to the front; call once a block, after emitUntil.
    void compact()
    {
        std::move(items_.begin() + static_cast<std::ptrdiff_t>(head_), items_.begin() + static_cast<std::ptrdiff_t>(size_),
                  items_.begin());
        size_ -= head_;
        head_ = 0;
    }

    std::size_t pending() const { return size_ - head_; }

private:
    enum class Kind : std::uint8_t { Pass, On, Off };
    struct Item {
        std::int64_t at;                // absolute sample
        const clap_event_header_t* ref; // Kind::Pass: the host's event, due this block
        Note note;
        Kind kind;
    };

    bool push(const Item& item)
    {
        const std::size_t pending = size_ - head_;
        const std::size_t need = item.kind == Kind::On ? 2 : item.kind == Kind::Off ? 0 : 1;
        if (pending == kCapacity || pending + outstanding_ + need > kCapacity) {
            return false;
        }
        if (size_ == kCapacity) compact();
        if (item.kind == Kind::On) {
            ++outstanding_;
        } else if (item.kind == Kind::Off && outstanding_ > 0) {
            --outstanding_;
        }
        // After every queued event at the same time: equal times keep the order they were pushed in.
        auto* first = items_.data() + head_;
        auto* end = items_.data() + size_;
        auto* pos = std::upper_bound(first, end, item.at, [](std::int64_t a, const Item& e) { return a < e.at; });
        std::move_backward(pos, end, end + 1);
        *pos = item;
        ++size_;
        return true;
    }

    void emit(const Item& e, std::int64_t blockStart, const clap_output_events_t* out)
    {
        const auto time = static_cast<std::uint32_t>(e.at - blockStart);
        if (e.kind == Kind::Pass) {
            out->try_push(out, e.ref);
            return;
        }
        const int k = slot(e.note.channel, e.note.key);
        if (k < 0) return;
        if (e.kind == Kind::On) {
            if (counts_[k] > 0) write(last_[k], false, time, out);
            counts_[k] = static_cast<std::uint16_t>(std::min(counts_[k] + 1, 0xFFFF));
            last_[k] = e.note;
            write(e.note, true, time, out);
        } else if (counts_[k] > 0 && --counts_[k] == 0) {
            Note n = last_[k];
            n.velocity = e.note.velocity;
            write(n, false, time, out);
        }
    }

    static void write(const Note& n, bool on, std::uint32_t time, const clap_output_events_t* out)
    {
        if (n.midi) {
            clap_event_midi_t e{};
            e.header = {sizeof e, time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            e.port_index = static_cast<std::uint16_t>(std::max<std::int16_t>(n.port, 0));
            e.data[0] = static_cast<std::uint8_t>((on ? 0x90 : 0x80) | (n.channel & 0x0F));
            e.data[1] = static_cast<std::uint8_t>(n.key & 0x7F);
            e.data[2] = on ? midiVelocity(n.velocity) : static_cast<std::uint8_t>(std::lround(releaseVelocity(n.velocity) * 127.0));
            out->try_push(out, &e.header);
        } else {
            clap_event_note_t e{};
            e.header = {sizeof e, time, CLAP_CORE_EVENT_SPACE_ID,
                        static_cast<std::uint16_t>(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            e.note_id = n.id;
            e.port_index = n.port;
            e.channel = n.channel;
            e.key = n.key;
            e.velocity = on ? noteOnVelocity(n.velocity) : releaseVelocity(n.velocity);
            out->try_push(out, &e.header);
        }
    }

    std::array<Item, kCapacity> items_{};
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::size_t outstanding_ = 0;          // notes taken whose off has not been pushed yet
    std::array<std::uint16_t, 2048> counts_{}; // notes sounding on each key
    std::array<Note, 2048> last_{};            // the note each key sounds, for its off
};

// The process loop of an effect built on the scheduler. D implements noteOn and noteOff, and may hide the hooks
// below: clearState, block (the transport, once a block), advance (generators, up to a time) and releaseAllNotes.
template <class D, std::size_t N>
struct ScheduledEffect {
    Scheduler out;
    double sampleRate = 48000.0;
    std::int64_t now = 0; // absolute sample at the block start

    void activate(double rate)
    {
        sampleRate = rate > 0.0 && std::isfinite(rate) ? rate : 48000.0;
        reset();
    }

    void reset()
    {
        out.clear();
        now = 0;
        static_cast<D&>(*this).clearState();
    }

    std::int64_t samples(double ms) const { return static_cast<std::int64_t>(ms * 0.001 * sampleRate); }

    void clearState() {}
    void block(const clap_event_transport_t*, const Values<N>&) {}
    void advance(std::int64_t, const Values<N>&) {}
    void releaseAllNotes() {}

    void process(const clap_process_t& process, Values<N>& p)
    {
        D& d = static_cast<D&>(*this);
        const clap_input_events_t* in = process.in_events;
        const clap_output_events_t* output = process.out_events;
        const std::uint32_t frames = process.frames_count;
        const std::int64_t end = now + std::max<std::uint32_t>(frames, 1); // a block of no frames still writes
        d.block(process.transport, p);
        const std::uint32_t count = in->size(in);
        for (std::uint32_t i = 0; i < count; ++i) {
            const clap_event_header_t* h = in->get(in, i);
            if (h == nullptr || applyParam(h, p, D::params) || !isNoteStream(h)) {
                continue;
            }
            const std::int64_t t = now + (frames > 0 ? std::min<std::uint32_t>(h->time, frames - 1) : 0);
            d.advance(t, p);
            out.emitUntil(t, now, output);
            Note n;
            switch (readNote(h, n)) {
            case Input::On:
                d.noteOn(t, n, p);
                break;
            case Input::Off:
                d.noteOff(t, n, p);
                break;
            case Input::ReleaseAll:
                d.releaseAllNotes();
                out.releaseAll(t, now, output);
                out.pass(t, h);
                break;
            case Input::Other:
                out.pass(t, h);
                break;
            }
        }
        d.advance(end, p);
        out.emitUntil(end, now, output);
        out.compact();
        now += frames;
    }
};

} // namespace notecraft
