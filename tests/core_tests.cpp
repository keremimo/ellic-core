// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// ellic-core's own tests: the engine, the model and the files, built with
// the core alone, without JUCE or a product (tests/test_core.py). Moved here
// from melodee-sample's tests/melodee.cpp, which keeps the tests of
// Melodee's screens and its MPC shell.
#include "melodee/engine/effects.h"
#include "melodee/engine/channel_strip.h"
#include "melodee/model/slice_banks.h"
#include "melodee/model/slice_hits.h"
#include "melodee/engine/macros.h"
#include <sstream>
#include <fstream>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <map>
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>
#include "melodee/engine/sample_data.h"
#include "melodee/engine/onsets.h"
#include "melodee/engine/root_find.h"
#include "melodee/engine/sampler.h"
#include "melodee/engine/sfz.h"
#include "melodee/engine/keygroup.h"
#include "melodee/engine/exs.h"
#include "melodee/engine/sf2.h"
#include "melodee/engine/dspreset.h"
#include "melodee/shell/stem_writer.h"
#include "melodee/shell/expansion_install.h"
#include "melodee/engine/stream_pager.h"
#include "melodee/shell/sample_cache.h"
#include "melodee/engine/take_recorder.h"
#include "melodee/shell/chop.h"
#include "melodee/shell/xpm_write.h"
#include "melodee/shell/drum_rack.h"
#include "melodee/engine/auto_probe.h"
#include "melodee/engine/external_notes.h"
#include "melodee/engine/release_fit.h"
#include "melodee/model/card_folders.h"
#include "melodee/shell/auto_sample.h"
#include "melodee/engine/loop_find.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/print_player.h"
#include "melodee/engine/print_render.h"
#include "melodee/model/print.h"
#include "melodee/engine/plugin_engine.h"
#include "melodee/engine/punch.h"
#include "melodee/engine/pad_fx_info.h"
#include "melodee/model/mod_matrix.h"
#include "melodee/engine/arp.h"
#include "melodee/model/note_edits.h"
#include "melodee/model/chords.h"
#include "melodee/model/progression.h"
#include "melodee/model/project_file.h"
#include "melodee/model/keygroup_track.h"
#include "melodee/model/generators.h"
#include "melodee/model/smf.h"
#include "melodee/model/lane_value.h"
#include "melodee/model/scales.h"
#include "melodee/shell/capture_ring.h"
#include "melodee/shell/record_place.h"
#include "melodee/engine/voice_tap.h"
#include "melodee/shell/midi_route.h"
#include "melodee/shell/mtc_in.h"
#include "melodee/engine/midi_out.h"
#include "melodee/model/step_order.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/model/fader.h"
#include "melodee/engine/effect_info.h"
#include "melodee/model/sound_file.h"
#include "melodee/shell/stems.h"
#include "melodee/shell/sample_memory.h"
#include "melodee/shell/stream_holds.h"
#include "melodee/engine/stream_pager.h"
#include "melodee/shell/free_length.h"
#include "melodee/shell/follow_tempo.h"
#include "melodee/shell/project_kits.h"
#include "melodee/shell/pad_gap.h"
#include "melodee/shell/zone_shapes.h"
#include "melodee/model/pad_params.h"
#include "melodee/model/input_config.h"
#include "melodee/model/song_flatten.h"
#include "melodee/engine/recall_ring.h"
#include "melodee/engine/bounce.h"
#include "melodee/engine/looper.h"
#include <algorithm>
#include <tuple>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unistd.h>
#include <memory>
#include <string>
#include <vector>

using namespace melodee;

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace {


float peak_of(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
    float peak = 0;
    for (size_t n = from; n < std::min(to, v.size()); ++n) { CHECK(std::isfinite(v[n])); peak = std::max(peak, std::fabs(v[n])); }
    return peak;
}
// A point of Motion (roadmap P1: the pattern's own) on sixteenth `sixteenth`; a lock with `hold` ticks.
MotionPoint point(unsigned sixteenth, unsigned param, int value, unsigned pad = 0, unsigned hold = 0) {
    MotionPoint m;
    m.tick = sixteenth * sixteenth_ticks; m.param = uint8_t(param); m.value = int16_t(value); m.pad = uint8_t(pad); m.hold = uint16_t(hold);
    return m;
}
// One note on step `step` of the pattern's Q, in place of what the step held.
void put(Pattern& p, unsigned step, uint8_t key, uint8_t velocity, uint8_t length = 1, uint8_t flags = 0) {
    Step s;
    s.notes[0] = {key, velocity};
    s.length = length; s.flags = flags;
    p.set_step(step, s);
}

// A synth track since Melodee's synths became plug-ins of their own (Melodee Poly, PD and Chip): a PLUGIN track. In
// the host tests a ToneRunner plays it, as the rack's plug-in would: a sine a note at its key's pitch and its
// velocity's level, a 5 ms attack and a release of `release` seconds, keeping every MIDI message it was sent.
struct ToneRunner final : PluginRunner {
    struct Message { uint8_t status = 0, a = 0, b = 0; };
    std::vector<Message> heard;
    float release = 0.003f;
    struct Voice { int key = -1; bool held = false; float phase = 0, level = 0, gain = 0; };
    std::array<Voice, 16> voices {};
    void midi(uint8_t status, uint8_t a, uint8_t b) override {
        heard.push_back({status, a, b});
        uint8_t kind = status & 0xf0;
        if (kind == 0x90 && b) {
            Voice* v = nullptr;
            for (auto& x : voices) if (x.key == a) v = &x;
            if (!v) for (auto& x : voices) if (x.key < 0) { v = &x; break; }
            if (!v) v = &voices[0];
            v->key = a; v->held = true; v->gain = b / 127.0f * 0.3f;
        } else if (kind == 0x80 || kind == 0x90) {
            for (auto& x : voices) if (x.key == a) x.held = false;
        } else if (kind == 0xb0 && (a == 120 || a == 123)) {
            for (auto& x : voices) { x.held = false; if (a == 120) x = Voice {}; }
        }
    }
    void silence() override { for (auto& x : voices) x = Voice {}; }
    void render(float* left, float* right, unsigned frames) override {
        float fall = std::exp(-1.0f / (release * 44100.0f)), rise = 1.0f - std::exp(-1.0f / (0.005f * 44100.0f));
        for (auto& v : voices) {
            if (v.key < 0) continue;
            float step = 440.0f * std::exp2((v.key - 69) / 12.0f) / 44100.0f;
            for (unsigned f = 0; f < frames; ++f) {
                v.level = v.held ? v.level + (v.gain - v.level) * rise : v.level * fall;
                if (!v.held && v.level < 1e-5f) { v = Voice {}; break; } // ended on its own frame, however the blocks are cut
                float x = std::sin(2 * 3.14159265f * v.phase) * v.level;
                v.phase += step; v.phase -= std::floor(v.phase);
                left[f] += x; right[f] += x;
            }
        }
    }
    unsigned count(uint8_t kind, int a = -1, int b = -1) const {
        unsigned n = 0;
        for (auto& m : heard) if ((m.status & 0xf0) == kind && (a < 0 || m.a == a) && (b < 0 || m.b == b)) ++n;
        return n;
    }
};
// Track `t` of a project a synth: a PLUGIN track on plug-in entry `entry`.
void synth(Track& t, unsigned entry) {
    load_sound(t, EngineKind::Plugin, 0);
    t.plugin = uint8_t(entry);
}
// A second of a sine at `hz`, half scale, for a kit's pad to play.
SampleData tone_sample(float hz = 220) {
    SampleData d;
    d.frames = 44100; d.left.resize(d.frames); d.right.resize(d.frames);
    for (unsigned n = 0; n < d.frames; ++n) d.left[n] = d.right[n] = 0.5f * std::sin(2 * 3.14159265f * hz * float(n) / 44100.0f);
    return d;
}
// Track `t` a kit whose first four pads play sample slot 0 (offer it a tone_sample()), each held as long as its note.
void tone_kit(Project& project, unsigned t) {
    load_sound(project.tracks[t], EngineKind::Sampler, 0);
    std::snprintf(project.samples.edit(0).path, sizeof(SampleRef::path), "/media/card/Samples/Tone.wav");
    project.samples.edit(0).frames = 44100;
    for (unsigned pad = 0; pad < 4; ++pad) project.tracks[t].pads[pad].sample = 0;
}
// The synths a host's tracks play: a ToneRunner for each PLUGIN track's entry, offered to the host (it takes them
// at its next block). Kept as long as the host plays them.
struct Tones {
    std::array<ToneRunner, max_plugins> at {};
    void offer(EngineHost& host, const Project& project) {
        for (const auto& t : project.tracks)
            if (t.engine == EngineKind::Plugin && t.plugin < max_plugins) CHECK(host.offer_plugin(t.plugin, &at[t.plugin]));
    }
};
// A Motion lane of an engine's own value (0..max_params-1), for the tests of how Motion is kept.
constexpr unsigned engine_lane = 16;
// For tests that only need their synth tracks heard: tone runners for a host's PLUGIN tracks, kept to the run's end.
void tones_for(EngineHost& host, const Project& project) {
    static std::vector<std::unique_ptr<Tones>> kept;
    kept.push_back(std::make_unique<Tones>());
    kept.back()->offer(host, project);
}

void layouts() {
    Project project;
    project.root = 2; project.scale = 2; // D Dorian
    Track track;
    track.octave = 2;
    // Pad 1 is D2 (38); sixteen notes of the scale up to E4.
    CHECK(pad_note(project, track, 0).key == 38 && pad_note(project, track, 0).root);
    CHECK(pad_note(project, track, 1).key == 40);
    CHECK(pad_note(project, track, 7).key == 50 && pad_note(project, track, 7).root);
    CHECK(pad_note(project, track, 15).key == 64);
    CHECK(note_name(38) == "D2" && note_name(60) == "C4");
    track.layout = PadLayout::Chromatic;
    CHECK(pad_note(project, track, 5).key == 43);
    track.layout = PadLayout::Levels; track.last_note = 57;
    CHECK(pad_note(project, track, 9).key == 57 && level_velocity(0) < level_velocity(15) && level_velocity(15) == 127);
}

// A solo counts only on a track that plays something (Track::used(), as the pad lights have it): an empty track left
// soloed, as a project kept it from Track Mute's Shift + pad, silences nothing; a kit soloed still silences the other.
void drum_racks() {
    // Ableton's Drum Rack presets (shell/drum_rack.h): their samples found in the libraries, each pad's values in
    // the kit's terms, the pads by note, and the rack as a project of one kit track.
    const AbletonLibraries move {"/data/CoreLibrary", "/data/UserData/UserLibrary"};
    CHECK(ableton_sample_path("ableton:/packs/abl-core-library/Samples/Drums/Kick/Kick%20808%20Solid.wav", "/x", move) ==
          "/data/CoreLibrary/Samples/Drums/Kick/Kick 808 Solid.wav");
    CHECK(ableton_sample_path("ableton:/user-library/Recordings/Take%201.wav", "/x", move) == "/data/UserData/UserLibrary/Recordings/Take 1.wav");
    CHECK(ableton_sample_path("ableton:/packs/another-pack/Samples/A.wav", "/x", move).empty());
    CHECK(ableton_sample_path("ableton:/user-library/A.wav", "/x", AbletonLibraries {}).empty()); // no library named
    CHECK(ableton_sample_path("file:///card/One%20Shot.wav", "/x", move) == "/card/One Shot.wav");
    CHECK(ableton_sample_path("Samples/Clap.wav", "/kits", move) == "/kits/Samples/Clap.wav");
    CHECK(ableton_sample_path("/card/Clap.wav", "/kits", move) == "/card/Clap.wav");
    CHECK(ableton_sample_path("", "/kits", move).empty());
    // Notes onto pads: 36 is A01, as the kit's notes run; past H16 or below 36, none.
    CHECK(drum_rack_pad(36) == 0 && drum_rack_pad(51) == 15 && drum_rack_pad(52) == 16);
    CHECK(drum_rack_pad(35) == -1 && drum_rack_pad(163) == 127 && drum_rack_pad(164) == -1);
    // A pad's values in the kit's terms.
    DrumRackPad kick;
    kick.note = 36; kick.volume_db = -6; kick.pan = 0.5; kick.transpose = -2; kick.detune = 0.25;
    kick.start = 0.25; kick.length = 0.5; kick.choke = 1; kick.velocity_to_volume = 0.5; kick.gate = true;
    kick.filter = "Lowpass"; kick.cutoff_hz = 1000; kick.resonance = 0.3;
    PadSound pad;
    drum_rack_pad_sound(kick, 44100, pad);
    CHECK(pad.start == 11025 && pad.end == 33075);
    CHECK(pad.level == 69 && std::fabs(volume_db(pad.level) + 6.0f) < 0.2f && pad.pan == 25 && pad.pitch == -2 && pad.fine == 25);
    CHECK(pad.group == 1 && pad.velocity_sense == 50 && pad.play == PadPlay::Gate);
    CHECK(pad.filter == PadFilter::Lpf4 && pad.cutoff == 57 && pad.resonance == 30); // 1 kHz: 5.6 octaves over 20 Hz
    DrumRackPad plain;
    plain.note = 37; plain.volume_db = -9; plain.filter = "Lowpass"; plain.cutoff_hz = 21999.99;
    PadSound whole;
    drum_rack_pad_sound(plain, 0, whole);
    CHECK(whole.start == 0 && whole.end == 0 && whole.level == 57 && whole.velocity_sense == 35 && whole.group == 0);
    CHECK(whole.filter == PadFilter::Off && whole.play == PadPlay::OneShot);
    plain.filter = "Lowpass 12dB"; plain.cutoff_hz = 3000;
    drum_rack_pad_sound(plain, 0, whole);
    CHECK(whole.filter == PadFilter::Lpf2);
    plain.filter = "Highpass"; plain.cutoff_hz = 30; // its lowest: open
    drum_rack_pad_sound(plain, 0, whole);
    CHECK(whole.filter == PadFilter::Off);
    plain.cutoff_hz = 200;
    drum_rack_pad_sound(plain, 0, whole);
    CHECK(whole.filter == PadFilter::Hpf4 && whole.cutoff == 33);
    plain.filter = "Peak"; // no pad filter to go to
    drum_rack_pad_sound(plain, 0, whole);
    CHECK(whole.filter == PadFilter::Off);
    plain.volume_db = -INFINITY; // a Gain of 0
    drum_rack_pad_sound(plain, 0, whole);
    CHECK(whole.level == 0);
    plain.volume_db = 6;
    drum_rack_pad_sound(plain, 0, whole);
    CHECK(whole.level == 127);
    // The rack as a kit: pads whose files are there, on their notes' pads, each file once; the rest left empty.
    namespace fs = std::filesystem;
    const auto folder = fs::temp_directory_path() / ("melodee-drum-rack-" + std::to_string(getpid()));
    fs::create_directories(folder);
    const auto kick_file = (folder / "Kick.wav").string(), snare_file = (folder / "Snare.wav").string();
    std::ofstream(kick_file) << "RIFF";
    std::ofstream(snare_file) << "RIFF";
    DrumRack rack;
    rack.name = "Test Kit";
    kick.uri = kick.file = kick_file;
    plain = DrumRackPad {}; plain.note = 37; plain.uri = plain.file = snare_file;
    DrumRackPad again = plain; again.note = 52; // the same file on A17's note: B01
    DrumRackPad none; none.note = 38;           // no sample
    DrumRackPad other; other.note = 39; other.uri = "ableton:/packs/another-pack/Samples/A.wav";
    DrumRackPad gone; gone.note = 40; gone.uri = gone.file = (folder / "Gone.wav").string();
    DrumRackPad low; low.note = 20; low.uri = low.file = kick_file; // below the kit
    rack.pads = {kick, plain, again, none, other, gone, low};
    auto project = std::make_unique<Project>();
    unsigned missing = 0, asked = 0;
    auto frames_of = [&](const std::string&) { ++asked; return uint32_t(88200); };
    CHECK(import_drum_rack(rack, *project, missing, frames_of).empty());
    const auto& track = project->tracks[0];
    CHECK(track.engine == EngineKind::Sampler && std::string(track.name) == "Test Kit" && std::string(project->name) == "Test Kit");
    CHECK(missing == 2 && asked == 1); // the other pack's and the one gone; only the kick plays part of its sample
    CHECK(track.pads[0].used() && track.pads[1].used() && track.pads[16].used());
    CHECK(!track.pads[2].used() && !track.pads[3].used() && !track.pads[4].used());
    CHECK(track.pads[1].sample == track.pads[16].sample && track.pads[0].sample != track.pads[1].sample);
    CHECK(std::string(project->samples[track.pads[0].sample].path) == kick_file && std::string(project->samples[track.pads[1].sample].name) == "Snare");
    CHECK(track.pads[0].start == 22050 && track.pads[0].end == 66150 && track.pads[0].group == 1);
    DrumRack empty;
    empty.pads = {none, other};
    CHECK(import_drum_rack(empty, *project, missing, frames_of) == "NO SAMPLES");
    fs::remove_all(folder);
    // Which files are presets.
    const auto preset = (folder.string() + ".json"), synth = (folder.string() + "-synth.json");
    std::ofstream(preset) << R"({"$schema": "http://tech.ableton.com/schema/song/1.8.3/devicePreset.json", "kind": "instrumentRack",)"
                             R"( "chains": [{"devices": [{"kind": "drumRack", "chains": []}]}]})";
    std::ofstream(synth) << R"({"$schema": "http://tech.ableton.com/schema/song/1.8.3/devicePreset.json", "kind": "instrumentRack",)"
                            R"( "chains": [{"devices": [{"kind": "drift"}]}]})";
    CHECK(is_drum_rack_preset(preset) && !is_drum_rack_preset(synth) && !is_drum_rack_preset(kick_file));
    fs::remove(preset);
    fs::remove(synth);
}

void solo_empty_track() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0);
    tone_kit(*project, 1);
    project->tracks[4].mix.solo = true; // track 5: empty, soloed in the file
    EngineHost host;
    host.load(*project, *bank);
    SampleData tone = tone_sample();
    host.offer_sample(0, &tone);
    std::vector<int32_t> out(256);
    auto peak = [&](unsigned blocks) {
        float most = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            host.render(out.data(), 128, 1);
            for (auto s : out) most = std::max(most, std::fabs(s / 2147483648.0f));
        }
        return most;
    };
    peak(2);
    host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note), 100});
    CHECK(peak(20) > 0.05f);
    host.send({Command::Type::Solo, 7, 0, 0, 1}); // track 8, empty, soloed now
    CHECK(peak(20) > 0.05f && host.meter(0) > 0.05f);
    host.send({Command::Type::Solo, 1, 0, 0, 1}); // kit 2 soloed: kit 1 is silenced, as before
    peak(4); // the solo eases in over a block
    CHECK(peak(20) < 1e-6f);
    host.send({Command::Type::Solo, 1, 0, 0, 0});
    CHECK(peak(20) > 0.05f);
    CHECK(host.dropped() == 0);
}

// The browser's preview through the host: an Audition at its pad.level, and AuditionLevel moving the one sounding now.
void audition_level_host() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0);
    EngineHost host;
    host.load(*project, *bank);
    SampleData tone = tone_sample();
    host.offer_sample(0, &tone);
    std::vector<int32_t> out(256);
    auto peak = [&](unsigned blocks) {
        float most = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            host.render(out.data(), 128, 1);
            most = 0; // the last block's: past the glide
            for (auto s : out) most = std::max(most, std::fabs(s / 2147483648.0f));
        }
        return most;
    };
    auto audition = [&](uint8_t level) {
        Command a {Command::Type::Audition, 0, 0, 0, 100};
        a.pad.sample = 0; a.pad.level = level;
        host.send(a);
    };
    peak(2);
    audition(100);
    const float full = peak(24);
    CHECK(full > 0.05f);
    host.send({Command::Type::AuditionLevel, 0, 0, 0, 0});
    CHECK(peak(24) < 1e-6f);
    host.send({Command::Type::AuditionLevel, 0, 0, 0, 100});
    CHECK(std::fabs(peak(24) / full - 1) < 0.05f);
    audition(0); // a new one at the level it carries
    CHECK(peak(24) < 1e-6f);
    CHECK(host.dropped() == 0);
}

void host_sequencer() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    synth(project->tracks[0], 0);
    project->tempo = 1200;
    put(bank->at[0][0], 4, 45, 100);
    EngineHost host;
    host.load(*project, *bank);
    Tones tones;
    tones.offer(host, *project);
    std::vector<int32_t> out(256);
    // Stopped: silent.
    host.render(out.data(), 128, 1);
    for (auto s : out) CHECK(s == 0);
    // Playing at 120 BPM a step is 5512.5 samples; step 5 starts at 22050.
    host.send({Command::Type::Play});
    std::vector<float> left;
    for (unsigned n = 0; n < 240; ++n) {
        host.render(out.data(), 128, 1);
        for (unsigned f = 0; f < 128; ++f) left.push_back(out[2 * f] / 2147483648.0f);
    }
    CHECK(host.playing());
    CHECK(peak_of(left, 0, 22000) == 0);
    CHECK(peak_of(left, 22050, 30000) > 0.005f);
    // Mute silences the track's mix but keeps the clock.
    host.send({Command::Type::Mute, 0, 0, 0, 1});
    host.send({Command::Type::Stop});
    for (unsigned n = 0; n < 50; ++n) host.render(out.data(), 128, 1);
    for (auto s : out) CHECK(s == 0);
    CHECK(!host.playing());
    // A step written while running lands in the mirror and plays.
    host.send({Command::Type::Mute, 0, 0, 0, 0});
    Command write {Command::Type::PatternData, 0, 0};
    write.data = new Pattern;
    put(*write.data, 0, 60, 120);
    host.send(write); // the copy it replaces goes back to be freed (here, by the host when it goes)
    host.send({Command::Type::Play});
    float peak = 0;
    for (unsigned n = 0; n < 40; ++n) {
        host.render(out.data(), 128, 1);
        for (auto s : out) peak = std::max(peak, std::fabs(s / 2147483648.0f));
    }
    CHECK(peak > 0.005f);
    CHECK(host.dropped() == 0);
}
// Slide, Tie and Accent as played: a tied note goes on into the next of its key, a slid one overlaps the next
// (a mono synth's legato) and glides there without a new attack, and an accented note comes out louder.
void sequenced_legato() {
    {   // MIDI out, which shows the notes as they go.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        project->tracks[0].params[midi_track::Port] = midi_track::UsbC; project->tracks[0].params[midi_track::Channel] = 1;
        project->tempo = 1200;
        auto& p = bank->at[0][0];
        put(p, 0, 60, 100, 1, step_tie); put(p, 1, 60, 100); put(p, 2, 64, 100, 1, step_slide); put(p, 3, 67, 100);
        EngineHost host;
        host.load(*project, *bank);
        host.set_audio_format(44100, 384);
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        std::vector<MidiOutEvent> notes;
        for (unsigned n = 0; n < 230; ++n) { // past step 4 (5512.5 frames a step)
            host.render(out.data(), 128, 1);
            MidiOutEvent e;
            while (host.take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOn || e.kind == MidiOutEvent::Kind::NoteOff) notes.push_back(e);
        }
        auto find = [&](MidiOutEvent::Kind kind, uint8_t key) {
            int count = 0; uint64_t at = 0;
            for (auto& e : notes) if (e.kind == kind && e.a == key) { if (!count) at = e.frame; ++count; }
            return std::pair<int, uint64_t>{count, at};
        };
        using K = MidiOutEvent::Kind;
        auto on60 = find(K::NoteOn, 60), off60 = find(K::NoteOff, 60), on64 = find(K::NoteOn, 64), off64 = find(K::NoteOff, 64), on67 = find(K::NoteOn, 67);
        CHECK(on60.first == 1 && off60.first == 1);              // tied onto the same key: one note
        CHECK(off60.second > on60.second + 5512 + 4000);          // held through step 2's note
        CHECK(on64.first == 1 && on67.first == 1 && off64.first == 1);
        CHECK(off64.second > on67.second);                        // the slide overlaps the next note
    }
    {   // A synth (a PLUGIN track): the accent is a louder velocity; a slide names the key the next note goes on from
        // (CC 84, portamento control) just before it, and a tie holds the legato pedal (CC 68) over that, as Melodee
        // Poly, PD and Chip take them.
        auto heard = [](uint8_t flags) {
            auto project = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            synth(project->tracks[0], 0);
            project->tempo = 1200;
            put(bank->at[0][0], 0, 57, 80, 1, flags); put(bank->at[0][0], 1, 64, 80);
            EngineHost host;
            host.load(*project, *bank);
            auto tones = std::make_unique<Tones>();
            tones->offer(host, *project);
            host.send({Command::Type::Play});
            std::vector<int32_t> out(256);
            for (unsigned n = 0; n < 80; ++n) host.render(out.data(), 128, 1);
            return tones->at[0].heard;
        };
        using Heard = std::vector<ToneRunner::Message>;
        auto velocity = [](const Heard& h) { for (auto& m : h) if ((m.status & 0xf0) == 0x90 && m.b) return int(m.b); return 0; };
        auto at = [](const Heard& h, uint8_t status, uint8_t a, int b = -1) {
            for (size_t n = 0; n < h.size(); ++n) if (h[n].status == status && h[n].a == a && (b < 0 || h[n].b == b)) return int(n);
            return -1;
        };
        auto plain = heard(0), accent = heard(step_accent), slide = heard(step_slide), tie = heard(step_tie);
        CHECK(velocity(plain) == 80 && velocity(accent) == 127); // 80 x 1.6, as far as MIDI goes
        CHECK(at(plain, 0xb0, 84) < 0 && at(plain, 0xb0, 68) < 0);
        int named = at(slide, 0xb0, 84, 57), next = at(slide, 0x90, 64), off = at(slide, 0x80, 57);
        CHECK(named >= 0 && next == named + 1 && off == next + 1 && at(slide, 0xb0, 68) < 0);
        int down = at(tie, 0xb0, 68, 127), tied = at(tie, 0xb0, 84, 57), on = at(tie, 0x90, 64), up = at(tie, 0xb0, 68, 0);
        CHECK(down >= 0 && tied == down + 1 && on == tied + 1 && up > on);
    }
}
// Issue 27, Settings' Mute Notes: a muted track or kit pad starts no new notes, and what it plays rings on to its
// own end (a MIDI track's note gets its Note Off when it was due, not at once); back to Audio, what still sounds goes.
void note_mutes() {
    using K = MidiOutEvent::Kind;
    // A MIDI track: C4 on step 1 held six steps, D4 on step 9; muted on step 3.
    auto midi = [](bool notes) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        project->tracks[0].params[midi_track::Port] = midi_track::UsbC; project->tracks[0].params[midi_track::Channel] = 1;
        project->tempo = 1200;
        put(bank->at[0][0], 0, 60, 100, 6); put(bank->at[0][0], 8, 62, 100, 4);
        EngineHost host;
        host.load(*project, *bank);
        host.set_audio_format(44100, 384);
        host.send({Command::Type::MuteMode, 0, 0, 0, int16_t(notes)});
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        std::vector<MidiOutEvent> events;
        for (unsigned n = 0; n < 500; ++n) {
            if (n == 90) host.send({Command::Type::Mute, 0, 0, 0, 1});
            if (n == 200) host.send({Command::Type::NoteOn, 0, 0, 70, 100}); // played by hand on the muted track
            host.render(out.data(), 128, 1);
            MidiOutEvent e;
            while (host.take_midi(e)) events.push_back(e);
        }
        return events;
    };
    auto find = [](const std::vector<MidiOutEvent>& events, K kind, uint8_t key) {
        int count = 0; uint64_t at = 0;
        for (auto& e : events) if (e.kind == kind && e.a == key) { if (!count) at = e.frame; ++count; }
        return std::pair<int, uint64_t> {count, at};
    };
    for (bool notes : {false, true}) {
        auto events = midi(notes);
        auto on60 = find(events, K::NoteOn, 60), off60 = find(events, K::NoteOff, 60);
        CHECK(on60.first == 1 && off60.first == 1);
        CHECK(find(events, K::NoteOn, 62).first == 0 && find(events, K::NoteOn, 70).first == 0); // nothing new either way
        if (notes) CHECK(off60.second > on60.second + 25000); // its own end, six steps on
        else CHECK(off60.second < on60.second + 14000);       // cut at the mute
    }

    // A kit: pad 1 on step 1 held six steps and again on step 9; the track or pad 1 muted on step 3. `back`: Audio again
    // on step 4.
    enum class What { Nothing, Track, Pad };
    auto kit = [](bool notes, What what, bool back = false) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        tone_kit(*project, 0);
        project->tempo = 1200;
        put(bank->at[0][0], 0, uint8_t(sampler_first_note), 100, 6); put(bank->at[0][0], 8, uint8_t(sampler_first_note), 100, 4);
        EngineHost host;
        host.load(*project, *bank);
        auto tone = tone_sample();
        CHECK(host.offer_sample(0, &tone));
        host.send({Command::Type::MuteMode, 0, 0, 0, int16_t(notes)});
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        std::vector<float> left;
        for (unsigned n = 0; n < 500; ++n) {
            if (n == 90 && what == What::Track) host.send({Command::Type::Mute, 0, 0, 0, 1});
            if (n == 90 && what == What::Pad) host.send({Command::Type::PadMute, 0, 0, 0, 1});
            if (n == 150 && back) host.send({Command::Type::MuteMode, 0, 0, 0, 0});
            host.render(out.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) left.push_back(out[2 * f] / 2147483648.0f);
        }
        return left;
    };
    auto open = kit(true, What::Nothing);
    CHECK(peak_of(open, 14000, 28000) > 0.05f && peak_of(open, 48000, 56000) > 0.05f); // the two notes, as heard unmuted
    for (auto what : {What::Track, What::Pad}) {
        auto cut = kit(false, what), rings = kit(true, what), back = kit(true, what, true);
        CHECK(peak_of(cut, 14000, 28000) < 1e-4f);                                       // Audio: silent at once
        CHECK(peak_of(rings, 14000, 28000) > 0.05f && peak_of(rings, 48000, 60000) < 1e-4f); // Notes: rings on, nothing new
        CHECK(peak_of(back, 21000, 28000) < 1e-4f);                                      // Audio again: it goes
    }
}

// Issue 32: a kit pad's mute recorded as a lane: it mutes and unmutes the pad where its points are, the pattern's
// start and Stop put the mute back as set (Mute + pad), and pad gaps and lane_own() know it as a lane on a pad.
void pad_mute_lane() {
    CHECK(is_pad_lane(motion_pad_mute) && is_pad_lane(motion_pad_param(PadParam::Level)) && !is_pad_lane(motion_level));
    CHECK(pad_param_of(motion_pad_mute) < 0 && !is_mix_lane(motion_pad_mute));
    auto run = [](unsigned blocks, bool stop_and_hit) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        tone_kit(*project, 0);
        project->tempo = 1200;
        auto& p = bank->at[0][0];
        for (unsigned step : {0u, 4u, 8u, 12u}) put(p, step, uint8_t(sampler_first_note), 100, 2);
        p.write_motion(point(4, motion_pad_mute, 1)); p.write_motion(point(8, motion_pad_mute, 0)); p.write_motion(point(12, motion_pad_mute, 1));
        EngineHost host;
        host.load(*project, *bank);
        auto tone = tone_sample();
        CHECK(host.offer_sample(0, &tone));
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        std::vector<float> left;
        auto render = [&](unsigned n) {
            for (unsigned b = 0; b < n; ++b) {
                host.render(out.data(), 128, 1);
                for (unsigned f = 0; f < 128; ++f) left.push_back(out[2 * f] / 2147483648.0f);
            }
        };
        render(blocks);
        if (stop_and_hit) {
            host.send({Command::Type::Stop});
            render(20);
            left.clear();
            host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note), 100}); // the pad as set: not muted
            render(40);
        }
        return left;
    };
    auto pass = run(800, false); // two passes and a bit (88200 frames a pass)
    CHECK(peak_of(pass, 2000, 20000) > 0.05f);  // step 1: heard
    CHECK(peak_of(pass, 24000, 43000) < 1e-4f); // steps 5-8: muted by the lane, its note too
    CHECK(peak_of(pass, 45500, 52000) > 0.05f); // step 9: unmuted
    CHECK(peak_of(pass, 68000, 87500) < 1e-4f); // steps 13-16: muted again
    CHECK(peak_of(pass, 89500, 96000) > 0.05f); // the next pass starts as set: heard
    auto stopped = run(560, true); // stopped on step 13, muted by the lane
    CHECK(peak_of(stopped) > 0.05f);

    // A pad gap closed under the lane moves it with its pad (its pad gone: the lane too); lane_own() reads the pad's
    // mute as set.
    Project project;
    tone_kit(project, 0);
    project.tracks[0].pads[1].sample = no_sample; // A02 empty: A03 and A04 move down one
    auto patterns = std::make_unique<std::array<Pattern, pattern_count>>();
    (*patterns)[0].write_motion(point(0, motion_pad_mute, 1, 2)); (*patterns)[0].write_motion(point(2, motion_pad_mute, 1, 1));
    close_pad_gap(project.tracks[0], *patterns, 1);
    CHECK((*patterns)[0].motion.size() == 1 && (*patterns)[0].motion[0].pad == 1 && (*patterns)[0].motion[0].param == motion_pad_mute);
    project.tracks[0].set_pad_muted(2, true);
    CHECK(lane_own(project, 0, motion_pad_mute, 2) == 1 && lane_own(project, 0, motion_pad_mute, 1) == 0);
}

void motion() {
    // The store (a pattern's own since roadmap P1): a second write at the same place replaces, clear drops one lane,
    // and it fills at the limit.
    Pattern m;
    constexpr unsigned a = sampler::Filter, b = sampler::Resonance; // two lanes of engine values
    CHECK(m.write_motion(point(4, a, 10)) && m.write_motion(point(4, a, 20)) && m.motion.size() == 1);
    CHECK(m.motion[0].value == 20);
    CHECK(m.write_motion(point(8, b, 5)) && m.write_motion(point(2, a, 5)));
    CHECK(m.motion[0].tick == 2 * sixteenth_ticks && m.motion[2].param == b); // in order of ticks
    CHECK(m.clear_lane(a, 0) && m.motion.size() == 1 && !m.has_lane(a, 0) && m.has_lane(b, 0));
    Pattern full;
    for (unsigned n = 0; n < motion_points; ++n) { MotionPoint p; p.tick = n; CHECK(full.write_motion(p)); }
    MotionPoint over; over.tick = motion_points;
    CHECK(!full.write_motion(over) && full.motion.size() == motion_points);

    // Played: the kit's volume brought down by motion from step 0 makes a line of hits much quieter.
    auto level = [](bool closed) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        tone_kit(*project, 0);
        for (unsigned s = 0; s < 16; ++s) put(bank->at[0][0], s, uint8_t(sampler_first_note), 100);
        if (closed) bank->at[0][0].write_motion(point(0, sampler::Level, 0));
        EngineHost host;
        host.load(*project, *bank);
        auto tone = tone_sample();
        CHECK(host.offer_sample(0, &tone));
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        double energy = 0;
        for (unsigned n = 0; n < 200; ++n) {
            host.render(out.data(), 128, 1);
            for (auto v : out) energy += double(v / 2147483648.0f) * (v / 2147483648.0f);
        }
        return energy;
    };
    CHECK(level(true) * 20 < level(false));
}
// Freeze plan F3: an audio track's take lives in its pattern (a pattern with one is not empty) and goes through a
// file with the track's settings; a take on a track that is not an audio track is dropped.
void audio_takes() {
    AudioRegion take;
    take.take = 3; take.tempo = 1200;
    take.frames = take.take_frames = uint32_t(ticks_to_frames(2 * 4 * ticks_per_quarter, 1200)); // two bars at 120 BPM
    CHECK(take.frames == 176400 && frames_to_ticks(take.frames, 1200) == 2 * 4 * ticks_per_quarter);
    Pattern pattern;
    CHECK(pattern.empty());
    pattern.audio = take;
    CHECK(!pattern.empty() && pattern != Pattern {});
    CHECK(audio_take_name(7) == "Take 007.wav" && audio_take_of("Take 007.wav") == 7 && audio_take_of("Take 7.wav.part") == 0);
    CHECK(audio_folder("/card/Melodee/Projects/Song.mlp") == "/card/Melodee/Projects/Song Audio");

    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    project->tracks[3].engine = EngineKind::Audio; project->tracks[3].set_name("VOX");
    project->tracks[3].audio.source = InputSource::Usb; project->tracks[3].audio.monitor = TrackMonitor::Off;
    take.gain = -35; take.fade_in = 441; take.offset = 100; take.frames -= 100; take.reverse = true;
    bank->at[3][5].audio = take;
    bank->at[3][5].length = 2 * 4 * ticks_per_quarter;
    bank->at[6][0].audio = take; // a kit track's: dropped
    auto bytes = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(bytes, *back, *back_bank).empty());
    const auto& t = back->tracks[3];
    CHECK(t.engine == EngineKind::Audio && std::string(t.name) == "VOX" && t.audio.source == InputSource::Usb && t.audio.monitor == TrackMonitor::Off);
    CHECK(back_bank->at[3][5].audio == take && back_bank->at[3][5].length == 2 * 4 * ticks_per_quarter && !back_bank->at[3][4].audio.used());
    CHECK(!back_bank->at[6][0].audio.used());
}

void arrange() {
    // The song store: placing a clip cuts what it covers; splits keep both sides.
    Song song;
    CHECK(song.place({0, 0, 0, 8, false}) && song.place({0, 1, 2, 2, false}));
    CHECK(song.count == 3 && song.clip_at(0, 1) >= 0 && song.clips[size_t(song.clip_at(0, 3))].pattern == 1);
    CHECK(song.clips[size_t(song.clip_at(0, 5))].pattern == 0 && song.clips[size_t(song.clip_at(0, 5))].bar == 4);
    CHECK(song.clip_at(1, 0) < 0 && song.length() == 8);
    song.erase(unsigned(song.clip_at(0, 3)));
    CHECK(song.clip_at(0, 3) < 0 && song.count == 2);
    // Markers: added at a bar, named after the one before, stepped through the names, then gone.
    CHECK(song.step_marker(4) && song.marker_count == 1 && std::string(song.markers[0].name) == "INTRO");
    CHECK(song.step_marker(12) && std::string(song.markers[size_t(song.marker_at(12))].name) == "VERSE");
    CHECK(song.step_marker(4) && std::string(song.markers[size_t(song.marker_at(4))].name) == "VERSE");
    for (unsigned n = 0; n < Song::marker_name_count - 2; ++n) CHECK(song.step_marker(4));
    CHECK(std::string(song.markers[size_t(song.marker_at(4))].name) == "OUTRO");
    CHECK(song.step_marker(4) && song.marker_at(4) < 0 && song.marker_count == 1 && song.marker_at(12) >= 0);

    auto setup = [](Project& project, PatternBank& bank) {
        synth(project.tracks[0], 0);
        project.tempo = 1200;
        put(bank.at[0][0], 0, 57, 100); // pattern 1: a note on its first step only
        put(bank.at[0][1], 0, 69, 100);
    };
    // One bar at 120 BPM is 88200 samples; render `bars` and say in which bars track 1 sounded.
    auto sounding = [](EngineHost& host, unsigned bars) {
        std::vector<bool> out;
        std::vector<int32_t> block(256);
        for (unsigned b = 0; b < bars; ++b) {
            float peak = 0;
            for (unsigned n = 0; n < 88200 / 128; ++n) {
                host.render(block.data(), 128, 1);
                for (auto v : block) peak = std::max(peak, std::fabs(v / 2147483648.0f));
            }
            out.push_back(peak > 0.002f);
        }
        return out;
    };
    // The song: track 1 has a clip in bar 2 only.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*project, *bank);
        project->song.place({0, 0, 1, 1, false});
        // Each host here on the heap: an EngineHost is 3.4 MB, three on this frame overflow an 8 MB stack.
        auto kept = std::make_unique<EngineHost>();
        EngineHost& host = *kept;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.send({Command::Type::Play, 0, 0, 0, 1});
        auto bars = sounding(host, 3);
        CHECK(!bars[0] && bars[1]);
        CHECK(!host.playing()); // past the song's end
    }
    // The song's loop inside a longer clip (the loop set to another track's clip): when it comes round, the pattern
    // is where the song is. A four-bar pattern with a note on its bar 3 alone, the loop bars 3 and 4: the note
    // again on each round (the pattern played on from its bar 1, out of step with the song).
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        project->tempo = 1200;
        auto& pattern = bank->at[0][0];
        pattern.length = 4 * project->bar_ticks();
        Event note;
        note.tick = 2 * project->bar_ticks(); note.key = 60; note.length = sixteenth_ticks;
        pattern.add(note);
        project->song.place({0, 0, 0, 8, false});
        project->song.loop_start = 2; project->song.loop_end = 4;
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        Tones tones;
        tones.offer(*host, *project);
        host->send({Command::Type::Play, 0, 0, 2, 1}); // from bar 3
        std::vector<int32_t> block(256);
        for (unsigned n = 0; n < 100; ++n) host->render(block.data(), 128, 1);
        CHECK(tones.at[0].count(0x90, 60) == 1);
        for (unsigned n = 0; n < 1400; ++n) host->render(block.data(), 128, 1); // 192000 frames: the loop came round at 176400
        CHECK(host->song_bar() == 2 && tones.at[0].count(0x90, 60) == 2 && host->position_of(0) >= 2 * int(project->bar_ticks()) && host->position_of(0) < 3 * int(project->bar_ticks()));
        for (unsigned n = 0; n < 1400; ++n) host->render(block.data(), 128, 1); // and again
        CHECK(tones.at[0].count(0x90, 60) == 3);
        host->send({Command::Type::Stop});
        host->render(block.data(), 128, 1);
        CHECK(host->offer_plugin(0, nullptr));
        host->render(block.data(), 128, 1);
        // Issue 60: played from past the loop's end, the song runs to its end and stops (the loop never comes round).
        host->send({Command::Type::Play, 0, 0, 6, 1}); // from bar 7 of 8
        for (unsigned n = 0; n < 2000; ++n) host->render(block.data(), 128, 1); // past bar 8's end
        CHECK(!host->playing());
    }
    // A clip changed for another pattern in its place while it plays: the new pattern from the next bar, where the
    // clip has it by then (the old one played on to the clip's end).
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*project, *bank);
        bank->at[0][1].length = 2 * project->bar_ticks();
        project->song.place({0, 0, 0, 8, false});
        auto kept = std::make_unique<EngineHost>();
        EngineHost& host = *kept;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.send({Command::Type::Play, 0, 0, 0, 1});
        std::vector<int32_t> block(256);
        for (unsigned n = 0; n < 100; ++n) host.render(block.data(), 128, 1);
        CHECK(host.pattern_of(0) == 0);
        Command change {Command::Type::SongClip, 0, 0, 0};
        change.clip = Clip {0, 1, 0, 8, false};
        host.send(change);
        for (unsigned n = 0; n < 700; ++n) host.render(block.data(), 128, 1); // 102400 frames: bar 2, tick 618 of it
        CHECK(host.song_bar() == 1 && host.pattern_of(0) == 1 && host.position_of(0) >= 3840 + 610 && host.position_of(0) <= 3840 + 625);
    }
    // Patterns: a launch waits for the next bar; the position counts ticks (960 a quarter, 22.97 samples each at 120 BPM).
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        setup(*project, *bank);
        auto kept = std::make_unique<EngineHost>();
        EngineHost& host = *kept;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.send({Command::Type::Play});
        std::vector<int32_t> block(256);
        for (unsigned n = 0; n < 80; ++n) host.render(block.data(), 128, 1); // 10240 samples: tick 445
        CHECK(host.position_of(0) == 445 && host.pattern_of(0) == 0);
        for (unsigned n = 0; n < 10; ++n) host.render(block.data(), 128, 1); // 11520: tick 501
        CHECK(host.position_of(0) == 501);
        host.send({Command::Type::Pattern, 0, 0, 0, 1});
        host.render(block.data(), 128, 1);
        CHECK(host.pattern_of(0) == 0); // still the old one until the bar ends
        for (unsigned n = 0; n < 88200 / 128; ++n) host.render(block.data(), 128, 1);
        CHECK(host.pattern_of(0) == 1);
    }
}
void effects() {
    // Pad FX: a load sets each pad's chain and takes the last project's away, a pad that had one set by its project
    // or by a command alike (the host keeps which pads have a kind, so a load passes the others unread).
    {
        auto host = std::make_unique<EngineHost>();
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        project->tracks[2].pad_fx.at[5].slots[1] = EffectSlot(uint8_t(EffectKind::Tremolo));
        host->load(*project, *patterns);
        CHECK(host->pad_fx_kind(2, 5, 1) == EffectKind::Tremolo);
        std::vector<int32_t> out(256);
        host->send({Command::Type::PadFxKind, 7, 100, 2, int16_t(EffectKind::AutoPan)}); // bank G's pad 4
        host->render(out.data(), 128, 1);
        CHECK(host->pad_fx_kind(7, 100, 2) == EffectKind::AutoPan);
        auto empty = std::make_unique<Project>();
        host->load(*empty, *patterns);
        CHECK(host->pad_fx_kind(2, 5, 1) == EffectKind::None && host->pad_fx_kind(7, 100, 2) == EffectKind::None);
    }
    // Every built effect stays finite and bounded on a loud chord, at its initial values and at the extremes.
    DelayPool pool;
    for (unsigned k = 1; k < unsigned(EffectKind::Count); ++k) {
        auto kind = EffectKind(k);
        if (!effect_info(kind).built) continue;
        for (int extreme = 0; extreme < 3; ++extreme) {
            Effect effect;
            effect.set_kind(kind, pool);
            if (extreme)
                for (unsigned q = 0; q < effect_params; ++q) {
                    const auto& pi = effect_info(kind).params[q];
                    if (pi.label) effect.set_param(q, extreme == 1 ? pi.min : pi.max);
                }
            std::vector<float> l(128), r(128);
            float peak = 0;
            for (unsigned b = 0; b < 400; ++b) {
                for (unsigned f = 0; f < 128; ++f) {
                    float t = float(b * 128 + f) / 44100.0f;
                    l[f] = 0.8f * std::sin(6.283f * 220 * t) + 0.2f * std::sin(6.283f * 331 * t);
                    r[f] = 0.8f * std::sin(6.283f * 277 * t);
                }
                effect.process(l.data(), r.data(), 128);
                for (unsigned f = 0; f < 128; ++f) { CHECK(std::isfinite(l[f]) && std::isfinite(r[f])); peak = std::max({peak, std::fabs(l[f]), std::fabs(r[f])}); }
            }
            CHECK(peak < 20); // a resonant filter or makeup gain may rise, but nothing runs away
            effect.set_kind(EffectKind::None, pool); // gives its delay memory back
        }
    }
    // Looper: what is recorded on one pass plays on the next with the input gone.
    {
        Effect looper;
        looper.set_kind(EffectKind::Looper, pool);
        looper.set_param(1, 1); // record
        unsigned length = unsigned(time_seconds(70) * 44100);
        std::vector<float> l(1), r(1);
        for (unsigned n = 0; n < length; ++n) { l[0] = r[0] = n < 2000 ? 0.5f : 0; looper.process(l.data(), r.data(), 1); }
        looper.set_param(1, 0);
        float heard = 0;
        for (unsigned n = 0; n < 2000; ++n) { l[0] = r[0] = 0; looper.process(l.data(), r.data(), 1); heard = std::max(heard, std::fabs(l[0])); }
        CHECK(heard > 0.4f);
        looper.set_kind(EffectKind::None, pool);
    }
    // Formant: an 800 Hz tone sits on A's first formant and between I's (280 and 2200 Hz).
    {
        auto level = [&](int vowel) {
            Effect formant;
            formant.set_kind(EffectKind::Formant, pool);
            formant.set_param(0, vowel);
            std::vector<float> l(128), r(128);
            float peak = 0;
            for (unsigned b = 0; b < 100; ++b) {
                for (unsigned f = 0; f < 128; ++f) l[f] = r[f] = 0.3f * std::sin(6.2831853f * 800 * float(b * 128 + f) / 44100.0f);
                formant.process(l.data(), r.data(), 128);
                if (b > 50) for (unsigned f = 0; f < 128; ++f) peak = std::max(peak, std::fabs(l[f]));
            }
            return peak;
        };
        CHECK(level(0) > level(50) * 2);
    }
    // When the pool is used up, a further delay effect passes sound through untouched.
    std::vector<Effect> many(DelayPool::blocks + 1);
    for (auto& e : many) e.set_kind(EffectKind::Echo, pool);
    std::vector<float> l(128, 0.5f), r(128, 0.5f);
    many.back().process(l.data(), r.data(), 128);
    CHECK(l[0] == 0.5f && r[127] == 0.5f);

    // Along the signal: a track's low-pass quietens it; its send into an echo bus keeps sounding after the note.
    auto render = [](bool filter, bool echo) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        if (filter) { project->tracks[0].fx.slots[0].kind = uint8_t(EffectKind::Filter); project->tracks[0].fx.slots[0].params = {5, 0, 0, 0, 0, 0}; }
        if (echo) {
            project->tracks[0].mix.sends[0] = 100;
            project->buses[0].fx.slots[0].kind = uint8_t(EffectKind::Echo);
            project->buses[0].fx.slots[0].params = {60, 50, 100, 80, 0, 0};
        }
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.send({Command::Type::NoteOn, 0, 0, 57, 110});
        std::vector<int32_t> out(256);
        double held = 0, after = 0;
        for (unsigned n = 0; n < 700; ++n) {
            if (n == 172) host.send({Command::Type::NoteOff, 0, 0, 57, 0}); // half a second in
            host.render(out.data(), 128, 1);
            for (auto v : out) (n < 172 ? held : n > 200 ? after : held) += double(v / 2147483648.0f) * (v / 2147483648.0f);
        }
        return std::pair<double, double>(held, after);
    };
    auto dry = render(false, false), filtered = render(true, false), echoed = render(false, true);
    CHECK(filtered.first * 10 < dry.first);
    CHECK(dry.second < 1e-6 && echoed.second > 1e-3);
}
void put32(std::ofstream& f, uint32_t v) { for (int n = 0; n < 4; ++n) f.put(char(v >> (8 * n))); }
void put16(std::ofstream& f, uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); }

// The browser's preview read a piece at a time: WavReader's pieces filled into a SampleFill make read_wav()'s sample to
// the bit, at every file rate (its own frames, never converted), published as they land; a fill stopped short is cut
// there; and a voice on a sample still being read plays what has landed, waits silent at its edge, goes on when more
// lands and ends where a cut one stops.
void preview_progressive() {
    const std::string path = "/tmp/melodee-progressive-" + std::to_string(::getpid()) + ".wav";
    auto write = [&](unsigned rate, unsigned channels, unsigned bits, unsigned frames) {
        std::ofstream f(path, std::ios::binary);
        const unsigned block = channels * bits / 8;
        f.write("RIFF", 4); put32(f, 36 + frames * block); f.write("WAVEfmt ", 8); put32(f, 16); put16(f, 1); put16(f, uint16_t(channels));
        put32(f, rate); put32(f, rate * block); put16(f, uint16_t(block)); put16(f, uint16_t(bits)); f.write("data", 4); put32(f, frames * block);
        uint32_t noise = 12345;
        for (unsigned n = 0; n < frames; ++n)
            for (unsigned c = 0; c < channels; ++c) {
                noise = noise * 1664525u + 1013904223u;
                double v = 0.6 * std::sin(n * (c ? 0.031 : 0.017)) + 0.2 * (int32_t(noise) / 2147483648.0);
                if (bits == 16) put16(f, uint16_t(int16_t(v * 32767)));
                else { int32_t s = int32_t(v * 8388607); f.put(char(s)); f.put(char(s >> 8)); f.put(char(s >> 16)); }
            }
    };
    struct Kind { unsigned rate, channels, bits; };
    for (Kind k : {Kind {22050, 2, 16}, Kind {44100, 2, 16}, Kind {48000, 1, 24}, Kind {96000, 2, 24}}) {
        write(k.rate, k.channels, k.bits, k.rate * 13 / 10);
        SampleData whole;
        CHECK(read_wav(path, whole, 60, nullptr).empty());
        CHECK(whole.rate == k.rate && whole.frames == k.rate * 13 / 10 && whole.left.size() == whole.frames); // its own frames
        CHECK(whole.model_frames() == uint32_t(std::llround(whole.frames * 44100.0 / k.rate)));
        WavReader reader;
        CHECK(reader.open(path, 60).empty() && reader.rate() == k.rate && reader.frames() == k.rate * 13 / 10);
        SampleData grown;
        SampleFill fill;
        fill.begin(grown, reader.frames(), reader.rate());
        CHECK(grown.growth && grown.frames == whole.frames && grown.rate == whole.rate && grown.left.size() == whole.left.size());
        std::vector<float> l(777), r(777);
        uint32_t landed = 0, got;
        while ((got = reader.read(l.data(), r.data(), 777))) {
            fill.add(l.data(), r.data(), got);
            uint32_t now = grown.growth->frames.load();
            CHECK(now == landed + got && now <= grown.frames); // as it comes: nothing held back to convert
            landed = now;
        }
        CHECK(!fill.to_come() && fill.filling() && !grown.growth->whole.load());
        fill.finish();
        CHECK(!fill.filling() && grown.growth->whole.load() && !grown.growth->cut.load());
        CHECK(grown.growth->frames.load() == grown.frames);
        CHECK(grown.left == whole.left && grown.right == whole.right);
    }
    { // Stopped after a piece: cut where it got to, the rest silent.
        write(48000, 2, 16, 48000);
        WavReader reader;
        CHECK(reader.open(path, 60).empty());
        SampleData grown;
        SampleFill fill;
        fill.begin(grown, reader.frames(), reader.rate());
        CHECK(grown.frames == 48000 && grown.rate == 48000);
        std::vector<float> l(6000), r(6000);
        fill.add(l.data(), r.data(), reader.read(l.data(), r.data(), 6000));
        fill.finish();
        const uint32_t landed = grown.growth->frames.load();
        CHECK(grown.growth->cut.load() && !grown.growth->whole.load() && landed == 6000);
        CHECK(std::all_of(grown.left.begin() + landed, grown.left.end(), [](float v) { return v == 0.0f; }));
        CHECK(reader.open(path, 0) == "no audio" && reader.open(path + ".none", 60) == "cannot open");
        WavReader second;
        CHECK(second.open(path, 1).empty() && second.frames() == 48000); // at most a second of it
    }
    std::remove(path.c_str());

    // A voice on it: the audition plays what has landed, waits silent at its edge, goes on when the rest lands.
    dsp::device_rate.store(44100.0f);
    SampleData data;
    data.frames = 44100; data.left.assign(44100, 0.5f); data.right.assign(44100, 0.5f);
    data.growth = std::make_shared<SampleGrowth>();
    data.growth->frames.store(4410);
    SamplerEngine::Table table {};
    table[preview_slot] = &data;
    auto loudest = [](SamplerEngine& e) {
        std::vector<float> l(128, 0.0f), r(128, 0.0f);
        e.render_stereo(l.data(), r.data(), 128);
        float most = 0;
        for (float v : l) most = std::max(most, std::fabs(v));
        return most;
    };
    {
        SamplerEngine engine;
        engine.set_samples(&table);
        engine.audition(uint16_t(preview_slot), 0, 0, 100);
        float first = 0;
        for (unsigned b = 0; b < 8; ++b) first = std::max(first, loudest(engine));
        CHECK(first > 0.01f);
        for (unsigned b = 0; b < 30; ++b) loudest(engine); // up to the edge: 4410 frames is 34 blocks
        CHECK(engine.sounding() && loudest(engine) == 0.0f && loudest(engine) == 0.0f); // waiting, not ended
        data.growth->frames.store(44100);
        data.growth->whole.store(true);
        float again = 0;
        for (unsigned b = 0; b < 4; ++b) again = std::max(again, loudest(engine));
        CHECK(again > 0.01f);
        engine.audition(no_sample, 0, 0, 0);
        for (unsigned b = 0; b < 40; ++b) loudest(engine);
        CHECK(!engine.sounding());
    }
    { // Cut short: it plays to there and ends.
        data.growth = std::make_shared<SampleGrowth>();
        data.growth->frames.store(4410);
        data.growth->cut.store(true);
        SamplerEngine engine;
        engine.set_samples(&table);
        engine.audition(uint16_t(preview_slot), 0, 0, 100);
        CHECK(loudest(engine) > 0.01f);
        for (unsigned b = 0; b < 40; ++b) loudest(engine);
        CHECK(!engine.sounding());
    }
    { // The browser's preview level (set_accent before the audition): 70 is that much quieter than 100, and
      // audition_level() glides the one sounding now there, to silence and back, without starting it again.
        SampleData flat;
        flat.frames = 44100; flat.left.assign(44100, 0.5f); flat.right.assign(44100, 0.5f);
        SamplerEngine::Table flat_table {};
        flat_table[preview_slot] = &flat;
        auto settled = [&](SamplerEngine& e) { float at = 0; for (unsigned b = 0; b < 24; ++b) at = loudest(e); return at; }; // past the 48 ms glide
        auto heard_at = [&](float accent) {
            SamplerEngine engine;
            engine.set_samples(&flat_table);
            engine.set_accent(accent);
            engine.audition(uint16_t(preview_slot), 0, 0, 100);
            engine.set_accent(1);
            return settled(engine);
        };
        const float full = heard_at(1), lower = heard_at(volume_gain(70));
        CHECK(full > 0.01f && std::fabs(lower / full - volume_gain(70)) < 0.01f);
        SamplerEngine engine;
        engine.set_samples(&flat_table);
        engine.audition(uint16_t(preview_slot), 0, 0, 100);
        settled(engine);
        engine.audition_level(volume_gain(70));
        CHECK(std::fabs(settled(engine) / full - volume_gain(70)) < 0.01f);
        engine.audition_level(0);
        CHECK(settled(engine) < 1e-5f && engine.sounding()); // silent, still playing
        engine.audition_level(1);
        CHECK(std::fabs(settled(engine) / full - 1) < 0.01f);
    }
}
void recording() {
    { // A free take ends at 60 s in the buffer; drained to the card as it comes (spill()), it goes on past that, the
      // buffer a ring, every frame drained as it came; drained no further, it ends where the ring is full.
        const float was = dsp::device_rate.load();
        dsp::device_rate.store(44100);
        auto take = std::make_unique<TakeRecorder>();
        std::vector<float> l(128), r(128);
        auto feed = [&](uint32_t from) { for (unsigned f = 0; f < 128; ++f) { l[f] = float((from + f) % 100000) / 100000.0f; r[f] = -l[f]; } };
        take->arm(-100, 0, 3);
        uint32_t sent = 0;
        while (take->state() == TakeRecorder::State::Recording) { feed(sent); take->process(l.data(), r.data(), 128); sent += 128; }
        CHECK(take->taken() == 60 * 44100 && take->length() == 60 * 44100); // not drained: the buffer's 60 s
        take->release();
        take->arm(-100, 0, 3);
        take->spill(true);
        sent = 0;
        uint32_t drained = 0;
        bool same = true;
        while (sent < 70 * 44100) {
            feed(sent);
            take->process(l.data(), r.data(), 128);
            sent += 128;
            for (; drained < take->taken(); ++drained) { // as SampleLibrary::take_service drains it
                const uint32_t at = drained % TakeRecorder::capacity;
                same = same && take->left()[at] == float(drained % 100000) / 100000.0f && take->right()[at] == -take->left()[at];
            }
            take->drained(drained);
        }
        CHECK(take->state() == TakeRecorder::State::Recording && take->taken() == sent && sent > 70 * 44100 - 128 && same && take->length() == 60 * 44100);
        take->stop();
        CHECK(take->state() == TakeRecorder::State::Done && take->frames() == sent);
        take->release();
        take->arm(-100, 0, 3);
        take->spill(true); // drained no further: as far as the ring goes
        sent = 0;
        while (take->state() == TakeRecorder::State::Recording && sent < 200 * 44100) { feed(sent); take->process(l.data(), r.data(), 128); sent += 128; }
        CHECK(take->state() == TakeRecorder::State::Done && take->taken() == TakeRecorder::capacity);
        take->release();
        dsp::device_rate.store(was);
    }
    // A take waits for the level, records its length, and is held until released.
    auto recorder = std::make_unique<TakeRecorder>();
    std::vector<float> quiet(128, 0.001f), loud(128, 0.5f);
    recorder->arm(-30, 1000, 7);
    recorder->process(quiet.data(), quiet.data(), 128);
    CHECK(recorder->state() == TakeRecorder::State::Armed && recorder->frames() == 0);
    for (unsigned n = 0; n < 10; ++n) recorder->process(loud.data(), loud.data(), 128);
    CHECK(recorder->state() == TakeRecorder::State::Done && recorder->frames() == 1000 && recorder->slot() == 7);
    CHECK(std::fabs(recorder->left()[999] - 0.5f) < 1e-6f && recorder->peak(127) > 100);
    recorder->arm(-30, 1000, 1); // held: a new take waits for release
    CHECK(recorder->frames() == 1000);
    recorder->release();
    CHECK(recorder->state() == TakeRecorder::State::Idle);
    // At once (-96 dB), stopped early (the engine host applies the gain before it).
    recorder->arm(-96, 0, 2);
    CHECK(recorder->state() == TakeRecorder::State::Recording);
    recorder->process(quiet.data(), quiet.data(), 128);
    recorder->stop();
    CHECK(recorder->state() == TakeRecorder::State::Done && recorder->frames() == 128 && std::fabs(recorder->left()[0] - 0.001f) < 1e-6f);
    recorder->release();
    // The host records the mix (resample) for a bar at 120 BPM.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    synth(project->tracks[0], 0);
    EngineHost host;
    host.load(*project, *bank);
    tones_for(host, *project);
    host.send({Command::Type::RecordArm, uint8_t(InputSource::Resample), 1, 5, -96});
    host.send({Command::Type::NoteOn, 0, 0, 57, 100});
    std::vector<int32_t> out(256);
    for (unsigned n = 0; n < 700; ++n) host.render(out.data(), 128, 1, nullptr);
    CHECK(host.recorder().state() == TakeRecorder::State::Done && host.recorder().frames() == 88200);
    float peak = 0;
    for (uint32_t f = 0; f < 88200; ++f) peak = std::max(peak, std::fabs(host.recorder().left()[f]));
    CHECK(peak > 0.01f);
}
void project_files() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    std::snprintf(project->name, sizeof(project->name), "Project 1");
    project->tempo = 1245; project->swing = 58; project->root = 2; project->scale = 2;
    load_sound(project->tracks[0], EngineKind::Midi, 0);
    project->tracks[0].params[midi_track::Cc1] = 33;
    project->tracks[0].mix = {90, -20, 40, 2, true, false, {0, 0, 40, 0}};
    project->tracks[0].mods[3] = {1, 2, -45};
    project->tracks[0].fx.slots[0] = {uint8_t(EffectKind::Echo), true, {60, 50, 30, 70, 0, 0}};
    load_sound(project->tracks[4], EngineKind::Sampler, 0);
    project->tracks[4].pads[9].sample = 2; project->tracks[4].pads[9].start = 1234; project->tracks[4].pads[9].group = 3;
    std::snprintf(project->samples.edit(2).path, sizeof(SampleRef::path), "/media/card/Samples/Kick.wav");
    bank->at[0][3].length = 96 * 480; bank->at[0][3].division = Division::Eighth;
    bank->at[0][3].add({95 * 480 + 5, 40, 64, 77, 100, step_tie}); // five ticks late, Rec Quantize off
    bank->at[0][3].write_motion(point(7, midi_track::Cc2, 61));
    project->song.place({0, 3, 8, 4, true});
    project->song.step_marker(8);
    project->song.loop_start = 8; project->song.loop_end = 12;
    project->buses[1].level = 33; project->buses[1].fx.slots[0].kind = uint8_t(EffectKind::Room);
    project->master.slots[0].kind = uint8_t(EffectKind::Limiter);
    project->tracks[0].chain = {2, 0, 5}; project->tracks[0].chain_count = 3; project->tracks[0].chain_at = 1; // seq-sheet2 B (CHAN)
    project->tracks[1].chain = {4}; project->tracks[1].chain_count = 1; // one pattern: no chain, nothing written

    auto data = save_project(*project, *bank);
    auto copy = std::make_unique<Project>();
    auto copy_bank = std::make_unique<PatternBank>();
    CHECK(load_project(data, *copy, *copy_bank).empty());
    CHECK(std::string(copy->name) == "Project 1" && copy->tempo == 1245 && copy->swing == 58 && copy->scale == 2);
    CHECK(copy->tracks[0].engine == EngineKind::Midi && copy->tracks[0].params[midi_track::Cc1] == 33);
    CHECK(copy->tracks[0].mix.pan == -20 && copy->tracks[0].mix.sends[2] == 40 && copy->tracks[0].mix.sends[0] == 0 && copy->tracks[0].mix.mute);
    CHECK(copy->tracks[0].mods[3].amount == -45 && copy->tracks[0].fx.slots[0].bypass && copy->tracks[0].fx.slots[0].params[3] == 70);
    CHECK(copy->tracks[4].engine == EngineKind::Sampler && copy->tracks[4].pads[9].start == 1234 && copy->tracks[4].pads[9].group == 3);
    { // The pattern chain (seq-sheet2 B): kept in order, from its first; the next queued in the last bar of the one playing.
        const auto& ch = copy->tracks[0];
        CHECK(ch.chained() && ch.chain_count == 3 && ch.chain[0] == 2 && ch.chain[1] == 0 && ch.chain[2] == 5 && ch.chain_at == 0);
        CHECK(!copy->tracks[1].chained() && copy->tracks[1].chain_count == 0);
        CHECK(ch.chain_ahead(0) == 2 && ch.chain_ahead(1) == 0 && ch.chain_ahead(3) == 2); // round again
        constexpr uint32_t bar = 4 * ticks_per_quarter;
        CHECK(!chain_due(0, 2 * bar, bar) && !chain_due(int(bar) - 1, 2 * bar, bar) && chain_due(int(bar), 2 * bar, bar));
        CHECK(chain_due(0, bar, bar) && !chain_due(-1, bar, bar)); // a one-bar pattern: at once; stopped: never
    }
    CHECK(!copy->tracks[4].pads[8].used());
    CHECK(std::string(copy->samples[2].path) == "/media/card/Samples/Kick.wav");
    CHECK(copy_bank->at[0][3].length == 96 * 480 && copy_bank->at[0][3].division == Division::Eighth);
    CHECK(copy_bank->at[0][3].events.size() == 1 && copy_bank->at[0][3].events[0] == Event({95 * 480 + 5, 40, 64, 77, 100, step_tie}));
    CHECK(copy_bank->at[0][3].step(95).notes[0].key == 64 && copy_bank->at[0][3].step(95).notes[0].offset == 5);
    CHECK(copy_bank->at[0][3].motion.size() == 1 && copy_bank->at[0][3].motion[0].value == 61 && copy_bank->at[0][3] == bank->at[0][3]);
    CHECK(copy->song.count == 1 && copy->song.clips[0].muted && copy->song.marker_count == 1 && copy->song.loop_end == 12);
    CHECK(copy->buses[1].level == 33 && copy->buses[1].fx.slots[0].kind == uint8_t(EffectKind::Room));
    CHECK(copy->master.slots[0].kind == uint8_t(EffectKind::Limiter));
    // A file that does not fit is refused and changes nothing; an unknown section is passed over.
    auto damaged = data;
    damaged.resize(damaged.size() / 2);
    copy->tempo = 999;
    CHECK(!load_project(damaged, *copy, *copy_bank).empty() && copy->tempo == 999);
    auto extended = data;
    const uint8_t extra[] {'N', 'E', 'X', 'T', 3, 0, 0, 0, 1, 2, 3};
    extended.insert(extended.end(), extra, extra + sizeof(extra));
    CHECK(load_project(extended, *copy, *copy_bank).empty() && copy->tempo == 1245);
    CHECK(!load_project({1, 2, 3}, *copy, *copy_bank).empty());
}
void helpers() {
    // Chords from the scale: pad 2 in D Dorian from octave 3 is Em7, E3 G3 B3 D4 (the Chord board).
    Project project;
    project.root = 2; project.scale = 2;
    Track track;
    track.octave = 2; track.chord_type = 1;
    std::array<int, notes_per_step> keys {};
    CHECK(chord_notes(project, track, 1, keys) == 4 && keys[0] == 40 && keys[1] == 43 && keys[2] == 47 && keys[3] == 50);
    CHECK(chord_name(project, track, 1) == "Em7" && chord_name(project, track, 0) == "Dm7");
    track.chord_invert = 1;
    chord_notes(project, track, 1, keys);
    CHECK(keys[0] == 43 && keys[3] == 52); // first inversion: E goes up
    track.chord_type = 4; track.chord_invert = 0;
    CHECK(chord_notes(project, track, 0, keys) == 3 && keys[2] == keys[0] + 12 && chord_name(project, track, 0) == "D5");
    track.chord_type = 1; track.transpose = 2; // the pads moved a tone, E Dorian: pad 2 is F#, its chord F#m7 (not Em7 under it)
    CHECK(chord_notes(project, track, 1, keys) == 4 && keys[0] == pad_note(project, track, 1).key && keys[0] == 42 && keys[3] == 52);
    CHECK(chord_name(project, track, 1) == "F#m7");
    track.transpose = 0;
    { // Any keys held together, named (stage-preset B): the shape over a root, the bass after a slash when it is not the root.
        const int dm7[] {38, 41, 45, 48}, c[] {48, 52, 55}, c_e[] {52, 55, 60}, one[] {60}, odd[] {60, 61}, gsus4[] {43, 48, 50};
        CHECK(name_chord(dm7, 4) == "Dm7" && name_chord(c, 3) == "C" && name_chord(c_e, 3) == "C/E");
        CHECK(name_chord(one, 1).empty() && name_chord(odd, 2).empty() && name_chord(gsus4, 3) == "Gsus4");
    }
    // The arp: up, down and up-down over two keys and two octaves.
    Arp a;
    a.press(64, 90); a.press(60, 100);
    a.octaves = 2;
    int v = 0;
    std::array<int, 6> up {};
    for (auto& k : up) k = a.next(v);
    CHECK(up[0] == 60 && up[1] == 64 && up[2] == 72 && up[3] == 76 && up[4] == 60);
    a.mode = ArpMode::Down; a.position = 0;
    CHECK(a.next(v) == 76 && a.next(v) == 72);
    a.mode = ArpMode::Order; a.position = 0; a.octaves = 1;
    CHECK(a.next(v) == 64 && v == 90 && a.next(v) == 60);
    a.release(64); a.release(60);
    CHECK(a.next(v) == -1);
    // The host plays the arp while the song is stopped.
    auto p = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    synth(p->tracks[0], 0);
    EngineHost host;
    host.load(*p, *bank);
    tones_for(host, *p);
    host.send({Command::Type::Arp, 0, 0, 0, 1});
    host.send({Command::Type::ArpKey, 0, 0, 60, 100});
    std::vector<int32_t> out(256);
    float peak = 0;
    for (unsigned n = 0; n < 100; ++n) { host.render(out.data(), 128, 1); for (auto s : out) peak = std::max(peak, std::fabs(s / 2147483648.0f)); }
    CHECK(!host.playing() && peak > 0.005f);
}
// Round 2, pad-fx C: punches kept with a pattern. What was heard becomes the pattern's punches where they fell,
// they go with the pattern through its speed, Double / Half Seq and Half / Double Speed, and the file keeps them
// (a pattern holding nothing else too); the host plays them on their ticks, several at once, each let go after its
// length, and quiet while a live punch sounds.
void kept_punches() {
    { // heard_punches: now 900, 132 ticks into the pass (begun at 768); the last pass is 516 .. 900.
        const PunchSpan spans[] {
            {100, 150, 4, 255, 90, 2, 1, false}, // before the last pass: gone
            {500, 600, 8, 2, 100, 1, 0, false},  // from before it: from where it begins
            {700, 0, 14, 255, 70, 3, 2, true},   // still sounding: until now
            {800, 800, 5, 255, 100, 2, 1, false}, // let go before its grid line: nothing heard
        };
        auto heard = heard_punches(spans, 4, 900, 132, 384);
        CHECK(heard.size() == 3);
        CHECK(heard[0] == PatternPunch({132, 84, 8, 2, 100, 1, 0}));
        CHECK(heard[1] == PatternPunch({316, 68, 14, 255, 70, 3, 2})); // across the loop point: up to the end,
        CHECK(heard[2] == PatternPunch({0, 132, 14, 255, 70, 3, 2}));  // and the rest from the start
        CHECK(heard_punches(spans, 4, 900, 132, 0).empty());
        auto later = heard_punches(spans + 1, 1, 5000, 0, 384); // Keep long after: the take is still the last one heard
        CHECK(later.size() == 1 && later[0].tick == 108 && later[0].length == 100);
        const PunchSpan two_takes[] {{100, 200, 4, 255, 100, 2, 1, false}, {1000, 1100, 8, 255, 100, 2, 1, false}};
        auto take = heard_punches(two_takes, 2, 2000, 0, 384); // a pass and more apart: the newer alone
        CHECK(take.size() == 1 && take[0].kind == 8);
        const PunchSpan long_one[] {{0, 0, 9, 255, 100, 2, 1, false}}; // held over two passes: one pass long, in two parts
        auto whole = heard_punches(long_one, 1, 1000, 232, 384);
        CHECK(whole.size() == 2 && whole[0].tick == 232 && whole[0].length == 152 && whole[1].tick == 0 && whole[1].length == 232);
        const PunchSpan from_top[] {{768, 0, 9, 255, 100, 2, 1, false}}; // a whole pass from its top: one
        auto top = heard_punches(from_top, 1, 1152, 0, 384);
        CHECK(top.size() == 1 && top[0].tick == 0 && top[0].length == 384);
        std::vector<PunchSpan> many;
        for (unsigned n = 0; n < 40; ++n) many.push_back({uint64_t(n * 8), uint64_t(n * 8 + 4), uint8_t(n % 16), 255, 100, 2, 1, false});
        CHECK(heard_punches(many.data(), 40, 384, 0, 384).size() == max_pattern_punches);
    }
    { // With the pattern: its speed, Double and Half Seq, Half and Double Speed.
        Pattern p;
        p.length = 384;
        p.punches = {{96, 48, 4, 255, 100, 2, 1}, {300, 24, 8, 255, 100, 2, 1}};
        auto fast = speed_pattern(p, 9); // x2: half the ticks
        CHECK(fast.length == 192 && fast.punches[0].tick == 48 && fast.punches[0].length == 24 && fast.punches[1].tick == 150 && fast.punches[1].length == 12);
        auto slow = speed_pattern(p, 5); // x1/2: twice
        CHECK(slow.punches[1].tick == 600 && slow.punches[1].length == 48);
        Pattern twice = p;
        CHECK(double_pattern(twice, 4 * 384) && twice.punches.size() == 4 && twice.punches[2].tick == 96 + 384 && twice.punches[3].tick == 300 + 384);
        Pattern bare = p;
        CHECK(double_pattern(bare, 4 * 384, false) && bare.punches.size() == 2);
        Pattern half = p;
        half_pattern(half);
        CHECK(half.length == 192 && half.punches.size() == 1 && half.punches[0].tick == 96);
        // Half Seq: a punch running into the second half ends with the first (it stayed on round the shorter loop),
        // and Double Seq after it brings back nothing of it.
        Pattern across = p;
        across.punches = {{0, 384, 4, 255, 100, 2, 1}, {100, 250, 8, 255, 100, 2, 1}, {200, 20, 9, 255, 100, 2, 1}};
        half_pattern(across);
        CHECK(across.punches.size() == 2 && across.punches[0] == PatternPunch({0, 192, 4, 255, 100, 2, 1}));
        CHECK(across.punches[1] == PatternPunch({100, 92, 8, 255, 100, 2, 1}));
        CHECK(double_pattern(across, 4 * 384) && across.punches.size() == 4 && across.punches[1].length == 92 && across.punches[3].tick == 292);
        Pattern stretched = p; // Half Speed: twice as long, a punch ends with the pattern
        stretched.punches = {{150, 100, 4, 255, 100, 2, 1}};
        scale_pattern(stretched, true);
        CHECK(stretched.punches.size() == 1 && stretched.punches[0].tick == 300 && stretched.punches[0].length == 84);
        Pattern shorter = p; // a shorter length: the punch past the end is not played at another speed either
        shorter.length = 192;
        auto shorter_fast = speed_pattern(shorter, 9);
        CHECK(shorter_fast.punches.size() == 1 && shorter_fast.punches[0].tick == 48);
        // A file from before: a punch across the loop point as it was heard, in two parts; one longer than its
        // pattern ends with it; one past the end stays there.
        Pattern old = p;
        old.punches = {{100, 500, 4, 255, 100, 2, 1}, {300, 200, 8, 255, 100, 2, 1}, {400, 50, 9, 255, 100, 2, 1}};
        split_punches(old);
        CHECK(old.punches.size() == 4 && old.punches[0] == PatternPunch({0, 116, 8, 255, 100, 2, 1}));
        CHECK(old.punches[1] == PatternPunch({100, 284, 4, 255, 100, 2, 1}) && old.punches[2] == PatternPunch({300, 84, 8, 255, 100, 2, 1}));
        CHECK(old.punches[3] == PatternPunch({400, 50, 9, 255, 100, 2, 1}));
        Pattern slower = p;
        scale_pattern(slower, true);
        CHECK(slower.punches.size() == 1 && slower.punches[0].tick == 192 && slower.punches[0].length == 96); // 600 falls past the end
        Pattern faster = p;
        scale_pattern(faster, false);
        CHECK(faster.punches.size() == 2 && faster.punches[1].tick == 150 && faster.punches[1].length == 12);
    }
    { // The file (KEPT): kept as they were, a pattern holding only punches too, beside the song's (PNCH); none
      // where there were none.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->song.add_punch({2, 4, 8, 3, 255, 90});
        bank->at[3][5].punches = {{0, 960, 8, 255, 100, 2, 1}, {2880, 480, 14, 3, 70, 3, 2}};
        auto data = save_project(*project, *bank);
        auto copy = std::make_unique<Project>();
        auto copy_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *copy, *copy_bank).empty());
        CHECK(copy_bank->at[3][5] == bank->at[3][5] && copy_bank->at[3][5].punches.size() == 2);
        CHECK(copy->song.punch_count == 1 && copy->song.punches[0].bar == 2 && copy->song.punches[0].kind == 3);
        CHECK(copy_bank->at[0][0].punches.empty() && copy_bank->at[3][4].punches.empty());
        // A file from before kept punches ended with their pattern (no 128 in a punch's last byte): one across the
        // loop point is read in two parts; a file since is read as it is.
        bank->at[3][5].punches = {{3000, 2000, 8, 255, 100, 2, 1}};
        auto since = save_project(*project, *bank);
        CHECK(load_project(since, *copy, *copy_bank).empty() && copy_bank->at[3][5].punches.size() == 1);
        const uint8_t tag[] {'K', 'E', 'P', 'T'};
        auto kept = std::search(since.begin(), since.end(), std::begin(tag), std::end(tag));
        CHECK(since.end() - kept >= 23 && (kept[22] & 128));
        kept[22] &= 127; // tag, size, track, pattern, count, then tick, length, kind, target, depth and this
        CHECK(load_project(since, *copy, *copy_bank).empty() && copy_bank->at[3][5].punches.size() == 2);
        CHECK(copy_bank->at[3][5].punches[0] == PatternPunch({0, 1160, 8, 255, 100, 2, 1}));
        CHECK(copy_bank->at[3][5].punches[1] == PatternPunch({3000, 840, 8, 255, 100, 2, 1}));
    }
    { // The host: Low on the master from the bar's start for a sixteenth's two, Stutter on track 1 from the first
      // sixteenth's end for four; both at once, each let go after its length, again on the next pass; Stop lets go.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        project->tempo = 1200;
        bank->at[0][0].length = project->bar_ticks();
        bank->at[0][0].punches = {{0, 480, uint8_t(PunchKind::Low), 255, 100, 2, 1}, {240, 960, uint8_t(PunchKind::Stutter), 0, 100, 2, 1}};
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        tones_for(*host, *project);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Play});
        unsigned blocks = 0;
        auto run_to = [&](unsigned n) { for (; blocks < n; ++blocks) host->render(out.data(), 128, 1); }; // a tick is 22.97 samples
        run_to(1);
        CHECK(host->kept_punches() == 1);
        run_to(50); // tick 275
        CHECK(host->kept_punches() == 3);
        run_to(100); // tick 557: Low let go
        CHECK(host->kept_punches() == 2);
        run_to(230); // tick 1297: Stutter let go
        CHECK(host->kept_punches() == 0);
        run_to(700); // the next pass
        CHECK(host->kept_punches() == 1);
        // A live punch wins: from its grid line (4080) the kept ones are quiet, Stutter due there too; let go, both
        // come back for the rest of their length.
        Command on {Command::Type::PunchOn, 255, uint8_t(2 | 1 << 4), uint16_t(PunchKind::Gate), 100};
        on.position = 1;
        host->send(on);
        run_to(740); // tick 4123
        CHECK(host->live_punches() == 1 && host->kept_punches() == 0);
        Command off {Command::Type::PunchOff, 255};
        off.position = 1;
        host->send(off);
        run_to(745); // tick 4151: Low (to 4320) and Stutter (to 5040) again
        CHECK(host->live_punches() == 0 && host->kept_punches() == 3);
        host->send({Command::Type::Stop});
        run_to(746);
        CHECK(host->kept_punches() == 0);
    }
    { // Pad FX quantize (Shift + B3): playing, a live punch waits for its snap grid's next line (1/4 here: tick 960)
      // while a kept one sounds on; with 64 in its timing (quantize off) it starts at once, the kept one giving way.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        project->tempo = 1200;
        bank->at[0][0].length = project->bar_ticks();
        bank->at[0][0].punches = {{0, project->bar_ticks(), uint8_t(PunchKind::Low), 255, 100, 2, 1}};
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        tones_for(*host, *project);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Play});
        unsigned blocks = 0;
        auto run_to = [&](unsigned n) { for (; blocks < n; ++blocks) host->render(out.data(), 128, 1); }; // a tick is 22.97 samples
        run_to(20); // tick 111
        CHECK(host->kept_punches() == 1);
        Command on {Command::Type::PunchOn, 255, uint8_t(0 | 1 << 4), uint16_t(PunchKind::Gate), 100};
        on.position = 1;
        host->send(on);
        run_to(60); // tick 334: waiting for 960
        CHECK(host->live_punches() == 1 && host->kept_punches() == 1);
        run_to(180); // tick 1003
        CHECK(host->kept_punches() == 0);
        Command off {Command::Type::PunchOff, 255};
        off.position = 1;
        host->send(off);
        run_to(183); // tick 1020: Low back
        CHECK(host->live_punches() == 0 && host->kept_punches() == 1);
        on.pattern = uint8_t(0 | 1 << 4 | 64);
        host->send(on);
        run_to(186); // tick 1036, the next quarter 1920
        CHECK(host->live_punches() == 1 && host->kept_punches() == 0);
    }
    // A kept punch ends with its pattern's loop, and with the pattern itself. A tick is 22.97 samples, 0.1794 blocks.
    auto kept_host = [](Project& project, PatternBank& bank) {
        synth(project.tracks[0], 0);
        project.tempo = 1200;
        auto host = std::make_unique<EngineHost>();
        host->load(project, bank);
        tones_for(*host, project);
        return host;
    };
    auto send_pattern = [](EngineHost& host, const Pattern& p) {
        Command c {Command::Type::PatternData, 0, 0};
        c.data = new Pattern(p);
        CHECK(host.send(c));
    };
    { // One longer than the loop (a shorter length set since, or a file Half Seq left it in): from its tick to the
      // loop's end, quiet from the top to its tick again, pass after pass. It stayed on from the second pass.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        bank->at[0][0].length = 3840;
        bank->at[0][0].punches = {{1920, 3840, uint8_t(PunchKind::Low), 255, 100, 2, 1}};
        auto host = kept_host(*project, *bank);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Play});
        unsigned blocks = 0;
        auto run_to = [&](unsigned n) { for (; blocks < n; ++blocks) host->render(out.data(), 128, 1); };
        run_to(180); // tick 1003
        CHECK(host->kept_punches() == 0);
        run_to(362); // tick 2014
        CHECK(host->kept_punches() == 1);
        run_to(782); // tick 4358, the second pass
        CHECK(host->kept_punches() == 0);
        run_to(1052); // tick 5862
        CHECK(host->kept_punches() == 1);
        run_to(1472); // tick 8203, the third
        CHECK(host->kept_punches() == 0);
    }
    { // Half Seq while a punch running into the second half sounds: it goes on to the first half's end, and plays
      // there again on the next pass. It sounded on round the shorter loop for its whole length.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        auto& pattern = bank->at[0][0];
        pattern.length = 7680;
        pattern.punches = {{3000, 3000, uint8_t(PunchKind::Low), 255, 100, 2, 1}};
        auto host = kept_host(*project, *bank);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Play});
        unsigned blocks = 0;
        auto run_to = [&](unsigned n) { for (; blocks < n; ++blocks) host->render(out.data(), 128, 1); };
        run_to(578); // tick 3221
        CHECK(host->kept_punches() == 1);
        half_pattern(pattern);
        CHECK(pattern.length == 3840 && pattern.punches.size() == 1 && pattern.punches[0].length == 840);
        send_pattern(*host, pattern);
        run_to(582); // tick 3243
        CHECK(host->kept_punches() == 1);
        run_to(700); // tick 3900, the next pass's 60
        CHECK(host->kept_punches() == 0);
        run_to(1240); // tick 6909, its 3069
        CHECK(host->kept_punches() == 1);
        pattern.punches.clear(); // Erase + pad while it sounds: gone at once
        send_pattern(*host, pattern);
        run_to(1243);
        CHECK(host->kept_punches() == 0);
    }
    { // Half Seq while a punch of the second half sounds: it goes with its half, at once.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        auto& pattern = bank->at[0][0];
        pattern.length = 7680;
        pattern.punches = {{7000, 600, uint8_t(PunchKind::High), 255, 100, 2, 1}};
        auto host = kept_host(*project, *bank);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Play});
        unsigned blocks = 0;
        auto run_to = [&](unsigned n) { for (; blocks < n; ++blocks) host->render(out.data(), 128, 1); };
        run_to(1275); // tick 7105
        CHECK(host->kept_punches() == 1);
        half_pattern(pattern);
        CHECK(pattern.punches.empty());
        send_pattern(*host, pattern);
        run_to(1278); // tick 7122
        CHECK(host->kept_punches() == 0);
    }
    { // An editor's loop: a punch reaching past the loop's end ends there.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        bank->at[0][0].length = 7680;
        bank->at[0][0].punches = {{960, 5000, uint8_t(PunchKind::Low), 255, 100, 2, 1}};
        auto host = kept_host(*project, *bank);
        std::vector<int32_t> out(256);
        Command loop {Command::Type::EditLoop, 0};
        loop.position = 0; loop.index = 1920; // the first two beats
        host->send(loop);
        host->send({Command::Type::Play});
        unsigned blocks = 0;
        auto run_to = [&](unsigned n) { for (; blocks < n; ++blocks) host->render(out.data(), 128, 1); };
        run_to(200); // tick 1114
        CHECK(host->kept_punches() == 1);
        run_to(380); // tick 2117, the loop's 197
        CHECK(host->kept_punches() == 0);
        run_to(560); // tick 3121, its 1201
        CHECK(host->kept_punches() == 1);
    }
    { // A punch held across the loop point, kept in two parts, sounds as one: the second carries on from the first,
      // not started again. Heard against the same punch in one part in a pattern twice as long; with another depth
      // the second part is a punch of its own, started at the top.
        auto render = [&](uint32_t length, std::vector<PatternPunch> punches) {
            auto project = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            auto& pattern = bank->at[0][0];
            pattern.length = 3840;
            for (unsigned step : {0u, 3u, 6u, 8u, 11u, 14u}) put(pattern, step, uint8_t(48 + step), 110, 2);
            if (length == 7680) CHECK(double_pattern(pattern, 7680));
            pattern.punches = std::move(punches);
            auto host = kept_host(*project, *bank);
            std::vector<int32_t> out(256);
            std::vector<float> heard;
            host->send({Command::Type::Play});
            for (unsigned n = 0; n < 1150; ++n) { // to tick 6409, before the punch comes round again
                host->render(out.data(), 128, 1);
                for (unsigned f = 0; f < 128; ++f) heard.push_back(float(out[2 * f]) / 2147483648.0f);
            }
            return heard;
        };
        const uint8_t stutter = uint8_t(PunchKind::Stutter);
        auto one = render(7680, {{0, 400, stutter, 255, 100, 2, 1}, {3000, 1240, stutter, 255, 100, 2, 1}});
        auto two = render(3840, {{0, 400, stutter, 255, 100, 2, 1}, {3000, 840, stutter, 255, 100, 2, 1}});
        auto apart = render(3840, {{0, 400, stutter, 255, 99, 2, 1}, {3000, 840, stutter, 255, 100, 2, 1}});
        float most = 0, least = 0, loud = 0;
        for (size_t n = 0; n < one.size(); ++n) {
            most = std::max(most, std::fabs(one[n] - two[n]));
            loud = std::max(loud, std::fabs(one[n]));
            if (n > 384 * 229) least = std::max(least, std::fabs(one[n] - apart[n]));
        }
        CHECK(loud > 0.01f && most < 1e-5f && least > 0.01f);
    }
    { // The song comes to its end while a kept punch sounds to its pattern's end: it lets go there. No tick came
      // after the last to end it, and it sounded on until Stop was pressed.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        bank->at[0][0].length = 3840;
        bank->at[0][0].punches = {{1920, 1920, uint8_t(PunchKind::Low), 255, 100, 2, 1}};
        project->song.place({0, 0, 0, 1, false});
        auto host = kept_host(*project, *bank);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Play, 0, 0, 0, 1});
        unsigned blocks = 0;
        auto run_to = [&](unsigned n) { for (; blocks < n; ++blocks) host->render(out.data(), 128, 1); };
        run_to(400); // tick 2225
        CHECK(host->playing() && host->kept_punches() == 1);
        run_to(700); // past tick 3840
        CHECK(!host->playing() && host->kept_punches() == 0);
    }
    { // The wheel moves the playhead (Locate) while a kept punch sounds: where it plays now is outside the punch,
      // which lets go. Its end was a tick counted from Play, so after a move back it sounded on for a pass and more.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        bank->at[0][0].length = 7680;
        bank->at[0][0].punches = {{7000, 600, uint8_t(PunchKind::High), 255, 100, 2, 1}};
        auto host = kept_host(*project, *bank);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Play});
        unsigned blocks = 0;
        auto run_to = [&](unsigned n) { for (; blocks < n; ++blocks) host->render(out.data(), 128, 1); };
        run_to(1275); // tick 7105
        CHECK(host->kept_punches() == 1);
        Command locate {Command::Type::Locate};
        locate.position = 960;
        host->send(locate);
        run_to(1280); // tick 982
        CHECK(host->position_of(0) < 1100 && host->kept_punches() == 0);
        run_to(1880); // tick 4321
        CHECK(host->kept_punches() == 0);
        locate.position = 7200; // into the punch: not sounding (as Play from inside one), then on the next pass
        host->send(locate);
        run_to(1890);
        CHECK(host->kept_punches() == 0);
        run_to(1890 + 1340); // tick 7200 + 7480: the next pass's 7000
        CHECK(host->kept_punches() == 1);
    }
    { // A track loaded whole while it plays (a track copied onto it, an undo): the punch of the pattern it played
      // lets go with that pattern.
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        bank->at[0][0].length = 7680;
        bank->at[0][0].punches = {{960, 6000, uint8_t(PunchKind::High), 255, 100, 2, 1}};
        auto host = kept_host(*project, *bank);
        std::vector<int32_t> out(256);
        host->send({Command::Type::Play});
        unsigned blocks = 0;
        auto run_to = [&](unsigned n) { for (; blocks < n; ++blocks) host->render(out.data(), 128, 1); };
        run_to(360); // tick 2003
        CHECK(host->kept_punches() == 1);
        auto bare = std::make_unique<std::array<Pattern, pattern_count>>();
        (*bare)[0].length = 7680;
        Command load {Command::Type::TrackLoad};
        load.track_pack = new TrackPack(0, project->tracks[0], *bare);
        CHECK(host->send(load));
        run_to(364);
        CHECK(host->kept_punches() == 0);
        const TrackPack* done = nullptr;
        while (host->take_done_track(done)) delete done;
    }
}

void punches() {
    // A test signal with no repeats in it: noise.
    auto signal = [](unsigned n) { uint32_t h = n * 2654435761u; return (int32_t(h) >> 8) / 8388608.0f * 0.5f; };
    const double bar = 88320; // about two seconds a bar, a whole number of samples a slice
    for (unsigned k = 0; k < punch_count; ++k) {
        Punch punch;
        std::vector<float> l(128), r(128);
        unsigned at = 0;
        auto block = [&] { for (unsigned f = 0; f < 128; ++f) l[f] = r[f] = signal(at + f); punch.process(l.data(), r.data(), 128); at += 128; };
        for (unsigned b = 0; b < 200; ++b) block(); // history
        punch.start(PunchKind(k), 1, bar, bar / 16, 1);
        float peak = 0;
        for (unsigned b = 0; b < 1400; ++b) { // four seconds held, past the history's length
            block();
            for (unsigned f = 0; f < 128; ++f) { CHECK(std::isfinite(l[f]) && std::isfinite(r[f])); peak = std::max(peak, std::fabs(l[f])); }
        }
        if (peak > 3) std::fprintf(stderr, "%s peak %f\n", punch_name(PunchKind(k)), peak);
        CHECK(peak < 3);
        punch.stop();
        for (unsigned b = 0; b < 40; ++b) block(); // the 60 ms fade and then some
        CHECK(!punch.active());
        block();
        for (unsigned f = 0; f < 128; ++f) CHECK(l[f] == signal(at - 128 + f)); // dry again, exactly
    }
    // Loop 16: the first slice plays as it comes, then again and again.
    {
        Punch punch;
        std::vector<float> l(1), r(1), out;
        unsigned at = 0, loop = unsigned(bar / 16);
        punch.start(PunchKind::Loop16, 1, bar, bar / 16, 0);
        for (; at < loop * 4; ++at) { l[0] = r[0] = signal(at); punch.process(l.data(), r.data(), 1); out.push_back(l[0]); }
        const unsigned seam = unsigned(std::ceil(Punch::seam_frames(loop))); // each turn's end crossfades into its start
        for (unsigned n = 0; n + seam < loop; ++n) {
            CHECK(out[n] == signal(n));
            CHECK(std::fabs(out[loop + n] - signal(n)) < 1e-6f && std::fabs(out[3 * loop + n] - signal(n)) < 1e-6f);
        }
    }
    // Stutter: the slice, then silence as long.
    {
        Punch punch;
        std::vector<float> l(1), r(1), out;
        unsigned loop = unsigned(bar / 32);
        punch.start(PunchKind::Stutter, 1, bar, bar / 16, 0);
        for (unsigned at = 0; at < loop * 3; ++at) { l[0] = r[0] = signal(at); punch.process(l.data(), r.data(), 1); out.push_back(l[0]); }
        CHECK(peak_of(out, loop + 2, 2 * loop - 2) == 0 && peak_of(out, 2 * loop + 2, 3 * loop - 2) > 0.1f);
    }
    // clear() (a project switch): what played before it reads as silence, though nothing is wiped; Echo repeats
    // what came two grid steps back, so without the clear it plays the noise again.
    for (bool cleared : {false, true}) {
        Punch punch;
        std::vector<float> l(128), r(128);
        unsigned at = 0;
        auto block = [&](bool sound) {
            for (unsigned f = 0; f < 128; ++f) l[f] = r[f] = sound ? signal(at + f) : 0.0f;
            punch.process(l.data(), r.data(), 128);
            at += 128;
        };
        for (unsigned b = 0; b < 200; ++b) block(true);
        if (cleared) punch.clear();
        for (unsigned b = 0; b < 4; ++b) block(false); // a little silence since
        punch.start(PunchKind::Echo, 1, bar, bar / 16, 0);
        float peak = 0;
        for (unsigned b = 0; b < 40; ++b) {
            block(false);
            for (unsigned f = 0; f < 128; ++f) peak = std::max(peak, std::fabs(l[f]));
        }
        CHECK(cleared ? peak == 0 : peak > 0.1f);
    }
    // Half depth: halfway between dry and wet (the gate's closed half).
    {
        Punch punch;
        std::vector<float> l(1), r(1);
        punch.start(PunchKind::Gate, 0.5f, bar, 1000, 0);
        float worst = 0;
        for (unsigned at = 0; at < 1000; ++at) {
            l[0] = r[0] = signal(at); punch.process(l.data(), r.data(), 1);
            if (at > 510) worst = std::max(worst, std::fabs(l[0] - signal(at) * 0.5f));
        }
        CHECK(worst < 1e-6f);
    }
    // The host: a master punch while stopped starts at once; Off lets go; a pad punch on a sampler track.
    {
        auto host = std::make_unique<EngineHost>();
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        host->load(*project, *patterns);
        std::vector<int32_t> out(256);
        host->send({Command::Type::PunchOn, 255, uint8_t(2 | 1 << 4), uint16_t(PunchKind::Gate), 100});
        host->render(out.data(), 128, 1);
        CHECK(host->punching());
        host->send({Command::Type::PunchOff, 254});
        host->render(out.data(), 128, 1);
        CHECK(!host->punching());
        host->send({Command::Type::Engine, 4, 0, uint16_t(EngineKind::Sampler)});
        host->send({Command::Type::PunchOn, 4, 2, uint16_t(unsigned(PunchKind::Stutter) | 3 << 8), 80});
        host->render(out.data(), 128, 1);
        CHECK(host->punching());
        host->send({Command::Type::PunchOff, 4, 0, uint16_t(3 << 8)});
        host->render(out.data(), 128, 1);
        CHECK(!host->punching());
    }
    // Punch settings survive a save.
    {
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        project->punch_target = 1; project->punch_bank = 1; project->punch_depth = 64; project->punch_snap = 3; project->punch_tail = 2;
        project->punch_quantize = false;
        std::strcpy(project->samples.edit(0).path, "/x.wav");
        auto bytes = save_project(*project, *patterns);
        auto back = std::make_unique<Project>();
        CHECK(load_project(bytes, *back, *patterns).empty());
        CHECK(back->punch_target == 1 && back->punch_bank == 1 && back->punch_depth == 64 && back->punch_snap == 3 && back->punch_tail == 2);
        CHECK(!back->punch_quantize);
        project->punch_quantize = true;
        auto again = std::make_unique<Project>();
        CHECK(load_project(save_project(*project, *patterns), *again, *patterns).empty() && again->punch_quantize);
    }
}

// Parity phase 5: Chop on a region (types, slice start / end, Split, Merge,
// Manual's limit and its taps pulled onto hits), the cards and their zoom,
// the preview slot in the engine, Trim Sample as a Shift + pad action.
// Hits from the spectrum (engine/onsets.cpp): what a level-only detector misses, cuts on or just before the attack,
// nothing from a held tone or the sample's end.
void onset_hits() {
    auto make = [](double seconds, const std::function<double(double)>& at) {
        SampleData s;
        s.frames = uint32_t(seconds * 44100);
        for (uint32_t n = 0; n < s.frames; ++n) s.left.push_back(float(at(n / 44100.0)));
        s.right = s.left;
        return s;
    };
    auto hits_of = [](const SampleData& s) {
        std::array<Hit, max_hits> hits {};
        unsigned count = find_hits(s, hits);
        return std::vector<Hit>(hits.begin(), hits.begin() + count);
    };
    // A cut within `early` ms before `seconds` (never after).
    auto near = [](const Hit& h, double seconds, double early) { return h.frame <= seconds * 44100 + 44 && h.frame + early * 44.1 >= seconds * 44100; };

    // A held tone from 0.2 s: one hit where it starts, none while it holds or where the sample ends.
    auto tone = hits_of(make(2.0, [](double t) { return t < 0.2 ? 0 : 0.4 * std::sin(2 * M_PI * 330 * t) * std::min(1.0, (t - 0.2) / 0.005); }));
    CHECK(tone.size() == 2 && tone[0].frame == 0 && near(tone[1], 0.2, 2));

    // Chord changes at the same loudness (10 ms crossfades at 0.5 and 1.0 s): hits there and nowhere else.
    auto chord = [](double t, const double* f) { double v = 0; for (int k = 0; k < 3; ++k) v += std::sin(2 * M_PI * f[k] * t); return 0.2 * v; };
    auto chords = hits_of(make(1.5, [&](double t) {
        static const double a[3] = {220, 277.18, 329.63}, b[3] = {196, 246.94, 293.66}, c[3] = {174.61, 220, 261.63};
        double x = t < 0.5 ? chord(t, a) : t < 0.51 ? chord(t, a) * (0.51 - t) / 0.01 + chord(t, b) * (t - 0.5) / 0.01
                 : t < 1.0 ? chord(t, b) : t < 1.01 ? chord(t, b) * (1.01 - t) / 0.01 + chord(t, c) * (t - 1.0) / 0.01 : chord(t, c);
        return x * std::min(1.0, t / 0.005);
    }));
    CHECK(chords.size() == 3 && near(chords[1], 0.5, 12) && near(chords[2], 1.0, 12));

    // Hats (30 ms noise bursts) every 0.25 s over a loud held 110 Hz pad: each one, cut at most 3 ms early.
    uint32_t noise = 1;
    auto hats = hits_of(make(1.5, [&](double t) {
        double x = 0.5 * std::sin(2 * M_PI * 110 * t) * std::min(1.0, t / 0.005), in = std::fmod(t, 0.25);
        if (t >= 0.25 && in < 0.03) {
            noise = noise * 1664525u + 1013904223u;
            x += 0.08 * ((noise >> 8) / 8388608.0 - 1) * std::exp(-in / 0.008);
        }
        return x;
    }));
    CHECK(hats.size() == 6);
    for (unsigned n = 1; n < hats.size(); ++n) CHECK(near(hats[n], 0.25 * n, 3) && hats[n].strength > 200);

    // A kick with a 5 ms ramp at 0.7 s over a fading tone: the cut lands where the ramp starts, not where it peaks.
    auto kick = hits_of(make(1.2, [](double t) {
        double x = t > 0.1 ? 0.3 * std::sin(2 * M_PI * 440 * t) * std::exp(-(t - 0.1) / 0.15) : 0;
        if (t >= 0.7) {
            double u = t - 0.7;
            x += 0.8 * std::min(1.0, u / 0.005) * std::sin(2 * M_PI * (60 + 200 * std::exp(-u / 0.02)) * u) * std::exp(-u / 0.2);
        }
        return x;
    }));
    CHECK(kick.size() == 3 && near(kick[1], 0.1, 2) && near(kick[2], 0.7, 2));

    // The same hits at -40 dB: the spectrum's floor follows the sample's level.
    auto quiet = hits_of(make(1.2, [](double t) { return t >= 0.7 ? 0.008 * std::sin(2 * M_PI * 200 * t) * std::exp(-(t - 0.7) / 0.1) : 0; }));
    CHECK(quiet.size() == 2 && near(quiet[1], 0.7, 2));
    CHECK(hits_of(SampleData {}).size() == 1);

    // A flam: a soft stroke 8 ms before the main one, a short dip between. The cut goes before the soft stroke, so
    // the slice before ends clean of it.
    uint32_t grain = 3;
    auto flam = hits_of(make(1.0, [&](double t) {
        auto burst = [&](double at, double level, double decay) {
            if (t < at) return 0.0;
            grain = grain * 1664525u + 1013904223u;
            return level * ((grain >> 8) / 8388608.0 - 1) * std::exp(-(t - at) / decay);
        };
        return burst(0.1, 0.5, 0.03) + burst(0.5, 0.06, 0.002) + burst(0.508, 0.6, 0.04);
    }));
    CHECK(flam.size() == 3 && near(flam[2], 0.5, 3));
}

// A sample's own beats (engine/onsets.cpp) and Chop's Beats on them (shell/chop.h): a loop's tempo exact from its
// length, its beats found past a lead-in, slices from the beat nearest the region's start, a free intro and outro one
// slice each, cuts that follow a performance that speeds up; none for a one shot.
void beat_grid() {
    struct Made { SampleData sample; std::array<Hit, max_hits> hits {}; unsigned count = 0; BeatGrid grid; };
    auto analysed = [](std::vector<float> wave) {
        Made m;
        m.sample.frames = uint32_t(wave.size());
        m.sample.left = wave;
        m.sample.right = std::move(wave);
        m.count = find_hits(m.sample, m.hits, m.grid);
        return m;
    };
    uint32_t noise = 7;
    auto add_kick = [](std::vector<float>& w, double at) {
        for (uint32_t n = uint32_t(at * 44100), end = std::min<uint32_t>(uint32_t(w.size()), n + 13230); n < end; ++n) {
            double u = (n - at * 44100) / 44100;
            w[n] += float(0.8 * std::sin(2 * M_PI * (55 + 150 * std::exp(-u / 0.015)) * u) * std::exp(-u / 0.12));
        }
    };
    auto add_noise = [&](std::vector<float>& w, double at, double level, double decay) {
        for (uint32_t n = uint32_t(at * 44100), end = std::min<uint32_t>(uint32_t(w.size()), n + uint32_t(decay * 6 * 44100)); n < end; ++n) {
            noise = noise * 1664525u + 1013904223u;
            w[n] += float(level * ((noise >> 8) / 8388608.0 - 1) * std::exp(-(n - at * 44100) / 44100 / decay));
        }
    };
    // 8 beats at 93 BPM: kicks on 1 and 3, a snare on 2 and 4, hats on every 8th.
    const double beat = 60 / 93.0;
    std::vector<float> wave(size_t(std::lround(8 * beat * 44100)), 0.0f);
    for (unsigned b = 0; b < 8; ++b) {
        if (b % 2 == 0) add_kick(wave, b * beat);
        else add_noise(wave, b * beat, 0.5, 0.05);
        add_noise(wave, b * beat, 0.12, 0.01);
        add_noise(wave, (b + 0.5) * beat, 0.12, 0.01);
    }
    auto loop = analysed(wave);
    CHECK(loop.grid.found() && loop.grid.tempo == 930);
    // A slice a beat from the start: on the beats, at most 3 ms early.
    auto on_beat = [](uint32_t cut, double seconds) { return cut <= seconds * 44100 + 44 && cut + 133 >= seconds * 44100; };
    auto ones = chop(ChopMode::Beats, 4, loop.sample.frames, loop.hits, loop.count, 1200, &loop.grid);
    CHECK(ones.count == 8);
    for (unsigned n = 1; n < ones.count; ++n) CHECK(on_beat(ones.start[n], n * beat));
    auto bars = chop(ChopMode::Beats, 16, loop.sample.frames, loop.hits, loop.count, 1200, &loop.grid);
    CHECK(bars.count == 2 && on_beat(bars.start[1], 4 * beat));
    // From a region that starts on beat 2: a slice every two beats from there.
    auto twos = chop(ChopMode::Beats, 8, uint32_t(beat * 44100), 0, loop.sample.frames, loop.hits, loop.count, 1200, &loop.grid);
    CHECK(twos.count == 4 && on_beat(twos.start[1], 3 * beat) && on_beat(twos.start[2], 5 * beat) && on_beat(twos.start[3], 7 * beat));
    // Without the sample's beats, the project's tempo as before: 120 BPM, a slice every 22050 frames.
    CHECK(chop(ChopMode::Beats, 4, loop.sample.frames, loop.hits, loop.count, 1200).start[1] == 22050);
    // Simpler's divisions under a beat: 1/8 cuts on the beats and the hats between them, 1/16 on the grid between those.
    auto eighths = chop(ChopMode::Beats, 2, loop.sample.frames, loop.hits, loop.count, 1200, &loop.grid);
    CHECK(eighths.count == 16);
    for (unsigned n = 1; n < eighths.count; ++n) CHECK(on_beat(eighths.start[n], n * beat / 2));
    auto near_grid = [](uint32_t cut, double seconds) { return std::fabs(double(cut) - seconds * 44100) <= 177; }; // 4 ms
    auto sixteenths = chop(ChopMode::Beats, 1, loop.sample.frames, loop.hits, loop.count, 1200, &loop.grid);
    CHECK(sixteenths.count == 32);
    for (unsigned n = 1; n < sixteenths.count; ++n) CHECK(near_grid(sixteenths.start[n], n * beat / 4));
    // Past 128 slices the rest are left out and the chop says so; 8 bars of 1/16 is 128 exactly, nothing left out.
    {
        std::array<Hit, max_hits> none {};
        bool capped = false;
        CHECK(max_slices == 128);
        auto full = chop(ChopMode::Beats, 1, 0, 0, 44100 * 20, none, 0, 1200, nullptr, &capped); // 160 16ths at 120 BPM
        CHECK(capped && full.count == max_slices);
        auto most = chop(ChopMode::Beats, 1, 0, 0, 44100 * 16, none, 0, 1200, nullptr, &capped);
        CHECK(!capped && most.count == 128);
        auto exact = chop(ChopMode::Beats, 1, 0, 0, 44100 * 8, none, 0, 1200, nullptr, &capped);
        CHECK(!capped && exact.count == 64);
        CHECK(chop(ChopMode::Beats, 4, 0, 0, 44100 * 8, none, 0, 1200, nullptr, &capped).count == 16 && !capped);
    }
    CHECK(division_sixteenths(0, 16) == 1 && division_sixteenths(1, 16) == 2 && division_sixteenths(quarter_division, 16) == 4);
    CHECK(division_sixteenths(3, 16) == 8 && division_sixteenths(4, 16) == 16 && division_sixteenths(6, 16) == 64);
    CHECK(division_sixteenths(4, 12) == 12 && division_sixteenths(5, 14) == 28 && division_sixteenths(99, 16) == 64); // 3/4, 7/8; past 4 bars
    CHECK(std::string(division_value(1)) == "1/8" && std::string(division_unit(0)).empty());
    CHECK(std::string(division_unit(4)) == "bar" && std::string(division_unit(6)) == "bars" && std::string(division_value(6)) == "4");
    // Hand cuts (Simpler's manual slices): a new chop keeps the cuts made or moved by hand and leaves out those taken out.
    {
        std::array<Hit, max_hits> none {};
        const uint32_t frames = 44100 * 4;
        auto quarters = chop(ChopMode::Regions, 4, 0, 0, frames, none, 0, 1200); // cuts at 44100, 88200, 132300
        HandCuts hand;
        CHECK(with_hand(quarters, hand, frames).same(quarters)); // none: the chop as it is
        hand.move(88200, 100000);                                // an automatic cut moved by hand
        hand.pin(20000);                                         // one made by hand (Split, a tap)
        hand.take_out(132300);                                   // one taken out (Erase, Merge)
        CHECK(hand.pinned(100000) && hand.pinned(20200) && !hand.pinned(88200) && hand.dropped(88200) && hand.dropped(132300));
        auto has = [](const Slices& s, uint32_t frame) {
            for (unsigned n = 1; n < s.count; ++n) if (s.start[n] == frame) return true;
            return false;
        };
        // Eight regions now (22050 apart): the hand cuts in, the moved and the taken-out ones out, the rest as cut; the
        // line at 22050, a quarter of its spacing from the hand cut at 20000, gives way to it (no 46 ms sliver).
        auto eighths = with_hand(chop(ChopMode::Regions, 8, 0, 0, frames, none, 0, 1200), hand, frames);
        CHECK(eighths.count == 7 && eighths.start[1] == 20000 && eighths.start[2] == 44100 && has(eighths, 100000) && has(eighths, 110250));
        CHECK(!has(eighths, 88200) && !has(eighths, 132300) && has(eighths, 154350) && eighths.stop[eighths.count - 1] == frames);
        hand.move(100000, 101000); // a hand cut moved again: no second automatic cut dropped
        CHECK(hand.pinned(101000) && !hand.pinned(100000) && hand.drop_count == 2 && hand.pin_count == 2);
        HandCuts near;
        near.pin(44500); // an automatic cut within 20 ms of a hand cut gives way to it
        auto kept = with_hand(quarters, near, frames);
        CHECK(kept.count == 4 && has(kept, 44500) && !has(kept, 44100));
        // A hand cut a little after a beat line (within a quarter of the spacing there): the line gives way, no sliver.
        HandCuts late; late.pin(50000);
        auto no_sliver = with_hand(quarters, late, frames);
        CHECK(no_sliver.count == 4 && has(no_sliver, 50000) && !has(no_sliver, 44100) && has(no_sliver, 88200));
        HandCuts between; between.pin(66000); // well between two lines: both stay
        auto both = with_hand(quarters, between, frames);
        CHECK(both.count == 5 && has(both, 44100) && has(both, 66000) && has(both, 88200));
        bool full = false; // 128 regions and a hand cut between two of them: one too many, the hand cut kept
        HandCuts early;
        early.pin(1000);
        auto most = with_hand(chop(ChopMode::Regions, int(max_slices), 0, 0, 2 * frames, none, 0, 1200), early, 2 * frames, &full);
        CHECK(full && most.count == max_slices && most.start[1] == 1000);
        HandCuts other = hand;
        CHECK(other.same(hand) && !other.same(near));
        // A chop read back from its WAV (no pins kept there): told against what its type cuts now. Manual cuts nothing,
        // so every cut is by hand; a Regions chop with one cut moved by hand and one taken out: just those two.
        Slices manual_back; manual_back.whole(0, frames);
        for (uint32_t at : {20000u, 50000u, 90000u}) manual_back.add(at, frames);
        HandCuts again;
        infer_hand(again, manual_back, chop(ChopMode::Manual, 0, 0, 0, frames, none, 0, 1200), frames);
        auto kept_back = with_hand(quarters, again, frames);
        CHECK(again.pin_count == 3 && again.drop_count == 0 && has(kept_back, 20000) && has(kept_back, 50000) && has(kept_back, 90000));
        Slices regions_back; regions_back.whole(0, frames); // quarters with 88200 moved to 100000 and 132300 taken out
        for (uint32_t at : {44100u, 100000u}) regions_back.add(at, frames);
        HandCuts told;
        infer_hand(told, regions_back, quarters, frames);
        CHECK(told.pin_count == 1 && told.pinned(100000) && told.dropped(88200) && told.dropped(132300) && !told.pinned(44100));
        CHECK(with_hand(quarters, told, frames).same(regions_back)); // a new chop by the same type gives it back as it was
        other.clear();
        CHECK(other.empty());
    }

    // The same tempo with the kick on 1, the and of 2 and 4's second 16th (3 + 3 + 2) and swung hats: still 93 BPM
    // (or 186), not the 124 its dotted spacing suggests.
    std::fill(wave.begin(), wave.end(), 0.0f);
    for (unsigned bar = 0; bar < 2; ++bar) {
        double at = bar * 4 * beat;
        for (double k : {0.0, 0.75, 1.5}) add_kick(wave, at + k * beat);
        for (double k : {2.0, 3.25}) add_kick(wave, at + k * beat);
        for (unsigned b = 1; b < 4; b += 2) add_noise(wave, at + b * beat, 0.5, 0.05);
        for (unsigned e = 0; e < 8; ++e) add_noise(wave, at + (e / 2 + (e % 2 ? 0.62 : 0.0)) * beat, 0.1, 0.01);
    }
    auto swung = analysed(wave);
    CHECK(swung.grid.tempo == 930 || swung.grid.tempo == 1860);

    // The first loop after 0.3 s of a quiet pad: its beats fall on the kicks, not on the file's start. From the
    // start, the lead-in is a slice of its own, then a slice a beat from the first kick.
    std::vector<float> lead(size_t(0.3 * 44100), 0.0f);
    for (size_t n = 0; n < lead.size(); ++n) lead[n] = float(0.02 * std::sin(2 * M_PI * 220 * n / 44100.0));
    std::fill(wave.begin(), wave.end(), 0.0f);
    for (unsigned b = 0; b < 8; ++b) {
        if (b % 2 == 0) add_kick(wave, b * beat);
        else add_noise(wave, b * beat, 0.5, 0.05);
        add_noise(wave, (b + 0.5) * beat, 0.12, 0.01);
    }
    lead.insert(lead.end(), wave.begin(), wave.end());
    auto late = analysed(lead);
    CHECK(late.grid.found() && late.grid.tempo == 930);
    double gap = std::fmod(late.grid.first / 44100.0 - 0.3 + 8 * beat, beat);
    CHECK(std::min(gap, beat - gap) < 0.03);
    auto after_lead = chop(ChopMode::Beats, 4, late.sample.frames, late.hits, late.count, 1200, &late.grid);
    CHECK(after_lead.count == 9 && on_beat(after_lead.start[1], 0.3));
    for (unsigned n = 2; n < after_lead.count; ++n) CHECK(on_beat(after_lead.start[n], 0.3 + (n - 1) * beat));
    // The same 8 beats between a free intro and a free outro (hits off the beat): from the file's start, the intro
    // is one slice, then a slice a beat, and the outro one slice from where the last beat ends.
    std::vector<float> free_take(size_t(2.0 * 44100), 0.0f);
    for (double at : {0.38, 0.95, 1.67}) add_noise(free_take, at, 0.3, 0.05); // a third to a half of a beat off the beats
    free_take.insert(free_take.end(), wave.begin(), wave.end());
    free_take.resize(free_take.size() + size_t(1.5 * 44100), 0.0f);
    for (double at : {0.21, 0.83}) add_noise(free_take, 2.0 + 8 * beat + at, 0.3, 0.05);
    auto framed = analysed(free_take);
    CHECK(framed.grid.found());
    auto parts = chop(ChopMode::Beats, 4, framed.sample.frames, framed.hits, framed.count, 1200, &framed.grid);
    CHECK(parts.count == 10 && on_beat(parts.start[1], 2.0));
    for (unsigned n = 2; n < parts.count; ++n) CHECK(on_beat(parts.start[n], 2.0 + (n - 1) * beat));
    // A region that starts 50 ms before a beat counts from that beat: no lead-in slice that short.
    auto sloppy = chop(ChopMode::Beats, 8, uint32_t((0.3 + 4 * beat - 0.05) * 44100), 0, late.sample.frames, late.hits, late.count, 1200,
                       &late.grid);
    CHECK(sloppy.count == 2 && on_beat(sloppy.start[1], 0.3 + 6 * beat));

    // A take that speeds up from 100 to 106 BPM over 12 beats, not cut to bars: the slices follow its kicks.
    wave.assign(size_t(7.4 * 44100), 0.0f);
    std::vector<double> kicks;
    for (double at = 0.2, bpm = 100; at < 7.2; at += 60 / bpm, bpm += 0.5) kicks.push_back(at);
    for (double at : kicks) { add_kick(wave, at); add_noise(wave, at, 0.1, 0.01); }
    auto take = analysed(wave);
    CHECK(take.grid.found() && take.grid.tempo > 1000 && take.grid.tempo < 1065);
    auto followed = chop(ChopMode::Beats, 4, uint32_t(kicks[0] * 44100), 0, take.sample.frames, take.hits, take.count, 1200, &take.grid);
    CHECK(followed.count >= kicks.size() - 1);
    for (unsigned n = 1; n < followed.count && n < kicks.size(); ++n) CHECK(on_beat(followed.start[n], kicks[n]));

    // One kick: no beats, so Beats keeps the project's tempo.
    wave.assign(44100 * 2, 0.0f);
    add_kick(wave, 0.1);
    auto one = analysed(wave);
    CHECK(!one.grid.found() && one.grid.at.empty());
}

// A sample's tempo (engine/onsets.cpp) on the loops it read wrong against 1739 Apple Loops of known tempo: bass and
// guitar lines read at half their tempo, or a dotted eighth taken for the beat; and the tempo Follow plays a region
// at (shell/follow_tempo.h).
void loop_tempos() {
    auto tempo = [](const std::vector<float>& wave) {
        std::array<Hit, max_hits> hits {};
        BeatGrid grid;
        find_hits_in(wave, hits, grid);
        return grid.found() ? grid.tempo / 10.0 : 0.0;
    };
    // A plucked note (three harmonics, a 4 ms attack) and a held chord (a 30 ms attack).
    auto pluck = [](std::vector<float>& w, double at, double hz, double seconds, double level) {
        for (uint32_t n = uint32_t(at * 44100), end = std::min<uint32_t>(uint32_t(w.size()), n + uint32_t(seconds * 44100)); n < end; ++n) {
            double u = (n - at * 44100) / 44100, shape = std::min(1.0, u / 0.004) * std::exp(-u / 0.25) * std::min(1.0, (seconds - u) / 0.01);
            w[n] += float(level * shape * (std::sin(2 * M_PI * hz * u) + 0.4 * std::sin(4 * M_PI * hz * u) + 0.2 * std::sin(6 * M_PI * hz * u)));
        }
    };
    auto chord = [](std::vector<float>& w, double at, std::initializer_list<double> hz, double seconds, double level) {
        for (uint32_t n = uint32_t(at * 44100), end = std::min<uint32_t>(uint32_t(w.size()), n + uint32_t(seconds * 44100)); n < end; ++n) {
            double u = (n - at * 44100) / 44100, sum = 0;
            for (double f : hz) sum += std::sin(2 * M_PI * f * u);
            w[n] += float(level * std::min(1.0, u / 0.03) * std::min(1.0, (seconds - u) / 0.05) * sum / double(hz.size()));
        }
    };

    // Funk bass on 3 + 3 + 2 sixteenths twice a bar, 4 bars at 128 BPM: 128 exactly (read as 64 before), and cut
    // off its bars (from a third of a beat in, 11.3 beats long) within 1%.
    {
        const double beat = 60 / 128.0;
        std::vector<float> wave(size_t(std::lround(16 * beat * 44100)), 0.0f);
        const double roots[] = {55, 55, 82.4, 73.4};
        for (unsigned bar = 0; bar < 4; ++bar)
            for (double s : {0.0, 3.0, 6.0, 8.0, 11.0, 14.0}) pluck(wave, (bar * 16 + s) / 4 * beat, roots[bar], 0.18, 0.5);
        CHECK(tempo(wave) == 128.0);
        std::vector<float> cut(wave.begin() + std::lround(0.37 * beat * 44100), wave.begin() + std::lround(11.7 * beat * 44100));
        CHECK(std::fabs(tempo(cut) / 128 - 1) < 0.01);
    }
    // A bass line with long notes on 1 and 3 and short ones on the ands of 2 and 4, 2 bars at 120 and at 116 BPM:
    // its own tempo, not half (60 and 58 before).
    for (double bpm : {120.0, 116.0}) {
        const double beat = 60 / bpm;
        std::vector<float> wave(size_t(std::lround(8 * beat * 44100)), 0.0f);
        for (unsigned bar = 0; bar < 2; ++bar) {
            const double at = bar * 4 * beat;
            pluck(wave, at, 55, 1.4 * beat, 0.6);
            pluck(wave, at + 1.5 * beat, 65.4, 0.4 * beat, 0.5);
            pluck(wave, at + 2 * beat, 73.4, 1.9 * beat, 0.6);
            pluck(wave, at + 3.5 * beat, 82.4, 0.4 * beat, 0.4);
        }
        CHECK(tempo(wave) == bpm);
    }
    // Notes on sixteenths 1, 4, 7, 11 and 14 of each bar over a held chord, 4 bars at 115 BPM: 115, not the
    // dotted eighth's pulse (158 before).
    {
        const double beat = 60 / 115.0;
        std::vector<float> wave(size_t(std::lround(16 * beat * 44100)), 0.0f);
        for (unsigned bar = 0; bar < 4; ++bar) {
            for (double s : {0.0, 3.0, 6.0, 10.0, 13.0}) pluck(wave, (bar * 16 + s) / 4 * beat, 110 * (1 + (int(s) % 3) * 0.25), 0.15, 0.4);
            chord(wave, bar * 4 * beat, {220, 277, 330}, 3.8 * beat, 0.08);
        }
        CHECK(tempo(wave) == 115.0);
    }

    // Follow's tempo for a region (tenths of a BPM). Found: in the octave nearest the project's tempo, exact at
    // whole bars of the region. 2 bars at 70 BPM read as 140: 70 in a 90 BPM project, 140 in a 130 BPM one.
    const auto bars_at = [](double bars, double bpm) { return uint32_t(std::lround(bars * 4 * 60 * 44100 / bpm)); };
    CHECK(follow_tempo(bars_at(2, 70), 1400, 900) == 700 && follow_tempo(bars_at(2, 70), 1400, 1300) == 1400);
    // Found up to 4% off 4 bars (the region a few frames short of them, too): exact. 3 bars at 90 BPM: 90 (not 4
    // bars' 120), exact within 1.5%, else as found.
    CHECK(follow_tempo(bars_at(4, 128) - 300, 1279, 1200) == 1281);
    CHECK(follow_tempo(bars_at(4, 128), 1268, 1200) == 1280 && follow_tempo(bars_at(4, 128), 1240, 1200) == 1280);
    CHECK(follow_tempo(bars_at(3, 90), 902, 1200) == 900 && follow_tempo(bars_at(3, 90), 930, 1200) == 930);
    // Not whole bars (2.6 bars at 100 BPM) or 5% off them: the found tempo as it is; one and two beats are whole.
    CHECK(follow_tempo(bars_at(2.6, 100), 1000, 1200) == 1000 && follow_tempo(bars_at(4, 100), 1050, 1200) == 1050);
    CHECK(follow_tempo(bars_at(0.25, 100), 990, 1200) == 1000 && follow_tempo(bars_at(0.5, 100), 1010, 1200) == 1000);
    // None found: the whole number of bars nearest the project's tempo between 70 and 180 BPM, as before (4 s is 2
    // bars at 120, or 1 at 60, which is out); a hit under 0.1 s keeps its speed.
    CHECK(follow_tempo(44100 * 4, 0, 900) == 1200 && follow_tempo(44100 * 4, 0, 1700) == 1200);
    CHECK(follow_tempo(bars_at(2, 70), 0, 900) == 700 && follow_tempo(4000, 1200, 1200) == 0);
}


// Parity phase 7b: the MPC's Pad FX, four live slots, Flex Beat.
void pad_fx_and_flex() {
    // The model's defaults are the guide's initial values; banks: Melodee 1, Melodee 2 (+ Flex Beat's four), MPC.
    for (unsigned m = 0; m < 16; ++m)
        for (unsigned n = 0; n < punch_params; ++n) {
            const auto& pi = mpc_pad_fx(m).params[n];
            CHECK(pad_fx_defaults[m][n] == (pi.label ? pi.initial : 0));
        }
    CHECK(std::string(mpc_pad_fx(0).name) == "Half Speed" && std::string(mpc_pad_fx(15).name) == "Reverb");
    CHECK(punch_on_pad(2, 0) == int(PunchKind::HalfSpeed) && punch_on_pad(2, 15) == int(PunchKind::Reverb));
    CHECK(punch_on_pad(0, 3) == int(PunchKind::Loop32) && punch_on_pad(1, 10) == int(PunchKind::Vinyl));
    CHECK(punch_on_pad(1, 11) == int(PunchKind::Backspin) && punch_on_pad(1, 14) == int(PunchKind::Pump) && punch_on_pad(1, 15) == -1);
    CHECK(std::string(flex_beat(0).name) == "Empty");
    for (unsigned n = 1; n < flex_pads; ++n) CHECK(unsigned(flex_beat(n).kind) < punch_count);

    auto signal = [](unsigned n) { uint32_t h = n * 2654435761u; return (int32_t(h) >> 8) / 8388608.0f * 0.5f; };
    const double bar = 88200, even_bar = 88320; // the second a whole number of samples a sixteenth
    // Delay 1/4 of a bar: an impulse comes back 22050 samples later, then again quieter.
    {
        Punch punch;
        const int16_t params[] {3, 50, 0};
        punch.start(PunchKind::Delay, 1, bar, bar / 16, 0, params);
        std::vector<float> l(1), r(1), out;
        for (unsigned n = 0; n < 50000; ++n) { l[0] = r[0] = n == 10 ? 1.0f : 0.0f; punch.process(l.data(), r.data(), 1); out.push_back(l[0]); }
        CHECK(std::fabs(out[10] - 1) < 1e-6f && out[10 + 22050] > 0.6f && out[10 + 44100] > 0.2f && out[10 + 44100] < out[10 + 22050]);
    }
    // Beat Repeat 1/16 reversed: after the slice, it plays backwards.
    {
        Punch punch;
        const int16_t params[] {4, 1, 0};
        punch.start(PunchKind::BeatRepeat, 1, even_bar, even_bar / 16, 0, params);
        unsigned loop = unsigned(even_bar / 16);
        std::vector<float> l(1), r(1), out;
        for (unsigned n = 0; n < loop * 3; ++n) { l[0] = r[0] = signal(n); punch.process(l.data(), r.data(), 1); out.push_back(l[0]); }
        CHECK(std::fabs(out[loop + 100] - signal(loop - 101)) < 1e-4f);
    }
    // Rev Stepper: forwards, backwards, forwards ... then a new slice let through.
    {
        Punch punch;
        const int16_t params[] {4, 2, 0};
        punch.start(PunchKind::RevStepper, 1, even_bar, even_bar / 16, 0, params);
        unsigned loop = unsigned(even_bar / 16);
        std::vector<float> l(1), r(1), out;
        for (unsigned n = 0; n < loop * 5; ++n) { l[0] = r[0] = signal(n); punch.process(l.data(), r.data(), 1); out.push_back(l[0]); }
        CHECK(std::fabs(out[3 * loop + 500] - signal(3 * loop + 500)) < 1e-6f); // two plays, then the input again
    }
    // K1-K3 change a running punch: LoFi at 2 bits holds only a few levels.
    {
        Punch punch;
        punch.start(PunchKind::LoFi, 1, bar, bar / 16, 0);
        const int16_t two_bits[] {2, 0, 0};
        punch.set_params(two_bits);
        std::vector<float> l(256), r(256);
        for (unsigned n = 0; n < 256; ++n) l[n] = r[n] = signal(n);
        punch.process(l.data(), r.data(), 256);
        const unsigned in = unsigned(std::ceil(Punch::seam_seconds * dsp::sample_rate())); // it comes in over that first
        for (unsigned n = in; n < 256; ++n) CHECK(l[n] == 0 || std::fabs(std::fabs(l[n]) - 0.5f) < 1e-6f || std::fabs(std::fabs(l[n]) - 1) < 1e-6f);
    }

    // The host: four live slots at once, each let go on its own; Flex Beat's One Shot gives way to Empty after a bar.
    {
        auto host = std::make_unique<EngineHost>();
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        host->load(*project, *patterns);
        std::vector<int32_t> out(256);
        const PunchKind kinds[] {PunchKind::HalfSpeed, PunchKind::Chorus, PunchKind::Comb, PunchKind::Granulator};
        for (unsigned n = 0; n < 4; ++n) {
            Command on {Command::Type::PunchOn, 255, uint8_t(2 | 1 << 4), uint16_t(kinds[n]), 100};
            on.position = n + 1;
            host->send(on);
        }
        host->render(out.data(), 128, 1);
        CHECK(host->live_punches() == 0xf);
        Command off {Command::Type::PunchOff, 255};
        off.position = 2;
        host->send(off);
        host->render(out.data(), 128, 1);
        CHECK(host->live_punches() == 0xd);
        host->send({Command::Type::PunchParam, 0, 2, 11, 200}); // Granulator's grain length while it plays
        host->send({Command::Type::PunchOff, 254});
        host->render(out.data(), 128, 1);
        CHECK(host->live_punches() == 0 && !host->punching());
        host->send({Command::Type::FlexBeat, 0, 0, 3, 0}); // quantize off: at once
        host->send({Command::Type::FlexBeat, 0, 0, 0, 3});
        host->render(out.data(), 128, 1);
        CHECK(host->flex_playing() == 3);
        for (unsigned n = 0; n < 700; ++n) host->render(out.data(), 128, 1); // a bar at 120 is 88200 samples
        CHECK(host->flex_playing() == 0);
        host->send({Command::Type::FlexBeat, 0, 0, 1, 1}); // Loop
        host->send({Command::Type::FlexBeat, 0, 0, 0, 7});
        for (unsigned n = 0; n < 1400; ++n) host->render(out.data(), 128, 1);
        CHECK(host->flex_playing() == 7);
        host->send({Command::Type::FlexBeat, 0, 0, 0, 0}); // Empty
        host->render(out.data(), 128, 1);
        CHECK(host->flex_playing() == 0);
    }
    // The project file keeps K1-K3 and Flex Beat; older files (no PFXP) load the defaults; the MPC's bank is 2.
    {
        auto project = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        CHECK(project->punch_bank == 2);
        project->pad_fx_params[14] = {2, 80, 2};
        project->flex = {1, 60, false};
        auto bytes = save_project(*project, *patterns);
        auto back = std::make_unique<Project>();
        CHECK(load_project(bytes, *back, *patterns).empty());
        CHECK(back->pad_fx_params[14][1] == 80 && back->pad_fx_params[14][2] == 2 && back->flex.mode == 1 && back->flex.mix == 60 && !back->flex.quantize);
        for (size_t n = 0; n + 4 <= bytes.size(); ++n) if (std::memcmp(&bytes[n], "PFXP", 4) == 0) std::memcpy(&bytes[n], "XXXX", 4);
        auto old = std::make_unique<Project>();
        CHECK(load_project(bytes, *old, *patterns).empty() && old->pad_fx_params == pad_fx_defaults && old->flex.quantize);
    }
}

// MTC in (parity phase 9b): generated time code at each rate, read, settled, followed through jitter, a jump, a lost
// piece, Stop and running backwards; then the engine host placing the patterns and the song and keeping them on time.
struct Qf { uint8_t data; uint64_t ns; double seconds; };

// Quarter frames from frame `first` (hour 1, as a DAW starts) for `rounds` rounds of eight; `pace` above 1 runs fast.
std::vector<Qf> mtc_stream(MtcRate rate, uint32_t first, unsigned rounds, uint64_t start_ns, double pace = 1, uint32_t jitter_ns = 0) {
    std::vector<Qf> out;
    double fps = mtc_frames_a_second(rate);
    uint32_t noise = 777;
    for (unsigned r = 0; r < rounds; ++r) {
        unsigned m = 0, sec = 0, f = 0, h = 1;
        uint32_t code = first + 2 * r;
        mtc_code_of(code, rate, m, sec, f);
        h += code / uint32_t(std::lround(fps * 3600)); // past the hour (not reached here)
        uint8_t nib[8] {uint8_t(f & 15), uint8_t(f >> 4), uint8_t(sec & 15), uint8_t(sec >> 4), uint8_t(m & 15), uint8_t(m >> 4),
                        uint8_t(h & 15), uint8_t(((h >> 4) & 1) | unsigned(rate) << 1)};
        for (unsigned k = 0; k < 8; ++k) {
            double at = (code + k / 4.0) / fps; // where the master is as piece k goes out
            double since = (code - first + k / 4.0) / fps / pace;
            noise = noise * 1664525u + 1013904223u;
            uint64_t j = jitter_ns ? (noise >> 8) % jitter_ns : 0;
            out.push_back({uint8_t(k << 4 | nib[k]), start_ns + uint64_t(since * 1e9) + j, at});
        }
    }
    return out;
}

void mtc_in() {
    using K = MtcReader::Event::Kind;
    // Frame indexes and back; 29.97 drop-frame skips ;00 and ;01 each minute but every tenth.
    {
        unsigned m = 0, s = 0, f = 0;
        CHECK(mtc_frame_index(1, 0, 2, MtcRate::Fps2997Drop) == 1800 && mtc_frame_index(10, 0, 0, MtcRate::Fps2997Drop) == 17982);
        mtc_code_of(1799, MtcRate::Fps2997Drop, m, s, f); CHECK(m == 0 && s == 59 && f == 29);
        mtc_code_of(1800, MtcRate::Fps2997Drop, m, s, f); CHECK(m == 1 && s == 0 && f == 2);
        mtc_code_of(17982, MtcRate::Fps2997Drop, m, s, f); CHECK(m == 10 && s == 0 && f == 0);
        for (uint32_t n = 0; n < 107892; n += 7) {
            mtc_code_of(n, MtcRate::Fps2997Drop, m, s, f);
            CHECK(mtc_frame_index(m, s, f, MtcRate::Fps2997Drop) == n);
        }
        CHECK(mtc_frame_index(2, 3, 4, MtcRate::Fps24) == (2 * 60 + 3) * 24 + 4 && mtc_frame_index(0, 1, 0, MtcRate::Fps30) == 30);
    }
    // Each rate: settles within six frames, starts where the master is, then every quarter frame is a time sample.
    for (auto rate : {MtcRate::Fps24, MtcRate::Fps25, MtcRate::Fps2997Drop, MtcRate::Fps30}) {
        double fps = mtc_frames_a_second(rate);
        // From 0:59:59 (29.97 df: across the minute, where two labels are skipped), started on piece 3, 0.2 ms jitter.
        uint32_t first = mtc_frame_index(0, 59, 0, rate);
        auto stream = mtc_stream(rate, first, 200, 5000000000ull, 1, 200000);
        MtcReader reader;
        int started = -1, times = 0, others = 0;
        double worst = 0;
        for (size_t n = 3; n < stream.size(); ++n) {
            auto e = reader.quarter(stream[n].data, stream[n].ns);
            if (e.kind == K::Start) { started = int(n); CHECK(std::fabs(e.seconds - stream[n].seconds) < 1e-6); }
            else if (e.kind == K::Time) { ++times; worst = std::max(worst, std::fabs(e.seconds - stream[n].seconds)); }
            else if (e.kind != K::None) ++others;
        }
        CHECK(started > 0 && started <= 3 + 5 + 16 && (stream[size_t(started)].ns - stream[3].ns) / 1e9 < 6 / fps);
        CHECK(times == int(stream.size()) - started - 1 && others == 0 && worst < 1e-6 && reader.rate() == rate && reader.jumps() == 0);
        // Quarter frames stop: Stop once, 120 ms after the last.
        uint64_t last = stream.back().ns;
        CHECK(reader.idle(last + 100000000).kind == K::None);
        CHECK(reader.idle(last + 130000000).kind == K::Stop && !reader.running());
        CHECK(reader.idle(last + 140000000).kind == K::None);
    }
    // A jump while running: settles again, then Jump to the new place; a lost piece is no jump.
    {
        MtcReader reader;
        auto a = mtc_stream(MtcRate::Fps25, 250, 20, 1000000000ull);
        for (auto& q : a) reader.quarter(q.data, q.ns);
        CHECK(reader.running());
        auto b = mtc_stream(MtcRate::Fps25, 750, 20, a.back().ns + 10000000);
        int jumped = -1, times = 0;
        for (size_t n = 0; n < b.size(); ++n) {
            auto e = reader.quarter(b[n].data, b[n].ns);
            if (e.kind == K::Jump) { jumped = int(n); CHECK(std::fabs(e.seconds - b[n].seconds) < 1e-6 && e.seconds > 30); }
            if (e.kind == K::Time && jumped < 0) { ++times; CHECK(e.seconds < 12); } // before the new code: on from the old
            CHECK(e.kind != K::Stop && e.kind != K::Start);
        }
        CHECK(jumped > 8 && jumped < 24 && times == 7 && reader.jumps() == 1); // pieces 0-6 on from the old code
        auto c = mtc_stream(MtcRate::Fps25, 790, 10, b.back().ns + 10000000);
        bool stray = false;
        for (size_t n = 0; n < c.size(); ++n) {
            if (n == 13) continue; // lost
            auto e = reader.quarter(c[n].data, c[n].ns);
            stray |= e.kind != K::Time && e.kind != K::None;
            if (e.kind == K::Time) CHECK(std::fabs(e.seconds - c[n].seconds) < 1e-6);
        }
        CHECK(!stray && reader.jumps() == 1 && reader.running());
        // Running backwards: Stop.
        auto back = mtc_stream(MtcRate::Fps25, 900, 4, c.back().ns + 10000000);
        for (auto& q : back) reader.quarter(q.data, q.ns);
        CHECK(reader.running());
        CHECK(reader.quarter(back[30].data, back.back().ns + 10000000).kind == K::Stop && !reader.running());
        // A full frame while stopped is a cue; while running, the quarter frames after it place the song.
        auto cue = reader.full(1, 2, 30, 12, MtcRate::Fps25, back.back().ns + 300000000);
        CHECK(cue.kind == K::Cue && std::fabs(cue.seconds - (150 + 12 / 25.0)) < 1e-9);
        auto d = mtc_stream(MtcRate::Fps25, 3762, 6, back.back().ns + 500000000);
        int starts = 0;
        for (auto& q : d) starts += reader.quarter(q.data, q.ns).kind == K::Start;
        CHECK(starts == 1);
        auto moved = reader.full(1, 0, 10, 0, MtcRate::Fps25, d.back().ns + 5000000);
        CHECK(moved.kind == K::None && !reader.running());
        auto e2 = mtc_stream(MtcRate::Fps25, 250, 6, d.back().ns + 10000000);
        int jumps = 0;
        for (auto& q : e2) jumps += reader.quarter(q.data, q.ns).kind == K::Jump;
        CHECK(jumps == 1);
    }
    // The engine host: MTC places the patterns through the tempo playing, keeps them on time, catches a jump.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        project->tempo = 1200;
        put(bank->at[0][0], 4, 45, 100);
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.set_audio_format(44100, 384);
        std::vector<int32_t> out(256);
        uint64_t frame = 0;
        auto render = [&] { host.render(out.data(), 128, 1); frame += 128; };
        auto mtc = [&](uint16_t what, double seconds, uint64_t at) {
            Command c {Command::Type::MtcIn, 0, 0, what};
            c.position = uint32_t(at); c.micros = uint32_t(seconds * 1e6);
            host.send(c);
        };
        render();
        host.send({Command::Type::Play});
        mtc(1, 2.5, frame); // 2.5 s at 120 BPM: tick 4800, the bar's second beat (a block more: 5.6 ticks)
        render();
        CHECK(host.playing() && host.mtc_following() && host.ticks() >= 4800 && host.ticks() <= 4806 && host.position_of(0) >= 960 &&
              host.position_of(0) <= 966 && host.mtc_places() == 1);
        // A master 0.1 % fast with up to 1 ms of jitter: the song keeps on its time, trimmed about 1000 ppm.
        double start = 2.5;
        uint64_t from = frame - 128; // where 2.5 s was
        uint32_t noise = 99;
        for (unsigned n = 0; n < 3445 * 2; ++n) { // 20 s
            if (n % 2 == 0) {
                double master = start + double(frame - from) / 44100 * 1.001;
                noise = noise * 1664525u + 1013904223u;
                int64_t late = int64_t((noise >> 8) % 44) - 22; // the stamp up to half a millisecond either way
                mtc(2, master, uint64_t(int64_t(frame) + late));
            }
            render();
        }
        double master = start + double(frame - from) / 44100 * 1.001;
        double ticks = master * 2 * ticks_per_quarter;
        CHECK(std::fabs(double(host.ticks()) - ticks) < 20 && host.mtc_places() == 1);
        CHECK(host.mtc_trim_ppm() > 900 && host.mtc_trim_ppm() < 1100 && host.mtc_error_us() > 300 && host.mtc_error_us() < 700);
        // A sample far off is ignored; four in a row place it again.
        mtc(2, master + 5, frame); render();
        CHECK(host.mtc_places() == 1);
        for (unsigned n = 0; n < 4; ++n) { mtc(2, master + 5 + n * 128 / 44100.0, frame); render(); }
        CHECK(host.mtc_places() == 2 && std::fabs(double(host.ticks()) - (master + 5) * 2 * ticks_per_quarter) < 40);
        // A locate within 20 ms of where it is: no jump.
        mtc(1, master + 5 + 5 * 128 / 44100.0, frame); render();
        CHECK(host.mtc_places() == 2);
        // BPM per pattern: pattern 1 at 90 BPM leads, so 2 s is tick 2880.
        host.send({Command::Type::Stop});
        host.send({Command::Type::PatternTempo, 0, 0, 0, 900});
        host.send({Command::Type::SeqTempo, 0, 0, 0, 1});
        render();
        CHECK(!host.playing() && !host.mtc_following());
        host.send({Command::Type::Play});
        mtc(1, 2.0, frame);
        render();
        CHECK(host.ticks() >= 2880 && host.ticks() <= 2885);
        host.send({Command::Type::Stop});
        host.send({Command::Type::SeqTempo, 0, 0, 0, 0});
        // The song: bar 5 and a beat; then round its loop (bars 3-4); past its end (no loop) it stops.
        Clip clip; clip.track = 0; clip.pattern = 0; clip.bar = 0; clip.bars = 8;
        host.send([&] { Command c {Command::Type::SongClip}; c.index = 0; c.clip = clip; return c; }());
        host.send({Command::Type::SongCount, 0, 0, 0, 1});
        render();
        host.send({Command::Type::Play, 0, 0, 0, 1});
        mtc(1, 8.5, frame);
        render();
        CHECK(host.song_playing() && host.song_bar() == 4 && host.position_of(0) >= 960 && host.position_of(0) <= 966);
        for (unsigned n = 0; n < 600; ++n) render(); // into bar 6: three beats left of bar 5 (1.5 s), 1.74 s rendered
        CHECK(host.song_bar() == 5);
        host.send({Command::Type::Stop});
        host.send({Command::Type::SongLoop, 0, 0, 2, 4});
        host.send({Command::Type::Play, 0, 0, 0, 1});
        mtc(1, 10.0, frame); // bar 6 on the bar line: the loop's bar 3 (index 2 + (5 - 2) % 2 = 3)
        render();
        CHECK(host.song_bar() == 3 && host.position_of(0) >= 0 && host.position_of(0) <= 6);
        host.send({Command::Type::Stop});
        host.send({Command::Type::SongLoop, 0, 0, 0, 0});
        host.send({Command::Type::Play, 0, 0, 0, 1});
        mtc(1, 15.5, frame); // the last bar's last beat: the song ends by itself and MTC no longer leads
        render();
        CHECK(host.song_bar() == 7 && host.mtc_following());
        for (unsigned n = 0; n < 200; ++n) render();
        CHECK(!host.playing() && !host.mtc_following());
        host.send({Command::Type::Play, 0, 0, 0, 1});
        mtc(1, 20.5, frame); // bar 11 of an eight-bar song
        render(); render();
        CHECK(!host.playing() && !host.mtc_following());
        CHECK(host.dropped() == 0 && host.midi_dropped() == 0);
    }
}

// Settings' sample rate (44.1 / 48 / 96 kHz): the engine follows the device's rate, so a project plays at the same
// tempo and pitch at each, and what is recorded or rendered is kept at 44.1 kHz.
// Constants written a 44.1 kHz frame at a time follow the device's rate: at 96 kHz an effect keeps its timing, its
// corners and its density (dsp.h's fall_per_frame, step_per_frame, every_model_frames, kept), and at 44.1 kHz the
// constants are themselves.
void rate_constants() {
    dsp::device_rate.store(44100.0f);
    CHECK(dsp::fall_per_frame(0.93f) == 0.93f && dsp::step_per_frame(0.3f) == 0.3f && dsp::noise_level() == 1.0f);
    CHECK(dsp::fall_per_block(0.97f, 128) == 0.97f && dsp::rate_ratio().model == 1 && dsp::rate_ratio().device == 1);
    for (uint64_t k = 0; k < 5000; ++k) CHECK(dsp::every_model_frames(k, 12, dsp::rate_ratio()) == (k % 12 == 0));
    dsp::device_rate.store(96000.0f);
    {
        auto ratio = dsp::rate_ratio();
        CHECK(ratio.model == 147 && ratio.device == 320);
        unsigned ticks = 0; // every 4096 44.1 kHz frames: 107.7 times in ten seconds, the first on frame 0
        for (uint64_t k = 0; k < 960000; ++k) ticks += dsp::every_model_frames(k, 4096, ratio);
        CHECK(ticks == 108);
        const float fall = dsp::fall_per_frame(0.9999f); // a second of it falls as far as 44100 frames at 44.1 kHz
        float x = 1;
        for (unsigned n = 0; n < 96000; ++n) x *= fall;
        CHECK(std::fabs(x / std::pow(0.9999f, 44100.0f) - 1) < 0.01f);
    }
    // Each test below runs at 44.1 and at 96 kHz and compares the two in time.
    auto run = [](EffectKind kind, std::initializer_list<std::pair<unsigned, int>> params, float rate, float seconds, auto&& input) {
        dsp::device_rate.store(rate);
        DelayPool pool;
        auto effect = std::make_unique<Effect>();
        effect->set_kind(kind, pool);
        for (auto [index, value] : params) effect->set_param(index, value);
        std::vector<float> l(size_t(seconds * rate)), r;
        for (size_t n = 0; n < l.size(); ++n) l[n] = input(float(n) / rate);
        r = l;
        for (size_t f = 0; f < l.size(); f += 128) effect->process(l.data() + f, r.data() + f, unsigned(std::min<size_t>(128, l.size() - f)));
        effect->set_kind(EffectKind::None, pool);
        return l;
    };
    // Tremolo's square is smoothed (0.01 a 44.1 kHz frame): its edge falls from 90 % to 10 % in the same 5 ms.
    {
        double edge[2] {};
        for (unsigned k = 0; k < 2; ++k) {
            const float rate = k ? 96000.0f : 44100.0f;
            auto out = run(EffectKind::Tremolo, {{0, 90}, {1, 100}, {2, 1}}, rate, 0.05f, [](float) { return 0.5f; });
            size_t high = 0, low = 0;
            for (size_t n = 0; n < out.size() && !low; ++n) { if (!high && out[n] < 0.45f) high = n; if (high && out[n] < 0.05f) low = n; }
            edge[k] = double(low - high) / rate;
        }
        if (!(edge[0] > 0.004 && std::fabs(edge[1] / edge[0] - 1) < 0.03)) std::fprintf(stderr, "tremolo edge %g s / %g s\n", edge[0], edge[1]);
        CHECK(edge[0] > 0.004 && std::fabs(edge[1] / edge[0] - 1) < 0.03);
    }
    // Crush at RATE 100 holds each value 32 44.1 kHz frames: 1378 new values a second at either rate.
    // Pad FX Alias holds every twelfth 44.1 kHz frame: 3675 a second.
    {
        auto steps = [](const std::vector<float>& out, size_t from) {
            unsigned n = 0;
            for (size_t i = from + 1; i < out.size(); ++i) n += std::fabs(out[i] - out[i - 1]) > 1e-5f;
            return n;
        };
        auto ramp = [](float t) { return -0.9f + 0.9f * t; }; // never still: every hold is a new value
        unsigned crush[2], alias[2];
        for (unsigned k = 0; k < 2; ++k) {
            const float rate = k ? 96000.0f : 44100.0f;
            crush[k] = steps(run(EffectKind::Crush, {{0, 16}, {1, 100}, {2, 100}}, rate, 1.0f, ramp), 0);
            dsp::device_rate.store(rate);
            Punch punch;
            std::vector<float> l(static_cast<size_t>(rate)), r;
            for (size_t n = 0; n < l.size(); ++n) l[n] = ramp(float(n) / rate);
            r = l;
            punch.start(PunchKind::Alias, 1, 2 * rate, 2 * rate / 16, 1);
            for (size_t f = 0; f < l.size(); f += 128) punch.process(l.data() + f, r.data() + f, unsigned(std::min<size_t>(128, l.size() - f)));
            alias[k] = steps(l, size_t(0.01f * rate)); // after its 2 ms fade in
        }
        if (crush[0] < 1376 || crush[0] > 1380 || std::abs(int(crush[1]) - int(crush[0])) > 2 || std::abs(int(alias[1]) - int(alias[0])) > 2)
            std::fprintf(stderr, "crush %u / %u, alias %u / %u\n", crush[0], crush[1], alias[0], alias[1]);
        CHECK(crush[0] >= 1376 && crush[0] <= 1380 && std::abs(int(crush[1]) - int(crush[0])) <= 2);
        CHECK(alias[0] > 3600 && std::abs(int(alias[1]) - int(alias[0])) <= 2);
    }
    // Vinyl at CRACKLE 100: about 40 crackles a second at either rate (a chance taken each frame, kept to its share).
    {
        unsigned crackles[2];
        for (unsigned k = 0; k < 2; ++k) {
            const float rate = k ? 96000.0f : 44100.0f;
            auto out = run(EffectKind::Vinyl, {{0, 50}, {1, 100}, {2, 10}}, rate, 30.0f, [](float) { return 0.0f; });
            unsigned count = 0;
            size_t last = 0;
            for (size_t n = 0; n < out.size(); ++n)
                if (std::fabs(out[n]) > 0.02f) { if (!count || n - last > size_t(0.002f * rate)) ++count; last = n; }
            crackles[k] = count;
        }
        if (!(crackles[0] > 30 * 30 && crackles[0] < 45 * 30 && std::fabs(float(crackles[1]) / float(crackles[0]) - 1) < 0.15f))
            std::fprintf(stderr, "crackles %u / %u in 30 s\n", crackles[0], crackles[1]);
        CHECK(crackles[0] > 30 * 30 && crackles[0] < 45 * 30 && std::fabs(float(crackles[1]) / float(crackles[0]) - 1) < 0.15f);
    }
    dsp::device_rate.store(44100.0f);
}

// The note editors' printed buttons (model/note_edits.h): split, legato, reverse, quantize, ramp, arp to notes.
void note_edits() {
    const uint32_t s = sixteenth_ticks;
    auto note = [&](uint32_t step, uint8_t key, uint32_t steps, uint8_t velocity = 100) {
        Event e; e.tick = step * s; e.key = key; e.length = uint16_t(steps * s); e.velocity = velocity; return e;
    };
    { // Chop: a 4-step note 2 + 2, again 1 + 1 each; a 1-step note stays.
        std::vector<Event> n {note(0, 60, 4), note(8, 62, 1), note(12, 64, 3)};
        CHECK(split_events(n, s) == 2 && n.size() == 5);
        CHECK(n[0].tick == 0 && n[0].length == 2 * s && n[1].tick == 2 * s && n[1].length == 2 * s);
        CHECK(n[2].length == s);                                       // too short to split
        CHECK(n[3].tick == 12 * s && n[3].length == 2 * s && n[4].tick == 14 * s && n[4].length == s); // 3 steps: 2 + 1
        CHECK(split_events(n, s) == 3);
    }
    { // Legato: each to the next start; a kit by key.
        Pattern rest; rest.length = 16 * s;
        CHECK(rest.add(note(10, 70, 1)));
        std::vector<Event> n {note(0, 60, 1), note(4, 62, 1)};
        legato_events(n, rest, false);
        CHECK(n[0].length == 4 * s && n[1].length == 6 * s);
        std::vector<Event> k {note(0, 36, 1), note(4, 37, 1)};
        legato_events(k, rest, true);
        CHECK(k[0].length == 16 * s && k[1].length == 12 * s);          // no later hit of their own pad: the end
    }
    { // Reverse over the span they cover.
        std::vector<Event> n {note(0, 60, 1), note(2, 62, 2)};
        reverse_events(n);
        CHECK(n[0].tick == 3 * s && n[1].tick == 0);
    }
    { // Quantize: a quarter of the way each pass, at least a tick, onto the line.
        std::vector<Event> n {note(0, 60, 1)};
        n[0].tick = 4 * s + 8;
        quantize_events(n, s, 16 * s, 25); CHECK(n[0].tick == 4 * s + 6);
        for (int k = 0; k < 10; ++k) quantize_events(n, s, 16 * s, 25);
        CHECK(n[0].tick == 4 * s);
    }
    { // Velocity ramp, and read back.
        std::vector<Event> n {note(0, 60, 1, 100), note(1, 60, 1, 100), note(2, 60, 1, 100)};
        ramp_events(n, -50);
        CHECK(n[0].velocity == 100 && n[1].velocity == 75 && n[2].velocity == 50 && ramp_of(n) == -50);
    }
    { // Arp to notes: a C major chord a bar of 1/16 notes, up; two octaves; a 16 Levels lane by pitch.
        std::vector<Event> chord {note(0, 60, 4), note(0, 64, 4), note(0, 67, 4)};
        auto up = arp_events(chord, unsigned(ArpMode::Up), arp_default_rate, 1, 50, 16 * s, -1, 127);
        CHECK(up.size() == 4 && up[0].key == 60 && up[1].key == 64 && up[2].key == 67 && up[3].key == 60);
        CHECK(up[1].tick == s && up[0].length == s / 2);
        auto two = arp_events(chord, unsigned(ArpMode::Up), arp_default_rate, 2, 100, 16 * s, -1, 127);
        CHECK(two.size() == 4 && two[3].key == 72);
        std::vector<Event> pad {chord[0], chord[0]};
        pad[0].key = 40; pad[0].pitch = 0; pad[1].key = 40; pad[1].pitch = 7;
        auto lane = arp_events(pad, unsigned(ArpMode::Down), arp_default_rate, 1, 50, 16 * s, 40, 127);
        CHECK(lane.size() == 4 && lane[0].key == 40 && lane[0].pitch == 7 && lane[1].pitch == 0);
        CHECK(arp_events(chord, 0, arp_default_rate, 1, 50, 2 * s, -1, 127).size() == 2); // cut at the pattern's end
    }
    { // take and put: out in order, back in place.
        Pattern p; p.length = 16 * s;
        CHECK(p.add(note(0, 60, 1)) && p.add(note(4, 62, 1)) && p.add(note(8, 64, 1)));
        auto out = take_events(p, {2, 0});
        CHECK(out.size() == 2 && out[0].key == 60 && out[1].key == 64 && p.events.size() == 1);
        put_events(p, out);
        CHECK(p.events.size() == 3 && p.events[2].key == 64);
    }
}

// The Looper (docs/LOOPER.md): takes, quantize, overdub, feedback, undo, the transport, sources, the stretch,
// fitting, holding for a save, and its four tracks in the engine host's mix.
// The Looper at the device's rate: a take at 48 kHz keeps the frames as they came (no conversion), its memory
// made 64 s at that rate; at 96 kHz the loop plays on the beat from its own frames, an empty loop's memory is made
// again at 96 kHz and a take there is at 96 kHz; a 44.1 kHz loop (an older project's file) plays in time at 96 kHz.
void looper_rates() {
    auto lp = std::make_unique<Looper>();
    Looper& looper = *lp;
    float out_l[loop_tracks][looper::block], out_r[loop_tracks][looper::block];
    float src_l[looper::block] {}, src_r[looper::block] {};
    auto signal = [](uint64_t f) { return float(0.3 * std::sin(double(f) * 0.0137) + 0.1 * std::sin(double(f) * 0.29)); };
    double tick = 0;
    uint64_t frame = 0;
    bool restart = true;
    std::vector<float> heard;
    auto block = [&](float rate) {
        const double per = 960.0 * 240 / 60.0 / rate; // 240 BPM: a bar of 4/4 is a second
        Looper::Clock c;
        c.tick = tick; c.per_frame = per; c.bar = 3840; c.beat = 960; c.running = true; c.restart = restart; c.rate = rate;
        restart = false;
        Looper::Sources s;
        s.frames = frame ? looper::block : 0;
        s.at[looper::source_input] = {src_l, src_r};
        looper.run(c, s, looper::block, out_l, out_r);
        for (unsigned f = 0; f < looper::block; ++f) {
            heard.push_back(out_l[0][f]);
            src_l[f] = signal(frame + f); src_r[f] = -src_l[f];
        }
        frame += looper::block;
        tick += looper::block * per;
        looper.service();
    };
    dsp::device_rate.store(48000.0f);
    looper.ask(looper::Do::Enable, 0, 1);
    for (unsigned n = 0; n < 20; ++n) block(48000);
    CHECK(looper.buffers_out() == 2 * loop_tracks);
    // A bar at 48 kHz: 48000 frames of its own, the tempo right, and played back frame for frame a bar later.
    looper.ask(looper::Do::Length, 0, 1);
    while (tick < 3840 - 2 * looper::block * 960.0 * 4 / 48000) block(48000);
    looper.ask(looper::Do::Primary, 0, 0);
    while (tick < 7680 + 4 * looper::block * 960.0 * 4 / 48000) block(48000);
    auto v = looper.view(0);
    CHECK(v.state == Looper::State::Playing && v.length == 48000 && v.rate == 48000 && v.tempo == 2400 && !v.stretched);
    const uint64_t take_start = 48000; // bar 2's frame
    while (tick < 11520 - 4 * looper::block * 960.0 * 4 / 48000) block(48000);
    float worst = 0;
    for (uint64_t f = take_start + 48000 + 2000; f < take_start + 96000 - 2000; ++f)
        worst = std::max(worst, std::fabs(heard[f] - signal(f - 48000)));
    if (worst > 1e-6f) std::fprintf(stderr, "looper 48k: worst %g\n", double(worst));
    CHECK(worst <= 1e-6f); // the frames themselves: nothing converted on the way in or out
    // The device goes to 96 kHz (the same project kept): the 48 kHz loop plays on the beat, a bar a second, not
    // stretched; empty loops' memory is made again at 96 kHz and the loop's own stays.
    dsp::device_rate.store(96000.0f);
    size_t from = heard.size();
    for (unsigned n = 0; n < 750; ++n) block(96000); // a second
    v = looper.view(0);
    CHECK(v.length == 48000 && v.rate == 48000 && !v.stretched && looper.buffers_out() == 2 * loop_tracks);
    // What it plays at 96 kHz is its 48 kHz frames read half a frame a frame: frame g is the take's g / 2.
    double phase = 0; // the loop's place at the first frame heard here, in its frames
    {
        double t0 = tick - 750.0 * looper::block * 960.0 * 4 / 96000; // the tick at `from`
        phase = std::fmod(t0, 3840.0) / 3840.0 * 48000;
    }
    worst = 0;
    for (size_t g = 4000; g < 90000; g += 7) {
        double at = std::fmod(phase + g * 0.5, 48000.0);
        if (at < 200 || at > 47800) continue; // clear of the seam's fade
        // the take's frame `at` is the source's frame take_start + at (as looper_engine finds it a bar on)
        double x = at;
        float a = signal(uint64_t(std::floor(x)) + take_start), b = signal(uint64_t(std::floor(x)) + 1 + take_start);
        float want = a + (b - a) * float(x - std::floor(x));
        worst = std::max(worst, std::fabs(heard[from + g] - want));
    }
    if (worst > 0.02f) std::fprintf(stderr, "looper 48k at 96k: worst %g\n", double(worst));
    CHECK(worst <= 0.02f);
    // A take on loop 2 now is at 96 kHz: a bar is 96000 frames.
    looper.ask(looper::Do::Length, 1, 1);
    looper.ask(looper::Do::Primary, 1, 0);
    for (unsigned n = 0; n < 4 * 750; ++n) block(96000);
    v = looper.view(1);
    CHECK(v.state == Looper::State::Playing && v.length == 96000 && v.rate == 96000 && v.tempo == 2400 && !v.stretched);
    // A loop saved at 44.1 kHz (a project from before) comes back and plays a bar a second at 96 kHz.
    auto* buffer = new LoopBuffer(looper::frames_at(96000));
    for (unsigned f = 0; f < 44100; ++f) buffer->left[f] = buffer->right[f] = 0.25f;
    LoopLoad item; item.buffer = buffer; item.loop = 2; item.frames = 44100; item.rate = 44100; item.ticks = 3840; item.tempo = 2400;
    CHECK(looper.give_load(item));
    for (unsigned n = 0; n < 8; ++n) block(96000);
    v = looper.view(2);
    CHECK(v.state == Looper::State::Playing && v.length == 44100 && v.rate == 44100 && !v.stretched);
    looper.ask(looper::Do::Enable, 0, 0);
    for (unsigned n = 0; n < 8; ++n) block(96000);
    CHECK(looper.buffers_out() == 0);
    dsp::device_rate.store(44100.0f);
}

void looper_engine() {
    constexpr double bpm = 240, per = 960.0 * bpm / 60.0 / 44100.0; // ticks a frame; a bar of 4/4 is one second
    constexpr unsigned bar_frames = 44100;
    auto signal = [](uint64_t f) { return float(0.3 * std::sin(double(f) * 0.0137) + 0.1 * std::sin(double(f) * 0.29)); };
    auto lp = std::make_unique<Looper>();
    Looper& looper = *lp;
    float out_l[loop_tracks][looper::block], out_r[loop_tracks][looper::block];
    float src_l[looper::block] {}, src_r[looper::block] {}, mix_l[looper::block] {}, mix_r[looper::block] {};
    uint64_t frame = 0;     // frames run so far
    double tick = 0;
    bool running = true, restart = true;
    std::vector<float> heard; // loop 1's output, every frame
    float constant = -1;      // >= 0: the source plays this instead of the signal
    // One block: the source played the block before (as the host keeps it), then the looper.
    auto block = [&](unsigned frames = looper::block) {
        Looper::Clock c;
        c.tick = tick; c.per_frame = per; c.bar = 3840; c.beat = 960;
        c.running = running; c.restart = restart; c.rate = 44100;
        restart = false;
        Looper::Sources s;
        s.frames = frame ? frames : 0;
        s.at[looper::source_input] = {src_l, src_r};
        s.at[looper::source_mix] = {mix_l, mix_r};
        looper.run(c, s, frames, out_l, out_r);
        for (unsigned f = 0; f < frames; ++f) {
            heard.push_back(out_l[0][f]);
            float x = constant >= 0 ? constant : signal(frame + f);
            src_l[f] = x; src_r[f] = -x;
            mix_l[f] = out_l[0][f] + 0.5f; mix_r[f] = out_r[0][f] + 0.5f; // what the "mix" played: the loop and a level
        }
        frame += frames;
        if (running) tick += frames * per;
        looper.service();
    };
    auto run_frames = [&](uint64_t frames) { for (uint64_t n = 0; n < frames / looper::block; ++n) block(); };

    // Off: no memory, nothing recorded.
    looper.ask(looper::Do::Primary, 0, 0);
    block();
    CHECK(looper.view(0).state == Looper::State::Empty && !looper.view(0).ready);
    // On: the service thread makes two buffers a loop.
    looper.ask(looper::Do::Enable, 0, 1);
    for (unsigned n = 0; n < 20; ++n) block();
    CHECK(looper.enabled_now() && looper.buffers_out() == 2 * loop_tracks);
    for (unsigned n = 0; n < loop_tracks; ++n) CHECK(looper.view(n).ready);

    // A take of one bar on the bar: Armed, then Recording on bar 2's downbeat, closed one bar later, then playing.
    looper.ask(looper::Do::Length, 0, 1);
    while (tick < 3840 - 2 * looper::block * per) block();
    looper.ask(looper::Do::Primary, 0, 0);
    block();
    CHECK(looper.view(0).state == Looper::State::Armed && looper.view(0).pending == Looper::Pending::Start);
    while (tick < 3840 + 10 * looper::block * per) block();
    CHECK(looper.view(0).state == Looper::State::Recording && looper.armed() == false && looper.recording());
    uint64_t take_start = uint64_t(std::llround(3840 / per)); // the frame of bar 2
    while (tick < 7680 + 4 * looper::block * per) block();
    auto v = looper.view(0);
    CHECK(v.state == Looper::State::Playing && v.length == bar_frames && std::fabs(v.ticks - 3840) < 1e-6 && v.tempo == 2400 && v.layers == 1);
    // It plays what was played in its bar, on the beat: frame F of bar 3 is the frame a bar before.
    while (tick < 11520 - 4 * looper::block * per) block();
    for (uint64_t f = take_start + bar_frames + 2000; f < take_start + 2 * bar_frames - 2000; f += 997)
        CHECK(std::fabs(heard[f] - signal(f - bar_frames)) < 2e-3f);
    // The waveform was drawn again from the take.
    unsigned lit = 0;
    for (unsigned c = 0; c < looper::peak_columns; ++c) lit += looper.peak(0, c) > 20;
    CHECK(lit > looper::peak_columns * 3 / 4);

    // Overdub a constant for a bar (feedback 100 %): old layers + 0.25; undo takes it back, redo brings it.
    constant = 0.25f;
    block();
    looper.ask(looper::Do::Primary, 0, 0);
    block(); block(); // the writer starts a block (the source's lag) after the press
    CHECK(looper.view(0).state == Looper::State::Overdub && looper.view(0).layers == 2);
    run_frames(bar_frames + 4 * looper::block);
    looper.ask(looper::Do::Primary, 0, 0);
    run_frames(8 * looper::block);
    v = looper.view(0);
    CHECK(v.state == Looper::State::Playing && v.undo == looper::Undo::Undo && v.layers == 2);
    uint64_t at = frame + 1000;
    run_frames(2000);
    CHECK(std::fabs(heard[at] - (signal(at - 3 * bar_frames) + 0.25f)) < 0.01f);
    looper.ask(looper::Do::Undo, 0, 0);
    run_frames(30 * looper::block); // swapped a chunk a block
    v = looper.view(0);
    CHECK(v.undo == looper::Undo::Redo && v.layers == 1);
    at = frame + 1000;
    run_frames(2000);
    CHECK(std::fabs(heard[at] - signal(at - (at - take_start) / bar_frames * bar_frames)) < 0.01f);
    looper.ask(looper::Do::Undo, 0, 0);
    run_frames(30 * looper::block);
    CHECK(looper.view(0).undo == looper::Undo::Undo && looper.view(0).layers == 2);
    // Feedback 0: an overdub pass replaces what it covers.
    looper.ask(looper::Do::Feedback, 0, 0);
    looper.ask(looper::Do::Primary, 0, 0);
    run_frames(bar_frames + 8 * looper::block);
    looper.ask(looper::Do::Primary, 0, 0);
    run_frames(8 * looper::block);
    at = frame + 500;
    run_frames(1000);
    CHECK(std::fabs(heard[at] - 0.25f) < 0.01f);
    looper.ask(looper::Do::Feedback, 0, 100);

    // Stop at the next bar (and cancel), then at once; Now plays it again.
    looper.ask(looper::Do::Toggle, 0, 0);
    CHECK(looper.view(0).pending == Looper::Pending::None); // not until the next block's view
    block();
    CHECK(looper.view(0).pending == Looper::Pending::Stop);
    looper.ask(looper::Do::Toggle, 0, 0);
    block();
    CHECK(looper.view(0).pending == Looper::Pending::None && looper.view(0).state == Looper::State::Playing);
    looper.ask(looper::Do::Halt, 0, 0);
    run_frames(2 * looper::block);
    CHECK(looper.view(0).state == Looper::State::Stopped && !looper.view(0).sounding);
    looper.ask(looper::Do::Now, 0, 0);
    run_frames(2 * looper::block);
    CHECK(looper.view(0).state == Looper::State::Playing && looper.view(0).sounding);

    // The transport stops: loops hold silent; a press on an empty loop arms; Play lines up and starts the take on bar 1.
    running = false;
    run_frames(4 * looper::block);
    CHECK(!looper.view(0).sounding && looper.view(0).state == Looper::State::Playing);
    looper.ask(looper::Do::Length, 1, 0);
    looper.ask(looper::Do::Source, 1, looper::source_mix);
    looper.ask(looper::Do::Primary, 1, 0);
    run_frames(4 * looper::block);
    CHECK(looper.view(1).state == Looper::State::Armed);
    running = true; restart = true; tick = 0;
    block();
    CHECK(looper.view(1).state == Looper::State::Recording && looper.view(0).sounding);
    CHECK(looper.view(0).position < 2 * looper::block); // loop 1 from its start
    // A free take closes on the next bar after the press.
    run_frames(bar_frames / 2);
    looper.ask(looper::Do::Primary, 1, 0);
    block();
    CHECK(looper.view(1).pending == Looper::Pending::Close);
    run_frames(bar_frames);
    v = looper.view(1);
    CHECK(v.state == Looper::State::Playing && v.length == bar_frames && std::fabs(v.ticks - 3840) < 1e-6);
    // The source is fixed while it records; never itself.
    looper.ask(looper::Do::Source, 2, looper::source_loop(2));
    block();
    CHECK(looper.view(2).source == looper::source_input && looper.view(2).errors == 1);
    // Takes close on whole bars from their start. Free, pressed: the nearest bar (one just passed drops what ran past
    // it, one ahead waits for its end); a set length plays out to its last bar whatever is pressed; Stop now keeps
    // the whole bars it has.
    {
        auto take = [&](unsigned bars) {
            looper.ask(looper::Do::Length, 2, int(bars));
            looper.ask(looper::Do::Primary, 2, 0);
            block();
            while (looper.view(2).state != Looper::State::Recording) block();
        };
        auto clear = [&] {
            looper.ask(looper::Do::Clear, 2, 0);
            run_frames(16 * looper::block);
            CHECK(looper.view(2).state == Looper::State::Empty);
        };
        take(0);
        run_frames(bar_frames * 5 / 4);
        looper.ask(looper::Do::Primary, 2, 0);
        block();
        v = looper.view(2);
        CHECK(v.state == Looper::State::Playing && v.length == bar_frames && std::fabs(v.ticks - 3840) < 1e-6);
        clear();
        take(0);
        run_frames(bar_frames * 17 / 10);
        looper.ask(looper::Do::Primary, 2, 0);
        block();
        CHECK(looper.view(2).state == Looper::State::Recording && looper.view(2).pending == Looper::Pending::Close);
        run_frames(bar_frames / 2);
        v = looper.view(2);
        CHECK(v.state == Looper::State::Playing && v.length == 2 * bar_frames && std::fabs(v.ticks - 7680) < 1e-6);
        clear();
        take(0);
        run_frames(bar_frames / 4);
        looper.ask(looper::Do::Primary, 2, 0); // under half a bar: still the first bar
        run_frames(bar_frames);
        v = looper.view(2);
        CHECK(v.state == Looper::State::Playing && v.length == bar_frames);
        clear();
        take(2);
        run_frames(bar_frames / 2);
        looper.ask(looper::Do::Primary, 2, 0);
        block();
        CHECK(looper.view(2).state == Looper::State::Recording);
        run_frames(bar_frames * 3 / 2);
        v = looper.view(2);
        CHECK(v.state == Looper::State::Playing && v.length == 2 * bar_frames && std::fabs(v.ticks - 7680) < 1e-6);
        // Armed with that two-bar loop playing (the longest), a take waits for its next start, not the next bar.
        looper.ask(looper::Do::Primary, 3, 0);
        run_frames(bar_frames * 3 / 2);
        CHECK(looper.view(3).state == Looper::State::Armed && looper.view(3).pending == Looper::Pending::Start);
        run_frames(bar_frames * 3 / 5);
        CHECK(looper.view(3).state == Looper::State::Recording);
        looper.ask(looper::Do::Clear, 3, 0);
        run_frames(16 * looper::block);
        CHECK(looper.view(3).state == Looper::State::Empty);
        clear();
        take(0);
        run_frames(bar_frames * 7 / 5);
        looper.ask(looper::Do::Halt, 2, 0);
        block();
        v = looper.view(2);
        CHECK(v.state == Looper::State::Stopped && v.length == bar_frames && std::fabs(v.ticks - 3840) < 1e-6);
        clear();
        looper.ask(looper::Do::Length, 2, 0);
    }

    // Another tempo: loop 1 stays on the beat and keeps its pitch (stretched), then Push fits it and overdubs.
    constexpr double faster = 960.0 * 300 / 60.0 / 44100.0;
    auto block_at = [&](double p) {
        Looper::Clock c; c.tick = tick; c.per_frame = p; c.running = true; c.rate = 44100;
        Looper::Sources s; s.frames = looper::block; s.at[looper::source_input] = {src_l, src_r};
        looper.run(c, s, looper::block, out_l, out_r);
        tick += looper::block * p;
        looper.service();
    };
    for (unsigned n = 0; n < 200; ++n) block_at(faster);
    CHECK(looper.view(0).stretched);
    float energy = 0;
    for (unsigned n = 0; n < 100; ++n) { block_at(faster); for (unsigned f = 0; f < looper::block; ++f) energy += out_l[0][f] * out_l[0][f]; }
    CHECK(energy > 1.0f);
    looper.ask(looper::Do::Primary, 0, 0);
    block_at(faster);       // the service thread fits it
    block_at(faster);       // and the audio thread swaps it in, then punches in
    v = looper.view(0);
    CHECK(!v.fitting && !v.stretched && v.length == 35280 && v.tempo == 3000 && v.state == Looper::State::Overdub && v.undo == looper::Undo::None);
    looper.ask(looper::Do::Primary, 0, 0);
    for (unsigned n = 0; n < 10; ++n) block_at(faster);
    // A set length too long for a loop's 64 s at this tempo (16 bars at 40 BPM: 96 s) gets memory made to its length:
    // Growing until the service thread has made it, then it arms; a shorter length gives it back.
    looper.ask(looper::Do::Clear, 2, 0);
    looper.ask(looper::Do::Length, 2, 16);
    Looper::Clock slow; slow.tick = tick; slow.per_frame = 960.0 * 40 / 60.0 / 44100.0; slow.running = true; slow.rate = 44100;
    Looper::Sources none;
    looper.run(slow, none, looper::block, out_l, out_r);
    const unsigned out_before = looper.buffers_out();
    looper.ask(looper::Do::Primary, 2, 0);
    looper.run(slow, none, looper::block, out_l, out_r);
    CHECK(looper.view(2).state == Looper::State::Empty && looper.view(2).error == looper::Error::Growing);
    looper.service();
    CHECK(looper.buffers_out() == out_before + 2);
    looper.run(slow, none, looper::block, out_l, out_r);
    looper.service(); // the two 64 s ones it held go back
    CHECK(looper.buffers_out() == out_before);
    looper.ask(looper::Do::Primary, 2, 0);
    looper.run(slow, none, looper::block, out_l, out_r);
    CHECK(looper.view(2).state == Looper::State::Armed);
    looper.ask(looper::Do::Primary, 2, 0); // not now
    // A hair slower (as MTC or MIDI clock in moves it): the memory made stays, rounded up a second, and arms at once.
    Looper::Clock slower = slow; slower.per_frame *= 0.9999;
    for (unsigned n = 0; n < 3; ++n) { looper.run(slower, none, looper::block, out_l, out_r); looper.service(); }
    CHECK(looper.buffers_out() == out_before);
    looper.ask(looper::Do::Primary, 2, 0);
    looper.run(slower, none, looper::block, out_l, out_r);
    CHECK(looper.view(2).state == Looper::State::Armed);
    looper.ask(looper::Do::Primary, 2, 0);
    looper.ask(looper::Do::Length, 2, 0);
    looper.run(slow, none, looper::block, out_l, out_r);
    looper.service(); looper.service();
    for (unsigned n = 0; n < 4; ++n) { looper.run(slow, none, looper::block, out_l, out_r); looper.service(); }
    CHECK(looper.view(2).state == Looper::State::Empty && looper.view(2).ready && looper.buffers_out() == out_before);
    // A longer length set while armed: the memory made for it goes in while it waits (it records into it, not 64 s).
    looper.ask(looper::Do::Length, 2, 1);
    looper.ask(looper::Do::Primary, 2, 0);
    looper.run(slow, none, looper::block, out_l, out_r);
    CHECK(looper.view(2).state == Looper::State::Armed);
    looper.ask(looper::Do::Length, 2, 16);
    looper.service();
    CHECK(looper.buffers_out() == out_before + 2);
    looper.run(slow, none, looper::block, out_l, out_r);
    looper.service();
    CHECK(looper.view(2).state == Looper::State::Armed && looper.buffers_out() == out_before); // the 64 s pair went back
    looper.ask(looper::Do::Primary, 2, 0);
    looper.ask(looper::Do::Length, 2, 0);
    for (unsigned n = 0; n < 4; ++n) { looper.run(slow, none, looper::block, out_l, out_r); looper.service(); }
    // 128 bars, as long as a pattern, is a length to set.
    looper.ask(looper::Do::Length, 2, 200);
    looper.run(slow, none, looper::block, out_l, out_r);
    CHECK(looper.view(2).bars == 128);
    looper.ask(looper::Do::Length, 2, 0);

    // Held for a save: granted once nothing writes it; a clear waits until it is let go.
    CHECK(!looper.hold(1));
    block_at(faster);
    CHECK(looper.hold(1) && looper.held_left(1) && looper.view(1).held);
    looper.ask(looper::Do::Clear, 1, 0);
    for (unsigned n = 0; n < 4; ++n) block_at(faster);
    CHECK(looper.view(1).state == Looper::State::Playing);
    looper.release(1);
    for (unsigned n = 0; n < 8; ++n) block_at(faster);
    CHECK(looper.view(1).state == Looper::State::Empty && !looper.view(1).held);

    // A saved loop comes back through the service thread, playing.
    auto* buffer = new LoopBuffer(looper::frames_at(44100));
    for (unsigned f = 0; f < 1000; ++f) buffer->left[f] = buffer->right[f] = 0.5f;
    LoopLoad item; item.buffer = buffer; item.loop = 3; item.frames = 1000; item.ticks = 960; item.tempo = 2400;
    CHECK(looper.give_load(item));
    for (unsigned n = 0; n < 4; ++n) block_at(per);
    v = looper.view(3);
    CHECK(v.state == Looper::State::Playing && v.length == 1000 && v.tempo == 2400 && looper.buffers_out() == 2 * loop_tracks);
    item.buffer = new LoopBuffer(looper::frames_at(44100)); item.stopped = true; // saved stopped: back stopped
    CHECK(looper.give_load(item));
    for (unsigned n = 0; n < 4; ++n) block_at(per);
    CHECK(looper.view(3).state == Looper::State::Stopped && looper.buffers_out() == 2 * loop_tracks);

    // Off: every loop goes, the memory goes back and is freed.
    looper.ask(looper::Do::Enable, 0, 0);
    for (unsigned n = 0; n < 4; ++n) block_at(per);
    CHECK(!looper.enabled_now() && looper.buffers_out() == 0);
    for (unsigned n = 0; n < loop_tracks; ++n) CHECK(looper.view(n).state == Looper::State::Empty);

    // In the engine host: L1 records track 1 (a PROPHET note), plays into its own track at its level, muted by its strip.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (auto& t : project->tracks) t.engine = EngineKind::None;
    synth(project->tracks[0], 0);
    project->looper.on = true;
    project->looper.loops[0].source = looper::source_track(0);
    project->looper.loops[0].bars = 1;
    project->tempo = 2400;
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    tones_for(*host, *project);
    std::vector<int32_t> out(256);
    auto render = [&](unsigned blocks) { for (unsigned n = 0; n < blocks; ++n) { host->render(out.data(), 128, 1); host->looper().service(); } };
    render(20);
    CHECK(host->looper().view(0).ready);
    host->send({Command::Type::Play, 0, 0, 0, 0});
    host->send({Command::Type::Looper, 0, 0, uint16_t(looper::Do::Primary)});
    host->send({Command::Type::NoteOn, 0, 0, 60, 100});
    render(4);
    CHECK(host->looper().view(0).state == Looper::State::Recording);
    render(bar_frames / 128 + 4);
    host->send({Command::Type::NoteOff, 0, 0, 60});
    host->send({Command::Type::Level, 0, 0, 0, 0}); // track 1 silent: what is heard now is the loop
    render(bar_frames / 128 / 2);
    CHECK(host->looper().view(0).state == Looper::State::Playing);
    float loud = 0;
    for (unsigned n = 0; n < 50; ++n) { render(1); for (auto x : out) loud = std::max(loud, std::fabs(x / 2147483648.0f)); }
    host->send({Command::Type::Mute, uint8_t(track_count), 0, 0, 1});
    render(4);
    loud = 0;
    for (unsigned n = 0; n < 50; ++n) { render(1); for (auto x : out) loud = std::max(loud, std::fabs(x / 2147483648.0f)); }
    CHECK(loud < 1e-4f);
    // Loops on hardware inputs of their own, at once: L2 the rear right input, L3 USB-C's left. Monitor On hears
    // them through their tracks; then both takes play back with Monitor Off.
    {
        std::vector<int32_t> adc(256);
        std::vector<float> usb(256);
        for (unsigned f = 0; f < 128; ++f) { adc[2 * f] = int32_t(0.25 * 2147483648.0); adc[2 * f + 1] = int32_t(0.5 * 2147483648.0); usb[2 * f] = 0.125f; usb[2 * f + 1] = 0.75f; }
        auto heard = [&](unsigned blocks) {
            float most = 0;
            for (unsigned n = 0; n < blocks; ++n) {
                host->render(out.data(), 128, 1, adc.data(), nullptr, usb.data());
                host->looper().service();
                if (n + 8 >= blocks) for (auto x : out) most = std::max(most, std::fabs(x / 2147483648.0f));
            }
            return most;
        };
        CHECK(looper::source_of(InputSource::RearR) == looper::source_direct(2) && looper::direct_input(looper::source_of(InputSource::UsbL)) == InputSource::UsbL);
        host->send({Command::Type::Looper, 1, 0, uint16_t(looper::Do::Source), int16_t(looper::source_of(InputSource::RearR))});
        host->send({Command::Type::Looper, 2, 0, uint16_t(looper::Do::Source), int16_t(looper::source_of(InputSource::UsbL))});
        host->send({Command::Type::Looper, 1, 0, uint16_t(looper::Do::Length), 1});
        host->send({Command::Type::Looper, 2, 0, uint16_t(looper::Do::Length), 1});
        host->send({Command::Type::LoopMonitor, 0, 0, 0, int16_t(Monitor::On)});
        loud = heard(16);
        CHECK(loud > 0.55f && loud < 0.7f); // 0.5 and 0.125, each centred
        host->send({Command::Type::LoopMonitor, 0, 0, 0, int16_t(Monitor::Off)});
        CHECK(heard(16) < 1e-4f);
        host->send({Command::Type::LoopMonitor, 0, 0, 0, int16_t(Monitor::Auto)});
        host->send({Command::Type::Looper, 1, 0, uint16_t(looper::Do::Primary)});
        host->send({Command::Type::Looper, 2, 0, uint16_t(looper::Do::Primary)});
        heard(1);
        CHECK(host->looper().view(1).state == Looper::State::Armed && host->looper().view(2).state == Looper::State::Armed);
        loud = heard(8);
        CHECK(loud > 0.55f); // Auto: heard while armed
        heard(3 * bar_frames / 128);
        auto two = host->looper().view(1), three = host->looper().view(2);
        CHECK(two.state == Looper::State::Playing && two.length == bar_frames && three.state == Looper::State::Playing && three.length == bar_frames);
        host->send({Command::Type::LoopMonitor, 0, 0, 0, int16_t(Monitor::Off)});
        std::fill(adc.begin(), adc.end(), 0); std::fill(usb.begin(), usb.end(), 0.0f);
        loud = heard(bar_frames / 128 / 2);
        CHECK(loud > 0.55f && loud < 0.7f); // what each took, played back
    }
    // The project keeps the Looper: its settings, each loop's values and saved audio's length, L1-L4's strips and chains.
    {
        Project saved;
        PatternBank no_patterns;
        saved.looper.on = true; saved.looper.quantize = 1; saved.looper.then = 2; saved.looper.transport = 1;
        saved.looper.loops[2] = LoopTrack {4, 70, looper::source_mix, 176400, 15362.5, 1200, 3, true};
        saved.looper.loops[1].source = looper::source_loop(1); // itself: never (read back as the input)
        saved.loop_mix[3].level = 90; saved.loop_mix[3].pan = -30; saved.loop_mix[3].send = 40; saved.loop_mix[3].bus = 2; saved.loop_mix[3].mute = true;
        saved.loop_fx[3].slots[0].kind = uint8_t(EffectKind::Echo);
        Project back;
        PatternBank back_patterns;
        CHECK(load_project(save_project(saved, no_patterns), back, back_patterns).empty());
        const auto& l = back.looper.loops[2];
        CHECK(back.looper.on && back.looper.quantize == 1 && back.looper.then == 2 && back.looper.transport == 1);
        CHECK(l.bars == 4 && l.feedback == 70 && l.source == looper::source_mix && l.frames == 176400 && std::fabs(l.ticks - 15362.5) < 1e-4 && l.tempo == 1200 && l.layers == 3 && l.stopped);
        CHECK(back.looper.loops[1].source == looper::source_input);
        const auto& m = back.loop_mix[3];
        CHECK(m.level == 90 && m.pan == -30 && m.send == 40 && m.bus == 2 && m.mute && back.loop_fx[3].count() == 1);
        CHECK(back.chain(loop_chain(3)).slots[0].kind == uint8_t(EffectKind::Echo) && is_loop_chain(loop_chain(0)) && !is_loop_chain(master_chain));
        Project plain_project;
        CHECK(load_project(save_project(plain_project, no_patterns), back, back_patterns).empty() && !back.looper.on);
    }
    // Stop: the loop holds silent.
    host->send({Command::Type::Mute, uint8_t(track_count), 0, 0, 0});
    host->send({Command::Type::Stop});
    render(8);
    CHECK(!host->looper().view(0).sounding);
}

// The SFZ reader, as melodee-ios's: its levels, comments, defines, includes and traps.
void sfz_reader() {
    using S = std::vector<std::string>;
    auto samples = [](const SfzRead& r) { S out; for (auto& g : r.regions) out.push_back(g.sample); return out; };
    {
        auto read = read_sfz(
            "// a comment\n/* a block\n   of comment */\n#define $LOW 36\n"
            "<control> default_path=Samples\\Keys\\ note_offset=0\n"
            "<global> ampeg_release=0.5 ampeg_attack=0.01 ampeg_sustain=80 ampeg_decay=1\n"
            "<group> lovel=1 hivel=64 volume=-6 seq_length=2\n"
            "<region> sample=C2 soft.wav lokey=$LOW hikey=47 pitch_keycenter=c2 tune=-5 seq_position=1 loop_mode=loop_continuous loop_start=100 loop_end=199\n"
            "<region> sample=C2 soft 2.wav lokey=36 hikey=47 pitch_keycenter=36 seq_position=2 pan=-50 bend_up=200\n"
            "<group> lovel=65\n"
            "<region> sample=C4 loud.wav key=c#4 volume=3 transpose=-1 offset=48 end=4799 loop_mode=one_shot group=2 off_by=3 pitch_keytrack=0\n"
            "<region> sample=*sine key=70\n<region> sample=gone.wav hikey=-1\n");
        CHECK(read.regions.size() == 3 && read.left == 2);
        CHECK(read.ignored == std::set<std::string> {"bend_up"});
        const auto& soft = read.regions[0], &second = read.regions[1], &loud = read.regions[2];
        CHECK(soft.sample == "Samples/Keys/C2 soft.wav" && second.sample == "Samples/Keys/C2 soft 2.wav");
        CHECK(soft.zone.low_key == 36 && soft.zone.high_key == 47 && soft.zone.root == 36);
        CHECK(soft.zone.low_velocity == 1 && soft.zone.high_velocity == 64 && soft.zone.gain == -6 && soft.zone.tune == -5);
        CHECK(soft.zone.sequence_length == 2 && soft.zone.sequence == 1 && second.zone.sequence == 2 && second.zone.pan == -0.5f);
        CHECK(soft.zone.loop == ZoneLoop::On && soft.zone.loop_start == 100 && soft.zone.loop_end == 200);
        CHECK(loud.zone.low_key == 61 && loud.zone.high_key == 61 && loud.zone.root == 61 && loud.zone.tune == -100 && loud.zone.gain == 3);
        CHECK(loud.zone.low_velocity == 65 && loud.zone.high_velocity == 127 && loud.zone.one_shot && loud.zone.group == 2 && loud.zone.off_by == 3);
        CHECK(loud.zone.keytrack == 0 && loud.offset == 48 && loud.end == 4800);
        CHECK(read.envelope.attack == 0.01 && read.envelope.decay == 1.0 && read.envelope.sustain == 80.0 && read.envelope.release == 0.5);
    }
    CHECK(sfz_note("60") == 60 && sfz_note("c4") == 60 && sfz_note("C4") == 60 && sfz_note("c#4") == 61 && sfz_note("db4") == 61);
    CHECK(sfz_note("a-1") == 9 && sfz_note("b3") == 59 && !sfz_note("zz") && !sfz_note("c"));
    {
        auto read = read_sfz("<group>hivel=50\n#include \"more.sfz\"\n<region>sample=c.wav", [](const std::string& name) {
            return name == "more.sfz" ? std::optional<std::string>("<region>sample=b.wav") : std::nullopt;
        });
        CHECK(samples(read) == S({"b.wav", "c.wav"}));
        for (auto& g : read.regions) CHECK(g.zone.high_velocity == 50);
    }
    { // laid out as Splendid Grand Piano's file is
        std::map<std::string, std::string> files {
            {"Data/Res.txt", "group_volume=-11\nampeg_attack=0.1\n\n<region> lokey=021 hikey=022 pitch_keycenter=033 sample=PP A0.$EXT\n"},
            {"Data/PP.txt", "<region> region_label=01 lokey=021 hikey=024 pitch_keycenter=023 ampeg_release=0.6 sample=PP B-1.$EXT\n"
                            "<region> region_label=07 lokey=036 hikey=037 pitch_keycenter=037 sample=PP C#1.$EXT volume=1\n"
                            "<region> region_label=08 lokey=038 hikey=039 pitch_keycenter=038 sample=PP D1.$EXT offset=1046\n"
                            "<region> region_label=44 lokey=091 hikey=091 pitch_keycenter=091 ampeg_release=10 sample=PP G5.$EXT pan=-30 offset=135\n"},
            {"Data/FF.txt", "<region> lokey=036 hikey=037 pitch_keycenter=037 sample=FF C#1.$EXT\n"}};
        auto read = read_sfz(
            "#define $RES 70\n#define $RELEASE 72\n#define $EXT flac\n#define $DIR Data\n"
            "<control>\ndefault_path=Samples/\nlabel_cc64=Sus.Pedal\nlabel_cc$RES=Resonance\nset_hdcc$RES=0.5\nset_hdcc$RELEASE=0.35\n"
            "//*******************************************************\n"
            "<global>\nampeg_attack=0.0005\nampeg_hold=8\nampeg_decay=80\nampeg_sustain=0\nampeg_release_oncc$RELEASE=2\nampeg_releasecc64=15\nampeg_release_curvecc64=11\n"
            "<curve>curve_index=11\nv000=0\nv127=1\n"
            "<group>\ngroup=1\nlocc64=65\nlocc$RES=1\n#include \"$DIR/Res.txt\"\n"
            "<group>\nlovel=1\nhivel=40\ncutoff=1000\ngroup_volume=0\n#define $DYN PP\ngroup_label=P$DYN\n#include \"$DIR/$DYN.txt\"\n"
            "<group>\nlovel=41\nhivel=127\ngroup_volume=-3\n#define $DYN FF\ngroup_label=$DYN\n#include \"$DIR/$DYN.txt\"\n",
            [&](const std::string& name) { auto f = files.find(name); return f == files.end() ? std::nullopt : std::optional<std::string>(f->second); });
        CHECK(read.left == 1 && read.left_by == std::set<std::string> {"locc64"}); // the resonance layer waits for the pedal
        CHECK(read.regions.size() == 5);
        const auto& first = read.regions[0], &sharp = read.regions[1], &last = read.regions[3], &loud = read.regions[4];
        CHECK(first.sample == "Samples/PP B-1.flac" && sharp.sample == "Samples/PP C#1.flac" && loud.sample == "Samples/FF C#1.flac");
        for (unsigned n = 0; n < 4; ++n) CHECK(read.regions[n].zone.cutoff == 1000 && read.regions[n].zone.high_velocity == 40);
        CHECK(loud.zone.cutoff == 0 && loud.zone.low_velocity == 41 && loud.zone.gain == -3);
        CHECK(read.regions[2].offset == 1046 && last.zone.pan == -0.3f);
        CHECK(read.envelope.sustain == 100.0); // the hold and decay pass the longest stage: the sample falls by itself
        CHECK(read.envelope.release == 0.001 + 2 * 0.35);
        CHECK(sharp.zone.release == 0 && loud.zone.release == 0);
        CHECK(first.zone.release == sfz_release_step(1.3) - sfz_release_step(0.701));
        CHECK(last.zone.release == 100 - sfz_release_step(0.701));
        CHECK(read.velocity == 100);
        for (auto& name : read.ignored) CHECK(name.rfind("label_", 0) != 0 && name.rfind("set_", 0) != 0);
        CHECK(!read.ignored.count("ampeg_release_oncc72") && !read.ignored.count("cutoff") && read.ignored.count("ampeg_releasecc64"));
    }
    {
        auto found = sfz_opcodes("sample=PP C#1.flac Lokey=36 x-y=1  9a=2 == hikey= 40 path=a=b.wav");
        std::vector<std::pair<std::string, std::string>> expected {
            {"sample", "PP C#1.flac"}, {"lokey", "36 x-"}, {"y", "1  9"}, {"a", "2 =="}, {"hikey", "40"}, {"path", ""}, {"a", "b.wav"}};
        CHECK(found == expected);
    }
    { // a line of stars is a line comment; a /* never closed is the rest of the file
        auto read = read_sfz("//****************\n<region> sample=a.wav // and what follows /* is still this line's\n"
                             "/* a // in a block */ <region> sample=b.wav\n/////////////\n<region> sample=c.wav /* never closed\n<region> sample=d.wav\n");
        CHECK(samples(read) == S({"a.wav", "b.wav", "c.wav"}));
        CHECK(sfz_uncommented("a//b\r\nc/*d*/e\n") == "a\r\nce\n");
    }
    {
        auto read = read_sfz("<control> set_cc1=100 set_hdcc2=0.25\n<region> sample=up.wav key=60 hicc64=63\n<region> sample=down.wav key=60 locc64=64\n"
                             "<region> sample=wheel high.wav key=61 locc1=90\n<region> sample=wheel low.wav key=61 hicc1=89\n"
                             "<region> sample=fine.wav key=62 lohdcc2=0.5\n<region> sample=fine low.wav key=62 hihdcc2=0.5\n<region> sample=note.wav key=63 locc133=64\n");
        // Issue 66: a controller's range other than the pedal's is followed as it plays (the region kept, the range and
        // where the controller starts on its zone); the pedal's waits as before; a region a controller's move starts is
        // left out.
        CHECK(samples(read) == S({"up.wav", "wheel high.wav", "wheel low.wav", "fine.wav", "fine low.wav", "note.wav"}));
        CHECK(read.left == 1 && read.left_by == std::set<std::string> {"locc64"} && read.ignored.count("locc133"));
        const auto& up = read.regions[0].zone, &high = read.regions[1].zone, &low = read.regions[2].zone, &fine = read.regions[3].zone;
        CHECK(up.cc == -1 && high.cc == 1 && high.cc_low == 90 && high.cc_high == 127 && high.cc_start == 100);
        CHECK(low.cc == 1 && low.cc_low == 0 && low.cc_high == 89 && fine.cc == 2 && fine.cc_low == 64 && fine.cc_start == 32);
        CHECK(read.regions[5].zone.cc == -1);
        auto hats = read_sfz("<region> sample=closed.wav key=42 hicc4=63\n<region> sample=open.wav key=42 locc4=64\n<region> sample=splash.wav on_locc4=100\n");
        CHECK(samples(hats) == S({"closed.wav", "open.wav"}) && hats.left == 1 && hats.left_by.count("on_locc4"));
        CHECK(hats.regions[0].zone.cc == 4 && hats.regions[0].zone.cc_high == 63 && hats.regions[1].zone.cc_low == 64 && hats.regions[1].zone.cc_start == 0);
    }
    {
        auto read = read_sfz("<global> sw_lokey=24 sw_hikey=25 sw_default=24\n<region> sample=note.wav key=60 sw_last=24\n<region> sample=other.wav key=60 sw_last=25\n"
                             "<region> sample=tail.wav key=60 trigger=release sw_last=24\n<region> sample=first.wav key=61 trigger=first sw_last=24\n"
                             "<region> sample=slur.wav key=61 trigger=legato sw_last=24\n");
        // roadmap P3: every articulation is a keyswitch's zone and a release sample plays on the key let go; a legato's is left out
        CHECK(samples(read) == S({"note.wav", "other.wav", "tail.wav", "first.wav"}));
        CHECK(read.left == 1 && read.left_by == std::set<std::string>({"trigger"}) && read.regions[2].zone.on_release);
    }
    {
        auto read = read_sfz("<region> sample=a.wav cutoff=800 resonance=9 fil_type=hpf_2p fil_veltrack=1200 fil_keytrack=50 fil_keycenter=c5\n"
                             "<region> sample=b.wav cutoff=800 fil_type=lpf_1p\n<region> sample=c.wav cutoff=800 fil_type=bpf_2p\n"
                             "<region> sample=d.wav cutoff=800 fil_type=brf_2p\n<region> sample=e.wav cutoff=99999 resonance=99\n");
        const auto& r = read.regions;
        CHECK(r[0].zone.cutoff == 800 && r[0].zone.resonance == 9 && r[0].zone.filter == PadFilter::Hpf2 && r[0].zone.cutoff_velocity == 1200
              && r[0].zone.cutoff_key == 50 && r[0].zone.cutoff_center == 72);
        CHECK(r[1].zone.filter == PadFilter::Lpf2 && r[2].zone.filter == PadFilter::Bpf2);
        CHECK(r[3].zone.cutoff == 0 && r[3].zone.filter == PadFilter::Off && read.ignored == std::set<std::string> {"fil_type"});
        CHECK(r[4].zone.cutoff == 20000 && r[4].zone.resonance == 40);
    }
    { // a zone's envelope is off the one most share
        auto read = read_sfz("<region> sample=a.wav ampeg_release=2\n<region> sample=b.wav ampeg_release=0.5 ampeg_attack=0.1\n<region> sample=c.wav ampeg_release=0.5\n"
                             "<region> sample=d.wav ampeg_release=0.5 ampeg_sustain=50 ampeg_decay=1\n<region> sample=e.wav\n");
        CHECK(read.envelope.attack == 0.0 && read.envelope.decay == 0.0 && read.envelope.sustain == 100.0 && read.envelope.release == 0.5);
        const auto& r = read.regions;
        CHECK(r[1].zone.release == 0 && r[0].zone.release == sfz_release_step(2) - sfz_release_step(0.5));
        CHECK(r[1].zone.attack == sfz_attack_step(0.1) && r[2].zone.attack == 0);
        CHECK(r[3].zone.sustain == 50 - 100 && r[3].zone.decay == sfz_decay_step(1) - sfz_decay_step(0));
        CHECK(r[4].zone.release == sfz_release_step(0.001) - sfz_release_step(0.5));
    }
    {
        auto read = read_sfz("<region> sample=a.wav\n<region> sample=b.wav");
        CHECK(!read.envelope.attack && !read.envelope.decay && !read.envelope.sustain && !read.envelope.release);
        for (auto& g : read.regions) CHECK(g.zone.attack == 0 && g.zone.release == 0 && g.zone.sustain == 0 && g.zone.decay == 0);
    }
    {
        auto read = read_sfz("<region> sample=a.wav ampeg_hold=1 ampeg_decay=2 ampeg_sustain=40 amp_veltrack=25");
        CHECK(read.envelope.decay == 3.0 && read.envelope.sustain == 40.0 && read.velocity == 25);
    }
    {
        std::string many;
        for (unsigned n = 0; n < max_zones + 3; ++n) many += "<region>sample=" + std::to_string(n) + ".wav\n";
        auto read = read_sfz(many);
        CHECK(read.regions.size() == max_zones && read.left == 3);
    }
}

// An MPC keygroup program's keygroups and layers as the SFZ reader's zones.
// The auto sampler's .xpm (shell/xpm_write.h): keys, roots, velocities and release as the MPC keeps them.
void xpm_writer() {
    XpmProgram program;
    program.name = "Pad & <Lead>";
    program.release = 0.4;
    XpmKeygroup low;
    low.low = 0; low.high = 37;
    low.layers = {{"Pad-C2-V63", 36, 0, 63, 48000}, {"Pad-C2-V127", 36, 64, 127, 50000}};
    low.layers[1].loop_start = 20000; low.layers[1].loop_end = 40000;
    XpmKeygroup high;
    high.low = 38; high.high = 127;
    high.layers = {{"Pad-Eb2-V127", 39, 0, 127, 44100}};
    program.keygroups = {low, high};
    const std::string text = xpm_text(program);
    auto has = [&](const char* part) { return text.find(part) != std::string::npos; };
    auto count = [&](const char* part) { size_t n = 0; for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n; return n; };
    CHECK(has("<ProgramName>Pad &amp; &lt;Lead&gt;</ProgramName>"));
    CHECK(has("<Program type=\"Keygroup\">") && has("<KeygroupNumKeygroups>2</KeygroupNumKeygroups>"));
    CHECK(count("<Instrument number=") == 128 && count("<Layer number=") == 512 && count("<PadNote number=") == 128); // as the MPC writes them
    CHECK(has("&quot;ProgramPads-v2.10&quot;") && has("<AudioRoute>2</AudioRoute>"));
    CHECK(has("<LowNote>0</LowNote>") && has("<HighNote>37</HighNote>") && has("<LowNote>38</LowNote>") && has("<HighNote>127</HighNote>"));
    CHECK(has("<RootNote>37</RootNote>") && has("<RootNote>40</RootNote>")); // one past the MIDI note
    CHECK(has("<SampleName>Pad-C2-V63</SampleName>") && has("<VelEnd>63</VelEnd>") && has("<VelStart>64</VelStart>"));
    CHECK(has("<SliceEnd>47999</SliceEnd>") && has("<SliceEnd>44099</SliceEnd>") && has("<TriggerMode>2</TriggerMode>")); // the last frame
    CHECK(!has("<TriggerMode>0</TriggerMode>"));
    { XpmProgram kit = program; kit.keygroups[0].one_shot = true; CHECK(xpm_text(kit).find("<TriggerMode>0</TriggerMode>") != std::string::npos); }
    { // a split: Mono on the lower side's keys only. The program is not Mono; that side's keygroups are, in one mute
      // group, and its glide is in tags of its own. Each keygroup follows velocity its own way, a layer at its own level.
        XpmProgram split = program;
        split.mono = true; split.legato = true; split.tied = false; split.glide = 0.06;
        split.mono_high = 37;
        split.keygroups[0].velocity = 0.25; split.keygroups[1].velocity = 1.0;
        split.keygroups[0].layers[0].gain = 2.0;
        const std::string text = xpm_text(split);
        auto has = [&](const std::string& part) { return text.find(part) != std::string::npos; };
        auto count = [&](const std::string& part) { size_t n = 0; for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n; return n; };
        CHECK(has("    <Mono>False</Mono>") && has("    <PortamentoTime>0.000000</PortamentoTime>") && has("<PortamentoLegato>False</PortamentoLegato>"));
        CHECK(has("<MonoSidePortamentoLegato>True</MonoSidePortamentoLegato>") && has("<MonoSidePortamentoTime>") && has("<MonoRetrigger>True</MonoRetrigger>"));
        CHECK(count("        <Mono>True</Mono>") == 1 && count("<Polyphony>1</Polyphony>") == 1 && count("<MuteGroup>1</MuteGroup>") == 1);
        CHECK(has("<VelocitySensitivity>0.250000</VelocitySensitivity>") && has("<VelocitySensitivity>1.000000</VelocitySensitivity>"));
        CHECK(has("<Volume>2.000000</Volume>"));
        CHECK(!split.mono_all() && split.mono_side(split.keygroups[0]) && !split.mono_side(split.keygroups[1]));
        // the program's Volume on the MPC's curve (40 log10(v) + 6 dB): 0 dB, and the pan law's 3 dB given back
        CHECK(has("    <Volume>0.707946</Volume>"));
        split.volume_db = 3.0103;
        CHECK(xpm_text(split).find("    <Volume>0.841") != std::string::npos);
    }
    { // a fall of two stages: a keygroup twice over the same keys and layers, each its share of the level and its release
        XpmProgram two = program;
        two.release = 0.05;
        XpmKeygroup ringing = two.keygroups[1];
        ringing.gain = 0.2; ringing.release = 1.5;
        two.keygroups[1].gain = 0.8;
        two.keygroups.push_back(ringing);
        const std::string text = xpm_text(two);
        CHECK(text.find("<KeygroupNumKeygroups>3</KeygroupNumKeygroups>") != std::string::npos);
        CHECK(text.find("<Volume>0.800000</Volume>") != std::string::npos && text.find("<Volume>0.200000</Volume>") != std::string::npos);
        char slow[64], fast[64];
        std::snprintf(slow, sizeof(slow), "<VolumeRelease>%.6f</VolumeRelease>", xpm_envelope_value(1.5));
        std::snprintf(fast, sizeof(fast), "<VolumeRelease>%.6f</VolumeRelease>", xpm_envelope_value(0.05));
        CHECK(text.find(slow) != std::string::npos && text.find(fast) != std::string::npos);
        CHECK(text.find("<TriggerMode>1</TriggerMode>") == std::string::npos && text.find("<SliceStart>0</SliceStart>") != std::string::npos);
        // read back as Melodee plays it: two zones over the same keys, the second 14 dB under with the longer release
        KeygroupProgram tree;
        tree.type = "Keygroup";
        tree.tags = {{"KeygroupNumKeygroups", "2"}};
        KeygroupInstrument body, ring;
        body.number = 1; body.tags = {{"LowNote", "38"}, {"HighNote", "127"}, {"TriggerMode", "2"}, {"VolumeRelease", std::to_string(xpm_envelope_value(0.05))}};
        body.layers = {{{"SampleName", "Pad-Eb2-V127"}, {"RootNote", "40"}, {"Volume", "0.8"}, {"SliceEnd", "44099"}, {"SliceLoop", "1"}, {"SliceLoopStart", "20000"}}};
        ring = body;
        ring.number = 2; ring.tags["VolumeRelease"] = std::to_string(xpm_envelope_value(1.5));
        ring.layers[0]["Volume"] = "0.2";
        tree.instruments = {body, ring};
        const SfzRead read = read_keygroup(tree);
        CHECK(read.regions.size() == 2 && read.regions[0].zone.low_key == 38 && read.regions[1].zone.low_key == 38);
        CHECK(std::fabs(read.regions[0].zone.gain - 20 * std::log10(0.8)) < 0.1 && std::fabs(read.regions[1].zone.gain - 20 * std::log10(0.2)) < 0.1);
        CHECK(read.regions[1].zone.release > read.regions[0].zone.release && read.regions[0].zone.loop == ZoneLoop::On && read.regions[1].zone.loop == ZoneLoop::On);
    }
    // the sampler's roots and velocities
    AutoSampleSettings set;
    CHECK(set.roots().size() == 21 && set.roots().front() == 36 && set.roots().back() == 96);
    set.layers = 3;
    CHECK(set.velocity(0) == 42 && set.velocity(1) == 84 && set.velocity(2) == 127);
    set.layers = 1;
    CHECK(set.velocity(0) == 127);
    // Mono and Portamento: the program's tags, on MPC 3's curve (30 s x e^(7 (v - 1)), 0 off)
    CHECK(!has("<Mono>True</Mono>") && has("<PortamentoTime>0.000000</PortamentoTime>"));
    program.mono = true; program.legato = true; program.glide = 0.06;
    const std::string mono = xpm_text(program);
    char porta[64];
    std::snprintf(porta, sizeof(porta), "<PortamentoTime>%.6f</PortamentoTime>", mpc_portamento_value(0.06));
    CHECK(mono.find("    <Mono>True</Mono>") != std::string::npos && mono.find(porta) != std::string::npos
          && mono.find("<PortamentoLegato>True</PortamentoLegato>") != std::string::npos);
    CHECK(std::fabs(mpc_portamento_seconds(1.0) - 30.0) < 1e-9 && mpc_portamento_seconds(0) == 0 && mpc_portamento_value(0) == 0);
    CHECK(std::fabs(mpc_portamento_seconds(mpc_portamento_value(0.06)) - 0.06) < 1e-6);
    CHECK(std::fabs(mpc_portamento_seconds(0.5) - 30.0 * std::exp(-3.5)) < 1e-9);
    KeygroupProgram tree;
    tree.type = "Keygroup";
    tree.tags = {{"Mono", "True"}, {"PortamentoLegato", "True"}, {"PortamentoTime", std::to_string(mpc_portamento_value(0.2))}};
    const SfzRead read = read_keygroup(tree);
    CHECK(read.mono && read.legato && std::fabs(read.glide - 0.2) < 1e-4);
    // Mono Retrig under MPC 3's name: whether a key pressed while one is held starts the note again. Written for a
    // mono program only; a program without it carries the note on where its portamento is Legato.
    CHECK(read.tied && mono.find("<MonoRetrigger>True</MonoRetrigger>") != std::string::npos && has("<MonoRetrigger>False</MonoRetrigger>"));
    program.tied = true;
    CHECK(xpm_text(program).find("<MonoRetrigger>False</MonoRetrigger>") != std::string::npos);
    tree.tags["MonoRetrigger"] = "True";
    CHECK(!read_keygroup(tree).tied && read_keygroup(tree).legato);
    tree.tags["MonoRetrigger"] = "False"; tree.tags["PortamentoLegato"] = "False";
    CHECK(read_keygroup(tree).tied && !read_keygroup(tree).legato);
    tree.tags.clear();
    CHECK(!read_keygroup(tree).mono && read_keygroup(tree).glide == 0);
    { // a split's mono side read back: the keys of the keygroups that are Mono in a mute group, its glide from its own tags
        KeygroupProgram split;
        split.type = "Keygroup";
        split.tags = {{"Mono", "False"}, {"PortamentoTime", "0.0"}, {"MonoRetrigger", "True"},
                      {"MonoSidePortamentoTime", std::to_string(mpc_portamento_value(0.2))}, {"MonoSidePortamentoLegato", "True"}};
        auto keygroup = [](unsigned number, int low, int high, bool mono, const char* velocity) {
            KeygroupInstrument made;
            made.number = number;
            made.tags = {{"LowNote", std::to_string(low)}, {"HighNote", std::to_string(high)}, {"Mono", mono ? "True" : "False"},
                         {"MuteGroup", mono ? "1" : "0"}, {"VelocitySensitivity", velocity}};
            KeygroupTags layer {{"Active", "True"}, {"SampleName", "S" + std::to_string(number)}, {"RootNote", std::to_string(low + 1)}};
            made.layers.push_back(layer);
            return made;
        };
        split.instruments = {keygroup(1, 0, 47, true, "0.2"), keygroup(2, 48, 59, true, "0.2"), keygroup(3, 60, 127, false, "0.9")};
        const SfzRead read = read_keygroup(split);
        CHECK(read.mono && read.mono_low == 0 && read.mono_high == 59 && !read.tied && read.legato && std::fabs(read.glide - 0.2) < 1e-4);
        CHECK(read.regions.size() == 3 && std::fabs(read.velocity - 20) < 0.5 && read.regions[0].zone.velocity == 0 && read.regions[2].zone.velocity == 70);
    }
}

// Melodee's own card folders the MPC's browsers leave out (model/card_folders.h): Cache and Crashes in Melodee/ only.
void card_folders() {
    CHECK(melodee_own_folder("/media/card/Melodee/Cache") && melodee_own_folder("/media/card/Melodee/Crashes"));
    CHECK(!melodee_own_folder("/media/card/Melodee/Plugins") && !melodee_own_folder("/media/card/Samples/Cache")
          && !melodee_own_folder("/media/card/Melodee/Projects/Crashes"));
}

// The auto sampler's files and the sheet's TIME: a sound the same on both sides (within a 24-bit step) goes into a
// mono WAV, read back onto both sides; the time is each note's hold, its fall to silence (TAIL at most) and a pause,
// at the speed the check played it.
void auto_sample_mono_time() {
    std::vector<float> both(2000);
    for (size_t f = 0; f < 1000; ++f) both[2 * f] = both[2 * f + 1] = 0.5f * std::sin(0.03f * float(f));
    CHECK(sides_alike(both.data(), 1000));
    both[701] += 1.0f / 16777216.0f; // half a 24-bit step apart: still one sound
    CHECK(sides_alike(both.data(), 1000));
    both[701] += 1.0f / 8388608.0f;  // a step and a half: two
    CHECK(!sides_alike(both.data(), 1000));
    both[701] = std::nanf("");
    CHECK(!sides_alike(both.data(), 1000));
    const auto path = (std::filesystem::temp_directory_path() / ("melodee-mono-" + std::to_string(getpid()) + ".wav")).string();
    std::vector<float> one(1000);
    for (size_t f = 0; f < one.size(); ++f) one[f] = 0.5f * std::sin(0.03f * float(f));
    WavStream wav;
    CHECK(wav.open(path, 48000, 24, 0, 1) && wav.add(one.data(), unsigned(one.size())) && wav.close());
    CHECK(std::filesystem::file_size(path) == 44 + 3 * one.size());
    SampleData read;
    WavInfo info;
    CHECK(read_wav(path, read, 60, &info).empty() && info.channels == 1 && read.frames == one.size());
    CHECK(info.root == -1); // no smpl chunk: no root of its own
    {   // a smpl chunk's MIDI unity note (45, A2) after the audio
        std::fstream add(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);
        uint8_t smpl[68] = {'s', 'm', 'p', 'l', 60};
        smpl[8 + 12] = 45;
        add.write(reinterpret_cast<const char*>(smpl), sizeof smpl);
        const uint32_t riff = uint32_t(add.tellp()) - 8;
        add.seekp(4);
        add.write(reinterpret_cast<const char*>(&riff), 4);
    }
    WavInfo rooted;
    CHECK(read_wav(path, read, 60, &rooted).empty() && rooted.root == 45 && rooted.loop_start == -1);
    for (size_t f = 0; f < one.size(); ++f) CHECK(read.left[f] == read.right[f] && std::fabs(read.left[f] - one[f]) < 2.0f / 8388608.0f);
    std::filesystem::remove(path);

    AutoSampleSettings set;
    set.low = 60; set.high = 66; set.step = 3; // C4, D#4, F#4
    set.hold_auto = false; set.hold = 2.0f; set.tail = 8.0f;
    CHECK(set.seconds_expected() == 0); // nothing heard yet
    set.found.heard = true; set.found.speed = 2.0f;
    set.found.release = 0.26f; // 26 dB in 0.26 s: silent (60 dB) after 0.6 s
    CHECK(std::fabs(set.seconds_expected() - 3 * (2.0f + 0.6f + 0.2f + 0.1f) / 2.0f) < 1e-3f);
    set.tail = 0.5f; // the tail cut at TAIL
    CHECK(std::fabs(set.seconds_expected() - 3 * (2.0f + 0.5f + 0.1f) / 2.0f) < 1e-3f);
    set.tail = 8.0f;
    set.found.release_slow = 2.6f; set.found.release_share = 0.1f; // a second stage from -20 dB: 40 dB more in 4 s
    CHECK(std::fabs(set.seconds_expected() - 3 * (2.0f + 4.0f + 0.2f + 0.1f) / 2.0f) < 1e-3f);
    set.layers = 2; // each key twice
    CHECK(std::fabs(set.seconds_expected() - 6 * (2.0f + 4.0f + 0.2f + 0.1f) / 2.0f) < 1e-3f);
}

// Velocity layers (engine/auto_probe.h probe_layers): the check's sixteen hits of one key split where the tone
// changes, an accent starting a layer; an .xpm keeps four of eight (xpm_layers); the sheet's LAYERS at Auto and 1-8.
void auto_sample_layers() {
    std::vector<uint8_t> v;
    for (int x = 8; x <= 127; x = x == 120 ? 127 : x + 8) v.push_back(uint8_t(x));
    std::vector<float> follows, same(v.size(), 800.0f), ramp, jump, jump_db;
    for (size_t i = 0; i < v.size(); ++i) {
        follows.push_back(20.0f * std::log10(v[i] / 127.0f)); // level as Velocity 1 plays it
        ramp.push_back(400.0f * std::pow(2.0f, 3.0f * v[i] / 127.0f)); // three octaves brighter, soft to hard
        jump.push_back(v[i] >= 104 ? 2400.0f : 800.0f);         // an accent at 101: brighter, louder
        jump_db.push_back(follows.back() + (v[i] >= 104 ? 6.0f : 0.0f));
    }
    // Level only, as Velocity plays it: one layer.
    CHECK(probe_layers(v, follows, same, 1.0f, 0, 0) == std::vector<uint8_t> {127});
    // The tone opens soft to hard: layers, rising, the last 127, no hit further than 1.5 dB from its layer's sample.
    const auto heard = probe_layers(v, follows, ramp, 1.0f, 0, 0);
    CHECK(heard.size() >= 3 && heard.size() <= 8 && heard.back() == 127 && std::is_sorted(heard.begin(), heard.end()));
    const auto eight = probe_layers(v, follows, ramp, 1.0f, 0, 8);
    CHECK(eight.size() == 8 && eight.back() == 127 && std::adjacent_find(eight.begin(), eight.end(), std::greater_equal<uint8_t>()) == eight.end());
    CHECK(probe_layers(v, follows, ramp, 1.0f, 0, 1) == std::vector<uint8_t> {127});
    // An accent starts a layer (the layer under it ends a velocity before), unless there is one layer only.
    CHECK(probe_layers(v, jump_db, jump, 1.0f, 101, 2) == (std::vector<uint8_t> {100, 127}));
    CHECK(probe_layers(v, jump_db, jump, 1.0f, 101, 0) == (std::vector<uint8_t> {100, 127}));
    CHECK(probe_layers(v, jump_db, jump, 1.0f, 101, 1) == std::vector<uint8_t> {127});
    const auto three = probe_layers(v, jump_db, jump, 1.0f, 101, 3);
    CHECK(three.size() == 3 && std::find(three.begin(), three.end(), 100) != three.end() && three.back() == 127);
    // Nothing heard: evenly. Fewer hits than layers asked: a layer a hit.
    CHECK(probe_layers({}, {}, {}, 1.0f, 0, 3) == (std::vector<uint8_t> {42, 84, 127}));
    CHECK(probe_layers({40, 80, 127}, {-10, -5, 0}, {400, 800, 1600}, 1.0f, 0, 8) == (std::vector<uint8_t> {40, 80, 127}));

    // An .xpm holds four: eight layers as four runs of two, each its top layer over the run's velocities.
    XpmKeygroup group;
    const uint8_t tops[] {16, 32, 48, 64, 80, 96, 112, 127};
    for (unsigned l = 0; l < 8; ++l) {
        XpmLayer layer;
        layer.sample = "Key-V" + std::to_string(tops[l]);
        layer.low_velocity = uint8_t(l ? tops[l - 1] + 1 : 0);
        layer.high_velocity = tops[l];
        group.layers.push_back(layer);
    }
    auto four = xpm_layers(group);
    CHECK(four.size() == 4 && four[0].sample == "Key-V32" && four[1].sample == "Key-V64" && four[2].sample == "Key-V96" && four[3].sample == "Key-V127");
    CHECK(four[0].low_velocity == 0 && four[1].low_velocity == 33 && four[2].low_velocity == 65 && four[3].low_velocity == 97 && four[3].high_velocity == 127);
    group.layers.resize(5); // five: the softest two share a run
    four = xpm_layers(group);
    CHECK(four.size() == 4 && four[0].sample == "Key-V32" && four[0].low_velocity == 0 && four[1].sample == "Key-V48" && four[3].sample == "Key-V80");
    group.layers.resize(3);
    CHECK(xpm_layers(group).size() == 3);
    XpmProgram program;
    program.name = "Key";
    group.layers.clear();
    for (unsigned l = 0; l < 8; ++l) group.layers.push_back({"Key-V" + std::to_string(tops[l]), 60, uint8_t(l ? tops[l - 1] + 1 : 0), tops[l], 44100});
    program.keygroups.push_back(group);
    auto count = [](const std::string& text, const std::string& what) {
        size_t n = 0;
        for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
        return n;
    };
    const std::string mpc2 = xpm_text(program), all = xpm_text(program, 8);
    CHECK(count(mpc2, "<Layer number=") == 128 * 4 && count(all, "<Layer number=") == 128 * 8);
    CHECK(mpc2.find("Key-V16") == std::string::npos && mpc2.find("<SampleName>Key-V32</SampleName>") != std::string::npos
          && all.find("<SampleName>Key-V16</SampleName>") != std::string::npos);

    // LAYERS at Auto: as many as the check heard, where it heard them; set by hand, as many as asked, the accent
    // kept only from two up. Not heard yet: one at Auto, evenly by hand.
    AutoSampleSettings set;
    CHECK(set.layers == 0 && set.layer_count() == 1 && set.velocity(0) == 127);
    set.found.heard = true;
    set.found.accent = 101;
    set.found.layers = 3;
    for (unsigned k = 1; k <= 8; ++k) {
        const auto made = probe_layers(v, jump_db, jump, 1.0f, 101, k);
        for (size_t l = 0; l < made.size(); ++l) set.found.layer_tops[k - 1][l] = made[l];
    }
    CHECK(set.layer_count() == 3 && set.velocity(2) == 127 && set.velocity(0) < set.velocity(1) && (set.velocity(0) == 100 || set.velocity(1) == 100));
    set.layers = 1;
    CHECK(set.layer_count() == 1 && set.velocity(0) == 127);
    set.layers = 2;
    CHECK(set.layer_count() == 2 && set.velocity(0) == 100 && set.velocity(1) == 127);
    set.found.takes = 3;
    CHECK(set.take_count() == 1); // takes with one layer only
    set.found = {};
    set.layers = 4;
    CHECK(set.velocity(0) == 31 && set.velocity(3) == 127);
}

// The auto sampler's release (engine/release_fit.h): a note's fall against its level as its key was let go, never
// rising, and the envelope fitted to it: one stage, or two where it drops and then rings on.
void release_fitting() {
    const float rate = 48000;
    const size_t held = size_t(rate * 1.0f);
    // A 220 Hz tone at `hold(t)` while held and `after(t)` (t from the release) once let go.
    auto note = [&](const std::function<double(double)>& hold, const std::function<double(double)>& after, float tail) {
        std::vector<float> x(held + size_t(tail * rate));
        for (size_t f = 0; f < x.size(); ++f) {
            const double t = double(f) / rate, wave = std::sin(2 * M_PI * 220 * t);
            x[f] = float(wave * (f < held ? hold(t) : after(double(f - held) / rate)));
        }
        return x;
    };
    auto steady = [](double) { return 0.5; };
    auto shape_of = [&](const std::vector<float>& x, bool cut = false) { return release_shape(x, x, held, rate, cut); };
    { // one stage: 26 dB down in 0.3 s
        const auto x = note(steady, [](double t) { return 0.5 * std::exp(-3 * t / 0.3); }, 2.0f);
        const auto shape = shape_of(x);
        CHECK(!shape.fading && shape.whole && shape.db.size() > 50 && std::fabs(shape.db[0]) < 2.5f);
        const auto model = fit_release({shape, shape});
        CHECK(!model.two() && model.fast > 0.24f && model.fast < 0.37f);
        CHECK(std::fabs(model.db(0.3f) + 26.06f) < 4.0f && model.db(0) == 0);
    }
    { // two: most of it gone in 50 ms, a fifth ringing on for 1.5 s
        const auto x = note(steady, [](double t) { return 0.5 * (0.8 * std::exp(-3 * t / 0.05) + 0.2 * std::exp(-3 * t / 1.5)); }, 6.0f);
        const auto model = fit_release({shape_of(x)});
        CHECK(model.two() && model.fast < 0.1f && model.slow > 1.1f && model.slow < 2.0f);
        CHECK(20 * std::log10(model.share) > -17.5f && 20 * std::log10(model.share) < -10.5f);
    }
    { // a delay: the note stops in 20 ms, echoes every 0.4 s, each 6 dB under the last: its shape never rises, and the
      // fit is a drop and then a ring over the echoes' own time (26 dB in 1.7 s)
        const auto x = note(steady, [](double t) {
            double level = 0.5 * std::exp(-3 * t / 0.02);
            for (int e = 1; e <= 12; ++e) {
                const double at = t - 0.4 * e;
                if (at > -0.15 && at < 0.15) level += 0.5 * 0.25 * std::pow(0.5, e - 1) * (0.5 + 0.5 * std::cos(M_PI * at / 0.15));
            }
            return level;
        }, 6.0f);
        const auto model = fit_release({shape_of(x)});
        CHECK(model.two() && model.fast < 0.15f && model.slow > 1.0f && model.slow < 2.8f);
    }
    { // a sound that swings 12 dB every 3 s, its keys let go at eight moments of the swing: each stops in 0.1 s, and a
      // quarter of its usual level rings on for 1.5 s whatever the moment. The notes together tell the drop and the
      // ring, where each alone would tell of its own moment.
        std::vector<ReleaseShape> notes;
        const double usual = 0.3;
        for (int n = 0; n < 8; ++n) {
            const double turn = n / 8.0;
            auto swing = [&](double t) { return usual * std::pow(10.0, 6.0 * std::sin(2 * M_PI * (t / 3.0 + turn)) / 20.0); };
            std::vector<float> x(size_t(rate * 8) + size_t(rate * 5));
            const size_t up = size_t(rate * 8);
            for (size_t f = 0; f < x.size(); ++f) {
                const double t = double(f) / rate, after = double(f) / rate - 8.0;
                const double dry = std::sin(2 * M_PI * 220 * t) * swing(t) * (f < up ? 1.0 : std::exp(-3 * after / 0.1));
                const double ring = f < up ? 0.0 : std::sin(2 * M_PI * 331 * t + n) * 0.25 * usual * std::exp(-3 * after / 1.5);
                x[f] = float(dry + ring);
            }
            notes.push_back(release_shape(x, x, up, rate, false));
        }
        const auto model = fit_release(notes);
        CHECK(model.two() && model.fast < 0.2f && model.slow > 1.0f && model.slow < 2.2f);
        CHECK(20 * std::log10(model.share) > -16.0f && 20 * std::log10(model.share) < -8.0f);
    }
    { // a pluck fading 17 dB a second that takes no notice of its key: no fall of its own, however long is known
        auto fade = [](double t) { return 0.5 * std::exp(-t / 0.5); };
        const auto x = note(fade, [&](double t) { return fade(1.0 + t); }, 2.0f);
        const auto shape = shape_of(x);
        CHECK(shape.fading && !shape.db.empty() && shape.db.back() > -2.5f);
        const auto model = fit_release({shape});
        CHECK(!model.two() && model.db(1.0f) > -3.0f);
    }
    { // the same pluck damped when let go (26 dB in 0.1 s): its own fade is not counted twice
        auto fade = [](double t) { return 0.5 * std::exp(-t / 0.5); };
        const auto x = note(fade, [&](double t) { return fade(1.0 + t) * std::exp(-3 * t / 0.1); }, 2.0f);
        const auto model = fit_release({shape_of(x)});
        CHECK(!model.two() && model.fast > 0.07f && model.fast < 0.14f);
    }
    { // cut dead: the shortest release; nothing to go by: 50 ms
        const auto x = note(steady, [](double) { return 0.0; }, 0.5f);
        CHECK(fit_release({shape_of(x)}).fast <= 0.02f);
        CHECK(fit_release({}).fast == 0.05f && !fit_release({}).two());
        std::vector<float> quiet(size_t(rate * 2), 0.0f);
        CHECK(release_shape(quiet, quiet, held, rate, false).db.empty());
    }
    { // notes cut at the longest tail kept say nothing after it: a long release is still found from what there is
        const auto x = note(steady, [](double t) { return 0.5 * std::exp(-3 * t / 4.0); }, 1.0f);
        const auto model = fit_release({shape_of(x, true)});
        CHECK(model.fast > 2.8f && model.fast < 5.6f);
    }
}

// The auto sampler on an instrument outside (engine/external_notes.h), with one made up: it answers 300 frames after a
// note is sent, with a sine that stops over 50 ms once its key is let go, over a little noise. What is kept of a note
// starts on its sound, its key's release where it was asked, its quiet end left out.
struct MadeUpInstrument final : ExternalIo {
    float rate = 48000;
    uint64_t clock = 0;
    uint32_t delay = 300;
    float noise = 0.0005f, level = 0.5f, fall = 0.05f;
    bool mute = false;
    std::vector<uint64_t> spikes; // frames heard at full scale whatever plays
    struct Voice { uint8_t key; float gain; double phase; uint64_t on, off; };
    std::vector<Voice> voices;
    std::vector<std::pair<uint64_t, Command>> posted;
    std::vector<float> heard;
    uint32_t seed = 9;
    unsigned ons = 0, offs = 0, all_offs = 0;
    uint64_t now() override { return clock; }
    void post(const Command& c, uint64_t at) override { posted.push_back({at, c}); }
    void forget() override { for (auto& p : posted) p.second.type = Command::Type::Reload; }
    bool copy(uint64_t from, uint32_t frames, float* l, float* r) override {
        if (from + frames > heard.size()) return false;
        for (uint32_t f = 0; f < frames; ++f) l[f] = r[f] = heard[from + f];
        return true;
    }
    void wait(unsigned ms) override {
        const uint64_t until = clock + uint64_t(ms * rate / 1000);
        for (; clock < until; ++clock) {
            for (auto& p : posted) { // each command as its frame comes, heard `delay` frames on
                if (p.first > clock || p.second.type == Command::Type::Reload) continue;
                const auto& c = p.second;
                if (c.type == Command::Type::NoteOn) { voices.push_back({uint8_t(c.index), level * float(c.value) / 100.0f, 0.0, clock + delay, UINT64_MAX}); ++ons; }
                else if (c.type == Command::Type::NoteOff) { for (auto& v : voices) if (v.key == c.index && v.off == UINT64_MAX) v.off = clock + delay; ++offs; }
                else if (c.type == Command::Type::AllNotesOff) { for (auto& v : voices) if (v.off == UINT64_MAX) v.off = clock + delay; ++all_offs; }
                p.second.type = Command::Type::Reload; // done
            }
            seed = seed * 1664525u + 1013904223u;
            double x = double(int32_t(seed)) / 2147483648.0 * noise;
            for (auto& v : voices) {
                if (clock < v.on || mute) continue;
                const double since = double(clock - v.on) / rate, hz = 440.0 * std::pow(2.0, (v.key - 69) / 12.0);
                double g = v.gain * std::min(1.0, since / 0.002);
                if (clock >= v.off) g *= std::exp(-double(clock - v.off) / rate / fall);
                v.phase += 2 * M_PI * hz / rate;
                x += g * std::sin(v.phase);
            }
            if (std::find(spikes.begin(), spikes.end(), clock) != spikes.end()) x = 1.0;
            heard.push_back(float(std::clamp(x, -1.0, 1.0)));
        }
    }
};

void external_notes() {
    const float rate = 48000;
    {
        MadeUpInstrument io;
        ExternalNotes notes(io, 3, rate);
        CHECK(notes.open(60, {}).empty());
        CHECK(notes.silence() > 0.0006f && notes.silence() < 0.002f); // its noise and a little more
        CHECK(notes.latency() > 200 && notes.latency() <= 300);       // the 300 frames, a millisecond early
        unsigned stretches = 0;
        const uint64_t hold = uint64_t(0.5f * rate);
        const NoteAudio a = notes.note(60, 100, hold, 2.0f, [&](const float*, const float*, unsigned n) { stretches += n; return true; });
        CHECK(a.error.empty() && a.held == hold && stretches >= a.left.size());
        size_t first = a.left.size();
        for (size_t f = 0; f < a.left.size(); ++f) if (std::fabs(a.left[f]) > 0.05f) { first = f; break; }
        CHECK(first < size_t(0.004f * rate));                           // it starts on its sound
        CHECK(peak_of(a.left, size_t(hold) - 4800, size_t(hold)) > 0.4f); // held to its key's release
        CHECK(a.release > uint64_t(0.15f * rate) && a.release < uint64_t(0.5f * rate) && !notes.clipped()); // the 50 ms fall, not the 2 s allowed
        CHECK(std::fabs(probe_pitch(a.left, 4800, 4096, rate) - 261.6f) < 2.0f);
        // a note that comes back later than open()'s did, and one sooner: each starts on its own sound, none of it lost
        for (uint32_t delay : {700u, 60u}) {
            io.delay = delay;
            const NoteAudio b = notes.note(64, 100, hold, 2.0f, {});
            size_t rise = b.left.size();
            for (size_t f = 0; f < b.left.size(); ++f) if (std::fabs(b.left[f]) > 0.05f) { rise = f; break; }
            CHECK(b.error.empty() && rise < size_t(0.004f * rate) && rise > 20 && std::fabs(b.left[0]) < 0.01f);
            CHECK(peak_of(b.left, size_t(hold) - 4800, size_t(hold)) > 0.4f && peak_of(b.left, size_t(hold) + 14400, b.left.size()) < 0.01f);
        }
        io.delay = 300;
        // a run of notes for the check: two keys, the second 0.3 s on
        const auto x = notes.notes({{0, true, 60, 100}, {uint64_t(0.3f * rate), false, 60, 100}, {uint64_t(0.3f * rate), true, 67, 100},
                                    {uint64_t(0.6f * rate), false, 67, 100}}, uint64_t(0.7f * rate), {});
        CHECK(x.size() == size_t(0.7f * rate));
        CHECK(std::fabs(probe_pitch(x, size_t(0.1f * rate), 4096, rate) - 261.6f) < 2.0f && std::fabs(probe_pitch(x, size_t(0.45f * rate), 4096, rate) - 392.0f) < 3.0f);
        // stopped part way: the key is let go
        const unsigned offs = io.offs, all = io.all_offs;
        const NoteAudio cut = notes.note(60, 100, hold, 2.0f, [&](const float*, const float*, unsigned) { return false; });
        io.wait(20);
        CHECK(cut.error == "stopped" && io.offs > offs && io.all_offs > all);
    }
    { // full scale: a flat top is a clip, on the key that made it; a peak that touches it for a frame or two is not
        MadeUpInstrument io;
        ExternalNotes notes(io, 0, rate);
        CHECK(notes.open(60, {}).empty() && !notes.clipped());
        const uint64_t at = io.clock + 4800;
        io.spikes = {at, at + 1, at + 2400};
        CHECK(notes.note(64, 100, uint64_t(0.2f * rate), 2.0f, {}).error.empty() && !notes.clipped() && notes.clipped_key() < 0);
        io.level = 1.5f;
        CHECK(notes.note(67, 100, uint64_t(0.2f * rate), 2.0f, {}).error.empty() && notes.clipped() && notes.clipped_key() == 67);
        io.level = 0.5f; // later notes keep the first key it clipped on
        CHECK(notes.note(72, 100, uint64_t(0.2f * rate), 2.0f, {}).error.empty() && notes.clipped_key() == 67);
    }
    { // a note still dying away as it opens: the silence is the input's once that has ended, not the note's end
        MadeUpInstrument io;
        io.fall = 0.15f;
        io.post({Command::Type::NoteOn, 3, 0, 60, 100}, 0);
        io.wait(100);
        ExternalNotes notes(io, 3, rate);
        CHECK(notes.open(60, {}).empty() && notes.silence() < 0.002f);
    }
    { // nothing comes back; an input that is not quiet; one that clips
        MadeUpInstrument silent;
        silent.mute = true;
        CHECK(ExternalNotes(silent, 0, rate).open(60, {}) == "no sound at the input");
        MadeUpInstrument noisy;
        noisy.noise = 0.2f;
        CHECK(ExternalNotes(noisy, 0, rate).open(60, {}) == "the input is not quiet");
        MadeUpInstrument hot;
        hot.level = 1.5f;
        ExternalNotes loud(hot, 0, rate);
        CHECK(loud.open(60, {}).empty() && loud.clipped() && loud.clipped_key() == 60);
        std::string why;
        MadeUpInstrument io;
        CHECK(ExternalNotes(io, 0, rate).open(60, [] { return false; }) == "stopped");
    }
}

// The auto sampler's check (engine/auto_probe.h), on made-up sounds: a pitch, a mono glide and a poly chord, an accent,
// drums, and hits that differ.
void auto_probe() {
    const float rate = 48000;
    auto tone = [&](std::vector<float>& x, size_t from, size_t frames, float hz_from, float hz_to, float slide, float level, double& phase) {
        if (x.size() < from + frames) x.resize(from + frames);
        for (size_t f = 0; f < frames; ++f) {
            const float t = float(f) / rate;
            const float hz = slide > 0 && t < slide ? hz_from * std::pow(hz_to / hz_from, t / slide) : hz_to;
            phase += 2 * M_PI * hz / rate;
            x[from + f] += level * float(std::sin(phase) + 0.3 * std::sin(2 * phase));
        }
    };
    { // a pitch; none in noise
        std::vector<float> x; double ph = 0;
        tone(x, 0, 4800, 220, 220, 0, 0.5f, ph);
        CHECK(std::fabs(probe_pitch(x, 0, 2048, rate) - 220) < 1.5f);
        std::vector<float> noise(4800);
        uint32_t seed = 7;
        for (auto& v : noise) { seed = seed * 1664525u + 1013904223u; v = float(int32_t(seed)) / 2147483648.0f * 0.5f; }
        CHECK(probe_pitch(noise, 0, 2048, rate) == 0 && probe_db(std::vector<float>(100), 0, 100) == -120.0f);
    }
    const float c3 = 130.81f, g3 = 196.0f;
    const size_t onset = size_t(0.4f * rate);
    { // mono, sliding a fifth over 0.1 s
        std::vector<float> x; double ph = 0;
        tone(x, 0, onset, c3, c3, 0, 0.5f, ph);
        tone(x, onset, size_t(0.8f * rate), c3, g3, 0.1f, 0.5f, ph);
        std::vector<float> alone; double pa = 0;
        tone(alone, 0, onset + size_t(0.8f * rate), c3, c3, 0, 0.5f, pa);
        const ProbeGlide g = probe_glide(x, alone, onset, c3, g3, rate);
        CHECK(g.mono && g.seconds > 0.07f && g.seconds < 0.14f);
        CHECK(probe_slides_apart(x, onset, c3, g3, rate)); // starts at the first key's pitch
    }
    { // played legato on a sound that fades: the second key starts it again, or the first's note carries on; a steady
      // sound tells nothing
        const size_t on = size_t(0.4f * rate), len = size_t(0.9f * rate);
        auto note = [&](std::vector<float>& x, size_t from, size_t frames, float hz, double since, double fall) {
            if (x.size() < from + frames) x.resize(from + frames);
            double phase = 0;
            for (size_t f = 0; f < frames; ++f) { // a tone fading by e each `fall` seconds, `since` seconds into it at `from`
                phase += 2 * M_PI * hz / rate;
                const double level = 0.5 * (fall > 0 ? std::exp(-(since + double(f) / rate) / fall) : 1.0);
                x[from + f] += float(level * (std::sin(phase) + 0.3 * std::sin(2 * phase)));
            }
        };
        std::vector<float> alone, fresh, again, carried;
        note(alone, 0, len, c3, 0, 0.2);
        note(fresh, 0, len, g3, 0, 0.2);
        note(again, 0, on, c3, 0, 0.2); note(again, on, len - on, g3, 0, 0.2);
        note(carried, 0, on, c3, 0, 0.2); note(carried, on, len - on, g3, 0.4, 0.2);
        CHECK(probe_retriggers(again, alone, fresh, on, rate) == 1 && probe_retriggers(carried, alone, fresh, on, rate) == 0);
        std::vector<float> steady_alone, steady_fresh, steady_both;
        note(steady_alone, 0, len, c3, 0, 0); note(steady_fresh, 0, len, g3, 0, 0);
        note(steady_both, 0, on, c3, 0, 0); note(steady_both, on, len - on, g3, 0, 0);
        CHECK(probe_retriggers(steady_both, steady_alone, steady_fresh, on, rate) == -1);
    }
    { // a key's character: two keys of one sound read alike, a sound that fades and a brighter one apart from it
        auto held = [&](float hz, double fall, float overtone) {
            std::vector<float> x(size_t(1.3f * rate));
            double phase = 0;
            for (size_t f = 0; f < x.size(); ++f) {
                phase += 2 * M_PI * hz / rate;
                const double level = 0.4 * (fall > 0 ? std::exp(-double(f) / rate / fall) : 1.0);
                x[f] = float(level * (std::sin(phase) + overtone * std::sin(4 * phase)));
            }
            return x;
        };
        const ProbeCharacter one = probe_character(held(c3, 0, 0.2f), c3, rate), next = probe_character(held(c3 * 1.0595f, 0, 0.2f), c3 * 1.0595f, rate);
        const ProbeCharacter fading = probe_character(held(c3, 0.15, 0.2f), c3, rate), bright = probe_character(held(c3, 0, 1.5f), c3, rate);
        CHECK(probe_apart(one, next) < 1.5f && std::fabs(one.fade) < 1.0f);
        CHECK(fading.fade > 30 && probe_apart(one, fading) > 20 && probe_apart(one, bright) > 6);
        CHECK(probe_character(std::vector<float>(100), c3, rate).level == -120.0f); // too short to tell
        // how far the level follows velocity: fully, half way, not at all
        const std::vector<uint8_t> velocities {16, 48, 80, 112, 127};
        auto levels = [&](double amount) {
            std::vector<float> db;
            for (uint8_t v : velocities) db.push_back(float(20 * std::log10(1.0 - amount * (1.0 - v / 127.0)) - 12));
            return db;
        };
        CHECK(std::fabs(probe_velocity(velocities, levels(1.0)) - 1.0f) < 0.02f && std::fabs(probe_velocity(velocities, levels(0.5)) - 0.5f) < 0.02f);
        CHECK(probe_velocity(velocities, levels(0.0)) < 0.02f && probe_velocity({}, {}) == 1.0f);
        CHECK(probe_velocity(velocities, levels(0.12)) == 0.0f); // too little to be velocity
        // A sound that wobbles from hit to hit (levels and brightness heard from a split patch): one big step and
        // back again is no accent; the same step that stays is.
        const std::vector<uint8_t> sixteen {8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127};
        const std::vector<float> wobble {-14.3f, -14.1f, -13.8f, -13.8f, -12.5f, -12.8f, -14.8f, -14.3f, -13.5f, -13.4f, -13.2f, -13.1f, -13.5f, -13.2f, -14.1f, -13.9f};
        const std::vector<float> brights {344, 356, 329, 318, 303, 318, 343, 346, 338, 301, 307, 317, 306, 308, 334, 338};
        CHECK(probe_accent(sixteen, wobble, brights) == 0);
        std::vector<float> stepped = wobble;
        for (size_t i = 0; i < stepped.size(); ++i) stepped[i] = i < 12 ? -20.0f - 0.2f * float(i % 2) : -12.0f - 0.2f * float(i % 2);
        CHECK(probe_accent(sixteen, stepped, std::vector<float>(16, 320.0f)) == 104);
    }
    { // low keys lie close in frequency (C2 and G2, 33 Hz): still told apart, mono and poly
        const float c2 = 65.41f, g2 = 98.0f;
        std::vector<float> x; double ph = 0;
        tone(x, 0, onset, c2, c2, 0, 0.5f, ph);
        tone(x, onset, size_t(0.8f * rate), g2, g2, 0, 0.5f, ph);
        std::vector<float> alone; double pa = 0;
        tone(alone, 0, onset + size_t(0.8f * rate), c2, c2, 0, 0.5f, pa);
        CHECK(probe_glide(x, alone, onset, c2, g2, rate).mono);
        std::vector<float> both; double a = 0, b = 0;
        tone(both, 0, onset + size_t(0.8f * rate), c2, c2, 0, 0.4f, a);
        tone(both, onset, size_t(0.8f * rate), g2, g2, 0, 0.4f, b);
        CHECK(!probe_glide(both, alone, onset, c2, g2, rate).mono);
    }
    { // mono, no slide: a jump
        std::vector<float> x; double ph = 0;
        tone(x, 0, onset, c3, c3, 0, 0.5f, ph);
        tone(x, onset, size_t(0.8f * rate), g3, g3, 0, 0.5f, ph);
        std::vector<float> alone; double pa = 0;
        tone(alone, 0, onset + size_t(0.8f * rate), c3, c3, 0, 0.5f, pa);
        const ProbeGlide g = probe_glide(x, alone, onset, c3, g3, rate);
        CHECK(g.mono && g.seconds == 0 && !probe_slides_apart(x, onset, c3, g3, rate));
    }
    { // poly: the first key plays on under the second
        std::vector<float> x; double a = 0, b = 0;
        tone(x, 0, onset + size_t(0.8f * rate), c3, c3, 0, 0.4f, a);
        tone(x, onset, size_t(0.8f * rate), g3, g3, 0, 0.4f, b);
        std::vector<float> alone; double pa = 0;
        tone(alone, 0, onset + size_t(0.8f * rate), c3, c3, 0, 0.4f, pa);
        CHECK(!probe_glide(x, alone, onset, c3, g3, rate).mono);
    }
    { // an accent: the sound jumps at 104; a velocity curve does not
        std::vector<uint8_t> vel;
        std::vector<float> db, bright, even_db;
        for (int v = 8; v <= 127; v = v == 120 ? 127 : v + 8) {
            vel.push_back(uint8_t(v));
            db.push_back(-20 + 0.1f * v + (v >= 104 ? 6.0f : 0.0f));
            bright.push_back(v >= 104 ? 1800.0f : 1000.0f);
            even_db.push_back(-40 + 0.3f * v);
        }
        CHECK(probe_accent(vel, db, bright) == 104);
        // a 303's: the same level up to 96, then 1.4 dB louder and darker
        std::vector<float> flat_db, flat_bright;
        for (auto v : vel) { flat_db.push_back(v >= 104 ? -31.0f : -32.4f); flat_bright.push_back(v >= 104 ? 305.0f : 346.0f); }
        CHECK(probe_accent(vel, flat_db, flat_bright) == 104);
        std::vector<float> curve; // level as 20 log(velocity): big first steps, no accent
        for (auto v : vel) curve.push_back(20.0f * std::log10(v / 127.0f));
        CHECK(probe_accent(vel, curve, std::vector<float>(vel.size(), 1000.0f)) == 0);
        CHECK(probe_accent(vel, even_db, std::vector<float>(vel.size(), 1000.0f)) == 0);
    }
    { // drums: pitches that do not follow the keys
        const std::vector<uint8_t> keys {48, 49, 50, 53, 60};
        std::vector<float> follows;
        for (auto k : keys) follows.push_back(440.0f * std::pow(2.0f, (k - 69) / 12.0f));
        CHECK(!probe_drums(keys, follows));
        CHECK(probe_drums(keys, {55.0f, 0, 0, 210.0f, 0}));
        CHECK(probe_drums(keys, {0, 0, 0, 0, 0}));
    }
    { // a held part that keeps its level sustains (a tremolo too); one that fades does not, nor silence
        std::vector<float> steady(48000), trem(48000), fade(48000);
        for (size_t f = 0; f < steady.size(); ++f) {
            const float wave = std::sin(2 * float(M_PI) * 220 * f / rate), t = f / rate;
            steady[f] = 0.5f * wave;
            trem[f] = 0.5f * wave * (0.7f + 0.3f * std::sin(2 * float(M_PI) * 5 * t));
            fade[f] = 0.5f * wave * std::exp(-3 * t);
        }
        CHECK(probe_sustains(steady, 0, steady.size(), rate) && probe_sustains(trem, 0, trem.size(), rate));
        CHECK(!probe_sustains(fade, 0, fade.size(), rate) && !probe_sustains(std::vector<float>(48000), 0, 48000, rate));
    }
    { // HOLD at Auto: how long a fading key is heard; whether a held sound moves; what the settings then hold for
        std::vector<float> pluck(size_t(rate * 8));
        for (size_t f = 0; f < pluck.size(); ++f) pluck[f] = float(0.5 * std::sin(2 * M_PI * 220 * f / rate) * std::pow(10.0, -15.0 * (f / rate) / 20)); // 15 dB a second
        const float heard = probe_fade_seconds(pluck, rate);
        CHECK(heard > 2.7f && heard < 3.4f); // 45 dB down after 3 s
        std::vector<float> still(size_t(rate * 8)), swell(size_t(rate * 8));
        for (size_t f = 0; f < still.size(); ++f) {
            const double t = double(f) / rate, wave = std::sin(2 * M_PI * 220 * t);
            still[f] = float(0.4 * wave);
            swell[f] = float(0.4 * wave * (0.6 + 0.4 * std::sin(2 * M_PI * 0.3 * t)));
        }
        CHECK(probe_fade_seconds(still, rate) > 7.9f && !sound_moves(still, still.size(), rate) && sound_moves(swell, swell.size(), rate));
        CHECK(!sound_moves(pluck, size_t(rate * 2), rate)); // a steady fade is not movement
        std::vector<float> settle(size_t(rate * 8)); // nor one that falls fast and then levels out (a 303's note)
        for (size_t f = 0; f < settle.size(); ++f) settle[f] = float(std::sin(2 * M_PI * 110 * f / rate) * (0.15 + 0.35 * std::exp(-double(f) / rate / 0.8)));
        CHECK(!sound_moves(settle, settle.size(), rate));
        AutoSampleSettings set;
        CHECK(set.hold_auto && set.hold_seconds() == 2.0f); // nothing heard yet
        set.found.heard = true; set.found.hold = 8.0f;
        CHECK(set.hold_seconds() == 8.0f);
        set.found.sustains = false; set.found.hold = 5.0f; // a sound that fades: as long as it is heard,
        CHECK(set.hold_seconds() == 5.0f);
        set.loop_auto = false; set.loop = true;            // but 2 s with LOOP put on: looped while it still sounds
        CHECK(set.hold_seconds() == 2.0f);
        set.hold_auto = false; set.hold = 3.5f;
        CHECK(set.hold_seconds() == 3.5f);
        // a note that has died away: silence is not movement, and there is nothing to loop
        std::vector<float> dies(size_t(rate * 8)), dies2;
        uint32_t seed = 5;
        for (size_t f = 0; f < dies.size(); ++f) {
            seed = seed * 1664525u + 1013904223u;
            dies[f] = float(0.3 * std::sin(2 * M_PI * 110 * f / rate) * std::pow(10.0, -14.0 * (f / rate) / 20)) + float(int32_t(seed)) / 2147483648.0f * 2e-6f;
        }
        dies2 = dies;
        CHECK(!sound_moves(dies, dies.size(), rate) && !find_loop(dies, dies2, dies.size(), rate, true).found());
        CHECK(!probe_sustains(dies, size_t(0.5f * rate), dies.size(), rate, 9.0f) && probe_sustains(swell, size_t(0.5f * rate), swell.size(), rate, 9.0f));
    }
    { // hits: the same at another phase are the same; one 3 dB louder differs
        std::vector<float> a, b, c; double p1 = 0, p2 = 1.3, p3 = 0;
        tone(a, 0, 19200, 220, 220, 0, 0.5f, p1);
        tone(b, 0, 19200, 220, 220, 0, 0.5f, p2);
        tone(c, 0, 19200, 220, 220, 0, 0.71f, p3);
        CHECK(!probe_varies({a, b}, rate) && probe_varies({a, c}, rate));
        // a low saw at another start phase (an oscillator running free): the same
        std::vector<float> s1(19200), s2(19200);
        for (size_t f = 0; f < s1.size(); ++f) {
            const double t = 65.4 * f / rate;
            s1[f] = float(0.5 * (2 * (t - std::floor(t)) - 1));
            s2[f] = float(0.5 * (2 * (t + 0.37 - std::floor(t + 0.37)) - 1));
        }
        CHECK(!probe_varies({s1, s2}, rate));
    }
}

// The auto sampler's loops (engine/loop_find.h): on a steady tone, a start and end a whole number of periods apart and
// a seamless join; none on silence or a short hold.
void loop_finder() {
    const float rate = 48000;
    const double hz = 220;
    std::vector<float> l(size_t(rate * 2)), r(size_t(rate * 2));
    for (size_t f = 0; f < l.size(); ++f) l[f] = r[f] = float(0.5 * std::sin(2 * M_PI * hz * double(f) / rate + 0.3));
    auto loop = find_loop(l, r, uint64_t(rate * 1.5), rate);
    CHECK(loop.found() && loop.match > 0.99f);
    CHECK(loop.end <= uint32_t(rate * 1.5) && loop.end - loop.start >= uint32_t(0.1f * rate) && loop.start >= uint32_t(rate * 1.5 / 4));
    const double periods = double(loop.end - loop.start) * hz / rate;
    CHECK(std::fabs(periods - std::round(periods)) < 0.05);
    CHECK(std::fabs(l[loop.end - 1] - l[loop.start - 1]) < 0.02f); // the frame before the jump meets the one before the start
    { // seven detuned saws: their beating moves the level, so the loop is a long one (a third of the hold or more, from
      // a fifth of the way in at the earliest), not a short one that would freeze a moment of it
        std::vector<float> wl(size_t(rate * 2)), wr;
        const double detune[] {-0.31, -0.19, -0.07, 0, 0.11, 0.23, 0.37};
        for (size_t f = 0; f < wl.size(); ++f) {
            double v = 0;
            for (double d : detune) { const double t = 65.4 * std::pow(2.0, d / 12) * f / rate + d; v += 2 * (t - std::floor(t)) - 1; }
            wl[f] = float(0.08 * v);
        }
        wr = wl;
        auto beating = find_loop(wl, wr, uint64_t(rate * 2), rate);
        CHECK(beating.found() && beating.moving && beating.end - beating.start >= uint32_t(rate * 2 / 3) && beating.start >= uint32_t(rate * 2 / 5));
    }
    { // a tone under a slow tremolo (a swing every 2 s), held 8 s: the loop is whole swings long, so it goes round in step
        std::vector<float> tl(size_t(rate * 8)), tr;
        for (size_t f = 0; f < tl.size(); ++f) {
            const double t = double(f) / rate;
            tl[f] = float(0.4 * std::sin(2 * M_PI * 220 * t) * (0.6 + 0.4 * std::sin(2 * M_PI * 0.5 * t)));
        }
        tr = tl;
        auto swing = find_loop(tl, tr, uint64_t(rate * 8), rate);
        CHECK(swing.found() && swing.moving);
        const double swings = double(swing.end - swing.start) / rate / 2.0;
        CHECK(swings > 0.9 && std::fabs(swings - std::round(swings)) < 0.08);
        // a steady tone held as long does not move: a short loop
        std::vector<float> sl(size_t(rate * 8)), sr;
        for (size_t f = 0; f < sl.size(); ++f) sl[f] = float(0.4 * std::sin(2 * M_PI * 220 * double(f) / rate));
        sr = sl;
        auto still = find_loop(sl, sr, uint64_t(rate * 8), rate);
        CHECK(still.found() && !still.moving && still.end - still.start <= uint32_t(rate));
    }
    { // a drum cut where it falls silent, not at the end of the time its key was held
        NoteAudio n;
        n.left.assign(size_t(rate * 1.5), 0.0f);
        for (size_t f = 0; f < size_t(0.2f * rate); ++f) n.left[f] = float(0.5 * std::sin(f * 0.1) * std::exp(-f / (0.03 * rate)));
        n.right = n.left; n.held = uint64_t(rate * 1.5);
        trim_silence(n, rate);
        CHECK(n.left.size() < size_t(0.25f * rate) && n.held == n.left.size() && n.release == 0);
    }
    { // a DC offset taken out (a pulse wave off centre), the note still starting at silence; a clean note left alone
        std::vector<float> a(static_cast<size_t>(rate)), b;
        for (size_t f = 0; f < a.size(); ++f) a[f] = float(0.3 * std::sin(2 * M_PI * 65.4 * double(f) / rate) - 0.13 * std::min(1.0, f / (0.002 * rate)));
        b = a;
        CHECK(remove_dc(a, b, a.size(), rate));
        double mean = 0;
        for (size_t f = size_t(0.5f * rate); f < a.size(); ++f) mean += a[f];
        CHECK(std::fabs(mean / (a.size() - size_t(0.5f * rate))) < 0.002 && std::fabs(a[0]) < 0.01f);
        std::vector<float> clean(static_cast<size_t>(rate)), clean2;
        for (size_t f = 0; f < clean.size(); ++f) clean[f] = float(0.3 * std::sin(2 * M_PI * 65.4 * double(f) / rate));
        clean2 = clean;
        const auto kept = clean;
        CHECK(!remove_dc(clean, clean2, clean.size(), rate) && clean == kept);
        for (auto& v : a) v = 0; // a quiet plug-in's offset: 0.004 under a peak of 0.07
        for (size_t f = 0; f < a.size(); ++f) a[f] = float(0.06 * std::sin(2 * M_PI * 65.4 * double(f) / rate) - 0.004);
        b = a;
        CHECK(remove_dc(a, b, a.size(), rate));
    }
    std::vector<float> quiet(size_t(rate), 0.0f), quiet2 = quiet;
    CHECK(!find_loop(quiet, quiet2, uint64_t(rate), rate).found());
    CHECK(!find_loop(l, r, uint64_t(0.2f * rate), rate).found());
    // a note fading as it is held (6 dB a second, a 303's): a short loop, levelled so it goes round at one level, and
    // what follows it raised as much, with no step at the loop's end
    std::vector<float> fl(size_t(rate * 2)), fr;
    for (size_t f = 0; f < fl.size(); ++f) fl[f] = float(0.5 * std::sin(2 * M_PI * hz * double(f) / rate) * std::pow(10.0, -6.0 * f / rate / 20));
    fr = fl;
    auto fading = find_loop(fl, fr, uint64_t(rate * 1.5), rate);
    CHECK(fading.found() && fading.end - fading.start < uint32_t(0.3f * rate));
    auto rms = [&](size_t at, size_t n) { double s = 0; for (size_t i = 0; i < n; ++i) s += double(fl[at + i]) * fl[at + i]; return 10 * std::log10(s / n); };
    const size_t w = size_t(0.02f * rate);
    CHECK(std::fabs(rms(fading.start - w, w) - rms(fading.end - w, w)) < 0.3);
    CHECK(std::fabs(rms(fading.end - w, w) - rms(fading.end, w)) < 0.3);
}

void keygroup_reader() {
    CHECK(std::fabs(keygroup_decibels(1.0)) < 1e-9 && std::fabs(keygroup_decibels(0.5) + 6.0206) < 1e-3);
    CHECK(mpc_level(0.707946) == 100 && mpc_level(1.0) == 127 && mpc_level(0) == 0 && mpc_pan(0.5) == 0 && mpc_pan(0.37) == -13);
    CHECK(mpc_filter(1) == PadFilter::Lpf2 && mpc_filter(7) == PadFilter::Hpf2 && mpc_filter(29) == PadFilter::Classic && mpc_filter(20) == PadFilter::Off);
    CHECK(std::fabs(keygroup_seconds(0) - 0.001) < 1e-12 && std::fabs(keygroup_seconds(1) - 100) < 1e-9);
    KeygroupProgram program;
    program.type = "Keygroup";
    program.tags = {{"KeygroupNumKeygroups", "2"}};
    KeygroupInstrument low;
    low.number = 1;
    low.tags = {{"LowNote", "0"}, {"HighNote", "59"}, {"ZonePlay", "1"}, {"TriggerMode", "2"}, {"VolumeRelease", "0.2"},
                {"VolumeSustain", "0.5"}, {"VelocitySensitivity", "0.5"}, {"FilterType", "2"}};
    low.layers = {{{"Active", "True"}, {"SampleName", "Piano C3 "}, {"RootNote", "61"}, {"VelStart", "0"}, {"VelEnd", "64"},
                   {"Volume", "0.353"}, {"Pan", "0"}, {"Pitch", "0"}, {"TuneCoarse", "2"}, {"TuneFine", "10"}, {"SliceLoop", "0"}},
                  {{"Active", "True"}, {"SampleName", "Piano C3 Hard"}, {"RootNote", "61"}, {"VelStart", "65"}, {"VelEnd", "127"},
                   {"Volume", "1.000000"}, {"Pan", "1"}, {"Pitch", "-0.5"}, {"SliceStart", "100"}, {"SliceEnd", "5000"},
                   {"SliceLoop", "1"}, {"SliceLoopStart", "2000"}},
                  {{"Active", "False"}, {"SampleName", "Off"}},
                  {{"Active", "True"}, {"SampleName", ""}}};
    KeygroupInstrument high;
    high.number = 2;
    high.tags = {{"LowNote", "60"}, {"HighNote", "127"}, {"ZonePlay", "0"}, {"TriggerMode", "0"}, {"IgnoreBaseNote", "True"}};
    high.layers = {{{"SampleName", "Hit A"}, {"VelStart", "0"}, {"VelEnd", "60"}, {"KeyTrack", "False"}, {"RootNote", "73"}},
                   {{"SampleFile", "C:\\Samples\\hit b.wav"}, {"VelStart", "61"}, {"VelEnd", "127"}, {"KeyTrack", "True"}, {"Loop", "True"}}};
    KeygroupInstrument past;
    past.number = 3;
    past.layers = {{{"SampleName", "Never"}}};
    program.instruments = {low, high, past};
    auto read = read_keygroup(program);
    CHECK(read.regions.size() == 4 && read.left == 0);
    CHECK(!read.regions[0].root_from_file && !read.regions[2].root_from_file);
    {   // RootNote 0 (MPC-V's own exports): the sample's own root, from its smpl chunk, not C-1
        KeygroupProgram own = program;
        own.instruments = {low};
        for (auto& l : own.instruments[0].layers) if (l.count("RootNote")) l["RootNote"] = "0";
        auto from_file = read_keygroup(own);
        CHECK(from_file.regions.size() == 2 && from_file.regions[0].root_from_file && from_file.regions[0].zone.root == 60);
    }
    const auto& soft = read.regions[0];
    const auto& hard = read.regions[1];
    CHECK(soft.sample == "Piano C3 .WAV" && hard.sample == "Piano C3 Hard.WAV");
    CHECK(soft.zone.low_key == 0 && soft.zone.high_key == 59 && soft.zone.root == 60);
    CHECK(soft.zone.low_velocity == 1 && soft.zone.high_velocity == 64 && hard.zone.low_velocity == 65);
    CHECK(soft.zone.tune == 210 && hard.zone.tune == -50);
    CHECK(std::fabs(soft.zone.gain - 20 * std::log10(0.353)) < 1e-4 && std::fabs(hard.zone.gain) < 1e-4);
    CHECK(soft.zone.pan == -1 && hard.zone.pan == 1);
    CHECK(soft.zone.loop == ZoneLoop::Off && soft.loop_named && !soft.loop_asked);
    CHECK(hard.offset == 100 && hard.end == 5000 && hard.zone.loop == ZoneLoop::On && hard.zone.loop_start == 2000 && hard.zone.loop_end == 5000);
    CHECK(read.ignored.count("FilterType") && read.velocity == 50);
    CHECK(read.envelope.sustain == 50.0 && std::fabs(*read.envelope.release - keygroup_seconds(0.2)) < 1e-9);
    // the cycle: one layer a press, whatever its velocity, a hit a turn
    const auto& a = read.regions[2];
    const auto& b = read.regions[3];
    CHECK(a.sample == "Hit A.WAV" && b.sample == "hit b.wav");
    CHECK(a.zone.low_velocity == 1 && a.zone.high_velocity == 127 && b.zone.low_velocity == 1);
    CHECK(a.zone.sequence == 1 && b.zone.sequence == 2 && a.zone.sequence_length == 2 && b.zone.sequence_length == 2);
    CHECK(a.zone.one_shot && b.zone.one_shot && b.zone.loop == ZoneLoop::Off);
    CHECK(a.zone.keytrack == 0 && b.zone.keytrack == 100 && a.zone.root == 72);
    CHECK(a.zone.release == sfz_release_step(keygroup_seconds(0.63)) - sfz_release_step(keygroup_seconds(0.2)));
    {   // a pick at random splits the dice; a release trigger's layers play on the key let go (roadmap P3)
        KeygroupProgram picked;
        picked.type = "Keygroup";
        KeygroupInstrument one;
        one.tags = {{"ZonePlay", "2"}};
        one.layers = {{{"SampleName", "x"}}, {{"SampleName", "y"}, {"Loop", "True"}}};
        KeygroupInstrument off = one;
        off.tags = {{"TriggerMode", "1"}};
        picked.instruments = {one, off};
        auto r = read_keygroup(picked);
        CHECK(r.regions.size() == 4 && r.left == 0 && !r.regions[1].zone.on_release && r.regions[2].zone.on_release && r.regions[3].zone.one_shot);
        CHECK(r.regions[0].zone.high_random == 0.5f && r.regions[1].zone.low_random == 0.5f);
        // Loop with no points: the file's own loop, if it has one
        CHECK(r.regions[1].zone.loop == ZoneLoop::On && r.regions[1].zone.loop_end == 0 && r.regions[1].loop_asked);
        CHECK(r.envelope.release && std::fabs(*r.envelope.release - keygroup_seconds(0.63)) < 1e-9);
    }
    {   // a keygroup's own volume and tune go onto its zones
        KeygroupProgram tuned;
        tuned.type = "Keygroup";
        KeygroupInstrument one;
        one.tags = {{"Volume", "1.0"}, {"TuneCoarse", "-1"}, {"TuneFine", "5"}};
        one.layers = {{{"SampleName", "x"}, {"Volume", "0.5"}, {"TuneCoarse", "2"}}};
        tuned.instruments = {one};
        auto r = read_keygroup(tuned);
        CHECK(r.regions.size() == 1 && r.regions[0].zone.tune == 105 && std::fabs(r.regions[0].zone.gain - (6 + 20 * std::log10(0.5))) < 1e-3);
    }
    {   // the program's own, over every keygroup: an organ's (Vintage Organs) velocity off, its volume and tune
        KeygroupProgram organ;
        organ.type = "Keygroup";
        organ.tags = {{"Volume", "0.707946"}, {"TuneCoarse", "1"}, {"KeygroupTranspose", "-2"}, {"KeygroupMasterTranspose", "0.500000"},
                      {"KeygroupNumKeygroups", "1"}};
        KeygroupInstrument one;
        one.number = 1;
        one.tags = {{"LowNote", "24"}, {"HighNote", "37"}, {"Volume", "0.46"}, {"VelocitySensitivity", "0.000000"}, {"ZonePlay", "1"}};
        one.layers = {{{"SampleName", "Organ-036 C1"}, {"RootNote", "37"}, {"Volume", "0.71"}},
                      {{"SampleName", "Organ-036 C1 b"}, {"RootNote", "37"}}};
        KeygroupInstrument spare = one;
        spare.number = 2; // past KeygroupNumKeygroups
        organ.instruments = {one, spare};
        auto r = read_keygroup(organ);
        CHECK(r.regions.size() == 2 && r.velocity == 0 && r.ignored.empty());
        CHECK(r.regions[0].zone.root == 36 && r.regions[0].zone.tune == -100 && r.regions[1].zone.low_velocity == 1); // both layers stack
        const double kg = 40 * std::log10(0.46) + 6;
        CHECK(std::fabs(r.regions[0].zone.gain - (kg + 20 * std::log10(0.71))) < 1e-3 && std::fabs(r.regions[1].zone.gain - kg) < 1e-3);
        organ.tags["KeygroupMasterTranspose"] = "0.6";
        CHECK(read_keygroup(organ).ignored.count("KeygroupMasterTranspose"));
    }
}

// 16 Levels in pitch on a kit: the octave − / + set over the pads; a synth's keeps its own.
void kit_levels_octave() {
    auto project = std::make_unique<Project>();
    auto& kit = project->tracks[4];
    kit.engine = EngineKind::Sampler;
    kit.levels = true;
    kit.levels_kind = uint8_t(LevelsKind::Tune);
    CHECK(levels_of(*project, kit, 3).semitones == 0); // the original on pad 4, as it is
    kit.octave = 3;
    CHECK(levels_of(*project, kit, 3).semitones == 12 && levels_of(*project, kit, 3).shown == 12);
    kit.octave = -1;
    kit.levels_kind = uint8_t(LevelsKind::Scale);
    CHECK(levels_of(*project, kit, 0).semitones == 2 - 36); // the sample a C, the root D, three octaves down
    kit.levels_kind = uint8_t(LevelsKind::Velocity);
    CHECK(levels_of(*project, kit, 0).semitones == 0);
    auto& synth = project->tracks[1];
    synth.engine = EngineKind::Plugin;
    synth.levels_kind = uint8_t(LevelsKind::Tune);
    synth.octave = 5;
    CHECK(levels_of(*project, synth, 3).semitones == 0);
    auto bank = std::make_unique<PatternBank>();
    auto bytes = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    CHECK(load_project(bytes, *back, *bank).empty() && back->tracks[4].octave == -1);
}

// A Logic EXS24 instrument's blocks, in either byte order, as Logic lays them out.
struct ExsBytes {
    bool big = false;
    std::string out;
    void put(std::string& c, size_t at, uint32_t v, unsigned width = 4) const {
        for (unsigned n = 0; n < width; ++n) c[at + n] = char(big ? v >> (8 * (width - 1 - n)) : v >> (8 * n));
    }
    void block(int kind, const std::string& name, const std::string& content) {
        std::string h(84, '\0');
        h[0] = char(big ? 0 : 1); h[1] = 1; h[3] = char(kind);
        put(h, 4, uint32_t(content.size()));
        std::memcpy(&h[16], big ? "SOBT" : "TBOS", 4);
        std::memcpy(&h[20], name.data(), std::min<size_t>(name.size(), 63));
        out += h + content;
    }
    struct Zone { int opts = 0, key = 60, fine = 0, pan = 0, volume = 0, low = 0, high = 127, vlow = 0, vhigh = 127, coarse = 0;
                  uint32_t start = 0, end = 0, ls = 0, le = 0; int lopts = 0, dir = 0; int32_t group = 0, sample = 0; };
    void zone(const Zone& z) {
        std::string c(136, '\0');
        c[0] = char(z.opts); c[1] = char(z.key); c[2] = char(z.fine); c[3] = char(z.pan); c[4] = char(z.volume);
        c[6] = char(z.low); c[7] = char(z.high); c[9] = char(z.vlow); c[10] = char(z.vhigh);
        put(c, 12, z.start); put(c, 16, z.end); put(c, 20, z.ls); put(c, 24, z.le);
        c[33] = char(z.lopts); c[34] = char(z.dir); c[80] = char(z.coarse);
        put(c, 88, uint32_t(z.group)); put(c, 92, uint32_t(z.sample));
        block(1, "zone", c);
    }
    struct Group { int volume = 0, pan = 0, options = 0, exclusive = 0, note = 0, select = 0, high_key = 127; int32_t after = -2; };
    void group(const Group& g) {
        std::string c(132, '\0');
        c[0] = char(g.volume); c[1] = char(g.pan); c[3] = char(g.options); c[4] = char(g.exclusive); c[6] = char(127);
        c[75] = char(g.note); put(c, 80, uint32_t(g.after)); c[84] = char(g.select); c[87] = char(127); c[89] = char(g.high_key);
        block(2, "group", c);
    }
    void sample(const std::string& name, const std::string& folder, bool named = true) {
        std::string c(named ? 592 : 336, '\0');
        std::memcpy(&c[80], folder.data(), folder.size());
        if (named) std::memcpy(&c[336], name.data(), name.size());
        block(3, name, c);
    }
    void parameters(const std::map<int, int>& old_ones, const std::map<int, int>& new_ones) {
        std::string c(4 + 300 + 4 + 4 * new_ones.size(), '\0');
        put(c, 0, 100);
        unsigned n = 0;
        for (const auto& [id, value] : old_ones) { c[4 + n] = char(id); put(c, 4 + 100 + 2 * n, uint16_t(int16_t(value)), 2); ++n; }
        put(c, 304, uint32_t(new_ones.size()));
        n = 0;
        for (const auto& [id, value] : new_ones) { put(c, 308 + 4 * n, uint32_t(id), 2); put(c, 310 + 4 * n, uint32_t(value), 2); ++n; }
        block(4, "Default Param", c);
    }
};

std::string exs_instrument(bool big) {
    ExsBytes b;
    b.big = big;
    b.block(0, "Test", std::string(80, '\0'));
    ExsBytes::Zone a; // velocity range on, tuned, panned, trimmed, looped
    a.opts = 8; a.key = 60; a.low = 0; a.high = 63; a.vlow = 0; a.vhigh = 63; a.fine = 10; a.coarse = 1; a.volume = -3; a.pan = 25;
    a.start = 100; a.end = 5000; a.lopts = 1; a.ls = 1000; a.le = 4000; a.group = 0; a.sample = 0;
    b.zone(a);
    ExsBytes::Zone one; // one shot, pitch off, no velocity range (its numbers unused), a loop one shot leaves alone
    one.opts = 1 | 2; one.key = 72; one.low = 64; one.high = 127; one.vlow = 20; one.vhigh = 30; one.lopts = 1; one.le = 10; one.group = 1; one.sample = 1;
    b.zone(one);
    ExsBytes::Zone first; // round robin, a loop to the end played to its end on release, backwards
    first.group = 2; first.sample = 2; first.lopts = 1 | 4; first.dir = 1;
    b.zone(first);
    ExsBytes::Zone second = first;
    second.group = 3; second.lopts = 0;
    b.zone(second);
    ExsBytes::Zone keyed; keyed.group = 4; keyed.sample = 0; b.zone(keyed);
    ExsBytes::Zone other_key; other_key.group = 5; b.zone(other_key);
    ExsBytes::Zone muted; muted.group = 6; b.zone(muted);
    ExsBytes::Zone nowhere; nowhere.group = -1; nowhere.sample = 9; b.zone(nowhere);
    ExsBytes::Zone gone; gone.group = -1; gone.sample = 3; b.zone(gone);
    ExsBytes::Group g0; g0.volume = -2; g0.pan = -10; g0.high_key = 60; b.group(g0);
    ExsBytes::Group g1; g1.exclusive = 3; b.group(g1);
    ExsBytes::Group g2; g2.select = 2; g2.after = -1; b.group(g2);
    ExsBytes::Group g3; g3.select = 2; g3.after = 2; b.group(g3);
    ExsBytes::Group g4; g4.select = 1; g4.note = 24; b.group(g4);
    ExsBytes::Group g5; g5.select = 1; g5.note = 25; b.group(g5);
    ExsBytes::Group g6; g6.options = 16; b.group(g6);
    b.sample("a.aif", "/Volumes/Library/Samples");
    b.sample("b.wav", "/Volumes/Library/Samples", false); // an older block: its name is the file's
    b.sample("c.wav", "/Volumes/Library/Samples");
    b.sample("gone.wav", "/Volumes/Library/Samples");
    b.parameters({{0x07, -6}, {0x0e, 1}, {0x0f, -5}, {0x51, 127}, {0x55, 64}, {0x5a, -30}}, {{0x16d, 0}});
    return b.out;
}

// A Logic EXS24 instrument's zones, groups, samples and parameters as the SFZ reader's zones.
void exs_reader() {
    std::string error;
    CHECK(read_exs("not an instrument at all, though long enough to hold a head of eighty-four bytes, surely", {}, &error).regions.empty() &&
          error == "Not an EXS instrument");
    CHECK(std::fabs(exs_seconds(127) - 10) < 1e-9 && exs_seconds(0) == 0);
    for (bool big : {false, true}) {
        const auto bytes = exs_instrument(big);
        CHECK(exs_magic(bytes));
        std::set<std::string> asked;
        auto read = read_exs(bytes, [&](const std::string& name, const std::string& folder) -> std::string {
            CHECK(folder == "/Volumes/Library/Samples");
            asked.insert(name);
            return name == "gone.wav" ? "" : "Samples/" + name;
        });
        CHECK(asked == std::set<std::string>({"a.aif", "b.wav", "c.wav", "gone.wav"}));
        CHECK(read.regions.size() == 5);
        CHECK(read.left == 4 && read.left_by == std::set<std::string>({"keyswitch", "muted group", "missing files"}));
        CHECK(read.ignored == std::set<std::string>({"loop direction"}));
        CHECK(std::fabs(read.velocity - 50) < 1e-9);
        CHECK(read.envelope.release && std::fabs(*read.envelope.release - exs_seconds(64)) < 1e-9 && read.envelope.sustain == 100.0);
        if (read.regions.size() != 5) continue;
        const auto& a = read.regions[0];
        CHECK(a.sample == "Samples/a.aif" && a.zone.low_key == 0 && a.zone.high_key == 60 && a.zone.root == 60);
        CHECK(a.zone.low_velocity == 1 && a.zone.high_velocity == 63 && a.zone.keytrack == 100);
        CHECK(a.zone.tune == 100 + 10 + 100 - 5 && std::fabs(a.zone.gain + 11) < 1e-6 && std::fabs(a.zone.pan - 0.3f) < 1e-6);
        CHECK(a.offset == 100 && a.end == 5000 && a.loop_named && a.loop_asked);
        CHECK(a.zone.loop == ZoneLoop::On && a.zone.loop_start == 1000 && a.zone.loop_end == 4000);
        const auto& one = read.regions[1];
        CHECK(one.sample == "Samples/b.wav" && one.zone.low_key == 64 && one.zone.low_velocity == 1 && one.zone.high_velocity == 127);
        CHECK(one.zone.one_shot && one.zone.keytrack == 0 && one.zone.loop == ZoneLoop::Off && !one.loop_asked && one.loop_named);
        CHECK(one.zone.group == 3 && one.zone.off_by == 3 && one.offset == -1 && one.end == -1 && one.zone.tune == 95);
        const auto& first = read.regions[2];
        const auto& second = read.regions[3];
        CHECK(first.zone.sequence == 1 && first.zone.sequence_length == 2 && second.zone.sequence == 2 && second.zone.sequence_length == 2);
        CHECK(first.zone.loop == ZoneLoop::UntilReleased && first.zone.loop_end == 0x7fffffff && second.zone.loop == ZoneLoop::Off);
        CHECK(read.regions[4].zone.sequence_length == 0 && read.regions[4].sample == "Samples/a.aif");
    }
    // Cut short: the blocks that came whole are read.
    auto whole = exs_instrument(false);
    auto cut = read_exs(whole.substr(0, whole.size() - 500), {});
    CHECK(cut.regions.size() == 5 && cut.envelope.release == std::nullopt);
}


// An expansion's zip entries stay inside its folder.
void expansion_paths() {
    CHECK(expansion_entry_path("Expansion.xml") == "Expansion.xml");
    CHECK(expansion_entry_path("Organ_[TrackData]/Organ-036 C1.wav") == "Organ_[TrackData]/Organ-036 C1.wav");
    CHECK(expansion_entry_path("a\\b//./c.wav") == "a/b/c.wav");
    CHECK(expansion_entry_path("/abs/c.wav") == "abs/c.wav");
    CHECK(expansion_entry_path("../evil.wav").empty() && expansion_entry_path("a/../../evil.wav").empty());
    CHECK(expansion_entry_path("C:\\evil.wav").empty() && expansion_entry_path("").empty());
}

// A drum program's pad, as the Dirty Drummer kits have them: four hits cycled,
// the pad's own level, pan, filter and envelope; a one-layer pad plays its WAV.
void drum_program_pads() {
    KeygroupInstrument kick;
    kick.number = 1;
    kick.tags = {{"ZonePlay", "0"}, {"TriggerMode", "0"}, {"Volume", "0.707946"}, {"Pan", "0.370000"}, {"Mono", "True"},
                 {"MuteGroup", "2"}, {"VolumeDecay", "0.050000"}, {"VolumeDecayType", "True"}, {"FilterType", "1"},
                 {"Cutoff", "0.732284"}, {"TuneCoarse", "-1"}, {"VelocitySensitivity", "0.5"}, {"LowNote", "0"}, {"HighNote", "127"}};
    for (const char* hit : {"Acoustic-Kick-dd2 dk k_p1", "Acoustic-Kick-dd2 dk k_p2", "Acoustic-Kick-dd2 dk k_p3", "Acoustic-Kick-dd2 dk k_p4"})
        kick.layers.push_back({{"Active", "True"}, {"SampleName", hit}, {"VelStart", "0"}, {"VelEnd", "127"}, {"RootNote", "0"},
                               {"Volume", "1.000000"}, {"Pan", "0.500000"}, {"Pitch", "-4.000000"}, {"TuneCoarse", "-4"}, {"KeyTrack", "False"},
                               {"SliceLoop", "0"}, {"SliceStart", "0"}, {"SliceEnd", "17557"}});
    kick.layers.push_back({{"Active", "True"}, {"SampleName", ""}});
    CHECK(keygroup_layers(kick).size() == 4);
    auto read = read_drum_pad(kick);
    CHECK(read.regions.size() == 4 && read.left == 0);
    for (unsigned n = 0; n < 4; ++n) {
        const auto& z = read.regions[n].zone;
        CHECK(z.low_key == 0 && z.high_key == 127 && z.root == 60 && z.keytrack == 100); // RootNote 0 is not C-1 here
        CHECK(z.sequence == n + 1 && z.sequence_length == 4 && z.one_shot && z.tune == -400 && z.gain == 0);
        CHECK(read.regions[n].end == 17557);
    }
    CHECK(read.regions[0].sample == "Acoustic-Kick-dd2 dk k_p1.WAV");
    CHECK(!read.envelope.attack && !read.envelope.release && read.regions[2].zone.release == 0); // the pad's envelope, not the zones'
    PadSound pad;
    keygroup_pad(kick, 1.0, pad);
    CHECK(pad.level == 100 && pad.pan == -13 && pad.mono && pad.group == 2 && pad.play == PadPlay::OneShot);
    CHECK(pad.pitch == -1 && pad.fine == 0 && pad.start == 0 && pad.end == 0); // the layers' tune and region are the zones'
    CHECK(pad.filter == PadFilter::Lpf2 && pad.cutoff == 73 && pad.decay == 5 && !pad.decay_from_start && pad.velocity_sense == 50);
    KeygroupInstrument snare = kick;
    snare.layers.resize(1);
    snare.layers[0]["Volume"] = "0.63";
    snare.layers[0]["SliceLoop"] = "1";
    snare.layers[0]["SliceLoopStart"] = "100";
    snare.tags["Cutoff"] = "1.0";
    snare.tags["FilterType"] = "1";
    PadSound one;
    keygroup_pad(snare, 2.0, one);
    CHECK(one.pitch == -5 && one.end == 2 * 17557 && one.loop && !one.loop_lock && one.loop_start == 200);
    CHECK(one.gain == int16_t(std::lround(20 * std::log10(0.63) * 100)) && one.filter == PadFilter::Off); // wide open: off
}

// Laid out once and found again; chunks whole pages; the least lately played going first.
void sample_cache() {
    namespace fs = std::filesystem;
    auto base = fs::temp_directory_path() / ("melodee-cache-" + std::to_string(getpid()));
    fs::create_directories(base / "Cache");
    SampleCache cache;
    cache.set_folder((base / "Cache").string());
    auto source = (base / "PP C#1.wav").string();
    { std::ofstream(source) << "abc"; }
    auto file = cache.file_of(source);
    CHECK(!file.empty() && !cache.file_of((base / "gone.wav").string()).size());
    LaidOut info;
    CHECK(!cache.ready(file, info));
    SampleData data;
    data.frames = 40000; data.left.resize(40000); data.right.resize(40000);
    for (unsigned n = 0; n < 40000; ++n) { data.left[n] = float(n); data.right[n] = -float(n); }
    LaidOut out; out.frames = 40000; out.channels = 2; out.source_rate = 48000; out.loop_start = 7; out.loop_end = 99;
    CHECK(cache.lay_out(file, data, out).empty());
    CHECK(!fs::exists(file + ".part"));
    CHECK(cache.ready(file, info) && info.frames == 40000 && info.channels == 2 && info.source_rate == 48000 && info.loop_start == 7 && info.loop_end == 99);
    {
        ZonePart part;
        CHECK(SampleCache::map(file, info, part));
        CHECK(part.left[39999] == 39999.0f && part.right[39999] == -39999.0f && part.left[0] == 0.0f);
        CHECK(part.pins.size() == 3);
        // both sides start on a chunk boundary of the file, so a chunk is whole pages
        CHECK((reinterpret_cast<uintptr_t>(part.left) - reinterpret_cast<uintptr_t>(part.mapping)) % stream_chunk_bytes == 0);
        CHECK((reinterpret_cast<uintptr_t>(part.right) - reinterpret_cast<uintptr_t>(part.mapping)) % stream_chunk_bytes == 0);
    }
    { // one side has no right
        auto mono = (base / "Cache/mono.f32").string();
        LaidOut one; one.frames = 100; one.channels = 1;
        SampleData m; m.frames = 100; m.left.assign(100, 0.5f); m.right = m.left;
        CHECK(cache.lay_out(mono, m, one).empty());
        LaidOut got;
        CHECK(cache.ready(mono, got) && got.channels == 1);
        ZonePart part;
        CHECK(SampleCache::map(mono, got, part) && part.right == part.left && part.left[99] == 0.5f);
        CHECK(fs::file_size(mono) == SampleCache::bytes_of(100, 1));
    }
    { std::ofstream(file, std::ios::app) << "x"; } // not the size it says: laid out again
    CHECK(!cache.ready(file, info));
    { std::ofstream(source, std::ios::app) << "more"; }
    CHECK(cache.file_of(source) != file); // the source changed
    { // two packs' C3.wav of one size and date: apart
        fs::create_directories(base / "A"); fs::create_directories(base / "B");
        std::ofstream(base / "A/C3.wav") << "abc"; std::ofstream(base / "B/C3.wav") << "abc";
        fs::last_write_time(base / "B/C3.wav", fs::last_write_time(base / "A/C3.wav"));
        CHECK(cache.file_of((base / "A/C3.wav").string()) != cache.file_of((base / "B/C3.wav").string()));
        CHECK(cache.file_of((base / "A/C3.wav").string()) == cache.file_of((base / "A/../A/C3.wav").string()));
    }
    fs::remove_all(base / "Cache");
    fs::create_directories(base / "Cache");
    for (unsigned n = 0; n < 3; ++n) {
        auto name = base / "Cache" / (std::to_string(n) + ".f32");
        std::ofstream(name) << std::string(1000, 'x');
        fs::last_write_time(name, fs::file_time_type::clock::now() - std::chrono::hours(10 - n));
    }
    { std::ofstream(base / "Cache/other.txt") << std::string(5000, 'x'); }
    { std::ofstream(base / "Cache/3.f32.part") << "x"; } // a stop mid-write
    { // not played for a long time
        std::ofstream(base / "Cache/old.f32") << "x";
        fs::last_write_time(base / "Cache/old.f32", fs::file_time_type::clock::now() - std::chrono::hours(24 * (cache_unplayed_days + 1)));
    }
    cache.tidy(2000);
    CHECK(!fs::exists(base / "Cache/0.f32") && fs::exists(base / "Cache/1.f32") && fs::exists(base / "Cache/2.f32") && fs::exists(base / "Cache/other.txt"));
    CHECK(!fs::exists(base / "Cache/3.f32.part") && !fs::exists(base / "Cache/old.f32"));
    fs::remove_all(base);
}

// The pager pins ahead of a voice and lets go behind; the audio thread sees only what is safe.
std::vector<std::pair<uintptr_t, bool>>* pin_log = nullptr;
bool pin_fails = false;
bool logged_pin(const void* address, size_t, bool lock) {
    if (lock && pin_fails) return false;
    if (pin_log) pin_log->push_back({reinterpret_cast<uintptr_t>(address), lock});
    return true;
}

// A platform's worker setup (engine/render_pool.h): run once on each worker's own thread, before its first job.
std::thread::id setup_caller;
std::atomic<int> setups_run {0};
std::atomic<int> setup_on_worker {0};
void count_setup() {
    setups_run.fetch_add(1);
    if (std::this_thread::get_id() != setup_caller) setup_on_worker.fetch_add(1);
}
void render_worker_setup() {
    setup_caller = std::this_thread::get_id();
    set_worker_setup(count_setup);
    {
        RenderPool pool;
        pool.start(2, 0);
        std::atomic<int> jobs {0};
        for (int round = 0; round < 3; ++round) pool.run(4, [&](unsigned) { jobs.fetch_add(1); });
        CHECK(jobs.load() == 12);
        for (int wait = 0; wait < 1000 && setups_run.load() < 2; ++wait) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        pool.stop();
    }
    set_worker_setup(nullptr);
    CHECK(setups_run.load() == 2 && setup_on_worker.load() == 2); // once a worker, on the worker, not again a run
}

// Rendering on several cores (engine/render_pool.h): a busy project sounds the same, sample for sample, with
// render workers sharing every block as with the audio thread alone; and the workers did share them.
void parallel_render() {
    auto render = [](unsigned workers, uint64_t* helped) {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        const EffectKind chains[][2] {{EffectKind::Reverb, EffectKind::Echo}, {EffectKind::Chorus, EffectKind::Drive},
                                      {EffectKind::Phaser, EffectKind::Plate}, {EffectKind::Crush, EffectKind::Filter},
                                      {EffectKind::Compressor, EffectKind::Room}, {EffectKind::TapeEcho, EffectKind::Width},
                                      {EffectKind::Flanger, EffectKind::Shimmer}, {EffectKind::Tape, EffectKind::PingPong}};
        for (unsigned t = 0; t < 8; ++t) {
            if (t % 2) tone_kit(*p, t); else synth(p->tracks[t], t); // synths (plug-ins) and kits in turn
            p->tracks[t].mix.pan = int8_t(int(t) * 12 - 42);
            p->tracks[t].mix.sends[t % bus_count] = uint8_t(20 + t * 5);
            p->tracks[t].mix.sends[(t + 1) % bus_count] = uint8_t(10 + t * 3); // two sends a track: two buses fed
            for (unsigned k = 0; k < 2; ++k) p->tracks[t].fx.slots[k] = {uint8_t(chains[t][k]), false, {60, 50, 40, 50, 30, 0}};
        }
        p->buses[1].fx.slots[0] = {uint8_t(EffectKind::Reverb), false, {70, 50, 40, 50, 0, 0}};
        EngineHost host;
        host.set_render_threads(workers, 0);
        host.load(*p, *b);
        auto tones = std::make_unique<Tones>();
        tones->offer(host, *p);
        auto tone = tone_sample();
        CHECK(host.offer_sample(0, &tone));
        std::vector<int32_t> block(256), all;
        for (unsigned n = 0; n < 600; ++n) {
            if (n % 50 == 0)
                for (unsigned t = 0; t < 8; ++t) { // a kit's notes are its first four pads
                    auto key = [&](unsigned at) { return uint16_t(t % 2 ? sampler_first_note + (t + at) % 4 : 48 + t + at % 2 * 7); };
                    host.send({Command::Type::NoteOff, uint8_t(t), 0, key(n / 50 + 1), 0});
                    host.send({Command::Type::NoteOn, uint8_t(t), 0, key(n / 50), int16_t(90 + t)});
                }
            host.render(block.data(), 128, 1);
            all.insert(all.end(), block.begin(), block.end());
        }
        if (helped) *helped = host.render_parallel_blocks();
        return all;
    };
    uint64_t helped = 0;
    auto alone = render(0, nullptr);
    auto shared = render(3, &helped);
    double energy = 0;
    for (int32_t s : alone) energy += std::fabs(s / 2147483648.0);
    CHECK(energy > 100);           // it plays
    CHECK(helped == 600);          // every block went to the workers
    CHECK(shared == alone);        // and sounds the same
    // Twice more: the workers sleep and wake between runs without losing a block.
    for (int again = 0; again < 2; ++again) CHECK(render(3, nullptr) == alone);
}

// Roadmap P2, one dispatch a block: a busy project (synths as plug-ins and kits of samples; ratchets with ramps,
// slides and ties, accents, Motion and locks on engine values, kit pads, effects and the wheel; an arp swung; notes,
// pads and values in by hand at their frames, mid-block) plays the same as the old way, where every track was cut
// wherever any track had a tick or a note and the workers were called for each run: the same to the sample, cut
// where the old way cut it or whole between each engine's own calls, with the workers or without.
void one_dispatch() {
    SampleData tone;
    tone.frames = 22050; tone.left.resize(22050); tone.right.resize(22050);
    for (unsigned n = 0; n < 22050; ++n) tone.left[n] = tone.right[n] = float(std::sin(n * 0.05) * 0.4 * std::exp(-double(n) / 6000.0));
    auto play = [&](bool whole, bool cut_as_split, unsigned workers, std::vector<float>* stems_out) {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        for (unsigned t = 0; t < 8; ++t) {
            if (t == 4 || t == 5) tone_kit(*p, t); else synth(p->tracks[t], t);
            p->tracks[t].mix.pan = int8_t(int(t) * 12 - 42);
            p->tracks[t].mix.sends[t % bus_count] = uint8_t(20 + t * 5);
        }
        p->tracks[7].fx.slots[0] = {uint8_t(EffectKind::Filter), false, {60, 50, 40, 50, 30, 0}};
        p->tracks[1].fx.slots[0] = {uint8_t(EffectKind::Chorus), false, {60, 50, 40, 50, 30, 0}};
        p->buses[0].fx.slots[0] = {uint8_t(EffectKind::Reverb), false, {70, 50, 40, 50, 0, 0}};
        for (unsigned t : {4u, 5u})
            for (unsigned pad = 0; pad < 8; ++pad) { p->tracks[t].pads[pad].sample = 0; p->tracks[t].pads[pad].play = PadPlay::OneShot; }
        p->tempo = 1370;
        p->swing = 58;
        auto& bass = b->at[0][0]; // slides, ties and an accent
        put(bass, 0, 36, 100, 2, step_slide); put(bass, 2, 43, 90, 1, step_tie); put(bass, 3, 43, 80); put(bass, 6, 39, 110, 1, step_accent);
        put(bass, 9, 41, 70, 3); put(bass, 13, 48, 100, 1, step_slide); put(bass, 14, 36, 90);
        CHECK(bass.write_motion(point(1, engine_lane, 20)) && bass.write_motion(point(5, engine_lane, 90, 0, 6)) && bass.write_motion(point(10, engine_lane + 1, 70)));
        CHECK(bass.write_motion(point(7, motion_wheel, 120)) && bass.write_motion(point(12, motion_wheel, 10)));
        for (unsigned t : {1u, 3u, 6u}) {
            auto& q = b->at[t][0];
            for (unsigned s = 0; s < 16; s += 1 + t % 3) put(q, s, uint8_t(55 + (s * 5 + t) % 17), uint8_t(60 + s * 3), 1, s % 5 == 0 ? step_accent : 0);
            q.events[1].ratchet = 3; q.events[1].ramp = -40;
            q.events[3].ratchet = 5; q.events[3].ramp = 60;
        }
        CHECK(b->at[6][0].write_motion(point(2, engine_lane, 10, 0, 12)) && b->at[6][0].write_motion(point(9, engine_lane + 2, 80)));
        CHECK(b->at[7][0].write_motion(point(4, motion_fx_base + 0, 10)) && b->at[7][0].write_motion(point(11, motion_fx_base + 1, 90, 0, 24)));
        put(b->at[7][0], 0, 60, 100, 8); put(b->at[7][0], 8, 67, 100, 8);
        auto& drums = b->at[4][0]; // a kit's pads 1, 3 and 7
        for (unsigned s = 0; s < 16; ++s) {
            Step st;
            st.notes[0] = {uint8_t(sampler_first_note + (s % 4 == 0 ? 0 : s % 2 ? 6 : 2)), uint8_t(80 + s)};
            drums.set_step(s, st);
        }
        drums.events[2].ratchet = 4; drums.events[2].ramp = -60;
        auto& kit = b->at[5][0];
        for (unsigned s = 0; s < 16; s += 2) put(kit, s, uint8_t(sampler_first_note + s / 2 % 4), 100);
        kit.events[1].ratchet = 3; kit.events[1].pitch_ramp = 7;
        CHECK(kit.write_motion(point(3, motion_pad_base + unsigned(PadParam::Pitch), 5, 1)) &&
              kit.write_motion(point(6, motion_pad_base + unsigned(PadParam::FilterCutoff), 30, 0, 12)));
        EngineHost host;
        host.set_render_threads(workers, 0);
        host.set_one_dispatch(whole);
        host.set_split_cuts(cut_as_split);
        host.load(*p, *b);
        CHECK(host.offer_sample(0, &tone));
        auto tones = std::make_unique<Tones>();
        tones->offer(host, *p);
        host.send({Command::Type::Arp, 2, 0, 0, 1}); // the arp on a synth, its keys held
        host.send({Command::Type::Arp, 2, 0, 2, 3});
        for (uint16_t key : {60, 64, 67}) host.send({Command::Type::ArpKey, 2, 0, key, 100});
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        std::vector<float> all, stems(size_t(track_count) * EngineHost::block);
        auto timed = [&](Command c, uint64_t frame) { c.timed = true; c.at = uint32_t(frame); host.send(c); };
        for (unsigned n = 0; n < 700; ++n) {
            uint64_t at = uint64_t(n) * 128 + 128; // the next block's
            if (n % 9 == 3) { // by hand, at their frames inside the next block, and values behind them
                timed({Command::Type::NoteOn, 1, 0, uint16_t(70 + n % 5), 90}, at + 17 + n % 60);
                host.send({Command::Type::Param, 1, 0, uint16_t(n % 7), int16_t(n % 100)});
                timed({Command::Type::PadNote, 5, 64 + 3, 2, 110}, at + 40 + n % 50);
                timed({Command::Type::Pressure, 0, 0, 36, int16_t(n % 127)}, at + 70);
                timed({Command::Type::NoteOff, 1, 0, uint16_t(70 + (n + 4) % 5)}, at + 100);
                host.send({Command::Type::Wheel, 6, 0, 0, int16_t(n * 7 % 1000)});
                Command fx {Command::Type::PadFxParam, 5, 0, 0, int16_t(n % 90)};
                host.send(fx);
            }
            host.render(out.data(), 128, 1, nullptr, stems.data());
            for (unsigned f = 0; f < 256; ++f) all.push_back(out[f] / 2147483648.0f);
            if (stems_out) stems_out->insert(stems_out->end(), stems.begin(), stems.begin() + 8 * 128);
        }
        CHECK(host.calls_lost() == 0);
        return all;
    };
    std::vector<float> old_stems, new_stems;
    auto old_way = play(false, false, 0, &old_stems);
    float loudest = peak_of(old_way);
    CHECK(loudest > 0.1f);                                     // it plays
    for (unsigned t = 0; t < 8; ++t) { // every track sounds
        float peak = 0;
        for (size_t n = 0; n < old_stems.size() / 1024; ++n) peak = std::max(peak, peak_of(old_stems, n * 1024 + t * 128, n * 1024 + t * 128 + 128));
        CHECK(peak > 0.01f);
    }
    CHECK(play(false, false, 3, nullptr) == old_way);          // the old way on the workers
    CHECK(play(true, true, 0, nullptr) == old_way);            // each track cut where the old way cut it
    CHECK(play(true, true, 3, nullptr) == old_way);
    CHECK(play(true, false, 0, nullptr) == old_way);           // whole: each engine cut only at its own calls
    CHECK(play(true, false, 3, &new_stems) == old_way);        // on the workers, one dispatch a block
    CHECK(new_stems == old_stems);                             // and every track's stem the same, to the sample
}

// Roadmap P2, the overload guard: blocks near their time make the next ones lighter. Over 85 % the busiest track gives
// up its oldest voice each block (eight held notes gone in eight blocks, the oldest first, each fading in tens of ms,
// not a click); between 70 and 85 % no voice is stolen but a released note's long tail ends in tens of ms; under 70 %
// a tail rings as long as its release says.
// A project load parked (EngineHost::park): the audio thread fades out, plays silence touching nothing while the
// loader builds the next project on its own thread, then fades in on it, with what was sent meanwhile after it.
void parked_load() {
    { // DelayPool::scrub: a free block written since its clear is cleared, and the next take finds it clean
        DelayPool pool;
        bool clean = false;
        float* a = pool.take(clean);
        CHECK(a && clean);
        a[5] = 1;
        pool.give(a);
        CHECK(pool.scrub() == 1 && pool.scrub() == 0);
        float* b = pool.take(clean);
        CHECK(b && clean && b[5] == 0);
        pool.give(b);
    }
    auto a = std::make_unique<Project>();
    auto ab = std::make_unique<PatternBank>();
    synth(a->tracks[0], 0);
    a->tempo = 1200;
    a->tracks[0].fx.slots[0] = {uint8_t(EffectKind::Echo), false, {60, 50, 40, 50, 30, 0}};
    auto b = std::make_unique<Project>();
    auto bb = std::make_unique<PatternBank>();
    synth(b->tracks[0], 0);
    b->tempo = 900;
    b->tracks[0].fx.slots[0] = {uint8_t(EffectKind::Reverb), false, {70, 50, 40, 50, 0, 0}};
    put(bb->at[0][0], 0, 60, 100, 4);
    { // called off before the audio thread came to it: no fade, nothing waits
        EngineHost host;
        host.load(*a, *ab);
        host.set_parking(true);
        uint32_t serial = host.park();
        CHECK(serial != 0);
        host.resume(serial);
        host.send({Command::Type::Tempo, 0, 0, 0, 1111});
        std::vector<int32_t> out(256);
        for (unsigned n = 0; n < 8; ++n) host.render(out.data(), 128, 1);
        CHECK(host.parks() == 0 && !host.parked(serial) && host.tempo_now() == 1111);
    }
    EngineHost host;
    host.load(*a, *ab);
    tones_for(host, *a);
    host.set_parking(true);
    host.send({Command::Type::NoteOn, 0, 0, 60, 100}); // held: the fade is heard on it
    std::vector<float> heard;
    std::atomic<bool> stop {false};
    std::thread audio([&] { // the audio thread, a block at a time
        std::vector<int32_t> out(256);
        while (!stop.load()) {
            host.render(out.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) heard.push_back(float(out[2 * f]) / 2147483648.0f);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    uint32_t serial = host.park();
    CHECK(serial != 0);
    host.send({Command::Type::Tempo, 0, 0, 0, 1337}); // after the Park: lands on the new project, after its build
    bool parked = false;
    for (unsigned n = 0; n < 2000 && !(parked = host.parked(serial)); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(parked && host.parks() == 1);
    auto pack = std::make_unique<ProjectPack>(*b, *bb);
    host.reload(*pack, false); // here, on the loader's thread
    std::this_thread::sleep_for(std::chrono::milliseconds(30)); // parked a while longer: still silent
    host.send({Command::Type::Play}); // waits behind the Park too: B plays from the first block it fades in on
    host.resume(serial);
    pack.reset(); // the last project's patterns freed by the loader
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop = true;
    audio.join();
    CHECK(host.tempo_now() == 1337 && host.playing());
    // Heard: the held note, faded out over Ease::seconds into a stretch of exact silence, then the new project faded in.
    size_t run = 0, from = 0, longest = 0, longest_from = 0;
    for (size_t n = 0; n < heard.size(); ++n) {
        if (heard[n] == 0) { if (!run++) from = n; if (run > longest) { longest = run; longest_from = from; } }
        else run = 0;
    }
    const size_t fade = size_t(Ease::seconds * dsp::sample_rate());
    CHECK(longest >= size_t(0.025 * dsp::sample_rate())); // at least the 30 ms the loader held it
    size_t z = longest_from, e = longest_from + longest;
    CHECK(z > 3 * fade && e + 4000 < heard.size());
    float held = peak_of(heard, z - 3 * fade, z - fade);
    CHECK(held > 0.02f);
    CHECK(peak_of(heard, z - 32, z) < 0.25f * held);        // the fade's foot, nearly silent
    CHECK(peak_of(heard, z - fade, z) <= held * 1.05f);      // and nothing louder on the way down
    float after = peak_of(heard, e + 2 * fade, e + 4000);
    CHECK(after > 0.02f);
    CHECK(peak_of(heard, e, e + 16) < 0.25f * after);         // the new project faded in, not jumped to
}

void overload_guard() {
    auto p = std::make_unique<Project>();
    auto b = std::make_unique<PatternBank>();
    tone_kit(*p, 0); // a kit, Melodee's own engine with voices to give
    for (unsigned pad = 0; pad < 8; ++pad) {
        auto& sound = p->tracks[0].pads[pad];
        sound.sample = 0; sound.play = PadPlay::Gate; sound.release = 90; // held while held, a long tail
    }
    EngineHost host;
    host.set_overload_guard(true);
    host.load(*p, *b);
    auto tone = tone_sample();
    CHECK(host.offer_sample(0, &tone));
    std::vector<int32_t> out(256);
    auto loud = [&](unsigned blocks) {
        float most = 0;
        for (unsigned n = 0; n < blocks; ++n) { host.render(out.data(), 128, 1); for (auto x : out) most = std::max(most, std::fabs(x / 2147483648.0f)); }
        return most;
    };
    for (uint16_t pad = 0; pad < 8; ++pad) host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note + pad), 25});
    CHECK(loud(20) > 0.05f);
    host.set_guard_test_share(0.9f);
    loud(4);
    CHECK(host.guard_steals() == 4 && host.guard_blocks() == 4); // one voice a block, from the busiest (the only) track
    CHECK(loud(4) > 0.01f);                                        // four still held
    loud(8);
    CHECK(host.guard_steals() == 8);                              // every held voice gone, then nothing left to take
    CHECK(loud(10) < 1e-4f);                                       // faded out, quickly
    // Tails: a released note rings on (its release 90) unless the guard is easing the blocks: over 85 % as it is let
    // go, then 75 % (still easing, as it came down from over 85 %), nothing held to steal.
    auto tail = [&](float first, float then) {
        host.set_guard_test_share(0.5f);
        host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note), 100});
        loud(20);
        host.set_guard_test_share(first);
        host.send({Command::Type::NoteOff, 0, 0, uint16_t(sampler_first_note)});
        loud(1);
        host.set_guard_test_share(then);
        loud(11); // 1536 frames in all, 35 ms at 44.1 kHz
        return loud(1);
    };
    uint32_t stolen = host.guard_steals();
    CHECK(tail(0.9f, 0.75f) < 1e-4f && host.guard_steals() == stolen); // hurried, nothing stolen
    CHECK(tail(0.75f, 0.75f) > 0.01f);                                   // 75 % from below: no guard, the whole release
}

// Samples at their own rate: resample() (for what needs another rate for a while: the stems program) lines up with its
// input (an impulse lands where its time says), passes DC and the audible band whole and keeps images out; a file is
// held at its own rate, never converted, with no copy at the device's; a voice reads it at its rate over the
// device's, and its taps come back in 44.1 kHz frames.
void sample_rates_p2() {
    // The converter: an impulse at 44.1 kHz frame 1000 lands at 48 kHz frame 1088 (1000 x 48000 / 44100 = 1088.4).
    std::vector<float> l(4410, 0.0f), r(4410, 0.0f), out_l, out_r;
    l[1000] = r[1000] = 1;
    resample(l.data(), r.data(), 4410, 44100, 48000, out_l, out_r);
    CHECK(out_l.size() == 4800 && out_r.size() == 4800);
    size_t peak = size_t(std::max_element(out_l.begin(), out_l.end()) - out_l.begin());
    CHECK(peak == 1088 || peak == 1089);
    // DC through whole; a 10 kHz tone kept at its level; 22 kHz (above 44.1's band) nothing to fold.
    auto tone = [&](double hz, unsigned from, unsigned to) {
        std::vector<float> a(from / 2), b, c;
        for (size_t n = 0; n < a.size(); ++n) a[n] = float(hz ? std::sin(2 * M_PI * hz * double(n) / from) : 1.0);
        resample(a.data(), a.data(), uint32_t(a.size()), from, to, b, c);
        double sum = 0;
        for (size_t n = b.size() / 4; n < b.size() * 3 / 4; ++n) sum += double(b[n]) * b[n];
        return std::sqrt(sum / double(b.size() / 2));
    };
    CHECK(std::fabs(tone(0, 44100, 48000) - 1.0) < 1e-3);
    CHECK(std::fabs(tone(10000, 44100, 48000) - std::sqrt(0.5)) < 0.01);
    CHECK(tone(30000, 96000, 44100) < 1e-3); // above 44.1 kHz's band: kept out, not folded down to 14.1 kHz
    // A 48 kHz WAV is read as it is: its own frames at 48 kHz, no copy at any other rate; 4410 frames as the project
    // counts them.
    const auto path = std::filesystem::temp_directory_path() / "melodee_p2_48k.wav";
    {
        std::ofstream file(path, std::ios::binary);
        auto put = [&](uint32_t v, unsigned bytes) { for (unsigned n = 0; n < bytes; ++n) file.put(char((v >> (8 * n)) & 255)); };
        const unsigned frames = 4800;
        file.write("RIFF", 4); put(36 + frames * 4, 4); file.write("WAVEfmt ", 8);
        put(16, 4); put(1, 2); put(2, 2); put(48000, 4); put(48000 * 4, 4); put(4, 2); put(16, 2);
        file.write("data", 4); put(frames * 4, 4);
        for (unsigned n = 0; n < frames; ++n) { int16_t v = int16_t((n % 100) * 300 - 15000); put(uint16_t(v), 2); put(uint16_t(v), 2); }
    }
    SampleData read;
    CHECK(read_wav(path.string(), read).empty());
    CHECK(read.frames == 4800 && read.rate == 48000 && read.model_frames() == 4410 && read.left.size() == 4800 && read.bytes() == 4800 * 8);
    for (unsigned n = 0; n < 4800; ++n) CHECK(read.left[n] == float(int16_t((n % 100) * 300 - 15000)) / 32768.0f);
    CHECK(read.own(4410) == 4800 && read.own(2205) == 2400 && read.model(2400) == 2205 && read.own(9999) == 4800);
    std::filesystem::remove(path);
    // A voice reads a sample's own frames, its rate over the device's each device frame; its taps in 44.1 kHz frames.
    const float was = dsp::device_rate.load();
    struct Case { unsigned sample, device; };
    for (Case c : {Case {48000, 48000}, Case {44100, 48000}, Case {48000, 44100}, Case {96000, 48000}}) {
        dsp::device_rate.store(float(c.device));
        SampleData ramp;
        ramp.rate = c.sample; ramp.frames = c.sample; ramp.left.resize(c.sample); ramp.right.resize(c.sample);
        for (unsigned n = 0; n < c.sample; ++n) ramp.left[n] = ramp.right[n] = float(std::sin(n * 0.01) * 0.5);
        SamplerEngine::Table table {};
        table[0] = &ramp;
        SamplerEngine engine;
        engine.set_samples(&table);
        engine.set_tempo(1200);
        PadSound pad; pad.sample = 0; pad.play = PadPlay::OneShot; pad.start = 4410; // a tenth of a second in
        engine.set_pad(0, pad);
        engine.note_on(int(sampler_first_note), 127);
        const unsigned out = c.device / 10; // a tenth of a second at the device's rate
        std::vector<float> a(out, 0.0f), b(out, 0.0f);
        for (unsigned at = 0; at < out; at += 128) engine.render_stereo(a.data() + at, b.data() + at, std::min(128u, out - at));
        TrackTaps taps;
        engine.taps(taps);
        CHECK(taps.count == 1 && taps.voice[0].from == 4410 && std::abs(int(taps.voice[0].at) - (4410 + 4410)) <= 1); // 0.1 s on
        CHECK(std::fabs(taps.voice[0].step - 44100.0f / float(c.device)) < 1e-4f);
        // What it played is its own frames from 0.1 s on, a device frame reading rate / device of them, read between
        // two frames where that falls between, only shaped by the pad's level and pan.
        const double step = double(c.sample) / c.device, first = c.sample / 10.0;
        auto own_at = [&](double x) {
            const auto i = size_t(x);
            const float t = float(x - double(i));
            return ramp.left[i] + (ramp.left[i + 1] - ramp.left[i]) * t;
        };
        float ratio = 0;
        for (unsigned f = out / 2; f < out; ++f) if (std::fabs(own_at(first + f * step)) > 0.2f) { ratio = a[f] / own_at(first + f * step); break; }
        CHECK(ratio > 0.1f);
        for (unsigned f = out / 2; f < out; ++f) CHECK(std::fabs(a[f] - own_at(first + f * step) * ratio) < 1e-4f);
    }
    dsp::device_rate.store(was);
}

// Roadmap P2: the Looper's four loops and their chains beside the tracks, then the buses' chains and Knob FX each a
// job of a second run: the same, to the sample, with the workers as without, and as the old way.
void loops_and_buses_parallel() {
    auto play = [](unsigned workers, bool whole) {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        for (unsigned t = 0; t < 4; ++t) {
            synth(p->tracks[t], t);
            for (unsigned bus = 0; bus < bus_count; ++bus) p->tracks[t].mix.sends[bus] = uint8_t(10 + 10 * bus + t);
            put(b->at[t][0], t, uint8_t(50 + 3 * t), 100, 4);
        }
        const EffectKind buses[] {EffectKind::Reverb, EffectKind::Echo, EffectKind::Plate, EffectKind::Chorus};
        for (unsigned bus = 0; bus < bus_count; ++bus) p->buses[bus].fx.slots[0] = {uint8_t(buses[bus]), false, {70, 50, 40, 50, 0, 0}};
        p->looper.on = true;
        EngineHost host;
        host.set_render_threads(workers, 0);
        host.set_one_dispatch(whole);
        host.load(*p, *b);
        auto tones = std::make_unique<Tones>();
        tones->offer(host, *p);
        std::vector<int32_t> out(256), all;
        auto render = [&](unsigned blocks) {
            for (unsigned n = 0; n < blocks; ++n) {
                host.render(out.data(), 128, 1);
                host.looper().service();
                all.insert(all.end(), out.begin(), out.end());
            }
        };
        render(8); // the Looper's memory
        const EffectKind chains[] {EffectKind::Drive, EffectKind::Phaser, EffectKind::Echo, EffectKind::Flanger};
        for (unsigned n = 0; n < loop_tracks; ++n) {
            auto* buffer = new LoopBuffer(looper::frames_at(44100));
            for (unsigned f = 0; f < 4000; ++f) buffer->left[f] = buffer->right[f] = float(std::sin(f * (0.01 + 0.007 * n)) * 0.3);
            LoopLoad item; item.buffer = buffer; item.loop = uint8_t(n); item.frames = 4000; item.ticks = 96; item.tempo = 1200;
            CHECK(host.looper().give_load(item));
            host.send({Command::Type::FxKind, uint8_t(loop_chain(n)), 0, 0, int16_t(chains[n])});
            host.send({Command::Type::Send, uint8_t(track_count + n), 0, 0, 40}); // L1-L4 into a bus too
        }
        host.send({Command::Type::KnobFx, 0, 0, 0, int16_t(EffectKind::Filter)}); // Knob FX on track 2, heard
        Command knob {Command::Type::KnobFx, 0, 0, 3}; knob.position = 2; host.send(knob);
        host.send({Command::Type::KnobFx, 0, 0, 2, 1});
        host.send({Command::Type::Play});
        render(400);
        for (unsigned n = 0; n < loop_tracks; ++n) CHECK(host.looper().view(n).state == Looper::State::Playing && host.loop_meter(n) > 0.01f);
        return all;
    };
    auto alone = play(0, true);
    int64_t loudest = 0;
    for (int32_t v : alone) loudest = std::max<int64_t>(loudest, std::llabs(int64_t(v)));
    CHECK(loudest > 200000000); // it plays
    CHECK(play(3, true) == alone);
    CHECK(play(3, false) == alone);
    CHECK(play(0, false) == alone);
}

void stream_pager() {
    std::vector<std::pair<uintptr_t, bool>> log;
    pin_log = &log; pin_fails = false;
    StreamPager pager(logged_pin);
    std::vector<float> side(stream_chunk * 10);
    auto base = reinterpret_cast<uintptr_t>(side.data());
    auto chunk_of = [&](uintptr_t a) { return (a - base) / (stream_chunk * sizeof(float)); };
    ZonePart part;
    part.left = part.right = side.data();
    part.pins.assign(10, 0);
    part.total = stream_chunk * 10;
    part.frames.store(part.total);
    const auto c = stream_chunk;
    int s = pager.open(part, 0, false, 1.0f);
    CHECK(s >= 0);
    CHECK(pager.limit(s) == c); // the head is held, so safe before the pager has answered
    while (pager.pump()) {}
    CHECK(pager.limit(s) == 3 * c && part.pins[0] == 1 && part.pins[1] == 1 && part.pins[2] == 1 && part.pins[3] == 0);
    pager.place(s, 2 * c + 5);
    while (pager.pump()) {}
    CHECK(pager.limit(s) == 5 * c);
    CHECK(part.pins[0] == 0 && part.pins[1] == 1); // a chunk behind stays
    CHECK(!log.empty() && !log.back().second); // the last it did was let go
    { // a second voice over the same chunks keeps them when the first lets go
        int t = pager.open(part, 2 * c, false, 1.0f);
        CHECK(t >= 0 && t != s);
        while (pager.pump()) {}
        CHECK(part.pins[2] == 2);
        log.clear();
        pager.close(s);
        while (pager.pump()) {}
        CHECK(part.pins[2] == 1 && part.pins[1] == 0);
        for (auto& e : log) CHECK(chunk_of(e.first) != 2);
        pager.close(t);
        while (pager.pump()) {}
        for (auto n : part.pins) CHECK(n == 0);
    }
    { // to the end, and no further
        int t = pager.open(part, 8 * c, false, 1.0f);
        while (pager.pump()) {}
        CHECK(pager.limit(t) == 10 * c);
        pager.close(t);
        while (pager.pump()) {}
    }
    { // a loop coming round starts a new run: its old answer no longer counts
        int t = pager.open(part, 0, false, 1.0f);
        while (pager.pump()) {}
        pager.place(t, 2 * c);
        while (pager.pump()) {}
        CHECK(pager.limit(t) == 5 * c);
        pager.restart(t, c / 2);
        CHECK(pager.limit(t) == c / 2 + c); // before the pager answers: the loop's held start only
        while (pager.pump()) {}
        CHECK(pager.limit(t) == 3 * c);
        pager.close(t);
        while (pager.pump()) {}
    }
    { // reversed, its end held (a long kit sample's reversed pad): a chunk down from where it starts, at once
        int t = pager.open(part, 9 * c + 7, true, 1.0f, c);
        CHECK(pager.limit(t) == 8 * c + 8);
        pager.close(t);
        while (pager.pump()) {}
    }
    { // reversed, nothing held there (a zone's end): nothing is safe until pinned, then downwards
        int t = pager.open(part, 9 * c + 7, true, 1.0f, 0);
        CHECK(pager.limit(t) == UINT32_MAX);
        while (pager.pump()) {}
        CHECK(pager.limit(t) == 7 * c);
        pager.place(t, 5 * c + 3);
        while (pager.pump()) {}
        CHECK(pager.limit(t) == 3 * c && part.pins[9] == 0);
        pager.close(t);
        while (pager.pump()) {}
    }
    { // what stays held for the part outlasts every voice
        CHECK(pager.hold(part, 100, c));
        CHECK(part.pins[0] == 1 && part.pins[1] == 1 && part.pins[2] == 0);
        int t = pager.open(part, 0, false, 1.0f);
        while (pager.pump()) {}
        pager.place(t, 6 * c);
        while (pager.pump()) {}
        CHECK(part.pins[0] == 1 && part.pins[1] == 1);
        pager.close(t);
        while (pager.pump()) {}
        CHECK(part.pins[0] == 1 && part.pins[1] == 1 && part.pins[6] == 0);
    }
    { // a card gone: the stream says so and pins nothing more
        pin_fails = true;
        int t = pager.open(part, 3 * c, false, 1.0f);
        while (pager.pump()) {}
        CHECK(pager.dead(t) && pager.limit(t) == 4 * c);
        pin_fails = false;
        pager.close(t);
        while (pager.pump()) {}
    }
    { // every stream taken: -1, not a reuse
        std::vector<int> all;
        for (unsigned n = 0; n < StreamPager::stream_count; ++n) all.push_back(pager.open(part, 0, false, 1.0f));
        for (int x : all) CHECK(x >= 0);
        CHECK(pager.open(part, 0, false, 1.0f) == -1);
        for (int x : all) pager.close(x);
        while (pager.pump()) {}
        CHECK(pager.open(part, 0, false, 1.0f) >= 0);
    }
    pin_log = nullptr;
}

// A pad holding an instrument: zones by key and velocity, as the pad's own sound in every other way.
struct ZoneRig {
    std::vector<std::vector<float>> audio;
    std::shared_ptr<Multisample> multi = std::make_shared<Multisample>();
    SampleData data;
    SamplerEngine::Table table {};
    std::unique_ptr<SamplerEngine> engine = std::make_unique<SamplerEngine>();
    PadSound pad = [] { PadSound p; p.mono = false; return p; }(); // an instrument loads Poly (shell/sample_files.cpp)
    void part(std::vector<float> frames) { audio.push_back(std::move(frames)); }
    SampleZone& zone(uint16_t which = 0) { multi->zones.emplace_back(); multi->zones.back().part = which; return multi->zones.back(); }
    void build(PadPlay play = PadPlay::OneShot) {
        multi->parts = std::make_unique<ZonePart[]>(audio.size());
        multi->part_count = unsigned(audio.size());
        for (size_t n = 0; n < audio.size(); ++n) {
            auto& p = multi->parts[n];
            p.left = p.right = audio[n].data();
            p.total = uint32_t(audio[n].size());
            p.pins.assign((p.total - 1) / stream_chunk + 1, 0);
            p.frames.store(p.total);
        }
        multi->turns.assign(multi->zones.size(), 0);
        data.multi = multi;
        table[3] = &data;
        engine->set_samples(&table);
        pad.sample = 3; pad.play = play;
        engine->set_pad(0, pad);
    }
    std::vector<float> run(unsigned frames) {
        std::vector<float> out(frames);
        std::vector<float> right(frames);
        for (unsigned done = 0; done < frames; done += 128) {
            unsigned n = std::min(128u, frames - done);
            std::fill(out.begin() + done, out.begin() + done + n, 0.0f);
            std::fill(right.begin() + done, right.begin() + done + n, 0.0f);
            engine->render_stereo(out.data() + done, right.data() + done, n);
        }
        return out;
    }
    void key(int semitones, int velocity = 127) { engine->note_on(int(sampler_first_note) | (uint8_t(int8_t(semitones)) << 8), velocity); }
    void off(int semitones) { engine->note_off(int(sampler_first_note) | (uint8_t(int8_t(semitones)) << 8)); }
};

std::vector<float> zone_tone(double hz, float level, unsigned frames) {
    std::vector<float> out(frames);
    for (unsigned n = 0; n < frames; ++n) out[n] = level * float(std::sin(2 * 3.14159265358979 * hz * n / dsp::model_rate));
    return out;
}
double zone_rms(const std::vector<float>& x, size_t from, size_t to) {
    double sum = 0;
    for (size_t n = from; n < to; ++n) sum += double(x[n]) * x[n];
    return std::sqrt(sum / double(std::max<size_t>(to - from, 1)));
}
double zone_hz(const std::vector<float>& x, size_t from, size_t to) {
    unsigned rises = 0;
    for (size_t n = from + 1; n < to; ++n) rises += x[n - 1] < 0 && x[n] >= 0;
    return rises * double(dsp::model_rate) / double(to - from);
}

// A mono program's portamento on a KEYGROUP track (an MPC keygroup's Mono, PortamentoTime, PortamentoLegato): with
// Legato a key pressed while one is held carries that note on, its pitch sliding to the new key; a Slide step does the
// same on any instrument, a Tie at once.
// A split (Multisample::mono_low, mono_high): Mono on one side's keys only. A note there cuts the last; the other side
// plays chords, and neither side cuts the other. A zone follows velocity its own way over the pad's.
void keygroup_split() {
    const double rate = dsp::sample_rate();
    ZoneRig r;
    r.part(zone_tone(220, 0.25f, 3 * 44100));
    r.part(zone_tone(660, 0.25f, 3 * 44100));
    auto& under = r.zone(0); under.root = 48; under.low_key = 0; under.high_key = 59;
    auto& over = r.zone(1); over.root = 72; over.low_key = 60; over.high_key = 127; over.velocity = -100;
    r.pad.release = 0;
    r.pad.velocity_sense = 100;
    r.multi->mono = true; r.multi->mono_low = 0; r.multi->mono_high = 59;
    r.build(PadPlay::Gate);
    r.engine->set_keyed(true);
    auto level = [&] { r.run(2000); return peak_of(r.run(unsigned(rate * 0.05))); };
    r.engine->note_on(48, 127);
    const float one = level();
    CHECK(one > 0.05f);
    r.engine->note_on(50, 127); // the mono side: the first is cut
    CHECK(level() < 1.3f * one);
    r.engine->note_on(72, 127); // the poly side: a chord, over the mono side's note
    r.engine->note_on(76, 127);
    CHECK(level() > 2.0f * one);
    r.engine->note_on(52, 127); // the mono side again: the chord plays on
    CHECK(level() > 2.0f * one);
    r.engine->note_off(72); r.engine->note_off(76);
    r.run(4000);
    const float left = level();
    CHECK(left > 0.6f * one && left < 1.3f * one); // the mono side's one note
    r.engine->note_off(52); // keys 48 and 50 are held still: the note goes back to the newest of them
    CHECK(level() > 0.6f * one);
    r.engine->note_off(48); r.engine->note_off(50);
    r.run(4000);
    CHECK(level() < 0.01f);
    // Velocity: the pad's is 100 (a soft press far quieter); the upper zone's is 100 under it (as loud at any velocity).
    r.engine->note_on(48, 32);
    const float soft = level();
    r.engine->note_off(48);
    r.run(4000);
    r.engine->note_on(72, 32);
    const float even = level();
    r.engine->note_off(72);
    CHECK(soft < 0.4f * one && even > 0.8f * one);
}

void keygroup_glide() {
    const double rate = dsp::sample_rate();
    auto rig = [](ZoneRig& r, bool mono, float glide) {
        r.part(zone_tone(220, 0.5f, 3 * 44100));
        auto& z = r.zone(0); z.root = 60;
        r.pad.release = 0;
        r.multi->mono = mono; r.multi->legato = mono; r.multi->tied = mono; r.multi->glide = glide;
        r.build(PadPlay::Gate);
        r.engine->set_keyed(true);
    };
    // Tied or not, on a sound that fades: a key pressed while one is held carries the faded note on, or starts the
    // sound again (as an MPC plays a mono program), its pitch sliding from the held key.
    for (bool tied : {true, false}) {
        ZoneRig r;
        std::vector<float> fading = zone_tone(220, 0.5f, 44100);
        for (size_t f = 0; f < fading.size(); ++f) fading[f] *= float(std::exp(-double(f) / (0.1 * 44100)));
        r.part(std::move(fading));
        auto& z = r.zone(0); z.root = 60;
        r.pad.release = 0;
        r.multi->mono = true; r.multi->legato = true; r.multi->tied = tied; r.multi->glide = 0.1f;
        r.build(PadPlay::Gate);
        r.engine->set_keyed(true);
        r.engine->note_on(60, 127);
        r.run(unsigned(rate * 0.5));
        r.engine->note_on(72, 127);
        const auto next = r.run(unsigned(rate * 0.05));
        CHECK(tied ? peak_of(next) < 0.02f : peak_of(next) > 0.2f);
        if (!tied) { const double hz = zone_hz(next, 0, next.size()); CHECK(hz > 240 && hz < 420); }
        // The newer key let go while the first is still held: the note goes back to the first, as a mono synth's
        // does: carried there, still faded (tied), or started again, sliding down to it.
        r.run(unsigned(rate * 0.5));
        r.engine->note_off(72);
        const auto back = r.run(unsigned(rate * 0.3));
        CHECK(tied ? peak_of(back) < 0.02f : peak_of(back) > 0.2f);
        if (!tied) CHECK(std::fabs(zone_hz(back, size_t(rate * 0.15), size_t(rate * 0.25)) - 220) < 12);
        // a key pressed after the last was let go starts at its own pitch (Legato: no slide from the last)
        r.engine->note_off(60);
        r.run(400);
        r.engine->note_on(60, 127);
        const auto apart = r.run(unsigned(rate * 0.04));
        CHECK(peak_of(apart) > 0.2f && zone_hz(apart, 0, apart.size()) < 250);
        // the older of two held keys let go: nothing changes; then the one that sounds: it ends, none left to go back to
        r.engine->note_on(72, 127);
        r.run(2000);
        r.engine->note_off(60);
        CHECK(tied || peak_of(r.run(2000)) > 0.1f);
        r.engine->note_off(72);
        r.run(8000);
        CHECK(peak_of(r.run(2000)) < 0.01f);
    }
    { // tied, on a sound that holds: back at the first key's pitch, the same note still sounding, until that is let go too
        ZoneRig r;
        rig(r, true, 0.05f);
        r.engine->note_on(60, 127);
        r.run(4000);
        r.engine->note_on(72, 127);
        r.run(unsigned(rate * 0.2));
        r.engine->note_off(72);
        r.run(unsigned(rate * 0.2));
        const auto home = r.run(6000);
        CHECK(std::fabs(zone_hz(home, 0, 6000) - 220) < 8 && peak_of(home) > 0.1f);
        r.engine->note_off(60);
        r.run(8000);
        CHECK(peak_of(r.run(2000)) < 0.01f);
    }
    {
        ZoneRig r;
        rig(r, true, 0.1f);
        r.engine->note_on(60, 127);
        r.run(4000);
        r.engine->note_on(72, 127); // legato: no new note, the pitch slides up an octave over 0.1 s
        const auto sliding = r.run(unsigned(rate * 0.05));
        const double mid = zone_hz(sliding, 0, sliding.size());
        CHECK(mid > 240 && mid < 420);
        r.run(unsigned(rate * 0.1));
        CHECK(std::fabs(zone_hz(r.run(6000), 0, 6000) - 440) < 8);
        r.engine->note_off(60); // the key slid from: the note is 72's now
        CHECK(std::fabs(zone_hz(r.run(6000), 0, 6000) - 440) < 8 && peak_of(r.run(2000), 0) > 0.1f);
        r.engine->note_off(72);
        r.run(8000);
        CHECK(peak_of(r.run(2000), 0) < 0.01f);
    }
    { // legato: a key pressed after the last was let go starts at its own pitch, no slide from the last
        ZoneRig r;
        rig(r, true, 0.1f);
        r.engine->note_on(60, 127);
        r.run(4000);
        r.engine->note_off(60);
        r.run(200);
        r.engine->note_on(72, 127);
        const auto start = r.run(unsigned(rate * 0.04));
        CHECK(zone_hz(start, 0, start.size()) > 410); // 440 (a 40 ms count of crossings reads within 25 Hz)
    }
    { // not legato: every press slides from the last key, let go or not
        ZoneRig r;
        rig(r, true, 0.1f);
        r.multi->legato = false;
        r.engine->note_on(60, 127);
        r.run(4000);
        r.engine->note_off(60);
        r.run(200);
        r.engine->note_on(72, 127);
        const auto start = r.run(unsigned(rate * 0.04));
        const double hz = zone_hz(start, 0, start.size());
        CHECK(hz > 225 && hz < 400);
    }
    { // no glide: the legato note jumps at once, still without a new note
        ZoneRig r;
        rig(r, true, 0.0f);
        r.engine->note_on(60, 127);
        r.run(4000);
        r.engine->note_on(67, 127);
        r.run(600);
        CHECK(std::fabs(zone_hz(r.run(6000), 0, 6000) - 220 * std::pow(2.0, 7 / 12.0)) < 8);
    }
    { // poly (no Mono): a second key is a second note, the first at its own pitch
        ZoneRig r;
        rig(r, false, 0.1f);
        r.engine->note_on(60, 127);
        r.run(4000);
        r.engine->note_on(72, 127);
        r.engine->note_off(72);
        r.run(8000);
        CHECK(std::fabs(zone_hz(r.run(6000), 0, 6000) - 220) < 8);
    }
    { // a Slide step on a poly instrument with no portamento of its own: the held note slides over slide_seconds
        ZoneRig r;
        rig(r, false, 0.0f);
        r.engine->note_on(60, 127);
        r.run(4000);
        r.engine->slide(60, 67, 100, true);
        const auto sliding = r.run(unsigned(rate * SamplerEngine::slide_seconds / 2));
        const double mid = zone_hz(sliding, 0, sliding.size());
        CHECK(mid > 225 && mid < 320);
        r.run(unsigned(rate * 0.06));
        CHECK(std::fabs(zone_hz(r.run(6000), 0, 6000) - 220 * std::pow(2.0, 7 / 12.0)) < 8);
        r.engine->note_off(67);
        r.run(8000);
        CHECK(peak_of(r.run(2000), 0) < 0.01f);
    }
    { // a looped note let go: the loop fades in 30 ms, its tail (a release zone, the same file from frame 30000) plays
        ZoneRig r;
        std::vector<float> tone(88200);
        for (size_t f = 0; f < tone.size(); ++f) tone[f] = f < 30000 ? 0.5f * float(std::sin(2 * M_PI * 220 * f / 44100.0))
                                                                     : 0.5f * float(std::sin(2 * M_PI * 220 * f / 44100.0)) * std::exp(-float(f - 30000) / 20000.0f);
        r.part(tone);
        auto& held = r.zone(0); held.root = 57; held.loop = ZoneLoop::On; held.loop_start = 10000; held.loop_end = 20000; held.end = 30000;
        held.release = -10; // the pad's release less ten steps: quick
        auto& gone = r.zone(0); gone.root = 57; gone.on_release = true; gone.one_shot = true; gone.start = 30000; gone.end = 88200;
        r.multi->has_release = true;
        r.pad.release = 10;
        r.build(PadPlay::Gate);
        r.engine->set_keyed(true);
        r.engine->note_on(57, 127);
        r.run(40000); // well past the 30000 frames held: the loop keeps it going
        CHECK(peak_of(r.run(2000), 0) > 0.2f);
        r.engine->note_off(57);
        r.run(4410);  // 100 ms on: the tail, falling from where the key was let go
        CHECK(peak_of(r.run(2000), 0) > 0.2f);
        r.run(44100);
        CHECK(peak_of(r.run(2000), 0) < 0.15f);
    }
    { // a Tie: there at once
        ZoneRig r;
        rig(r, false, 0.0f);
        r.engine->note_on(60, 127);
        r.run(4000);
        r.engine->slide(60, 64, 100, false);
        r.run(600);
        CHECK(std::fabs(zone_hz(r.run(6000), 0, 6000) - 220 * std::pow(2.0, 4 / 12.0)) < 8);
    }
}

void sampler_zones() {
    // A zone voice's short loop (SamplerEngine::zone_run_plain()) sounds exactly as play_zone()'s full one: looped and
    // one-shot zones, a loop until released, a filtered zone, chords, bends gliding and keys let go.
    {
        auto render = [](bool plain) {
            SamplerEngine::plain_runs = plain;
            ZoneRig rig;
            rig.part(zone_tone(220, 0.5f, 30000));
            rig.part(zone_tone(331, 0.4f, 20000));
            auto& low = rig.zone(0); low.high_key = 59; low.root = 57; low.loop = ZoneLoop::On; low.loop_start = 4000; low.loop_end = 9000;
            auto& high = rig.zone(1); high.low_key = 60; high.root = 64; high.loop = ZoneLoop::UntilReleased; high.loop_start = 2000; high.loop_end = 7000;
            high.filter = PadFilter::Lpf2; high.cutoff = 0.4f; high.resonance = 0.3f;
            auto& top = rig.zone(0); top.low_key = 72; top.root = 72; top.one_shot = true; top.end = 12000;
            rig.build(PadPlay::Gate);
            std::vector<float> out;
            for (unsigned b = 0; b < 300; ++b) {
                if (b % 20 == 0) { rig.key(-5 + int(b / 20 % 4) * 7); rig.key(14); }
                if (b % 20 == 12) { rig.off(-5 + int(b / 20 % 4) * 7); rig.off(14); }
                if (b == 100) rig.engine->set_bend(0.5f);
                if (b == 180) rig.engine->set_bend(0.0f);
                auto part = rig.run(b % 3 ? 128 : 45);
                out.insert(out.end(), part.begin(), part.end());
            }
            SamplerEngine::plain_runs = true;
            return out;
        };
        SamplerEngine::plain_taken = 0;
        const auto full = render(false);
        CHECK(SamplerEngine::plain_taken == 0);
        SamplerEngine::held_taken = 0;
        const auto plain = render(true);
        CHECK(SamplerEngine::plain_taken > 200);
        CHECK(SamplerEngine::held_taken > 50); // a zone holding its sustain: the run without the tests
        {
            SamplerEngine::held_runs = false;
            const auto unheld = render(true);
            SamplerEngine::held_runs = true;
            CHECK(unheld == plain);
        }
        size_t differ = 0;
        for (size_t n = 0; n < std::min(full.size(), plain.size()); ++n) differ += full[n] != plain[n];
        if (differ) std::fprintf(stderr, "zone_run_plain: %zu samples differ\n", differ);
        CHECK(full.size() == plain.size() && differ == 0 && peak_of(full, 0) > 0.05f);
    }
    { // a key plays its zone in tune, whichever zone it is
        ZoneRig r;
        r.part(zone_tone(220, 0.5f, 44100)); r.part(zone_tone(880, 0.5f, 44100));
        auto& low = r.zone(0); low.low_key = 0; low.high_key = 65; low.root = 60;
        auto& high = r.zone(1); high.low_key = 66; high.high_key = 127; high.root = 72;
        r.build();
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 220) < 8);
        r.engine->silence();
        r.key(5); // key 65, up five semitones from its root
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 220 * std::pow(2.0, 5 / 12.0)) < 8);
        r.engine->silence();
        r.key(12); // key 72: the other zone, at its own pitch
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880) < 12);
    }
    { // the pad's Semi moves the key, its Fine tunes every zone
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100));
        r.zone(0).root = 60;
        r.pad.pitch = 12;
        r.build();
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880) < 12);
    }
    { // a keytrack of nothing stays at its pitch; a zone's tune is its own
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100));
        auto& z = r.zone(0); z.keytrack = 0; z.tune = 1200;
        r.build();
        r.key(7);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880) < 12);
    }
    { // a velocity plays its layer
        ZoneRig r;
        r.part(zone_tone(440, 0.1f, 44100)); r.part(zone_tone(440, 0.8f, 44100));
        auto& soft = r.zone(0); soft.high_velocity = 64;
        auto& loud = r.zone(1); loud.low_velocity = 65;
        r.pad.velocity_sense = 0;
        r.build();
        r.key(0, 40); auto a = r.run(4000); r.engine->silence();
        r.key(0, 100); auto b = r.run(4000);
        CHECK(zone_rms(b, 500, 4000) > 6 * zone_rms(a, 500, 4000));
    }
    { // round robin takes turns; every zone a press reaches plays
        ZoneRig r;
        r.part(zone_tone(440, 0.1f, 44100)); r.part(zone_tone(440, 0.8f, 44100));
        auto& one = r.zone(0); one.sequence_length = 2; one.sequence = 1;
        auto& two = r.zone(1); two.sequence_length = 2; two.sequence = 2;
        r.pad.velocity_sense = 0;
        r.build();
        std::vector<double> levels;
        for (unsigned n = 0; n < 4; ++n) { r.key(0); levels.push_back(zone_rms(r.run(3000), 500, 3000)); r.engine->silence(); }
        CHECK(levels[1] > 5 * levels[0] && levels[3] > 5 * levels[2] && std::fabs(levels[0] - levels[2]) < 1e-3);
    }
    { // layers add
        ZoneRig r;
        r.part(zone_tone(440, 0.3f, 44100));
        r.zone(0); r.zone(0);
        r.pad.velocity_sense = 0;
        r.build();
        r.key(0); auto both = r.run(3000); r.engine->silence();
        ZoneRig one;
        one.part(zone_tone(440, 0.3f, 44100));
        one.zone(0);
        one.pad.velocity_sense = 0;
        one.build();
        one.key(0); auto alone = one.run(3000);
        CHECK(std::fabs(zone_rms(both, 500, 3000) / zone_rms(alone, 500, 3000) - 2.0) < 0.05);
    }
    { // a press of a group cuts what is off by it
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100)); r.part(zone_tone(880, 0.5f, 44100));
        auto& open = r.zone(0); open.low_key = 60; open.high_key = 60; open.root = 60; open.off_by = 1;
        auto& shut = r.zone(1); shut.low_key = 61; shut.high_key = 61; shut.root = 61; shut.group = 1;
        r.build();
        r.key(0); r.run(2000);
        r.key(1);
        auto after = r.run(6000);
        CHECK(std::fabs(zone_hz(after, 2000, 6000) - 880) < 15);
    }
    { // a loop holds while the note does, and until let go goes on to the end after
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 4410));
        auto& z = r.zone(0); z.loop = ZoneLoop::On; z.loop_start = 1000; z.loop_end = 3000;
        r.pad.release = 0;
        r.build(PadPlay::Gate);
        r.key(0);
        CHECK(zone_rms(r.run(44100), 40000, 44100) > 0.2); // long past the sample's length
        r.off(0);
        CHECK(zone_rms(r.run(8000), 6000, 8000) < 1e-3);
        ZoneRig held;
        held.part(zone_tone(441, 0.5f, 8820));
        auto& h = held.zone(0); h.loop = ZoneLoop::UntilReleased; h.loop_start = 1000; h.loop_end = 3000;
        held.pad.decay = 100; held.pad.sustain = 100; held.pad.release = 100;
        held.build(PadPlay::Gate);
        held.key(0);
        held.run(44100);
        held.off(0);
        auto tail = held.run(44100);
        CHECK(zone_rms(tail, 0, 2000) > 0.1); // sounding through the release...
        CHECK(!held.engine->sounding() || zone_rms(tail, 12000, 13000) == 0.0); // ...and gone once past the sample's end
    }
    { // a loop may end at the very end of its file
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 4000));
        auto& z = r.zone(0); z.loop = ZoneLoop::On; z.loop_start = 2000; z.loop_end = 4000;
        r.build(PadPlay::Gate);
        r.key(0);
        CHECK(zone_rms(r.run(30000), 25000, 30000) > 0.2);
    }
    { // the seam: a loop of whole cycles at any pitch has no step at the join
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 8000));
        auto& z = r.zone(0); z.loop = ZoneLoop::On; z.loop_start = 1000; z.loop_end = 1000 + 1000; // ten cycles
        r.build(PadPlay::Gate);
        r.key(7); // a fifth up: the position lands between frames at every join
        auto out = r.run(40000);
        double jump = 0;
        for (size_t n = 2000; n < out.size(); ++n) jump = std::max(jump, double(std::fabs(out[n] - out[n - 1])));
        double expected = 0.5 * 0.7071 * 2 * 3.14159265 * 441 * std::pow(2.0, 7 / 12.0) / 44100;
        CHECK(jump < expected * 1.15);
    }
    { // a zone's own envelope is off the pad's
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 44100 * 2));
        auto& z = r.zone(0); z.release = 60;
        r.pad.release = 10;
        r.build(PadPlay::Gate);
        r.key(0); r.run(2000); r.off(0);
        auto slow = r.run(8000);
        ZoneRig fast;
        fast.part(zone_tone(441, 0.5f, 44100 * 2));
        fast.zone(0);
        fast.pad.release = 10;
        fast.build(PadPlay::Gate);
        fast.key(0); fast.run(2000); fast.off(0);
        auto quick = fast.run(8000);
        CHECK(zone_rms(slow, 4000, 8000) > 5 * zone_rms(quick, 4000, 8000));
    }
    { // a zone's filter is its own, and the pad's cutoff still moves it
        auto heard = [](int cutoff, float hz) {
            ZoneRig r;
            r.part(zone_tone(4000, 0.5f, 44100));
            auto& z = r.zone(0);
            if (hz > 0) { z.filter = PadFilter::Lpf2; z.cutoff = hz; }
            r.pad.cutoff = uint8_t(cutoff);
            r.build();
            r.key(0);
            return zone_rms(r.run(8000), 2000, 8000);
        };
        CHECK(heard(100, 400) < 0.2 * heard(100, 0));
        CHECK(heard(100, 16000) > 0.8 * heard(100, 0));
        CHECK(heard(50, 16000) < 0.5 * heard(100, 16000));
    }
    { // one note of a chord ends alone
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 44100 * 3));
        r.zone(0);
        r.pad.release = 0;
        r.pad.velocity_sense = 0;
        r.build(PadPlay::Gate);
        r.key(4); r.key(7);
        auto both = r.run(4000);
        r.off(7);
        r.run(4000);
        auto one = r.run(4000);
        CHECK(zone_rms(one, 0, 4000) > 0.2 && zone_rms(one, 0, 4000) < 0.85 * zone_rms(both, 1000, 4000));
        CHECK(std::fabs(zone_hz(one, 0, 4000) - 441 * std::pow(2.0, 4 / 12.0)) < 8);
    }
    { // silent until its file lands
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 44100));
        r.zone(0);
        r.build();
        r.multi->parts[0].frames.store(0);
        r.key(0);
        CHECK(zone_rms(r.run(4000), 0, 4000) == 0.0 && !r.engine->sounding());
    }
    { // a muted pad, and the pad's level, as any pad
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, 44100));
        r.zone(0);
        r.pad.velocity_sense = 0;
        r.build();
        r.engine->set_pad_mute(0, true);
        r.key(0);
        CHECK(zone_rms(r.run(2000), 0, 2000) == 0.0);
        r.engine->set_pad_mute(0, false);
        r.key(0);
        double full = zone_rms(r.run(2000), 500, 2000);
        r.engine->silence();
        PadSound quiet = r.pad; quiet.level = 50;
        r.engine->set_pad(0, quiet);
        r.key(0);
        CHECK(zone_rms(r.run(2000), 500, 2000) < 0.5 * full);
    }
    { // streamed, it is the same frames as held; starved, a gap with nothing read past what is safe
        pin_log = nullptr; pin_fails = false;
        const unsigned frames = stream_chunk * 6;
        auto held = [&](ZoneRig& r) { r.part(zone_tone(441, 0.5f, frames)); auto& z = r.zone(0); z.start = 100; r.build(); };
        ZoneRig a, b;
        held(a); held(b);
        StreamPager pager(logged_pin);
        b.engine->set_pager(&pager);
        a.key(0); b.key(0);
        while (pager.pump()) {}
        for (unsigned n = 0; n < frames / 128; ++n) {
            auto x = a.run(128), y = b.run(128);
            for (unsigned f = 0; f < 128; ++f) CHECK(x[f] == y[f]);
            while (pager.pump()) {}
        }
        CHECK(!b.engine->sounding() && pager.underruns() == 0); // and its stream was let go at its end
        for (unsigned n = 0; n < StreamPager::stream_count; ++n) CHECK(pager.open(b.multi->parts[0], 0, false, 1.0f) >= 0);

        ZoneRig r;
        r.part(zone_tone(441, 0.5f, frames));
        for (size_t n = stream_chunk; n < frames; ++n) r.audio[0][n] = std::numeric_limits<float>::quiet_NaN(); // what must not be read
        r.zone(0);
        r.build();
        StreamPager slow(logged_pin);
        r.engine->set_pager(&slow);
        r.key(0);
        auto out = r.run(stream_chunk * 3); // never pumped
        double tail = 0;
        for (auto x : out) { CHECK(std::isfinite(x)); }
        tail = zone_rms(out, stream_chunk * 2, stream_chunk * 3);
        CHECK(tail == 0.0 && zone_rms(out, 1000, stream_chunk - 1000) > 0.2);
        CHECK(slow.underruns() == 1);
        for (size_t n = stream_chunk; n < frames; ++n) r.audio[0][n] = 0.25f; // the card catches up
        while (slow.pump()) {}
        CHECK(zone_rms(r.run(4000), 500, 4000) > 0.05);
    }
    { // a card gone: the voice stops and the note is not stuck
        pin_fails = true;
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, stream_chunk * 4));
        r.zone(0);
        r.build();
        StreamPager gone(logged_pin);
        r.engine->set_pager(&gone);
        r.key(0);
        while (gone.pump()) {}
        r.run(stream_chunk * 2);
        CHECK(!r.engine->sounding());
        pin_fails = false;
    }
    { // more voices than streams: what is held plays, and nothing else is read
        ZoneRig r;
        r.part(zone_tone(441, 0.5f, stream_chunk * 3));
        for (size_t n = stream_chunk; n < r.audio[0].size(); ++n) r.audio[0][n] = std::numeric_limits<float>::quiet_NaN();
        r.zone(0);
        r.build();
        StreamPager full(logged_pin);
        for (unsigned n = 0; n < StreamPager::stream_count; ++n) full.open(r.multi->parts[0], 0, false, 1.0f);
        r.engine->set_pager(&full);
        r.key(0);
        auto out = r.run(stream_chunk * 2);
        for (auto x : out) CHECK(std::isfinite(x));
        CHECK(zone_rms(out, 1000, stream_chunk - 1000) > 0.2 && !r.engine->sounding());
    }
}

// A note's pitch: a chord on one pad, kept through steps and files, older files reading as before.
void note_pitches() {
    { // a FLAC's loop, kept from the WAV it was made of
        namespace fs = std::filesystem;
        auto path = (fs::temp_directory_path() / ("melodee-loop-" + std::to_string(getpid()) + ".flac")).string();
        auto write = [&](bool looped) {
            std::string b = "fLaC";
            auto block = [&](uint8_t type, const std::string& body, bool last) {
                b += char(type | (last ? 0x80 : 0)); b += char(body.size() >> 16); b += char(body.size() >> 8); b += char(body.size()); b += body;
            };
            auto le = [](uint32_t v) { return std::string {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; };
            block(0, std::string(34, 'x'), false);
            block(2, "riff" + std::string("RIFF") + le(1000) + "WAVE", false); // the file's other foreign chunks
            std::string data(60, '\0');
            if (looped) { data.replace(28, 4, le(1)); data.replace(44, 4, le(319390)); data.replace(48, 4, le(322438)); }
            block(2, "riffsmpl" + le(60) + data, false);
            block(1, std::string(100, '\0'), true);
            std::ofstream(path, std::ios::binary) << b;
        };
        WavInfo info;
        write(true);
        CHECK(read_flac_loop(path, info) && info.loop_start == 319390 && info.loop_end == 322439);
        write(false);
        WavInfo none;
        CHECK(!read_flac_loop(path, none) && none.loop_end == -1);
        { std::ofstream(path, std::ios::binary) << "RIFFnotflac"; }
        CHECK(!read_flac_loop(path, none));
        fs::remove(path);
    }
    { // an MP3's encoder delay and padding, from the LAME tag in its first frame (headers as lame and ffmpeg write them)
        namespace fs = std::filesystem;
        auto path = (fs::temp_directory_path() / ("melodee-gapless-" + std::to_string(getpid()) + ".mp3")).string();
        auto write = [&](const std::string& id3, std::string header, unsigned side, const char* tag, const char* encoder) {
            std::string b = id3 + header + std::string(side, '\0') + tag + std::string {0, 0, 0, 0x0f} + std::string(112, '\0');
            std::string version(encoder); version.resize(9, ' '); // its 9-byte version, then 12 bytes before the delay
            b += version + std::string(12, '\0') + std::string {char(576 >> 4), char((576 & 15) << 4 | 1170 >> 8), char(1170 & 255)};
            b += std::string(400, '\0');
            std::ofstream(path, std::ios::binary) << b;
        };
        auto reads = [&](unsigned frame_samples, bool gapless) {
            Mp3Head h;
            return read_mp3_head(path, h) && h.frame_samples == frame_samples && h.gapless == gapless
                && (!gapless || (h.delay == 576 && h.padding == 1170));
        };
        write("", "\xff\xfb\x90\xc4", 17, "Info", "LAME4.0 ");                  // MPEG 1, mono
        CHECK(reads(1152, true));
        write("", "\xff\xfb\x94\x64", 32, "Xing", "LAME3.100");                 // MPEG 1, stereo
        CHECK(reads(1152, true));
        write("", "\xff\xf3\x80\x64", 17, "Info", "LAME4.0 ");                  // MPEG 2, stereo
        CHECK(reads(576, true));
        write("ID3\x04" + std::string(5, '\0') + "\x22" + std::string(34, ' '), "\xff\xfb\x90\xc0", 17, "Info", "Lavc63.1."); // ffmpeg's ID3 first
        CHECK(reads(1152, true));
        write("", "\xff\xfb\x90\xc4", 17, "Info", "Other!!! ");                 // no LAME tag: nothing to trim
        CHECK(reads(1152, false));
        write("", "\xff\xfb\x90\xc4", 32, "Info", "LAME4.0 ");                  // the tag not where a mono frame keeps it
        CHECK(reads(1152, false));
        { std::ofstream(path, std::ios::binary) << "RIFF" << std::string(600, '\0'); }
        Mp3Head none;
        CHECK(!read_mp3_head(path, none));
        fs::remove(path);
    }
    CHECK(pad_note(40, 0) == 40 && (pad_note(40, 5) & 0xff) == 40 && int8_t(pad_note(40, -7) >> 8) == -7);
    Pattern p;
    Event a; a.tick = 0; a.key = 40; a.pitch = 4;
    Event b = a; b.pitch = 7;
    CHECK(p.add(a) && p.add(b) && p.events.size() == 2 && p.events[0].pitch == 4 && p.events[1].pitch == 7);
    Event again = a; again.velocity = 33;
    CHECK(p.add(again) && p.events.size() == 2 && p.events[0].velocity == 33); // the same note replaced, not a third
    Step st = p.step(0);
    p.set_step(0, st);
    CHECK(p.events.size() == 2 && p.events[0].pitch == 4 && p.events[1].pitch == 7); // edited as steps, they keep their pitch
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    bank->at[2][1].length = 96;
    bank->at[2][1].events = p.events;
    auto bytes = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto read = std::make_unique<PatternBank>();
    CHECK(load_project(bytes, *back, *read).empty());
    const auto& got = read->at[2][1].events;
    CHECK(got.size() == 2 && got[0].pitch == 4 && got[1].pitch == 7);
    { // a file with no PTCH: every pitch stays 0
        Event plain; plain.key = 41;
        bank->at[2][1].events = {plain};
        auto old = std::make_unique<PatternBank>();
        CHECK(load_project(save_project(*project, *bank), *back, *old).empty());
        CHECK(old->at[2][1].events.size() == 1 && old->at[2][1].events[0].pitch == 0);
        auto tag = save_project(*project, *bank);
        CHECK(std::search(tag.begin(), tag.end(), "PTCH", "PTCH" + 4) == tag.end());
    }
    SampleRef ref;
    std::strcpy(ref.path, "/media/card/Piano/Grand.SFZ");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Piano/Grand.wav");
    CHECK(!ref.instrument());
    std::strcpy(ref.path, "/media/card/Keys/Rhodes.XPM");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Keys/Rhodes.xpj");
    CHECK(!ref.instrument());
    std::strcpy(ref.path, "/media/card/Samples/Boys Choir/Boys Choir.exs"); // held while held, as an SFZ is
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Kits/Kit-Lofi Patch.xpm#12");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Projects/Cycle 86bpm.xpj#3.16");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Kits/Kick #1.wav");
    CHECK(!ref.instrument());
    std::strcpy(ref.path, "/media/card/Expansions/Organs/Organ-Farfisa VO Miller.xty");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Expansions/Kits/Kit.XTY#5");
    CHECK(ref.instrument());
    // A '#' in a file's own name or its folder's is no pad or preset: the file is what its extension says.
    std::strcpy(ref.path, "/media/card/Strings/Strings C#.sfz");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Leads/Lead F#.dspreset");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/C# Kits/Kit.xpm#4");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/media/card/Piano.sf2#7");
    CHECK(ref.instrument());
    std::strcpy(ref.path, "/content/Samples/Phrases/KeysPh-Kalim1 G#m90 MP.WAV");
    CHECK(!ref.instrument());
    std::strcpy(ref.path, "/media/card/Kits/Kit.sfz#2"); // an SFZ has no pads
    CHECK(!ref.instrument());
    Track kit; kit.engine = EngineKind::Sampler;
    CHECK(note_key(kit, 20) == -1 && note_key(kit, 20, true) == 20 && note_key(kit, 127, true) == 127 && note_key(kit, 128, true) == -1);
    // 16 Levels: the keyboard plays the 16 Levels pad, a single sample as well as an instrument (key 60 its root)
    kit.levels_pad = 18; kit.pads[18].sample = 4; kit.pads[18].pitch = 3;
    CHECK(levels_key(kit, 60, false) == -1); // 16 Levels off: the kit's pads
    kit.levels = true;
    const int note = int(sampler_first_note) + 18;
    CHECK(levels_key(kit, 60, false) == note && levels_key(kit, 72, false) == pad_note(note, 12) && levels_key(kit, 20, false) == pad_note(note, -40));
    // Issue 43: an instrument keeps the pad's Semi on the keys, as its pads under Scale, Tune and Chords: key 60 the
    // pad as they play it (and no sample's note); only Zones, whose pads play its keys, plays it at real pitch.
    for (auto kind : {LevelsKind::Scale, LevelsKind::Tune, LevelsKind::Velocity, LevelsKind::Filter, LevelsKind::Chords}) {
        kit.levels_kind = uint8_t(kind);
        CHECK(levels_key(kit, 60, true) == note && levels_key(kit, 63, true) == pad_note(note, 3) && levels_key(kit, 60, true, 5) == note);
        CHECK(levels_key_semitones(kit, 48, true) == -12);
    }
    kit.levels_kind = uint8_t(LevelsKind::Zones);
    CHECK(levels_key(kit, 63, true) == note && levels_key(kit, 60, true) == pad_note(note, -3)); // an instrument at real pitch
    CHECK(levels_key_semitones(kit, 63, true) == 0 && levels_key_semitones(kit, 60, false) == 0); // a single sample: Zones is Scale
    CHECK(levels_key(kit, 128, false) == -1);
    kit.arp = true;
    CHECK(levels_key(kit, 60, false) == -1);
    kit.arp = false; kit.repeat = true;
    CHECK(levels_key(kit, 60, false) == -1); // Note Repeat too
    kit.repeat = false; kit.pads[18].sample = no_sample;
    CHECK(levels_key(kit, 60, false) == -1); // an empty pad: the kit's pads
}


// Plug-ins (docs/MELODEE.md): the engine host plays whatever PluginRunner the rack offers an entry, as a PLUGIN
// track's instrument and as a chain's Plugin effect; projects keep the table, the tracks' entries and the states.
struct FakePlugin final : PluginRunner {
    bool instrument = true;
    int notes_on = 0, notes_off = 0, silenced = 0;
    unsigned rendered = 0;
    float level = 0;
    void midi(uint8_t status, uint8_t, uint8_t data2) override {
        if ((status & 0xf0) == 0x90 && data2) { ++notes_on; level = 0.25f; }
        if ((status & 0xf0) == 0x80) { ++notes_off; level = 0; }
    }
    void silence() override { ++silenced; level = 0; }
    void render(float* left, float* right, unsigned frames) override {
        rendered += frames;
        for (unsigned f = 0; f < frames; ++f) {
            if (instrument) { left[f] += level; right[f] += level; }
            else { left[f] *= 0.5f; right[f] *= 0.5f; } // an effect: half as loud
        }
    }
};

// Melodee's folders on the card (model/card_folders.h): samples in Melodee/Samples, .syx files in Melodee/SysEx. A
// sample saved in the old top-level Samples folder opens from Melodee/Samples once it is there (a program's pad by its
// file), else where it was; any other path as saved.
void card_sample_paths() {
    namespace fs = std::filesystem;
    auto top = fs::temp_directory_path() / ("melodee-samples-" + std::to_string(getpid()));
    const std::string t = top.string();
    fs::remove_all(top);
    fs::create_directories(top / "Samples" / "Recorded");
    fs::create_directories(top / "Melodee" / "Samples" / "Kits");
    { std::ofstream(top / "Samples" / "Recorded" / "TAKE 1.wav") << "x"; }
    { std::ofstream(top / "Melodee" / "Samples" / "Kits" / "Kick.wav") << "x"; }
    { std::ofstream(top / "Melodee" / "Samples" / "Kits" / "Kit.xpm") << "x"; }
    CHECK(samples_folder(t) == t + "/Melodee/Samples" && sysex_folder(t) == t + "/Melodee/SysEx");
    CHECK(old_samples_folder(t) == t + "/Samples" && old_sysex_folder(t) == t + "/SysEx");
    CHECK(samples_folder("").empty() && sysex_folder("").empty() && old_samples_folder("").empty());
    CHECK(card_sample_path(t, t + "/Samples/Recorded/TAKE 1.wav") == t + "/Samples/Recorded/TAKE 1.wav"); // not moved
    CHECK(card_sample_path(t, t + "/Samples/Kits/Kick.wav") == t + "/Melodee/Samples/Kits/Kick.wav");     // moved
    CHECK(card_sample_path(t, t + "/Samples/Kits/Kit.xpm#3") == t + "/Melodee/Samples/Kits/Kit.xpm#3");   // a pad of a moved program
    CHECK(card_sample_path(t, t + "/Samples/Gone.wav") == t + "/Samples/Gone.wav");                       // on neither: as saved
    CHECK(card_sample_path(t, t + "/Melodee/Samples/Kits/Kick.wav") == t + "/Melodee/Samples/Kits/Kick.wav");
    CHECK(card_sample_path(t, t + "/SamplesMore/Kick.wav") == t + "/SamplesMore/Kick.wav");               // not the folder
    CHECK(card_sample_path(t, "/content/Samples/Kick.wav") == "/content/Samples/Kick.wav");               // the internal drive
    CHECK(card_sample_path("", t + "/Samples/Kits/Kick.wav") == t + "/Samples/Kits/Kick.wav");            // no card
    fs::create_directories(top / "Melodee" / "Samples" / "Recorded");                                     // both: the new one
    { std::ofstream(top / "Melodee" / "Samples" / "Recorded" / "TAKE 1.wav") << "x"; }
    CHECK(card_sample_path(t, t + "/Samples/Recorded/TAKE 1.wav") == t + "/Melodee/Samples/Recorded/TAKE 1.wav");
    fs::remove_all(top);
}

void plugins() {
    // The file: PLGT makes the track a PLUGIN track (TRAK says None, as older builds should see it), PLUG keeps
    // each entry's path and state, and TRANSPORT (on unless turned off; a file from before it: on).
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tracks[2].engine = EngineKind::Plugin;
        project->tracks[2].plugin = 5;
        std::snprintf(project->plugins[5].path, plugin_path_length, "/media/card/Plugins/Nekobi.vst3");
        std::snprintf(project->plugins[9].path, plugin_path_length, "/media/card/Plugins/sd88me - VST - Acid-303");
        std::snprintf(project->plugins[7].path, plugin_path_length, "/media/card/Plugins/lsp-plugins.vst3"); // one of a bundle's
        project->plugins[7].id = -123456789;
        std::snprintf(project->plugins[7].name, sizeof(project->plugins[7].name), "Compressor x2 Stereo");
        project->buses[0].fx.slots[0] = {plugin_effect_kind, false, {7, 0, 0, 0, 0, 0}};
        project->master.slots[0] = {plugin_effect_kind, false, {9, 0, 0, 0, 0, 0}};
        project->plugins[9].transport = false; // a plug-in with its own sequencer, played only by its notes
        std::snprintf(project->plugins[5].preset, sizeof(project->plugins[5].preset), "Leads/Acid Line");
        std::array<std::vector<uint8_t>, max_plugins> states;
        states[5] = {1, 2, 3, 250};
        auto data = save_project(*project, *bank, &states);
        auto copy = std::make_unique<Project>();
        auto copy_bank = std::make_unique<PatternBank>();
        std::array<std::vector<uint8_t>, max_plugins> read;
        CHECK(load_project(data, *copy, *copy_bank, nullptr, &read).empty());
        CHECK(copy->tracks[2].engine == EngineKind::Plugin && copy->tracks[2].plugin == 5);
        CHECK(std::string(copy->plugins[5].path) == "/media/card/Plugins/Nekobi.vst3");
        CHECK(std::string(copy->plugins[9].path) == "/media/card/Plugins/sd88me - VST - Acid-303");
        CHECK(read[5] == states[5] && read[9].empty() && !copy->plugins[4].used());
        CHECK(copy->plugins[7].id == -123456789 && std::string(copy->plugins[7].name) == "Compressor x2 Stereo" && copy->plugins[5].id == 0);
        CHECK(copy->master.slots[0].kind == plugin_effect_kind && copy->master.slots[0].params[0] == 9);
        CHECK(plugin_used_by(*copy, 5) && plugin_used_by(*copy, 9) && !plugin_used_by(*copy, 4));
        CHECK(!copy->plugins[9].transport && copy->plugins[5].transport && copy->plugins[7].transport);
        CHECK(std::string(copy->plugins[5].preset) == "Leads/Acid Line" && copy->plugins[7].preset[0] == 0);
        copy->tracks[2].engine = EngineKind::Midi; // the entry stays in the table but nothing plays it
        CHECK(!plugin_used_by(*copy, 5));
    }
    // The host: a PLUGIN track with no plug-in yet is silent; once offered, notes reach it and it is heard; a
    // plug-in effect on the track's chain works on what the track plays; a new offer hands the old one back.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    project->tracks[0].engine = EngineKind::Plugin;
    project->tracks[0].plugin = 3;
    EngineHost host;
    host.load(*project, *bank);
    std::vector<int32_t> out(256);
    auto peak = [&](unsigned blocks) {
        float most = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            host.render(out.data(), 128, 1);
            for (auto v : out) most = std::max(most, std::fabs(v / 2147483648.0f));
        }
        return most;
    };
    host.send({Command::Type::NoteOn, 0, 0, 60, 100});
    CHECK(peak(4) == 0); // nothing offered yet
    FakePlugin synth;
    CHECK(host.offer_plugin(3, &synth));
    host.send({Command::Type::NoteOn, 0, 0, 60, 100});
    float loud = peak(4);
    CHECK(synth.notes_on == 1 && synth.rendered >= 4 * 128 && loud > 0.05f);
    // The effect: entry 7 on the track's chain halves it.
    FakePlugin half;
    half.instrument = false;
    host.send({Command::Type::FxKind, 0, 0, 0, int16_t(EffectKind::Plugin)});
    host.send({Command::Type::FxParam, 0, 0, 0, 7});
    CHECK(host.offer_plugin(7, &half));
    float halved = peak(4);
    CHECK(half.rendered >= 4 * 128 && std::fabs(halved - loud * 0.5f) < loud * 0.05f);
    // Bypassed, the effect is skipped.
    host.send({Command::Type::FxBypass, 0, 0, 0, 1});
    unsigned before = half.rendered;
    CHECK(std::fabs(peak(4) - loud) < loud * 0.05f && half.rendered == before);
    // Another plug-in for entry 3: the old one comes back to be freed and the track plays the new one.
    FakePlugin second;
    CHECK(host.offer_plugin(3, &second));
    peak(1);
    PluginRunner* back = nullptr;
    CHECK(host.take_retired_plugin(back) && back == &synth);
    host.send({Command::Type::NoteOn, 0, 0, 62, 100});
    peak(2);
    CHECK(second.notes_on == 1 && synth.notes_on == 1);
    // The track to another entry: it lets go of the plug-in it played.
    host.send({Command::Type::TrackPlugin, 0, 0, 0, 4});
    CHECK(peak(2) == 0 && second.silenced >= 1);
    // Entries let go of (nullptr) hand theirs back too.
    CHECK(host.offer_plugin(7, nullptr) && host.offer_plugin(3, nullptr));
    peak(1);
    unsigned handed = 0;
    while (host.take_retired_plugin(back)) handed += back == &half || back == &second;
    CHECK(handed == 2);
}

// The transport plug-ins hear (engine/effects.h, EffectClock; the rack hands it on as each plug-in's play head):
// stopped until Play, then where the song is in quarter notes from its top at the tempo; held where Stop left it;
// stopped through a count-in and from the top once it is over. A plug-in with a sequencer of its own (LXRXXL
// BAZZLINE) starts and stops with Melodee instead of playing from the moment it loads.
struct ClockPlugin final : PluginRunner {
    std::vector<EffectClock> heard;
    void midi(uint8_t, uint8_t, uint8_t) override {}
    void silence() override {}
    void render(float*, float*, unsigned) override { heard.push_back(*effect_clock_now); }
};

void plugin_transport() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    project->tracks[0].engine = EngineKind::Plugin;
    project->tracks[0].plugin = 3;
    EngineHost host;
    host.load(*project, *bank);
    ClockPlugin plugin;
    CHECK(host.offer_plugin(3, &plugin));
    std::vector<int32_t> out(256);
    auto play = [&](unsigned blocks) { for (unsigned n = 0; n < blocks; ++n) host.render(out.data(), 128, 1); };
    const double block = 128 / 22050.0; // quarter notes a block at 120 BPM
    play(4);
    CHECK(!plugin.heard.empty());
    for (const auto& c : plugin.heard) CHECK(!c.playing);
    CHECK(plugin.heard.back().bpm == 120 && plugin.heard.back().sig_beats == 4 && plugin.heard.back().sig_unit == 4);
    CHECK(plugin.heard.back().block != plugin.heard.front().block);

    host.send({Command::Type::Play});
    plugin.heard.clear();
    play(172);
    CHECK(plugin.heard.front().playing && std::fabs(plugin.heard.front().song) < 1e-6);
    double at = plugin.heard.back().song;
    CHECK(plugin.heard.back().playing && std::fabs(at - 171 * block) < 1e-3);
    for (size_t n = 1; n < plugin.heard.size(); ++n) CHECK(plugin.heard[n].song >= plugin.heard[n - 1].song);

    host.send({Command::Type::Stop});
    plugin.heard.clear();
    play(3);
    for (const auto& c : plugin.heard) CHECK(!c.playing && c.song == at);

    host.send({Command::Type::Play, 1}); // a bar's count-in: 88200 frames at 120 BPM
    plugin.heard.clear();
    play(680);
    for (const auto& c : plugin.heard) CHECK(!c.playing);
    plugin.heard.clear();
    play(20);
    auto first = std::find_if(plugin.heard.begin(), plugin.heard.end(), [](const EffectClock& c) { return c.playing; });
    CHECK(first != plugin.heard.end() && first->song < 2 * block);
    CHECK(plugin.heard.back().playing && plugin.heard.back().song > 0);

    // Another tempo and time signature: what the plug-in reads next block.
    host.send({Command::Type::Stop});
    host.send({Command::Type::Tempo, 0, 0, 0, 900});
    host.send({Command::Type::Signature, 0, 0, ticks_per_quarter / 2, 3 * ticks_per_quarter}); // 6/8: a beat an eighth, a bar three quarters
    plugin.heard.clear();
    play(2);
    CHECK(plugin.heard.back().bpm == 90 && plugin.heard.back().sig_beats == 6 && plugin.heard.back().sig_unit == 8);
    host.send({Command::Type::Signature, 0, 0, ticks_per_quarter, 4 * ticks_per_quarter});
}

// Roadmap P0: a hit lands in Seq Record where it was heard, and plays at its own frame, a block late.
void hits_on_time() {
    // The place's arithmetic: counted back from the playhead and wrapped; a step snapped onto still to come or not.
    CHECK(wrap_tick(-3, 384) == 381 && wrap_tick(390, 384) == 6 && wrap_tick(5, 384) == 5);
    // A hit on a kit lane of its own length lands where the lane was: 72 ticks round, from where the pattern began.
    CHECK(lane_tick(100, 0, 384, 72) == 28 && lane_tick(100, 1, 384, 72) == 52); // the second pass: 484 ticks on
    CHECK(lane_tick(-3, 1, 384, 72) == 21 && lane_tick(-3, 0, 384, 72) == 69);   // counted back into the pass before
    CHECK(lane_tick(100, 3, 384, 384) == 100 && lane_tick(-3, 3, 384, 0) == 381); // no lane of its own: the pattern's tick
    CHECK(still_ahead(100, 120, 102, 384));  // the playhead two ticks on: the step is still to come, skipped once
    CHECK(!still_ahead(100, 120, 121, 384)); // it played already
    CHECK(!still_ahead(100, 96, 102, 384));  // snapped back: behind the hit
    CHECK(still_ahead(380, 0, 382, 384));    // onto the first step, next time round
    CHECK(!still_ahead(3, 0, 5, 384));       // back onto the first step: behind
    CHECK(still_ahead(100, 101, 99, 384));   // the hit rounded up to a tick the playhead has not played

    // Where a hit was heard: its stamp less the output latency (three periods), on the nearest tick, though the
    // playhead has moved on since.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        project->tempo = 1200; // 120 BPM: a tick every 22.96875 frames at 44.1 kHz
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.set_audio_format(44100, 384);
        std::vector<int32_t> out(256);
        host.send({Command::Type::Play}); // the patterns from tick 0, its first tick on the first frame
        for (int b = 0; b < 40; ++b) host.render(out.data(), 128, 1);
        const auto& clock = host.audio_clock();
        CHECK(clock.known() && host.position_of(0) > 200);
        const double tick = 44100 * 60.0 / 120 / ticks_per_quarter;
        const int64_t latency_ns = int64_t(384 * 1e9 / 44100);
        int position = -1;
        uint32_t ticks = 0;
        // In as tick 100's frame reached the jacks: tick 100.
        CHECK(host.heard_place(0, uint64_t(clock.heard_at(uint64_t(100 * tick))), position, ticks) && position == 100 && ticks == 101);
        // In as that frame was rendered: heard 384 frames (16.7 ticks) before, tick 83. The playhead's own place would be 222.
        CHECK(host.heard_place(0, uint64_t(clock.heard_at(uint64_t(100 * tick)) - latency_ns), position, ticks) && position == 83);
        // The nearest tick: four tenths of a tick on, the same; six tenths, the next.
        CHECK(host.place_at(0, uint64_t(100.4 * tick), position, ticks) && position == 100);
        CHECK(host.place_at(0, uint64_t(100.6 * tick), position, ticks) && position == 101 && ticks == 102);
        CHECK(host.place_at(0, 0, position, ticks) && position == 0); // no tick of the run before its first
        CHECK(!host.heard_place(0, 0, position, ticks));              // no stamp: none (the playhead, in Context::record)
        host.send({Command::Type::Stop});
        host.render(out.data(), 128, 1);
        CHECK(!host.place_at(0, uint64_t(10 * tick), position, ticks)); // stopped: none
    }

    // Two hits 5 ms apart (220.5 frames) come out 5 ms apart, each a block after the frame it came in at, not on
    // block starts; one that came in after its frame (a late MIDI thread) plays at once and is counted.
    {
        using K = MidiOutEvent::Kind;
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        synth(project->tracks[0], 0);
        synth(project->tracks[1], 1);
        EngineHost host;
        host.load(*project, *bank);
        tones_for(host, *project);
        host.set_audio_format(44100, 384);
        host.send({Command::Type::MidiTrack, 0, 0, 5, 1}); // MIDI out too, to the frame
        host.send({Command::Type::MidiTrack, 1, 0, 6, 1});
        std::vector<int32_t> out(256);
        std::vector<float> stems(size_t(track_count) * EngineHost::block);
        std::array<std::vector<float>, 2> heard;
        auto render = [&] {
            host.render(out.data(), 128, 1, nullptr, stems.data());
            for (unsigned t = 0; t < 2; ++t) heard[t].insert(heard[t].end(), stems.begin() + t * 128, stems.begin() + (t + 1) * 128);
        };
        for (int b = 0; b < 4; ++b) render();
        const auto& clock = host.audio_clock();
        // In 1 ms into rendering frame 512's block, and 5 ms after.
        const int64_t first = clock.heard_at(512) - int64_t(384 * 1e9 / 44100) + 1000000, second = first + 5000000;
        const uint64_t f1 = clock.frame_at(first) + 128, f2 = clock.frame_at(second) + 128;
        CHECK(f2 - f1 >= 220 && f2 - f1 <= 221 && f1 % 128 != 0 && f2 % 128 != 0);
        host.send_at({Command::Type::NoteOn, 0, 0, 60, 100}, uint64_t(first));
        host.send({Command::Type::Param, 1, 0, 0, project->tracks[1].params[0]}); // untimed: waits behind, still applied
        host.send_at({Command::Type::NoteOn, 1, 0, 60, 100}, uint64_t(second));
        render(); // the block the first hit is taken in, a block before its frame: its MIDI out already
        std::vector<uint64_t> on;
        MidiOutEvent e;
        while (host.take_midi(e)) if (e.kind == K::NoteOn) on.push_back(e.frame);
        CHECK(f1 >= 640 && on.size() == 1 && on[0] == f1 - EngineHost::block);
        for (int b = 0; b < 7; ++b) render();
        while (host.take_midi(e)) if (e.kind == K::NoteOn) on.push_back(e.frame);
        CHECK(on.size() == 2 && on[0] == f1 - EngineHost::block && on[1] == f2 - EngineHost::block); // MIDI out: as played
        auto onset = [&](const std::vector<float>& v) { for (size_t n = 0; n < v.size(); ++n) if (v[n] != 0) return n; return v.size(); };
        CHECK(onset(heard[0]) >= f1 && onset(heard[0]) <= f1 + 2);
        CHECK(onset(heard[1]) >= f2 && onset(heard[1]) <= f2 + 2);
        CHECK(host.timed_kept() == 2 && host.timed_missed() == 0);
        // Late: its frame went by before it came (the MIDI thread woke late). It plays at the next block's start.
        Command late {Command::Type::NoteOn, 0, 0, 62, 100};
        late.timed = true; late.at = uint32_t(f1);
        host.send(late);
        render();
        on.clear();
        while (host.take_midi(e)) if (e.kind == K::NoteOn) on.push_back(e.frame);
        CHECK(on.size() == 1 && on[0] == 11 * 128 && host.timed_missed() == 1 && host.timed_worst_frames() == 12 * 128 - f1);
    }
}

// The audio audit's fixes (2026-11-12): nothing steps where the sound should run on, nothing comes back that
// had gone, and nothing loud is clipped on its way to a file.
void audio_quality() {
    dsp::device_rate.store(44100.0f);
    auto largest_step = [](const std::vector<float>& y, size_t from, size_t to) {
        float most = 0;
        for (size_t k = std::max<size_t>(from, 1); k < std::min(to, y.size()); ++k) most = std::max(most, std::fabs(y[k] - y[k - 1]));
        return most;
    };
    auto sine = [](unsigned frames, double hz, float level) {
        SampleData s;
        s.frames = frames; s.left.resize(frames); s.right.resize(frames);
        for (unsigned n = 0; n < frames; ++n) s.left[n] = s.right[n] = float(level * std::sin(2 * M_PI * hz * n / 44100));
        return s;
    };
    auto play = [](SamplerEngine& e, std::vector<float>& l, unsigned frames) {
        l.assign(frames, 0.0f);
        std::vector<float> r(frames, 0.0f);
        for (unsigned at = 0; at < frames; at += 128) e.render_stereo(l.data() + at, r.data() + at, std::min(128u, frames - at));
    };
    // A kit pad's seamless one-cycle loop plays in tune: 100 frames a turn, not 99.
    {
        SampleData cycle = sine(100, 441, 1);
        SamplerEngine::Table table {};
        table[0] = &cycle;
        SamplerEngine e;
        e.set_samples(&table);
        PadSound pad; pad.sample = 0; pad.loop = true;
        e.set_pad(0, pad);
        e.note_on(int(sampler_first_note), 127);
        std::vector<float> l;
        play(e, l, 44100);
        unsigned rises = 0;
        double first = -1, last = 0;
        for (size_t k = 22051; k < l.size(); ++k)
            if (l[k - 1] < 0 && l[k] >= 0) { double t = double(k - 1) + l[k - 1] / (l[k - 1] - l[k]); if (first < 0) first = t; last = t; ++rises; }
        CHECK(rises > 1 && std::fabs((rises - 1) / ((last - first) / 44100) - 441) < 0.5); // it played 445.45 Hz
    }
    // A full kit: the 33rd voice fades the oldest out (a choke's fade), never cuts it dead.
    {
        SampleData tone = sine(88200, 200, 0.5f), quiet = sine(88200, 200, 0);
        SamplerEngine::Table table {};
        table[0] = &tone; table[1] = &quiet;
        SamplerEngine e;
        e.set_samples(&table);
        PadSound a; a.sample = 0;
        e.set_pad(0, a);
        PadSound b; b.sample = 1;
        for (unsigned p = 1; p < 40; ++p) e.set_pad(p, b);
        std::vector<float> l(128 * 40, 0.0f), r(128 * 40, 0.0f);
        unsigned at = 0;
        auto run = [&](unsigned blocks) { for (unsigned k = 0; k < blocks; ++k, at += 128) e.render_stereo(l.data() + at, r.data() + at, 128); };
        e.note_on(int(sampler_first_note), 127);
        run(4);
        for (unsigned p = 1; p < 32; ++p) { e.note_on(int(sampler_first_note + p), 127); run(1); }
        float natural = largest_step(l, 1, at);
        unsigned steal = at;
        e.note_on(int(sampler_first_note + 32), 127);
        run(4);
        CHECK(largest_step(l, steal - 1, at) < 2 * natural); // it jumped 25 times the sine's own step
    }
    // An empty pad takes no voice: hit with 32 sounding, it fades none of them out (the oldest went quiet).
    {
        SampleData tone = sine(88200, 200, 0.5f), quiet = sine(88200, 200, 0);
        SamplerEngine::Table table {};
        table[0] = &tone; table[1] = &quiet;
        SamplerEngine e;
        e.set_samples(&table);
        PadSound a; a.sample = 0;
        e.set_pad(0, a);
        PadSound b; b.sample = 1;
        for (unsigned p = 1; p < 32; ++p) e.set_pad(p, b);
        std::vector<float> l(128 * 100, 0.0f), r(128 * 100, 0.0f);
        unsigned at = 0;
        auto run = [&](unsigned blocks) { for (unsigned k = 0; k < blocks; ++k, at += 128) e.render_stereo(l.data() + at, r.data() + at, 128); };
        e.note_on(int(sampler_first_note), 127);
        run(4);
        for (unsigned p = 1; p < 32; ++p) { e.note_on(int(sampler_first_note + p), 127); run(1); }
        e.note_on(int(sampler_first_note + 45), 127); // pad 46: nothing on it
        run(60);
        float peak = 0;
        for (unsigned k = at - 128 * 4; k < at; ++k) peak = std::max(peak, std::fabs(l[k]));
        CHECK(peak > 0.1f); // the tone, the oldest, still plays
    }
    // Cloud grains read between frames, and never off their region's end (round to the file's start, as they did).
    {
        SampleData high = sine(44100, 5000, 0.5f);
        SamplerEngine::Table table {};
        table[0] = &high;
        SamplerEngine e;
        e.set_samples(&table);
        PadSound pad; pad.sample = 0; pad.pitch = 1; // a semitone up: every grain reads between frames
        pad.cloud = true; pad.cloud_spread = 0; pad.cloud_size = 60; pad.cloud_density = 20;
        e.set_pad(0, pad);
        e.note_on(int(sampler_first_note), 127);
        std::vector<float> l;
        play(e, l, 44100);
        double hz = 5000 * std::pow(2.0, 1 / 12.0), tone = 0, worst = -300;
        auto level = [&](double f) {
            double re = 0, im = 0, sum = 0;
            for (size_t k = 0; k < 16384; ++k) {
                double w = 0.5 - 0.5 * std::cos(2 * M_PI * k / 16383), ph = 2 * M_PI * f * k / 44100;
                re += l[8192 + k] * w * std::cos(ph); im -= l[8192 + k] * w * std::sin(ph); sum += w;
            }
            return 20 * std::log10(std::max(2 * std::sqrt(re * re + im * im) / sum, 1e-12));
        };
        tone = level(hz);
        for (double f = 300; f < 20000; f += 25) if (std::fabs(f - hz) > 150) worst = std::max(worst, level(f));
        CHECK(tone - worst > 30); // nearest-frame reads left a spur 20 dB under it
        SampleData edged = sine(44100, 200, 0);
        for (unsigned n = 0; n < 44100; ++n) if (n < 10000 || n >= 30000) edged.left[n] = edged.right[n] = (n % 7 < 3 ? 0.9f : -0.9f);
        table[0] = &edged;
        SamplerEngine cut;
        cut.set_samples(&table);
        PadSound region; region.sample = 0; region.start = 10000; region.end = 30000; // silent inside, loud outside
        region.cloud = true; region.cloud_spread = 0; region.cloud_size = 80; region.cloud_density = 50; region.cloud_position = 98;
        cut.set_pad(0, region);
        cut.note_on(int(sampler_first_note), 127);
        play(cut, l, 88200);
        CHECK(peak_of(l) == 0);
    }
    // An echo bypassed and back: what it held when it was bypassed never plays again.
    {
        DelayPool pool;
        Effect echo;
        echo.set_kind(EffectKind::Echo, pool);
        std::vector<float> l(128), r(128);
        float after = 0;
        for (unsigned block = 0; block < 400; ++block) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            if (block == 0) l[0] = r[0] = 1.0f;
            if (block == 20) echo.set_bypass(true);  // before its first echo
            if (block == 200) echo.set_bypass(false);
            echo.process(l.data(), r.data(), 128);
            if (block >= 200) for (float x : l) after = std::max(after, std::fabs(x));
        }
        CHECK(after < 1e-6f); // it played the old impulse at 0.3
    }
    // Punch over a running sound: a repeat's turns, Stutter's edges and Upper's windows never step.
    for (PunchKind kind : {PunchKind::Loop32, PunchKind::Stutter, PunchKind::Upper, PunchKind::RevStepper}) {
        Punch punch;
        std::vector<float> l(128), r(128);
        unsigned n = 0;
        float worst = 0, before = 0;
        for (unsigned block = 0; block < 1000; ++block) {
            for (unsigned f = 0; f < 128; ++f, ++n) l[f] = r[f] = float(0.5 * std::sin(2 * M_PI * 220.0 * n / 44100));
            if (block == 100) punch.start(kind, 1.0f, 88200, 88200 / 16, 0);
            punch.process(l.data(), r.data(), 128);
            for (unsigned f = 0; f < 128; ++f) { if (block >= 100) worst = std::max(worst, std::fabs(l[f] - before)); before = l[f]; }
        }
        float own = float(0.5 * 2 * M_PI * 220 / 44100) * (kind == PunchKind::Upper ? 2 : 1); // Upper plays it an octave up
        if (worst > 2 * own) std::fprintf(stderr, "%s step %f\n", punch_name(kind), worst);
        CHECK(worst < 2 * own); // Loop 32 stepped 0.085, Stutter 0.39, Rev Stepper 0.41, Upper 0.97
    }
    // A track's level and mute, and main volume, ease: no step bigger than the sound's own.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Sampler, 0);
        project->tracks[0].pads[0].sample = 0; project->tracks[0].pads[0].loop = true;
        SampleData tone = sine(44100, 200, 0.5f); // 200 whole cycles: a seamless loop
        EngineHost host;
        host.load(*project, *bank);
        CHECK(host.offer_sample(0, &tone));
        std::vector<int32_t> out(256);
        std::vector<float> heard;
        float volume = 1;
        auto run = [&](unsigned blocks) {
            for (unsigned b = 0; b < blocks; ++b) {
                host.render(out.data(), 128, volume);
                for (unsigned f = 0; f < 128; ++f) heard.push_back(out[2 * f] / 2147483648.0f);
            }
        };
        host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note), 127});
        run(20);
        float natural = largest_step(heard, heard.size() - 128 * 10, heard.size());
        CHECK(natural > 0.005f);
        auto eased = [&](const Command& c, float to_volume) {
            size_t from = heard.size();
            host.send(c);
            volume = to_volume;
            run(8);
            return largest_step(heard, from - 1, heard.size()) < 1.5f * natural;
        };
        CHECK(eased({Command::Type::Level, 0, 0, 0, 50}, 1));   // 0 dB to -12 dB
        CHECK(eased({Command::Type::Level, 0, 0, 0, 100}, 1));
        CHECK(eased({Command::Type::Mute, 0, 0, 0, 1}, 1));     // a mute cut it in one sample
        CHECK(peak_of(heard, heard.size() - 128) == 0);           // and it is silent once eased out
        CHECK(eased({Command::Type::Mute, 0, 0, 0, 0}, 1));
        CHECK(eased({Command::Type::Pan, 0, 0, 0, -50}, 1));
        CHECK(eased({Command::Type::Pan, 0, 0, 0, 0}, 0.25f)); // main volume a quarter at once
    }
    // A mixdown past full scale reads back whole (32-bit float), not clipped at 0 dBFS; a hot sample saved too.
    {
        std::string path = "/tmp/melodee-loud-test.wav";
        WavStream wav;
        CHECK(wav.open(path));
        std::vector<float> loud(2 * 100);
        for (unsigned f = 0; f < 100; ++f) { loud[2 * f] = 1.5f * std::sin(f * 0.1f); loud[2 * f + 1] = -loud[2 * f]; }
        CHECK(wav.add(loud.data(), 100) && wav.close());
        SampleData read;
        CHECK(read_wav(path, read).empty() && read.frames == 100 && std::fabs(read.left[15] - loud[30]) < 1e-6f);
        SampleData hot;
        hot.frames = 3; hot.left.resize(3); hot.right.resize(3);
        hot.left[0] = 1.25f; hot.left[1] = -0.5f; hot.right[0] = -1.75f; hot.right[1] = 0.25f;
        CHECK(write_wav(path, hot).empty() && read_wav(path, read).empty() && std::fabs(read.left[0] - 1.25f) < 1e-6f
              && std::fabs(read.right[0] + 1.75f) < 1e-6f);
        std::remove(path.c_str());
    }
}

// Roadmap P3: Pad Link through more than one pad, round the bank once; a muted pad links on.
void pad_link_chains() {
    SampleData data;
    data.frames = 44100;
    data.left.assign(data.frames, 0.3f); data.right = data.left;
    SamplerEngine::Table table {};
    table[1] = &data;
    auto engine = std::make_unique<SamplerEngine>();
    engine->set_samples(&table);
    std::vector<float> l(128), r(128);
    auto pads_heard = [&](int hit) {
        engine->note_on(int(sampler_first_note) + hit, 100);
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        engine->render_stereo(l.data(), r.data(), 128);
        TrackTaps t; t.track = 0; engine->taps(t);
        std::vector<int> pads;
        for (unsigned n = 0; n < t.count; ++n) pads.push_back(t.voice[n].pad);
        std::sort(pads.begin(), pads.end());
        engine->silence();
        return pads;
    };
    PadSound pad;
    pad.sample = 1;
    PadSound a = pad, b = pad, c = pad;
    a.link = 2; b.link = 3; c.link = 1; // A01 -> A02 -> A03 -> back to A01: each once
    engine->set_pad(0, a); engine->set_pad(1, b); engine->set_pad(2, c);
    CHECK((pads_heard(0) == std::vector<int> {0, 1, 2}));
    CHECK((pads_heard(1) == std::vector<int> {0, 1, 2}));
    engine->set_pad_mute(1, true); // A02 silent, A03 still reached through it
    CHECK((pads_heard(0) == std::vector<int> {0, 2}));
    engine->set_pad_mute(1, false);
    PadSound d = pad; d.link = 2; // B01 -> B02: a link stays in its own bank
    engine->set_pad(16, d); engine->set_pad(17, pad);
    CHECK((pads_heard(16) == std::vector<int> {16, 17}));
}


// Chop's slices onto pads keep the chopped pad's values; what was its region's alone goes.
void chop_slices_keep_pad() {
    PadSound parent;
    parent.sample = 3; parent.start = 1000; parent.end = 90000;
    parent.pitch = -5; parent.fine = 20; parent.warp = warp_seq; parent.own_tempo = 924; parent.beats = 8;
    parent.group = 4; parent.play = PadPlay::Gate; parent.mono = true; parent.level = 80; parent.pan = -12;
    parent.filter = PadFilter::Lpf4; parent.cutoff = 40; parent.decay = 30;
    parent.link = 2; parent.loop = true; parent.loop_lock = false; parent.loop_start = 5000;
    parent.layers[0].sample = 7; parent.low_velocity = 40;
    // Warp's Seq by # Beats: 8 beats over the parent's 89000 frames is 237.8 BPM, the slice's own tempo.
    CHECK(region_frames(parent, 0) == 89000 && region_frames(parent, 50000) == 49000);
    PadSound one = slice_sound(parent, 20000, 30000, region_frames(parent, 0));
    CHECK(one.sample == 3 && one.start == 20000 && one.end == 30000);
    CHECK(one.pitch == -5 && one.fine == 20 && one.warp == warp_seq && one.own_tempo == 2378 && one.group == 4);
    CHECK(one.play == PadPlay::Gate && one.mono && one.level == 80 && one.pan == -12 && one.filter == PadFilter::Lpf4 && one.cutoff == 40 && one.decay == 30);
    CHECK(one.loop && one.loop_point() == 20000 && one.link == 0 && one.beats == 0 && !one.layered() && one.plain_layers());
    CHECK(slice_sound(parent, 20000, 30000, 0).own_tempo == 924); // the region unknown: the tempo it had
    PadSound by_tempo = parent; by_tempo.beats = 0;
    CHECK(slice_sound(by_tempo, 20000, 30000, 89000).own_tempo == 924);
    PadSound longer = parent; longer.warp = 150;
    CHECK(slice_sound(longer, 20000, 30000, 89000).own_tempo == 924 && slice_sound(longer, 20000, 30000, 89000).warp == 150);
    PadSound whole = parent; whole.end = 0; // to the sample's end
    CHECK(region_frames(whole, 89000) == 88000 && beats_tempo(2, 44100) == 1200);
    // Chop's audition: the pad's speed and pitch alone.
    const PadSound heard = slice_heard(parent, 20000, 30000, 89000);
    CHECK(heard.sample == 3 && heard.start == 20000 && heard.end == 30000 && heard.pitch == -5 && heard.fine == 20);
    CHECK(heard.warp == warp_seq && heard.own_tempo == 2378 && heard.beats == 0);
    CHECK(heard.play == PadPlay::OneShot && !heard.loop && heard.group == 0 && heard.filter == PadFilter::Off && heard.level == 100 && heard.pan == 0);
}

// Chop's audition with a kit pad plays at that pad's speed: Warp 200 % twice as long; with no pad, or a pad that
// holds another sample, as the file is.
void chop_audition_pad_speed() {
    dsp::device_rate.store(44100.0f);
    SampleData data;
    data.frames = 44100; data.left.assign(44100, 0.5f); data.right.assign(44100, 0.5f);
    SamplerEngine::Table table {};
    table[2] = &data;
    auto blocks = [&](int pad, uint16_t holds) {
        SamplerEngine engine;
        engine.set_samples(&table);
        PadSound p; p.sample = holds; p.warp = 200;
        engine.set_pad(4, p);
        engine.audition(2, 0, 4410, 100, pad);
        std::vector<float> l(128), r(128);
        unsigned n = 0;
        while (engine.sounding() && n < 400) { engine.render_stereo(l.data(), r.data(), 128); ++n; }
        return n;
    };
    const unsigned plain = blocks(-1, 2), warped = blocks(4, 2), other = blocks(4, 3);
    CHECK(plain > 30 && plain < 45);
    CHECK(warped > plain * 18 / 10 && warped < plain * 22 / 10);
    CHECK(other == plain);
}


// Roadmap P3, instruments: release samples, crossfades over velocity, keyswitches.
void instrument_p3() {
    { // xfin / xfout: a straight gain across the range, the power curve its root
        SampleZone z;
        z.xfin_lovel = 0; z.xfin_hivel = 64; z.xfout_lovel = 64; z.xfout_hivel = 127; z.vel_power = false;
        CHECK(zone_crossfade(z, 60, 0) == 0.0f && std::fabs(zone_crossfade(z, 60, 32) - 0.5f) < 0.01f && zone_crossfade(z, 60, 64) == 1.0f);
        CHECK(std::fabs(zone_crossfade(z, 60, 127)) < 1e-6f);
        z.vel_power = true;
        CHECK(std::fabs(zone_crossfade(z, 60, 32) - std::sqrt(0.5f)) < 0.01f);
        SampleZone keys;
        keys.xfin_lokey = 48; keys.xfin_hikey = 60;
        CHECK(zone_crossfade(keys, 40, 100) == 0.0f && zone_crossfade(keys, 70, 100) == 1.0f);
    }
    { // a zone faded out at this velocity does not sound at all
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100));
        auto& z = r.zone(0); z.xfin_lovel = 50; z.xfin_hivel = 100;
        r.pad.velocity_sense = 0;
        r.build();
        r.key(0, 40);
        CHECK(zone_rms(r.run(4000), 500, 4000) < 1e-6);
        r.key(0, 110);
        CHECK(zone_rms(r.run(4000), 500, 4000) > 0.1);
    }
    { // a release sample plays when the key is let go, quieter the longer it was held
        auto played = [](unsigned held_blocks) {
            ZoneRig r;
            r.part(zone_tone(440, 0.5f, 44100)); r.part(zone_tone(880, 0.5f, 44100));
            r.zone(0);
            auto& tail = r.zone(1); tail.on_release = true; tail.rt_decay = 6;
            r.multi->has_release = true;
            r.pad.velocity_sense = 0; r.pad.release = 0;
            r.build(PadPlay::Gate);
            r.key(0, 100);
            auto pressed = r.run(4000);
            if (held_blocks) r.run(held_blocks * 128);
            r.off(0);
            auto after = r.run(8000);
            return std::make_tuple(zone_hz(pressed, 500, 4000), zone_hz(after, 3000, 8000), zone_rms(after, 3000, 8000));
        };
        auto [first, second, loud] = played(0);
        CHECK(std::fabs(first - 440) < 10 && std::fabs(second - 880) < 15); // the press plays its zone alone; the release the other
        auto [a, b, quiet] = played(345); // about a second more held: 6 dB down
        (void)a; (void)b;
        CHECK(quiet < loud * 0.62 && quiet > loud * 0.38);
    }
    { // keyswitches: the last one pressed picks the zones; it sounds nothing itself
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100)); r.part(zone_tone(880, 0.5f, 44100));
        auto& legato = r.zone(0); legato.switch_key = 24; legato.root = 60;
        auto& staccato = r.zone(1); staccato.switch_key = 25; staccato.root = 60; staccato.keytrack = 100;
        r.multi->switch_low = 24; r.multi->switch_high = 25; r.multi->switch_default = 24;
        r.build();
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 440) < 8);
        r.engine->silence();
        r.key(25 - 60);
        CHECK(zone_rms(r.run(2000), 200, 2000) < 1e-6); // the switch itself: silent
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880) < 12);
    }
    { // SFZ: what the reader takes of them
        auto read = read_sfz("<global> sw_lokey=24 sw_hikey=25 sw_default=24\n<region> sample=note.wav key=60 sw_last=24\n<region> sample=other.wav key=60 sw_last=25\n"
                             "<region> sample=tail.wav key=60 trigger=release rt_decay=3 sw_last=24\n<region> sample=soft.wav key=61 xfin_lovel=0 xfin_hivel=40 xf_velcurve=gain\n"
                             "<region> sample=slur.wav key=61 trigger=legato\n");
        CHECK(read.regions.size() == 4 && read.left == 1 && read.left_by == std::set<std::string>({"trigger"}));
        CHECK(read.switch_low == 24 && read.switch_high == 25 && read.switch_default == 24);
        CHECK(read.regions[0].zone.switch_key == 24 && read.regions[1].zone.switch_key == 25 && !read.regions[0].zone.on_release);
        CHECK(read.regions[2].zone.on_release && read.regions[2].zone.rt_decay == 3.0f);
        CHECK(read.regions[3].zone.xfin_hivel == 40 && !read.regions[3].zone.vel_power && read.regions[3].zone.switch_key == -1);
        CHECK(read.ignored.empty());
    }
}

// A SoundFont made here: one preset, its instrument two sounds panned apart (a stereo pair), looped.
std::vector<uint8_t> tiny_sf2() {
    std::vector<uint8_t> out;
    auto u8 = [](std::vector<uint8_t>& v, unsigned x) { v.push_back(uint8_t(x)); };
    auto u16 = [&](std::vector<uint8_t>& v, unsigned x) { u8(v, x & 0xff); u8(v, (x >> 8) & 0xff); };
    auto u32 = [&](std::vector<uint8_t>& v, uint32_t x) { u16(v, x & 0xffff); u16(v, x >> 16); };
    auto name = [&](std::vector<uint8_t>& v, const char* text, size_t size) { for (size_t n = 0; n < size; ++n) u8(v, n < std::strlen(text) ? uint8_t(text[n]) : 0); };
    auto chunk = [&](const char* id, const std::vector<uint8_t>& body) {
        std::vector<uint8_t> c(id, id + 4);
        u32(c, uint32_t(body.size()));
        c.insert(c.end(), body.begin(), body.end());
        if (body.size() % 2) c.push_back(0);
        return c;
    };
    auto list = [&](const char* kind, std::vector<std::vector<uint8_t>> parts) {
        std::vector<uint8_t> body(kind, kind + 4);
        for (auto& p : parts) body.insert(body.end(), p.begin(), p.end());
        return chunk("LIST", body);
    };
    std::vector<uint8_t> ifil; u16(ifil, 2); u16(ifil, 1);
    std::vector<uint8_t> smpl;
    for (unsigned n = 0; n < 2000; ++n) u16(smpl, uint16_t(int16_t(n < 1000 ? 8000 : -8000)));
    std::vector<uint8_t> phdr, pbag, pmod(10, 0), pgen, inst, ibag, imod(10, 0), igen, shdr;
    name(phdr, "Tiny", 20); u16(phdr, 5); u16(phdr, 0); u16(phdr, 0); u32(phdr, 0); u32(phdr, 0); u32(phdr, 0);
    name(phdr, "EOP", 20); u16(phdr, 0); u16(phdr, 0); u16(phdr, 1); u32(phdr, 0); u32(phdr, 0); u32(phdr, 0);
    u16(pbag, 0); u16(pbag, 0); u16(pbag, 1); u16(pbag, 0);
    u16(pgen, 41); u16(pgen, 0); u16(pgen, 0); u16(pgen, 0);
    name(inst, "Pair", 20); u16(inst, 0);
    name(inst, "EOI", 20); u16(inst, 2);
    u16(ibag, 0); u16(ibag, 0); u16(ibag, 4); u16(ibag, 0); u16(ibag, 8); u16(ibag, 0);
    for (int side = 0; side < 2; ++side) {
        u16(igen, 43); u16(igen, 60 | 72 << 8);
        u16(igen, 17); u16(igen, uint16_t(int16_t(side ? 500 : -500)));
        u16(igen, 54); u16(igen, 1);
        u16(igen, 53); u16(igen, unsigned(side));
    }
    u16(igen, 0); u16(igen, 0);
    for (int side = 0; side < 2; ++side) {
        name(shdr, side ? "Tone R" : "Tone L", 20);
        u32(shdr, side * 1000); u32(shdr, side * 1000 + 1000); u32(shdr, side * 1000 + 100); u32(shdr, side * 1000 + 900); u32(shdr, 22050);
        u8(shdr, 64); u8(shdr, 0); u16(shdr, unsigned(1 - side)); u16(shdr, side ? 2 : 4);
    }
    name(shdr, "EOS", 20); for (int n = 0; n < 26; ++n) u8(shdr, 0);
    auto info = list("INFO", {chunk("ifil", ifil)});
    auto sdta = list("sdta", {chunk("smpl", smpl)});
    auto pdta = list("pdta", {chunk("phdr", phdr), chunk("pbag", pbag), chunk("pmod", pmod), chunk("pgen", pgen), chunk("inst", inst),
                              chunk("ibag", ibag), chunk("imod", imod), chunk("igen", igen), chunk("shdr", shdr)});
    std::vector<uint8_t> body {'s', 'f', 'b', 'k'};
    for (auto* part : {&info, &sdta, &pdta}) body.insert(body.end(), part->begin(), part->end());
    out = chunk("RIFF", body);
    return out;
}

// Roadmap P3: SF2 and DecentSampler instruments read as zones.
void instrument_formats_p3() {
    {
        auto bytes = tiny_sf2();
        Sf2 file;
        CHECK(file.open(bytes.data(), bytes.size()).empty());
        CHECK(file.presets().size() == 1 && file.presets()[0].program == 5 && file.presets()[0].name == "Tiny" && file.plays(file.presets()[0]));
        auto read = file.read(file.presets()[0]);
        CHECK(read.regions.size() == 1 && read.left == 0); // two sides, one stereo sound
        const auto& r = read.regions[0];
        CHECK(r.sample == "0+1" && r.zone.low_key == 60 && r.zone.high_key == 72 && r.zone.root == 64 && r.zone.pan == 0);
        CHECK(r.zone.loop == ZoneLoop::On && r.zone.loop_start == 100 && r.zone.loop_end == 900);
        std::vector<float> left, right;
        unsigned rate = 0;
        int64_t from = 0, to = 0;
        CHECK(file.sound(r.sample, left, right, rate, from, to));
        CHECK(left.size() == 1000 && right.size() == 1000 && rate == 22050 && from == 100 && to == 900);
        CHECK(left[10] > 0.2f && right[10] < -0.2f);
        CHECK(!file.sound("7", left, right, rate, from, to));
        std::string named;
        unsigned preset = 0;
        CHECK(split_sf2_preset("/media/card/Samples/Piano.SF2#12", named, preset) && named == "/media/card/Samples/Piano.SF2" && preset == 12);
        CHECK(!split_sf2_preset("/media/card/Samples/Kit.xpm#2", named, preset));
        SampleRef ref;
        std::snprintf(ref.path, sizeof(ref.path), "/media/card/Piano.sf2#3");
        CHECK(ref.instrument());
        std::snprintf(ref.path, sizeof(ref.path), "/media/card/Strings.dspreset");
        CHECK(ref.instrument());
        std::vector<uint8_t> broken(bytes.begin(), bytes.begin() + 40);
        Sf2 bad;
        CHECK(!bad.open(broken.data(), broken.size()).empty());
    }
    {
        DsElement root {"DecentSampler", {}, {}};
        DsElement groups {"groups", {{"attack", "0.01"}, {"release", "0.5"}}, {}};
        DsElement group {"group", {{"volume", "-6dB"}, {"seqMode", "round_robin"}}, {}};
        group.children.push_back({"sample", {{"path", "Samples/C3 a.wav"}, {"rootNote", "48"}, {"loNote", "46"}, {"hiNote", "50"}, {"seqPosition", "1"}}, {}});
        group.children.push_back({"sample", {{"path", "Samples/C3 b.wav"}, {"rootNote", "48"}, {"loNote", "46"}, {"hiNote", "50"}, {"seqPosition", "2"}}, {}});
        DsElement release_group {"group", {{"trigger", "release"}}, {}};
        release_group.children.push_back({"sample", {{"path", "Samples/C3 rel.wav"}, {"rootNote", "48"}, {"loopEnabled", "true"}, {"loopStart", "10"}, {"loopEnd", "99"}}, {}});
        DsElement off_group {"group", {{"enabled", "false"}}, {}};
        off_group.children.push_back({"sample", {{"path", "Samples/off.wav"}}, {}});
        groups.children = {group, release_group, off_group};
        root.children.push_back(groups);
        root.children.push_back({"effects", {}, {{"effect", {{"type", "reverb"}}, {}}}});
        auto read = read_dspreset(root);
        CHECK(read.regions.size() == 3 && read.left == 1 && read.left_by.count("enabled"));
        CHECK(read.regions[0].sample == "Samples/C3 a.wav" && read.regions[0].zone.root == 48 && read.regions[0].zone.low_key == 46);
        CHECK(read.regions[0].zone.sequence == 1 && read.regions[1].zone.sequence == 2 && read.regions[1].zone.sequence_length == 2);
        CHECK(std::fabs(read.regions[0].zone.gain + 6) < 0.01f && read.ignored.count("effects"));
        CHECK(read.regions[2].zone.on_release && read.regions[2].zone.loop == ZoneLoop::On && read.regions[2].zone.loop_end == 100);
        CHECK(read.envelope.release && std::fabs(*read.envelope.release - 0.5) < 1e-9);
        std::string why;
        read_dspreset(DsElement {"NotAPreset", {}, {}}, &why);
        CHECK(!why.empty());
        CHECK(dspreset_decibels("0.5") && std::fabs(*dspreset_decibels("0.5") + 6.02) < 0.01);
    }
    { // the preset as its face first shows it: each control's value through its bindings
        auto knob = [](std::map<std::string, std::string> a, std::vector<DsElement> bindings) { return DsElement {"labeled-knob", std::move(a), std::move(bindings)}; };
        auto binding = [](std::map<std::string, std::string> a) { return DsElement {"binding", std::move(a), {}}; };
        DsElement ui {"ui", {}, {{"tab", {}, {
            // a sample start past the files' 250 ms of silence (SamuFL's Home Percussions, TIGHT)
            knob({{"minValue", "-50"}, {"maxValue", "0"}, {"value", "0"}},
                 {binding({{"type", "general"}, {"level", "instrument"}, {"position", "0"}, {"parameter", "SAMPLE_START"},
                           {"translation", "linear"}, {"translationOutputMin", "9600"}, {"translationOutputMax", "12000"}})}),
            // one fader: the natural layer up, the hyped one down
            knob({{"value", "0"}},
                 {binding({{"type", "amp"}, {"level", "group"}, {"tags", "natural"}, {"parameter", "AMP_VOLUME"}, {"translation", "table"}, {"translationTable", "0,1;1,0"}}),
                  binding({{"type", "amp"}, {"level", "group"}, {"tags", "hyped"}, {"parameter", "AMP_VOLUME"}, {"translation", "table"}, {"translationTable", "0,0;1,1"}}),
                  binding({{"type", "effect"}, {"level", "instrument"}, {"position", "1"}, {"parameter", "LEVEL"}})}),
            // a value past its range is held to it; no output range passes it as it is
            knob({{"minValue", "0"}, {"maxValue", "4"}, {"value", "2000"}}, {binding({{"type", "amp"}, {"level", "instrument"}, {"parameter", "ENV_RELEASE"}})}),
            knob({{"value", "0.25"}}, {binding({{"type", "amp"}, {"level", "instrument"}, {"parameter", "ENV_ATTACK"}, {"triggerOnLoad", "false"}})}),
            {"menu", {{"value", "2"}}, {{"option", {}, {binding({{"type", "amp"}, {"level", "group"}, {"position", "2"}, {"parameter", "ENABLED"}, {"translation", "fixed_value"}, {"translationValue", "true"}})}},
                                        {"option", {}, {binding({{"type", "amp"}, {"level", "group"}, {"position", "2"}, {"parameter", "ENABLED"}, {"translation", "fixed_value"}, {"translationValue", "false"}})}}}},
        }}}};
        DsElement groups {"groups", {{"attack", "0.0"}, {"release", "1.75"}}, {}};
        groups.children.push_back({"group", {{"tags", "natural"}, {"volume", "6dB"}}, {{"sample", {{"path", "Samples/Thump.wav"}, {"rootNote", "60"}, {"loNote", "60"}, {"hiNote", "60"}}, {}}}});
        groups.children.push_back({"group", {{"tags", "hyped"}, {"volume", "8dB"}}, {{"sample", {{"path", "Samples/Thump_HYPED.wav"}, {"rootNote", "60"}, {"loNote", "60"}, {"hiNote", "60"}}, {}}}});
        groups.children.push_back({"group", {}, {{"sample", {{"path", "Samples/Slap.wav"}, {"rootNote", "62"}, {"start", "0"}}, {}}}});
        DsElement root {"DecentSampler", {}, {ui, groups}};
        auto read = read_dspreset(root);
        CHECK(read.regions.size() == 1 && read.regions[0].sample == "Samples/Thump.wav");
        CHECK(read.regions[0].offset == 12000 && std::fabs(read.regions[0].zone.gain) < 0.01f);
        CHECK(read.left == 2 && read.left_by.count("volume") && read.left_by.count("enabled"));
        CHECK(read.envelope.release && std::fabs(*read.envelope.release - 4) < 1e-9);
        CHECK(read.envelope.attack && *read.envelope.attack == 0);
        // a start set at level instrument reaches a sample that names its own (the format keeps start on samples only)
        DsElement pad_ui {"ui", {}, {knob({{"minValue", "0.001"}, {"maxValue", "2"}, {"value", "1"}},
            {binding({{"type", "general"}, {"level", "instrument"}, {"position", "0"}, {"parameter", "SAMPLE_START"}, {"translation", "linear"},
                      {"translationInputMin", "0"}, {"translationInputMax", "2"}, {"translationOutputMin", "96000"}, {"translationOutputMax", "0"}}),
             binding({{"type", "amp"}, {"level", "instrument"}, {"position", "0"}, {"parameter", "ENV_ATTACK"}})})}};
        DsElement pad_groups {"groups", {{"attack", "0.01"}}, {{"group", {{"tags", "pad"}}, {{"sample", {{"path", "Samples/Pad.wav"}, {"start", "0"}}, {}}}}}};
        auto pad = read_dspreset(DsElement {"DecentSampler", {}, {pad_ui, pad_groups}});
        CHECK(pad.regions.size() == 1 && pad.regions[0].offset == 48000);
        CHECK(pad.envelope.attack && std::fabs(*pad.envelope.attack - 1) < 1e-9);
        // a tag turned off by a button's first state
        DsElement tag_ui {"ui", {}, {{"button", {{"value", "0"}}, {{"state", {}, {binding({{"type", "general"}, {"level", "instrument"}, {"identifier", "room"},
            {"parameter", "TAG_ENABLED"}, {"translation", "fixed_value"}, {"translationValue", "false"}})}}}}}};
        DsElement tag_groups {"groups", {}, {{"group", {{"tags", "room"}}, {{"sample", {{"path", "Samples/Room.wav"}}, {}}}},
                                             {"group", {}, {{"sample", {{"path", "Samples/Close.wav"}}, {}}}}}};
        auto tagged = read_dspreset(DsElement {"DecentSampler", {}, {tag_ui, tag_groups}});
        CHECK(tagged.regions.size() == 1 && tagged.regions[0].sample == "Samples/Close.wav" && tagged.left_by.count("tags"));
    }
    { // 16 Levels' Zones: an instrument's pad plays the keys its zones sound on, lowest on pad 1, sixteen to a page
        auto project = std::make_unique<Project>();
        auto& t = project->tracks[0];
        t.engine = EngineKind::Sampler; t.levels = true; t.levels_pad = 3; t.octave = kit_level_octave;
        auto& ref = project->samples.edit(5);
        std::snprintf(ref.path, sizeof(ref.path), "/media/card/Samples/Kit.dspreset");
        t.pads[3].sample = 5; t.pads[3].pitch = 2;
        for (int k : {60, 62, 65}) ref.keys[unsigned(k) / 64] |= uint64_t(1) << (unsigned(k) % 64);
        for (int k = 70; k < 90; ++k) ref.keys[unsigned(k) / 64] |= uint64_t(1) << (unsigned(k) % 64);
        t.levels_kind = uint8_t(LevelsKind::Zones);
        auto first = levels_of(*project, t, 0);
        CHECK(!first.none && first.shown == 60 && first.semitones == -2); // a hit plays 60 + the pad's Semi + these
        CHECK(levels_of(*project, t, 2).shown == 65 && levels_of(*project, t, 3).shown == 70 && levels_of(*project, t, 15).shown == 82);
        std::array<LevelsKind, 6> types {};
        CHECK(levels_types(*project, t, types) == 6 && types[2] == LevelsKind::Zones && types[5] == LevelsKind::Chords);
        CHECK(zone_key_count(ref) == 23 && zone_pages(ref) == 2);
        t.octave = kit_level_octave + 1; // − / +: the next page
        CHECK(levels_of(*project, t, 0).shown == 83 && levels_of(*project, t, 6).shown == 89 && levels_of(*project, t, 7).none);
        t.octave = kit_highest_octave; CHECK(levels_of(*project, t, 0).shown == 83); // held to the last page
        // 16 Levels opens in Zones on a drum or percussion instrument's pad in place of Scale or Tune, not of Velocity
        t.levels_kind = uint8_t(LevelsKind::Scale); CHECK(levels_opening(*project, t) == LevelsKind::Scale); // a melodic one
        ref.one_key_zones = true;
        CHECK(levels_opening(*project, t) == LevelsKind::Zones);
        t.levels_kind = uint8_t(LevelsKind::Tune); CHECK(levels_opening(*project, t) == LevelsKind::Zones);
        t.levels_kind = uint8_t(LevelsKind::Velocity); CHECK(levels_opening(*project, t) == LevelsKind::Velocity);
        t.levels_kind = uint8_t(LevelsKind::Zones);
        // a plain sample's pad has no Zones: it plays as Scale
        std::snprintf(project->samples.edit(6).path, sizeof(SampleRef::path), "/media/card/Samples/Hit.wav");
        t.pads[4].sample = 6; t.levels_pad = 4;
        CHECK(!levels_of(*project, t, 0).none && levels_types(*project, t, types) == 5 && levels_opening(*project, t) == LevelsKind::Scale);
    }
}

// Roadmap P3: Chop's slices kept with the project (CHOP; older files: none, a damaged one dropped).
void chop_kept() {
    // A sample's chop is kept in its WAV file (shell/mpc_wav_slices.h), not in the project: a save leaves it out.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    std::snprintf(project->samples.edit(7).path, sizeof(SampleRef::path), "break.wav");
    auto& c = project->chops.edit(7);
    c.from = 1000; c.to = 90000; c.mode = ChopMode::Manual; c.amount = 0;
    c.slices.whole(1000, 90000); c.slices.add(20000, 100000); c.slices.add(51000, 100000);
    auto file = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(file, *back, *back_bank).empty() && !back->chops[7].used());
    std::string text(file.begin(), file.end());
    CHECK(text.find("CHOP") == std::string::npos && text.find("CHP2") == std::string::npos);
    // A project from before: its CHOP (the starts and where the last slice ends, every slice touching the next)
    // read, to be written into the WAV at the next save.
    auto old_chop = [&](uint32_t second) {
        std::vector<uint8_t> section {'C', 'H', 'O', 'P', 0, 0, 0, 0, 7};
        auto u32 = [&](uint32_t v) { for (int b = 0; b < 4; ++b) section.push_back(uint8_t(v >> (8 * b))); };
        u32(1000); u32(90000); section.push_back(uint8_t(ChopMode::Manual)); section.push_back(0); section.push_back(0);
        section.push_back(3); u32(90000); u32(1000); u32(second); u32(51000);
        uint32_t size = uint32_t(section.size() - 8);
        for (int b = 0; b < 4; ++b) section[size_t(4 + b)] = uint8_t(size >> (8 * b));
        auto old = file;
        size_t at = text.find("SMPL");
        old.insert(old.begin() + long(at), section.begin(), section.end());
        return old;
    };
    CHECK(load_project(old_chop(20000), *back, *back_bank).empty());
    const auto& o = back->chops[7];
    CHECK(o.used() && o.unwritten && o.slices.count == 3 && o.slices.end(0, 100000) == 20000 && o.slices.end(1, 100000) == 51000
          && o.slices.end(2, 100000) == 90000);
    CHECK(load_project(old_chop(500), *back, *back_bank).empty() && !back->chops[7].used()); // the second start before the first
}

// What the pager reads from a file stays in memory once nothing counts on it (engine/stream_pager.h): a chunk let
// go and held again is not read again; past the budget the chunk longest unused is given back, and read again when
// it is wanted; a chunk counted on is never given back; the file stays open between reads; and a part's end takes
// its chunks out of the count.
void stream_cache_keeps() {
    const std::string path = "/tmp/melodee-cache-" + std::to_string(::getpid()) + ".wav";
    const unsigned frames = stream_chunk * 8;
    auto write = [&](std::ofstream& f, int add) { for (unsigned n = 0; n < frames; ++n) put16(f, uint16_t(int16_t(n % 20000 + add))); };
    {
        std::ofstream f(path, std::ios::binary);
        f.write("RIFF", 4); put32(f, 36 + frames * 2); f.write("WAVEfmt ", 8); put32(f, 16); put16(f, 1); put16(f, 1);
        put32(f, 44100); put32(f, 44100 * 2); put16(f, 2); put16(f, 16); f.write("data", 4); put32(f, frames * 2);
        write(f, 0);
    }
    auto was = [](uint32_t n) { return float(int16_t(n % 20000)) / 32768.0f; };
    auto now = [](uint32_t n) { return float(int16_t(n % 20000 + 100)) / 32768.0f; };
    const uint64_t before = stream_cache_held(), one = stream_chunk_bytes;
    {
        WavReader wav;
        ZonePart part;
        CHECK(wav.open(path, 60).empty() && read_in_place(path, wav.layout(), part));
        StreamPager pager;
        CHECK(pager.hold_chunk(part, 0) && pager.hold_chunk(part, 1));
        CHECK(stream_cache_held() == before + 2 * one && part.left[5] == was(5) && part.left[stream_chunk + 5] == was(stream_chunk + 5));
        pager.let_go(part, 0);
        CHECK(stream_cache_held() == before + 2 * one); // nothing counts on it: it stays
        { // the file's frames changed where it lies: a read would show
            std::ofstream f(path, std::ios::binary | std::ios::in | std::ios::out); // not cut short
            f.seekp(44);
            write(f, 100);
        }
        CHECK(pager.hold_chunk(part, 0) && part.left[5] == was(5)); // held again: not read again
        pager.let_go(part, 0);
        pager.let_go(part, 1);
        stream_cache_budget(before + one); // room for one: the one longest unused goes
        CHECK(stream_cache_held() == before + one);
        CHECK(pager.hold_chunk(part, 1) && part.left[stream_chunk + 5] == was(stream_chunk + 5)); // the later one stayed
        CHECK(pager.hold_chunk(part, 0) && part.left[5] == now(5));                               // the earlier is read again
        CHECK(stream_cache_held() == before + 2 * one); // both counted on: neither is given back
        pager.let_go(part, 1);
        CHECK(stream_cache_held() == before + one && part.left[5] == now(5)); // let go past the budget: given back at once
        // The file stays open: moved away, a chunk not read yet is still read.
        CHECK(std::rename(path.c_str(), (path + ".moved").c_str()) == 0);
        CHECK(pager.hold_chunk(part, 3) && part.left[3 * stream_chunk + 7] == now(3 * stream_chunk + 7));
        CHECK(stream_cache_held() == before + 2 * one);
    }
    CHECK(stream_cache_held() == before); // the part gone: its chunks counted no more
    stream_cache_budget(stream_cache_bytes);
    // What is given back for a load that needs the memory: every chunk nothing counts on.
    {
        CHECK(std::rename((path + ".moved").c_str(), path.c_str()) == 0);
        WavReader wav;
        ZonePart part;
        CHECK(wav.open(path, 60).empty() && read_in_place(path, wav.layout(), part));
        StreamPager pager;
        CHECK(pager.hold_chunk(part, 0) && pager.hold_chunk(part, 1) && pager.hold_chunk(part, 2));
        pager.let_go(part, 0);
        pager.let_go(part, 2);
        CHECK(stream_cache_give_back() == 2 * one && stream_cache_held() == before + one && part.left[stream_chunk + 5] == now(stream_chunk + 5));
        CHECK(std::rename(path.c_str(), (path + ".moved").c_str()) == 0);
    }
    // Which WAVs play from the card: the long ones, and with no room in memory those past two seconds.
    CHECK(streams_from_card(11 * 44100, 44100, true) && !streams_from_card(10 * 44100, 44100, true));
    CHECK(!streams_from_card(5 * 48000, 48000, true) && streams_from_card(5 * 48000, 48000, false));
    CHECK(!streams_from_card(2 * 48000, 48000, false) && !streams_from_card(44100, 44100, false));
    CHECK(stream_head_frames(44100) == 22050 && stream_head_frames(96000) == 48000 && stream_head_frames(22050) == stream_chunk);
    std::remove((path + ".moved").c_str());
}

// A long WAV read from the card as it plays (SampleData::stream, read in place): a pad on it plays its file's own
// frames from where it starts (held: at once, no wait), on past the held chunk as the pager reads ahead, reversed from
// its held end, and round its loop from the held loop point; where a project's pads on long samples need holding
// (shell/stream_holds.h); and hits found in a given 44.1 kHz sum as in a sample in memory.
void long_samples_streamed() {
    const std::string path = "/tmp/melodee-long-" + std::to_string(::getpid()) + ".wav";
    const unsigned rate = 48000, frames = rate * 20;
    auto value = [](uint32_t n) { return float(int16_t(12000 * std::sin(n * 0.013))) / 32768.0f; };
    {
        std::ofstream f(path, std::ios::binary);
        f.write("RIFF", 4); put32(f, 36 + frames * 4); f.write("WAVEfmt ", 8); put32(f, 16); put16(f, 1); put16(f, 2);
        put32(f, rate); put32(f, rate * 4); put16(f, 4); put16(f, 16); f.write("data", 4); put32(f, frames * 4);
        for (unsigned n = 0; n < frames; ++n) { int16_t v = int16_t(12000 * std::sin(n * 0.013)); put16(f, uint16_t(v)); put16(f, uint16_t(int16_t(-v / 2))); }
    }
    WavReader wav;
    CHECK(wav.open(path, stream_most_seconds).empty() && wav.frames() == frames && uint64_t(wav.frames()) > uint64_t(stream_seconds) * wav.rate());
    auto data = std::make_unique<SampleData>();
    data->stream = std::make_shared<ZonePart>();
    CHECK(read_in_place(path, wav.layout(), *data->stream));
    data->stream->frames.store(data->stream->total);
    data->frames = data->stream->total; data->rate = rate;
    CHECK(data->left.empty() && data->model_frames() == 44100 * 20 && data->stream->rate == 48000.0f);
    const float was = dsp::device_rate.load();
    dsp::device_rate.store(48000);
    StreamPager pager;
    SamplerEngine::Table table {};
    table[0] = data.get();
    auto hold = [&](uint32_t own, bool back) { // as SampleLibrary::holds_service holds a range
        uint32_t from = own, to = own + stream_chunk;
        if (back) { to = own; from = own > stream_chunk ? own - stream_chunk : 0; }
        for (uint32_t c = from / stream_chunk; c <= (to - 1) / stream_chunk; ++c) CHECK(pager.hold_chunk(*data->stream, c));
    };
    auto play = [&](const PadSound& pad, unsigned blocks) {
        SamplerEngine engine;
        engine.set_samples(&table);
        engine.set_pager(&pager);
        engine.set_tempo(1200);
        engine.set_pad(0, pad);
        engine.note_on(int(sampler_first_note), 127);
        std::vector<float> out, l(128), r(128);
        for (unsigned b = 0; b < blocks; ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            engine.render_stereo(l.data(), r.data(), 128);
            out.insert(out.end(), l.begin(), l.end());
            while (pager.pump()) {}
        }
        return out;
    };
    // What a voice plays, against where it reads (its own frames, a frame each device frame here): the same sound
    // scaled by the pad's level and pan, from the first block.
    auto matches = [&](const std::vector<float>& out, const std::function<double(unsigned)>& at, unsigned from) {
        float ratio = 0;
        for (unsigned f = from; f < out.size() && !ratio; ++f) if (std::fabs(value(uint32_t(at(f)))) > 0.2f) ratio = out[f] / value(uint32_t(at(f)));
        if (ratio < 0.1f) return false;
        for (unsigned f = from; f < out.size(); ++f) if (std::fabs(out[f] - value(uint32_t(at(f))) * ratio) > 1e-4f) return false;
        return true;
    };
    const unsigned under = pager.underruns();
    { // forward from a held start, 2.7 s: well past the held chunk
        PadSound pad; pad.sample = 0; pad.start = 441000; // 10 s in
        const uint32_t start = data->own(pad.start);
        CHECK(start == 480000);
        hold(start, false);
        const auto out = play(pad, 1000);
        CHECK(matches(out, [&](unsigned f) { return double(start) + f; }, 200)); // past its 2 ms fade in
        CHECK(std::fabs(out[300]) > 0 || std::fabs(out[301]) > 0);
    }
    { // reversed from a held end
        PadSound pad; pad.sample = 0; pad.start = 441000; pad.end = 661500; pad.reverse = true;
        const uint32_t end = data->own(pad.end);
        hold(end, true);
        const auto out = play(pad, 500);
        CHECK(matches(out, [&](unsigned f) { return double(end - 1) - f; }, 200));
    }
    { // round a one second loop, its loop point held: on from the loop point, no gap
        PadSound pad; pad.sample = 0; pad.start = 441000; pad.end = 494100; pad.loop = true; pad.loop_lock = false; pad.loop_start = 450000;
        const uint32_t start = data->own(pad.start), end = data->own(pad.end), point = data->own(pad.loop_start);
        hold(start, false); hold(point, false);
        const auto out = play(pad, 940); // 2.5 s: round twice
        CHECK(matches(out, [&](unsigned f) {
            double at = double(start) + f;
            while (at >= end) at -= double(end - point);
            return at;
        }, 200));
    }
    { // Warp (Tones) and CLOUD on a long sample play as on the same sample in memory, to the frame
        SampleData mem;
        CHECK(read_wav(path, mem, 60).empty() && mem.frames == frames && mem.rate == rate);
        SamplerEngine::Table mem_table {};
        mem_table[0] = &mem;
        auto render = [&](const SamplerEngine::Table& t, const PadSound& pad, bool streamed) {
            SamplerEngine engine;
            engine.set_samples(&t);
            if (streamed) engine.set_pager(&pager);
            engine.set_tempo(1200);
            engine.set_pad(0, pad);
            engine.note_on(int(sampler_first_note), 127);
            std::vector<float> out, l(128), r(128);
            for (unsigned b = 0; b < 600; ++b) {
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                engine.render_stereo(l.data(), r.data(), 128);
                out.insert(out.end(), l.begin(), l.end());
                out.insert(out.end(), r.begin(), r.end());
                while (pager.pump()) {}
            }
            return out;
        };
        PadSound warp; warp.sample = 0; warp.start = 441000; warp.end = 661500; warp.warp = 150;
        hold(data->own(warp.start), false);
        const auto streamed_warp = render(table, warp, true), mem_warp = render(mem_table, warp, false);
        CHECK(streamed_warp == mem_warp && peak_of(mem_warp) > 0.1f);
        PadSound cloud; cloud.sample = 0; cloud.start = 441000; cloud.end = 661500; cloud.cloud = true;
        for (uint32_t c = data->own(cloud.start) / stream_chunk; c <= (data->own(cloud.end) - 1) / stream_chunk; ++c) CHECK(pager.hold_chunk(*data->stream, c));
        const auto streamed_cloud = render(table, cloud, true), mem_cloud = render(mem_table, cloud, false);
        CHECK(streamed_cloud == mem_cloud && peak_of(mem_cloud) > 0.05f);
    }
    CHECK(pager.underruns() == under);
    { // The card late for a while: the voice plays what was read ahead, is silent, and comes back in time, where it
      // would have been by then, once the pager reads again (SamplerEngine::pass()).
        PadSound pad; pad.sample = 0; pad.start = 88200; // 2 s in
        const uint32_t start = data->own(pad.start);
        hold(start, false);
        SamplerEngine engine;
        engine.set_samples(&table);
        engine.set_pager(&pager);
        engine.set_tempo(1200);
        engine.set_pad(0, pad);
        engine.note_on(int(sampler_first_note), 127);
        std::vector<float> out, l(128), r(128);
        for (unsigned b = 0; b < 1000; ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            engine.render_stereo(l.data(), r.data(), 128);
            out.insert(out.end(), l.begin(), l.end());
            if (b < 5 || b >= 700) while (pager.pump()) {} // no reads from block 5 to block 700
        }
        CHECK(pager.underruns() == under + 1 && engine.sounding());
        float early = 0, gap = 0;
        for (unsigned f = 200; f < 128 * 100; ++f) early = std::max(early, std::fabs(out[f]));
        for (unsigned f = 128 * 600; f < 128 * 690; ++f) gap = std::max(gap, std::fabs(out[f]));
        CHECK(early > 0.1f && gap == 0.0f);
        CHECK(matches(out, [&](unsigned f) { return double(start) + f; }, 128 * 730)); // in time: not where it stopped
    }
    dsp::device_rate.store(was);
    std::remove(path.c_str());

    // Where a project's pads on a long sample need holding: each start (a reversed one's end, below it), a loop point
    // with its seam's crossfade before it and the frames past its end, a layer's start, Chop's slices; a sample in
    // memory needs none.
    auto p = std::make_unique<Project>();
    load_sound(p->tracks[0], EngineKind::Sampler, 0);
    auto& ref = p->samples.edit(3);
    std::snprintf(ref.path, sizeof(SampleRef::path), "/card/Samples/Long.wav");
    ref.frames = 882000; ref.streamed = true;
    std::snprintf(p->samples.edit(4).path, sizeof(SampleRef::path), "/card/Samples/Hit.wav");
    p->samples.edit(4).frames = 44100;
    auto& kit = p->tracks[0];
    kit.pads[0].sample = 3; kit.pads[0].start = 1000;
    kit.pads[1].sample = 3; kit.pads[1].start = 2000; kit.pads[1].end = 50000; kit.pads[1].reverse = true;
    kit.pads[2].sample = 3; kit.pads[2].start = 3000; kit.pads[2].loop = true; kit.pads[2].loop_lock = false; kit.pads[2].loop_start = 9000;
    kit.pads[2].loop_xfade = 10; // 441 frames
    kit.pads[3].sample = 4;
    kit.pads[3].layers[0].sample = 3; kit.pads[3].layers[0].start = 7000;
    kit.pads[4].sample = 3; kit.pads[4].start = 1000; // the same start as pad 1: held once
    kit.pads[5].sample = 3; kit.pads[5].start = 100000; kit.pads[5].cloud = true; // a cloud: its region, up to 30 s of it
    auto holds = stream_holds(*p);
    const std::vector<HeldRange> want {{1000, 1000, false}, {3000, 3000, false}, {7000, 7000, false}, {8559, 9000, false},
                                       {50000, 50000, true}, {100000, 882000, false}, {882000, 882441, false}}; // the cloud: to its end, short of 30 s
    CHECK(holds.size() == 1 && holds[0].first == 3 && holds[0].second == want);
    Slices slices;
    slices.whole(10000, 20000);
    CHECK(slices.add(15000, 882000));
    holds = stream_holds(*p, &slices, 3);
    CHECK(holds.size() == 1 && holds[0].second.size() == want.size() + 2 && holds[0].second[4] == (HeldRange {10000, 10000, false}));
    CHECK(stream_holds(*p, &slices, 4)[0].second == want); // Chop on a sample in memory: nothing more
    ref.streamed = false;
    CHECK(stream_holds(*p).empty());

    // A zoomed view of a long sample is held while it is narrow enough to draw from its frames; wider, it is not.
    ref.streamed = true;
    holds = stream_holds(*p, nullptr, no_sample, 3, HeldRange {100000, 100000 + view_most, false});
    CHECK(holds[0].second.size() == want.size() + 1);
    CHECK(stream_holds(*p, nullptr, no_sample, 3, HeldRange {100000, 100001 + view_most, false})[0].second == want);
    CHECK(stream_holds(*p, nullptr, no_sample, 4, HeldRange {0, 1000, false})[0].second == want); // a sample in memory
    ref.streamed = false;

    // Edits of a long sample read its file: WavReader from any frame on, a few frames straight from the file (zero
    // snap), and a region copied into a WAV of as many bits with no dither (the same values).
    {
        const std::string wav16 = "/tmp/melodee-long16-" + std::to_string(::getpid()) + ".wav";
        const unsigned n = 50000;
        {
            std::ofstream f(wav16, std::ios::binary);
            f.write("RIFF", 4); put32(f, 36 + n * 4); f.write("WAVEfmt ", 8); put32(f, 16); put16(f, 1); put16(f, 2);
            put32(f, 48000); put32(f, 48000 * 4); put16(f, 4); put16(f, 16); f.write("data", 4); put32(f, n * 4);
            for (unsigned k = 0; k < n; ++k) { put16(f, uint16_t(int16_t(k % 30000))); put16(f, uint16_t(int16_t(-int(k % 20000)))); }
        }
        WavReader reader;
        CHECK(reader.open(wav16, 60).empty() && reader.seek(40000));
        std::vector<float> l(100), r(100);
        CHECK(reader.read(l.data(), r.data(), 100) == 100 && l[0] == float(int16_t(40000 % 30000)) / 32768.0f && r[99] == float(int16_t(-int(40099 % 20000))) / 32768.0f);
        CHECK(!reader.seek(n + 1));
        ZonePart part;
        CHECK(reader.open(wav16, 60).empty() && read_in_place(wav16, reader.layout(), part));
        std::vector<float> fl(10), fr(10);
        CHECK(read_frames(part, 12345, 10, fl.data(), fr.data()) && fl[3] == float(int16_t(12348)) / 32768.0f && fr[9] == float(int16_t(-12354)) / 32768.0f);
        CHECK(read_frames(part, n - 4, 10, fl.data(), fr.data()) && !read_frames(part, n, 1, fl.data(), fr.data())); // cut at its end
        // A region copied undithered at 16 bits: the same values.
        const std::string copy = wav16 + ".copy.wav";
        WavStream out;
        out.set_dither(false);
        CHECK(out.open(copy, 48000, 16, 0, 2));
        std::vector<float> both(2 * 1000);
        CHECK(reader.seek(20000) && reader.read(l.data(), r.data(), 1) == 1);
        CHECK(reader.seek(20000));
        std::vector<float> rl(1000), rr(1000);
        CHECK(reader.read(rl.data(), rr.data(), 1000) == 1000);
        for (unsigned k = 0; k < 1000; ++k) { both[2 * k] = rl[k]; both[2 * k + 1] = rr[k]; }
        CHECK(out.add(both.data(), 1000) && out.close());
        SampleData back;
        CHECK(read_wav(copy, back).empty() && back.frames == 1000 && back.rate == 48000);
        for (unsigned k = 0; k < 1000; ++k) CHECK(back.left[k] == rl[k] && back.right[k] == rr[k]);
        std::remove(copy.c_str());
        std::remove(wav16.c_str());
    }

    // Hits found in a given sum, as in the sample itself.
    SampleData clicks;
    clicks.frames = 44100 * 2; clicks.left.assign(clicks.frames, 0.0f); clicks.right.assign(clicks.frames, 0.0f);
    for (unsigned n = 0; n < clicks.frames; ++n) if (n % 11025 < 300) clicks.left[n] = clicks.right[n] = float(std::exp(-double(n % 11025) / 60.0) * std::sin(n * 0.3));
    std::vector<float> sum(clicks.frames);
    for (unsigned n = 0; n < clicks.frames; ++n) sum[n] = clicks.left[n] + clicks.right[n];
    std::array<Hit, max_hits> a {}, b {};
    BeatGrid ga, gb;
    const unsigned found = find_hits(clicks, a, ga);
    CHECK(found >= 8 && find_hits_in(sum, b, gb) == found);
    for (unsigned n = 0; n < found; ++n) CHECK(a[n].frame == b[n].frame && a[n].strength == b[n].strength);
}

// A long sample of a kind that is not read in place (MP3, FLAC, Ogg, AIFF) is laid out once as a PCM WAV in the card's
// cache and read from there as it plays (shell/sample_cache.h's wav_copy(), the load in shell/sample_library.cpp). The
// host tests link no decoder, so here one stands in that hands on known frames: the WAV holds them at the source's rate
// and bits, to the value; it is read in place as any long WAV, and a pad on it plays as on the same frames in memory; a
// second load finds the WAV and decodes nothing; a changed source misses it; a mono source takes one side; a source
// that ends before the length it gave ends the WAV there; 16 bits from a source that keeps none are dithered; one
// that gives nothing leaves no file; tidy() ages the WAVs as the rest.
void long_samples_copied() {
    namespace fs = std::filesystem;
    const auto base = fs::temp_directory_path() / ("melodee-copy-" + std::to_string(getpid()));
    fs::create_directories(base / "Samples");
    SampleCache cache;
    cache.set_folder((base / "Melodee/Cache").string()); // made by the first copy
    const std::string source = (base / "Samples/SET.flac").string();
    { std::ofstream(source) << "not decoded here"; }
    const unsigned rate = 48000, frames = rate * 12;
    auto left_of = [](uint32_t n) { return float(std::lround(3000000 * std::sin(n * 0.013))) / 8388608.0f; }; // on 24-bit steps
    auto right_of = [](uint32_t n) { return float(std::lround(-1500000 * std::sin(n * 0.007))) / 8388608.0f; };
    unsigned calls = 0;
    uint32_t at = 0;
    const FrameReader read = [&](float* l, float* r, uint32_t n) {
        ++calls;
        n = std::min(n, frames - at);
        for (uint32_t k = 0; k < n; ++k) { l[k] = left_of(at + k); r[k] = right_of(at + k); }
        at += n;
        return n;
    };
    WavCopy what;
    what.rate = rate; what.frames = frames; what.channels = 2; what.bits = wav_copy_bits(24, false);
    bool made = false;
    const std::string copy = cache.wav_copy(source, what, read, &made);
    CHECK(made && !copy.empty() && copy == cache.wav_of(source) && fs::path(copy).parent_path() == base / "Melodee/Cache");
    CHECK(fs::path(copy).extension() == ".wav" && copy != cache.file_of(source) && !fs::exists(copy + ".part") && at == frames);
    CHECK(fs::file_size(copy) == 44 + uint64_t(frames) * 6 && fs::file_size(copy) <= SampleCache::wav_bytes(what));

    // It is a long PCM WAV as any other: 24-bit, at the source's rate, read in place, its values the source's.
    WavReader wav;
    CHECK(wav.open(copy, stream_most_seconds).empty() && wav.frames() == frames && wav.rate() == rate);
    CHECK(wav.layout().bits == 24 && wav.layout().channels == 2 && !wav.layout().floating);
    CHECK(streams_from_card(wav.frames(), wav.rate(), true));
    auto data = std::make_unique<SampleData>();
    data->stream = std::make_shared<ZonePart>();
    CHECK(read_in_place(copy, wav.layout(), *data->stream));
    data->stream->frames.store(data->stream->total);
    data->frames = data->stream->total; data->rate = rate;
    std::vector<float> l(1000), r(1000);
    CHECK(read_frames(*data->stream, 400000, 1000, l.data(), r.data()));
    for (unsigned k = 0; k < 1000; ++k) CHECK(l[k] == left_of(400000 + k) && r[k] == right_of(400000 + k));

    // A pad on it plays what a pad on the same frames read into memory plays.
    {
        SampleData memory;
        std::vector<float> ml(frames), mr(frames);
        for (uint32_t n = 0; n < frames; ++n) { ml[n] = left_of(n); mr[n] = right_of(n); }
        adopt(memory, std::move(ml), std::move(mr), rate);
        const float was = dsp::device_rate.load();
        dsp::device_rate.store(48000);
        StreamPager pager;
        SamplerEngine::Table from_card {}, from_memory {};
        from_card[0] = data.get(); from_memory[0] = &memory;
        PadSound pad; pad.sample = 0; pad.start = 220500; // 5 s in
        const uint32_t start = data->own(pad.start);
        CHECK(start == 240000);
        for (uint32_t c = start / stream_chunk; c <= (start + stream_chunk - 1) / stream_chunk; ++c) CHECK(pager.hold_chunk(*data->stream, c));
        auto render = [&](const SamplerEngine::Table& table, bool paged) {
            SamplerEngine engine;
            engine.set_samples(&table);
            if (paged) engine.set_pager(&pager);
            engine.set_tempo(1200);
            engine.set_pad(0, pad);
            engine.note_on(int(sampler_first_note), 127);
            std::vector<float> out, bl(128), br(128);
            for (unsigned b = 0; b < 800; ++b) { // 2.1 s: well past what is held
                std::fill(bl.begin(), bl.end(), 0.0f); std::fill(br.begin(), br.end(), 0.0f);
                engine.render_stereo(bl.data(), br.data(), 128);
                out.insert(out.end(), bl.begin(), bl.end());
                out.insert(out.end(), br.begin(), br.end());
                while (pager.pump()) {}
            }
            return out;
        };
        const auto card = render(from_card, true), mem = render(from_memory, false);
        CHECK(card == mem && peak_of(mem) > 0.1f && pager.underruns() == 0);
        dsp::device_rate.store(was);
    }

    // A second load finds the WAV: nothing is decoded. A source that changed misses it.
    const unsigned before = calls;
    at = 0;
    CHECK(cache.wav_copy(source, what, read, &made) == copy && !made && calls == before && cache.wav_ready(copy));
    CHECK(!cache.wav_ready(source) && !cache.wav_ready(copy + ".gone"));
    { std::ofstream(source, std::ios::app) << ", and longer now"; }
    CHECK(cache.wav_of(source) != copy);

    // A mono source takes one side. One that ends before the length it gave (an MP3's may be a guess) ends the WAV
    // there. It keeps no bits of its own: 16, dithered, each value within a step and a half of the source's.
    const std::string talk = (base / "Samples/TALK.mp3").string();
    { std::ofstream(talk) << "nor this"; }
    auto tone = [](uint32_t n) { return 0.4f * float(std::sin(n * 0.01)); };
    const uint32_t said = 44100 * 11, held = said - 1000;
    uint32_t given = 0;
    WavCopy one;
    one.rate = 44100; one.frames = said; one.channels = 1; one.bits = wav_copy_bits(0, false); one.dither = true;
    const std::string spoken = cache.wav_copy(talk, one, [&](float* bl, float* br, uint32_t n) {
        n = std::min(n, held - given);
        for (uint32_t k = 0; k < n; ++k) bl[k] = br[k] = tone(given + k);
        given += n;
        return n;
    }, &made);
    CHECK(made && wav.open(spoken, stream_most_seconds).empty() && wav.frames() == held && wav.rate() == 44100);
    CHECK(wav.layout().channels == 1 && wav.layout().bits == 16 && fs::file_size(spoken) == 44 + uint64_t(held) * 2);
    {
        ZonePart part;
        CHECK(read_in_place(spoken, wav.layout(), part) && part.right == part.left);
        CHECK(read_frames(part, 300000, 1000, l.data(), r.data()));
        bool dithered = false;
        for (unsigned k = 0; k < 1000; ++k) {
            CHECK(std::fabs(l[k] - tone(300000 + k)) <= 1.5f / 32768.0f && r[k] == l[k]);
            dithered = dithered || l[k] != float(std::floor(double(tone(300000 + k)) * 32768.0 + 0.5)) / 32768.0f;
        }
        CHECK(dithered);
    }

    // A source that gives nothing leaves no file; with no folder (no card) there is no WAV to go to.
    const std::string empty = (base / "Samples/EMPTY.ogg").string();
    { std::ofstream(empty) << "x"; }
    CHECK(cache.wav_copy(empty, one, [](float*, float*, uint32_t) { return uint32_t(0); }, &made).empty() && !made);
    CHECK(!fs::exists(cache.wav_of(empty)) && !fs::exists(cache.wav_of(empty) + ".part"));
    SampleCache nowhere;
    CHECK(nowhere.wav_of(talk).empty() && nowhere.wav_copy(talk, one, read, &made).empty() && !made && calls == before);

    // The bits a WAV takes for its source's, and the frames one holds.
    CHECK(wav_copy_bits(16, false) == 16 && wav_copy_bits(24, false) == 24 && wav_copy_bits(20, false) == 24 && wav_copy_bits(8, false) == 16);
    CHECK(wav_copy_bits(32, false) == 32 && wav_copy_bits(32, true) == 32 && wav_copy_bits(0, false) == 16);
    CHECK(SampleCache::wav_most_frames(what) == (0xffffffffu - 4096) / 6 && SampleCache::wav_most_frames(one) == (0xffffffffu - 4096) / 2);

    // tidy(): a WAV not played for a long time goes, and a .part a stop left; one played lately stays.
    fs::last_write_time(copy, fs::file_time_type::clock::now() - std::chrono::hours(24 * (cache_unplayed_days + 1)));
    { std::ofstream(copy + ".part") << "x"; }
    cache.tidy();
    CHECK(!fs::exists(copy) && !fs::exists(copy + ".part") && fs::exists(spoken));
    data.reset();
    fs::remove_all(base);
}

// A project's samples have no count to run out of: the tables keep the slots written (model/slot_table.h), the file
// keeps the slots past the first 64 in SMPX, the engine plays them, and a load is refused only where memory cannot
// take it (shell/sample_memory.h).
void samples_unlimited() {
    // The tables: a slot never written reads empty and holds nothing; the first write into a block of 64 makes it.
    {
        SlotTable<SampleRef> table;
        CHECK(table.blocks_held() == 0 && table.span() == 0);
        CHECK(!table[0].used() && !table[sample_slots - 1].used() && !table[sample_slots + 5].used());
        CHECK(table.blocks_held() == 0); // reading made none
        const SampleRef* first = &table.edit(3);
        std::snprintf(table.edit(3).path, sizeof(SampleRef::path), "/a.wav");
        CHECK(table.blocks_held() == 1 && table.span() == 64 && table[3].used() && !table[4].used());
        std::snprintf(table.edit(5000).path, sizeof(SampleRef::path), "/b.wav");
        CHECK(table.blocks_held() == 2 && table.span() == 5056 && first == &table[3]); // a reference stays good as it grows
        unsigned used = 0, walked = 0;
        for (const auto& ref : table) { used += ref.used(); ++walked; }
        CHECK(used == 2 && walked == sample_slots);
        SlotTable<SampleRef> copy = table; // a copy holds what the table holds
        CHECK(copy.blocks_held() == 2 && std::string(copy[3].path) == "/a.wav" && std::string(copy[5000].path) == "/b.wav");
        const SampleRef* kept = &copy[3];
        table.edit(3).frames = 7;
        copy = table; // copied over again (the screen's, each frame): into the blocks it has
        CHECK(&copy[3] == kept && copy[3].frames == 7);
        copy = SlotTable<SampleRef> {};
        CHECK(copy.blocks_held() == 0 && !copy[3].used());
        // In a project a table is its blocks' pointers: what a copy of the project carries is the slots in use.
        CHECK(sizeof(SlotTable<SampleRef>) == sizeof(void*) * (sample_slots / SlotTable<SampleRef>::block_slots));
        CHECK(sample_slots >= track_count * sampler_pads * pad_layers && preview_slot == sample_slots && no_sample > preview_slot);
    }
    // 401 samples across four kits: saved, loaded.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (unsigned t = 0; t < 4; ++t) {
        load_sound(project->tracks[t], EngineKind::Sampler, 0);
        for (unsigned pad = 0; pad < 100; ++pad) {
            const unsigned slot = t * 100 + pad;
            auto& ref = project->samples.edit(slot);
            std::snprintf(ref.path, sizeof(ref.path), "/media/card/Samples/Kit %u/Hit %u.wav", t + 1, pad + 1);
            std::snprintf(ref.name, sizeof(ref.name), "HIT %u", slot);
            project->tracks[t].pads[pad].sample = uint16_t(slot);
        }
    }
    const unsigned last = sample_slots - 1;
    std::snprintf(project->samples.edit(last).path, sizeof(SampleRef::path), "/media/card/Samples/Last.wav");
    project->tracks[3].pads[127].sample = uint16_t(last);
    project->tracks[2].pads[5].layers[0].sample = 390; // a layer on a slot past the first 64
    CHECK(set_trim(project->tracks[0].zone_trims, 3, 2, 3, -20, 15));    // zone trims: a slot in the first 64,
    CHECK(set_trim(project->tracks[1].zone_trims, 7, 4, 107, 30, -40));  // one past them,
    CHECK(set_trim(project->tracks[3].zone_trims, 10, 9, 310, -5, 60));  // and one past a byte
    auto bytes = save_project(*project, *bank);
    auto bodies = [&](const char* tag) { // the file's sections of one kind
        std::vector<std::vector<uint8_t>> out;
        for (size_t at = 8; at + 8 <= bytes.size();) {
            const size_t n = size_t(bytes[at + 4]) | size_t(bytes[at + 5]) << 8 | size_t(bytes[at + 6]) << 16 | size_t(bytes[at + 7]) << 24;
            if (!std::memcmp(&bytes[at], tag, 4)) out.emplace_back(bytes.begin() + long(at + 8), bytes.begin() + long(at + 8 + n));
            at += 8 + n;
        }
        return out;
    };
    {
        // The first 64 slots in SMPL, the slot a byte, as a build that holds 64 reads them; the rest in SMPX, which
        // it skips (its pads on those slots open empty).
        const auto smpl = bodies("SMPL"), smpx = bodies("SMPX");
        CHECK(smpl.size() == legacy_sample_slots && smpx.size() == 401 - legacy_sample_slots);
        for (const auto& body : smpl) CHECK(body[0] < legacy_sample_slots);
        for (const auto& body : smpx) CHECK(unsigned(body[0] | body[1] << 8) >= legacy_sample_slots);
        CHECK(bodies("ZTRM").size() == 1 && bodies("ZTRX").size() == 2);
    }
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(bytes, *back, *back_bank).empty());
    for (unsigned t = 0; t < 4; ++t)
        for (unsigned pad = 0; pad < 100; ++pad) {
            const unsigned slot = t * 100 + pad;
            CHECK(back->tracks[t].pads[pad].sample == slot);
            CHECK(std::string(back->samples[slot].path) == project->samples[slot].path && std::string(back->samples[slot].name) == project->samples[slot].name);
        }
    CHECK(back->tracks[3].pads[127].sample == last && std::string(back->samples[last].path) == "/media/card/Samples/Last.wav");
    CHECK(back->tracks[2].pads[5].layers[0].sample == 390);
    CHECK(!back->samples[400].used() && !back->samples[last - 1].used());
    CHECK(back->samples.blocks_held() == 8 && back->chops.blocks_held() == 0); // 400 slots in seven blocks, and the last
    const ZoneTrim* trim = find_trim(back->tracks[0].zone_trims, 3, 2, 3);
    CHECK(trim && trim->tune == -20 && trim->level == 15);
    trim = find_trim(back->tracks[1].zone_trims, 7, 4, 107);
    CHECK(trim && trim->tune == 30 && trim->level == -40);
    trim = find_trim(back->tracks[3].zone_trims, 10, 9, 310);
    CHECK(trim && trim->tune == -5 && trim->level == 60);
    // A project of 64 samples is written as it always was (SMPL and ZTRM alone), and loads.
    {
        auto small = std::make_unique<Project>();
        load_sound(small->tracks[0], EngineKind::Sampler, 0);
        for (unsigned n = 0; n < legacy_sample_slots; ++n) {
            std::snprintf(small->samples.edit(n).path, sizeof(SampleRef::path), "/media/card/Samples/S%u.wav", n);
            small->tracks[0].pads[n].sample = uint16_t(n);
        }
        CHECK(set_trim(small->tracks[0].zone_trims, 63, 1, 63, 10, -10));
        bytes = save_project(*small, *bank);
        CHECK(bodies("SMPL").size() == 64 && bodies("SMPX").empty() && bodies("ZTRM").size() == 1 && bodies("ZTRX").empty());
        auto opened = std::make_unique<Project>();
        CHECK(load_project(bytes, *opened, *back_bank).empty());
        CHECK(opened->tracks[0].pads[63].sample == 63 && std::string(opened->samples[63].path) == "/media/card/Samples/S63.wav");
        trim = find_trim(opened->tracks[0].zone_trims, 63, 1, 63);
        CHECK(trim && trim->tune == 10 && trim->level == -10 && opened->samples.blocks_held() == 1);
    }
    // Played: a line of hits on a pad whose sample is in slot `slot`, the project through its file first.
    auto energy = [](unsigned slot, bool offered) {
        auto kit = std::make_unique<Project>();
        auto patterns = std::make_unique<PatternBank>();
        load_sound(kit->tracks[0], EngineKind::Sampler, 0);
        std::snprintf(kit->samples.edit(slot).path, sizeof(SampleRef::path), "/media/card/Samples/Tone.wav");
        kit->tracks[0].pads[0].sample = uint16_t(slot);
        for (unsigned s = 0; s < 16; ++s) put(patterns->at[0][0], s, uint8_t(sampler_first_note), 100);
        auto opened = std::make_unique<Project>();
        auto opened_patterns = std::make_unique<PatternBank>();
        CHECK(load_project(save_project(*kit, *patterns), *opened, *opened_patterns).empty());
        auto host = std::make_unique<EngineHost>();
        host->load(*opened, *opened_patterns);
        auto tone = tone_sample();
        if (offered) CHECK(host->offer_sample(slot, &tone));
        host->send({Command::Type::Play});
        std::vector<int32_t> out(256);
        double sum = 0;
        for (unsigned n = 0; n < 200; ++n) {
            host->render(out.data(), 128, 1);
            for (auto v : out) sum += double(v / 2147483648.0f) * (v / 2147483648.0f);
        }
        return sum;
    };
    const double first = energy(0, true);
    CHECK(first > 1 && energy(300, false) == 0); // heard, and nothing where the slot holds no sample
    for (unsigned slot : {64u, 300u, 4097u, sample_slots - 1}) CHECK(std::fabs(energy(slot, true) - first) < first * 1e-6);
    // Memory: a load must leave 256 MB free; a host does not say what is free, and is allowed.
    const uint64_t mb = uint64_t(1) << 20;
    CHECK(memory_headroom == 256 * mb && stream_seconds == 10 && stream_most_seconds == 6 * 3600);
    CHECK(memory_allows(0, 500 * mb));
    CHECK(memory_allows(memory_headroom + 10 * mb, 10 * mb) && !memory_allows(memory_headroom + 10 * mb, 10 * mb + 1));
    CHECK(!memory_allows(memory_headroom - 1, 0) && !memory_allows(100 * mb, 1) && memory_allows(memory_headroom, 0));
    // What a kit sample takes: its own frames, a second of stereo floats 352800 bytes at 44.1 kHz, 384000 at 48 kHz,
    // whatever the device's rate (no copy at another rate).
    CHECK(kit_sample_bytes(44100, 44100) == 352800);
    CHECK(kit_sample_bytes(48000, 48000) == 384000);
    CHECK(kit_sample_bytes(96000, 96000) == 768000);
    CHECK(kit_sample_bytes(uint64_t(600) * 44100, 44100) == uint64_t(600) * 352800); // all of it: no cut at 60 s
    CHECK(kit_sample_bytes(1000, 0) == 0);
    // With 1.5 GB free, one-second 44.1 kHz hits are let in until 256 MB is left: 3697 of them.
    uint64_t free = 1500 * mb;
    unsigned taken = 0;
    while (memory_allows(free, kit_sample_bytes(44100, 44100))) { free -= kit_sample_bytes(44100, 44100); ++taken; }
    CHECK(taken == (1500 * mb - memory_headroom) / 352800 && taken == 3697 && free >= memory_headroom);
}

// Roadmap P3: a 16 Levels Filter note kept by Seq Record plays at its cutoff (LVCF), the pad's own filter otherwise.
void levels_cutoff_kept() {
    CHECK(pad_note(40, 0, 0) == (40 | 1 << 16) && pad_note(40, -2, 50) == (40 | 0xfe << 8 | 51 << 16) && pad_note(40, 3, -1) == pad_note(40, 3));
    SampleData data;
    data.frames = 44100;
    data.left.resize(data.frames);
    uint32_t seed = 1;
    for (auto& x : data.left) { seed = seed * 1664525u + 1013904223u; x = float(int32_t(seed)) / 2147483648.0f * 0.5f; } // noise: every band
    data.right = data.left;
    SamplerEngine::Table table {};
    table[1] = &data;
    auto heard = [&](int key) {
        auto engine = std::make_unique<SamplerEngine>();
        engine->set_samples(&table);
        PadSound pad; pad.sample = 1; pad.velocity_sense = 0;
        engine->set_pad(0, pad);
        engine->note_on(key, 100);
        std::vector<float> l(128), r(128);
        double sum = 0;
        for (unsigned b = 0; b < 60; ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            engine->render_stereo(l.data(), r.data(), 128);
            if (b > 10) for (float x : l) sum += double(x) * x;
        }
        engine->note_off(key & 0xffff); // its note off needs no cutoff
        return std::sqrt(sum);
    };
    const int key = int(sampler_first_note);
    double open = heard(key), closed = heard(pad_note(key, 0, 5)), wide = heard(pad_note(key, 0, 100));
    CHECK(closed < open * 0.3 && wide > open * 0.6);
    // The file: LVCF by each note's place; older files play every note at its pad's own.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto& pattern = bank->at[4][2];
    Event a; a.tick = 0; a.key = uint8_t(key); a.cutoff = 31;
    Event b; b.tick = 96; b.key = uint8_t(key + 1);
    pattern.add(a); pattern.add(b);
    auto file = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(file, *back, *back_bank).empty());
    CHECK(back_bank->at[4][2].events.size() == 2 && back_bank->at[4][2].events[0].cutoff == 31 && back_bank->at[4][2].events[1].cutoff == 0);
    std::string text(file.begin(), file.end());
    file[text.find("LVCF")] = 'X';
    CHECK(load_project(file, *back, *back_bank).empty() && back_bank->at[4][2].events[0].cutoff == 0);
}

// Roadmap P4, plug-in delay compensation. A plug-in that is late by its latency: an instrument sounds a note that many
// frames after it came; an effect gives back what it was given that many frames later.
struct LatePlugin final : PluginRunner {
    bool instrument = true;
    unsigned late = 0;
    int countdown = -1;
    std::array<float, 8192> line_l {}, line_r {};
    unsigned at = 0;
    void midi(uint8_t status, uint8_t, uint8_t data2) override { if ((status & 0xf0) == 0x90 && data2) countdown = int(late); }
    void silence() override {}
    void render(float* left, float* right, unsigned frames) override {
        for (unsigned f = 0; f < frames; ++f) {
            if (instrument) {
                if (countdown == 0) { left[f] += 0.5f; right[f] += 0.5f; }
                if (countdown >= 0) --countdown;
            } else {
                line_l[at & 8191] = left[f]; line_r[at & 8191] = right[f];
                left[f] = line_l[(at - late) & 8191]; right[f] = line_r[(at - late) & 8191];
                ++at;
            }
        }
    }
    unsigned latency() const override { return late; }
};

// A chain that hears nothing and holds nothing rests, and what comes out of
// the engine is the same to the last bit as when every chain is run in every block. Every kind of effect in turn, a
// kind a chain on sixteen kits and four buses: a second of sound, a silence long enough for chains to rest, then
// the sound that wakes them.
void chains_rest() {
    std::vector<EffectKind> kinds;
    for (unsigned k = 1; k < unsigned(EffectKind::Count); ++k)
        if (EffectKind(k) != EffectKind::Plugin && effect_info(EffectKind(k)).built) kinds.push_back(EffectKind(k));
    const auto tone = tone_sample();
    const unsigned chains = track_count + bus_count;
    unsigned rested_most = 0;
    for (size_t first = 0; first < kinds.size(); first += chains) {
        auto slot_of = [&](size_t n) {
            const auto kind = kinds[(first + n) % kinds.size()];
            std::array<int16_t, 6> values {};
            for (unsigned q = 0; q < 6; ++q) values[q] = int16_t(effect_info(kind).params[q].label ? effect_info(kind).params[q].initial : 0);
            return EffectSlot {uint8_t(kind), false, values};
        };
        auto play = [&](bool rests, unsigned& rested) {
            EngineHost::chain_rests = rests;
            auto project = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            for (unsigned t = 0; t < track_count; ++t) {
                tone_kit(*project, t);
                project->tracks[t].fx.slots[0] = slot_of(t);
                project->tracks[t].mix.sends[t % bus_count] = 40;
                put(bank->at[t][0], t % 4, uint8_t(sampler_first_note + t % 4), 100);
            }
            for (unsigned b = 0; b < bus_count; ++b) project->buses[b].fx.slots[0] = slot_of(track_count + b);
            project->tempo = 1200;
            EngineHost host;
            host.load(*project, *bank);
            CHECK(host.offer_sample(0, &tone));
            host.send({Command::Type::Play});
            std::vector<int32_t> heard, out(256);
            const unsigned second = 44100 / 128;
            for (unsigned n = 0; n < 15 * second; ++n) {
                if (n == second * 3 / 2) host.send({Command::Type::Stop});
                if (n == 13 * second) { rested = host.chains_resting(); host.send({Command::Type::Play}); }
                host.render(out.data(), 128, 1);
                heard.insert(heard.end(), out.begin(), out.end());
            }
            EngineHost::chain_rests = true;
            return heard;
        };
        unsigned rested = 0, never = 0;
        const auto resting = play(true, rested);
        const auto running = play(false, never);
        rested_most = std::max(rested_most, rested);
        size_t differ = 0;
        float loudest = 0;
        for (size_t n = 0; n < std::min(resting.size(), running.size()); ++n) { differ += resting[n] != running[n]; loudest = std::max(loudest, std::fabs(resting[n] / 2147483648.0f)); }
        if (differ) std::fprintf(stderr, "chains_rest: from kind %u, %zu samples differ\n", unsigned(kinds[first]), differ);
        CHECK(resting.size() == running.size() && differ == 0 && loudest > 0.01f && never == 0);
    }
    CHECK(rested_most >= 8); // with a kind a chain, most chains of a group come to rest in the silence
}

// Roadmap P4: SYNC on the delays. An echo at a note value of the song's clock, following it when the tempo changes.
void delay_sync_p4() {
    CHECK(delay_sync_param(EffectKind::Echo) == 4 && delay_sync_param(EffectKind::DiffDelay) == 5 && delay_sync_param(EffectKind::Reverb) < 0);
    CHECK(delay_sync_quarters(0) == 0 && delay_sync_quarters(9) == 1 && std::string(delay_sync_name(7)) == "1/8D");
    double quarter = effect_clock.quarter;
    for (auto kind : {EffectKind::Echo, EffectKind::DiffDelay}) {
        DelayPool pool;
        Effect echo;
        echo.set_kind(kind, pool);
        echo.set_param(1, 0);   // no feedback: one echo
        echo.set_param(2, 100); // all wet
        if (kind == EffectKind::DiffDelay) echo.set_param(3, 0); // no smear: the echo where it lands
        echo.set_param(unsigned(delay_sync_param(kind)), 9); // 1/4
        effect_clock.quarter = 11025; // 240 BPM at 44.1 kHz frames
        auto echo_at = [&](unsigned length) {
            std::vector<float> l(length, 0.0f), r(length, 0.0f);
            l[0] = r[0] = 1;
            for (unsigned at = 0; at < length; at += 128) echo.process(l.data() + at, r.data() + at, std::min(128u, length - at));
            unsigned best = 0; float most = 0;
            for (unsigned f = 0; f < length; ++f) if (std::fabs(l[f]) > most) { most = std::fabs(l[f]); best = f; }
            return best;
        };
        unsigned first = echo_at(16384);
        CHECK(first >= 11024 && first <= 11026);
        effect_clock.quarter = 5512.5; // the tempo doubled: the echo glides to an eighth of a second
        echo_at(44100);
        echo.clear();
        while (echo.clearing()) { std::array<float, 128> l {}, r {}; echo.process(l.data(), r.data(), 128); } // its line cleared a slice a block
        unsigned second = echo_at(16384);
        CHECK(second >= 5511 && second <= 5514);
        echo.set_param(unsigned(delay_sync_param(kind)), 0); // Off: TIME again
        CHECK(echo_at(32768) != second);
    }
    effect_clock.quarter = quarter;
}

// A project's load (every chain's effects set again) or a new kind never clears a 1.1 MB delay block on the audio
// thread at once (about 0.2 ms each on the MPC: twenty of them a dropout): a block never written since it was cleared
// is taken first, and one that was written is cleared a slice a block, the effect sounding as with an empty line.
void delay_pool_clears() {
    auto pool = std::make_unique<DelayPool>();
    Effect a, b;
    a.set_kind(EffectKind::Echo, *pool);
    CHECK(!a.clearing() && pool->free_blocks() == DelayPool::blocks - 1);
    a.set_param(2, 100); // all wet
    std::array<float, 128> l {}, r {};
    l[0] = r[0] = 1;
    a.process(l.data(), r.data(), 128);
    for (unsigned n = 0; n < 400; ++n) { l.fill(0); r.fill(0); a.process(l.data(), r.data(), 128); } // its line written
    a.set_kind(EffectKind::Echo, *pool); // the same block, cleared a slice at a time
    CHECK(a.clearing());
    unsigned calls = 0;
    float most = 0;
    while (a.clearing() && calls < 100) {
        l.fill(calls ? 0.0f : 0.5f); r.fill(calls ? 0.0f : 0.5f); // a sound while it clears: its dry share alone
        a.process(l.data(), r.data(), 128);
        ++calls;
        for (float x : l) most = std::max(most, std::fabs(x));
    }
    CHECK(calls <= (DelayPool::block_floats + 16383) / 16384 && most < 0.5f * 0.75f); // the dry share only (MIX 30 % after set_kind)
    for (unsigned n = 0; n < 2000; ++n) { // nothing of the old echoes comes back
        l.fill(0); r.fill(0);
        a.process(l.data(), r.data(), 128);
        for (float x : l) CHECK(std::fabs(x) < 1e-6f);
    }
    // Given back, a written block is taken last; when it is the only one left it is cleared before it sounds.
    a.set_kind(EffectKind::None, *pool);
    b.set_kind(EffectKind::Echo, *pool);
    CHECK(!b.clearing());
    std::vector<std::unique_ptr<Effect>> rest;
    while (pool->free_blocks() > 1) { rest.push_back(std::make_unique<Effect>()); rest.back()->set_kind(EffectKind::Reverb, *pool); CHECK(!rest.back()->clearing()); }
    Effect last;
    last.set_kind(EffectKind::Echo, *pool);
    CHECK(last.clearing() && pool->free_blocks() == 0);
    // A project's load sets each effect's values at once, worked out once: it sounds as set one at a time.
    auto twice = std::make_unique<DelayPool>();
    for (auto kind : {EffectKind::Compressor, EffectKind::Reverb, EffectKind::Limiter, EffectKind::Formant, EffectKind::DiffDelay}) {
        Effect one, all;
        const std::array<int16_t, effect_params> values {-20, 6, 3, 40, 60, 80};
        one.set_kind(kind, *twice);
        for (unsigned q = 0; q < effect_params; ++q) one.set_param(q, values[q]);
        all.set_kind(kind, *twice, false);
        all.set_params(values);
        for (unsigned n = 0; n < 20; ++n) {
            std::array<float, 128> l1 {}, r1 {}, l2 {}, r2 {};
            for (unsigned f = 0; f < 128; ++f) l1[f] = r1[f] = l2[f] = r2[f] = 0.5f * float(std::sin((n * 128 + f) * 0.031));
            one.process(l1.data(), r1.data(), 128); all.process(l2.data(), r2.data(), 128);
            CHECK(l1 == l2 && r1 == r2);
        }
        one.set_kind(EffectKind::None, *twice); all.set_kind(EffectKind::None, *twice);
    }
}

// Roadmap P4: modulators (an LFO, free or on the clock, an envelope follower) on any lane, and macros.
void modulators_p4() {
    // Shapes and the swing around a lane.
    CHECK(std::fabs(lfo_value(LfoShape::Sine, 0.25) - 1) < 1e-5f && std::fabs(lfo_value(LfoShape::Triangle, 0.75) + 1) < 1e-5f);
    CHECK(lfo_value(LfoShape::Square, 0.2) == 1 && lfo_value(LfoShape::SawUp, 0) == -1 && lfo_value(LfoShape::Hold, 0.3, 0.1f, -0.4f) == -0.4f);
    ModTarget g; g.track = 0; g.param = uint8_t(motion_level); g.low = 0; g.high = 127; g.depth = 50;
    CHECK(modulated(g, 100, 1, false) == 127 && modulated(g, 64, -1, false) == 32 && modulated(g, 64, 1, true) == 127);
    CHECK(Macro::at(MacroTarget {0, 0, 0, 20, 100}, 127) == 100 && Macro::at(MacroTarget {0, 0, 0, 100, 20}, 0) == 100);
    // The file: MODS and MACR.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        auto& m = project->modulators[2];
        m.kind = ModKind::Lfo; m.shape = LfoShape::Drift; m.sync = true; m.division = 6; m.rate = 33;
        m.targets[1] = ModTarget {4, uint8_t(motion_send(2)), 0, -40, 0, 100, 0};
        auto& f = project->modulators[5];
        f.kind = ModKind::Follower; f.source = 18; f.attack = 5; f.release = 70;
        f.targets[0] = ModTarget {1, uint8_t(motion_pad_param(PadParam::Pitch)), 9, 30, -48, 48, 0};
        auto& macro = project->macros[3];
        macro.value = 99;
        macro.targets[0] = MacroTarget {2, uint8_t(motion_level), 0, 40, 120};
        macro.targets[5] = MacroTarget {7, uint8_t(motion_master_fx(1, 2)), 0, 100, 0};
        auto data = save_project(*project, *bank);
        auto copy = std::make_unique<Project>();
        auto copy_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *copy, *copy_bank).empty());
        CHECK(copy->modulators[2] == m && copy->modulators[5] == f && copy->modulators[0].kind == ModKind::Off);
        CHECK(copy->macros[3].value == 99 && copy->macros[3].targets[0] == macro.targets[0] && copy->macros[3].target_count() == 2);
        CHECK(lane_own(*project, 4, motion_send(2), 0) == 0 && lane_own(*project, 2, motion_level, 0) == 100);
    }
    // The host: a held tone, its level swung by an LFO, then ducked by a follower of another track.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (unsigned t = 0; t < 2; ++t) { project->tracks[t].engine = EngineKind::Plugin; project->tracks[t].plugin = uint8_t(t); }
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    FakePlugin tone, key;
    CHECK(host->offer_plugin(0, &tone) && host->offer_plugin(1, &key));
    std::vector<int32_t> out(256);
    std::vector<float> stems(size_t(track_count) * EngineHost::block);
    auto levels = [&](unsigned blocks, float& low, float& high) {
        low = 1e9f; high = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            host->render(out.data(), 128, 1, nullptr, stems.data());
            float most = 0;
            for (unsigned f = 0; f < 128; ++f) most = std::max(most, std::fabs(stems[f]));
            low = std::min(low, most); high = std::max(high, most);
        }
    };
    host->send({Command::Type::NoteOn, 0, 0, 60, 100});
    float low = 0, high = 0;
    levels(20, low, high);
    float steady = high;
    CHECK(steady > 0.1f && low > steady * 0.95f);
    Modulator lfo;
    lfo.kind = ModKind::Lfo; lfo.shape = LfoShape::Sine; lfo.rate = 70; // about 2.5 Hz
    lfo.targets[0] = ModTarget {0, uint8_t(motion_level), 0, 100, 0, 127, 100};
    host->send(pack_mod_target(0, 0, lfo.targets[0]));
    host->send(pack_modulator(0, lfo));
    levels(400, low, high); // over a second: the level swings
    CHECK(low < steady * 0.3f && high > steady * 0.9f && std::fabs(host->modulator_value(0)) <= 1);
    // Its lane's own value moves while it swings: the swing follows it.
    host->send({Command::Type::Level, 0, 0, 0, 60}); // around 60 now: up to 123 (half the range each way)
    levels(400, low, high);
    CHECK(high > steady * 1.3f && low < steady * 0.05f);
    // Off: the lane where it was set (60).
    lfo.kind = ModKind::Off;
    host->send(pack_modulator(0, lfo));
    levels(20, low, high); // eased there
    levels(10, low, high);
    CHECK(std::fabs(high - low) < 1e-3f && high < steady * 0.6f && high > steady * 0.2f);
    // A follower of track 2 pulls track 1's level down while it plays.
    host->send({Command::Type::Level, 0, 0, 0, 100});
    Modulator follower;
    follower.kind = ModKind::Follower; follower.source = 1; follower.attack = 0; follower.release = 20;
    follower.targets[0] = ModTarget {0, uint8_t(motion_level), 0, -100, 0, 127, 100};
    host->send(pack_modulator(1, follower));
    host->send(pack_mod_target(1, 0, follower.targets[0]));
    levels(20, low, high);
    CHECK(high > steady * 0.9f);
    host->send({Command::Type::NoteOn, 1, 0, 60, 100});
    levels(10, low, high); // the follower rises, the level eases down
    levels(20, low, high);
    CHECK(high < steady * 0.2f && host->modulator_value(1) > 0.4f);
    host->send({Command::Type::NoteOff, 1, 0, 60, 0});
    levels(200, low, high);
    CHECK(high > steady * 0.9f);
    CHECK(host->offer_plugin(0, nullptr) && host->offer_plugin(1, nullptr));
    host->render(out.data(), 128, 1);
}

// The sequencer's clock at 960 a quarter, as the MPC counts. A file without TPQN counted 96: each tick in it comes
// back ten times as many (notes, punches, lanes, Motion and locks, the groove, the Looper's loops). Values past what
// the older fields hold (a note longer than 17 bars, a long lock, a groove offset past a byte, a long loop) come
// back whole from ELEN, MHLD, GRVW and LOOP's longer ticks.
void clock_960() {
    static_assert(ticks_per_quarter == 960 && sixteenth_ticks == 240, "the MPC's clock");
    CHECK(division_ticks(Division::Quarter) == 960 && division_ticks(Division::SixteenthTriplet) == 160 &&
          division_ticks(Division::SixtyFourth) == 60 && division_ticks(Division::ThirtySecondTriplet) == 80);
    CHECK(note_ticks(0) == 1 && note_ticks(70000) == 70000 && note_ticks(uint64_t(1) << 40) == max_pattern_ticks);
    CHECK(arp_rate_sequencer_ticks(arp_default_rate) == sixteenth_ticks && arp_rate_sequencer_ticks(9) == 40); // 1/16, 1/64T
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto& p = bank->at[2][7];
    // Written on the old clock: a bar of 384, a note on 96 for 48, a punch, a lane, a lock, a groove; a loop of a bar.
    p.length = 384;
    p.add({96, 48, 60, 100, 100, 0});
    p.punches = {{24, 72, 8, 255, 100, 2, 1}};
    p.set_lane_length(40, 192);
    MotionPoint lock; lock.tick = 12; lock.param = 3; lock.value = 50; lock.hold = 6;
    p.write_motion(lock);
    p.feel.groove_step = 24; p.feel.groove_offsets = {3, -5}; p.feel.groove_levels = {100, 80};
    project->looper.on = true;
    project->looper.loops[1] = LoopTrack {1, 100, looper::source_input, 88200, 384.5, 1200, 1, false};
    auto bytes = save_project(*project, *bank);
    const uint8_t tag[] {'T', 'P', 'Q', 'N'};
    auto at = std::search(bytes.begin(), bytes.end(), std::begin(tag), std::end(tag));
    CHECK(at != bytes.end() && at[8] == (960 & 0xff) && at[9] == 960 >> 8);
    bytes.erase(at, at + 10); // a file from before: no TPQN
    auto old = std::make_unique<Project>();
    auto old_bank = std::make_unique<PatternBank>();
    CHECK(load_project(bytes, *old, *old_bank).empty());
    const auto& q = old_bank->at[2][7];
    CHECK(q.length == 3840 && q.events.size() == 1 && q.events[0].tick == 960 && q.events[0].length == 480);
    CHECK(q.punches.size() == 1 && q.punches[0].tick == 240 && q.punches[0].length == 720);
    CHECK(q.lane_length(40) == 1920 && q.motion.size() == 1 && q.motion[0].tick == 120 && q.motion[0].hold == 60);
    CHECK(q.feel.groove_step == 240 && q.feel.groove_offsets == std::vector<int16_t>({30, -50}) && q.feel.groove_levels[1] == 80);
    CHECK(std::fabs(old->looper.loops[1].ticks - 3845) < 1e-6 && old->looper.loops[1].frames == 88200);

    // On the new clock: long values whole, and a file read back as written (TPQN 960, nothing scaled).
    p.length = 40 * 3840;
    p.events.clear();
    p.add({0, 30 * 3840, 60, 100, 100, 0});  // 30 bars
    p.add({3840, 240, 62, 100, 100, 0});
    p.motion.clear();
    lock.tick = 0; lock.hold = 20 * 3840;
    p.write_motion(lock);
    lock.tick = 3840; lock.param = 4; lock.hold = 240;
    p.write_motion(lock);
    p.feel.groove_step = 1920; p.feel.groove_offsets = {700, -900}; // half a step of a half note: past a byte
    project->looper.loops[1].ticks = 64 * 8640.25;                    // 64 bars of 9/4: past 65535
    bytes = save_project(*project, *bank);
    for (const char* name : {"ELEN", "MHLD", "GRVW"}) CHECK(std::search(bytes.begin(), bytes.end(), name, name + 4) != bytes.end());
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(bytes, *back, *back_bank).empty());
    CHECK(back_bank->at[2][7] == p);
    CHECK(back_bank->at[2][7].events[0].length == 30 * 3840 && back_bank->at[2][7].motion[0].hold == 20 * 3840);
    CHECK(std::fabs(back->looper.loops[1].ticks - 64 * 8640.25) < 1e-2);
    project->looper.loops[1].ticks = 1234.567891; // a free take closed off the bar: as fine as the first field holds
    bytes = save_project(*project, *bank);
    CHECK(load_project(bytes, *back, *back_bank).empty() && std::fabs(back->looper.loops[1].ticks - 1234.567891) < 2e-5);
    // Nothing long: none of the three.
    p.events = {{0, 240, 60, 100, 100, 0}}; p.motion.clear(); p.feel = Feel {};
    bytes = save_project(*project, *bank);
    for (const char* name : {"ELEN", "MHLD", "GRVW"}) CHECK(std::search(bytes.begin(), bytes.end(), name, name + 4) == bytes.end());

    // Humanize draws for a note on the old clock's ticks what it drew there (seeded by tick / 10): tick 96 then, 960
    // now, moved by the same share of a quarter of a step.
    auto old_draw = [](uint32_t tick, unsigned key, unsigned salt) { // pattern.cpp's humanize_draw before 960
        uint32_t h = tick * 2654435761u ^ (uint32_t(key) << 16) ^ salt * 40503u;
        auto next = [&] { h ^= h << 13; h ^= h >> 17; h ^= h << 5; return double(h % 10001) / 10000.0; };
        return next() + next() - 1.0;
    };
    Pattern human;
    human.length = 3840; human.feel.humanize = 100;
    human.add({960, 240, 60, 100, 100, 0});
    auto felt = feel_pattern(human);
    CHECK(felt.events.size() == 1 && felt.events[0].tick == uint32_t(std::floor(960 + old_draw(96, 60, 1) * 240 / 4.0 + 0.5)));
}

// Roadmap P4: exports and imports. Standard MIDI Files out and in, a mixdown in integer PCM dithered, stems beside it.
void exports_p4() {
    // A MIDI file written and read back: the notes, their channel and the tempo.
    {
        std::vector<SmfTrack> tracks(2);
        tracks[0].name = "BASS"; tracks[0].channel = 2;
        tracks[0].notes = {{0, 48, 36, 100}, {96, 24, 43, 90}, {96, 96, 48, 80}};
        tracks[1].name = "KIT"; tracks[1].channel = 10;
        tracks[1].notes = {{0, 12, 36, 127}, {48, 12, 38, 110}};
        auto bytes = write_smf(tracks, 1284, 3, 4);
        std::vector<SmfTrack> back;
        std::string why;
        unsigned tempo = 0;
        CHECK(read_smf(bytes, back, why, &tempo) && back.size() == 2 && tempo == 1284);
        CHECK(back[0].name == "BASS" && back[0].channel == 2 && back[0].notes.size() == 3 && back[0].notes[1].tick == 96 &&
              back[0].notes[1].key == 43 && back[0].notes[1].length == 24 && back[0].notes[2].length == 96 && back[0].notes[1].velocity == 90);
        CHECK(back[1].channel == 10 && back[1].notes[1].key == 38 && back[1].notes[1].tick == 48);
        // Another division, running status and a note on at velocity 0 as its off: 480 a quarter becomes 960.
        std::vector<uint8_t> other {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0,
                                    'M', 'T', 'r', 'k', 0, 0, 0, 16,
                                    0x00, 0x91, 60, 100, 0x83, 0x60, 60, 0, 0x00, 62, 90, 0x81, 0x70, 62, 0, 0x00};
        // (the last byte is padding the length covers)
        CHECK(read_smf(other, back, why) && back.size() == 1 && back[0].channel == 2 && back[0].notes.size() == 2);
        CHECK(back[0].notes[0].tick == 0 && back[0].notes[0].length == 960 && back[0].notes[1].tick == 960 && back[0].notes[1].length == 480);
        CHECK(!read_smf({1, 2, 3}, back, why) && why == "not a MIDI file");
    }
    // A project's tracks as a MIDI file (no song: the patterns playing), and one laid on a kit's pattern.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tracks[0].engine = EngineKind::Sampler; project->tracks[0].set_name("DRUMS");
        project->tracks[3].engine = EngineKind::Plugin; project->tracks[3].midi_channel = 5;
        Event kick; kick.tick = 0; kick.key = uint8_t(sampler_first_note); kick.length = 12;
        Event snare = kick; snare.tick = 96; snare.key = uint8_t(sampler_first_note + 2);
        bank->at[0][0].add(kick); bank->at[0][0].add(snare);
        Event note = kick; note.key = 64; note.length = 192;
        bank->at[3][0].add(note);
        auto tracks = smf_tracks(*project, *bank);
        CHECK(tracks.size() == 2 && tracks[0].channel == 10 && tracks[0].name == "DRUMS" && tracks[0].notes[1].key == 38 && tracks[1].channel == 5);
        Pattern laid;
        CHECK(smf_pattern(tracks, project->tracks[0], project->bar_ticks(), laid));
        CHECK(laid.length == project->bar_ticks() && laid.events.size() == 3 && laid.events[0].key == sampler_first_note); // the 64 too: pad 29
        Track empty_kit; empty_kit.engine = EngineKind::Sampler;
        std::vector<SmfTrack> high(1);
        high[0].notes = {{0, 10, 20, 100}};
        CHECK(!smf_pattern(high, empty_kit, 384, laid)); // below the kit's first pad
    }
    // A mixdown in 16-bit PCM: dithered (a step either way at most around silence), the format says 16 bits.
    {
        auto dir = std::filesystem::temp_directory_path() / "melodee-p4-dither.wav";
        WavStream wav;
        CHECK(wav.open(dir.string(), 48000, 16));
        std::vector<float> frames(2 * 4800, 0.0f);
        CHECK(wav.add(frames.data(), 4800) && wav.close() && wav.frames() == 4800);
        std::ifstream in(dir, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(bytes.size() == 44 + 4800 * 4 && bytes[20] == 1 && bytes[34] == 16 && bytes[24] == uint8_t(48000 & 0xff));
        int lowest = 0, highest = 0;
        unsigned nonzero = 0;
        for (size_t at = 44; at + 1 < bytes.size(); at += 2) {
            int v = int16_t(uint16_t(bytes[at] | bytes[at + 1] << 8));
            lowest = std::min(lowest, v); highest = std::max(highest, v); nonzero += v != 0;
        }
        CHECK(lowest >= -1 && highest <= 1 && nonzero > 1000); // TPDF: noise of a step, never more
        std::filesystem::remove(dir);
    }
    // Stems beside the mixdown: each track as it joins the mix, the same frames.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tracks[0].engine = EngineKind::Plugin; project->tracks[0].plugin = 0;
        auto& p = bank->at[0][0];
        p.length = 4 * ticks_per_quarter;
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        FakePlugin tone;
        CHECK(host->offer_plugin(0, &tone));
        auto ring = std::make_unique<StemRing>();
        ring->arm(1u << 0 | 1u << 5);
        host->set_stems(ring.get());
        // Live stems beside it, every block: what the render's stems get, the same values.
        std::vector<float> live(size_t(StemRing::stem_count) * 2 * EngineHost::block, 9.0f);
        host->set_live_stems(live.data());
        Command arm {Command::Type::BounceArm, 0, 0, 0, 0};
        arm.position = ticks_per_quarter / 2; // an eighth: a quarter of a second at 120
        host->send(arm);
        host->send({Command::Type::Play, 0});
        host->send({Command::Type::NoteOn, 0, 0, 60, 100});
        std::vector<int32_t> out(256);
        for (unsigned n = 0; n < 200; ++n) host->render(out.data(), 128, 1);
        CHECK(host->bounce().state() == Bounce::State::Done && host->bounce().taken() > 0);
        std::vector<float> got(size_t(StemRing::stem_count) * 2 * 65536);
        unsigned frames = ring->pop(got.data(), 65536);
        CHECK(frames == host->bounce().taken() && !ring->failed());
        float loud = 0, quiet = 0;
        for (unsigned f = 0; f < frames; ++f) { loud = std::max(loud, std::fabs(got[2 * f])); quiet = std::max(quiet, std::fabs(got[size_t(5) * 2 * 65536 + 2 * f])); }
        CHECK(loud > 0.1f && quiet == 0);
        // The tone holds still, so the last block's live stem is the level the ring got; track 6 silent in both.
        CHECK(std::fabs(live[0] - got[2 * (frames - 1)]) < 1e-6f && live[size_t(5) * 2 * EngineHost::block] == 0.0f);
        host->set_live_stems(nullptr);
        host->set_stems(nullptr);
        CHECK(host->offer_plugin(0, nullptr));
        host->render(out.data(), 128, 1);
    }
    // Live stems alone, no render running (the Ableton Move's USB submixes): a track as it joins the mix, the
    // tracks with no engine silent, a bus's return at its level, a muted track silent once its mute has eased in,
    // and nothing written once they are let go.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        project->tracks[0].engine = EngineKind::Plugin; project->tracks[0].plugin = 0;
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        FakePlugin tone;
        CHECK(host->offer_plugin(0, &tone));
        std::vector<float> rows(size_t(StemRing::stem_count) * 2 * EngineHost::block, 9.0f);
        host->set_live_stems(rows.data());
        host->send({Command::Type::Send, 0, 0, 0, 100});   // track 1 into bus A
        host->send({Command::Type::Return, 0, 0, 0, 100}); // bus A's return up
        host->send({Command::Type::NoteOn, 0, 0, 60, 100});
        std::vector<int32_t> out(256);
        auto play = [&](unsigned blocks) { for (unsigned n = 0; n < blocks; ++n) host->render(out.data(), 128, 1); };
        auto peak = [&](unsigned stem) {
            float most = 0;
            for (unsigned side = 0; side < 2; ++side)
                for (unsigned f = 0; f < EngineHost::block; ++f) most = std::max(most, std::fabs(rows[size_t(stem * 2 + side) * EngineHost::block + f]));
            return most;
        };
        play(40);
        CHECK(peak(0) > 0.05f && peak(track_count) > 0.01f); // track 1, bus A
        bool rest_silent = peak(track_count + 1) == 0;
        for (unsigned s = 1; s < track_count; ++s) rest_silent = rest_silent && peak(s) == 0;
        for (unsigned s = track_count + bus_count; s < StemRing::stem_count; ++s) rest_silent = rest_silent && peak(s) == 0; // the submixes
        CHECK(rest_silent);
        host->send({Command::Type::Return, 0, 0, 0, 0});
        play(40);
        CHECK(peak(track_count) == 0 && peak(0) > 0.05f);
        // The mixer: sent to submix 4, the submix's stem has it too (the track's own as it leaves for it).
        const unsigned sub4 = track_count + bus_count + 3;
        CHECK(StemRing::stem_count == track_count + bus_count + sub_count);
        host->send({Command::Type::TrackOut, 0, 0, 0, out_sub(3)});
        play(40);
        CHECK(peak(sub4) > 0.05f && std::fabs(peak(sub4) - peak(0)) < 0.01f && peak(sub4 - 1) == 0 && peak(sub4 + 1) == 0);
        host->send({Command::Type::TrackOut, 0, 0, 0, out_master});
        play(40);
        CHECK(peak(sub4) == 0 && peak(0) > 0.05f);
        host->send({Command::Type::Mute, 0, 0, 0, 1});
        play(40);
        CHECK(peak(0) == 0);
        host->set_live_stems(nullptr);
        std::fill(rows.begin(), rows.end(), 9.0f);
        play(1);
        CHECK(rows.front() == 9.0f && rows.back() == 9.0f);
        CHECK(host->offer_plugin(0, nullptr));
        play(1);
    }
}

// Roadmap P5: always-on capture. Every track's notes kept in a ring made once, Record on or not; Shift + Seq Record lays
// the phrase (or so many bars) into patterns after the fact, the same bars for every track, on the bar lines while the
// transport ran, from the first note when stopped.
void capture_p5() {
    auto ring = std::make_unique<CaptureRing>();
    const uint32_t bar = 4 * ticks_per_quarter;
    // Played while the transport ran (run 3): track 2 a kick on bars 5 and 6, track 7 a bass note in bar 6; a gap of
    // three bars before them (bar 1's note on track 2 is another phrase).
    ring->press(2, 36, 100, 0, 3, 0 * bar + 10); ring->release(2, 36, 100, 0 * bar + 30);
    ring->press(2, 36, 110, 0, 3, 4 * bar); ring->release(2, 36, 100, 4 * bar + 24);
    ring->press(7, 40 | 2 << 8, 90, 0, 3, 5 * bar + 3 * sixteenth_ticks + 2); // still held
    ring->press(2, 36, 120, 0, 3, 5 * bar + 8 * sixteenth_ticks - 3); ring->release(2, 36, 100, 5 * bar + 8 * sixteenth_ticks + 20);
    CHECK(ring->count() == 4 && ring->played(2) == 3 && ring->played(7) == 1);
    CaptureAsk ask;
    ask.bar = bar; ask.grid = sixteenth_ticks; ask.ticks = 5 * bar + 12 * sixteenth_ticks;
    auto w = capture_window(*ring, ask);
    CHECK(w.any && w.run == 3 && w.start == 4 * bar && w.length == 2 * bar); // the phrase: bars 5 and 6
    Pattern kick, bass;
    CHECK(lay_capture(*ring, 2, w, ask, kick) && kick.length == 2 * bar && kick.events.size() == 2);
    CHECK(kick.events[0].tick == 0 && kick.events[0].velocity == 110 && kick.events[0].length == sixteenth_ticks);
    CHECK(kick.events[1].tick == bar + 8 * sixteenth_ticks && kick.events[1].velocity == 120); // onto its sixteenth
    CHECK(lay_capture(*ring, 7, w, ask, bass) && bass.events.size() == 1 && bass.events[0].tick == bar + 3 * sixteenth_ticks &&
          bass.events[0].key == 40 && bass.events[0].pitch == 2 && bass.events[0].length == 9 * sixteenth_ticks); // held until now
    Pattern none;
    CHECK(!lay_capture(*ring, 5, w, ask, none));
    { // Where it goes: the pattern playing when it is empty, else the next empty one, round to the first; none: -1.
        auto bank = std::make_unique<PatternBank>();
        auto& own = bank->at[2];
        CHECK(capture_into(own, 0) == 0 && capture_into(own, 5) == 5);
        own[5].add(kick.events[0]);
        CHECK(capture_into(own, 5) == 6);
        for (unsigned n = 6; n < pattern_count; ++n) own[n].add(kick.events[0]);
        CHECK(capture_into(own, 5) == 0);
        for (unsigned n = 0; n < 5; ++n) own[n].add(kick.events[0]);
        CHECK(capture_into(own, 5) == -1);
    }
    ask.grid = 1; // as played
    CHECK(lay_capture(*ring, 2, w, ask, kick) && kick.events[1].tick == bar + 8 * sixteenth_ticks - 3 && kick.events[1].length == 23);
    ask.bars = 4; // four bars back from the bar after the last note: bars 3-6, bar 1's note still out
    w = capture_window(*ring, ask);
    CHECK(w.start == 2 * bar && w.length == 4 * bar && lay_capture(*ring, 2, w, ask, kick) && kick.events.size() == 2 && kick.events[0].tick == 2 * bar);
    ask.bars = 8; // reaching past the transport's start: from it
    w = capture_window(*ring, ask);
    CHECK(w.start == 0 && w.length == 8 * bar && lay_capture(*ring, 2, w, ask, kick) && kick.events.size() == 3 && kick.events[0].tick == 10);
    ask.tracks = 1u << 2; // only track 2 looked at: its own phrase
    ask.bars = capture_auto;
    w = capture_window(*ring, ask);
    CHECK(w.start == 4 * bar && w.length == 2 * bar);
    // Played stopped (run 0), at 120 BPM: the phrase's first note is its downbeat; a gap of more than two bars ends it.
    auto loose = std::make_unique<CaptureRing>();
    CaptureAsk still;
    still.bar = bar; still.grid = sixteenth_ticks; still.tick_ms = 60000.0 / 120 / ticks_per_quarter; // a bar is 2000 ms
    loose->press(4, 60, 100, 1000, 0, 0); loose->release(4, 60, 1100, 0);       // long before: not this phrase
    loose->press(4, 62, 100, 9000, 0, 0); loose->release(4, 62, 9240, 0);       // the downbeat
    loose->press(4, 64, 80, 9500 + 3, 0, 0); loose->release(4, 64, 9750, 0);    // a beat later, 3 ms late
    loose->press(4, 67, 90, 12100, 0, 0); loose->release(4, 67, 12300, 0);      // in the second bar
    still.now = 14000;
    auto sw = capture_window(*loose, still);
    CHECK(sw.any && sw.run == 0 && sw.start == 9000 && sw.length == 2 * bar);
    Pattern melody;
    CHECK(lay_capture(*loose, 4, sw, still, melody) && melody.events.size() == 3 && melody.events[0].tick == 0 &&
          melody.events[1].tick == ticks_per_quarter && melody.events[2].tick == bar + 9 * sixteenth_ticks); // 3100 ms in: 595 ticks, the nearest sixteenth
    CHECK(melody.events[0].length == 2 * sixteenth_ticks && melody.events[1].length == 2 * sixteenth_ticks);
    still.bars = 1; // a bar back from where the last note ended: the second bar's note alone, its own downbeat
    sw = capture_window(*loose, still);
    CHECK(sw.start == 12100 && sw.length == bar && lay_capture(*loose, 4, sw, still, melody) && melody.events.size() == 1 && melody.events[0].tick == 0);
    still.bars = 2; still.now = 30000; // asked long after: still the last two bars played, not two bars back from now
    sw = capture_window(*loose, still);
    CHECK(sw.start == 9000 && sw.length == 2 * bar && lay_capture(*loose, 4, sw, still, melody) && melody.events.size() == 3);
    // The ring goes round: minutes of notes, the oldest let go, nothing made as it plays.
    auto full = std::make_unique<CaptureRing>();
    for (unsigned n = 0; n < capture_size + 100; ++n) full->press(n % 3, 60, 100, n, 1, n * 24);
    CHECK(full->count() == capture_size && full->newest(0).at == capture_size + 99 && full->newest(capture_size - 1).at == 100);
    static_assert(sizeof(CaptureRing) < 256 * 1024, "the ring stays small");
}

// A pad's values reach what it plays now, gliding there without a step: a knob (a Pad command), Motion, the kit's
// values, the tempo, an instrument's trim, the pad's region. A new sample on the pad leaves what plays alone.
void live_pad_values() {
    // A second of DC on the left (each frame is the voice's gain heard), a fast sine on the right (a low pass shows).
    SampleData data;
    data.frames = 44100;
    data.left.assign(44100, 0.5f); data.right.resize(44100);
    for (unsigned n = 0; n < 44100; ++n) data.right[n] = float(std::sin(n * 1.0) * 0.5);
    SampleData other = data;
    SamplerEngine::Table table {};
    table[3] = &data; table[4] = &other;
    const int key = int(sampler_first_note);
    struct Heard { float left = 0, right = 0, jump = 0; std::vector<float> l; };
    // Renders `blocks` of 128: the last block's peaks and the largest step from frame to frame on the left.
    auto run = [&](SamplerEngine& e, unsigned blocks) {
        Heard h;
        std::vector<float> l(128), r(128);
        float last = -1;
        for (unsigned b = 0; b < blocks; ++b) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            e.render_stereo(l.data(), r.data(), 128);
            h.left = h.right = 0;
            for (unsigned f = 0; f < 128; ++f) {
                CHECK(std::isfinite(l[f]) && std::isfinite(r[f]));
                if (last >= 0) h.jump = std::max(h.jump, std::fabs(l[f] - last));
                last = l[f];
                h.left = std::max(h.left, std::fabs(l[f])); h.right = std::max(h.right, std::fabs(r[f]));
                h.l.push_back(l[f]);
            }
        }
        return h;
    };
    auto tap = [](const SamplerEngine& e) { TrackTaps t; e.taps(t); return t; };
    PadSound held;
    held.sample = 3; held.play = PadPlay::Gate; held.velocity_sense = 0; held.attack = 0; held.release = 0;
    auto engine_with = [&](const PadSound& pad) {
        auto e = std::make_unique<SamplerEngine>();
        e->set_samples(&table);
        e->set_pad(0, pad);
        e->note_on(key, 127);
        return e;
    };
    { // the level turned down as it plays: down to nothing while held, gliding there
        auto e = engine_with(held);
        auto before = run(*e, 10);
        CHECK(before.left > 0.3f);
        PadSound quiet = held; quiet.level = 0;
        e->set_pad(0, quiet);
        auto after = run(*e, 30);
        CHECK(e->sounding() && after.left < 1e-3f);
        CHECK(after.jump < 0.01f); // no step: 0.35 down over 8 ms
        CHECK(after.l[64] > 0.2f && after.l[64] < before.left); // under way after 64 frames, not there yet
    }
    { // the pad's filter closed as it plays: by a knob, then by Motion; the high side goes, the DC stays
        PadSound open = held; open.filter = PadFilter::Lpf2; open.cutoff = 100;
        auto e = engine_with(open);
        auto before = run(*e, 10);
        PadSound shut = open; shut.cutoff = 25;
        e->set_pad(0, shut);
        auto after = run(*e, 30);
        CHECK(before.right > 0.2f && after.right < 0.25f * before.right);
        CHECK(std::fabs(after.left - before.left) < 0.02f && after.jump < 0.01f);
        e->move_pad(0, PadParam::FilterCutoff, 100); // Motion opens it again on the same note
        CHECK(run(*e, 30).right > 0.8f * before.right);
        e->restore_pads(); // the pattern's start: the kit's own cutoff, heard at once
        CHECK(run(*e, 30).right < 0.25f * before.right);
    }
    { // pitch: the pad's Semi, then the kit's TUNE, retune the note held
        auto e = engine_with(held);
        run(*e, 4);
        CHECK(std::fabs(tap(*e).voice[0].step - 1.0f) < 1e-4f);
        PadSound up = held; up.pitch = 12;
        e->set_pad(0, up);
        run(*e, 1);
        const float gliding = tap(*e).voice[0].step;
        CHECK(gliding > 1.0f && gliding < 2.0f); // on its way
        run(*e, 30);
        CHECK(std::fabs(tap(*e).voice[0].step - 2.0f) < 1e-3f);
        e->set_param(sampler::Tune, -12);
        run(*e, 30);
        CHECK(std::fabs(tap(*e).voice[0].step - 1.0f) < 1e-3f);
    }
    { // pan and the kit's level, heard on the note held
        auto e = engine_with(held);
        auto centre = run(*e, 10);
        PadSound left = held; left.pan = -50;
        e->set_pad(0, left);
        auto panned = run(*e, 30);
        CHECK(panned.right < 1e-3f && panned.left > 1.35f * centre.left && panned.jump < 0.01f);
        e->set_param(sampler::Level, 0);
        CHECK(run(*e, 30).left < 1e-3f && e->sounding());
    }
    { // a sustain turned while held: the envelope goes there at its decay, no step
        PadSound sus = held; sus.decay = 0; sus.sustain = 100;
        auto e = engine_with(sus);
        auto full = run(*e, 10);
        PadSound half = sus; half.sustain = 50;
        e->set_pad(0, half);
        auto after = run(*e, 30);
        CHECK(std::fabs(after.left - 0.5f * full.left) < 0.01f && after.jump < 0.01f);
    }
    { // a new sample on the pad: what plays goes on as it was, the next hit plays the new one
        auto e = engine_with(held);
        run(*e, 4);
        PadSound swapped = held; swapped.sample = 4; swapped.pitch = 12; swapped.level = 0;
        e->set_pad(0, swapped);
        auto still = run(*e, 30);
        auto t = tap(*e);
        CHECK(t.count == 1 && t.voice[0].slot == 3 && std::fabs(t.voice[0].step - 1.0f) < 1e-4f && still.left > 0.3f);
    }
    { // a one shot whose end is trimmed behind where it plays fades out at once, without a click
        PadSound shot = held; shot.play = PadPlay::OneShot;
        auto e = engine_with(shot);
        run(*e, 20);
        PadSound trimmed = shot; trimmed.end = 1000;
        e->set_pad(0, trimmed);
        auto gone = run(*e, 4);
        CHECK(!e->sounding() && gone.jump < 0.02f);
    }
    { // a loop's end moved as it loops: it goes round its new loop
        PadSound looped = held; looped.loop = true; looped.end = 4410;
        auto e = engine_with(looped);
        run(*e, 20);
        PadSound shorter = looped; shorter.end = 2205;
        e->set_pad(0, shorter);
        run(*e, 60);
        auto t = tap(*e);
        CHECK(e->sounding() && t.count == 1 && t.voice[0].to == 2205 && t.voice[0].at < 2205);
    }
    { // Follow: a tempo change speeds a note that follows it, stretched from where it is, without a dip
        PadSound follow = held; follow.own_tempo = 1200;
        auto e = std::make_unique<SamplerEngine>();
        e->set_samples(&table);
        e->set_param(sampler::Follow, 1);
        e->set_param(sampler::FollowHow, 1); // Tones: windows that sum to one
        e->set_tempo(1200);
        e->set_pad(0, follow);
        e->note_on(key, 127);
        auto before = run(*e, 10);
        CHECK(!(tap(*e).voice[0].flags & tap::Stretch));
        e->set_tempo(1800);
        auto after = run(*e, 40);
        auto t = tap(*e);
        CHECK((t.voice[0].flags & tap::Stretch) && std::fabs(t.voice[0].step - 1.5f) < 1e-3f);
        CHECK(std::fabs(after.left - before.left) < 0.02f && after.jump < 0.01f);
    }
    { // ZONES' trim on an instrument's zone, heard on the note held
        ZoneRig r;
        r.part(zone_tone(440, 0.5f, 44100));
        r.zone(0).root = 60;
        r.build(PadPlay::Gate);
        r.key(0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 440) < 8);
        r.engine->set_trim(0, 0, 3, 100, 0);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 440 * std::pow(2.0, 1 / 12.0)) < 8);
        r.pad.cutoff = 100; r.pad.pitch = 12; // the pad's Semi moves the note held an octave
        r.engine->set_pad(0, r.pad);
        CHECK(std::fabs(zone_hz(r.run(8000), 800, 8000) - 880 * std::pow(2.0, 1 / 12.0)) < 14);
    }
}

// Freeze plan F2a: prints. A print host (EngineHost::Lean) holding one track of a project plays it as the whole
// project's host plays that track, sample for sample, after its chain; the hash moves with what made the print and
// nothing else; PRNT keeps the records; the ticks fall where tick_frame() says; a plug-in's latency is taken out.
void prints_f2a() {
    const float rate = dsp::sample_rate();
    // Two kits: track 1 through an Echo and a Pumper (tempo-synced) with a send into a reverb bus, playing pattern 4;
    // track 2 its own beat. Track 1 at unity and centred, as a print plays it.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0); tone_kit(*project, 1);
    auto& t1 = project->tracks[0];
    t1.fx.slots[0] = {uint8_t(EffectKind::Echo), false, {40, 50, 50, 50, 0, 0}};
    t1.fx.slots[1] = {uint8_t(EffectKind::Pumper), false, {4, 50, 60, 20, 20, 50}};
    t1.mix.sends[0] = 40;
    t1.pattern = 3;
    project->buses[0].fx.slots[0] = {uint8_t(EffectKind::Reverb), false, {70, 50, 40, 50, 0, 0}};
    project->swing = 58;
    project->tempo = 1333;
    for (unsigned s = 0; s < 16; s += 3) put(bank->at[0][3], s, uint8_t(sampler_first_note + s % 4), uint8_t(70 + s));
    for (unsigned s = 0; s < 16; s += 2) put(bank->at[1][0], s, uint8_t(sampler_first_note + 1), 90);
    put(bank->at[0][0], 0, uint8_t(sampler_first_note), 100); // another pattern of track 1: not played
    auto tone = tone_sample();
    const uint32_t ticks = played_pattern(bank->at[0][3], t1.speed).length;

    // The whole project, track 1 taken after its chain as the Track source takes it.
    auto live = std::make_unique<EngineHost>();
    live->load(*project, *bank);
    CHECK(live->offer_sample(0, &tone));
    live->set_print_tap(0, true);
    std::vector<int32_t> out(256);
    live->send({Command::Type::Play}); // the sample taken in at the first block's start, before Play
    std::vector<float> heard_l, heard_r;
    live->render(out.data(), 128, 1);
    const uint64_t pass = live->tick_frame(ticks);
    CHECK(pass > 44100 && pass < 3 * 44100);
    for (unsigned f = 0; f < 128; ++f) { heard_l.push_back(live->print_left()[f]); heard_r.push_back(live->print_right()[f]); }
    while (heard_l.size() < 2 * pass + 256) {
        live->render(out.data(), 128, 1);
        for (unsigned f = 0; f < 128; ++f) { heard_l.push_back(live->print_left()[f]); heard_r.push_back(live->print_right()[f]); }
    }
    CHECK(peak_of(heard_l, size_t(pass), size_t(2 * pass)) > 0.05f);

    auto print = [&](PrintTail tail, PrintTap tap = PrintTap::PostFx) {
        auto p = std::make_unique<Project>(*project);
        auto b = std::make_unique<PatternBank>(*bank);
        print_project(*p, *b, 0, 3);
        CHECK(!p->tracks[1].used() && b->at[0][0].empty() && !b->at[0][3].empty() && p->tracks[0].pattern == 3);
        CHECK(p->buses[0].fx.slots[0].kind == 0 && p->tracks[0].mix.sends[0] == 0);
        auto host = std::make_unique<EngineHost>(EngineHost::Lean {2});
        CHECK(host->lean() && host->delay_blocks_free() == 2);
        host->load(*p, *b);
        CHECK(host->offer_sample(0, &tone));
        return render_print(*host, 0, ticks, tap, tail);
    };
    auto loop = print(PrintTail::Loop);
    CHECK(loop.error.empty() && loop.pass == pass && loop.latency == 0 && loop.left.size() == pass);
    uint64_t differ = 0;
    for (uint64_t f = 0; f < pass; ++f) differ += loop.left[f] != heard_l[pass + f] || loop.right[f] != heard_r[pass + f];
    CHECK(differ == 0); // the second pass, with the first's echoes in it, as heard
    auto cut = print(PrintTail::Cut);
    differ = 0;
    for (uint64_t f = 0; f < pass; ++f) differ += cut.left[f] != heard_l[f] || cut.right[f] != heard_r[f];
    CHECK(cut.left.size() == pass && differ == 0);
    auto ring = print(PrintTail::Ring); // the sequencer stops where the pass ends: the echoes ring out, then quiet
    CHECK(ring.left.size() > pass + uint64_t(0.3 * rate) && ring.left.size() < pass + uint64_t(9 * rate));
    CHECK(peak_of(ring.left, size_t(pass + uint64_t(0.1 * rate))) > 0.01f);
    for (uint64_t f = 0; f < pass - pass % 128; ++f) CHECK(ring.left[f] == cut.left[f]); // its last block is split
    auto pre = print(PrintTail::Cut, PrintTap::PreFx); // the engine alone: no echo after the last hit
    CHECK(pre.left.size() == pass && peak_of(pre.left) > 0.05f && pre.left != cut.left);
    CHECK(model_frames_of(pass, rate) == uint32_t(std::llround(double(pass) * 44100 / rate)));

    // A print host's own effect clock: rendering one beside the live host on the same thread, at another tempo, with
    // a Pumper of its own, leaves what the live host plays as it was.
    {
        auto again = std::make_unique<EngineHost>();
        again->load(*project, *bank);
        CHECK(again->offer_sample(0, &tone));
        auto other = std::make_unique<Project>(*project);
        auto ob = std::make_unique<PatternBank>(*bank);
        print_project(*other, *ob, 0, 3);
        other->tempo = 900;
        auto beside = std::make_unique<EngineHost>(EngineHost::Lean {2});
        beside->load(*other, *ob);
        CHECK(beside->offer_sample(0, &tone));
        again->set_print_tap(0, true);
        std::vector<int32_t> o(256);
        again->send({Command::Type::Play}); beside->send({Command::Type::Play});
        differ = 0;
        for (uint64_t at = 0; at < 2 * pass; at += 128) {
            beside->render(o.data(), 128, 1);
            again->render(o.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) differ += again->print_left()[f] != heard_l[at + f];
        }
        CHECK(differ == 0);
    }

    // Each tick on the frame tick_frame() says: one frame a render, the tick count watched.
    {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        tone_kit(*p, 0);
        p->tempo = 1777; p->swing = 63;
        b->at[0][0].length = 4 * sixteenth_ticks;
        auto host = std::make_unique<EngineHost>();
        host->load(*p, *b);
        host->send({Command::Type::Play});
        std::vector<int32_t> o(2);
        std::vector<uint64_t> at;
        for (uint64_t f = 0; at.size() < 40; ++f) {
            uint32_t before = host->ticks();
            host->render(o.data(), 1, 1);
            if (host->ticks() > before) at.push_back(f);
        }
        for (uint32_t k = 0; k < at.size(); ++k) CHECK(host->tick_frame(k) == at[k]);
    }

    // The hash: what makes the print moves it, the mix, other tracks and other patterns do not.
    auto states = std::array<std::vector<uint8_t>, max_plugins> {};
    auto hash = [&](const Project& p, const PatternBank& b, PrintTap tap = PrintTap::PostFx, PrintTail tail = PrintTail::Loop,
                    unsigned at = 48000) {
        auto q = std::make_unique<Project>(p);
        auto k = std::make_unique<PatternBank>(b);
        print_project(*q, *k, 0, 3);
        return print_hash(*q, *k, states, tap, tail, at);
    };
    const uint64_t h = hash(*project, *bank);
    CHECK(h == hash(*project, *bank));
    auto changed = [&](const std::function<void(Project&, PatternBank&)>& change) {
        auto p = std::make_unique<Project>(*project);
        auto b = std::make_unique<PatternBank>(*bank);
        change(*p, *b);
        return hash(*p, *b) != h;
    };
    // The mixer: a track's level and pan come after its chain and its strip, so a print is made at unity and centred
    // with its strip flat, and none of those count, with effects on its chain or not (print_mix()). Sends never do.
    CHECK(project->tracks[0].fx.slots[0].used());
    CHECK(!changed([](Project& p, PatternBank&) { p.tracks[0].mix.level = 64; }) && !changed([](Project& p, PatternBank&) { p.tracks[0].mix.pan = -30; }));
    CHECK(!changed([](Project& p, PatternBank&) { p.tracks[0].mix.sends[1] = 80; }));
    CHECK(!changed([](Project& p, PatternBank&) { p.tracks[0].strip.eq[4] = 60; p.tracks[0].strip.comp_amount = 20; }));
    {
        auto plain = std::make_unique<Project>(*project);
        plain->tracks[0].fx = Chain {};
        const uint64_t at_unity = hash(*plain, *bank);
        plain->tracks[0].mix.level = 64; plain->tracks[0].mix.pan = -30; plain->tracks[0].mix.sends[1] = 80;
        CHECK(hash(*plain, *bank) == at_unity);
    }
    CHECK(!changed([](Project& p, PatternBank&) { p.tracks[0].mix.mute = true; p.tracks[1].mix.solo = true; }));
    CHECK(!changed([](Project& p, PatternBank&) { p.tracks[0].set_name("Drums"); p.tracks[0].last_pad = 7; p.name[0] = 'X'; }));
    CHECK(!changed([](Project&, PatternBank& b) { put(b.at[1][0], 1, uint8_t(sampler_first_note), 50); }));     // another track's notes
    CHECK(!changed([](Project&, PatternBank& b) { put(b.at[0][0], 5, uint8_t(sampler_first_note), 50); }));     // another pattern
    CHECK(!changed([](Project& p, PatternBank&) { p.buses[0].fx.slots[0].params[0] = 10; p.master.slots[0] = {uint8_t(EffectKind::Drive), false, {}}; }));
    CHECK(!changed([](Project&, PatternBank& b) { b.at[0][3].write_motion(point(2, motion_level, 40)); })); // a mix lane stays live
    CHECK(changed([](Project&, PatternBank& b) { put(b.at[0][3], 1, uint8_t(sampler_first_note), 50); }));      // its notes
    CHECK(changed([](Project&, PatternBank& b) { b.at[0][3].length = 8 * sixteenth_ticks; }));
    CHECK(changed([](Project& p, PatternBank&) { p.tracks[0].pads[0].pitch = 3; }));                          // its sound
    CHECK(changed([](Project& p, PatternBank&) { p.tracks[0].fx.slots[0].params[1] = 77; }));                 // its chain
    CHECK(changed([](Project& p, PatternBank&) { p.tempo = 1200; }));
    CHECK(changed([](Project& p, PatternBank&) { p.tracks[0].speed = 8; }));
    CHECK(changed([](Project& p, PatternBank&) { std::snprintf(p.samples.edit(0).path, sizeof(SampleRef::path), "/media/card/Other.wav"); }));
    CHECK(hash(*project, *bank, PrintTap::PreFx) != h && hash(*project, *bank, PrintTap::PostFx, PrintTail::Ring) != h);
    CHECK(hash(*project, *bank, PrintTap::PostFx, PrintTail::Loop, 44100) != h); // printed at another rate
    {   // a plug-in's state counts only where the track uses it
        auto p = std::make_unique<Project>(*project);
        std::snprintf(p->plugins[2].path, sizeof(p->plugins[2].path), "Plugins/Some Effect.vst3");
        uint64_t unused = hash(*p, *bank);
        states[2] = {1, 2, 3};
        CHECK(hash(*p, *bank) == unused);
        p->tracks[0].fx.slots[2] = {plugin_effect_kind, false, {2, 0, 0, 0, 0, 0}};
        uint64_t used = hash(*p, *bank);
        states[2] = {1, 2, 4};
        CHECK(hash(*p, *bank) != used && used != unused);
        states[2].clear();
    }

    // PRNT: the records go and come back with the project; a file without them loads with none.
    PrintTable table;
    PrintRecord a; a.track = 0; a.pattern = 3; a.frames = 88200; a.pass = 88200; a.hash = h; a.file = print_file_name(0, 3);
    PrintRecord b2; b2.track = 5; b2.pattern = 127; b2.tap = PrintTap::PreFx; b2.tail = PrintTail::Ring; b2.frames = 99999; b2.pass = 44100;
    b2.rate = 96000;
    b2.hash = 0xfedcba9876543210ull; b2.file = print_file_name(5, 127);
    table.put(b2); table.put(a);
    CHECK(table.records.size() == 2 && table.records[0].track == 0 && table.find(5, 127) && !table.find(5, 126));
    CHECK(a.file == "Track 1 A4.wav" && b2.file == "Track 6 H16.wav");
    auto bytes = save_project(*project, *bank, nullptr, &table);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    PrintTable read;
    CHECK(load_project(bytes, *back, *back_bank, nullptr, nullptr, &read).empty() && read == table);
    CHECK(load_project(bytes, *back, *back_bank).empty() && back->tracks[0].pattern == 3); // as an older build reads it
    CHECK(load_project(save_project(*project, *bank), *back, *back_bank, nullptr, nullptr, &read).empty() && read.records.empty());
    CHECK(prints_folder("/media/card/Projects/Song.mldp") == "/media/card/Projects/Song Prints" && prints_folder("").empty());

    // A plug-in 300 frames late: its print starts on the pattern's first tick all the same.
    {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        synth(p->tracks[0], 0);
        put(b->at[0][0], 0, 60, 100);
        auto host = std::make_unique<EngineHost>(EngineHost::Lean {0});
        host->load(*p, *b);
        LatePlugin slow;
        slow.late = 300;
        CHECK(host->offer_plugin(0, &slow));
        auto made = render_print(*host, 0, played_pattern(b->at[0][0], speed_normal).length, PrintTap::PostFx, PrintTail::Cut);
        CHECK(made.error.empty() && made.latency == 300 && std::fabs(made.left[0] - 0.5f) < 1e-4f);
        for (unsigned f = 1; f < 2000; ++f) CHECK(made.left[f] == 0.0f);
    }
    std::puts("prints ok");
}

// Freeze plan F2b: a frozen track plays its prints instead of its engine and chain. At Post FX a kit's prints sound as
// the live track does, sample for sample, across a loop's wrap and a pattern change; at Pre FX its chain runs live
// after them; its level and pan still apply. The engine sleeps a second after it falls quiet, a hand wakes it in the
// same block, a pattern without a print keeps it awake. FRZN keeps the freeze. A streamed print reads only what the
// pager pinned.
// Freeze plan F3: an audio track plays its pattern's take from the pattern's top, on the frame, again each pass; a
// take armed records one pass from where the pattern comes round (here the track itself, as the Track source takes it).
// Freeze plan F3, Warp: a take recorded at 120 BPM played at 150 BPM ends a fifth sooner; Repitch raises it with the
// speed, Stretch keeps its pitch (and Stretch's semitones move it alone); off, it plays as recorded.
void audio_warp() {
    const float rate = dsp::sample_rate();
    const unsigned frames = unsigned(rate) * 2;
    std::vector<float> sine(frames);
    for (unsigned f = 0; f < frames; ++f) sine[f] = 0.5f * std::sin(2 * dsp::pi * 441.0f * float(f) / rate);
    struct Heard { double hz; unsigned length; };
    auto play = [&](bool warp, WarpMode mode, float pitch) {
        AudioPack pack;
        unsigned part = pack.hold_take(sine, sine, rate);
        RegionClip& clip = pack.add(0);
        clip.take = pack.takes[part].get(); clip.frames = frames; clip.resident = true;
        clip.tempo = 1200; clip.warp = warp; clip.mode = mode; clip.pitch = pitch;
        RegionPlayer player;
        player.set_tempo(1500);
        player.follow(&clip, 0, true, true, rate);
        std::vector<float> l(256), r(256), out;
        while (player.sounding() && out.size() < size_t(frames) * 2) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            player.render(l.data(), r.data(), 256);
            out.insert(out.end(), l.begin(), l.end());
        }
        unsigned rising = 0, length = 0;
        const size_t from = size_t(rate * 0.2f), to = size_t(rate * 1.2f);
        for (size_t f = from + 1; f < to && f < out.size(); ++f) rising += out[f - 1] < 0 && out[f] >= 0;
        for (size_t f = 0; f < out.size(); ++f) if (std::fabs(out[f]) > 1e-3f) length = unsigned(f);
        return Heard {double(rising) * rate / double(to - from), length};
    };
    auto off = play(false, WarpMode::Stretch, 1), repitch = play(true, WarpMode::Repitch, 1), stretch = play(true, WarpMode::Stretch, 1);
    auto up = play(true, WarpMode::Stretch, std::pow(2.0f, 3 / 12.0f));
    CHECK(std::fabs(off.hz - 441) < 4 && off.length + 300 >= frames);
    CHECK(std::fabs(repitch.hz - 551.25) < 6 && std::abs(int(repitch.length) - int(frames * 4 / 5)) < 600);
    CHECK(std::fabs(stretch.hz - 441) < 6 && std::abs(int(stretch.length) - int(frames * 4 / 5)) < int(rate * 0.07f));
    CHECK(std::fabs(up.hz - 524.4) < 8 && std::abs(int(up.length) - int(frames * 4 / 5)) < int(rate * 0.07f));
}

void audio_track_playback() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    project->tracks[0].engine = EngineKind::Audio;
    bank->at[0][0].length = 4 * ticks_per_quarter; // one bar
    const unsigned rate = unsigned(std::lround(dsp::sample_rate()));
    const size_t bar_frames = size_t(rate) * 2; // 4/4 at 120 BPM
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    host->set_print_tap(0, true);
    auto* pack = new AudioPack;
    const size_t take = bar_frames / 2; // half a bar: the second half of each pass is quiet
    unsigned part = pack->hold_take(std::vector<float>(take, 0.25f), std::vector<float>(take, -0.25f), float(rate));
    RegionClip& clip = pack->add(0);
    clip.take = pack->takes[part].get(); clip.frames = uint32_t(take); clip.resident = true;
    CHECK(pack->clips[0] == &clip && pack->clips[1] == nullptr);
    Command regions {Command::Type::AudioRegions, 0};
    regions.audio = pack;
    CHECK(host->send(regions));
    host->send({Command::Type::AudioRecord, 0, 0, 0, 0}); // one pass, from the pattern's top
    host->send({Command::Type::Play});
    std::vector<int32_t> o(256);
    std::vector<float> left;
    for (unsigned b = 0; b < unsigned(3 * bar_frames / 128); ++b) {
        host->render(o.data(), 128, 1);
        for (unsigned f = 0; f < 128; ++f) left.push_back(host->print_left()[f]);
    }
    for (size_t pass = 0; pass < 2; ++pass) { // each pass: the take from the top, then quiet
        size_t top = pass * bar_frames;
        float low = 1, high = 0;
        for (size_t f = top + 8; f < top + take - 8; ++f) { low = std::min(low, left[f]); high = std::max(high, left[f]); }
        CHECK(std::fabs(low - 0.25f) < 1e-4f && std::fabs(high - 0.25f) < 1e-4f);
        CHECK(peak_of(left, top + take + 8, top + bar_frames - 8) < 1e-6f);
    }
    CHECK(std::fabs(left[bar_frames]) > 0.2f && std::fabs(left[bar_frames - 300]) < 1e-6f); // on the second pass's first frame
    CHECK(host->audio_underruns() == 0 && host->audio_waits() == 0);
    auto& ring = host->audio_take();
    CHECK(ring.state() == Bounce::State::Done && ring.taken() + 1 >= bar_frames && ring.taken() <= bar_frames + 1);
    CHECK(host->audio_take_pattern() == 0 && host->audio_take_track() < 0);
    std::vector<float> taken(2 * 4096);
    float heard = 0;
    for (unsigned n; (n = ring.pop(taken.data(), 4096)) > 0;) for (unsigned f = 0; f < 2 * n; ++f) heard = std::max(heard, std::fabs(taken[f]));
    CHECK(std::fabs(heard - 0.25f) < 1e-3f);
    ring.release();
    host->send({Command::Type::AudioRegions, 0}); // none: the pack handed back
    host->render(o.data(), 128, 1);
    AudioPack* back = nullptr;
    for (unsigned b = 0; b < 20 && !host->take_done_audio(back); ++b) host->render(o.data(), 128, 1);
    CHECK(back == pack);
    delete back;
}

void freeze_f2b() {
    const float rate = dsp::sample_rate();
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0); tone_kit(*project, 1);
    project->tempo = 1200; project->swing = 50;
    // A1 and A2 of track 1: notes early in the bar (each pad plays its second of tone), so nothing sounds over a
    // pattern's end; A3 has no print.
    const uint8_t pad = uint8_t(sampler_first_note);
    put(bank->at[0][0], 0, pad, 100); put(bank->at[0][0], 3, uint8_t(pad + 1), 90); put(bank->at[0][0], 5, uint8_t(pad + 2), 80);
    put(bank->at[0][1], 2, uint8_t(pad + 3), 110); put(bank->at[0][1], 6, pad, 70);
    put(bank->at[0][2], 0, uint8_t(pad + 1), 100);
    for (unsigned s = 0; s < 16; s += 4) put(bank->at[1][0], s, uint8_t(pad + 1), 90);
    auto tone = tone_sample();
    const uint32_t ticks = played_pattern(bank->at[0][0], speed_normal).length;

    // A print of track 1's pattern, as the print engine makes it, held whole.
    auto clip_of = [&](const Project& from, unsigned pattern, PrintTail tail, PrintTap tap) {
        auto p = std::make_unique<Project>(from);
        auto b = std::make_unique<PatternBank>(*bank);
        print_project(*p, *b, 0, pattern);
        auto host = std::make_unique<EngineHost>(EngineHost::Lean {2});
        host->load(*p, *b);
        CHECK(host->offer_sample(0, &tone));
        auto a = render_print(*host, 0, played_pattern(b->at[0][pattern], speed_normal).length, tap, tail);
        CHECK(a.error.empty());
        std::vector<float> frames(a.left.size() * 2);
        for (size_t f = 0; f < a.left.size(); ++f) { frames[2 * f] = a.left[f]; frames[2 * f + 1] = a.right[f]; }
        auto c = std::make_unique<PrintClip>();
        c->hold_whole(std::move(frames), uint32_t(std::lround(rate)), uint32_t(a.pass), tail);
        return c;
    };
    auto pack_of = [&](const Project& from, PrintTail tail, PrintTap tap) {
        auto* pack = new FreezePack;
        pack->tap = tap;
        for (unsigned p : {0u, 1u}) { pack->owned.push_back(clip_of(from, p, tail, tap)); pack->clips[p] = pack->owned.back().get(); }
        return pack;
    };
    // The project played, track 1 taken as the Track source takes it (after its chain, strip, level and pan): A1 for
    // two passes, then A2 (launched in the second pass). Frozen on `pack` from the start when one is given. `each`
    // before every block.
    struct Run { std::vector<float> l, r; std::unique_ptr<EngineHost> host; };
    const uint64_t pass = uint64_t(std::llround(ticks * 44100.0 * 60 / 120 / ticks_per_quarter * rate / 44100));
    const unsigned blocks = unsigned((3 * pass + pass / 2) / 128);
    auto run = [&](const Project& from, FreezePack* pack, const std::function<void(EngineHost&, unsigned)>& each = {}) {
        Run out;
        out.host = std::make_unique<EngineHost>();
        auto& host = *out.host;
        host.load(from, *bank);
        CHECK(host.offer_sample(0, &tone));
        host.send({Command::Type::RecordSource, uint8_t(InputSource::Track), 0, 0, 1});
        if (pack) { Command c {Command::Type::Freeze, 0}; c.freeze = pack; CHECK(host.send(c)); }
        host.send({Command::Type::Play});
        std::vector<int32_t> o(256);
        for (unsigned b = 0; b < blocks; ++b) {
            if (b == unsigned(pass * 3 / 2 / 128)) host.send({Command::Type::Pattern, 0, 0, 0, 1});
            if (each) each(host, b);
            host.render(o.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) { out.l.push_back(host.print_left()[f]); out.r.push_back(host.print_right()[f]); }
        }
        return out;
    };
    auto differ = [](const Run& a, const Run& b, float by = 0) {
        uint64_t n = 0;
        for (size_t f = 0; f < a.l.size(); ++f) n += std::fabs(a.l[f] - b.l[f]) > by || std::fabs(a.r[f] - b.r[f]) > by;
        return n;
    };

    auto live = run(*project, nullptr);
    CHECK(live.host->pattern_of(0) == 1);
    for (uint64_t p = 0; p < 3; ++p) CHECK(peak_of(live.l, size_t(p * pass), size_t((p + 1) * pass)) > 0.05f); // A1, A1, A2
    CHECK(live.host->frozen_tracks() == 0 && live.host->sleeping_tracks() == 0);
    {   // Post FX: the same, sample for sample, the engine asleep, its CPU gone from the track
        auto frozen = run(*project, pack_of(*project, PrintTail::Loop, PrintTap::PostFx));
        CHECK(differ(live, frozen) == 0);
        CHECK(frozen.host->frozen_tracks() == 1 && frozen.host->printing_tracks() == 1 && frozen.host->sleeping_tracks() == 1);
        CHECK(frozen.host->print_underruns() == 0 && frozen.host->print_waits() == 0);
    }
    {   // its level and pan after the print
        auto mixed = std::make_unique<Project>(*project);
        mixed->tracks[0].mix.level = 80; mixed->tracks[0].mix.pan = -20;
        auto heard = run(*mixed, nullptr);
        auto frozen = run(*mixed, pack_of(*project, PrintTail::Loop, PrintTap::PostFx));
        CHECK(differ(heard, live, 1e-3f) > 0 && differ(heard, frozen, 1e-5f) == 0);
    }
    {   // Pre FX: the chain (an Echo, its repeats over the patterns' ends) runs on the print as it ran on the engine
        auto echoed = std::make_unique<Project>(*project);
        echoed->tracks[0].fx.slots[0] = {uint8_t(EffectKind::Echo), false, {40, 50, 50, 50, 0, 0}};
        auto heard = run(*echoed, nullptr);
        auto frozen = run(*echoed, pack_of(*echoed, PrintTail::Loop, PrintTap::PreFx));
        CHECK(differ(heard, live, 1e-3f) > 0 && differ(heard, frozen) == 0);
    }
    {   // Post FX with a Drive on the chain at level 60, panned: the Drive never hears the level or pan (the mixer:
        // they come after the chain and the strip), so the print is made at unity and centred (print_mix()) and the
        // frozen track sounds the same; its strip stays live on the print
        auto driven = std::make_unique<Project>(*project);
        driven->tracks[0].fx.slots[0] = {uint8_t(EffectKind::Drive), false, {90, 60, 50, 50, 0, 0}};
        driven->tracks[0].mix.level = 60; driven->tracks[0].mix.pan = 15;
        driven->tracks[0].strip.eq[3] = 60; driven->tracks[0].strip.comp_amount = 18;
        CHECK(print_mix(driven->tracks[0]).level == 100 && print_mix(driven->tracks[0]).pan == 0);
        auto heard = run(*driven, nullptr);
        auto frozen = run(*driven, pack_of(*driven, PrintTail::Loop, PrintTap::PostFx));
        CHECK(differ(heard, frozen, 1e-5f) == 0);
        auto flat = std::make_unique<Project>(*driven); // the strip flat: the print the same, the sound not
        flat->tracks[0].strip = Strip {};
        CHECK(differ(run(*flat, pack_of(*flat, PrintTail::Loop, PrintTap::PostFx)), frozen, 1e-3f) > 0);
        auto louder = std::make_unique<Project>(*driven); // the level turned up after the freeze: the print follows it
        louder->tracks[0].mix.level = 80;
        auto* again = pack_of(*driven, PrintTail::Loop, PrintTap::PostFx);
        auto up = run(*louder, again);
        CHECK(peak_of(up.l, 0, up.l.size()) > peak_of(frozen.l, 0, frozen.l.size()) * 1.5f);
    }

    // Wake and sleep. Asleep a second after the freeze (it took over with nothing sounding); a hand's note wakes it in
    // the same block and sounds over the print; a second after it falls quiet it sleeps again, not before.
    {
        auto* pack = pack_of(*project, PrintTail::Loop, PrintTap::PostFx);
        const unsigned second = unsigned(rate / 128), hit = second + 40, off = hit + 20;
        std::vector<uint32_t> asleep(blocks);
        unsigned b_now = 0;
        auto woke = run(*project, pack, [&](EngineHost& host, unsigned b) {
            b_now = b;
            if (b) asleep[b - 1] = host.sleeping_tracks() & 1;
            if (b == hit) host.send({Command::Type::NoteOn, 0, 0, uint16_t(pad + 3), 100});
            if (b == off) host.send({Command::Type::NoteOff, 0, 0, uint16_t(pad + 3), 0});
        });
        (void)b_now;
        CHECK(!asleep[second - 10] && asleep[second + 5] && asleep[hit - 1]);
        CHECK(!asleep[hit] && !asleep[off + second - 20]); // awake from its block on, held, then a second's quiet
        CHECK(asleep[off + 2 * second] && asleep[blocks - 2]);
        uint64_t heard = 0; // the hand's note over the print: the frames after it differ from the print alone
        for (size_t f = size_t(hit) * 128; f < size_t(off) * 128; ++f) heard += woke.l[f] != live.l[f];
        CHECK(heard > 1000);
        for (size_t f = 0; f < size_t(hit) * 128; ++f) CHECK(woke.l[f] == live.l[f]);
    }
    // Freeze plan F2d: a deep freeze plays its prints as a freeze does, sample for sample, but nothing wakes the
    // engine: a hand's note, a pad, the arp go nowhere, and a pattern without a print stays silent and asleep.
    {
        auto* pack = pack_of(*project, PrintTail::Loop, PrintTap::PostFx);
        pack->deep = true;
        const unsigned second = unsigned(rate / 128), hit = second + 40, off = hit + 20;
        std::vector<uint32_t> asleep(blocks);
        auto deep = run(*project, pack, [&](EngineHost& host, unsigned b) {
            if (b) asleep[b - 1] = host.sleeping_tracks() & 1;
            if (b == hit) {
                host.send({Command::Type::NoteOn, 0, 0, uint16_t(pad + 3), 100});
                Command hand {Command::Type::PadNote, 0, 0, 2, 100};
                host.send(hand);
                host.send({Command::Type::ArpKey, 0, 0, 60, 100});
            }
            if (b == off) { host.send({Command::Type::NoteOff, 0, 0, uint16_t(pad + 3), 0}); host.send({Command::Type::ArpKey, 0, 0, 60, 0}); }
        });
        CHECK(differ(live, deep) == 0);
        CHECK(asleep[second + 5] && asleep[hit] && asleep[off] && asleep[blocks - 2]);
    }
    {
        auto* pack = pack_of(*project, PrintTail::Loop, PrintTap::PostFx);
        pack->deep = true;
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        CHECK(host->offer_sample(0, &tone));
        host->set_print_tap(0, true);
        Command c {Command::Type::Freeze, 0}; c.freeze = pack;
        CHECK(host->send(c));
        host->send({Command::Type::Pattern, 0, 0, 0, 2}); // A3: no print
        host->send({Command::Type::Play});
        std::vector<int32_t> o(256);
        float loudest = 0;
        for (unsigned b = 0; b < unsigned(3 * rate / 128); ++b) {
            host->render(o.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) loudest = std::max(loudest, std::fabs(host->print_left()[f]));
        }
        CHECK(loudest < 1e-6f && (host->sleeping_tracks() & 1) && !(host->printing_tracks() & 1));
        host->send({Command::Type::Freeze, 0}); // unfrozen: live again, the pack handed back
        host->render(o.data(), 128, 1);
        CHECK(host->frozen_tracks() == 0 && host->sleeping_tracks() == 0);
        FreezePack* back = nullptr;
        for (unsigned b = 0; b < 20 && !host->take_done_freeze(back); ++b) host->render(o.data(), 128, 1);
        CHECK(back == pack);
        delete back;
    }
    {   // a pattern with no print plays live, the engine kept awake while it plays; unfrozen, the pack comes back
        auto* pack = pack_of(*project, PrintTail::Loop, PrintTap::PostFx);
        auto host = std::make_unique<EngineHost>();
        host->load(*project, *bank);
        CHECK(host->offer_sample(0, &tone));
        host->set_print_tap(0, true);
        Command c {Command::Type::Freeze, 0}; c.freeze = pack;
        CHECK(host->send(c));
        host->send({Command::Type::Pattern, 0, 0, 0, 2});
        host->send({Command::Type::Play});
        std::vector<int32_t> o(256);
        float loudest = 0;
        for (unsigned b = 0; b < unsigned(3 * rate / 128); ++b) {
            host->render(o.data(), 128, 1);
            CHECK(!(host->sleeping_tracks() & 1) && !(host->printing_tracks() & 1));
            for (unsigned f = 0; f < 128; ++f) loudest = std::max(loudest, std::fabs(host->print_left()[f]));
        }
        CHECK(loudest > 0.05f && host->frozen_tracks() == 1);
        host->send({Command::Type::Pattern, 0, 0, 0, 0}); // A1 on the next bar: its print, and asleep a second on
        for (unsigned b = 0; b < unsigned(4 * rate / 128); ++b) host->render(o.data(), 128, 1);
        CHECK((host->printing_tracks() & 1) && (host->sleeping_tracks() & 1));
        // Locate into the pattern: the print moves there with it.
        Command locate {Command::Type::Locate}; locate.position = 3 * ticks / 4;
        host->send(locate);
        host->render(o.data(), 128, 1);
        host->render(o.data(), 128, 1);
        CHECK(host->printing_tracks() & 1);
        host->send({Command::Type::Freeze, 0}); // unfrozen at once
        host->render(o.data(), 128, 1);
        CHECK(host->frozen_tracks() == 0 && host->sleeping_tracks() == 0);
        FreezePack* back = nullptr;
        for (unsigned b = 0; b < 20 && !host->take_done_freeze(back); ++b) host->render(o.data(), 128, 1);
        CHECK(back == pack);
        delete back;
    }

    // FRZN: a track's freeze goes and comes back with the project; a file without it loads with none; an older build
    // reads the file and plays the track live.
    {
        PrintTable table;
        auto& f = table.freeze[3];
        f.state = FreezeState::Frozen; f.tap = PrintTap::PreFx; f.tail = PrintTail::Ring;
        f.set(0, true); f.set(64, true); f.set(127, true);
        CHECK(f.plays(127) && f.plays(64) && !f.plays(1) && table.freeze[0].state == FreezeState::None);
        auto bytes = save_project(*project, *bank, nullptr, &table);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        PrintTable read;
        CHECK(load_project(bytes, *back, *back_bank, nullptr, nullptr, &read).empty() && read == table && read.freeze[3].frozen());
        CHECK(load_project(bytes, *back, *back_bank).empty()); // as an older build reads it
        CHECK(load_project(save_project(*project, *bank), *back, *back_bank, nullptr, nullptr, &read).empty() && !read.freeze[3].frozen());
    }

    // A print's file: its frames on a page's boundary (a JUNK chunk before them), read back as written.
    {
        char name[] = "/tmp/melodee-print-XXXXXX";
        int fd = mkstemp(name);
        CHECK(fd >= 0);
        close(fd);
        WavStream wav;
        std::vector<float> frames(2 * 1000);
        for (size_t n = 0; n < frames.size(); ++n) frames[n] = float(n % 77) / 100.0f - 0.3f;
        CHECK(wav.open(name, 44100, 32, 4096) && wav.add(frames.data(), 1000) && wav.close());
        std::ifstream in(name, std::ios::binary | std::ios::ate);
        CHECK(uint64_t(in.tellg()) == 4096 + 8000);
        SampleData read;
        CHECK(read_wav(name, read).empty() && read.frames == 1000 && read.left[999] == frames[1998] && read.right[5] == frames[11]);
        std::remove(name);
    }

    // Streamed: a print read through the pager reads only pinned frames. Its first second is held: it starts at once;
    // pumped, it keeps ahead; not pumped, the frames past what is pinned are silent and counted; a jump to where
    // nothing is held waits, not counted as late.
    {
        std::vector<std::pair<uintptr_t, bool>> log;
        pin_log = &log; pin_fails = false;
        StreamPager pager(logged_pin);
        const uint32_t frames = 6 * stream_chunk;
        std::vector<float> data(2 * frames);
        for (size_t n = 0; n < data.size(); ++n) data[n] = 0.25f;
        PrintClip clip;
        clip.part.left = clip.part.right = data.data();
        clip.part.total = 2 * frames;
        clip.part.pins.assign(clip.part.total / stream_chunk, 0);
        clip.part.frames.store(clip.part.total);
        clip.frames = frames; clip.pass = frames; clip.rate = uint32_t(rate); clip.tail = PrintTail::Loop;
        CHECK(pager.hold(clip.part, 0, 2 * stream_chunk));
        clip.held.push_back({0, 2 * stream_chunk});
        CHECK(clip.safe_from(0) == 2 * stream_chunk && clip.safe_from(stream_chunk) == 0);
        PrintPlayer player;
        player.set_pager(&pager);
        std::vector<float> l(128), r(128);
        player.follow(&clip, 0, 0, 1e9, rate);
        CHECK(player.playing());
        float sum = 0;
        for (unsigned b = 0; b < 2 * stream_chunk / 128; ++b) { // pumped as its thread would
            std::fill(l.begin(), l.end(), 0.0f);
            player.render(l.data(), r.data(), 128);
            while (pager.pump()) {}
            for (float v : l) sum += v;
        }
        CHECK(player.underruns == 0 && player.waits == 0 && std::fabs(sum - 0.25f * 2 * stream_chunk) < 1);
        for (unsigned b = 0; b < 4 * stream_chunk / 128; ++b) player.render(l.data(), r.data(), 128); // the pager left behind
        CHECK(player.underruns > 0);
        const uint32_t late = player.underruns;
        player.cut_all();
        while (pager.pump()) {}
        player.follow(&clip, 9, 4.5 * stream_chunk, 1e9, rate); // into the middle, nothing held there
        player.render(l.data(), r.data(), 128);
        CHECK(player.waits == 1 && player.underruns == late);
        while (pager.pump()) {}
        std::fill(l.begin(), l.end(), 0.0f);
        player.render(l.data(), r.data(), 128);
        CHECK(l[127] > 0.2f && player.underruns == late); // the pager has it now
        player.cut_all();
        while (pager.pump()) {}
        pin_log = nullptr;
    }
    std::puts("freeze ok");
}

// Pad sends, freeze: a kit's print carries its pads' sends in a pair a bus (A and C here), cut to the print's frames;
// a frozen kit plays them into its buses as the live kit sends, at its level, so what the buses hear is the same.
void pad_sends_freeze() {
    const float rate = dsp::sample_rate();
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0);
    project->tempo = 1200;
    auto& kit = project->tracks[0];
    kit.pads[0].sends = {30, 0, 0, 0}; kit.pads[1].sends = {0, 0, 50, 0};
    CHECK(print_sends(kit) == 5 && print_sends(project->tracks[1]) == 0);
    const uint8_t pad = uint8_t(sampler_first_note);
    put(bank->at[0][0], 0, pad, 100); put(bank->at[0][0], 8, uint8_t(pad + 1), 100);
    auto tone = tone_sample();
    const uint32_t ticks = played_pattern(bank->at[0][0], speed_normal).length;

    auto p = std::make_unique<Project>(*project);
    auto b = std::make_unique<PatternBank>(*bank);
    print_project(*p, *b, 0, 0);
    auto printer = std::make_unique<EngineHost>(EngineHost::Lean {2});
    printer->load(*p, *b);
    CHECK(printer->offer_sample(0, &tone));
    auto a = render_print(*printer, 0, ticks, PrintTap::PostFx, PrintTail::Loop, {}, print_sends(kit));
    CHECK(a.error.empty() && a.sends == 5 && a.send_left[0].size() == a.left.size() && a.send_left[2].size() == a.left.size() && a.send_left[1].empty());
    const size_t early = size_t(rate / 2) + 37, late = size_t(rate * 3 / 2) + 37; // off the tone's zero crossings
    CHECK(std::fabs(a.left[early]) > 0.01f && std::fabs(a.send_left[0][early] / a.left[early] - 0.30f) < 1e-3f && a.send_left[2][early] == 0.0f);
    CHECK(std::fabs(a.left[late]) > 0.01f && std::fabs(a.send_left[2][late] / a.left[late] - 0.50f) < 1e-3f && a.send_left[0][late] == 0.0f);

    // Held whole with its sends, as the shell maps the file: six floats a frame.
    std::vector<float> frames(a.left.size() * 6);
    for (size_t f = 0; f < a.left.size(); ++f) {
        float* at = frames.data() + 6 * f;
        at[0] = a.left[f]; at[1] = a.right[f]; at[2] = a.send_left[0][f]; at[3] = a.send_right[0][f]; at[4] = a.send_left[2][f]; at[5] = a.send_right[2][f];
    }
    auto* pack = new FreezePack;
    pack->owned.push_back(std::make_unique<PrintClip>());
    pack->owned.back()->hold_whole(std::move(frames), uint32_t(std::lround(rate)), uint32_t(a.pass), PrintTail::Loop, 5);
    CHECK(pack->owned.back()->stride == 6 && pack->owned.back()->frames == a.left.size());
    pack->clips[0] = pack->owned.back().get();

    // Three passes, live and frozen: the pads' sends into A and C as the host taps them.
    struct Heard { std::vector<float> a, c; };
    const unsigned blocks = unsigned(uint64_t(a.pass) * 3 / 128);
    auto run = [&](FreezePack* frozen, int level) {
        Heard out;
        EngineHost host;
        host.load(*project, *bank);
        CHECK(host.offer_sample(0, &tone));
        host.set_print_tap(0, true);
        if (level >= 0) host.send({Command::Type::Level, 0, 0, 0, int16_t(level)});
        if (frozen) { Command c {Command::Type::Freeze, 0}; c.freeze = frozen; CHECK(host.send(c)); }
        host.send({Command::Type::Play});
        std::vector<int32_t> o(256);
        for (unsigned n = 0; n < blocks; ++n) {
            host.render(o.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) { out.a.push_back(host.print_send_left(0)[f]); out.c.push_back(host.print_send_left(2)[f]); }
        }
        return out;
    };
    auto live = run(nullptr, -1), frozen = run(pack, -1);
    float most = 0, diff = 0;
    for (size_t f = size_t(a.pass); f < live.a.size(); ++f) { // from the second pass: the first hit live eases its send in once
        most = std::max(most, std::fabs(live.a[f]));
        diff = std::max({diff, std::fabs(live.a[f] - frozen.a[f]), std::fabs(live.c[f] - frozen.c[f])});
    }
    CHECK(most > 0.05f && diff < 1e-3f);
}

// Pads sent to submixes (MPC 3's pad output): pad 3 into submix 3 leaves the kit's stem and plays in the submix's, at
// the kit's level; soloing submix 3 keeps the kit heard; USB out's submix rows carry it; PSUB keeps it.
void pad_outputs() {
    { // A destination stays in the route mask until its last sounding pad moves or loses its sample.
        SamplerEngine kit;
        PadSound pad;
        pad.out = 3;
        kit.set_pad(0, pad);
        CHECK(kit.routes() == 0);
        pad.sample = 0;
        kit.set_pad(0, pad);
        kit.set_pad(1, pad);
        CHECK(kit.routes() == 4);
        pad.out = 8;
        kit.set_pad(1, pad);
        CHECK(kit.routes() == (4 | 128));
        kit.move_pad(1, PadParam::Level, 42);
        kit.restore_pads();
        CHECK(kit.routes() == (4 | 128));
        pad.sample = no_sample;
        kit.set_pad(1, pad);
        kit.set_pad(0, PadSound {});
        CHECK(kit.routes() == 0);
    }
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0);
    project->tracks[0].pads[2].out = 3;
    auto saved = save_project(*project, *bank);
    auto loaded = std::make_unique<Project>();
    auto loaded_bank = std::make_unique<PatternBank>();
    CHECK(load_project(saved, *loaded, *loaded_bank).empty() && loaded->tracks[0].pads[2].out == 3 && loaded->tracks[0].pads[0].out == 0);
    CHECK(mixer_left_out(*project) == 1 && mixer_left_out(*project, true) == 0); // Live leaves it out, MPC 3 carries it
    EngineHost host;
    host.load(*project, *bank);
    auto tone = tone_sample();
    CHECK(host.offer_sample(0, &tone));
    host.send({Command::Type::Level, 0, 0, 0, 80});
    std::vector<float> rows(size_t(StemRing::stem_count) * 2 * EngineHost::block, 9.0f);
    host.set_live_stems(rows.data());
    std::vector<int32_t> out(256);
    std::vector<float> usb(size_t(track_count) * EngineHost::block, 9.0f);
    auto play = [&](unsigned blocks) { for (unsigned n = 0; n < blocks; ++n) host.render(out.data(), 128, 1, nullptr, usb.data()); };
    auto peak = [&](unsigned stem) {
        float most = 0;
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned f = 0; f < EngineHost::block; ++f) most = std::max(most, std::fabs(rows[size_t(stem * 2 + side) * EngineHost::block + f]));
        return most;
    };
    auto usb_peak = [&](unsigned row) {
        float most = 0;
        for (unsigned f = 0; f < EngineHost::block; ++f) most = std::max(most, std::fabs(usb[size_t(row) * EngineHost::block + f]));
        return most;
    };
    const unsigned sub3 = track_count + bus_count + 2;
    host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note + 2), 100});
    play(30);
    CHECK(peak(0) == 0.0f && peak(sub3) > 0.02f && peak(sub3 - 1) == 0.0f); // the kit's stem has none of it, submix 3 all
    const float routed = peak(sub3);
    host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note), 100});
    play(30);
    CHECK(peak(0) > 0.02f && std::fabs(peak(sub3) - routed) < 1e-4f); // pad 1 in the kit, pad 3 still in submix 3 only
    // USB out: tracks' rows (the kit mono in row 0), then the submixes' pairs (submix 3 in rows 4 and 5).
    CHECK(usb_peak(0) > 0.02f && usb_peak(4) == 0.0f);
    host.set_usb_submixes(true);
    play(4);
    CHECK(usb_peak(4) > 0.02f && usb_peak(5) > 0.02f && usb_peak(0) == 0.0f && usb_peak(2) == 0.0f);
    host.set_usb_submixes(false);
    // The kit's level reaches it: at 40 it is quieter.
    host.send({Command::Type::Level, 0, 0, 0, 40});
    play(30);
    CHECK(peak(sub3) < routed * 0.8f && peak(sub3) > 0.0f);
    // Submix 3 soloed: the kit (whose pad is in it) is still heard; its stem too.
    host.send({Command::Type::SubMix, 2, 0, Command::SubSolo, 1});
    play(30);
    CHECK(peak(sub3) > 0.0f && peak(0) > 0.0f);
    host.set_live_stems(nullptr);
}

// StemWriter: each file whole and in order across chunks, a pad stem's silence first, a silent file's peak 0.
void stem_writer() {
    const auto dir = std::filesystem::temp_directory_path() / "melodee-stem-writer";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    StemWriter writer;
    std::vector<std::string> paths {(dir / "a.wav").string(), (dir / "b.wav").string(), (dir / "quiet.wav").string()};
    CHECK(writer.open(paths, 48000) && writer.files() == 3);
    writer.add_silence(1, 100);
    const unsigned total = 133000; // past two chunks, in blocks of 1000
    CHECK(total > StemWriter::chunk_frames * 2);
    std::vector<float> block(2 * 1000);
    std::vector<float> zeros(2 * 1000, 0.0f);
    for (unsigned at = 0; at < total; at += 1000) {
        for (unsigned f = 0; f < 1000; ++f) { block[2 * f] = float((at + f) % 997) / 997.0f; block[2 * f + 1] = -block[2 * f]; }
        CHECK(writer.add(0, block.data(), 1000) && writer.add(1, block.data(), 1000) && writer.add(2, zeros.data(), 1000));
    }
    writer.flush();
    for (unsigned n = 0; n < 500 && !writer.done(); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(writer.done() && writer.queued_bytes() == 0);
    std::vector<float> loudest;
    CHECK(writer.close(&loudest) && loudest.size() == 3 && loudest[0] > 0.99f && loudest[2] == 0.0f);
    SampleData a, b;
    CHECK(read_wav(paths[0], a).empty() && read_wav(paths[1], b).empty());
    CHECK(a.frames == total && b.frames == total + 100);
    bool same = true;
    for (unsigned f = 0; f < total; f += 331) same = same && a.left[f] == float(f % 997) / 997.0f && a.right[f] == -a.left[f] && b.left[f + 100] == a.left[f];
    CHECK(same && b.left[99] == 0.0f);
    std::filesystem::remove_all(dir);
}

// Pad stems beside the stems: each kit pad apart, after its own chain, at the kit's level and pan; with no chain and a
// flat strip on the kit they add up to its stem, frame for frame. A pad stem holds only its own pad's hits.
void pad_stems() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0);
    EngineHost host;
    host.load(*project, *bank);
    auto tone = tone_sample();
    CHECK(host.offer_sample(0, &tone));
    host.send({Command::Type::Level, 0, 0, 0, 80});
    auto ring = std::make_unique<StemRing>();
    ring->arm(1u << 0, 2);
    ring->set_pad(0, 0, 0); // pad stem 0: pad 1
    ring->set_pad(1, 0, 2); // pad stem 1: pad 3
    CHECK(ring->rows() == 3 && ring->row_stem(0) == 0 && ring->row_stem(2) == StemRing::stem_count + 1 && ring->pads() == 2);
    CHECK(ring->pad_rows(0)[2] == 1 && ring->pad_rows(0)[1] == -1 && ring->pad_rows(1)[0] == -1);
    host.set_stems(ring.get());
    Command arm {Command::Type::BounceArm, 0, 0, 0, 0};
    arm.position = ticks_per_quarter / 2;
    host.send(arm);
    host.send({Command::Type::Play, 0});
    host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note), 100});
    std::vector<int32_t> out(256);
    for (unsigned n = 0; n < 20; ++n) host.render(out.data(), 128, 1);
    host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note + 2), 60});
    for (unsigned n = 0; n < 200; ++n) host.render(out.data(), 128, 1);
    CHECK(host.bounce().state() == Bounce::State::Done && host.bounce().taken() > 20 * 128);
    constexpr unsigned most = 65536;
    std::vector<float> got(size_t(ring->rows()) * 2 * most);
    unsigned frames = ring->pop_rows(got.data(), most);
    CHECK(frames == host.bounce().taken() && !ring->failed());
    auto at = [&](unsigned row, unsigned side, unsigned f) { return got[size_t(row) * 2 * most + 2 * f + side]; };
    float kit = 0, worst = 0, first = 0, early_third = 0, third = 0;
    for (unsigned f = 0; f < frames; ++f)
        for (unsigned side = 0; side < 2; ++side) {
            kit = std::max(kit, std::fabs(at(0, side, f)));
            worst = std::max(worst, std::fabs(at(1, side, f) + at(2, side, f) - at(0, side, f)));
            first = std::max(first, std::fabs(at(1, side, f)));
            (f < 19 * 128 ? early_third : third) = std::max(f < 19 * 128 ? early_third : third, std::fabs(at(2, side, f)));
        }
    CHECK(kit > 0.05f && worst < 1e-5f && first > 0.02f && early_third == 0.0f && third > 0.01f);
    host.set_stems(nullptr);
    // The old layout still: pop() hands stem_count blocks, a pad stem in none of them.
    ring->arm(1u << 3);
    CHECK(ring->rows() == 1 && ring->pads() == 0 && ring->pad_rows(0)[0] == -1);
    std::array<float, 4> one {0.5f, 0.25f, 0.5f, 0.25f};
    std::array<const float*, StemRing::row_most> l {}, r {};
    l[3] = one.data(); r[3] = one.data() + 1;
    ring->write(l.data(), r.data(), 0, 1);
    std::vector<float> old(size_t(StemRing::stem_count) * 2 * 4, 9.0f);
    CHECK(ring->pop(old.data(), 4) == 1 && old[size_t(3) * 2 * 4] == 0.5f && old[size_t(3) * 2 * 4 + 1] == 0.25f && old[0] == 0.0f);
}

// Pad sends: each kit pad into buses A-D at its own send, after its own chain, eased; none from a pad at 0. In the
// host they follow the kit's level and mute, and PSND / BSND keep them.
void pad_sends() {
    SampleData data;
    data.frames = 44100;
    data.left.assign(data.frames, 0.3f); data.right = data.left;
    SamplerEngine::Table table {};
    table[1] = &data;
    auto engine = std::make_unique<SamplerEngine>();
    engine->set_samples(&table);
    std::vector<float> l(128), r(128);
    std::array<std::vector<float>, 4> bus_l, bus_r;
    for (unsigned b = 0; b < 4; ++b) { bus_l[b].assign(128, 0.0f); bus_r[b].assign(128, 0.0f); }
    auto wire = [&](bool on) {
        for (unsigned b = 0; b < 4; ++b) engine->set_send_out(b, on ? bus_l[b].data() : nullptr, on ? bus_r[b].data() : nullptr);
    };
    auto block = [&] {
        std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
        for (unsigned b = 0; b < 4; ++b) { std::fill(bus_l[b].begin(), bus_l[b].end(), 0.0f); std::fill(bus_r[b].begin(), bus_r[b].end(), 0.0f); }
        engine->render_stereo(l.data(), r.data(), 128);
    };
    PadSound kick, snare, hat, clap;
    kick.sample = snare.sample = hat.sample = clap.sample = 1;
    snare.sends = {30, 0, 0, 0}; hat.sends = {15, 0, 0, 0}; clap.sends = {30, 20, 0, 0};
    CHECK(!kick.sending() && snare.sending());
    engine->set_pad(0, kick); engine->set_pad(1, snare); engine->set_pad(2, hat); engine->set_pad(3, clap);
    wire(true);
    auto hit = [&](int pad) { engine->silence(); engine->note_on(int(sampler_first_note) + pad, 100); block(); };
    hit(0); // Kick: dry
    float dry = l[100];
    CHECK(dry > 0.05f && engine->sends_fed() == 0 && bus_l[0][100] == 0.0f);
    hit(1); // Snare: its send eases up over its first block, then holds at 30 %
    CHECK(engine->sends_fed() == 1 && bus_l[0][2] < bus_l[0][120] && std::fabs(l[100] - dry) < 1e-6f);
    block();
    CHECK(std::fabs(bus_l[0][100] / l[100] - 0.30f) < 1e-4f && std::fabs(bus_r[0][100] / r[100] - 0.30f) < 1e-4f && bus_l[1][100] == 0.0f);
    hit(2); block();
    CHECK(std::fabs(bus_l[0][100] / l[100] - 0.15f) < 1e-4f);
    hit(3); block(); // Clap into A and B
    CHECK(engine->sends_fed() == 3 && std::fabs(bus_l[0][100] / l[100] - 0.30f) < 1e-4f && std::fabs(bus_l[1][100] / l[100] - 0.20f) < 1e-4f);
    clap.sends = {0, 20, 0, 0}; // turned down while it plays: eased away, not cut
    engine->set_pad(3, clap); block();
    CHECK(bus_l[0][2] > 0.0f && bus_l[0][127] < bus_l[0][2] && engine->sends_fed() == 3);
    block();
    CHECK(engine->sends_fed() == 2);
    wire(false); // no buses handed over: the kit plays as it would
    hit(1); block();
    CHECK(std::fabs(l[100] - dry) < 1e-6f && engine->sends_fed() == 0);

    // The host: a kit whose Snare sends 30 % into bus A (return 100) is heard 1.3 times as loud; Kick at 0 as dry;
    // the send follows the kit's level, and its mute silences it.
    auto heard = [](unsigned pad, uint8_t send, int level, bool mute) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        tone_kit(*project, 0);
        project->tracks[0].pads[pad].sends[0] = send;
        EngineHost host;
        host.load(*project, *bank);
        auto tone = tone_sample();
        CHECK(host.offer_sample(0, &tone));
        host.send({Command::Type::Return, 0, 0, 0, 100});
        if (level >= 0) host.send({Command::Type::Level, 0, 0, 0, int16_t(level)});
        if (mute) host.send({Command::Type::Mute, 0, 0, 0, 1});
        std::vector<int32_t> out(256);
        for (unsigned n = 0; n < 4; ++n) host.render(out.data(), 128, 1);
        host.send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note + pad), 25});
        double peak = 0;
        for (unsigned n = 0; n < 40; ++n) {
            host.render(out.data(), 128, 1);
            for (auto v : out) peak = std::max(peak, std::fabs(double(v) / 2147483648.0));
        }
        return peak;
    };
    double plain = heard(1, 0, -1, false), sent = heard(1, 30, -1, false);
    CHECK(plain > 0.05 && std::fabs(sent / plain - 1.3) < 0.01);
    CHECK(heard(0, 0, -1, false) == plain);
    double low = heard(1, 0, 50, false), low_sent = heard(1, 30, 50, false);
    CHECK(low < plain * 0.8 && std::fabs(low_sent / low - 1.3) < 0.01);
    CHECK(heard(1, 30, -1, true) < 1e-6);

    // PSND keeps them, an empty pad's are not kept; without PSND none; BSND carries them in a bank file.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto& kit = project->tracks[2];
    kit.engine = EngineKind::Sampler;
    std::snprintf(project->samples.edit(0).path, sizeof(SampleRef::path), "Samples/Snare.wav");
    project->samples.edit(0).frames = 44100;
    kit.pads[1].sample = 0; kit.pads[2].sample = 0;
    kit.pads[1].sends = {30, 0, 0, 100}; kit.pads[2].sends = {15, 0, 0, 0}; kit.pads[9].sends = {50, 0, 0, 0};
    auto file = save_project(*project, *bank);
    auto loaded = std::make_unique<Project>();
    auto loaded_bank = std::make_unique<PatternBank>();
    CHECK(load_project(file, *loaded, *loaded_bank).empty());
    CHECK((loaded->tracks[2].pads[1].sends == std::array<uint8_t, 4> {30, 0, 0, 100}) && loaded->tracks[2].pads[2].sends[0] == 15 &&
          !loaded->tracks[2].pads[9].sending() && !loaded->tracks[2].pads[0].sending());
    std::string text(file.begin(), file.end());
    file[text.find("PSND")] = 'X';
    CHECK(load_project(file, *loaded, *loaded_bank).empty() && !loaded->tracks[2].pads[1].sending());
    BankFile saved = bank_of(*project, 2, 0, "Sends"), back;
    CHECK(load_bank(save_bank(saved), back) && back.pads[1].sends[3] == 100 && back.pads[2].sends[0] == 15);
}

// Cloud is each pad's own: one pad plays grains while the pad beside it plays plain; PCLD keeps it, BCLD in a bank
// file; a file from before, whose kit held one Cloud, gives its values to every pad and the kit's go back to their start.
void pad_cloud() {
    SampleData noise = tone_sample();
    SamplerEngine::Table table {};
    table[0] = &noise;
    SamplerEngine e;
    e.set_samples(&table);
    PadSound plain; plain.sample = 0; plain.play = PadPlay::Gate;
    PadSound grains = plain; grains.cloud = true; grains.cloud_density = 60;
    e.set_pad(0, plain); e.set_pad(1, grains);
    std::vector<float> l(128), r(128);
    auto flags_of = [&](int note) {
        e.note_on(note, 100);
        for (unsigned b = 0; b < 4; ++b) e.render_stereo(l.data(), r.data(), 128);
        TrackTaps t; t.track = 0;
        e.taps(t);
        uint8_t flags = t.count ? t.voice[0].flags : 0xFF;
        e.note_off(note);
        for (unsigned b = 0; b < 400; ++b) e.render_stereo(l.data(), r.data(), 128);
        return flags;
    };
    CHECK(!(flags_of(int(sampler_first_note)) & tap::Cloud));
    CHECK(flags_of(int(sampler_first_note) + 1) & tap::Cloud);

    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto& kit = project->tracks[2];
    kit.engine = EngineKind::Sampler;
    std::snprintf(project->samples.edit(0).path, sizeof(SampleRef::path), "Samples/Pad.wav");
    project->samples.edit(0).frames = 44100;
    kit.pads[1].sample = 0; kit.pads[2].sample = 0;
    kit.pads[1].cloud = true; kit.pads[1].cloud_position = 70; kit.pads[1].cloud_scan = 90;
    auto file = save_project(*project, *bank);
    auto loaded = std::make_unique<Project>();
    auto loaded_bank = std::make_unique<PatternBank>();
    CHECK(load_project(file, *loaded, *loaded_bank).empty());
    const auto& back = loaded->tracks[2];
    CHECK(back.pads[1].cloud && back.pads[1].cloud_position == 70 && back.pads[1].cloud_scan == 90 && back.pads[2].plain_cloud());
    BankFile saved = bank_of(*project, 2, 0, "Cloud"), read;
    CHECK(load_bank(save_bank(saved), read) && read.pads[1].cloud && read.pads[1].cloud_position == 70 && read.pads[2].plain_cloud());

    // An older file: no PCLD, the kit's own Cloud on.
    kit.pads[1] = kit.pads[2];
    kit.params[sampler::Cloud] = 1; kit.params[sampler::Position] = 20; kit.params[sampler::Density] = 90;
    file = save_project(*project, *bank);
    CHECK(load_project(file, *loaded, *loaded_bank).empty());
    const auto& old = loaded->tracks[2];
    const auto& info = engine_info(EngineKind::Sampler);
    CHECK(old.pads[1].cloud && old.pads[2].cloud && old.pads[0].cloud && old.pads[2].cloud_position == 20 && old.pads[2].cloud_density == 90);
    CHECK(old.params[sampler::Cloud] == 0 && old.params[sampler::Position] == info.params[sampler::Position].initial);
}

// A PLUGIN track's performance knobs (engine/macros.h): the filter cuts, the echo rings on after its knob is turned
// back, and the sends' delay memory goes back to the pool once they fall silent (or the track stops being a plug-in's).
void plugin_macros() {
    DelayPool pool(4);
    MacroStage stage;
    stage.set_pool(&pool);
    std::vector<float> l(256), r(256);
    auto run = [&](float in, bool alternate) {
        for (unsigned f = 0; f < 256; ++f) l[f] = r[f] = alternate && (f & 1) ? -in : in;
        stage.process(l.data(), r.data(), 256);
        float peak = 0;
        for (unsigned f = 0; f < 256; ++f) peak = std::max(peak, std::fabs(l[f]));
        return peak;
    };
    CHECK(run(0.5f, true) == 0.5f); // all at 0: untouched
    stage.set(0, MacroKind::Filter, -90); // low pass, far down: the highest frequency all but gone
    float cut = 1;
    for (int b = 0; b < 8; ++b) cut = run(0.5f, true);
    CHECK(cut < 0.05f && pool.free_blocks() == 4); // the filter takes no delay memory
    stage.set(0, MacroKind::Filter, 0);
    CHECK(run(0.5f, true) == 0.5f);

    stage.set(1, MacroKind::Echo, 100);
    run(0.5f, false); // a block in at full send
    CHECK(pool.free_blocks() == 3);
    stage.set(1, MacroKind::Echo, 0); // turned back: what went in still repeats
    float tail = 0;
    for (int b = 0; b < 200; ++b) tail = std::max(tail, run(0, false));
    CHECK(tail > 0.05f && stage.sounding());
    for (int b = 0; b < 2400 && pool.free_blocks() < 4; ++b) run(0, false);
    CHECK(pool.free_blocks() == 4 && !stage.sounding()); // silent a second: its block back

    stage.set(2, MacroKind::Space, 60);
    run(0.5f, false);
    CHECK(pool.free_blocks() == 3);
    stage.release(); // the track plays something else now
    CHECK(pool.free_blocks() == 4 && !stage.sounding());

    DelayPool none(0); // the pool used up: a send stays silent, never the dry sound twice
    MacroStage starved;
    starved.set_pool(&none);
    starved.set(2, MacroKind::Space, 100);
    std::fill(l.begin(), l.end(), 0.5f); std::fill(r.begin(), r.end(), 0.5f);
    starved.process(l.data(), r.data(), 256);
    CHECK(l[100] == 0.5f);

    CHECK(macro_kind(0, 0) == MacroKind::Filter && macro_kind(0, 1) == MacroKind::Echo && macro_kind(0, 2) == MacroKind::Space &&
          macro_kind(int(MacroKind::Space), 0) == MacroKind::Space && macro_kind(99, 1) == MacroKind::Echo);
}

// A sample's note (engine/root_find.h): found in a held tone, a low one dying away, an 808's tail and a note off
// pitch, none in a noise, a phrase or silence. 16 Levels Scale and Chords, MIDI into 16 Levels, a KEYGROUP track of one
// sample and the project file follow it; a project from before plays its samples as the C they always were.
void sample_roots() {
    auto hz = [](double note) { return 440.0 * std::pow(2.0, (note - 69) / 12); };
    auto tone = [](double f, double seconds, int harmonics, double decay) {
        std::vector<float> x(size_t(seconds * 44100));
        for (size_t n = 0; n < x.size(); ++n) {
            const double t = double(n) / 44100;
            double v = 0;
            for (int h = 1; h <= harmonics; ++h) v += std::sin(2 * M_PI * f * h * t) / h;
            x[n] = float(0.4 * v * std::min(1.0, t / 0.005) * std::exp(-decay * t));
        }
        return x;
    };
    const auto a4 = find_root(tone(440, 1, 1, 0));
    CHECK(a4.pitch_class == 9 && std::abs(a4.cents) <= 2);
    CHECK(find_root(tone(hz(36), 1.5, 30, 0)).pitch_class == 0); // C2, a saw
    CHECK(find_root(tone(hz(31), 2, 30, 2)).pitch_class == 7);   // G1 dying away
    CHECK(find_root(tone(hz(21), 2, 1, 0)).pitch_class == 9);    // A0, the lowest
    const auto sharp = find_root(tone(hz(55.3), 1, 10, 0)), flat = find_root(tone(hz(54.55), 1, 10, 0)); // G3 30 cents up, 45 down
    CHECK(sharp.pitch_class == 7 && std::abs(sharp.cents - 30) <= 3 && flat.pitch_class == 7 && std::abs(flat.cents + 45) <= 3);
    std::vector<float> kick(66150); // an 808: from 150 Hz down onto F1 in 30 ms, then its tail
    double phase = 0;
    for (size_t n = 0; n < kick.size(); ++n) {
        const double t = double(n) / 44100;
        phase += 2 * M_PI * (hz(29) + (150 - hz(29)) * std::exp(-t / 0.03)) / 44100;
        kick[n] = float(0.8 * std::sin(phase) * std::exp(-2 * t));
    }
    CHECK(find_root(kick).pitch_class == 5);
    std::vector<float> noise(44100);
    uint32_t seed = 1;
    for (auto& v : noise) { seed = seed * 1664525u + 1013904223u; v = float(int32_t(seed)) / 4.3e9f; }
    std::vector<float> phrase, two;
    for (int note : {60, 64, 67, 71}) { const auto t = tone(hz(note), 0.4, 6, 1); phrase.insert(phrase.end(), t.begin(), t.end()); }
    for (int note : {60, 67, 60, 67}) { const auto t = tone(hz(note), 0.3, 6, 1); two.insert(two.end(), t.begin(), t.end()); }
    CHECK(!find_root(noise).found() && !find_root(phrase).found() && !find_root(two).found());
    CHECK(find_root(tone(hz(96), 1, 1, 0)).pitch_class == 0); // C7
    // Chord stabs: a minor chord never repeats exactly, so it is read as a chord, its root where the bass is.
    auto chord = [&](std::initializer_list<int> notes) {
        std::vector<float> x(66150);
        for (int note : notes) { const auto t = tone(hz(note), 1.5, 6, 0.5); for (size_t n = 0; n < x.size(); ++n) x[n] += t[n] / float(notes.size()); }
        return find_root(x);
    };
    CHECK(chord({57, 60, 64}).pitch_class == 9 && chord({48, 51, 55}).pitch_class == 0); // Am, Cm
    CHECK(chord({45, 60, 64, 67}).pitch_class == 9); // Am7 over its root, not C6
    CHECK(!find_root(std::vector<float>(44100)).found() && !find_root(tone(hz(60), 0.06, 5, 0)).found()); // silence, a blip

    // 16 Levels on a G sample in C minor: pad 1 plays it up to C, a chord's C4 and MIDI's C4 the same.
    auto project = std::make_unique<Project>();
    std::strcpy(project->samples.edit(0).path, "/card/Vox G.wav");
    project->samples.edit(0).root = 7;
    auto& kit = project->tracks[0];
    load_sound(kit, EngineKind::Sampler, 0);
    kit.pads[0].sample = 0;
    kit.levels = true; kit.levels_pad = 0; kit.levels_kind = uint8_t(LevelsKind::Scale);
    project->root = 0; project->scale = 1;
    CHECK(levels_root_shift(*project, kit) == -5 && kit_root_shift(*project, kit) == 5);
    CHECK(levels_of(*project, kit, 0).semitones == 5 + level_octave(kit) && levels_of(*project, kit, 2).semitones == 8 + level_octave(kit)); // C, Eb
    CHECK(levels_chord_semitones(60, levels_root_shift(*project, kit)) == 5);
    CHECK(levels_key(kit, 60, false, levels_root_shift(*project, kit)) == pad_note(int(sampler_first_note), 5));
    project->samples.edit(0).root = 6; // F#: up the nearest way
    CHECK(levels_root_shift(*project, kit) == 6 && kit_root_shift(*project, kit) == 6);
    for (int8_t none : {root_none, root_unread}) { project->samples.edit(0).root = none; CHECK(levels_root_shift(*project, kit) == 0); }
    std::strcpy(project->samples.edit(2).path, "/card/Keys/Piano.sfz"); // an instrument's zones have their own
    project->samples.edit(2).root = 7;
    CHECK(root_shift(*project, 2) == 0 && root_shift(*project, no_sample) == 0);

    // A KEYGROUP track of one sample in A: A3 (57) plays it as it is, the A nearest C4.
    std::strcpy(project->samples.edit(1).path, "/card/Bass A.wav");
    project->samples.edit(1).root = 9;
    auto& keys = project->tracks[1];
    keys.engine = EngineKind::Keygroup; keys.pads[0].sample = 1;
    CHECK(keyed_root(*project, keys) == 57);
    keys.pads[0].sample = 2;
    CHECK(keyed_root(*project, keys) == 60); // an instrument
    keys.pads[0].sample = 1;
    {
        SampleData data;
        data.frames = 44100;
        for (uint32_t n = 0; n < data.frames; ++n) data.left.push_back(0.4f * std::sin(float(2 * M_PI * 220.0 * n / 44100)));
        data.right = data.left;
        SamplerEngine::Table table {};
        table[1] = &data;
        auto engine = std::make_unique<SamplerEngine>();
        engine->set_samples(&table);
        PadSound pad;
        pad.sample = 1; pad.play = PadPlay::Gate; pad.release = 0;
        engine->set_pad(0, pad);
        engine->set_keyed(true);
        auto heard = [&](int key) { // the pitch a key plays, by its crossings
            engine->note_on(key, 127);
            std::vector<float> l(8192), r(8192);
            for (unsigned done = 0; done < l.size(); done += 128) engine->render_stereo(l.data() + done, r.data() + done, 128);
            engine->note_off(key);
            std::vector<float> rest(128), rest_r(128);
            for (unsigned k = 0; k < 40; ++k) engine->render_stereo(rest.data(), rest_r.data(), 128);
            unsigned crossings = 0;
            for (size_t n = 2001; n < l.size(); ++n) crossings += (l[n - 1] < 0) != (l[n] < 0);
            return crossings / 2.0f / float((l.size() - 2000) / 44100.0);
        };
        engine->set_keyed_root(keyed_root(*project, keys));
        CHECK(std::abs(heard(57) - 220) < 6 && std::abs(heard(69) - 440) < 10); // A3 as it is, A4 an octave up
        engine->set_keyed_root(60);
        CHECK(std::abs(heard(60) - 220) < 6); // as a C (a project from before)
    }

    // The project file keeps each note; a file from before has none: its samples play as C.
    std::strcpy(project->samples.edit(3).path, "/card/Kick.wav");
    project->samples.edit(3).root = root_none;
    std::strcpy(project->samples.edit(4).path, "/card/Take 1.wav"); // not read yet when saved: found once it is
    auto bank = std::make_unique<PatternBank>();
    const auto bytes = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(bytes, *back, *back_bank).empty());
    CHECK(back->samples[1].root == 9 && back->samples[3].root == root_none && back->samples[4].root == root_unread);
    auto older = bytes; // as an older build wrote it: SMPL without the note after the name
    for (size_t at = 8; at + 8 <= older.size();) {
        uint32_t n = 0;
        for (int k = 0; k < 4; ++k) n |= uint32_t(older[at + 4 + size_t(k)]) << (8 * k);
        if (!std::memcmp(&older[at], "SMPL", 4)) {
            older.erase(older.begin() + std::ptrdiff_t(at + 8 + n - 1));
            --n;
            for (int k = 0; k < 4; ++k) older[at + 4 + size_t(k)] = uint8_t(n >> (8 * k));
        }
        at += 8 + n;
    }
    CHECK(older.size() < bytes.size() && load_project(older, *back, *back_bank).empty());
    CHECK(back->samples[1].root == root_none && back->samples[4].root == root_none && std::string(back->samples[1].path) == "/card/Bass A.wav");
}

// A MIDI track's negative DELAY past the output's latency: the sequencer plays it ahead of the other tracks, so each
// note's MIDI goes that much before its frame is heard (an instrument on a computer, heard back over USB-C, lands
// with the rest). A launch waiting for the bar line is read ahead: its program before its notes, its notes once.
void midi_lookahead() {
    using K = MidiOutEvent::Kind;
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    for (auto& t : project->tracks) t.engine = EngineKind::None;
    load_sound(project->tracks[0], EngineKind::Midi, 0);
    project->tracks[0].params[midi_track::Channel] = 3;
    project->tempo = 1200;
    put(bank->at[0][0], 0, 60, 100);
    put(bank->at[0][0], 8, 62, 100); // step 8: tick 192, 44,100 frames in at 120 BPM
    put(bank->at[0][1], 0, 64, 100);
    bank->at[0][1].program = 9;
    EngineHost host;
    host.load(*project, *bank);
    host.set_audio_format(44100, 384);
    std::vector<int32_t> out(256);
    uint64_t rendered = 0;
    struct Out { MidiOutEvent e; uint64_t by; }; // by: the frames rendered when it came out
    std::vector<Out> got;
    auto run = [&](unsigned blocks) {
        for (unsigned n = 0; n < blocks; ++n) {
            host.render(out.data(), 128, 1);
            rendered += 128;
            MidiOutEvent e;
            while (host.take_midi(e)) got.push_back({e, rendered});
        }
    };
    host.send({Command::Type::Param, 0, 0, midi_track::Delay, -30}); // 1,323 frames: 939 past the latency, five ticks ahead
    run(2);
    got.clear();
    const uint64_t play_at = rendered;
    host.send({Command::Type::Play});
    run(400); // past step 8
    auto find = [&](K kind, uint8_t key) -> const Out* { for (auto& o : got) if (o.e.kind == kind && o.e.a == key) return &o; return nullptr; };
    const Out* second = find(K::NoteOn, 62);
    const uint64_t at = play_at + 44100 - 1323;
    CHECK(second && second->e.frame + 3 >= at && second->e.frame <= at + 3);
    CHECK(second && second->by <= second->e.frame + 384); // decided by the time it is to go (its frame less the latency)
    int count62 = 0;
    for (auto& o : got) count62 += o.e.kind == K::NoteOn && o.e.a == 62;
    CHECK(count62 == 1);
    // A launch on the next bar (88,200 frames, the pattern 4 bars long? its own length: one bar of 16 steps).
    got.clear();
    host.send({Command::Type::Pattern, 0, 0, 0, 1});
    run(800);
    const Out* launched = find(K::NoteOn, 64);
    const Out* program = find(K::Program, 8);
    CHECK(launched && program);
    if (launched && program) {
        size_t on = size_t(launched - got.data()), prog = size_t(program - got.data());
        CHECK(prog < on && program->e.frame <= launched->e.frame);
        uint64_t bar = 88200;
        CHECK((launched->e.frame + 1323 - play_at) % bar <= 3 || (launched->e.frame + 1323 - play_at) % bar >= bar - 3); // on a bar line, less the delay
    }
    int count64 = 0, programs = 0;
    for (auto& o : got) { count64 += o.e.kind == K::NoteOn && o.e.a == 64; programs += o.e.kind == K::Program; }
    CHECK(count64 >= 1 && programs == 1); // the program once, read ahead, not again at the bar
    // No DELAY: on its frame as before (heard then).
    host.send({Command::Type::Param, 0, 0, midi_track::Delay, 0});
    host.send({Command::Type::Stop});
    run(4);
    got.clear();
    const uint64_t again = rendered;
    host.send({Command::Type::Play});
    run(4);
    const Out* first = find(K::NoteOn, 64);
    CHECK(first && first->e.frame == again && !first->e.live);
}

void track_names() {
    // A name of its own (Tracks, Shift + Push): kept when the track's sound, plug-in or kit changes, saved in TNAM.
    Track t;
    t.name_from("KIT");
    CHECK(std::string(t.name) == "KIT" && !t.named);
    t.set_name("LEAD"); t.named = true;
    t.name_from("PIANO");
    load_sound(t, EngineKind::Sampler, 0);
    CHECK(std::string(t.name) == "LEAD");
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    project->tracks[1].engine = EngineKind::Sampler; project->tracks[1].set_name("DRUMS"); project->tracks[1].named = true;
    project->tracks[2].engine = EngineKind::Sampler; project->tracks[2].set_name("KIT");
    auto bytes = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    CHECK(load_project(bytes, *back, *bank).empty());
    CHECK(back->tracks[1].named && std::string(back->tracks[1].name) == "DRUMS" && !back->tracks[2].named);
    project->tracks[1].named = false; // none named: no TNAM, as older builds wrote
    const auto plain = save_project(*project, *bank);
    const std::string tag = "TNAM";
    CHECK(std::search(plain.begin(), plain.end(), tag.begin(), tag.end()) == plain.end());
    CHECK(std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end()) != bytes.end());
}

// Issue 24: slice hits, kit notes played on Chop while Seq Record ran (Event::slice). Each plays a slice of its pad's
// sample's chop as the chop is now, the kit keeping no pad for it: in the pattern, the file, the sampler, the host's
// sequencer and its chop table, and laid on free pads for the MPC 3 export (model/slice_hits.h).
void slice_hit_notes() {
    const uint8_t key = uint8_t(sampler_first_note + 4);
    // Two slices of one pad on one tick are two notes, in slice order; the same slice again replaces it.
    Pattern p;
    Event a; a.tick = 0; a.key = key; a.velocity = 100; a.slice = 3;
    Event b = a; b.slice = 1;
    CHECK(p.add(a) && p.add(b) && p.events.size() == 2 && p.events[0].slice == 1 && p.events[1].slice == 3);
    Event louder = a; louder.velocity = 50;
    CHECK(p.add(louder) && p.events.size() == 2 && p.events[1].velocity == 50);
    Event own = a; own.slice = 0;
    CHECK(p.add(own) && p.events.size() == 3 && p.events[0].slice == 0);
    CHECK(own != a); // the slice tells two notes apart (undo, the screen's sketches)
    // Step Edit's rewrite of a step keeps a hit's slice.
    Pattern one;
    one.add(b);
    Step st = one.step(0);
    st.notes[0].velocity = 90;
    one.set_step(0, st);
    CHECK(one.events.size() == 1 && one.events[0].slice == 1 && one.events[0].velocity == 90);
    // The note as the engine and the shell carry it: the slice + 1 over the cutoff, none on a 16-bit key.
    CHECK(note_slice(pad_note(int(key), 0, -1, 5)) == 5 && (pad_note(int(key), 0, -1, 5) & 0xffff) == key);
    CHECK(note_slice(pad_note(int(key), -3, 40, 0)) == 0 && note_slice(int(key)) == -1);
    CHECK(note_slice(int(int16_t(pad_note(int(key), -12)))) == -1); // a pad's key and semitones in 16 bits, sign and all
    // The last of 128 slices over the top cutoff and the lowest semitones: each part reads back, the note stays positive.
    const int last = pad_note(int(key), -12, 100, int(max_slices) - 1);
    CHECK(last > 0 && note_slice(last) == int(max_slices) - 1 && ((last >> 16) & 0x7f) == 101 && int8_t(last >> 8) == -12);
    CHECK(((pad_note(int(key), 0, 127, -1) >> 16) & 0x7f) == 101 && note_slice(pad_note(int(key), 0, 127, -1)) == -1);

    // The file: SLCE, by place in EVNT; a file without it plays each note's pad region.
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    auto& kit = project->tracks[2];
    load_sound(kit, EngineKind::Sampler, 0); // a kit's own values (its level), its pads cleared
    for (auto& pad : kit.pads) pad = PadSound {};
    for (unsigned n = 0; n < 4; ++n) kit.pads[n].sample = uint16_t(20 + n); // A01-A04 taken
    kit.pads[4].sample = 7; kit.pads[4].play = PadPlay::OneShot;          // A05: the chopped pad
    bank->at[2][1] = p;
    bank->at[2][1].add([&] { Event e = a; e.tick = 2 * sixteenth_ticks; e.slice = 9; return e; }()); // past the chop's end
    auto data = save_project(*project, *bank);
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[2][1] == bank->at[2][1]);
    const std::string tag(data.begin(), data.end());
    CHECK(tag.find("SLCE") != std::string::npos);
    CHECK(!any_slice_hits(*std::make_unique<PatternBank>()) && any_slice_hits(*bank));

    // 3b's count: hits on pads holding the sample, from a slice on.
    CHECK(slice_hits(*project, *bank, 7) == 3 && slice_hits(*project, *bank, 7, 2) == 2 && slice_hits(*project, *bank, 7, 3) == 1);
    CHECK(slice_hits(*project, *bank, 8) == 0 && sliced_samples(*project, *bank) == std::vector<uint16_t> {7});

    // The sampler: a slice hit sounds as TO PADS' pad over that slice; past the chop's end nothing; with no chop the
    // pad's own region; Chop's PadNote (play_pad) as the pattern's note.
    SampleData noise;
    noise.frames = 44100;
    noise.left.resize(44100); noise.right.resize(44100);
    uint32_t seed = 11;
    for (unsigned n = 0; n < 44100; ++n) {
        seed = seed * 1664525u + 1013904223u;
        noise.left[n] = noise.right[n] = float(int32_t(seed) >> 8) / float(1 << 23) * 0.5f;
    }
    SamplerEngine::Table table {};
    table[7] = &noise;
    Slices chop;
    chop.count = 3;
    chop.start[0] = 0; chop.start[1] = 10000; chop.start[2] = 20000;
    chop.stop[0] = 10000; chop.stop[1] = 20000; chop.stop[2] = 0;
    SamplerEngine::Chops chops {};
    chops[7] = &chop;
    const PadSound whole = kit.pads[4];
    auto render = [&](const PadSound& pad, int note, int pad_slice = -2) {
        SamplerEngine e;
        e.set_samples(&table);
        e.set_chops(&chops);
        e.set_pad(4, pad);
        if (pad_slice > -2) e.play_pad(4, 100, 0, -1, pad_slice);
        else e.note_on(note, 100);
        std::vector<float> out, l(128), r(128);
        for (unsigned block = 0; block < 120; ++block) {
            std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
            e.render_stereo(l.data(), r.data(), 128);
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
    };
    auto loudest = [](const std::vector<float>& v) { float m = 0; for (float x : v) m = std::max(m, std::fabs(x)); return m; };
    const auto hit = render(whole, pad_note(key, 0, -1, 1));
    CHECK(loudest(hit) > 0.05f && hit == render(slice_sound(whole, 10000, 20000, 0), key));
    CHECK(hit != render(whole, key));
    CHECK(loudest(render(whole, pad_note(key, 0, -1, 5))) == 0.0f);
    CHECK(render(whole, 0, 1) == hit);
    chops[7] = nullptr;
    CHECK(render(whole, pad_note(key, 0, -1, 1)) == render(whole, key));
    chops[7] = &chop;

    // A slice's own Reverse (Chop's Shift + Loop): that slice alone plays backwards; on a reversed pad it plays forwards.
    {
        PadSound back_pad = slice_sound(whole, 10000, 20000, 0);
        back_pad.reverse = true;
        const auto backwards = render(back_pad, key);
        CHECK(loudest(backwards) > 0.05f && backwards != hit);
        chop.reverse[1] = true;
        CHECK(render(whole, pad_note(key, 0, -1, 1)) == backwards && render(whole, 0, 1) == backwards);
        CHECK(render(whole, pad_note(key, 0, -1, 0)) == render(slice_sound(whole, 0, 10000, 0), key)); // the others as the pad
        PadSound reversed_pad = whole;
        reversed_pad.reverse = true;
        CHECK(render(reversed_pad, pad_note(key, 0, -1, 1)) == hit); // the pad's Reverse turned round again
        CHECK(chop_slice_sound(reversed_pad, chop, 0, 0).reverse && !chop_slice_sound(reversed_pad, chop, 1, 0).reverse);
        auto& kit_track = project->tracks[2];
        const uint8_t last_was = kit_track.last_pad;
        kit_track.last_pad = 4;
        CHECK(chop_kit_pad(kit_track, 7) == 4 && chop_kit_pad(kit_track, 9) == -1);
        CHECK(slice_heard_reversed(kit_track, 7, chop, 1) && !slice_heard_reversed(kit_track, 7, chop, 0));
        kit_track.pads[4].reverse = true;
        CHECK(!slice_heard_reversed(kit_track, 7, chop, 1) && slice_heard_reversed(kit_track, 7, chop, 0));
        kit_track.pads[4].reverse = false;
        kit_track.last_pad = 0; // A01 holds another sample: the first pad holding it
        CHECK(chop_kit_pad(kit_track, 7) == 4);
        kit_track.last_pad = last_was;
        chop.reverse[1] = false;
        // Chop's audition with no kit pad (as the file is) backwards too.
        auto heard = [&](bool reverse) {
            SamplerEngine e;
            e.set_samples(&table);
            e.audition(7, 10000, 20000, 100, -1, reverse);
            std::vector<float> out, l(128), r(128);
            for (unsigned block = 0; block < 40; ++block) {
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                e.render_stereo(l.data(), r.data(), 128);
                out.insert(out.end(), l.begin(), l.end());
            }
            return out;
        };
        CHECK(loudest(heard(true)) > 0.05f && heard(true) != heard(false));
        // The flag goes with its slice through a cut, a merge and a new chop; a cut in a reversed slice is reversed too.
        Slices s;
        s.whole(0, 40000);
        CHECK(s.add(10000, 40000) && s.add(20000, 40000) && !s.any_reversed());
        s.reverse[1] = true;
        Slices was = s;
        CHECK(!was.same(Slices {}) && s.same(was));
        was.reverse[1] = false;
        CHECK(!s.same(was));
        CHECK(s.add(15000, 40000) && s.count == 4 && !s.reversed(0) && s.reversed(1) && s.reversed(2) && !s.reversed(3));
        CHECK(s.add(5000, 40000) && s.reversed(2) && s.reversed(3) && !s.reversed(4)); // the slices after it move up
        s.remove(2); // slice 3 into slice 2 (not reversed): it keeps its own
        CHECK(s.count == 4 && !s.reversed(1) && s.reversed(2) && !s.reversed(3) && !s.reverse[4]);
        Slices again;
        again.whole(0, 40000);
        CHECK(again.add(15200, 40000) && again.add(30000, 40000));
        again.keep_reverse(s, hand_reach); // 15200 is 200 frames from the reversed slice's 15000
        CHECK(!again.reversed(0) && again.reversed(1) && !again.reversed(2));
        s.whole(0, 0);
        CHECK(!s.any_reversed());
    }

    // The host: the chop the project keeps, loaded; its pattern's slice hit as TO PADS' pad playing a plain note.
    project->chops.edit(7).slices = chop;
    project->chops.edit(7).to = 44100;
    auto laid_project = std::make_unique<Project>(*project);
    laid_project->tracks[2].pads[5] = slice_sound(whole, 10000, 20000, 0);
    auto beat = std::make_unique<PatternBank>(), laid_beat = std::make_unique<PatternBank>();
    beat->at[2][0].add([&] { Event e = a; e.slice = 2; return e; }());     // slice 2 (index 1)
    laid_beat->at[2][0].add([&] { Event e = a; e.slice = 0; e.key = uint8_t(key + 1); return e; }()); // A06, that slice
    auto play_host = [&](const Project& pr, const PatternBank& bk, EngineHost& host) {
        host.load(pr, bk);
        CHECK(host.offer_sample(7, &noise));
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256), all;
        for (unsigned block = 0; block < 60; ++block) { host.render(out.data(), 128, 1); all.insert(all.end(), out.begin(), out.end()); }
        return all;
    };
    auto host = std::make_unique<EngineHost>(), laid_host = std::make_unique<EngineHost>();
    const auto heard = play_host(*project, *beat, *host), laid_heard = play_host(*laid_project, *laid_beat, *laid_host);
    int32_t most = 0;
    for (auto v : heard) most = std::max(most, v < 0 ? -v : v);
    CHECK(most > 0 && heard == laid_heard);
    // A newer chop for the slot: swapped in at the next block, the old one handed back to be freed.
    Command swap {Command::Type::Chop, 0, 0, 7};
    swap.chop = new Slices(chop);
    CHECK(host->send(swap));
    std::vector<int32_t> out(256);
    host->render(out.data(), 128, 1);
    const Slices* old = nullptr;
    CHECK(host->take_retired_chop(old) && old && old != swap.chop && old->same(chop));
    delete old;
    const SampleData* gone = nullptr;
    while (host->take_retired(gone)) {}
    while (laid_host->take_retired(gone)) {}

    // The MPC 3 export: each pad and slice played laid once on the next free pad (POLY, the pad's sound over the
    // slice), its hits moved there as plain notes; a hit past the chop's end dropped.
    auto export_project = std::make_unique<Project>(*project);
    auto export_bank = std::make_unique<PatternBank>(*bank);
    const unsigned left = lay_slice_hits(*export_project, *export_bank, [&](unsigned s) -> const Slices& { return s == 7 ? chop : project->chops[s].slices; });
    const auto& moved = export_bank->at[2][1].events;
    CHECK(left == 0 && !any_slice_hits(*export_bank) && moved.size() == 3);
    const auto& laid = export_project->tracks[2].pads;
    CHECK(laid[5].start == 0 && laid[5].end == 10000 && laid[5].sample == 7);  // slice 1, hit first
    CHECK(laid[6].start == 20000 && laid[6].end == 0 && !laid[7].used());      // slice 3; slice 9 is past three: dropped
    CHECK(!laid[5].reverse && !laid[6].reverse);
    {
        auto back_project = std::make_unique<Project>(*project);
        auto back_bank = std::make_unique<PatternBank>(*bank);
        Slices backwards = chop;
        backwards.reverse[2] = true; // slice 3 reversed on its own: laid as a reversed pad
        CHECK(lay_slice_hits(*back_project, *back_bank, [&](unsigned) -> const Slices& { return backwards; }) == 0);
        CHECK(!back_project->tracks[2].pads[5].reverse && back_project->tracks[2].pads[6].reverse);
    }
    CHECK(moved[0].key == key && moved[1].key == key + 1 && moved[2].key == key + 2 && moved[2].velocity == 50);
    // No pad free: the hits play their pad's whole region, counted.
    auto full = std::make_unique<Project>(*project);
    for (unsigned n = 5; n < sampler_pads; ++n) full->tracks[2].pads[n].sample = 30;
    auto full_bank = std::make_unique<PatternBank>(*bank);
    CHECK(lay_slice_hits(*full, *full_bank, [&](unsigned) -> const Slices& { return chop; }) == 2 && full_bank->at[2][1].events.size() == 3);
}

// The mixer's model (melodee-sample docs/MIXER_PLAN.md, core step 1): every channel's strip, the tracks' outputs and
// the eight submixes go into the file and come back; a project that leaves them as they come writes none of it, and
// a file without them loads every strip flat and every track into the master.
void mixer_strips() {
    auto has = [](const std::vector<uint8_t>& bytes, const char* tag) {
        return std::search(bytes.begin(), bytes.end(), tag, tag + 4) != bytes.end();
    };
    auto plain = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    CHECK(plain->strip(0).flat() && plain->strip(strip_sub(7)).flat() && plain->subs[3].plain());
    auto bytes = save_project(*plain, *bank);
    CHECK(!has(bytes, "STRP") && !has(bytes, "OUTS") && !has(bytes, "SUBM"));
    // Strip numbers: tracks, buses and the master as their chains, then the input and the submixes.
    CHECK(&plain->strip(3) == &plain->tracks[3].strip && &plain->strip(bus_chain(2)) == &plain->buses[2].strip);
    CHECK(&plain->strip(master_chain) == &plain->master_strip && &plain->strip(strip_input) == &plain->input_strip);
    CHECK(&plain->strip(strip_sub(5)) == &plain->subs[5].strip && strip_channels == strip_sub(sub_count - 1) + 1);
    // Chain numbers: the submixes' after the Looper's (past chain_count), then the kit pads'.
    CHECK(&plain->chain(sub_chain(3)) == &plain->subs[3].fx && sub_chain(0) == chain_count && pad_chain(0, 0) == mix_chain_count);
    CHECK(&plain->chain(pad_chain(2, 5)) == &plain->tracks[2].pad_fx.at[5] && &plain->chain(loop_chain(1)) == &plain->loop_fx[1]);

    auto project = std::make_unique<Project>();
    auto& bass = project->tracks[2].strip;
    bass.trim = 35; bass.width = 80; bass.eq = {40, 60, 20, -50, -30, 0, 20, 0, -40};
    bass.comp_amount = 18; bass.comp_ratio = 40; bass.comp_speed = uint8_t(CompSpeed::Fast);
    bass.comp_key = fx_key_track(0); bass.comp_mix = 70; bass.comp_gain = 20;
    project->tracks[5].strip.eq_on = false; project->tracks[5].strip.eq[8] = 500; // past the range: read back at 12 dB
    project->tracks[5].strip.comp_on = false;
    project->tracks[1].mix.out = out_sub(1); project->tracks[3].mix.out = out_sub(1); project->tracks[0].mix.out = out_sub(0);
    project->buses[0].strip.width = 150;
    project->master_strip.eq[7] = 10;
    project->input_strip.trim = 60;
    auto& music = project->subs[1];
    music.mix.level = 90; music.mix.pan = -10; music.mix.sends = {20, 0, 0, 5}; music.mix.mute = true;
    std::snprintf(music.name, sizeof(music.name), "Music");
    music.fx.slots[0].kind = uint8_t(EffectKind::Echo);
    music.fx.slots[1].kind = uint8_t(EffectKind::Compressor); music.fx.slots[1].key = fx_key_sub(4); // keyed by another submix
    project->tracks[4].fx.slots[0].kind = uint8_t(EffectKind::Gate); project->tracks[4].fx.slots[0].key = fx_key_sub(1);
    project->tracks[6].strip.comp_amount = 10; project->tracks[6].strip.comp_key = fx_key_sub(7);
    music.strip.comp_amount = 6;
    project->subs[6].strip.trim = -20; // a submix whose strip alone is set
    project->buses[2].mute = true; project->input_wet = true;
    bytes = save_project(*project, *bank);
    CHECK(has(bytes, "STRP") && has(bytes, "OUTS") && has(bytes, "SUBM"));
    CHECK(mixer_left_out(*plain) == 0 && mixer_left_out(*project) > 0); // what an export cannot carry yet

    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(bytes, *back, *back_bank).empty());
    const auto& b = back->tracks[2].strip;
    CHECK(b.trim == 35 && b.width == 80 && b.eq_on && b.eq == bass.eq && b.comp_on);
    CHECK(b.comp_amount == 18 && b.comp_ratio == 40 && b.comp_speed == uint8_t(CompSpeed::Fast) && b.comp_key == fx_key_track(0));
    CHECK(b.comp_mix == 70 && b.comp_gain == 20 && !b.flat() && !b.eq_flat() && !b.comp_idle());
    CHECK(!back->tracks[5].strip.eq_on && back->tracks[5].strip.eq[8] == eq_range && !back->tracks[5].strip.comp_on);
    CHECK(back->tracks[0].strip.flat() && back->tracks[0].mix.out == out_sub(0));
    CHECK(back->tracks[1].mix.out == out_sub(1) && back->tracks[3].mix.out == out_sub(1) && back->tracks[4].mix.out == out_master);
    CHECK(back->buses[0].strip.width == 150 && back->master_strip.eq[7] == 10 && back->input_strip.trim == 60);
    CHECK(back->buses[2].mute && !back->buses[1].mute && back->input_wet);
    const auto& m = back->subs[1];
    CHECK(!std::strcmp(m.name, "Music") && m.mix.level == 90 && m.mix.pan == -10 && m.mix.mute && !m.mix.solo);
    CHECK(m.mix.sends[0] == 20 && m.mix.sends[3] == 5 && m.fx.count() == 2 && m.fx.slots[0].kind == uint8_t(EffectKind::Echo));
    CHECK(m.fx.slots[1].key == fx_key_sub(4) && back->tracks[4].fx.slots[0].key == fx_key_sub(1) && back->tracks[6].strip.comp_key == fx_key_sub(7));
    CHECK(m.strip.comp_amount == 6 && m.mix.out == out_master && !m.plain());
    CHECK(back->subs[6].plain() && back->subs[6].strip.trim == -20 && back->subs[0].plain() && back->subs[0].strip.flat());

    // A file from before the mixer: TRAK's mix alone. Loaded over a project that had strips, nothing of them is left.
    CHECK(load_project(save_project(*plain, *bank), *back, *back_bank).empty());
    for (unsigned n = 0; n < strip_channels; ++n) CHECK(back->strip(n).flat());
    for (const auto& t : back->tracks) CHECK(t.mix.out == out_master);
    for (const auto& s : back->subs) CHECK(s.plain());
    for (const auto& b : back->buses) CHECK(!b.mute);
    CHECK(!back->input_wet);
}

// The mixer's strip as it sounds (engine/channel_strip.h, core step 2): flat passes every sample untouched; a band
// lifts or cuts its octave and leaves the others; trim and the compressor's gain are plain gains; the compressor
// takes a loud tone down by its ratio over the threshold, hears its key in place of the channel when keyed, and does
// nothing at mix 0 or off; width 0 is mono; a change glides; the bands sit at their frequencies at 96 kHz too.
// The mixer, core step 3 (melodee-sample docs/MIXER_PLAN.md): a track's strip after its chain and before its level and
// pan, set from the project or a value at a time (StripValue); each side's meter and the compressor's reduction; a
// strip keyed by another track.
// The mixer's EQ picture (melodee-sample docs/MIXER_PLAN.md step 5): the strip picked has its sound after its strip
// copied into the spectrum ring each block; none picked, nothing is written. The ring wraps and reads oldest first.
void mixer_spectrum() {
    {
        auto ring = std::make_unique<ScopeRing>();
        std::vector<float> l(ScopeRing::size + 100), r(ScopeRing::size + 100);
        for (unsigned f = 0; f < l.size(); ++f) { l[f] = float(f); r[f] = float(f) + 2; }
        ring->put(l.data(), r.data(), unsigned(l.size()));
        float last[3];
        ring->latest(last, 3);
        const float n = float(l.size());
        CHECK(last[0] == n - 2 && last[1] == n - 1 && last[2] == n && ring->written() == l.size()); // (l + r) / 2: f + 1
    }
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0);
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    SampleData tone = tone_sample(1000);
    CHECK(host->offer_sample(0, &tone));
    std::vector<int32_t> out(256);
    auto play = [&](unsigned blocks) { for (unsigned n = 0; n < blocks; ++n) host->render(out.data(), 128, 1); };
    host->set_spectrum_tap(0);
    play(2);
    host->send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note), 100});
    play(20);
    std::vector<float> seen(1024);
    host->spectrum().latest(seen.data(), 1024);
    float rms = 0;
    unsigned crossings = 0;
    for (unsigned f = 0; f < seen.size(); ++f) {
        rms += seen[f] * seen[f];
        if (f && (seen[f - 1] < 0) != (seen[f] < 0)) ++crossings;
    }
    rms = std::sqrt(rms / float(seen.size()));
    const float expected = 2 * 1000.0f * 1024 / float(dsp::sample_rate()); // a 1 kHz tone crosses zero twice a cycle
    CHECK(rms > 0.1f && std::fabs(float(crossings) - expected) <= 3);
    host->set_spectrum_tap(-1);
    const uint32_t before = host->spectrum().written();
    play(5);
    CHECK(host->spectrum().written() == before);
    host->set_spectrum_tap(int(strip_channels)); // out of range: none
    play(1);
    CHECK(host->spectrum().written() == before);
}

void mixer_tracks() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0); tone_kit(*project, 1);
    project->tracks[1].strip.eq[2] = 30; project->tracks[1].strip.comp_ratio = 80;
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    SampleData tone = tone_sample(1000);
    CHECK(host->offer_sample(0, &tone));
    CHECK(host->strip_values(1).eq[2] == 30 && host->strip_values(1).comp_ratio == 80 && host->strip_values(0).flat());
    host->send({Command::Type::RecordSource, uint8_t(InputSource::Track), 0, 0, 1}); // track 1 as the mix takes it
    std::vector<int32_t> out(256);
    float most_l = 0, most_r = 0; // the Track source's peaks over the last play()
    auto play = [&](unsigned blocks) {
        most_l = most_r = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            host->render(out.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) {
                most_l = std::max(most_l, std::fabs(host->print_left()[f])); most_r = std::max(most_r, std::fabs(host->print_right()[f]));
            }
        }
        return std::max(most_l, most_r);
    };
    auto set = [&](unsigned t, StripPart part, int value) {
        CHECK(host->send({Command::Type::StripValue, uint8_t(t), 0, uint16_t(part), int16_t(value)}));
    };
    auto note = [&](unsigned t, bool on) {
        host->send({on ? Command::Type::NoteOn : Command::Type::NoteOff, uint8_t(t), 0, uint16_t(sampler_first_note), 100});
    };
    play(2);
    note(0, true);
    const float flat = play(10);
    CHECK(flat > 0.2f && host->strip_meter(0).left > 0.2f && host->strip_meter(0).right > 0.2f && host->strip_meter(0).reduction == 0);
    // A band at 1 kHz lifted 12 dB (it glides there in 0.1 s): about four times as loud.
    set(0, eq_part(4), 120);
    play(30);
    const float lifted = play(5);
    CHECK(host->strip_values(0).eq[4] == 120 && lifted > flat * 3.5f && lifted < flat * 4.2f);
    set(0, eq_part(4), 900); // out of range: held at +12 dB
    CHECK(play(2) > 0 && host->strip_values(0).eq[4] == 120);
    set(0, eq_part(4), 0);
    play(30);
    // The compressor, then the level after it: turned down, the sound drops and the reduction stays.
    set(0, StripPart::Amount, 30);
    const float pressed = (play(40), play(5));
    const float reduced = host->strip_meter(0).reduction;
    CHECK(reduced > 3 && pressed < flat * 0.8f);
    host->send({Command::Type::Level, 0, 0, 0, 50}); // level_gain(50): a quarter
    play(10);
    const float quieter = play(5);
    CHECK(std::fabs(host->strip_meter(0).reduction - reduced) < 0.5f && std::fabs(quieter / pressed - 0.25f) < 0.02f);
    // The pan last of all: hard left, nothing on the right.
    host->send({Command::Type::Pan, 0, 0, 0, -50});
    play(4);
    play(5);
    CHECK(std::fabs(most_l / quieter - 1.4142f) < 0.05f && most_r == 0.0f); // a side at the centre: 0.71 of a hard pan
    host->send({Command::Type::Pan, 0, 0, 0, 0});
    host->send({Command::Type::Level, 0, 0, 0, 100});
    // Keyed by track 2: it hears track 2, silent at first, then playing.
    set(0, StripPart::Key, fx_key_track(1));
    note(0, false); note(0, true); // the tone again from its start
    play(200); // the release (Medium: about 43 dB a second at 4:1) from the 13 dB it took above
    CHECK(host->strip_meter(0).reduction < 0.5f && play(5) > flat * 0.9f);
    note(1, true);
    play(20);
    CHECK(host->strip_meter(0).reduction > 3 && play(5) < flat * 0.8f);
    set(0, StripPart::CompOn, 0); // off: its values kept, the sound as it comes
    play(20);
    CHECK(host->strip_values(0).comp_amount == 30 && play(5) > flat * 0.9f);
    std::puts("mixer tracks ok");
}

// The mixer, core step 4: submixes. A track sent to one plays through its chain, strip, level and pan into the master
// (and as much as its sends into the buses); mute and solo (a submix's solo solos its tracks, and counts only with a
// track in it); a track moved back to the master; a strip keyed by a submix.
void mixer_submixes() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0); tone_kit(*project, 1);
    project->tracks[0].mix.out = out_sub(2);
    project->subs[2].mix.level = 64; // level_gain: 0.4096
    project->subs[5].mix.solo = true; // nothing in it: its solo does not count
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    SampleData tone = tone_sample(1000);
    CHECK(host->offer_sample(0, &tone));
    std::vector<int32_t> out(256);
    auto play = [&](unsigned blocks) { // the master's peak over the last blocks
        float most = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            host->render(out.data(), 128, 1);
            for (auto v : out) most = std::max(most, std::fabs(v / 2147483648.0f));
        }
        return most;
    };
    auto sub = [&](unsigned n, uint16_t part, int value) { CHECK(host->send({Command::Type::SubMix, uint8_t(n), 0, part, int16_t(value)})); };
    auto note = [&](unsigned t, bool on) {
        host->send({on ? Command::Type::NoteOn : Command::Type::NoteOff, uint8_t(t), 0, uint16_t(sampler_first_note), 100});
    };
    auto mute = [&](unsigned t, bool on) { host->send({Command::Type::Mute, uint8_t(t), 0, 0, int16_t(on)}); };
    play(2);
    // Track 2 alone, straight to the master: the level a track plays at. Both notes are held from here on (the
    // tone lasts a second); track 2 is muted to hear the submix alone.
    note(1, true);
    const float direct = play(10);
    CHECK(direct > 0.2f && host->strip_meter(strip_sub(2)).left == 0);
    mute(1, true);
    // Track 1 through submix 3 at level 64.
    note(0, true);
    play(10);
    const float through = play(10);
    CHECK(std::fabs(through / direct - 0.4096f) < 0.01f && std::fabs(host->strip_meter(strip_sub(2)).left - through) < 0.01f && host->meter(0) > 0.2f);
    // A send: bus B (no chain, its return at 70) takes half of it, in phase.
    sub(2, Command::SubSend0 + 1, 50);
    play(10);
    CHECK(std::fabs(play(5) / through - 1.35f) < 0.02f);
    sub(2, Command::SubSend0 + 1, 0);
    // Muted: nothing of it, though the track still plays (its own meter).
    sub(2, Command::SubMute, 1);
    play(10);
    CHECK(play(5) < 1e-6f && host->meter(0) > 0.2f);
    sub(2, Command::SubMute, 0);
    // Solo: submix 3's solos track 1 in it; track 2, straight to the master, is silenced (heard, the two would
    // peak at 0.16 at least).
    mute(1, false);
    sub(2, Command::SubSolo, 1);
    play(10);
    CHECK(std::fabs(play(5) / through - 1) < 0.02f);
    sub(2, Command::SubSolo, 0);
    // Track 1's own solo: its submix plays it on, track 2 silenced.
    host->send({Command::Type::Solo, 0, 0, 0, 1});
    play(10);
    CHECK(std::fabs(play(5) / through - 1) < 0.02f);
    host->send({Command::Type::Solo, 0, 0, 0, 0});
    play(10);
    // Track 2's strip keyed by submix 3: ducked while track 1 plays through it.
    host->send({Command::Type::StripValue, 1, 0, uint16_t(StripPart::Key), fx_key_sub(2)});
    host->send({Command::Type::StripValue, 1, 0, uint16_t(StripPart::Amount), 40});
    play(30);
    CHECK(host->strip_meter(1).reduction > 3);
    // Track 1 back to the master, crossing over in one block: at its own level again.
    mute(1, true);
    host->send({Command::Type::TrackOut, 0, 0, 0, out_master});
    play(10);
    CHECK(std::fabs(play(5) / direct - 1) < 0.02f && host->strip_meter(strip_sub(2)).left < through);
    // Delay compensation: track 1 through a submix whose chain holds a plug-in 200 frames late, track 2 straight to
    // the master, both from the same tick: each heard alone, both start on the same frame (track 2 waits).
    auto first_heard = [&](unsigned silent) {
        auto p = std::make_unique<Project>();
        auto b = std::make_unique<PatternBank>();
        tone_kit(*p, 0); tone_kit(*p, 1);
        p->tracks[0].mix.out = out_sub(4);
        p->subs[4].fx.slots[0] = {uint8_t(EffectKind::Plugin), false, {0, 0, 0, 0, 0, 0}};
        p->tracks[silent].mix.mute = true;
        put(b->at[0][0], 0, uint8_t(sampler_first_note), 100); put(b->at[1][0], 0, uint8_t(sampler_first_note), 100);
        auto h = std::make_unique<EngineHost>();
        h->load(*p, *b);
        LatePlugin slow;
        slow.instrument = false; slow.late = 200;
        CHECK(h->offer_sample(0, &tone) && h->offer_plugin(0, &slow));
        h->render(out.data(), 128, 1); // the plug-in taken in
        h->send({Command::Type::Play});
        for (unsigned n = 0; n < 40; ++n) {
            h->render(out.data(), 128, 1);
            for (unsigned f = 0; f < 128; ++f) if (out[2 * f] != 0) return int(n * 128 + f);
        }
        return -1;
    };
    const int via_sub = first_heard(1), direct_first = first_heard(0);
    CHECK(via_sub > 200 && via_sub == direct_first);
    std::puts("mixer submixes ok");
}

// The mixer, core step 5: a bus's strip after its chain, before its return, and its mute; the master's strip before
// its chain, with no compressor of its own; the input's strip where Monitor hears it, and takes before it (dry) or
// after it (wet). Their meters.
void mixer_buses_input() {
    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0);
    project->tracks[0].mix.sends[0] = 100; // into bus A (no chain, its return at 70)
    project->buses[0].mute = true;
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    SampleData tone = tone_sample(1000);
    CHECK(host->offer_sample(0, &tone));
    std::vector<int32_t> out(256), in(256);
    const int32_t* input = nullptr;
    auto play = [&](unsigned blocks) { // the master's peak over these blocks
        float most = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            host->render(out.data(), 128, 1, input);
            for (auto v : out) most = std::max(most, std::fabs(v / 2147483648.0f));
        }
        return most;
    };
    auto set = [&](unsigned n, StripPart part, int value) {
        CHECK(host->send({Command::Type::StripValue, uint8_t(n), 0, uint16_t(part), int16_t(value)}));
    };
    play(2);
    host->send({Command::Type::NoteOn, 0, 0, uint16_t(sampler_first_note), 100});
    play(10);
    const float direct = play(5); // bus A muted (from the file)
    CHECK(direct > 0.2f && host->strip_meter(bus_chain(0)).left < 1e-6f);
    host->send({Command::Type::BusMute, 0, 0, 0, 0});
    play(10);
    CHECK(std::fabs(play(5) / direct - 1.7f) < 0.02f && std::fabs(host->strip_meter(bus_chain(0)).left / direct - 0.7f) < 0.02f);
    set(bus_chain(0), StripPart::Trim, -60); // -6 dB on the bus, before its return
    play(30);
    CHECK(std::fabs(play(5) / direct - (1 + 0.7f * 0.501f)) < 0.02f);
    host->send({Command::Type::BusMute, 0, 0, 0, 1});
    // The master's strip: +6 dB. Its compressor is the Compressor: a value for it is not taken.
    set(master_chain, StripPart::Trim, 60);
    set(master_chain, StripPart::Amount, 30);
    play(30);
    const float master = play(5);
    CHECK(std::fabs(master / direct - 1.995f) < 0.03f && host->strip_values(master_chain).comp_amount == 0);
    CHECK(std::fabs(host->strip_meter(master_chain).left - master) < 0.01f);
    set(master_chain, StripPart::Trim, 0);
    host->send({Command::Type::Mute, 0, 0, 0, 1});
    play(30);
    // The input: the rear inputs at a quarter, monitored (a centred pad's pan law).
    for (unsigned f = 0; f < 128; ++f) in[2 * f] = in[2 * f + 1] = int32_t(0.25f * std::sin(2 * 3.14159265f * 1000.0f * float(f) / 128.0f * 4) * 2147483647.0f);
    input = in.data();
    host->send({Command::Type::RecordSource, uint8_t(InputSource::Rear), 0, 0, 0});
    host->send({Command::Type::Monitor, 0, 0, 0, 1});
    play(20);
    const float monitored = play(5);
    CHECK(std::fabs(monitored - 0.25f * 0.7071f) < 0.01f && std::fabs(host->in_level(0) - 0.25f) < 0.01f);
    set(strip_input, StripPart::Trim, 60); // +6 dB: Monitor hears it; recorded dry, the take (and the meter) do not
    play(30);
    CHECK(std::fabs(play(5) / monitored - 1.995f) < 0.03f && std::fabs(host->in_level(0) - 0.25f) < 0.01f);
    CHECK(std::fabs(host->strip_meter(strip_input).left - 0.5f) < 0.02f);
    host->send({Command::Type::InputWet, 0, 0, 0, 1}); // wet: they do
    play(5);
    CHECK(std::fabs(host->in_level(0) - 0.5f) < 0.02f);
    std::puts("mixer buses input ok");
}

// The mixer, core step 6: Motion on a track's strip. A lane a value but the switches and the key; a lane stands where
// the strip is set; a print leaves the lanes live; a file keeps them in MOTS (older builds skip it); played, a lane
// moves the strip, and Stop puts it back as set (a value set meanwhile counting).
void mixer_motion() {
    CHECK(motion_strip_count == 16 && motion_strip_base == motion_mix_end && motion_strip(StripPart::Trim) == int(motion_strip_base));
    CHECK(motion_strip(StripPart::Eq4) == int(motion_strip_base) + 6 && motion_strip(StripPart::Gain) == int(motion_strip_base) + 15);
    CHECK(motion_strip(StripPart::EqOn) < 0 && motion_strip(StripPart::CompOn) < 0 && motion_strip(StripPart::Key) < 0);
    CHECK(!is_strip_lane(motion_strip_base - 1) && !is_strip_lane(motion_strip_base + 16) && !is_mix_lane(motion_strip_base));
    for (unsigned n = 0; n < motion_strip_count; ++n) CHECK(motion_strip(strip_lane_part(motion_strip_base + n)) == int(motion_strip_base + n));

    auto project = std::make_unique<Project>();
    auto bank = std::make_unique<PatternBank>();
    tone_kit(*project, 0);
    project->tracks[0].strip.eq[4] = -30; project->tracks[0].strip.comp_ratio = 60;
    const unsigned eq_lane = unsigned(motion_strip(StripPart::Eq4)), ratio_lane = unsigned(motion_strip(StripPart::Ratio));
    CHECK(lane_own(*project, 0, eq_lane, 0) == -30 && lane_own(*project, 0, ratio_lane, 0) == 60);
    auto& pattern = bank->at[0][0];
    put(pattern, 0, uint8_t(sampler_first_note), 100);
    pattern.write_motion(point(0, eq_lane, 120));
    pattern.write_motion(point(4, motion_level, 80));
    pattern.write_motion(point(8, ratio_lane, 150));

    // The file: the strip's points in MOTS, the rest in MOTK, together again on load.
    auto has = [](const std::vector<uint8_t>& bytes, const char* tag) { return std::search(bytes.begin(), bytes.end(), tag, tag + 4) != bytes.end(); };
    auto bytes = save_project(*project, *bank);
    CHECK(has(bytes, "MOTS") && has(bytes, "MOTK"));
    auto back = std::make_unique<Project>();
    auto back_bank = std::make_unique<PatternBank>();
    CHECK(load_project(bytes, *back, *back_bank).empty());
    const auto& m = back_bank->at[0][0].motion;
    CHECK(m.size() == 3 && m[0].param == eq_lane && m[0].value == 120 && m[1].param == motion_level && m[2].param == ratio_lane);
    {
        auto lone = std::make_unique<PatternBank>();
        lone->at[2][5].write_motion(point(3, eq_lane, -60)); // a pattern with the strip's points alone
        auto b = save_project(*project, *lone);
        CHECK(has(b, "MOTS") && load_project(b, *back, *back_bank).empty() && back_bank->at[2][5].motion.size() == 1);
    }
    // A print: the strip's lanes stay live, as the mix's.
    {
        auto p = std::make_unique<Project>(*project);
        auto b = std::make_unique<PatternBank>(*bank);
        print_project(*p, *b, 0, 0);
        CHECK(b->at[0][0].motion.empty());
    }

    // Played: the lanes move the strip; Stop puts it back as set.
    auto host = std::make_unique<EngineHost>();
    host->load(*project, *bank);
    SampleData tone = tone_sample();
    CHECK(host->offer_sample(0, &tone));
    std::vector<int32_t> out(256);
    auto play = [&](unsigned blocks) { for (unsigned n = 0; n < blocks; ++n) host->render(out.data(), 128, 1); };
    play(2);
    CHECK(host->strip_values(0).eq[4] == -30);
    host->send({Command::Type::Play});
    play(4);
    CHECK(host->strip_values(0).eq[4] == 120 && host->strip_values(0).comp_ratio == 60);
    play(unsigned(44100 / 128)); // past step 9 at 120 BPM
    CHECK(host->strip_values(0).comp_ratio == 150);
    host->send({Command::Type::StripValue, 0, 0, uint16_t(StripPart::Trim), 30}); // set while it plays: kept
    host->send({Command::Type::Stop});
    play(2);
    const Strip& now = host->strip_values(0);
    CHECK(now.eq[4] == -30 && now.comp_ratio == 60 && now.trim == 30);
    std::puts("mixer motion ok");
}

void channel_strip_sound() {
    const float rate_was = dsp::sample_rate();
    constexpr unsigned block = 128;
    // The strip on a sine at `hz` and `amp` (and the key on another at `key_amp`, when given) for `seconds`; the
    // gain of the last quarter second against the sine's own, in dB.
    auto run = [&](ChannelStrip& s, float hz, float amp, float seconds = 1.0f, float key_amp = -1) {
        const float rate = dsp::sample_rate();
        const unsigned total = unsigned(seconds * rate), from = total - unsigned(rate / 4);
        std::vector<float> l(block), r(block), kl(block), kr(block);
        double sum = 0;
        unsigned counted = 0;
        for (unsigned at = 0; at < total; at += block) {
            for (unsigned f = 0; f < block; ++f) {
                double t = double(at + f) / rate;
                l[f] = r[f] = float(amp * std::sin(2 * M_PI * hz * t));
                kl[f] = kr[f] = float(key_amp * std::sin(2 * M_PI * 1000 * t));
            }
            s.process(l.data(), r.data(), block, key_amp >= 0 ? kl.data() : nullptr, key_amp >= 0 ? kr.data() : nullptr);
            for (unsigned f = 0; f < block; ++f) if (at + f >= from && at + f < total) { sum += double(l[f]) * l[f]; ++counted; }
        }
        double rms = std::sqrt(sum / std::max(counted, 1u));
        return float(20 * std::log10(std::max(rms, 1e-9) / (amp / std::sqrt(2.0))));
    };
    auto made = [](const Strip& v) { auto s = std::make_unique<ChannelStrip>(); s->set(v); s->reset(); return s; };
    for (float rate : {44100.0f, 96000.0f}) {
        dsp::device_rate = rate;
        // Flat: nothing done, every sample as it was.
        {
            ChannelStrip unset;
            CHECK(unset.idle());
            auto s = made(Strip {});
            CHECK(s->idle());
            std::vector<float> l(block), r(block);
            for (unsigned f = 0; f < block; ++f) { l[f] = std::sin(f * 0.1f); r[f] = -l[f] * 0.5f; }
            auto l0 = l, r0 = r;
            s->process(l.data(), r.data(), block);
            CHECK(l == l0 && r == r0 && s->reduction_db() == 0);
        }
        // A band: its octave lifted or cut, two octaves off nearly untouched, the far end untouched.
        {
            Strip v;
            v.eq[4] = 60; // 1 kHz, +6 dB
            auto s = made(v);
            CHECK(std::fabs(run(*s, 1000, 0.25f) - 6) < 0.2f);
            s = made(v);
            CHECK(std::fabs(run(*s, 4000, 0.25f)) < 0.6f && std::fabs(run(*s, 63, 0.25f)) < 0.1f);
            v.eq = {}; v.eq[1] = -120; // 125 Hz, -12 dB
            s = made(v);
            CHECK(std::fabs(run(*s, 125, 0.25f) + 12) < 0.3f);
            // EQ off: the bands glide back to nothing, the strip goes idle, their gains kept.
            v.eq_on = false;
            s->set(v);
            CHECK(std::fabs(run(*s, 125, 0.25f)) < 0.05f && s->idle() && s->values().eq[1] == -120);
        }
        // Trim and the compressor's gain (no compression at amount 0): plain gains; off, none.
        {
            Strip v;
            v.trim = 60;
            auto s = made(v);
            CHECK(std::fabs(run(*s, 440, 0.1f) - 6) < 0.05f);
            v = Strip {}; v.comp_gain = 60;
            s = made(v);
            CHECK(std::fabs(run(*s, 440, 0.1f) - 6) < 0.05f && s->reduction_db() == 0);
            v.comp_on = false;
            s->set(v);
            CHECK(std::fabs(run(*s, 440, 0.1f)) < 0.05f && s->idle());
        }
        // The compressor: -6 dB peaks, threshold -20 dB, 4:1: the peaks come out at -16.5 dB, 10.5 dB down.
        {
            Strip v;
            v.comp_amount = 20; v.comp_ratio = 40;
            auto s = made(v);
            float g = run(*s, 1000, 0.5f);
            CHECK(std::fabs(g + 10.5f) < 1.0f && std::fabs(s->reduction_db() - 10.5f) < 1.0f);
            s = made(v);
            CHECK(std::fabs(run(*s, 1000, 0.05f)) < 0.05f); // -26 dB peaks: under the threshold
            v.comp_mix = 0;
            s = made(v);
            CHECK(std::fabs(run(*s, 1000, 0.5f)) < 0.05f && s->reduction_db() == 0);
            // Keyed: a quiet channel taken down by its loud key; a loud channel with a silent key left alone.
            v.comp_mix = 100;
            s = made(v);
            CHECK(std::fabs(run(*s, 300, 0.05f, 1.0f, 0.5f) + 10.5f) < 1.0f);
            s = made(v);
            CHECK(std::fabs(run(*s, 300, 0.5f, 1.0f, 0.0f)) < 0.05f);
            // Each speed comes to the same reduction on a steady tone.
            for (unsigned speed = 0; speed < unsigned(CompSpeed::Count); ++speed) {
                v.comp_speed = uint8_t(speed);
                s = made(v);
                CHECK(std::fabs(run(*s, 1000, 0.5f) + 10.5f) < 1.2f);
            }
        }
        // Width: 0 is mono, 100 as it is.
        {
            Strip v;
            v.width = 0;
            auto s = made(v);
            std::vector<float> l(block), r(block, 0.0f);
            for (unsigned f = 0; f < block; ++f) l[f] = std::sin(f * 0.05f);
            s->process(l.data(), r.data(), block);
            bool mono = true;
            for (unsigned f = 0; f < block; ++f) mono &= std::fabs(l[f] - r[f]) < 1e-6f;
            CHECK(mono && std::fabs(l[40] - std::sin(40 * 0.05f) * 0.5f) < 1e-5f);
        }
        // A change glides: +12 dB on 1 kHz set at once has moved under 1 dB after one block, and is all there by 0.5 s.
        {
            auto s = made(Strip {});
            Strip v;
            v.eq[4] = 120;
            s->set(v);
            std::vector<float> l(block), r(block);
            for (unsigned f = 0; f < block; ++f) l[f] = r[f] = 0.25f * float(std::sin(2 * M_PI * 1000 * f / rate));
            auto l0 = l;
            s->process(l.data(), r.data(), block);
            double in = 0, out = 0;
            for (unsigned f = 0; f < block; ++f) { in += double(l0[f]) * l0[f]; out += double(l[f]) * l[f]; }
            CHECK(10 * std::log10(out / in) < 1.0);
            CHECK(std::fabs(run(*s, 1000, 0.25f, 0.5f) - 12) < 0.3f);
        }
    }
    dsp::device_rate = rate_was;
}

} // namespace


// Elektron's sequencing: per track speed x1/64 .. x4, per track direction (FWD, REV, RND), per step PLAY (the way
// the sample plays), SOUND (a sample of its own) and SLICE RND.
void elektron_sequencing() {
    // Speeds in order, x1 in the middle; the five a file before kept read as they were, today's as the nearest.
    CHECK(speed_count == 11 && speed_of(speed_normal).num == 1 && speed_of(speed_normal).den == 1);
    CHECK(speed_of(0).den == 64 && speed_of(speed_count - 1).num == 4);
    for (unsigned s = 1; s < speed_count; ++s)
        CHECK(double(speed_of(s).num) / speed_of(s).den > double(speed_of(s - 1).num) / speed_of(s - 1).den);
    CHECK(speed_of_legacy(0) == 5 && speed_of_legacy(2) == speed_normal && speed_of_legacy(4) == 9 && speed_of_legacy(7) == speed_normal);
    for (unsigned legacy = 0; legacy < 5; ++legacy) CHECK(legacy_speed(speed_of_legacy(legacy)) == legacy);
    CHECK(legacy_speed(0) == 2 && legacy_speed(10) == 2);
    { // x1/64: a note longer than any pattern as written still plays its whole length
        Pattern p;
        p.length = 4 * 16 * sixteenth_ticks;
        put(p, 0, 60, 100, 64);
        const Pattern slow = speed_pattern(p, 0);
        CHECK(slow.length == p.length * 64 && slow.events[0].length == p.events[0].length * 64);
    }
    // The order of a pass's steps: every step once, another order for another seed.
    for (uint32_t count : {1u, 2u, 3u, 7u, 16u, 100u, 8192u}) {
        std::vector<int> seen(count, 0);
        for (uint32_t k = 0; k < count; ++k) ++seen[shuffled_step(k, count, 1234)];
        for (int n : seen) CHECK(n == 1);
    }
    {
        bool other = false;
        for (uint32_t k = 0; k < 16; ++k) other |= shuffled_step(k, 16, direction_seed(0, 0)) != shuffled_step(k, 16, direction_seed(0, 1));
        CHECK(other && direction_seed(0, 3) == direction_seed(0, 3) && direction_seed(0, 3) != direction_seed(1, 3));
    }
    // A directed loop's ticks: REV plays the last step first, a note keeping its place in its step; over a pass every
    // tick of the pattern is played once, at any speed, Q and length (a part step at the end included).
    {
        StepClock clock;
        const uint32_t end = 16 * sixteenth_ticks;
        auto first = directed_tick(0, end, clock, PlayDirection::Reverse, 0);
        CHECK(first.from == 15 * sixteenth_ticks && first.to == first.from + 1 && first.step == 15);
        auto later = directed_tick(sixteenth_ticks + 7, end, clock, PlayDirection::Reverse, 0);
        CHECK(later.from == 14 * sixteenth_ticks + 7 && later.step == 14);
        auto last = directed_tick(end - 1, end, clock, PlayDirection::Reverse, 0);
        CHECK(last.from == sixteenth_ticks - 1 && last.to == sixteenth_ticks);
        auto fwd = directed_tick(123, end, clock, PlayDirection::Forward, 0);
        CHECK(fwd.from == 123 && fwd.to == 124);
    }
    for (unsigned speed : {0u, 4u, 6u, 7u, 8u, 10u})
        for (Division q : {Division::Sixteenth, Division::SixteenthTriplet, Division::Quarter})
            for (uint32_t written : {16u * sixteenth_ticks, 1000u, 3u * sixteenth_ticks + 50u})
                for (PlayDirection d : {PlayDirection::Reverse, PlayDirection::Random}) {
                    StepClock clock;
                    clock.q = division_ticks(q);
                    clock.speed = speed_of(speed);
                    const uint32_t end = uint32_t(played_ticks(written, speed));
                    std::vector<uint8_t> heard(end, 0);
                    for (uint32_t at = 0; at < end; ++at) {
                        auto here = directed_tick(at, end, clock, d, 99);
                        CHECK(here.from <= here.to && here.to <= end);
                        for (uint32_t k = here.from; k < here.to; ++k) ++heard[k];
                    }
                    for (auto n : heard) CHECK(n == 1);
                }
    // The engine: a MIDI track's four notes, backward, then shuffled (each once a pass), then forward again.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        load_sound(project->tracks[0], EngineKind::Midi, 0);
        project->tracks[0].params[midi_track::Port] = midi_track::UsbC; project->tracks[0].params[midi_track::Channel] = 1;
        project->tracks[0].direction = uint8_t(PlayDirection::Reverse);
        project->tempo = 1200;
        auto& p = bank->at[0][0];
        p.length = 4 * sixteenth_ticks;
        for (unsigned s = 0; s < 4; ++s) put(p, s, uint8_t(60 + s), 100);
        EngineHost host;
        host.load(*project, *bank);
        host.set_audio_format(44100, 384);
        host.send({Command::Type::Play});
        std::vector<int32_t> out(256);
        auto keys = [&](unsigned blocks) {
            std::vector<uint8_t> on;
            for (unsigned n = 0; n < blocks; ++n) {
                host.render(out.data(), 128, 1);
                MidiOutEvent e;
                while (host.take_midi(e)) if (e.kind == MidiOutEvent::Kind::NoteOn) on.push_back(e.a);
            }
            return on;
        };
        auto pass = keys(172); // four steps of 5512.5 frames: 22050
        CHECK(pass.size() >= 4 && pass[0] == 63 && pass[1] == 62 && pass[2] == 61 && pass[3] == 60);
        host.send({Command::Type::TrackDirection, 0, 0, 0, int16_t(PlayDirection::Random)});
        auto shuffled = keys(172 * 4);
        CHECK(shuffled.size() >= 12);
        for (size_t from = 4; from + 4 <= 12; from += 4) { // whole passes after the one under way: each key once
            std::vector<uint8_t> one(shuffled.begin() + long(from), shuffled.begin() + long(from + 4));
            std::sort(one.begin(), one.end());
            CHECK(one == (std::vector<uint8_t> {60, 61, 62, 63}));
        }
        CHECK(host.dropped() == 0);
    }
    // Step PLAY, SOUND and SLICE in the file: PLAY in the flags' bits 4-5, SOUND in ESMP, SLICE RND in SLCE.
    {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        tone_kit(*project, 0);
        project->tracks[0].speed = 0; project->tracks[1].speed = 9;
        project->tracks[0].direction = uint8_t(PlayDirection::Random);
        auto& p = bank->at[0][3];
        put(p, 0, uint8_t(sampler_first_note), 100);
        p.events[0].flags = with_play(p.events[0].flags | step_accent, StepPlay::Reverse);
        p.events[0].sample = 13;
        p.events[0].slice = slice_random;
        CHECK(step_play(p.events[0].flags) == StepPlay::Reverse && (p.events[0].flags & step_accent));
        CHECK(step_play(with_play(p.events[0].flags, StepPlay::Pad)) == StepPlay::Pad);
        Step st = p.step(0); // Step Edit's rewrite keeps the note's sample and slice
        st.notes[0].velocity = 90;
        p.set_step(0, st);
        CHECK(p.events[0].sample == 13 && p.events[0].slice == slice_random && step_play(p.events[0].flags) == StepPlay::Reverse);
        auto data = save_project(*project, *bank);
        const std::string tags(data.begin(), data.end());
        CHECK(tags.find("ESMP") != std::string::npos && tags.find("TSP2") != std::string::npos && tags.find("TDIR") != std::string::npos);
        auto back = std::make_unique<Project>();
        auto back_bank = std::make_unique<PatternBank>();
        CHECK(load_project(data, *back, *back_bank).empty() && back_bank->at[0][3] == bank->at[0][3]);
        CHECK(back->tracks[0].speed == 0 && back->tracks[1].speed == 9 && back->tracks[0].direction == uint8_t(PlayDirection::Random));
    }
    // The sampler: a step's sample through the pad (its whole sample), its way over the pad's, SLICE RND's draw.
    {
        auto noise = [](uint32_t seed) {
            SampleData d;
            d.frames = 30000; d.left.resize(d.frames); d.right.resize(d.frames);
            for (unsigned n = 0; n < d.frames; ++n) {
                seed = seed * 1664525u + 1013904223u;
                d.left[n] = d.right[n] = float(int32_t(seed) >> 8) / float(1 << 23) * 0.5f * (1.0f - float(n) / d.frames);
            }
            return d;
        };
        SampleData a = noise(3), b = noise(77);
        SamplerEngine::Table table {};
        table[7] = &a; table[8] = &b;
        Slices chop;
        chop.count = 3;
        chop.start[0] = 0; chop.start[1] = 10000; chop.start[2] = 20000;
        chop.stop[0] = 10000; chop.stop[1] = 20000; chop.stop[2] = 0;
        SamplerEngine::Chops chops {};
        chops[8] = &chop;
        PadSound pad;
        pad.sample = 7; pad.start = 500; pad.end = 25000; pad.play = PadPlay::OneShot;
        const int key = int(sampler_first_note) + 4;
        auto render = [&](const PadSound& sound, int note, int sample, unsigned play, unsigned pick) {
            SamplerEngine e;
            e.set_samples(&table);
            e.set_chops(&chops);
            e.set_pad(4, sound);
            if (sample >= 0 || play || pick) e.step_hit(sample, play, pick);
            e.note_on(note, 100);
            std::vector<float> out, l(128), r(128);
            for (unsigned block = 0; block < 250; ++block) {
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                e.render_stereo(l.data(), r.data(), 128);
                out.insert(out.end(), l.begin(), l.end());
            }
            return out;
        };
        PadSound whole_b = pad; whole_b.sample = 8; whole_b.start = whole_b.end = 0;
        const auto locked = render(pad, key, 8, 0, 0);
        CHECK(peak_of(locked) > 0.05f && locked == render(whole_b, key, -1, 0, 0) && locked != render(pad, key, -1, 0, 0));
        PadSound back_pad = pad; back_pad.reverse = true;
        CHECK(render(pad, key, -1, unsigned(StepPlay::Reverse), 0) == render(back_pad, key, -1, 0, 0));
        CHECK(render(back_pad, key, -1, unsigned(StepPlay::Forward), 0) == render(pad, key, -1, 0, 0));
        // SLICE RND: slice draw % count of the step's sample's chop.
        const int random_slice = pad_note(key, 0, -1, int(slice_random) - 1);
        CHECK(render(pad, random_slice, 8, 0, 4) == render(whole_b, pad_note(key, 0, -1, 1), -1, 0, 0));
        CHECK(render(pad, random_slice, -1, 0, 4) == render(pad, key, -1, 0, 0)); // no chop on its sample: the region
    }
    std::puts("elektron sequencing ok");
}

int main() {
    elektron_sequencing();
    clock_960();
    slice_hit_notes();
    mixer_strips();
    channel_strip_sound();
    mixer_tracks();
    mixer_spectrum();
    mixer_submixes();
    mixer_buses_input();
    mixer_motion();
    track_names();
    midi_lookahead();
    pad_sends();
    pad_stems();
    pad_outputs();
    stem_writer();
    pad_cloud();
    pad_sends_freeze();
    plugin_macros();
    capture_p5();
    prints_f2a();
    freeze_f2b();
    audio_track_playback();
    audio_warp();
    exports_p4();
    modulators_p4();
    delay_pool_clears();
    delay_sync_p4();
    chains_rest();
    chop_kept();
    samples_unlimited();
    stream_cache_keeps();
    long_samples_streamed();
    long_samples_copied();
    levels_cutoff_kept();
    pad_link_chains();
    chop_slices_keep_pad();
    chop_audition_pad_speed();
    instrument_p3();
    instrument_formats_p3();
    plugins();
    plugin_transport();
    card_sample_paths();
    sfz_reader();
    keygroup_reader();
    xpm_writer();
    loop_finder();
    auto_probe();
    external_notes();
    release_fitting();
    auto_sample_mono_time();
    auto_sample_layers();
    card_folders();
    exs_reader();
    kit_levels_octave();
    drum_program_pads();
    expansion_paths();
    sample_cache();
    stream_pager();
    parallel_render();
    render_worker_setup();
    one_dispatch();
    loops_and_buses_parallel();
    overload_guard();
    parked_load();
    sample_rates_p2();
    sampler_zones();
    keygroup_glide();
    keygroup_split();
    live_pad_values();
    note_pitches();
    looper_engine();
    looper_rates();
    note_edits();
    rate_constants();
    mtc_in();
    pad_fx_and_flex();
    onset_hits();
    sample_roots();
    beat_grid();
    loop_tempos();
    hits_on_time();
    punches();
    kept_punches();
    helpers();
    project_files();
    recording();
    preview_progressive();
    effects();
    arrange();
    audio_takes();
    motion();
    note_mutes();
    pad_mute_lane();
    layouts();
    host_sequencer();
    solo_empty_track();
    audition_level_host();
    drum_racks();
    sequenced_legato();
    audio_quality();
    std::puts("core ok");
}
