// Scale, Chord, Note Filter and Random Pitch.
#include "support.hpp"
#include "transforms.hpp"

using namespace notecraft;
using namespace notecraft::test;

namespace {

std::vector<int> keys(const std::vector<Any>& out, bool ons)
{
    std::vector<int> k;
    for (const Any& a : out) if (ons ? starts(a) : ends(a)) k.push_back(keyOf(a));
    return k;
}

void scaleSnaps()
{
    const std::uint16_t major = kScaleMasks[0];
    CHECK(Scale::snap(64, 0, major, 0) == 64);
    CHECK(Scale::snap(61, 0, major, 0) == 60); // C#: a tie goes down
    CHECK(Scale::snap(66, 0, major, 0) == 65); // F#: a tie goes down
    CHECK(Scale::snap(61, 0, major, 1) == 62);
    CHECK(Scale::snap(63, 0, major, 2) == 62);
    CHECK(Scale::snap(61, 0, major, 3) == -1);
    CHECK(Scale::snap(70, 9, kScaleMasks[1], 0) == 69); // A minor: A#, a tie, goes down to A
    // C# major pentatonic: the top two keys, F# and G, are outside it, and nothing is above them.
    CHECK(Scale::snap(127, 1, kScaleMasks[9], 1) == -1);
    CHECK(Scale::snap(127, 1, kScaleMasks[9], 0) == 125);
}

void scaleOffFollowsTheNoteWhereverItWent()
{
    Scale fx;
    fx.activate(kRate);
    Values<3> v;
    defaults<Scale>(v);
    // The root changes while C# is held: its off still ends the C it became.
    const auto out = run(fx, v, {on(0, 61), param(10, Scale::Root, 1.0), off(20, 61), on(30, 61), off(40, 61)}, 64, 2);
    CHECK(checkPaired(out) == 0);
    CHECK((keys(out, true) == std::vector<int>{60, 61}));
    CHECK((keys(out, false) == std::vector<int>{60, 61}));
}

void scaleDropsWithTheirOffs()
{
    Scale fx;
    fx.activate(kRate);
    Values<3> v;
    defaults<Scale>(v);
    v[Scale::Action].store(3.0);
    const auto out = run(fx, v, {on(0, 61), on(1, 62), off(20, 61), off(21, 62)}, 64, 2);
    CHECK((keys(out, true) == std::vector<int>{62}));
    CHECK((keys(out, false) == std::vector<int>{62}));
}

void chordAddsNotes()
{
    Chord fx;
    fx.activate(kRate);
    Values<4> v;
    defaults<Chord>(v);
    v[Chord::Velocity].store(0.5);
    const auto out = run(fx, v, {on(0, 60, 0.8), off(100, 60), on(200, 125), off(300, 125)}, 64, 10);
    CHECK(checkPaired(out) == 0);
    CHECK((keys(out, true) == std::vector<int>{60, 64, 67, 125})); // shifts off the keyboard are left out
    CHECK(out.size() > 2 && out[0].note.velocity == 0.8 && out[1].note.velocity == 0.4);
}

// C and E with a major third each: E is asked for twice. It restarts, and ends with the last note holding it.
void chordSharedKeysStayPaired()
{
    Chord fx;
    fx.activate(kRate);
    Values<4> v;
    defaults<Chord>(v);
    const auto out = run(fx, v, {on(0, 60), on(10, 64), off(100, 60), off(200, 64)}, 64, 10);
    CHECK(checkPaired(out) == 0);
    int eEnds = 0, eEndAt = 0;
    for (const Any& a : out) {
        if (ends(a) && keyOf(a) == 64) {
            ++eEnds;
            eEndAt = static_cast<int>(a.header.time);
        }
    }
    CHECK(eEnds == 2 && eEndAt == 200); // once to restart it at 10, once at the end
}

void noteFilterPassesItsRanges()
{
    NoteFilter fx;
    fx.activate(kRate);
    Values<4> v;
    defaults<NoteFilter>(v);
    v[NoteFilter::LowKey].store(72.0); // upside down: reads as 60..72
    v[NoteFilter::HighKey].store(60.0);
    v[NoteFilter::LowVelocity].store(64.0);
    const auto out = run(fx, v, {on(0, 59), on(1, 60, 0.8), on(2, 65, 0.3), on(3, 72, 0.9), off(10, 59), off(11, 60),
                                 off(12, 65), off(13, 72)},
                         64, 2);
    CHECK(checkPaired(out) == 0);
    CHECK((keys(out, true) == std::vector<int>{60, 72}));
}

void randomPitchStaysInRange()
{
    RandomPitch fx;
    fx.activate(kRate);
    Values<3> v;
    defaults<RandomPitch>(v);
    v[RandomPitch::Chance].store(1.0);
    v[RandomPitch::Range].store(2.0);
    std::vector<Any> in;
    for (std::uint32_t i = 0; i < 400; ++i) {
        in.push_back(on(i * 10, 60));
        in.push_back(off(i * 10 + 5, 60));
    }
    auto out = run(fx, v, in, 64, 70);
    CHECK(checkPaired(out) == 0);
    std::array<int, 128> seen{};
    for (int k : keys(out, true)) ++seen[static_cast<std::size_t>(k)];
    CHECK(seen[58] > 0 && seen[59] > 0 && seen[61] > 0 && seen[62] > 0);
    CHECK(seen[58] + seen[59] + seen[61] + seen[62] == 400);

    v[RandomPitch::Direction].store(1.0); // Up
    out = run(fx, v, in, 64, 70);
    for (int k : keys(out, true)) CHECK(k == 61 || k == 62);

    v[RandomPitch::Chance].store(0.0);
    out = run(fx, v, in, 64, 70);
    for (int k : keys(out, true)) CHECK(k == 60);
}

} // namespace

int main()
{
    scaleSnaps();
    scaleOffFollowsTheNoteWhereverItWent();
    scaleDropsWithTheirOffs();
    chordAddsNotes();
    chordSharedKeysStayPaired();
    noteFilterPassesItsRanges();
    randomPitchStaysInRange();
    return finish();
}
