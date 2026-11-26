// Fake CLAP event lists and a process loop: the effects run in the tests the way a host runs them.
#pragma once

#include "common.hpp"

#include <cstdio>
#include <vector>

namespace notecraft::test {

inline int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++notecraft::test::failures;                                \
        }                                                               \
    } while (0)

inline int finish()
{
    std::printf(failures == 0 ? "all passed\n" : "%d failed\n", failures);
    return failures == 0 ? 0 : 1;
}

union Any {
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_midi_t midi;
    clap_event_param_value_t param;
    clap_event_transport_t transport;
};

inline Any transport(std::uint32_t time)
{
    Any a{};
    a.transport.header = {sizeof(clap_event_transport_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
    return a;
}

inline Any note(std::uint16_t type, std::uint32_t time, int key, double velocity, int channel = 0)
{
    Any a{};
    a.note = clap_event_note_t{{sizeof(clap_event_note_t), time, CLAP_CORE_EVENT_SPACE_ID, type, 0}, -1, 0,
                               static_cast<std::int16_t>(channel), static_cast<std::int16_t>(key), velocity};
    return a;
}

inline Any on(std::uint32_t time, int key, double velocity = 0.8) { return note(CLAP_EVENT_NOTE_ON, time, key, velocity); }
inline Any off(std::uint32_t time, int key) { return note(CLAP_EVENT_NOTE_OFF, time, key, 0.0); }

inline Any midi(std::uint32_t time, std::uint8_t b0, std::uint8_t b1, std::uint8_t b2)
{
    Any a{};
    a.midi = clap_event_midi_t{{sizeof(clap_event_midi_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0}, 0, {b0, b1, b2}};
    return a;
}

inline Any param(std::uint32_t time, clap_id id, double value)
{
    Any a{};
    a.param = clap_event_param_value_t{
        {sizeof(clap_event_param_value_t), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0}, id, nullptr, -1, -1, -1, -1, value};
    return a;
}

struct Lists {
    std::vector<Any> in;
    std::vector<Any> out;
    clap_input_events_t inList{this,
                               [](const clap_input_events_t* l) {
                                   return static_cast<std::uint32_t>(static_cast<Lists*>(l->ctx)->in.size());
                               },
                               [](const clap_input_events_t* l, std::uint32_t i) -> const clap_event_header_t* {
                                   return &static_cast<Lists*>(l->ctx)->in[i].header;
                               }};
    clap_output_events_t outList{this, [](const clap_output_events_t* l, const clap_event_header_t* h) {
                                     Any a{};
                                     std::memcpy(&a, h, std::min<std::size_t>(h->size, sizeof a));
                                     static_cast<Lists*>(l->ctx)->out.push_back(a);
                                     return true;
                                 }};
};

template <class E>
void defaults(Values<E::params.size()>& v)
{
    for (std::size_t i = 0; i < v.size(); ++i) v[i].store(E::params[i].defaultValue);
}

constexpr double kRate = 48000.0;

// Runs `blocks` blocks of `frames` over `in` (absolute times); returns the output with absolute times. A playing
// transport moves on with each block.
template <class E>
std::vector<Any> run(E& fx, Values<E::params.size()>& v, std::vector<Any> in, std::uint32_t frames,
                     std::uint32_t blocks, clap_event_transport_t* tr = nullptr)
{
    std::stable_sort(in.begin(), in.end(), [](const Any& a, const Any& b) { return a.header.time < b.header.time; });
    std::vector<Any> result;
    std::size_t next = 0;
    for (std::uint32_t b = 0; b < blocks; ++b) {
        Lists l;
        for (; next < in.size() && in[next].header.time < (b + 1) * frames; ++next) {
            l.in.push_back(in[next]);
            l.in.back().header.time -= b * frames;
        }
        clap_process_t process{};
        process.steady_time = -1;
        process.frames_count = frames;
        process.transport = tr;
        process.in_events = &l.inList;
        process.out_events = &l.outList;
        fx.process(process, v);
        std::uint32_t last = 0;
        for (Any& a : l.out) {
            CHECK(a.header.time < frames);
            CHECK(a.header.time >= last); // in time order
            last = a.header.time;
            a.header.time += b * frames;
            result.push_back(a);
        }
        if (tr != nullptr && (tr->flags & CLAP_TRANSPORT_IS_PLAYING) != 0) {
            tr->song_pos_beats += std::llround(frames * tr->tempo / 60.0 / kRate * static_cast<double>(CLAP_BEATTIME_FACTOR));
        }
    }
    return result;
}

inline bool isType(const Any& a, std::uint16_t type) { return a.header.type == type; }

// A note-on or note-off in either dialect: its channel and key, and whether it starts the note.
inline bool noteOf(const Any& a, int& channel, int& key, bool& starts)
{
    if (isType(a, CLAP_EVENT_NOTE_ON) || isType(a, CLAP_EVENT_NOTE_OFF)) {
        channel = a.note.channel;
        key = a.note.key;
        starts = isType(a, CLAP_EVENT_NOTE_ON);
        return true;
    }
    const int status = a.midi.data[0] & 0xF0;
    if (isType(a, CLAP_EVENT_MIDI) && (status == 0x80 || status == 0x90)) {
        channel = a.midi.data[0] & 0x0F;
        key = a.midi.data[1];
        starts = status == 0x90 && a.midi.data[2] > 0;
        return true;
    }
    return false;
}

inline bool starts(const Any& a)
{
    int c = 0, k = 0;
    bool s = false;
    return noteOf(a, c, k, s) && s;
}

inline bool ends(const Any& a)
{
    int c = 0, k = 0;
    bool s = true;
    return noteOf(a, c, k, s) && !s;
}

inline int keyOf(const Any& a)
{
    int c = 0, k = -1;
    bool s = false;
    noteOf(a, c, k, s);
    return k;
}

// Every on starts a silent key and every off ends a sounding one. Returns the keys still sounding at the end.
inline int checkPaired(const std::vector<Any>& out)
{
    std::array<bool, 2048> sounding{};
    int bad = 0;
    for (const Any& a : out) {
        int channel = 0, key = 0;
        bool s = false;
        if (!noteOf(a, channel, key, s)) continue;
        bool& k = sounding[static_cast<std::size_t>(channel * 128 + key)];
        if (s == k) ++bad;
        k = s;
    }
    CHECK(bad == 0);
    return static_cast<int>(std::count(sounding.begin(), sounding.end(), true));
}

} // namespace notecraft::test
