// Every effect, random playing and random parameter changes, both dialects: every on gets its off, no off comes
// without an on, and once every key is let go nothing is left sounding.
#include "effects.hpp"
#include "generators.hpp"
#include "support.hpp"
#include "timing.hpp"
#include "transforms.hpp"

using namespace notecraft;
using namespace notecraft::test;

namespace {

Any noteEvent(bool midiDialect, bool start, std::uint32_t time, int key, double velocity)
{
    if (midiDialect) {
        return midi(time, start ? 0x90 : 0x80, static_cast<std::uint8_t>(key),
                    start ? static_cast<std::uint8_t>(1 + std::lround(velocity * 126.0)) : 0);
    }
    return note(start ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF, time, key, start ? velocity : 0.0);
}

// A random performance: `events` steps of notes, releases and parameter changes, then every held key let go.
template <class E>
std::vector<Any> perform(Rng& rng, int events, bool midiDialect, bool overlaps, std::uint32_t& end)
{
    std::vector<Any> in;
    std::array<int, 128> held{};
    std::uint32_t t = 0;
    for (int i = 0; i < events; ++i) {
        t += static_cast<std::uint32_t>(rng.below(400));
        const double r = rng.next();
        const int key = 48 + rng.below(24);
        if (r < 0.45 && (overlaps || held[key] == 0)) {
            in.push_back(noteEvent(midiDialect, true, t, key, rng.next()));
            ++held[key];
        } else if (r < 0.9) {
            for (int k = 0; k < 128; ++k) {
                const int j = (key + k) % 128;
                if (held[j] > 0) {
                    in.push_back(noteEvent(midiDialect, false, t, j, 0.0));
                    --held[j];
                    break;
                }
            }
        } else if (r < 0.97) {
            const auto id = static_cast<std::size_t>(rng.below(static_cast<int>(E::params.size())));
            const Param& p = E::params[id];
            in.push_back(param(t, static_cast<clap_id>(id), p.min + rng.next() * (p.max - p.min)));
        } else {
            in.push_back(midi(t, 0xB0, 1, static_cast<std::uint8_t>(rng.below(128))));
        }
    }
    for (int k = 0; k < 128; ++k) {
        for (; held[k] > 0; --held[k]) in.push_back(noteEvent(midiDialect, false, t + 1, k, 0.0));
    }
    end = t + 2;
    return in;
}

// `allOff`: the effect holds notes past their release by design, so the performance ends with all-notes-off.
template <class E>
void fuzz(bool overlaps, bool allOff = false)
{
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        for (bool midiDialect : {false, true}) {
            Rng rng{seed * 0x2545F4914F6CDD1Dull};
            E fx;
            fx.activate(kRate);
            Values<E::params.size()> v;
            defaults<E>(v);
            std::uint32_t end = 0;
            auto in = perform<E>(rng, 800, midiDialect, overlaps, end);
            if (allOff) in.push_back(midi(end, 0xB0, 123, 0));
            const std::uint32_t frames = 256;
            const std::uint32_t tail = static_cast<std::uint32_t>(40.0 * kRate); // the longest echo, 16 x 2 s, and more
            const auto out = run(fx, v, in, frames, (end + tail) / frames);
            const int sounding = checkPaired(out);
            if (sounding != 0) std::printf("  %s, seed %d, %s: %d keys left sounding\n", E::name, static_cast<int>(seed),
                                           midiDialect ? "MIDI" : "CLAP", sounding);
            CHECK(sounding == 0);
            CHECK(fx.out.pending() == 0);
        }
    }
}

// Velocity Curve and Humanize have no scheduler of their own to check, so only their pairing.
template <class E>
void fuzzPassThrough()
{
    for (std::uint64_t seed = 1; seed <= 3; ++seed) {
        for (bool midiDialect : {false, true}) {
            Rng rng{seed * 0x9E3779B97F4A7C15ull};
            E fx;
            fx.activate(kRate);
            Values<E::params.size()> v;
            defaults<E>(v);
            std::uint32_t end = 0;
            const auto in = perform<E>(rng, 800, midiDialect, false, end);
            const auto out = run(fx, v, in, 256, (end + 48000) / 256);
            CHECK(checkPaired(out) == 0);
        }
    }
}

// Sixteen two-second echoes of a thousand notes is more than the queue holds: notes are dropped, never an off.
void aFloodDropsNotesNotOffs()
{
    NoteEcho fx;
    fx.activate(kRate);
    Values<5> v;
    defaults<NoteEcho>(v);
    v[NoteEcho::Repeats].store(16.0);
    v[NoteEcho::Time].store(2000.0);
    v[NoteEcho::Feedback].store(1.0);
    std::vector<Any> in;
    for (std::uint32_t i = 0; i < 1000; ++i) {
        in.push_back(on(i * 48, 30 + static_cast<int>(i % 60)));
        in.push_back(off(i * 48 + 24, 30 + static_cast<int>(i % 60)));
    }
    const auto out = run(fx, v, in, 512, static_cast<std::uint32_t>(40.0 * kRate / 512));
    int ons = 0;
    for (const Any& a : out) ons += starts(a) ? 1 : 0;
    CHECK(ons < 17000); // some were dropped
    CHECK(checkPaired(out) == 0);
}

} // namespace

int main()
{
    fuzzPassThrough<VelocityCurve>();
    fuzzPassThrough<Humanize>();
    fuzz<Strum>(true);
    fuzz<NoteEcho>(true);
    fuzz<NoteLength>(true);
    fuzz<Scale>(true);
    fuzz<Chord>(true);
    fuzz<NoteFilter>(true);
    fuzz<RandomPitch>(true);
    fuzz<Arpeggiator>(true);
    fuzz<Latch>(true, true);
    aFloodDropsNotesNotOffs();
    return finish();
}
