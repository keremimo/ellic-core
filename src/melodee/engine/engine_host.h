// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/engine/spsc_queue.h"
#include "melodee/engine/arp.h"
#include "melodee/engine/channel_strip.h"
#include "melodee/engine/note_repeat.h"
#include "melodee/engine/command.h"
#include "melodee/engine/compensation.h"
#include "melodee/engine/looper.h"
#include "melodee/engine/master_comp.h"
#include "melodee/engine/midi_engine.h"
#include "melodee/engine/plugin_engine.h"
#include "melodee/engine/print_player.h"
#include "melodee/engine/audio_player.h"
#include "melodee/engine/effects.h"
#include "melodee/engine/pad_fx_info.h"
#include "melodee/engine/punch.h"
#include "melodee/engine/render_pool.h"
#include "melodee/engine/sampler.h"
#include "melodee/engine/take_recorder.h"
#include "melodee/engine/bounce.h"
#include "melodee/engine/recall_ring.h"
#include "melodee/model/step_order.h"
#include "melodee/engine/midi_out.h"
#include "melodee/model/input_config.h"
#include "melodee/model/project.h"
#include "melodee/engine/scope_ring.h"
#include <array>
#include <string>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <memory>

// The audio side of Melodee: sixteen track slots, each with its engine and
// mix strip, a mirror of every pattern, and the sequencer clock. The shell
// sends Commands from the MIDI thread; render() runs on the audio thread and
// never allocates, locks or waits. What the screen needs back (playhead,
// meters) is published as atomics.
namespace melodee {

// A gain eased across each block instead of stepping at its start (a step is a click; a row of them, as a fader
// moves, a zipper): a straight line from where the last block ended toward the target, at most a full swing
// (0 to 1) per `seconds`. Level, pan, sends, mute and solo, the buses' returns, Monitor and main volume move so.
struct Ease {
    static constexpr float seconds = 0.01f;
    float from = 0, to = 0, step = 0; // this block's ends, and its change a frame
    // Once a block, before at(): toward `target` over `frames`; `jump`: there at once (a project just loaded).
    void aim(float target, unsigned frames, bool jump = false) {
        from = jump ? target : to;
        float most = float(frames) / (seconds * dsp::sample_rate());
        to = jump ? target : from + std::clamp(target - from, -most, most);
        step = (to - from) / float(std::max(frames, 1u));
    }
    float at(unsigned f) const { return from + step * float(f + 1); } // frame f of the block
    bool flat() const { return step == 0; }
    bool silent() const { return from == 0 && to == 0; }
};

// One track's sixteen patterns as the audio thread plays them: copies made
// on the MIDI thread, swapped in by pointer and handed back to be freed
// there, so the audio thread never allocates or frees.
using PlayPatterns = std::array<Pattern*, pattern_count>;
PlayPatterns copy_patterns(const std::array<Pattern, pattern_count>& patterns, unsigned speed = speed_normal); // as they play
void free_patterns(PlayPatterns& patterns);

// A whole project for the audio thread to switch to at once (a load or a new
// project). The MIDI thread makes it, the audio thread swaps its patterns
// for the ones it played and hands it back, to be freed with those.
struct ProjectPack {
    ProjectPack(const Project& project, const PatternBank& bank);
    ~ProjectPack();
    ProjectPack(const ProjectPack&) = delete;
    ProjectPack& operator=(const ProjectPack&) = delete;
    Project project;
    std::array<PlayPatterns, track_count> patterns {};
};

// One track for the audio thread to take whole, without stopping (a track
// copy, or its undo): the track and its sixteen patterns, their motion in
// them. Made and freed like a ProjectPack.
struct TrackPack {
    TrackPack(unsigned track, const Track& value, const std::array<Pattern, pattern_count>& patterns);
    ~TrackPack();
    TrackPack(const TrackPack&) = delete;
    TrackPack& operator=(const TrackPack&) = delete;
    uint8_t track = 0;
    Track value;
    PlayPatterns patterns {};
};

// A sequenced note's own sound (step SOUND and PLAY): the sample slot it plays in place of its pad's (-1 the pad's),
// the way it plays (StepPlay: Forward or Reverse, Random already turned into one of them) and the slice a SLICE RND
// takes (a draw, modulo the chop's slices).
struct StepSound { int32_t sample = -1; uint8_t play = 0; uint16_t pick = 0; bool own() const { return sample < 0 && !play; } };

class EngineHost {
public:
    EngineHost();
    // A print host (freeze plan F2a, shell/print_engine.h): one track's engine, chain and sequencer, rendered beside
    // the live host on another thread. It makes none of the buffers a print never uses (Sample Record's take, Audio
    // Recall, the mixdown's ring, the punches' histories, delay compensation's lines), holds `delay_blocks` of the
    // effects' delay memory, keeps its own effect clock, never sets the device's rate (a rate other than the one it
    // was made at leaves it silent), and renders on the calling thread alone.
    struct Lean { unsigned delay_blocks = 0; };
    explicit EngineHost(Lean lean);
    ~EngineHost();
    bool lean() const { return is_lean; }
    EngineHost(const EngineHost&) = delete;
    EngineHost& operator=(const EngineHost&) = delete;
    // Before the audio thread starts: the whole project and its patterns.
    void load(const Project& project, const PatternBank& patterns);

    // MIDI thread only. False when the queue is full (the change is lost).
    bool send(const Command& command) {
        if (queue.push(command)) return true;
        lost.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // MIDI thread: a note played at `ns` (CLOCK_MONOTONIC, the pad's or the MIDI message's receive stamp). It plays
    // a block after the frame being rendered then, at that frame (Command::timed), so two hits keep their spacing
    // to the frame instead of moving to block starts. Untimed (the next block's start) while the clock is unknown.
    bool send_at(Command command, uint64_t ns) {
        if (ns && clock.known()) { command.timed = true; command.at = uint32_t(clock.frame_at(int64_t(ns)) + block); }
        return send(command);
    }
    // Any thread: where `track` was in its pattern when a hit at `ns` was heard (its stamp less the output latency),
    // to the nearest tick, counted back from position_of() (so it may come out below 0: wrap it by the pattern's
    // length), and ticks() then. False while no tick of this run is known for it. place_at: the same for the frame
    // rendered then.
    bool heard_place(unsigned track, uint64_t ns, int& position, uint32_t& ticks) const;
    bool place_at(unsigned track, uint64_t frame, int& position, uint32_t& ticks) const;
    // Any thread: timed notes played on their frame, those that came after it (played at once), and the latest by.
    uint32_t timed_kept() const { return timed_on_time.load(std::memory_order_relaxed); }
    // Any thread (roadmap P1): ratchets' hits played after their first; notes a trig condition let through and held back.
    uint32_t ratchet_hits() const { return hits_played.load(std::memory_order_relaxed); }
    uint32_t trigs_kept() const { return trig_kept.load(std::memory_order_relaxed); }
    uint32_t trigs_dropped() const { return trig_dropped.load(std::memory_order_relaxed); }
    bool filling() const { return fill_now.load(std::memory_order_relaxed); }
    uint32_t timed_missed() const { return timed_late.load(std::memory_order_relaxed); }
    uint32_t timed_worst_frames() const { return timed_worst.load(std::memory_order_relaxed); }

    // Audio thread: `frames` interleaved stereo S32 frames, after `volume`;
    // `input` the capture block (silent when there is none), for Record.
    // `stems`, when given, gets each track's stem: `track_count` rows of
    // `block` floats, the track after its chain and fader as you hear it,
    // summed to mono with its pan law taken back out, before the buses, the
    // master chain and main volume. Muted (or soloed-out) tracks are silent.
    // `usb`, when given, is what a computer plays over USB-C: `frames`
    // interleaved stereo floats, for the USB sources. `fixed`, when given,
    // gets `out` as it would be at full main volume, for what a computer
    // records over USB-C (Settings' USB volume).
    void render(int32_t* out, unsigned frames, float volume, const int32_t* input = nullptr, float* stems = nullptr,
                const float* usb = nullptr, int32_t* fixed = nullptr);
    // Before the audio thread starts: `count` render workers beside it (0: it renders alone), at its real-time
    // priority. Each block's tracks, their engines and then their chains, are shared out among them once the
    // last block's took more than `from_ns` (all cores together; 0 always, for tests). `pinned` (roadmap P2): the
    // audio thread keeps to core 1 (from its first block) and worker n to core 2 + n; whoever starts Melodee keeps
    // its other threads on core 0 (rendering_thread()). Else every one floats over every core.
    void set_render_threads(unsigned count, uint64_t from_ns = 150000, bool pinned = false);
    unsigned render_threads() const { return workers.workers(); }
    bool render_pinned() const { return pin_audio >= 0; }
    // Any thread: whether thread `tid` renders (the audio thread, once it has played a block, or a worker).
    bool rendering_thread(int tid) const;
    // Any thread: how long the workers wait awake for a run that follows within the block (render_pool.h).
    void set_render_spin(unsigned us, bool always = false) { workers.set_spin(us); workers.set_linger_always(always); } // always: after every run, as before
    // Any thread: the workers used (true) or left asleep, from the next block; for comparing the two.
    void set_render_parallel(bool on) { parallel_allowed.store(on, std::memory_order_relaxed); }
    bool render_parallel() const { return parallel_allowed.load(std::memory_order_relaxed); }
    // Any thread (roadmap P2), from the next block: one dispatch a block (true, the default), each track's engine over
    // the whole block with its notes and values at their frames, then its chain, as one job; or the old way, every
    // track cut wherever any track has a tick or a note and the workers called for each run, then for the chains.
    void set_one_dispatch(bool on) { whole_blocks.store(on, std::memory_order_relaxed); }
    bool one_dispatch() const { return whole_blocks.load(std::memory_order_relaxed); }
    // Tests: one dispatch, but each track's engine cut wherever the old way cut every track, so the two sound the same
    // to the sample (engines step their modulation from the start of each render call).
    void set_split_cuts(bool on) { split_cuts = on; }
    // Calls a track's job could not keep (its list full), made at the block's first frame instead.
    uint32_t calls_lost() const { return calls_full.load(std::memory_order_relaxed); }
    // Any thread: the share of its time the latest blocks took (the peak, falling slowly), and how many blocks the
    // workers have helped with.
    float render_load() const { return block_load.load(std::memory_order_relaxed); }
    // Any thread (roadmap P2): track `t`'s share of a block's time, its engine and chain on whichever core took them
    // (the peak, falling slowly, as render_load()); and the busiest track now (-1: none takes any).
    float track_load(unsigned t) const { return track_loads[t % track_count].load(std::memory_order_relaxed); }
    int busiest_track() const;
    // Any thread (roadmap P2), the overload guard: on (Melodee's) or off (the default: a test's output must not hang
    // on how fast its computer is); the voices it stole and the blocks it lightened, since start.
    void set_overload_guard(bool on) { guard_allowed.store(on, std::memory_order_relaxed); }
    uint32_t guard_steals() const { return guard_stolen.load(std::memory_order_relaxed); }
    uint32_t guard_blocks() const { return guard_eased.load(std::memory_order_relaxed); }
    // Tests: every block counted as taking `share` of its time (below 0: as measured).
    void set_guard_test_share(float share) { guard_test.store(share, std::memory_order_relaxed); }
    uint64_t render_parallel_blocks() const { return parallel_blocks.load(std::memory_order_relaxed); }
    // Chains at rest in the last block (resting chains, below). chain_rests off: every chain is run every block, as
    // before; for the tests and the bench, which compare the two.
    unsigned chains_resting() const { return chains_resting_now.load(std::memory_order_relaxed); }
    // The render thread's (the bench, the tests): each chain that holds an effect: 'z' at rest; else what kept it up in
    // its last block: 'i' sound coming in, 'o' sound going out, 's' an effect's state still moving, 'p' a plug-in or a
    // keyed effect, 'q' quiet, counting towards its rest.
    std::string chains_map() const {
        std::string map;
        for (unsigned c = 0; c < mix_chain_count; ++c) if (fx[c][0].kind() != EffectKind::None) map += chain_rest[c].asleep ? 'z' : chain_rest[c].why;
        return map;
    }
    static inline bool chain_rests = true;
    // The Looper (docs/LOOPER.md): its loops play into the tracks L1-L4, after the sixteen.
    Looper& looper() { return loops; }
    const Looper& looper() const { return loops; }
    float loop_meter(unsigned loop) const { return loop_meters[loop % loop_tracks].load(std::memory_order_relaxed); }
    TakeRecorder& recorder() { return take; }
    const TakeRecorder& recorder() const { return take; }
    RecallRing& recall() { return recall_ring; }
    const RecallRing& recall() const { return recall_ring; }
    Bounce& bounce() { return render_out; }
    // Roadmap P4: stems beside the next render (StemRing, the service thread's; nullptr: none), taken while it runs.
    void set_stems(StemRing* ring) { stem_ring.store(ring, std::memory_order_release); }
    // Live stems, every block (the Ableton Move's USB submixes): the same stems a render's StemRing gets, each
    // track as it leaves for the master or its submix (after its chain, strip, level, pan and mute, eased as heard,
    // its key pad with it), each bus's return and each submix, written into `rows[(stem * 2 + side) * block + f]`:
    // stems 0-15 the tracks, 16-19 buses A-D, 20-27 the submixes, side 0 left (StemRing::stem_count rows). A track
    // with no engine is silent; a Park's fade is kept, as the stems have it. Audio thread, before render(), as often
    // as it likes; nullptr (the default): none taken, nothing spent.
    void set_live_stems(float* rows) { live_stems = rows; }
    const Bounce& bounce() const { return render_out; }
    // A print host: track `track` taken each block after its chain (`post`), before its strip's level and pan (the
    // mixer, docs/MIXER_PLAN.md: a print holds neither), or as its engine left it; print_left() / print_right() hold
    // the block just rendered.
    void set_print_tap(unsigned track, bool post) {
        print_host = true;
        source_track.store(track % track_count, std::memory_order_relaxed);
        source_post.store(post, std::memory_order_relaxed);
        source_now.store(unsigned(InputSource::Track), std::memory_order_relaxed);
    }
    const float* print_left() const { return tap_l; }
    const float* print_right() const { return tap_r; }
    // And what its kit's pads sent into each bus that block, as they left their own chains (before the track's level
    // and pan); a bit a bus with anything there.
    unsigned print_sends_fed() const { return tap_sends; }
    const float* print_send_left(unsigned bus) const { return tap_send_l[bus % bus_count]; }
    const float* print_send_right(unsigned bus) const { return tap_send_r[bus % bus_count]; }
    // Device frames from Play (from the top) to the frame tick `ticks` plays on, at the tempo and swing playing now,
    // placed as render() places each tick (its frames counted up to each one).
    uint64_t tick_frame(uint32_t ticks) const;
    unsigned record_source() const { return source_now.load(std::memory_order_relaxed); } // InputSource
    unsigned record_track() const { return source_track.load(std::memory_order_relaxed); } // InputSource::Track: which
    bool record_post() const { return source_post.load(std::memory_order_relaxed); }      // and after its chain
    bool monitoring() const { return monitor_on.load(std::memory_order_relaxed); }
    // Any thread: Settings' USB-C in Direct (drivers/direct_monitor.h): what a computer plays is then heard straight
    // at the jacks, so this mix leaves it out wherever it would play it (a MIDI track's AUDIO input with MONITOR on,
    // Input Config's Monitor, a loop on it); usb_heard() says, after each block, whether one of those wanted it, and
    // usb_gain_left() / right() at what level each side, before main volume: what those would have given it before
    // their effects (a MIDI track's AUDIO LEVEL, level and pan; Monitor's input gain at a centred pan; a loop's level
    // and pan), added up.
    void set_usb_direct(bool on) { usb_direct.store(on, std::memory_order_relaxed); }
    // USB out: render()'s `stems` rows hold the submixes instead of the tracks: submix n's left in row 2n, its right
    // in row 2n + 1, as it joins the master (the same as a render's submix stems).
    void set_usb_submixes(bool on) { usb_submixes.store(on, std::memory_order_relaxed); }
    bool usb_heard() const { return usb_heard_now.load(std::memory_order_relaxed); }
    float usb_gain_left() const { return usb_gain_l.load(std::memory_order_relaxed); }
    float usb_gain_right() const { return usb_gain_r.load(std::memory_order_relaxed); }
    bool input_taken() const { return source_taken.load(std::memory_order_relaxed); }

    // Feedback, any thread.
    bool playing() const { return is_playing.load(std::memory_order_relaxed); }
    uint32_t ticks() const { return tick_count.load(std::memory_order_relaxed); } // clock ticks since Play, ticks_per_quarter a quarter
    // Where a track is in its pattern, in ticks; -1 while it plays none.
    int position_of(unsigned track) const { return positions_now[track % track_count].load(std::memory_order_relaxed); }
    // (Roadmap P1: in its pattern's ticks as written, whatever the track's speed.)
    // And how many times its pattern has come round since it began (roadmap P1: a kit lane's place is from both).
    uint32_t pass_of(unsigned track) const { return passes_now[track % track_count].load(std::memory_order_relaxed); }
    unsigned count_in() const { return count_beat.load(std::memory_order_relaxed); } // the count-in's beat now, 1..; 0 none
    uint32_t count_in_ticks() const { return count_rest.load(std::memory_order_relaxed); } // the count-in's ticks still to come
    uint32_t count_in_from() const { return count_from.load(std::memory_order_relaxed); }  // the tick the song starts on after it
    unsigned tempo_now() const { return tempo_shown.load(std::memory_order_relaxed); } // tenths of a BPM, as it plays
    unsigned pattern_of(unsigned track) const { return patterns_now[track % track_count].load(std::memory_order_relaxed); }
    // Roadmap P5: how many follow actions have come on a track, and the tracks a STOP left silent (a bit each).
    uint32_t follows_of(unsigned track) const { return follows_now[track % track_count].load(std::memory_order_relaxed); }
    uint32_t stopped_tracks() const { return stopped_now.load(std::memory_order_relaxed); }
    // Issue 40: the tracks whose mute or solo waits for its step (Rec quantize's Q, a bit each), and a kit track's
    // pads whose mute waits (a bit each, 64 to a word).
    uint32_t mutes_waiting() const { return mute_waits_now.load(std::memory_order_relaxed); }
    uint64_t pad_mutes_waiting(unsigned track, unsigned word) const {
        return pad_waits_now[track % track_count][word % pad_wait_words].load(std::memory_order_relaxed);
    }
    bool punching() const { return punch_on.load(std::memory_order_relaxed); }
    EffectKind pad_fx_kind(unsigned track, unsigned pad, unsigned slot) const { // for tests
        return (*pad_fx)[track % track_count][pad % sampler_pads][slot % pad_slots].kind();
    }
    float compression() const { return comp.reduction_db(); } // dB the master compressor takes off now
    void compressor_activity(float& level_db, float& reduce_db) { comp.take_activity(level_db, reduce_db); } // the page's picture
    unsigned flex_playing() const { return flex_now.load(std::memory_order_relaxed); } // Flex Beat's pad playing, 0 Empty
    unsigned live_punches() const { return live_on.load(std::memory_order_relaxed); }  // Pad FX: a bit a live slot sounding
    unsigned kept_punches() const { return kept_on.load(std::memory_order_relaxed); }  // pad-fx C: a bit a kept slot sounding
    bool song_playing() const { return in_song.load(std::memory_order_relaxed); }
    unsigned song_bar() const { return bar_now.load(std::memory_order_relaxed); }
    float meter(unsigned track) const { return meters[track % track_count].load(std::memory_order_relaxed); }
    // A mixer channel's meter, by its strip number (model/project.h; tracks so far, docs/MIXER_PLAN.md core step 3):
    // each side's peak after its strip, level, pan and mute, and its compressor's gain reduction in dB, each falling
    // back as meter() does.
    struct StripMeter { float left = 0, right = 0, reduction = 0; };
    StripMeter strip_meter(unsigned n) const {
        n %= strip_channels;
        return {peak_l[n].load(std::memory_order_relaxed), peak_r[n].load(std::memory_order_relaxed), reductions[n].load(std::memory_order_relaxed)};
    }
    // What a channel's strip plays now (the values its commands set), by strip number.
    const Strip& strip_values(unsigned n) const { return strips[n % strip_channels].values(); }
    // The mixer's EQ picture (melodee-sample docs/MIXER_PLAN.md step 5): one channel, by strip number, has its sound
    // after its strip (before level and pan) copied into spectrum() each block; -1 none, which costs nothing.
    void set_spectrum_tap(int strip) { spectrum_tap.store(strip >= 0 && strip < int(strip_channels) ? strip : -1, std::memory_order_relaxed); }
    const ScopeRing& spectrum() const { return scope; }
    // The voices of the track the screen watches (engine/voice_tap.h), as the last block left them; false while a
    // write was under way each time it looked. watch_taps(-1): none taken.
    void watch_taps(int track) { tap_track.store(track, std::memory_order_relaxed); }
    bool taps(TrackTaps& out) const { return tap_board.read(out); }
    float peak() const { return output_peak.load(std::memory_order_relaxed); }
    // The meter lights beside the screen: the mix (before main volume, the listening level) and the input source,
    // side 0 left, 1 right, as peaks falling meter_fall_db a second (stock's fall: -60 dB in a second).
    static constexpr float meter_fall_db = 60;
    float out_level(unsigned side) const { return out_levels[side & 1].load(std::memory_order_relaxed); }
    float in_level(unsigned side) const { return in_levels[side & 1].load(std::memory_order_relaxed); }
    unsigned dropped() const { return lost.load(std::memory_order_relaxed); }

    // Service thread: a loaded sample for slot `slot` (nullptr empties it). The
    // audio thread swaps it in at its next block and hands the one it replaced
    // back through take_retired(), to be freed there.
    bool offer_sample(unsigned slot, const SampleData* sample) { return offers.push({uint16_t(slot), sample}); }
    StreamPager& streams() { return pager; } // multisampled instruments' reading from the card
    bool take_retired(const SampleData*& sample) { return retired.pop(sample); }
    // The rack's thread (plugin/plugin_rack.h): the plug-in for entry `entry` of Project::plugins (nullptr: none). The
    // audio thread swaps it in at its next block, after the commands sent before it, and hands the one it replaced
    // back through take_retired_plugin(), to be freed there.
    bool offer_plugin(unsigned entry, PluginRunner* runner) { return plugin_offers.push({uint8_t(entry), runner}); }
    bool take_retired_plugin(PluginRunner*& runner) { return retired_plugins.pop(runner); }
    // MIDI thread: a ProjectPack the audio thread has finished with, to free.
    bool take_done_pack(const ProjectPack*& pack) { return done_packs.pop(pack); }
    // A project load without a dropout (MIDI thread, the one that sends commands). park() sends a Park behind the
    // commands already sent: the audio thread plays up to it, fades out over Ease::seconds, then plays silence
    // touching nothing of its state but the clock, taking no more commands, samples or plug-ins. Once parked(serial)
    // says so, the caller owns the host's state: reload() rebuilds the whole project there, however long it takes,
    // and clears the delay memory left behind; resume() lets the audio thread go on, fading in from silence, with
    // whatever was sent meanwhile. resume() before parked() (the audio thread not running) calls the park off.
    // park() returns 0 when the queue is full. With parking off (host tests and the DSP bench drive render()
    // themselves) a load sends a Reload instead, rebuilt on the audio thread as before.
    void set_parking(bool on) { parking_allowed = on; }
    bool parking() const { return parking_allowed; }
    uint32_t park();
    bool parked(uint32_t serial) const { return serial && parked_serial.load(std::memory_order_acquire) == serial; }
    void reload(ProjectPack& pack, bool keep_loops); // the Reload's work: the audio thread's, or the loader's while parked
    void resume(uint32_t serial) { resume_serial.store(serial, std::memory_order_release); }
    unsigned parks() const { return parks_done.load(std::memory_order_relaxed); } // times the audio thread parked (status)
    uint64_t blocks_rendered() const { return blocks_seen.load(std::memory_order_relaxed); } // render() calls, parked too
    bool take_done_track(const TrackPack*& pack) { return done_tracks.pop(pack); }
    // MIDI thread (parity phase 9): what goes out, each at its frame, and the clock that says when that frame is heard.
    bool take_midi(MidiOutEvent& event) { return midi_events.pop(event); }
    // Before the audio thread starts: called on the audio thread as each MIDI message played by hand is emitted, to
    // wake the MIDI thread for it (Session::wake_midi()).
    void set_hand_waker(void (*wake)(void*), void* context) { hand_waker = wake; hand_waker_context = context; }
    const AudioClock& audio_clock() const { return clock; }
    // Audio thread, before the first block at each open of the device (and before it starts): its frames a second and
    // the frames it holds queued (MIDI out lines up with them). A new rate silences render() until the project comes
    // again (a Reload: every engine and effect worked out anew at it); format_changes() tells the shell to send it.
    void set_audio_format(double frames_a_second, uint64_t latency_frames);
    unsigned format_changes() const { return format_serial.load(std::memory_order_acquire); }
    unsigned midi_dropped() const { return midi_lost.load(std::memory_order_relaxed); }
    bool following() const { return ext_now.load(std::memory_order_relaxed); } // the sequencer waits for MIDI clock in
    // MTC in (parity phase 9b): the song placed and kept on time by it; how far off it is (filtered) and the pace trim.
    bool mtc_following() const { return mtc_now.load(std::memory_order_relaxed); }
    int mtc_error_us() const { return mtc_error_now.load(std::memory_order_relaxed); }
    int mtc_trim_ppm() const { return mtc_ppm.load(std::memory_order_relaxed); }
    unsigned mtc_places() const { return mtc_placed.load(std::memory_order_relaxed); } // locates done (Start, jumps, catch-ups)
    // MIDI thread (roadmap P1): a note the arp (Note Repeat) played while Seq Record was on, for the pattern it fell in.
    struct ArpWritten { uint8_t track = 0, pattern = 0; uint16_t key = 0; uint8_t velocity = 0, cutoff = 0; uint32_t tick = 0; uint32_t length = 0; };
    bool take_arp_written(ArpWritten& note) { return arp_written.pop(note); }
    // MIDI thread: a pattern copy the audio thread played until a newer one came (PatternData), to free.
    bool take_retired_pattern(Pattern*& pattern) { return retired_patterns.pop(pattern); }
    // MIDI thread (issue 24): a sample's chop the audio thread held until a newer one came (Command::Chop), to free.
    bool take_retired_chop(const Slices*& chop) { return retired_chops.pop(chop); }
    // Any thread (roadmap P4, plug-in delay compensation): device frames everything heard is held back by, so the
    // latest path (a plug-in's latency, its rate converter's, a Limiter's LOOKAHEAD) meets the rest: the tracks' and
    // loops' latest, the buses' and Knob FX's latest after them, and the master's chain. MIDI out, the metronome and
    // Seq Record's placing follow it. track_latency(): a track's own path (its plug-in and chain).
    unsigned compensation() const { return pdc_total.load(std::memory_order_relaxed); }
    unsigned track_latency(unsigned t) const { return pdc_tracks[t % track_count].load(std::memory_order_relaxed); }
    // A track's engine's own (a PLUGIN track's plug-in), before its chain (freeze plan F2b: a Pre FX print's).
    unsigned engine_latency(unsigned t) const { return pdc_engines[t % track_count].load(std::memory_order_relaxed); }
    unsigned bus_latency(unsigned b) const { return pdc_buses[b % bus_count].load(std::memory_order_relaxed); }
    unsigned master_latency() const { return pdc_master.load(std::memory_order_relaxed); }
    // Freeze plan F2b (engine/print_player.h). MIDI thread: a FreezePack the audio thread has finished with, to free
    // once the pager has seen its streams closed (StreamPager::flush). Any thread: the tracks frozen now, those whose
    // print plays now, and those whose engine sleeps (a bit each); print runs that met frames not pinned yet, and
    // waits after a jump to where nothing was held.
    bool take_done_freeze(FreezePack*& pack) { return done_freezes.pop(pack); }
    // Freeze plan F3 (engine/audio_player.h). MIDI thread: an AudioPack the audio thread has finished with, to free once
    // the pager has seen its streams closed. Any thread: audio tracks' runs that met frames not pinned yet, and waits
    // after a jump. The service thread drains audio_take() (Command::AudioRecord) into the take's file as it comes.
    bool take_done_audio(AudioPack*& pack) { return done_audio.pop(pack); }
    uint32_t audio_underruns() const { return audio_under.load(std::memory_order_relaxed); }
    uint32_t audio_waits() const { return audio_waited.load(std::memory_order_relaxed); }
    Bounce& audio_take() { return audio_in; }
    const Bounce& audio_take() const { return audio_in; }
    int audio_take_track() const { return audio_track.load(std::memory_order_relaxed); }     // -1: none
    int audio_take_pattern() const { return audio_pattern.load(std::memory_order_relaxed); } // -1: not begun
    uint32_t frozen_tracks() const { return frozen_now.load(std::memory_order_relaxed); }
    uint32_t printing_tracks() const { return printing_now.load(std::memory_order_relaxed); }
    uint32_t sleeping_tracks() const { return sleeping_now.load(std::memory_order_relaxed); }
    uint32_t print_underruns() const { return print_under.load(std::memory_order_relaxed); }
    uint32_t print_waits() const { return print_waited.load(std::memory_order_relaxed); }
    // Frames from a pattern's top to its tick `ticks`, unrounded, at the tempo and swing playing now: where a print
    // places that tick (its frame is this rounded up).
    double pattern_frames(uint32_t ticks) const;
    // Any thread: delay blocks free in the effects' pool (a delay, reverb or chorus with none passes its sound through).
    unsigned delay_blocks_free() const { return pool.free_blocks(); }
    // Any thread (roadmap P4): MIDI tracks' slot messages sent, and those a later value replaced before their time.
    uint32_t slots_sent() const { return cc_sent.load(std::memory_order_relaxed); }
    uint32_t slots_thinned() const { return cc_thinned.load(std::memory_order_relaxed); }
    // Any thread (roadmap P4): modulator `n`'s value now (an LFO's -1..1, a follower's 0..1), for its page.
    float modulator_value(unsigned n) const { return mod_now[n % modulator_count].load(std::memory_order_relaxed); }

    static constexpr unsigned block = 128;

private:
    EngineHost(bool lean, unsigned delay_blocks);
    const bool is_lean = false;
    EffectClock own_clock;                // a print host's
    EffectClock* fx_clock = &effect_clock; // the clock its effects hear: the live one, or own_clock
    // Freeze plan F2b: the clock a frozen track's engine hears while its print plays in its place (Slot::hears_stop), this
    // block's with the song stopped (as TRANSPORT off has it: the tempo and the place, never Play). A plug-in that
    // plays by itself (a sequencer or an arpeggiator of its own) then stops as it does on Stop, rather than playing
    // under its print.
    EffectClock held_clock;
    struct PendingOff { int32_t key = -1; int32_t samples = 0; }; // a pad's key, with an instrument's semitones over it
    struct PendingOn { int32_t key = -1; int16_t velocity = 0; int32_t from = -1; int32_t samples = 0, length = 0; }; // an arp note swung late
    // A ratchet's hit still to come (roadmap P1): `samples` frames on it plays `key` (as the engine plays it; a kit
    // pad's semitones over it) and `midi` out, for `length` frames.
    struct PendingHit { int32_t key = -1; int16_t midi = -1; uint8_t velocity = 0; bool accent = false, sound = true; int32_t samples = 0, length = 0, shift = 0; StepSound step; };
    // Roadmap P2, one dispatch a block: a call to a track's engine the sequencing pass made past the block's first
    // frame, kept for the track's job to make at that frame (play_calls()).
    struct EngineCall {
        enum class Op : uint8_t { NoteOn, NoteOff, Slide, Param, Wheel, Pressure, Mod, PlayPad, Audition, MovePad, RestorePad,
                                  RestorePads, PadFxParam, Bend, Control, PadMute };
        Op op = Op::NoteOn;
        bool flag = false;  // Slide: glide; PadMute: fade (Mute Audio)
        uint16_t frame = 0; // into the block
        int32_t a = 0, b = 0, c = 0, d = 0;
        float x = 1;        // NoteOn, Slide: the accent's gain; Wheel, Pressure, Bend: the value
    };
    static constexpr unsigned calls_a_block = 384; // a track's, more than a pattern's start (every value back) needs
    struct Slot {
        // What every block reads of every track, an empty one too (render()'s passes over the tracks, run_hits(),
        // next_hit(), arp_live()): first and side by side. A slot is over 100 kB (the sampler most of it), so these
        // apart would be ten lines far apart in each slot for a block on a cold cache to wait for, not a few.
        EngineKind kind = EngineKind::None;
        bool mute = false, solo = false;
        // A solo counts only on a track that plays something (Track::used(), as the pad lights have it): an empty
        // track kept soloed would silence every other track with nothing to show why.
        bool soloing() const { return solo && kind != EngineKind::None; }
        bool knob_pad_fed = false; // some sub-block of this block put something in knob_pad_l / knob_pad_r
        bool print_fed = false;    // this block's print is in print_l / print_r (freeze plan F2b, below)
        Engine* engine = nullptr;
        unsigned hits_waiting = 0;                   // ratchets' hits (`hits`) still to come
        int32_t repeat_late = -1, repeat_length = 0; // a swung step of Note Repeat: frames to it, and its notes' length
        PendingOn arp_late;
        PrintPlayer print; // the track's prints, their voices (freeze plan F2b, below)
        Arp arp;
        NoteRepeat repeat;
        SamplerEngine sampler;
        MidiEngine midi; // silent: a MIDI track (docs/MIDI_TRACKS.md)
        PluginEngine plugin;             // a PLUGIN track: the rack's plug-in for `plugin_entry`
        uint8_t plugin_entry = no_plugin;
        float gain = 1, pan_left = 0.7071f, pan_right = 0.7071f;
        std::array<float, bus_count> sends {}; // into each bus A-D (round 2: four sends a track)
        // As the block plays them (Ease): level and pan on each side, heard (1) or muted / soloed out (0),
        // into Knob FX (1) or not, and each send.
        Ease mix_l, mix_r, audible, knob_share;
        std::array<Ease, bus_count> sends_now {};
        uint8_t pattern = 0, queued = 255; // queued: launches on the next bar
        uint8_t out = out_master;          // the mixer: where it plays, the master or a submix (Mix::out)
        uint8_t out_was = out_master;      // where it played in the last block (a change crosses over in the next)
        uint32_t start = 0;                // the tick its pattern started on
        int clip = -1;                     // in the song: the clip playing
        bool silent = false;               // in the song: no clip here
        // Off (sequence links): stopped by its scene or SEQ + STOP until a launch, through Play; off_queued: from the
        // next bar (Command::Pattern with a value below 0).
        bool off = false, off_queued = false;
        std::array<PendingOff, 32> offs {};
        // A note with Slide or Tie, held into the next note of its pattern, which takes it over (key: as the
        // engine plays it; midi: as it went out). Its off, a sixteenth past that note, lets it go if none comes.
        struct Held { int16_t key = -1, midi = -1; bool glide = false; } held;
        std::array<int16_t, max_params> base {}; // the track's own values; motion only borrows them
        std::array<std::array<int16_t, effect_params>, max_slots> fx_base {}; // and its effects'
        bool fx_moved = false;
        // Roadmap P4, the mix's lanes: the track's own level, pan and sends as set (Motion only borrows them), and
        // whether Motion moved them, or the master's values, since.
        int16_t mix_level = 100, mix_pan = 0;
        std::array<int16_t, bus_count> mix_sends {};
        bool mix_moved = false, master_moved = false;
        bool strip_moved = false; // the mixer: Motion moved its strip (core step 6); Stop puts it back as set
        bool moved = false;                       // motion changed a value since Play
        // Issue 32: a kit's pad mutes as set (Mute + pad; a mute lane only borrows them), and whether a lane moved one.
        std::array<uint64_t, (sampler_pads + 63) / 64> pad_mutes {};
        bool pad_mutes_moved = false;
        float wheel = 0;                          // the mod wheel as the fader left it, 0..1
        bool wheel_moved = false;                 // and motion moved it since (parity phase 8)
        float bend = 0;                           // a PLUGIN track's pitch bend as the fader left it, -1..1
        bool bend_moved = false;                  // and motion moved it since
        // Motion's locks playing (roadmap P1): a lane, and the ticks before it goes back.
        struct Lock { int16_t param = -1; uint8_t pad = 0; uint32_t left = 0; };
        std::array<Lock, 16> locks {};
        unsigned locks_on = 0;
        float buffer[block] {};
        float left[block] {}, right[block] {};
        float knob_pad_l[block] {}, knob_pad_r[block] {}; // a kit's pads under Knob FX, as the sampler left them (knob_pad_fed)
        struct PadSendBuffers { float l[bus_count][block] {}, r[bus_count][block] {}; };
        std::unique_ptr<PadSendBuffers> pad_sends = std::make_unique<PadSendBuffers>(); // a kit's pad sends, each bus apart (heap: 4 kB a track)
        unsigned send_pad_fed = 0;                                                // a bit a bus: something there this block
        struct PadSubBuffers { float l[sub_count][block] {}, r[sub_count][block] {}; };
        std::unique_ptr<PadSubBuffers> pad_subs = std::make_unique<PadSubBuffers>(); // a kit's pads sent to submixes, each apart (8 kB)
        unsigned sub_pad_fed = 0;                                                 // a bit a submix: something there this block
        uint8_t pad_routes = 0;                                                   // this block: the submixes its pads are sent to
        std::array<PendingHit, 32> hits {}; // ratchets' hits to come, each on its frame (hits_waiting)
        // Trig conditions (model/trig.h): how the last weighed note went, on the track and on each key (a kit's
        // lanes), and whether one has been weighed since Play (none yet: counted as played, for NEI).
        bool weighed = false, went = false;
        std::array<uint64_t, 4> keys_weighed {}, keys_went {};
        int arp_played = -1, arp_from = -1; // the arp's last note and the held key it came from
        // Notes Seq Record just wrote ahead of the playhead (Rec Quantize), already heard live: skipped once.
        struct Skip { int32_t key = -1; uint8_t pattern = 0; uint32_t tick = 0, until = 0; };
        std::array<Skip, 8> skips {};
        std::array<EngineCall, calls_a_block> calls {}; // this block's, in the order made (roadmap P2)
        unsigned call_count = 0;
        // Roadmap P4, a MIDI track's slots (docs/MIDI_TRACKS.md M2): at most one message a slot every 2 ms, the latest
        // value waiting (`pending`, -1 none) until its time; the program last sent (0 none), so a launch sends a change.
        struct Thin { uint64_t last = 0; int pending = -1; };
        std::array<Thin, 24> thin {};
        int program_sent = -1;
        // Roadmap P5: a follow action (model/follow.h) due on tick `follow_at` (follow_do: the action, 0 none), planned
        // as its pattern starts; it waits while a launch is queued, and a chained track's never come.
        bool chained = false;
        // A MIDI track ahead (play_ahead()): the next tick to play, the start and pattern read from, and a launch read
        // ahead (the bar line it comes on, UINT32_MAX none, and its pattern).
        uint32_t ahead_next = 0, ahead_start = UINT32_MAX, ahead_bar = UINT32_MAX;
        uint8_t ahead_pattern = 255, ahead_queued = 255;
        uint32_t loop_from = 0, loop_ticks = 0; // an editor's loop (Command::EditLoop): only these ticks play; as written (edit_loop())
        uint8_t follow_do = 0;
        uint32_t follow_at = 0;
        // Freeze plan F2b: the track's prints (none: it plays live), and ones unfrozen whose voices still fade; the
        // voices (`print`, at the top); whether the print plays the pattern now (its notes then sequence nothing on
        // the engine), the engine awake (hand notes or a live pattern; asleep its render and, after the print, its
        // chain are skipped), the frames it has been quiet, keys held by hand; this block's print (print_l / print_r:
        // its rows of EngineHost::print_frames, on the heap, so a host on a test's stack stays as small as it was;
        // `print_fed`, at the top) and its level and pan.
        FreezePack* freeze = nullptr;
        FreezePack* freeze_leaving = nullptr;
        bool print_on = false, awake = true, print_post = true; // print_post: its prints' tap (kept for a fade's end)
        uint32_t quiet = 0;
        // Its engine hears the song stopped (held_clock): frozen, and its print playing in its place or the track
        // stopped (a STOP, a song row without it), unless a track playing live shares its plug-in (`shared`, which
        // would hear Play and Stop by turns). `hears_stop` as it is now, `stop_from` at the block's start, and the frames it
        // changed on since (a block played in one dispatch renders after it is sequenced: each part hears its own).
        bool hears_stop = false, stop_from = false, shared = false;
        uint8_t stop_count = 0;
        std::array<uint16_t, 4> stop_frames {};
        std::bitset<256> hand {};
        float* print_l = nullptr;
        float* print_r = nullptr;
        // Freeze plan F3: an AUDIO track's engine (its takes' voices), its patterns' takes, ones replaced whose voices
        // still fade, and the pattern tick it followed last (a jump when the next is not the one after).
        AudioTrackEngine audio;
        AudioPack* audio_pack = nullptr;
        AudioPack* audio_leaving = nullptr;
        uint32_t audio_last = UINT32_MAX;
    };
    // The sequencing pass's calls to a track's engine: straight in at the block's first frame (nothing of it rendered
    // yet) or the old way (every track rendered up to each tick), else kept for its job, at block_at (roadmap P2).
    bool deferring = false; // the sequencing pass of a one-dispatch block is running
    bool keep_call(Slot& slot, const EngineCall& call);
    void engine_note_on(Slot& slot, int key, int velocity, float accent = 1, const StepSound& step = {});
    void engine_note_off(Slot& slot, int key);
    void engine_slide(Slot& slot, int from, int to, int velocity, bool glide, float accent);
    void engine_param(Slot& slot, int id, int value);
    void engine_wheel(Slot& slot, float value);
    void engine_bend(Slot& slot, float value); // -1..1
    void engine_pressure(Slot& slot, int key, float value);
    void make_call(unsigned t, Slot& slot, const EngineCall& call); // one kept call, now
    void render_engine(unsigned t, Slot& slot, unsigned at, unsigned frames, bool stopped = false); // the engine's frames [at, at + frames)
    void follow_stop(Slot& slot, unsigned at, bool whole); // freeze plan F2b: whether its engine hears the song stopped
    void play_calls(unsigned t, Slot& slot, unsigned frames);   // the engine over the whole block, each call at its frame
    std::atomic<bool> whole_blocks {true};
    int keyed_pad = -1;                            // this block's: the Sidechain's key track, when its key is one kit pad
    bool split_cuts = false;                       // tests: each job cut where the old way cut every track
    std::array<uint16_t, block + 1> cuts {};       // this block's runs' starts, the old way's cuts
    unsigned cut_count = 0;
    std::atomic<uint32_t> calls_full {0};
    void load_track(unsigned t, const Track& track); // a whole track's sound, kit, mix and effects
    void load_state(const Project& project, bool keep_loops = false); // all but the patterns (keep_loops: the same project at a new rate)
    void run_late(Slot& slot, unsigned frames);       // a swung arp note, when its time comes
    void repeat_step(Slot& slot, int32_t length);     // Note Repeat: every pad held, together
    void arp_write(unsigned t, Slot& slot, int key, int velocity, unsigned rate, unsigned gate); // Seq Record: a repeat into the pattern
    bool clock_free(const Slot& slot) const;          // no other track repeats or arpeggiates on the shared clock
    bool arp_live() const;                            // keys held under Note Repeat or the arp, or a swung step to come
    unsigned next_late() const;                       // frames to the next swung step (~0u: none)
    // Ratchets: the hits due now (at block_at) play, the rest come `frames` nearer; the frames to the next one due
    // (the render loop splits there, so each lands on its frame); and every one dropped (Stop, Play).
    void run_hits(unsigned t, Slot& slot, unsigned frames);
    unsigned next_hit() const;
    void drop_hits();
    void play_hit(unsigned t, Slot& slot, const PendingHit& hit, uint64_t at);
    // Trig conditions: whether `e` plays on pass `pass` of track `t` (its chance too), and how it went kept for PRE and NEI.
    bool trig_plays(unsigned t, Slot& slot, const Event& e, uint32_t pass);
    bool fill = false; // Fill held (Command::Fill)
    void arp_note(Slot& slot, int key, int velocity, int from, int32_t length); // with its pad's pressure
    void apply(const Command& command);
    void select_engine(Slot& slot, EngineKind kind);
    void tick();       // plays whatever falls on this clock tick, then moves on
    void play_tick_notes(unsigned t, Slot& slot, uint32_t at_tick, unsigned pattern_index, uint32_t start, bool seq);
    void play_ahead(unsigned t, Slot& slot, bool seq);
    void restart_ahead(); // a new place (Play, Locate, MTC): every MIDI track's read-ahead from it (issue 59)
    int64_t midi_early(unsigned t) const; // a MIDI track's negative DELAY, frames
    int64_t midi_shift = 0;               // frames this tick's MIDI goes from its frame (play_ahead())
    uint64_t seq_frame() const { return uint64_t(std::max<int64_t>(0, int64_t(frame_now + block_at) + midi_shift)); }
    uint64_t early_frame(unsigned t) const { return uint64_t(std::max<int64_t>(0, int64_t(frame_now + block_at) - midi_early(t))); }
    void play_event(unsigned t, Slot& slot, const Pattern& pattern, size_t n, uint32_t at, uint32_t loop, uint32_t pass, bool sound,
                    Slot::Held& holding); // one note of the pattern on this tick (roadmap P1: a kit lane's loop its own)
    void arp_tick();   // the arps' own clock, running whether the song plays or not
    void bar_start();  // launches queued patterns, or follows the song
    // Freeze plan F2b: track t plays `pack`'s prints from now (nullptr: live again), the one it played fading out;
    // a pack handed back; the frozen tracks waiting for this bar; the engine woken by a hand.
    void set_freeze(unsigned t, FreezePack* pack);
    void retire_freeze(FreezePack* pack);
    void freeze_at_bar();
    void wake(Slot& slot) { slot.awake = true; slot.quiet = 0; }
    void print_tick(unsigned t, Slot& slot, uint32_t at, uint32_t length);
    std::unique_ptr<std::array<float, 2 * block * track_count>> print_frames;
    std::array<FreezePack*, track_count> freeze_next {}; // waiting for the next bar
    static constexpr float sleep_floor = 3.16e-5f; // -90 dB: a frozen track's engine below it a second sleeps
    float pan_centre = 0.7071f;                    // set_pan()'s sides at the centre, as a print was made at
    static constexpr float pan_balance = 1.4142f;  // a track's pan on the engine's picture: unity at the centre
    SpscQueue<64, FreezePack*> done_freezes;
    // Freeze plan F3: audio tracks.
    void set_audio(unsigned t, AudioPack* pack);
    void retire_audio(AudioPack* pack);
    void audio_tick(unsigned t, Slot& slot, uint32_t at);
    SpscQueue<64, AudioPack*> done_audio;
    std::array<AudioPack*, 16> audio_limbo {};
    std::atomic<uint32_t> audio_under {0}, audio_waited {0};
    std::atomic<int> audio_track {-1};   // the track a take is armed or recording for
    std::atomic<int> audio_pattern {-1}; // the pattern it is recording (from its first frame)
    std::array<FreezePack*, 16> freeze_limbo {};
    std::atomic<uint32_t> frozen_now {0}, printing_now {0}, sleeping_now {0}, print_under {0}, print_waited {0};
    // Roadmap P5: track t's follow action planned from where its pattern started (dice rolled now), and brought.
    void plan_follow(unsigned t);
    void follow(unsigned t);
    void stop_track(unsigned t); // a follow action's STOP, or off: what it holds lets go, and it is silent
    FollowDice dice;
    uint16_t scene_tempo = 0; // a scene's tempo waiting for the bar its patterns launch on (0 none)
    uint16_t scene_mute_set = 0, scene_mute_on = 0; // a scene's mutes waiting for that bar: the tracks, those muted
    void scene_mutes_now(); // and now
    // Issue 40: mutes pressed while playing with Rec quantize on wait for the next step of the track's Q (the
    // command's position, in ticks): tracks' mutes and solos (the tracks, and on or off) and kit pads' mutes (a bit a
    // pad), each track's due on one tick (wait_due), set there (mutes_due()) or on Stop.
    static constexpr unsigned pad_wait_words = (sampler_pads + 63) / 64;
    uint16_t mute_wait_set = 0, mute_wait_on = 0, solo_wait_set = 0, solo_wait_on = 0;
    std::array<std::array<uint64_t, pad_wait_words>, track_count> pad_wait_set {}, pad_wait_on {};
    std::array<uint32_t, track_count> wait_due {}, wait_grid {};
    uint16_t pad_waits = 0; // the tracks with a pad's mute waiting
    bool waits_on(unsigned t) const { return ((mute_wait_set | solo_wait_set | pad_waits) >> (t % track_count)) & 1; }
    bool mute_waits(uint32_t grid) const { return running && grid != 0; }
    void wait_for(unsigned t, uint32_t grid);   // the track's waits due on the next multiple of `grid`
    void mutes_due(int track = -1);             // what waits on the track (-1: every track), now
    void publish_mute_waits();
    void pad_mute_now(Slot& slot, unsigned pad, bool on); // a kit pad's mute as set, at once
    // Roadmap P5: the song's tempo lane where the song plays now (0: none, or not the song), worked out each tick.
    uint16_t lane_tempo = 0;
    void follow_lane();
    double tick_samples(uint32_t at) const;
    double ticks_frames(uint32_t from, uint32_t to) const;
    // Motion's points on ticks [from, to) of the pattern as played; `top` the pass's first tick (lanes start again).
    void play_motion(unsigned track, Slot& slot, const Pattern& pattern, uint32_t from, uint32_t to, bool top);
    // Elektron's per track direction (model/step_order.h): what tick `at` of a loop plays, the loop's steps from
    // `base` (written ticks) up to `end` (as played), on pass `pass`; `lane` a kit lane's key + 1 (0 the pattern).
    DirectedTick directed(unsigned track, const Pattern& pattern, uint32_t at, uint32_t end, uint32_t base, uint32_t pass, unsigned lane = 0) const;
    StepSound step_sound(unsigned track, const Slot& slot, const Event& e, uint32_t pass) const; // step SOUND and PLAY
    void move_lane(unsigned track, Slot& slot, unsigned param, unsigned pad, int value); // a Motion lane to `value`
    void restore_lane(unsigned track, Slot& slot, unsigned param, unsigned pad);        // and back to the track's own
    void pad_mute_call(Slot& slot, unsigned pad, bool on); // issue 32: a lane's mute on a kit pad, on its frame
    void restore(Slot& slot);
    // Roadmap P4: a mix lane of track `t` to `value` (its level, pan or a send; the master's chain or a return), and
    // back to the track's own (the master's as set).
    void move_mix(unsigned t, Slot& slot, unsigned param, int value);
    void restore_mix(Slot& slot, unsigned param);
    // The mixer, core step 6: a track's strip on Motion's lanes (model/strip.h is_strip_lane()). strip_set_by: each
    // track's strip as its commands set it, which Motion moves away from and Stop (or a lane's end) brings back.
    void move_strip(unsigned t, unsigned param, int value);
    void restore_strip(unsigned t, unsigned param);
    std::array<Strip, track_count> strip_set_by {};
    // Roadmap P4, modulators (model/modulators.h): each as the shell last sent it, where its cycle is, its random
    // values, a follower's level, and each target's lane: where it stands (`center`: as set, or where Motion has it),
    // as set (`own`), and what was last written there. A lane set from elsewhere tells them (mod_note); the
    // modulators write center + their swing each block (mod_write, which tells none of them).
    struct ModRun {
        Modulator m;
        double phase = 0;
        float last = 0, next = 0, value = 0, env = 0;
        std::array<int, modulator_targets> center {}, own {}, written {};
        std::array<bool, modulator_targets> fresh {};
    };
    std::array<ModRun, modulator_count> mod_runs {};
    std::array<std::atomic<float>, modulator_count> mod_now {};
    uint32_t mod_seed = 0x9e3779b9u;
    bool mods_on = false; // any modulator with a target
    void set_modulator(unsigned n, const Modulator& m, bool keep_centers);
    void run_modulators(unsigned frames);
    void mod_note(unsigned t, unsigned param, unsigned pad, int value, bool own); // a lane set: where it stands (and as set)
    void mod_note_any(unsigned param, int value, bool own);                      // a master lane, on whichever track holds it
    void mod_back(unsigned t, int param = -1, unsigned pad = 0);                   // Motion let go of track t (one lane): as set
    void mod_write(unsigned t, unsigned param, unsigned pad, int value);
    void set_level(Slot& slot, int level);
    void set_pan(Slot& slot, int pan);
    std::array<std::array<int16_t, effect_params>, max_slots> master_base {}; // the master chain's values as set
    std::array<int16_t, bus_count> returns_base {70, 70, 70, 70};             // the returns as set
    void schedule_off(Slot& slot, int key, int32_t samples);
    void run_offs(Slot& slot, unsigned frames);
    void cancel_off(Slot& slot, int key);
    double tick_length() const { return dsp::sample_rate() * 60.0 / (tempo / 10.0) / ticks_per_quarter; } // device frames a clock tick
    double arp_tick_length() const { return tick_length() * arp_tick_ticks; }
    // MIDI out and sync (parity phase 9).
    // Roadmap P4: out as late as everything heard is held back (compensation()), so outside gear lines up with it; a
    // MIDI track's own by a positive DELAY too (a negative one: the sequencer plays it ahead, play_ahead()).
    // Played by hand (`from_hand`, while a command take_commands() marks so is applied, or a note's MIDI taken early,
    // hand_midi()): out at once instead, from the frame it was played at (send_at() plays it a block on), held back
    // only by a positive DELAY, the MIDI thread woken for it (set_hand_waker()).
    int64_t track_delay(unsigned t) const;
    void emit(const MidiOutEvent& e) {
        MidiOutEvent late = e;
        if (from_hand) {
            late.live = true;
            late.made_ns = int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            late.frame = late.frame > block ? late.frame - block : 0;
            if (late.track < track_count && late.dest != MidiDest::Config) late.frame += uint64_t(std::max<int64_t>(0, track_delay(late.track)));
        } else {
            late.frame += pdc.total;
            if (late.track < track_count && late.dest != MidiDest::Config) late.frame = uint64_t(std::max<int64_t>(0, int64_t(late.frame) + track_delay(late.track)));
        }
        if (!midi_events.push(late)) midi_lost.fetch_add(1, std::memory_order_relaxed);
        else if (late.live && hand_waker) hand_waker(hand_waker_context);
    }
    static bool by_hand(const Command& c); // its MIDI out goes at once: a note, pressure, the mod wheel, a knob, MIDI in
    bool hand_midi(const Command& c, unsigned offset); // a note played by hand: its MIDI out as it is taken (true: done)
    bool from_hand = false, hand_early = false; // applying a command played by hand; the waiting one's MIDI went out
    void (*hand_waker)(void*) = nullptr;
    void* hand_waker_context = nullptr;
    bool out_wanted(unsigned t, int key) const;                              // this note of this track goes out
    bool heard(unsigned t) const;                                            // not muted (Audio), nor silenced by another's solo
    // Issue 27, Settings' Mute Notes: a muted track or kit pad starts no new notes and what it plays rings on. Not on
    // an AUDIO track or a frozen one: what they play is audio, which a mute silences either way.
    bool note_mutes = false;
    bool mutes_notes(const Slot& s) const { return note_mutes && s.kind != EngineKind::Audio && !s.freeze; }
    bool note_muted(unsigned t) const { const auto& s = slots[t % track_count]; return s.mute && mutes_notes(s); }
    bool is_midi(unsigned t) const { return slots[t % track_count].kind == EngineKind::Midi; }
    void midi_silenced();                                                    // MIDI tracks muted or soloed out: their notes end
    void midi_note(unsigned t, int key, int velocity, uint64_t at, int64_t length); // and its off after `length` frames (-1 none)
    void midi_note_off(unsigned t, int key, uint64_t at);
    bool midi_hold(unsigned t, int key, int64_t length, uint64_t at); // a tie: the note sounding goes on `length` frames from `at`
    void midi_offs_due(uint64_t until);                                      // offs before `until`, at their frames
    void midi_all_off(unsigned track, uint64_t at);                         // 255: every track's
    // A MIDI track's own parameter beyond port and channel: a controller value goes out as a CC; bank and
    // program (any of the three) as the bank selects that are on, then the program, when it is on.
    void midi_param(unsigned t, int id, int value, uint64_t at);
    void midi_program(unsigned t, uint64_t at, bool force = true, int pattern_index = -1); // force: even when it is what was sent last; pattern_index: that pattern's
    void midi_slot(unsigned t, int slot, int value, uint64_t at);  // a slot's value as what its address says
    void midi_thinned(unsigned frames);                            // slots' values that waited, when their time comes
    void midi_inputs(const int32_t* input, const float* usb, unsigned frames); // MIDI tracks' inputs as their sound (M5)
    std::atomic<uint32_t> cc_sent {0}, cc_thinned {0};
    void sync_stop(uint64_t at);
    void ext_release(uint64_t at);                                           // clock pulses whose frame has come
    std::atomic<unsigned> format_serial {0};
    bool reformatting = false; // audio thread: the rate changed and the Reload has not come yet
    // Parking (park()): the MIDI thread's next serial and whether loads park at all; the audio thread's Park taken
    // (fading out toward it; 0 none), whether it has gone silent, the frames played since (frame_now waits for the
    // loader, then catches up), and the gain the fade takes the output through.
    bool parking_allowed = false;
    uint32_t park_next = 0;
    uint32_t park_wanted = 0;
    bool is_parked = false;
    uint64_t parked_frames = 0;
    Ease park_gain {1, 1, 0};
    std::atomic<uint32_t> parked_serial {0}, resume_serial {0};
    std::atomic<unsigned> parks_done {0};
    std::atomic<uint64_t> blocks_seen {0};
    bool park_over() const { return int32_t(resume_serial.load(std::memory_order_acquire) - park_wanted) >= 0; }
    void play_parked(int32_t* out, unsigned frames, float* stems);
    void go_parked(); // audio thread, the last thing in a block
    SpscQueue<1024, MidiOutEvent> midi_events;
    std::atomic<unsigned> midi_lost {0};
    AudioClock clock;
    uint64_t frame_now = 0;                                                  // frames rendered before this block
    std::array<uint8_t, track_count> out_channel {}, out_mode {};
    struct MidiOff { bool on = false; uint8_t channel = 1, key = 0, track = 0; uint64_t at = 0; MidiDest dest = MidiDest::Config; };
    static MidiOutEvent off_event(const MidiOff& off, uint64_t at) {
        MidiOutEvent e {MidiOutEvent::Kind::NoteOff, off.channel, off.key, 0, 0, at};
        e.dest = off.dest;
        if (off.dest != MidiDest::Config) e.track = off.track;
        return e;
    }
    std::array<MidiOff, 128> midi_offs {};
    // Some off may be on in midi_offs (only midi_note() puts one there). Without one nothing reads them: 3 kB each
    // block and on every note let go, which a block on a cold cache waits for while Pad MIDI Out is off.
    bool midi_offs_held = false;
    SyncOut sync;
    bool sync_started = false;                                                // the song's clocks are going (after any count-in)
    bool sync_clock = false, sync_mtc = false, sync_cv = false;
    unsigned sync_ppqn = 2;
    bool ext_on = false, ext_gate = false;                                    // MIDI clock in: followed; the sequencer waits for it
    uint32_t ext_clocks = 0, ext_base = 0;                                   // pulses let through since Start; the tick it began on
    static constexpr uint32_t ext_pulse_ticks = ticks_per_quarter / 24;      // a MIDI clock pulse
    uint16_t ext_tempo = 0;
    std::array<uint32_t, 64> ext_ring {};                                     // pulses not let through yet: their frames
    unsigned ext_head = 0, ext_tail = 0;
    std::atomic<bool> ext_now {false};
    // MTC in (parity phase 9b). Time code says where the song should be, not its tempo: the sequencer runs on its own
    // tempo, a hair faster or slower (`speed`, within 0.5 %) to stay on the master's time; far off, it is placed again.
    bool mtc_on = false;
    double mtc_at = 0;    // the song's seconds at frame_now, as the sequencer has run them
    double mtc_error = 0; // seconds the master is ahead of the song, filtered against jitter
    double speed = 1;     // the sequencer's pace against the audio clock
    unsigned mtc_misses = 0;
    std::atomic<bool> mtc_now {false};
    std::atomic<int> mtc_error_now {0}, mtc_ppm {0};
    std::atomic<unsigned> mtc_placed {0};
    void mtc_place(double seconds); // the song (or the patterns playing) to where `seconds` falls at the tempo playing now
    void mtc_off();
    void clips_at_bar(bool force);    // the song's clips for song_bar_now (a clip from where it stands in the bar)
    double quarter_samples() const { return tick_length() * ticks_per_quarter; }
    void follow_tempo();  // the tempo now: the global one, or the lead pattern's own (BPM per pattern)
    bool skipped(Slot& slot, unsigned key, uint32_t tick);

    SpscQueue<4096, Command> queue;
    // The first command not applied yet: a timed note before its frame, or one sent after it, waiting behind it.
    Command waiting {};
    bool has_waiting = false;
    unsigned take_commands(unsigned offset, unsigned frames); // those due by `offset`; the offset the next one waits for
    static bool mid_block(Command::Type type);                // applied inside a block too (notes and values)
    static constexpr int32_t timed_reach = 3 * 128;           // further ahead than a hit can be (three blocks): at once
    // Each tick of a pattern playing: its frame (low 32 bits) and tick_now, for place_at(). The audio thread writes
    // the ring, any thread reads it: four quarters and more back (a power of two, so the count wraps round it whole).
    static constexpr unsigned tick_ring_size = 4096;
    static_assert(tick_ring_size >= 4 * ticks_per_quarter && (tick_ring_size & (tick_ring_size - 1)) == 0, "the ring reaches a bar back");
    std::array<std::atomic<uint64_t>, tick_ring_size> tick_ring {};
    std::atomic<uint32_t> tick_head {0};  // ticks written
    std::atomic<float> tick_frames {0};   // frames the newest tick lasts (swing and MTC's pace in)
    std::atomic<uint32_t> timed_on_time {0}, timed_late {0}, timed_worst {0};
    std::atomic<uint32_t> hits_played {0}, trig_kept {0}, trig_dropped {0};
    std::atomic<bool> fill_now {false};
    // Plug-ins: each entry's as the rack offered it; the ones replaced wait in the queue to be freed.
    struct PluginOffer { uint8_t entry = 0; PluginRunner* runner = nullptr; };
    SpscQueue<64, PluginOffer> plugin_offers;
    SpscQueue<64, PluginRunner*> retired_plugins;
    std::array<PluginRunner*, max_plugins> runners {};
    std::array<PluginRunner*, 16> plugin_limbo {}; // replaced ones the queue had no room for yet
    PluginRunner* runner_of(unsigned entry) const { return entry < max_plugins ? runners[entry] : nullptr; }
    void take_plugins(); // the offers waiting, at the start of a block
    struct Offer { uint16_t slot = 0; const SampleData* sample = nullptr; };
    SpscQueue<64, Offer> offers;
    SpscQueue<128, const SampleData*> retired;
    SpscQueue<8, const ProjectPack*> done_packs;
    SpscQueue<8, const TrackPack*> done_tracks;
    SpscQueue<1024, Pattern*> retired_patterns;
    std::array<Pattern*, 64> limbo {}; // retired ones the queue had no room for yet
    void retire(Pattern* pattern);
    SamplerEngine::Table samples {};
    // Issue 24: each sample slot's chop as the shell last sent it, for slice hits (the shell's copies, never changed
    // in place: a newer one replaces it and it goes back through retired_chops to be freed). Freed with the host.
    struct ChopTable {
        SamplerEngine::Chops at {};
        ChopTable() = default;
        ChopTable(const ChopTable&) = delete;
        ChopTable& operator=(const ChopTable&) = delete;
        ~ChopTable() { for (const Slices* s : at) delete s; }
    } chops;
    SpscQueue<1024, const Slices*> retired_chops;
    StreamPager pager;
    TakeRecorder take;
    RecallRing recall_ring; // Audio Recall: the source, always
    Bounce render_out;      // Resample and the song's mixdown
    Bounce audio_in;        // freeze plan F3: an audio track's take, from the input as Sample Record hears it
    std::atomic<bool> usb_submixes {false}; // set_usb_submixes()
    std::atomic<StemRing*> stem_ring {nullptr}; // roadmap P4: stems beside it (each track as it joins the mix, the returns)
    float* live_stems = nullptr;                // audio thread: set_live_stems()'s rows
    void take_stems(unsigned frames);
    void silence_live_stems() { if (live_stems) std::fill(live_stems, live_stems + size_t(StemRing::stem_count) * 2 * block, 0.0f); }
    std::atomic<unsigned> source_now {1};
    std::atomic<unsigned> source_track {0};
    std::atomic<bool> source_post {true};
    uint32_t auto_quiet = 0;       // RecordAutoEnd: a Free take ends after this long quiet (frames at the model rate; 0 never)
    float auto_silence_db = -60;   // and what quiet is
    std::atomic<bool> monitor_on {false};
    std::atomic<bool> usb_direct {false}, usb_heard_now {false};
    std::atomic<float> usb_gain_l {0}, usb_gain_r {0};
    bool usb_wanted = false;              // this block: a monitor wanted USB-C in, which direct monitoring plays instead
    float usb_want_l = 0, usb_want_r = 0; // and at what level each side
    std::atomic<bool> source_taken {false}; // InputTaken: the source's ADC side is routed elsewhere
    std::atomic<uint8_t> loop_monitor {uint8_t(Monitor::Auto)}; // Input Config's Monitor, for loops on a hardware input
    float record_gain = 1;  // linear
    int cue_track = -1;     // a take cued for Rec length Seq starts where this track's pattern comes round
    uint32_t bounce_ticks = 0; // a pattern render ends after this many ticks; 0 with bounce_song
    bool bounce_song = false;  // the song's render: to its end, then its tail
    unsigned block_at = 0;  // frames of this block rendered when tick() runs
    unsigned block_frames = 0; // frames this block renders (its MIDI is taken after it)
    void select_input(const int32_t* input, const float* usb, unsigned frames); // into record_l / record_r
    double until_arp = 0;
    uint32_t arp_ticks = 0;
    uint32_t arp_origin = 0;                    // the sequencer's tick at the arp's tick 0 (Play, a locate)
    SpscQueue<512, ArpWritten> arp_written;     // its notes for Seq Record
    bool click = false;
    float click_level = 0.4f, click_env = 0, click_phase = 0, click_step = 0;
    float click_fall = 0.9985f; // the blip's fall a frame: 0.9985 a 44.1 kHz frame, the same time at any rate
    float record_l[block] {}, record_r[block] {};
    float tap_l[block] {}, tap_r[block] {}; // the Track source this block
    float tap_send_l[bus_count][block] {}, tap_send_r[bus_count][block] {}; // a print host: the tapped kit's pad sends
    unsigned tap_sends = 0;
    // Punch: one on the master or a track. A press while playing waits for
    // the next step of its snap grid.
    struct PendingPunch { bool waiting = false; uint8_t target = 255; PunchKind kind {}; float depth = 1; unsigned snap = 2, tail = 0; };
    Punch punch; // the song's punches
    uint8_t punch_target = 255;
    // Pad FX (parity phase 7b): four punches held at once, in series on their target, each with its own grid wait.
    static constexpr unsigned live_slots = 4;
    struct Live {
        explicit Live(bool history = true) : punch(history) {}
        Punch punch; PendingPunch pending; uint8_t target = 255;
    };
    std::array<Live, live_slots> live;
    std::array<std::array<int16_t, punch_params>, 16> mpc_params {}; // the MPC's Pad FX: K1-K3 each
    const int16_t* params_of(PunchKind kind) const { return punch_is_mpc(kind) ? mpc_params[unsigned(kind) - punch_mpc_first].data() : nullptr; }
    void start_live(unsigned slot, const PendingPunch& p);
    void run_live(uint8_t target, float* left, float* right, unsigned frames);
    // Flex Beat: a time effect over the master for a bar, then Empty again (One Shot) or again (Loop).
    Punch flex;
    uint8_t flex_pad = 0, flex_mode = 0, flex_mix = 100;
    bool flex_quantize = true, flex_waiting = false;
    double flex_left = 0; // samples of its bar left
    void start_flex();
    std::atomic<unsigned> flex_now {0};
    PendingPunch pending;
    bool song_recording = false, punch_from_song = false, live_punch = false;
    uint32_t song_punch_until = 0; // the tick a song punch lets go
    void play_song_punches();      // on each sixteenth of the song: punches written there
    // pad-fx C (round 2): punches kept with patterns, four at once, each in series on its target before the live
    // ones and let go after its length (`due` until then), at the end of its track's loop at the latest. A live
    // punch wins: while one sounds the kept ones are quiet, and they come back for the rest of their length when it
    // lets go. `track`: whose pattern it came from; when that pattern is edited, changed for another or stops, the
    // punch goes with it.
    struct Kept {
        explicit Kept(bool history = true) : punch(history) {}
        Punch punch;
        uint8_t target = 255, track = 255;
        uint32_t until = 0;
        bool on = false, due = false;
        PunchKind kind {};
        float depth = 1;
        unsigned snap = 2, tail = 1;
    };
    std::array<Kept, live_slots> kept;
    // Track t's pattern at `at`, in a loop ending at `end`: punches kept there.
    void play_pattern_punches(unsigned t, const Pattern& pattern, uint32_t at, uint32_t end);
    void sound_kept(Kept& k);                                       // a kept punch heard (again)
    // Track t's editor's loop in its pattern as it plays (`length` ticks): where it starts and how long it is. The
    // shell sends it in the pattern's ticks as written; a track at another speed plays them scaled. False with none
    // set, or one that starts past the pattern's end.
    bool edit_loop(unsigned t, uint32_t length, uint32_t& from, uint32_t& span) const;
    void fit_kept(unsigned t);                                      // track t's pattern or loop changed under its punches
    void drop_kept(unsigned t);                                     // track t left its pattern: its punches end on this tick
    void drop_punches();                                            // the transport stops: the song's punch and the kept ones let go
    static bool same_punch(const Kept& k, const PatternPunch& e) {
        return k.kind == PunchKind(e.kind % punch_count) && k.target == e.target && k.depth == e.depth / 100.0f && k.snap == (e.snap & 3u) &&
               k.tail == e.tail % 3u;
    }
    bool hand_punch() const;                                        // a live punch sounds
    void start_punch(const PendingPunch& p);
    double bar_length() const { return tick_length() * bar; }
    std::array<Slot, track_count> slots;
    // Rendering on several cores (render_pool.h): the workers; the tracks heaviest first by what each took last
    // block, the ones with an engine in that order; each track's time this block (engine and chain), and the total.
    static constexpr int render_priority = 30;            // the audio thread's (base drivers/audio.cpp)
    int pin_audio = -1;                                   // the core the audio thread keeps to (-1: any)
    std::atomic<int> audio_tid {0};                       // the audio thread's id, from its first block (Linux)
    uint64_t parallel_from_ns = 150000;                   // less than this a block: the audio thread alone
    RenderPool workers;
    std::array<uint8_t, track_count> order = [] { std::array<uint8_t, track_count> o {}; for (unsigned t = 0; t < track_count; ++t) o[t] = uint8_t(t); return o; }();
    std::array<uint8_t, track_count> sounding_tracks {};
    std::array<uint32_t, track_count> track_ns {};
    uint64_t track_work_ns = 0;
    std::atomic<float> block_load {0};
    std::array<std::atomic<float>, track_count> track_loads {};
    // The overload guard (roadmap P2): from guard_on of a block's time the next block starts with the busiest track's
    // oldest voice stolen and every released voice's tail hurried; down to guard_off the tails stay hurried.
    static constexpr float guard_on = 0.85f, guard_off = 0.7f;
    float last_share = 0; // the last block's time against its budget
    bool guarding = false;
    std::atomic<uint32_t> guard_stolen {0}, guard_eased {0};
    std::atomic<float> guard_test {-1};
    std::atomic<bool> guard_allowed {false};
    void guard();
    std::atomic<bool> parallel_allowed {true};
    std::atomic<uint64_t> parallel_blocks {0};
    bool plugin_in_two_places() const;
    bool plugin_on_two_buses() const;
    bool plugin_on_two_chains(unsigned first, unsigned count) const; // one plug-in in two of chains first.. first + count - 1
    DelayPool pool;
    std::array<std::array<Effect, max_slots>, mix_chain_count> fx;
    // Resting chains. A chain that has heard silence and given silence for
    // rest_after frames, every effect in it left by each of those blocks as Effect::rest() leaves it, is no longer
    // run: its effects only rest(). Silence is anything under Effect::rest_floor (-571 dB). It is run again from the first block that brings it sound, or in which
    // one of its effects, the tempo or the rate has changed. rest_after is twice the longest delay line and a
    // block: by then nothing a line held can still be on its way out. Chains with a plug-in or a keyed effect
    // never rest (a plug-in has its own gate). Each chain's record is its thread's for the block, like its effects.
    struct ChainRest { uint32_t quiet = 0, stamp = 0; double quarter = 0; float rate = 0; bool asleep = false; char why = 'i'; uint8_t wait = 0; };
    std::array<ChainRest, mix_chain_count> chain_rest {};
    static constexpr uint32_t rest_after = 2 * DelayPool::half + block;
    static constexpr uint8_t rest_retry = 32; // blocks
    uint32_t chain_stamp(unsigned chain) const { uint32_t sum = 0; for (const auto& e : fx[chain]) sum += e.changes(); return sum; }
    std::atomic<uint32_t> chains_resting_now {0};
    std::unique_ptr<std::array<SamplerEngine::PadEffects, track_count>> pad_fx; // each sampler pad's chain
    // Which of a track's pad effects have a kind (bit pad * pad_slots + slot): a load passes the rest without
    // reading them, 8192 effects of about a kilobyte each (8 MB), cold, whose kind() took ~2.4 ms of a load's
    // audio callback on the MPC.
    std::array<std::bitset<sampler_pads * pad_slots>, track_count> pad_fx_kinds {};
    std::array<float, bus_count> returns {0.7f, 0.7f, 0.7f, 0.7f};
    std::array<Ease, bus_count> returns_now {};
    Ease volume_now, monitor_now; // main volume and Monitor as the block plays them
    bool snap_mix = true;         // the next block's eases start at their targets (a project just loaded)
    bool print_host = false;      // set_print_tap(): a print's tap, not the Track source's
    float bus_l[bus_count][block] {}, bus_r[bus_count][block] {}, master_l[block] {}, master_r[block] {};
    void run_chain(unsigned chain, float* left, float* right, unsigned frames);
    // Roadmap P4, keyed Compressors and Gates: each slot's key (model/project.h fx_key_*), and this block's plan of
    // where each key is heard: a track whose chain runs before (its output this block), else the block before (a bus
    // keying a track, a track keyed itself keying another, a loop track's chain). A track keyed by another track runs
    // its chain after the others (keyed_tracks). key_prev: each key source's output the block before.
    std::array<std::array<uint8_t, max_slots>, mix_chain_count> fx_key {};
    std::array<bool, track_count> keyed_track {};   // this block: its chain waits for the tracks that key it
    uint32_t key_sources = 0;                       // a bit a track (0-15) or bus (16-19) some chain is keyed by
    std::array<std::array<float, block>, track_count + bus_count + sub_count> key_prev_l {}, key_prev_r {}; // tracks, buses, submixes
    void plan_keys();
    void key_effect(unsigned chain, unsigned slot, Effect& effect);
    // Where `key` (fx_key_*) is heard by chain `chain` this block, as key_effect() has it.
    void key_buffers(unsigned chain, unsigned key, const float*& l, const float*& r);
    // Mixer (docs/MIXER_PLAN.md): each channel's strip, by strip number (model/project.h), after its chain and before
    // its level and pan. Tracks so far (core step 3).
    std::array<ChannelStrip, strip_channels> strips {};
    bool strip_keyed(unsigned n) const; // its compressor listens to another channel this block
    // The submixes (core step 4) as the block plays them. A track whose output is a submix (Slot::out) is summed into
    // its buffers in place of the master; then each submix runs its chain, its strip and its level and pan, and plays
    // into the master and, as much as each send, the buses. sub_l / sub_r end the block holding what it played,
    // before its mute (a key's sound, as a track's).
    struct SubSlot {
        float gain = 1, pan_left = 0.7071f, pan_right = 0.7071f;
        int16_t level = 100, pan = 0;
        bool mute = false, solo = false;
        std::array<float, bus_count> sends {};
        Ease mix_l, mix_r, audible;
        std::array<Ease, bus_count> sends_now {};
        uint16_t members = 0; // a bit a track with something to play going into it
        bool fed = false;     // something went into it this block
        bool live = false;    // run this block: fed, or its chain or strip still sounding
    };
    std::array<SubSlot, sub_count> submixes {};
    float sub_l[sub_count][block] {}, sub_r[sub_count][block] {};
    void run_submix(unsigned s, unsigned frames);
    bool sub_soloing(unsigned s) const; // its solo counts: a track with something to play goes into it
    // Soloed: its own solo, or its submix's (a submix's solo solos the tracks that play into it).
    bool soloed(const Slot& slot) const {
        if (slot.solo || (slot.out && submixes[(slot.out - 1) % sub_count].solo)) return true;
        for (unsigned n = 0; n < sub_count; ++n) if (((slot.pad_routes >> n) & 1) && submixes[n].solo) return true; // a pad in it
        return false;
    }
    bool solo_on() const; // any solo counts: a track's or a submix's (and the Looper's in render())
    // Core step 5: each bus's return muted; the input heard by takes after its strip (input_wet) or before, the
    // input after its strip as Monitor hears it (wet_l / wet_r, when it is not recorded wet).
    std::array<bool, bus_count> bus_mute {};
    bool input_wet = false;
    float wet_l[block] {}, wet_r[block] {};
    void strip_on_input(unsigned frames, const float*& heard_l, const float*& heard_r);
    void keep_keys(unsigned frames); // this block's key sources, for the next
    static inline const float silence_block[block] {}; // the key of a track with nothing on it
    // Roadmap P4, delay compensation: each block's plan (plan_compensation(), at its start) of how late each line
    // plays, from the latencies along each chain (chain_latency()); `on` while any line delays or eases.
    unsigned chain_latency(unsigned chain) const;
    void plan_compensation();
    void compensate(unsigned line, float* left, float* right, unsigned frames) { lines.run(line, left, right, frames, pdc.delay[line], block_count); }
    struct Compensation {
        std::array<unsigned, CompensationLines::count> delay {};
        unsigned tracks = 0, returns = 0, master = 0, total = 0; // the latest track or loop, bus or Knob FX after it, master
        bool on = false;
    };
    Compensation pdc;
    CompensationLines lines;
    uint64_t block_count = 0; // blocks rendered (a line skipped for a block starts again from silence)
    std::atomic<unsigned> pdc_total {0}, pdc_master {0};
    std::array<std::atomic<unsigned>, track_count> pdc_tracks {}, pdc_engines {};
    std::array<std::atomic<unsigned>, bus_count> pdc_buses {};
    unsigned click_wait = 0; // frames the metronome's blip waits, as late as what it counts
    float knob_pad_sum_l[block] {}, knob_pad_sum_r[block] {}; // compensating: kits' pads into Knob FX, held back apart
    float pad_sub_sum_l[sub_count][block] {}, pad_sub_sum_r[sub_count][block] {}; // compensating: kits' pads sent to submixes, held back apart
    float pad_send_sum_l[bus_count][block] {}, pad_send_sum_r[bus_count][block] {}; // compensating: kits' pad sends, held back apart
    // Knob FX (parity phase 7): one effect the chosen tracks and kit pads go
    // through on their way to the master (`knob_in`, its twin, on the input
    // during a take with Rec Input FX); after its targets let go it rings on
    // until its tail has been quiet for knob_quiet_seconds (longer than any
    // echo's time), or for knob_ring_seconds at most (then cleared, so none
    // of what it held comes back when it is next fed).
    static constexpr float knob_quiet_seconds = 1.5f, knob_ring_seconds = 30.0f, knob_floor = 1e-5f;
    Effect knob, knob_in;
    bool knob_heard = false, knob_rec = false;
    uint16_t knob_tracks = 0;
    std::array<std::array<uint64_t, 2>, track_count> knob_pads {};
    bool knob_ringing = false;           // fed, or its tail still playing out
    uint32_t knob_quiet = 0, knob_rung = 0; // frames its tail has been quiet, and has rung, since it was last fed
    float knob_l[block] {}, knob_r[block] {};
    float key_l[block] {}, key_r[block] {}, aside_l[block] {}, aside_r[block] {};
    // Pad stems (StemRing::pads()): each pad stem's left and right for this block, from the kit's lanes, then at its
    // track's level, pan and mute; the ring this block writes them to, when it takes pad stems.
    std::unique_ptr<float[]> pad_stem_rows = std::make_unique<float[]>(size_t(StemRing::pad_most) * 2 * block);
    StemRing* pad_stem_ring = nullptr;
    float* pad_stem_row(unsigned n, unsigned side) { return pad_stem_rows.get() + size_t(2 * n + side) * block; }
    float key_pad_l[block] {}, key_pad_r[block] {}; // Sidechain on one kit pad: that pad as the sampler left it // Sidechain: the key heard by the detector, and as it joins the master
    void knob_targets(); // hands each kit its pads (none while Knob FX is not heard)
    void knob_command(const Command& c);
    MasterCompressor comp;
    MasterComp comp_settings;
    // The Looper and its four tracks' strips; what each source played the block before, for its loops to record.
    struct LoopStrip {
        float gain = 1, pan_left = 0.7071f, pan_right = 0.7071f, send = 0;
        bool mute = false, solo = false;
        uint8_t bus = 0;
        Ease mix_l, mix_r, audible, send_now; // as the block plays them (Ease)
        float left[block] {}, right[block] {};
    };
    Looper loops;
    std::array<LoopStrip, loop_tracks> loop_strips {};
    std::array<std::atomic<float>, loop_tracks> loop_meters {};
    float source_l[looper::source_count][block] {}, source_r[looper::source_count][block] {};
    unsigned source_frames = 0;
    bool loop_restart = false; // Play or a jump: the loops line up when the sequencer next runs
    bool loop_running = false, loop_counting = false;
    double loop_tick = 0;      // the sequencer's place at this block's first frame (the tick it plays next, less what is left)
    bool loop_strip_command(const Command& c); // Level, Pan, Send, Mute, Solo, SendBus for L1-L4 (track 16-19)
    void play_loops(unsigned frames);           // the loops into their tracks and through their chains (a job)
    void mix_loops(unsigned frames, bool solo); // and those into the master and buses, after the tracks
    bool loops_heard = false;                   // this block's: the Looper was on after its run
    uint32_t loop_ns = 0;                       // what the Looper's job took last block (roadmap P2)
    void keep_sources(const int32_t* input, const float* usb, unsigned frames); // this block's sources for the loops' next one
    void publish_taps(unsigned frames);         // the watched track's voices, for the screen
    std::array<PlayPatterns, track_count> patterns {};
    Song song;
    bool song_mode = false;
    unsigned song_bar_now = 0;
    uint16_t tempo = 1200, global_tempo = 1200;
    // BPM per pattern: the tempo of the pattern launched last (`lead`), when set.
    bool seq_tempo = false;
    std::array<uint16_t, pattern_count> pattern_tempo {};
    uint8_t lead = 0;
    unsigned bar = 4 * ticks_per_quarter, beat = ticks_per_quarter; // the time signature, in ticks
    uint32_t count_left = 0; // Count-In: ticks of the bar counted before the song starts
    uint8_t swing = 50;
    bool running = false;
    uint32_t tick_now = 0;
    double until_tick = 0; // samples until the next tick
    uint32_t random = 0x2545F491u;

    std::atomic<bool> is_playing {false};
    std::atomic<uint32_t> tick_count {0};
    std::array<std::atomic<int>, track_count> positions_now {};
    std::array<std::atomic<uint32_t>, track_count> passes_now {};
    std::array<std::atomic<uint8_t>, track_count> speeds_now {}; // each track's speed (roadmap P1), for place_at()
    std::array<std::atomic<uint8_t>, track_count> directions_now {}; // each track's PlayDirection (model/step_order.h)
    std::atomic<unsigned> count_beat {0};
    std::atomic<uint32_t> count_rest {0}, count_from {0};
    std::atomic<unsigned> tempo_shown {1200};
    std::array<std::atomic<unsigned>, track_count> patterns_now {};
    std::atomic<bool> in_song {false};
    std::atomic<unsigned> bar_now {0};
    std::array<std::atomic<float>, track_count> meters {};
    std::array<std::atomic<float>, strip_channels> peak_l {}, peak_r {}, reductions {}; // strip_meter()
    std::atomic<int> spectrum_tap {-1}; // set_spectrum_tap()
    ScopeRing scope;
    void spectrum_put(unsigned n, const float* l, const float* r, unsigned frames) {
        if (int(n) == spectrum_tap.load(std::memory_order_relaxed)) scope.put(l, r, frames);
    }
    std::atomic<int> tap_track {-1};
    TapBoard tap_board;
    std::atomic<float> output_peak {0};
    std::array<std::atomic<float>, 2> out_levels {}, in_levels {};
    std::atomic<unsigned> lost {0};
    std::atomic<bool> punch_on {false};
    std::atomic<unsigned> live_on {0};
    std::atomic<unsigned> kept_on {0}; // pad-fx C
    std::array<std::atomic<uint32_t>, track_count> follows_now {}; // roadmap P5
    std::atomic<uint32_t> stopped_now {0};
    std::atomic<uint32_t> mute_waits_now {0};
    std::array<std::array<std::atomic<uint64_t>, pad_wait_words>, track_count> pad_waits_now {};
};

} // namespace melodee
