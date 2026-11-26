# NoteCraft

Eleven note effects, CLAP and VST3 from one source (the VST3 is built by
[clap-wrapper](https://github.com/free-audio/clap-wrapper)). Each has one note input, one note output and no
audio. They read and write CLAP note events and MIDI 1.0, and answer in the dialect they were played in.

**Velocity and feel**

- **Velocity Curve** shapes note-on velocity along straight lines from L (input velocity 0) to M (input velocity
  at *Mid position*) to H (input velocity 1). *Lo*, *Mid* and *Hi* set the output velocity at each point, and
  *Random* adds up to ±25 % of full scale.
- **Humanize** plays each note with probability *Chance* (at 0 % nothing plays). A note that plays gets a delay of
  0 to *Timing* ms, and its velocity moves by up to ±*Velocity*/2. Its off keeps its delay, so its length is kept.

**Time**

- **Strum** spreads the notes of a chord (notes that start together) *Time* apart: *Up*, *Down*, *Alternate*
  (switching each chord) or *Random*.
- **Note Echo** repeats each note *Repeats* times, *Time* apart or at a note value of the host's tempo (*Sync*),
  each repeat *Feedback* times as loud as the last and *Pitch* semitones higher. Each repeat is as long as the
  note.
- **Note Length** makes every note *Length* long (*Fixed*), or its played length times *Stretch*.

**Pitch**

- **Scale** moves notes outside a scale (13 scales, any root) to the *Nearest* note in it, the next one *Up* or
  *Down*, or *Drops* them.
- **Chord** adds up to three notes at *Shift 1–3* semitones (0 is off), at *Velocity* times the note's own.
- **Note Filter** passes only notes within a key range and a velocity range.
- **Random Pitch** moves a note, with probability *Chance*, by 1 to *Range* semitones, *Up*, *Down* or *Both*.

**Generators**

- **Arpeggiator** plays the held keys one at a time at *Rate* (1/4 to 1/32, triplets), over *Octaves*, each for
  *Gate* of a step: *Up*, *Down*, *Up/Down*, *Random* or *As played*. While the host plays, steps fall on its beat
  grid; while it is stopped, the first key starts them at once.
- **Latch** keeps notes sounding after their keys are let go: *Hold* keeps the last chord until a key is pressed
  with none held, *Toggle* starts and ends a note with alternate presses of its key.

**What every effect keeps to**

- Every note-on gets its note-off, whatever the parameters do in between: a note's off ends exactly the notes its
  on started. A key played again while held ends its notes first.
- When two notes land on one key (a chord on a scale, overlapping echoes), the key restarts, and its off goes out
  with the last note that holds it. No off goes out without an on.
- A note-on is taken only while there is room for its off as well, so a flood (16 two-second echoes of fast
  playing) drops new notes, never an off. An all-notes-off (CC 120/123, wildcard off) ends everything sounding or
  pending.
- A note-on never leaves at velocity 0, which would read as a note-off. Notes an effect makes or moves to another
  key carry no host note id.
- Nothing moves notes early: that needs a host that compensates note-effect latency. Note Length only lengthens
  for the same reason.

The VST3 is categorized `Instrument|Synth` with an event output bus, since VST3 has no MIDI effect category.
That is the plug-in class transit#671 describes.

## Build

CMake 3.28+ and a C++17 compiler. The first configure downloads CLAP 1.2.10, clap-wrapper v0.18.0 and the VST3
SDK 3.8.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On macOS, install with:

```sh
cp -R build/NoteCraft.clap ~/Library/Audio/Plug-Ins/CLAP/
cp -R build/NoteCraft.vst3 ~/Library/Audio/Plug-Ins/VST3/
```

## Validate

```sh
clap-validator validate build/NoteCraft.clap --only-failed
clap-validator fuzz -j2 -d30s build/NoteCraft.clap
# Steinberg's validator, built from the downloaded SDK:
cmake -S build/cpm/vst3sdk -B build/vst3-validator -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF -DSMTG_ADD_VST3_PLUGINS_SAMPLES=OFF -DSMTG_RUN_VST_VALIDATOR=OFF
cmake --build build/vst3-validator --target validator
build/vst3-validator/bin/Release/validator build/NoteCraft.vst3
```

clap-validator 0.4.1 reports two failures for Latch (`param-set-events`, `param-set-no-cookies`): its one
parameter has two values, and the validator's fixed random draw is the default, so it sees no change. Every other
test passes.

There is no custom editor yet: hosts show their generic parameter controls.
