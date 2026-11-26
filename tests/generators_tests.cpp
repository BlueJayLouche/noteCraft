// Arpeggiator and Latch.
#include "generators.hpp"
#include "support.hpp"

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

std::vector<int> firstKeys(const std::vector<Any>& out, std::size_t n)
{
    std::vector<int> k;
    for (const Any& a : out) if (starts(a) && k.size() < n) k.push_back(keyOf(a));
    return k;
}

// Stopped, 120 BPM: a sixteenth is 6000 samples. Gate 50 %.
void arpPlaysHeldKeysUpwards()
{
    Arpeggiator fx;
    fx.activate(kRate);
    Values<4> v;
    defaults<Arpeggiator>(v);
    const auto out = run(fx, v, {on(0, 67), on(0, 60), on(0, 64), off(30000, 60), off(30000, 64), off(30000, 67)}, 64, 1000);
    CHECK(checkPaired(out) == 0);
    CHECK((onsOf(out) == Times{{60, 0}, {64, 6000}, {67, 12000}, {60, 18000}, {64, 24000}}));
    for (const Any& a : out) {
        if (ends(a)) CHECK(a.header.time % 6000 == 3000);
    }
}

void arpModesAndOctaves()
{
    const std::vector<Any> held{on(0, 64), on(0, 60), on(0, 67), off(60000, 60), off(60000, 64), off(60000, 67)};
    const auto keysFor = [&held](double mode, double octaves) {
        Arpeggiator fx;
        fx.activate(kRate);
        Values<4> v;
        defaults<Arpeggiator>(v);
        v[Arpeggiator::Mode].store(mode);
        v[Arpeggiator::Octaves].store(octaves);
        const auto out = run(fx, v, held, 64, 1000);
        CHECK(checkPaired(out) == 0);
        return firstKeys(out, 7);
    };
    CHECK((keysFor(1, 1) == std::vector<int>{67, 64, 60, 67, 64, 60, 67}));
    CHECK((keysFor(2, 1) == std::vector<int>{60, 64, 67, 64, 60, 64, 67}));
    CHECK((keysFor(4, 1) == std::vector<int>{64, 60, 67, 64, 60, 67, 64})); // as played
    CHECK((keysFor(0, 2) == std::vector<int>{60, 64, 67, 72, 76, 79, 60}));
}

// Playing at beat 0.1, 120 BPM: the first sixteenth falls on beat 0.25, 0.15 beats (3600 samples) in.
void arpFollowsTheHostsGrid()
{
    Arpeggiator fx;
    fx.activate(kRate);
    Values<4> v;
    defaults<Arpeggiator>(v);
    clap_event_transport_t tr{};
    tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING;
    tr.tempo = 120.0;
    tr.song_pos_beats = CLAP_BEATTIME_FACTOR / 10;
    const auto out = run(fx, v, {on(0, 60), off(20000, 60)}, 64, 400, &tr);
    CHECK(checkPaired(out) == 0);
    const Times ons = onsOf(out);
    CHECK(ons.size() == 3);
    CHECK(ons.size() == 3 && ons[0].second >= 3599 && ons[0].second <= 3601 && ons[1].second - ons[0].second >= 5999 &&
          ons[1].second - ons[0].second <= 6001);
}

void latchHoldsTheLastChord()
{
    Latch fx;
    fx.activate(kRate);
    Values<1> v;
    defaults<Latch>(v);
    const auto out = run(fx, v,
                         {on(0, 60), on(1, 64), off(100, 60), off(101, 64), on(1000, 67), on(1001, 60), off(1100, 67),
                          off(1101, 60), midi(2000, 0xB0, 123, 0)},
                         64, 40);
    CHECK(checkPaired(out) == 0);
    std::vector<std::pair<int, bool>> notes;
    for (const Any& a : out) if (starts(a) || ends(a)) notes.emplace_back(keyOf(a), starts(a));
    // 60 and 64 sound past their release; the new chord ends them (60 starts again), and all-notes-off ends the
    // rest, in key order.
    CHECK((notes == std::vector<std::pair<int, bool>>{{60, true}, {64, true}, {60, false}, {64, false}, {67, true},
                                                      {60, true}, {60, false}, {67, false}}));
}

void latchToggles()
{
    Latch fx;
    fx.activate(kRate);
    Values<1> v;
    defaults<Latch>(v);
    v[Latch::Mode].store(1.0);
    const auto out = run(fx, v, {on(0, 60), off(100, 60), on(200, 60), off(300, 60)}, 64, 10);
    CHECK(checkPaired(out) == 0);
    CHECK(out.size() == 2 && starts(out[0]) && out[0].header.time == 0 && ends(out[1]) && out[1].header.time == 200);
}

} // namespace

int main()
{
    arpPlaysHeldKeysUpwards();
    arpModesAndOctaves();
    arpFollowsTheHostsGrid();
    latchHoldsTheLastChord();
    latchToggles();
    return finish();
}
