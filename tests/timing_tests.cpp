// Strum, Note Echo and Note Length.
#include "support.hpp"
#include "timing.hpp"

#include <utility>

using namespace notecraft;
using namespace notecraft::test;

namespace {

using Times = std::vector<std::pair<int, std::uint32_t>>; // key, time

Times onsOf(const std::vector<Any>& out)
{
    Times t;
    for (const Any& a : out) if (starts(a)) t.emplace_back(keyOf(a), a.header.time);
    return t;
}

Times offsOf(const std::vector<Any>& out)
{
    Times t;
    for (const Any& a : out) if (ends(a)) t.emplace_back(keyOf(a), a.header.time);
    return t;
}

void strumSpreadsAChord()
{
    Strum fx;
    fx.activate(kRate);
    Values<2> v;
    defaults<Strum>(v);
    v[Strum::Time].store(10.0); // 480 samples
    const std::vector<Any> chord{on(0, 67), on(0, 60), on(0, 64), off(2000, 60), off(2000, 64), off(2000, 67)};
    auto out = run(fx, v, chord, 64, 100);
    CHECK(checkPaired(out) == 0);
    CHECK((onsOf(out) == Times{{60, 0}, {64, 480}, {67, 960}}));
    CHECK((offsOf(out) == Times{{60, 2000}, {64, 2480}, {67, 2960}})); // each off keeps its note's delay

    v[Strum::Direction].store(1.0); // Down
    out = run(fx, v, chord, 64, 100);
    CHECK((onsOf(out) == Times{{67, 0}, {64, 480}, {60, 960}}));
}

void strumAlternatesAndLeavesSingleNotesAlone()
{
    Strum fx;
    fx.activate(kRate);
    Values<2> v;
    defaults<Strum>(v);
    v[Strum::Time].store(10.0);
    v[Strum::Direction].store(2.0); // Alternate
    const auto out = run(fx, v,
                         {on(0, 60), on(0, 64), off(1000, 60), off(1000, 64), on(5000, 72), off(5100, 72), on(9000, 60),
                          on(9000, 64), off(10000, 60), off(10000, 64)},
                         64, 300);
    CHECK(checkPaired(out) == 0);
    CHECK((onsOf(out) == Times{{60, 0}, {64, 480}, {72, 5000}, {64, 9000}, {60, 9480}}));
}

void echoRepeatsFadeAndRise()
{
    NoteEcho fx;
    fx.activate(kRate);
    Values<5> v;
    defaults<NoteEcho>(v);
    v[NoteEcho::Repeats].store(2.0);
    v[NoteEcho::Time].store(100.0); // 4800 samples
    v[NoteEcho::Feedback].store(0.5);
    v[NoteEcho::Pitch].store(12.0);
    const auto out = run(fx, v, {on(0, 60, 0.8), off(1000, 60)}, 64, 400);
    CHECK(checkPaired(out) == 0);
    CHECK((onsOf(out) == Times{{60, 0}, {72, 4800}, {84, 9600}}));
    CHECK((offsOf(out) == Times{{60, 1000}, {72, 5800}, {84, 10600}}));
    std::vector<double> velocities;
    for (const Any& a : out) if (starts(a)) velocities.push_back(a.note.velocity);
    CHECK((velocities == std::vector<double>{0.8, 0.4, 0.2}));
}

void echoFollowsTheHostsTempo()
{
    NoteEcho fx;
    fx.activate(kRate);
    Values<5> v;
    defaults<NoteEcho>(v);
    v[NoteEcho::Repeats].store(1.0);
    v[NoteEcho::Sync].store(4.0); // 1/8
    clap_event_transport_t tr{};
    tr.flags = CLAP_TRANSPORT_HAS_TEMPO;
    tr.tempo = 120.0; // an eighth is 0.25 s
    const auto out = run(fx, v, {on(0, 60), off(100, 60)}, 64, 300, &tr);
    CHECK((onsOf(out) == Times{{60, 0}, {60, 12000}}));
}

// Repeats of one key overlap each other and the next note on that key: still one on, then one off, at a time.
void echoOverlapsOnOneKeyStayPaired()
{
    NoteEcho fx;
    fx.activate(kRate);
    Values<5> v;
    defaults<NoteEcho>(v);
    v[NoteEcho::Time].store(10.0);
    const auto out = run(fx, v, {on(0, 60), on(200, 60), off(5000, 60), off(5200, 60)}, 64, 300);
    CHECK(checkPaired(out) == 0);
}

void noteLengthFixedIgnoresThePlayedOff()
{
    NoteLength fx;
    fx.activate(kRate);
    Values<3> v;
    defaults<NoteLength>(v);
    v[NoteLength::Length].store(100.0); // 4800 samples
    const auto out = run(fx, v, {on(0, 60), off(100, 60), on(1000, 62), off(20000, 62)}, 64, 400);
    CHECK(checkPaired(out) == 0);
    CHECK((offsOf(out) == Times{{60, 4800}, {62, 5800}}));
}

void noteLengthStretches()
{
    NoteLength fx;
    fx.activate(kRate);
    Values<3> v;
    defaults<NoteLength>(v);
    v[NoteLength::Mode].store(1.0);
    v[NoteLength::Stretch].store(2.0);
    // The mode changes while the note is held: it keeps the mode it started with.
    const auto out = run(fx, v, {on(0, 60), param(10, NoteLength::Mode, 0.0), off(1000, 60)}, 64, 100);
    CHECK(checkPaired(out) == 0);
    CHECK((offsOf(out) == Times{{60, 2000}}));
}

} // namespace

int main()
{
    strumSpreadsAChord();
    strumAlternatesAndLeavesSingleNotesAlone();
    echoRepeatsFadeAndRise();
    echoFollowsTheHostsTempo();
    echoOverlapsOnOneKeyStayPaired();
    noteLengthFixedIgnoresThePlayedOff();
    noteLengthStretches();
    return finish();
}
