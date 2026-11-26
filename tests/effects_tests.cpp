// Velocity Curve and Humanize, driven the way a host calls process.
#include "effects.hpp"
#include "support.hpp"

using namespace notecraft;
using namespace notecraft::test;

namespace {

void velocityCurveMath()
{
    CHECK(VelocityCurve::curve(0.25, 0.0, 0.5, 1.0, 0.5) == 0.25);
    CHECK(std::abs(VelocityCurve::curve(0.75, 0.2, 0.2, 0.8, 0.5) - 0.5) < 1e-12);
    CHECK(VelocityCurve::curve(0.0, 0.1, 0.4, 0.9, 0.0) == 0.4); // M at the left edge hides L
    CHECK(VelocityCurve::curve(1.0, 0.1, 0.4, 0.9, 1.0) == 0.4); // and at the right edge hides H
}

void velocityCurveDefaultsPassNotesThrough()
{
    VelocityCurve fx;
    Values<5> v;
    defaults<VelocityCurve>(v);
    const auto out = run(fx, v,
                         {note(CLAP_EVENT_NOTE_ON, 3, 60, 0.5), transport(4), midi(5, 0xB0, 1, 99), midi(7, 0x90, 62, 64),
                          note(CLAP_EVENT_NOTE_OFF, 9, 60, 0.3), midi(11, 0x90, 62, 0)},
                         64, 1);
    CHECK(out.size() == 5); // the transport is the host's to the plug-in, never passed on
    CHECK(out[0].note.velocity == 0.5 && out[0].header.time == 3);
    CHECK(out[1].midi.data[2] == 99 && out[1].header.time == 5);
    CHECK(out[2].midi.data[2] == 64 && out[2].header.time == 7);
    CHECK(out[3].note.velocity == 0.3 && isType(out[3], CLAP_EVENT_NOTE_OFF)); // release velocity untouched
    CHECK(out[4].midi.data[2] == 0);                                            // a MIDI off stays an off
}

void velocityCurveParametersApplyAtTheirOffset()
{
    VelocityCurve fx;
    Values<5> v;
    defaults<VelocityCurve>(v);
    const auto out = run(fx, v,
                         {note(CLAP_EVENT_NOTE_ON, 0, 60, 0.5), param(1, VelocityCurve::Lo, 0.0),
                          param(1, VelocityCurve::Mid, 0.0), param(1, VelocityCurve::Hi, 0.0),
                          note(CLAP_EVENT_NOTE_ON, 2, 61, 0.5), midi(3, 0x90, 62, 100)},
                         64, 1);
    CHECK(out.size() == 3); // the parameter changes are the plug-in's, not passed on
    CHECK(out[0].note.velocity == 0.5);
    CHECK(out[1].note.velocity == 1.0 / 127.0); // flattened to zero, a note-on keeps one step
    CHECK(out[2].midi.data[2] == 1);
}

void velocityCurveRandomStaysInRange()
{
    VelocityCurve fx;
    Values<5> v;
    defaults<VelocityCurve>(v);
    v[VelocityCurve::Random].store(1.0);
    std::vector<Any> in;
    for (std::uint32_t i = 0; i < 500; ++i) in.push_back(note(CLAP_EVENT_NOTE_ON, i, 60, 0.5));
    const auto out = run(fx, v, in, 512, 1);
    double lo = 1.0, hi = 0.0;
    for (const Any& a : out) {
        lo = std::min(lo, a.note.velocity);
        hi = std::max(hi, a.note.velocity);
    }
    CHECK(lo >= 0.25 && hi <= 0.75 && hi - lo > 0.4);
}

void humanizeWithoutTimingOrVelocityIsTransparent()
{
    Humanize fx;
    fx.activate(48000.0);
    Values<3> v;
    defaults<Humanize>(v);
    v[Humanize::Timing].store(0.0);
    v[Humanize::Velocity].store(0.0);
    const std::vector<Any> in{note(CLAP_EVENT_NOTE_ON, 3, 60, 0.5), midi(5, 0xB0, 1, 99),
                              note(CLAP_EVENT_NOTE_OFF, 70, 60, 0.0), midi(80, 0x90, 64, 30)};
    std::vector<Any> withTransport = in;
    withTransport.insert(withTransport.begin() + 1, transport(4));
    const auto out = run(fx, v, withTransport, 64, 2);
    CHECK(out.size() == in.size());
    for (std::size_t i = 0; i < out.size() && i < in.size(); ++i) {
        CHECK(out[i].header.time == in[i].header.time);
        CHECK(out[i].header.type == in[i].header.type);
    }
    CHECK(out[0].note.velocity == 0.5 && out[3].midi.data[2] == 30);
}

// Every note moves late by at most Timing, and its off by the same amount: lengths kept, no off before its on.
void humanizeKeepsEachNotesLength(bool midiDialect)
{
    Humanize fx;
    fx.activate(48000.0);
    Values<3> v;
    defaults<Humanize>(v);
    v[Humanize::Timing].store(10.0); // 480 samples
    v[Humanize::Velocity].store(0.0);
    std::vector<Any> in;
    for (std::uint32_t n = 0; n < 200; ++n) {
        const int key = 60 + static_cast<int>(n % 12);
        in.push_back(midiDialect ? midi(n * 100, 0x90, key, 80) : note(CLAP_EVENT_NOTE_ON, n * 100, key, 0.6));
        in.push_back(midiDialect ? midi(n * 100 + 50, 0x80, key, 0) : note(CLAP_EVENT_NOTE_OFF, n * 100 + 50, key, 0.0));
    }
    const auto out = run(fx, v, in, 64, 400);
    CHECK(out.size() == in.size());
    std::array<std::int64_t, 128> onAt{};
    std::array<int, 128> seen{};
    int moved = 0;
    for (const Any& a : out) {
        const bool on = midiDialect ? (a.midi.data[0] & 0xF0) == 0x90 : isType(a, CLAP_EVENT_NOTE_ON);
        const int key = midiDialect ? a.midi.data[1] : a.note.key;
        if (!on) {
            CHECK(a.header.time - onAt[key] == 50);
            continue;
        }
        // Key k's j-th note went in at (12 j + k - 60) * 100.
        const std::int64_t input = (12 * seen[key]++ + key - 60) * 100;
        CHECK(a.header.time >= input && a.header.time <= input + 480);
        moved += a.header.time != input ? 1 : 0;
        onAt[key] = a.header.time;
        if (midiDialect) CHECK(a.midi.data[2] == 80);
    }
    CHECK(moved > 150);
}

void humanizeAtChanceZeroDropsEveryNoteAndItsOff()
{
    Humanize fx;
    fx.activate(48000.0);
    Values<3> v;
    defaults<Humanize>(v);
    v[Humanize::Chance].store(0.0);
    const auto out = run(fx, v,
                         {note(CLAP_EVENT_NOTE_ON, 0, 60, 0.5), midi(1, 0x90, 62, 90), midi(2, 0xB0, 1, 99),
                          note(CLAP_EVENT_NOTE_OFF, 10, 60, 0.0), midi(11, 0x80, 62, 0), midi(12, 0x90, 62, 0)},
                         64, 2);
    // The controller, then the second MIDI off of key 62: its one dropped note's off was already swallowed.
    CHECK(out.size() == 2);
    CHECK(out.size() == 2 && out[0].midi.data[0] == 0xB0 && out[1].midi.data[0] == 0x90 && out[1].header.time == 12);
}

// About half the notes play, and per key the output alternates on, off: never an off without its on.
void humanizeAtChanceHalfPairsWhatItPlays()
{
    Humanize fx;
    fx.activate(48000.0);
    Values<3> v;
    defaults<Humanize>(v);
    v[Humanize::Chance].store(0.5);
    std::vector<Any> in;
    for (std::uint32_t n = 0; n < 400; ++n) {
        const int key = 60 + static_cast<int>(n % 5);
        in.push_back(note(CLAP_EVENT_NOTE_ON, n * 1000, key, 0.6));
        in.push_back(note(CLAP_EVENT_NOTE_OFF, n * 1000 + 500, key, 0.0));
    }
    const auto out = run(fx, v, in, 64, 400 * 1000 / 64 + 20);
    std::array<bool, 128> sounding{};
    int ons = 0;
    for (const Any& a : out) {
        const bool on = isType(a, CLAP_EVENT_NOTE_ON);
        CHECK(sounding[a.note.key] != on);
        sounding[a.note.key] = on;
        ons += on ? 1 : 0;
    }
    CHECK(ons > 120 && ons < 280);
    for (bool s : sounding) CHECK(!s);
}

// All notes off forgets the dropped notes, so a later note on that key still gets its off.
void humanizeAllNotesOffForgetsDroppedNotes()
{
    Humanize fx;
    fx.activate(48000.0);
    Values<3> v;
    defaults<Humanize>(v);
    v[Humanize::Chance].store(0.0);
    v[Humanize::Timing].store(0.0);
    const auto out = run(fx, v,
                         {note(CLAP_EVENT_NOTE_ON, 0, 60, 0.5), midi(1, 0xB0, 123, 0), param(2, Humanize::Chance, 1.0),
                          note(CLAP_EVENT_NOTE_ON, 3, 60, 0.5), note(CLAP_EVENT_NOTE_OFF, 4, 60, 0.0)},
                         64, 1);
    CHECK(out.size() == 3);
    CHECK(out.size() == 3 && isType(out[2], CLAP_EVENT_NOTE_OFF));
}

void humanizeAllNotesOffWaitsForPendingOns()
{
    Humanize fx;
    fx.activate(48000.0);
    Values<3> v;
    defaults<Humanize>(v);
    v[Humanize::Timing].store(100.0);
    const auto out = run(fx, v, {note(CLAP_EVENT_NOTE_ON, 0, 60, 0.5), midi(1, 0xB0, 123, 0), midi(2, 0xB0, 7, 100)}, 64, 100);
    CHECK(out.size() == 3);
    std::int64_t on = -1, allOff = -1;
    for (const Any& a : out) {
        if (isType(a, CLAP_EVENT_NOTE_ON)) on = a.header.time;
        if (isType(a, CLAP_EVENT_MIDI) && a.midi.data[1] == 123) allOff = a.header.time;
        if (isType(a, CLAP_EVENT_MIDI) && a.midi.data[1] == 7) CHECK(a.header.time == 2); // a controller is not delayed
    }
    CHECK(on > 2 && allOff >= on);
}

void humanizeVelocityStaysInRange()
{
    Humanize fx;
    fx.activate(48000.0);
    Values<3> v;
    defaults<Humanize>(v);
    v[Humanize::Timing].store(0.0);
    v[Humanize::Velocity].store(1.0);
    std::vector<Any> in;
    for (std::uint32_t i = 0; i < 500; ++i) in.push_back(note(CLAP_EVENT_NOTE_ON, i, 60, 0.5));
    const auto out = run(fx, v, in, 512, 1);
    CHECK(out.size() == 500);
    double lo = 1.0, hi = 0.0;
    for (const Any& a : out) {
        lo = std::min(lo, a.note.velocity);
        hi = std::max(hi, a.note.velocity);
    }
    CHECK(lo >= 1.0 / 127.0 && hi <= 1.0 && hi - lo > 0.8);
}

void humanizeResetDropsWhatIsPending()
{
    Humanize fx;
    fx.activate(48000.0);
    Values<3> v;
    defaults<Humanize>(v);
    v[Humanize::Timing].store(100.0);
    std::vector<Any> in;
    for (std::uint32_t i = 0; i < 32; ++i) in.push_back(note(CLAP_EVENT_NOTE_ON, i, 60 + static_cast<int>(i), 0.5));
    run(fx, v, in, 64, 1);
    CHECK(fx.size > 0);
    fx.reset();
    CHECK(run(fx, v, {}, 64, 100).empty());
}

} // namespace

int main()
{
    velocityCurveMath();
    velocityCurveDefaultsPassNotesThrough();
    velocityCurveParametersApplyAtTheirOffset();
    velocityCurveRandomStaysInRange();
    humanizeWithoutTimingOrVelocityIsTransparent();
    humanizeAtChanceZeroDropsEveryNoteAndItsOff();
    humanizeAtChanceHalfPairsWhatItPlays();
    humanizeAllNotesOffForgetsDroppedNotes();
    humanizeKeepsEachNotesLength(false);
    humanizeKeepsEachNotesLength(true);
    humanizeAllNotesOffWaitsForPendingOns();
    humanizeVelocityStaysInRange();
    humanizeResetDropsWhatIsPending();
    return finish();
}
