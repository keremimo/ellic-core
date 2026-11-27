// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/audio_track.h"
#include "melodee/model/follow.h"
#include "melodee/model/limits.h"
#include "melodee/model/trig.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// A pattern as a list of note events on the sequencer's clock (960 ticks a
// quarter), up to 128 bars long. Q is the grid the pages step through and
// quantising snaps to; the notes themselves sit on any tick. The Steps and
// Roll pages see the pattern through step(): the notes that start inside one
// step of the Q grid, with where in the step each one falls.
namespace melodee {

// Q: the grid a pattern is edited and quantised on, as on the MPC.
enum class Division : uint8_t {
    Quarter, QuarterTriplet, Eighth, EighthTriplet, Sixteenth, SixteenthTriplet, ThirtySecond, ThirtySecondTriplet,
    SixtyFourth, Count
};
inline unsigned division_ticks(Division d) {
    constexpr unsigned q = ticks_per_quarter;
    static const unsigned ticks[] {q, q * 2 / 3, q / 2, q / 3, q / 4, q / 6, q / 8, q / 12, q / 16};
    return ticks[unsigned(d) < unsigned(Division::Count) ? unsigned(d) : 4];
}
inline const char* division_name(Division d) {
    static const char* names[] {"1/4", "1/4T", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64"};
    return names[unsigned(d) < unsigned(Division::Count) ? unsigned(d) : 4];
}
inline bool division_triplet(Division d) { return unsigned(d) % 2 == 1 && d != Division::SixtyFourth; }
// Files from before parity phase 2 numbered five divisions: 1/8, 1/8T, 1/16, 1/16T, 1/32.
inline Division legacy_division(unsigned value) {
    static const Division map[] {Division::Eighth, Division::EighthTriplet, Division::Sixteenth, Division::SixteenthTriplet, Division::ThirtySecond};
    return value < 5 ? map[value] : Division::Sixteenth;
}

enum StepFlag : uint8_t { step_accent = 1, step_slide = 2, step_tie = 4, step_mute = 8 }; // step_mute: kept, not played (Roll)

// Step PLAY (Step Edit's SOUND cards): the way a kit or keygroup pad's sample plays for a note, kept in its flags'
// bits 4-5: as the pad plays it (its Reverse, a slice's own), forward, reversed, or one or the other at each hit.
enum class StepPlay : uint8_t { Pad, Forward, Reverse, Random, Count };
constexpr unsigned step_play_shift = 4;
constexpr uint8_t step_play_bits = uint8_t(3u << step_play_shift);
inline StepPlay step_play(uint8_t flags) { return StepPlay((flags & step_play_bits) >> step_play_shift); }
inline uint8_t with_play(uint8_t flags, StepPlay play) {
    return uint8_t((flags & ~step_play_bits) | (unsigned(play) & 3u) << step_play_shift);
}
inline const char* step_play_name(StepPlay play) {
    static const char* const names[] {"PAD", "FWD", "REV", "RND"};
    return names[unsigned(play) & 3u];
}
// Event::slice: a slice of the sample's chop at random each hit (Step Edit's SLICE RND).
constexpr uint8_t slice_random = 255;

// A ratchet (roadmap P1): a note played as so many hits spread evenly over its length.
constexpr unsigned max_ratchet = 8;
constexpr int max_pitch_ramp = 24; // semitones a kit pad's last hit sits from its first

// A note's length, a lock's hold or a kit lane's ticks as kept: at least one tick, at most the longest pattern.
inline uint32_t note_ticks(uint64_t ticks) { return uint32_t(std::clamp<uint64_t>(ticks, 1, max_pattern_ticks)); }

// One note of the pattern: where it starts, how long it is held (ticks).
struct Event {
    uint32_t tick = 0;
    uint32_t length = sixteenth_ticks; // 1..max_pattern_ticks (note_ticks)
    uint8_t key = 0, velocity = 100, chance = 100, flags = 0;
    int8_t pitch = 0; // a sampler pad's: semitones from its root (16 Levels, an instrument's keys)
    // Ratchet: hits inside its length, 1..8; the last hit's velocity in percent off the note's (-100..100), the
    // hits between going evenly; on a kit pad the last hit's semitones over the first (-24..24) too.
    uint8_t ratchet = 1;
    int8_t ramp = 0, pitch_ramp = 0;
    uint8_t condition = trig_always; // which passes it plays on, beside its chance (model/trig.h)
    uint8_t cutoff = 0; // roadmap P3, a kit pad's: 16 Levels Filter's cutoff + 1 it was played at (0: the pad's own)
    // A kit pad's, played on Chop while Seq Record ran or set on Step Edit's SLICE: the slice + 1 of the chop of the
    // sample it plays, as the chop is now (0: the pad's own region; slice_random: one at random each hit). The kit
    // keeps no pad for it (model/slice_hits.h).
    uint8_t slice = 0;
    // Step SOUND (Elektron's sample lock): the slot + 1 of Project::samples a kit or keygroup pad plays for this note
    // in place of its own sample, through the pad (its tune, envelope, filter, Pad FX and sends); 0: the pad's own.
    uint16_t sample = 0;
    bool operator==(const Event& o) const {
        return tick == o.tick && length == o.length && key == o.key && velocity == o.velocity && chance == o.chance && flags == o.flags &&
               pitch == o.pitch && ratchet == o.ratchet && ramp == o.ramp && pitch_ramp == o.pitch_ramp && condition == o.condition &&
               cutoff == o.cutoff && slice == o.slice && sample == o.sample;
    }
    bool operator!=(const Event& o) const { return !(*this == o); }
    unsigned hits() const { return std::clamp<unsigned>(ratchet, 1, max_ratchet); }
    // Hit `n` of the ratchet (0 the first): how hard, and its semitones over `pitch` (a kit pad's ramp).
    int hit_velocity(int played, unsigned n) const {
        unsigned last = hits() - 1;
        if (!last || !ramp) return played;
        double scale = 1.0 + ramp / 100.0 * double(std::min(n, last)) / double(last);
        return std::clamp(int(played * scale + 0.5), 1, 127);
    }
    int hit_pitch(unsigned n) const {
        unsigned last = hits() - 1;
        if (!last || !pitch_ramp) return 0;
        double at = pitch_ramp * double(std::min(n, last)) / double(last);
        return int(at < 0 ? at - 0.5 : at + 0.5);
    }
    // Copies what plays it besides where and which (chance, flags, the ratchet, the condition, a sample of its own),
    // for a note rewritten in place.
    void take_play(const Event& o) {
        chance = o.chance; flags = o.flags; ratchet = o.ratchet; ramp = o.ramp; pitch_ramp = o.pitch_ramp; condition = o.condition;
        sample = o.sample;
    }
};

// The pages' view of one step: up to four notes starting in it.
struct Note {
    uint8_t key = 0, velocity = 0; // velocity 0: no note
    uint16_t offset = 0;           // ticks after the step's start
};
struct Step {
    std::array<Note, notes_per_step> notes {};
    uint8_t length = 1;    // in steps of the pattern's Q
    uint8_t chance = 100;  // percent
    uint8_t flags = 0;
    uint8_t ratchet = 1;   // the first note's (Event::ratchet); set_step keeps each note's own
    int8_t ramp = 0;
    uint8_t condition = 0; // the first note's (Event::condition), kept the same way
    bool empty() const { for (auto n : notes) if (n.velocity) return false; return true; }
    const Note* first() const { for (auto& n : notes) if (n.velocity) return &n; return nullptr; }
};

// A point of Motion (roadmap P1: kept by its pattern, at tick resolution): from `tick` on, a lane holds `value`. A
// lane is an engine parameter, an effect's on the track's chain or the mod wheel (`param`, model/limits.h's
// motion_* numbers), or a kit pad's own value on `pad`. A lock (`hold` above 0: a pot turned with a step or note
// held) holds for that many ticks only, then the lane goes back to what it was. A ramp (`ramp`: a point set by hand
// on Motion) does not hold: from its tick the lane goes in a straight line to the lane's next point (locks aside)
// up to the pattern's end, and holds when there is none. A lane whose first point is a ramp holds that point's
// value from the pattern's start. A point on the end itself (`tick` == the pattern's length, not a lock) is never
// played: it is where the ramp before it ends, the value the lane reaches as the pattern comes round.
struct MotionPoint {
    uint32_t tick = 0;
    uint8_t param = 0, pad = 0;
    int16_t value = 0;
    uint32_t hold = 0; // 0..max_pattern_ticks
    bool ramp = false;
    bool lock() const { return hold > 0; }
    bool on(unsigned p, unsigned k) const { return param == p && pad == k; }
    bool operator==(const MotionPoint& o) const {
        return tick == o.tick && param == o.param && pad == o.pad && value == o.value && hold == o.hold && ramp == o.ramp;
    }
    bool operator!=(const MotionPoint& o) const { return !(*this == o); }
    // Inside a pattern of `length` ticks: before its end, or a ramp on the end itself. A recorded point there (a
    // knob turned across the bar line, a pattern shortened after recording) stays hidden past the end, as before ramps.
    bool within(uint32_t length) const { return tick < length || (tick == length && ramp && !lock()); }
};

// What the audio thread needs of a pattern's ramps, worked out beforehand on another thread (Pattern::plan_ramps(),
// which played_pattern() runs): each point's ramp_end(), and the points that set their lane from the pattern's start
// (the first of their lane, a ramp, after tick 0). Not saved or compared. An edit through the Pattern's own calls
// drops it (write_motion, erase_point, move_point, clear_lane, cut, sort_motion), and it holds only for as many
// points as it was made for (ramps_planned()): a copy edited after it was planned is never read with a stale one.
struct RampPlan {
    bool planned = false;
    uint32_t points = 0;         // motion.size() when it was made
    std::vector<uint32_t> ends;  // per point of motion, its ramp_end(); empty when no point is a ramp
    std::vector<uint32_t> leads; // by tick
};

// A kit lane of its own length (roadmap P1, polymeter inside a kit): `key`'s notes loop over `length` ticks from
// where the pattern began, shorter than the pattern; its passes counted on their own (trig conditions).
struct LaneLength {
    uint8_t key = 0;
    uint32_t length = 0;
    bool operator==(const LaneLength& o) const { return key == o.key && length == o.length; }
};

// The Euclidean rhythm last laid on a kit lane (roadmap P1, model/generators.h): its hits over its steps, turned.
struct EuclidLane {
    uint8_t key = 0, hits = 0, steps = 16, rotate = 0;
    bool operator==(const EuclidLane& o) const { return key == o.key && hits == o.hits && steps == o.steps && rotate == o.rotate; }
};

// A pattern's feel (roadmap P1, Time Correct heard live): how its notes play without rewriting them. The audio thread
// plays feel_pattern() of the pattern; the pages and the file keep the notes as written.
struct Feel {
    uint8_t strength = 0;          // 0..100: notes drawn that far toward the nearest step of the pattern's Q
    uint8_t swing = 50;            // 50..75: with strength, every second step of a straight Q that late, as Time Correct swings
    uint8_t humanize = 0;          // 0..100: each note early or late, up to a quarter of a step of Q (fixed per note)
    uint8_t humanize_velocity = 0; // 0..100: each note softer or harder, up to 20
    // A groove taken from another pattern (groove_of()): how far each of its steps falls off the grid and how hard it
    // plays against the mean, laid on these notes by the step nearest each, `groove_amount` of it.
    uint16_t groove_from = 0;      // 0 none; else 1 + track * pattern_count + pattern, where it was taken from (groove_source)
    uint8_t groove_amount = 100;
    uint16_t groove_step = sixteenth_ticks;
    std::vector<int16_t> groove_offsets; // ticks, within half a step
    std::vector<uint8_t> groove_levels;  // percent, 25..175
    bool plain() const { return !strength && !humanize && !humanize_velocity && (groove_offsets.empty() || !groove_amount); }
    bool operator==(const Feel& o) const {
        return strength == o.strength && swing == o.swing && humanize == o.humanize && humanize_velocity == o.humanize_velocity &&
               groove_from == o.groove_from && groove_amount == o.groove_amount && groove_step == o.groove_step &&
               groove_offsets == o.groove_offsets && groove_levels == o.groove_levels;
    }
    bool operator!=(const Feel& o) const { return !(*this == o); }
};

// pad-fx C (round 2): a punch kept in a pattern, looping with it like Motion: from `tick` (of the pattern) for
// `length` ticks, punch `kind` (engine/punch.h) on `target` (255 the master, else a track) at `depth` percent, on the
// grid it was played with (`snap`, engine/punch.h punch_snaps) and letting go as it did (`tail`, punch_tails).
// It ends with its pattern: one held across the loop point is kept in two parts (lay_punch), and the engine never
// sounds one past the end of the loop playing, however the pattern was shortened since.
struct PatternPunch {
    uint32_t tick = 0, length = sixteenth_ticks;
    uint8_t kind = 0, target = 255, depth = 100, snap = 2, tail = 1;
    bool operator==(const PatternPunch& o) const {
        return tick == o.tick && length == o.length && kind == o.kind && target == o.target && depth == o.depth && snap == o.snap && tail == o.tail;
    }
};
constexpr unsigned max_pattern_punches = 32;
// A punch as it was heard, in the sequencer's ticks since Play (`until` 0: still sounding); `felt`: its pad's
// pressure has set its depth (the deepest is kept).
struct PunchSpan { uint64_t from = 0, until = 0; uint8_t kind = 0, target = 255, depth = 100, snap = 2, tail = 1; bool felt = false; };
// A punch laid in a pattern `length` ticks long: one reaching past the end in two parts, up to the end and the rest
// from the start (the engine carries on from the one into the other), never round to where it began.
inline void lay_punch(std::vector<PatternPunch>& out, PatternPunch p, uint32_t length) {
    if (p.tick >= length) return;
    uint32_t room = length - p.tick, rest = p.length > room ? std::min(p.length - room, p.tick) : 0;
    p.length = std::min(p.length, room);
    out.push_back(p);
    if (!rest || out.size() >= max_pattern_punches) return;
    p.tick = 0; p.length = rest;
    out.push_back(p);
}
// The take (what B3 KEEP keeps, and the page draws): the spans heard up to one pass (`length` ticks) back from where
// the latest let go (now, while one still sounds), however long ago, as punches of the pattern playing `position`
// ticks into its loop at `now`, so they fall where they were heard (one across the loop point in two parts); at
// most max_pattern_punches, oldest first.
inline std::vector<PatternPunch> heard_punches(const PunchSpan* spans, unsigned count, uint64_t now, uint32_t position, uint32_t length) {
    std::vector<PatternPunch> out;
    if (!length || !count) return out;
    uint64_t last = 0;
    for (unsigned n = 0; n < count; ++n) last = std::max(last, spans[n].until ? spans[n].until : now);
    uint64_t window = last >= length ? last - length : 0, start = now >= position ? now - position : 0; // where this pass began
    for (unsigned n = 0; n < count && out.size() < max_pattern_punches; ++n) {
        uint64_t from = std::max(spans[n].from, window), until = spans[n].until ? spans[n].until : now;
        if (until <= from || until <= window) continue;
        int64_t at = (int64_t(from) - int64_t(start)) % int64_t(length);
        if (at < 0) at += int64_t(length);
        PatternPunch p;
        p.tick = uint32_t(at);
        p.length = uint32_t(std::clamp<uint64_t>(until - from, 1, length));
        p.kind = spans[n].kind; p.target = spans[n].target; p.depth = spans[n].depth; p.snap = spans[n].snap; p.tail = spans[n].tail;
        lay_punch(out, p, length);
    }
    return out;
}

struct Pattern {
    uint32_t length = 16 * sixteenth_ticks; // ticks
    Division division = Division::Sixteenth; // Q
    std::vector<Event> events;               // by tick, then key
    std::vector<MotionPoint> motion;         // by tick, then lane; at most motion_points
    std::vector<LaneLength> lanes;           // keys looping on their own, by key
    Feel feel;                               // how its notes play (roadmap P1)
    std::vector<EuclidLane> euclids;         // the rhythms laid on its kit lanes, by key (roadmap P1)
    std::vector<PatternPunch> punches;       // punches kept with it (pad-fx C), by tick
    PatternFollow follow;                    // what comes after it by itself (roadmap P5, model/follow.h)
    uint8_t program = 0;                     // a MIDI track's: the program it sends as it starts, 1..128; 0 the track's own
    AudioRegion audio;                       // an AUDIO track's take, played from its top (freeze plan F3, model/audio_track.h)
    // Free length (shell/free_length.h): while it is empty, its first take sets its length, a bar at a time. K1 to
    // a number of bars, Half or Double Seq, or a launch that gives it a length turn it off.
    bool free = true;
    RampPlan ramp_plan;                      // its ramps worked out for the audio thread (played_pattern())

    unsigned q() const { return division_ticks(division); }
    // Steps of Q the pattern holds (a last part step counts).
    unsigned steps() const { return std::max(1u, unsigned((length + q() - 1) / q())); }
    bool empty() const { return (events.empty() || events.front().tick >= length) && !audio.used(); }
    // Free length counts only while the pattern is empty.
    bool is_free() const { return free && empty(); }
    bool operator==(const Pattern& o) const {
        return length == o.length && division == o.division && events == o.events && motion == o.motion && lanes == o.lanes &&
               feel == o.feel && euclids == o.euclids && punches == o.punches && follow == o.follow && program == o.program &&
               audio == o.audio && is_free() == o.is_free();
    }
    bool operator!=(const Pattern& o) const { return !(*this == o); }
    // The first event at or after `tick`.
    size_t lower(uint32_t tick) const {
        return size_t(std::lower_bound(events.begin(), events.end(), tick, [](const Event& e, uint32_t t) { return e.tick < t; }) - events.begin());
    }
    // Adds a note in its place; one already on that tick with that key is replaced. False when full.
    bool add(const Event& e);
    // Removes the note with that key on that tick; false when there is none.
    bool remove(uint32_t tick, unsigned key);
    // The notes starting in step `s` of Q (the first four), and setting them:
    // notes the step showed and no longer holds go, the rest keep their
    // place in the step and, while the step's length is unchanged, their own
    // length. Notes past the fourth in a step are left as they are.
    Step step(unsigned s) const;
    void set_step(unsigned s, const Step& value);
    // Keeps the events in order after they were moved in place.
    void sort();
    // Drops every note (and Motion point, and kept punch) from `tick` on; a kept punch reaching past it ends there.
    void cut(uint32_t tick);
    // Clear pattern: its notes and its Motion go, as the question says (its length, Q, lanes, kept punches and the
    // rest stay). False when it held neither.
    bool clear_notes_and_motion();

    // Motion (roadmap P1). The first point at or after `tick`.
    size_t motion_lower(uint32_t tick) const {
        return size_t(std::lower_bound(motion.begin(), motion.end(), tick, [](const MotionPoint& m, uint32_t t) { return m.tick < t; }) - motion.begin());
    }
    // Puts a point in its place; one on the same tick and lane is replaced. False when full.
    bool write_motion(const MotionPoint& m);
    // A lane's points: removed (locks too: false when there were none), counted, or any there.
    bool clear_lane(unsigned param, unsigned pad);
    unsigned lane_points(unsigned param, unsigned pad) const;
    bool has_lane(unsigned param, unsigned pad) const { return lane_points(param, pad) > 0; }
    // The lock on a lane at `tick`, if one starts there.
    const MotionPoint* lock_at(uint32_t tick, unsigned param, unsigned pad) const;
    // What a lane holds at `tick` as recorded (locks aside): the last point at or before it (on its way to the next
    // when it is a ramp); before the first, that one's value when it is a ramp (set by hand: the lane holds it from
    // the pattern's start), else `own`.
    int lane_value(uint32_t tick, unsigned param, unsigned pad, int own) const;
    // A lane's first point (locks aside) up to the pattern's end, if any.
    const MotionPoint* first_point(unsigned param, unsigned pad) const;
    // Where the ramp from point `n` goes: the next point on its lane (locks aside) up to the pattern's end (one on
    // the end too), else motion.size() (it holds). Points that are not ramps go nowhere.
    size_t ramp_end(size_t n) const;
    // ramp_end() of every point at once, in one pass (for drawing and planning a whole pattern).
    std::vector<uint32_t> ramp_ends() const;
    // A ramp's value at `tick`, from `a` at `from` to `b` at `to` (rounded to the nearest); and between two points.
    static int ramp_value(uint32_t from, int a, uint32_t to, int b, uint32_t tick);
    static int ramp_value(const MotionPoint& from, const MotionPoint& to, uint32_t tick) {
        return ramp_value(from.tick, from.value, to.tick, to.value, tick);
    }
    // Motion edited by hand: the point (not a lock) on a lane at `tick`, if one is there; that point taken out; or
    // moved to `to` (false when another point of the lane is there, or it is past the pattern's end; on it is kept).
    const MotionPoint* point_at(uint32_t tick, unsigned param, unsigned pad) const;
    MotionPoint* point_at(uint32_t tick, unsigned param, unsigned pad) {
        return const_cast<MotionPoint*>(static_cast<const Pattern&>(*this).point_at(tick, param, pad));
    }
    bool erase_point(uint32_t tick, unsigned param, unsigned pad);
    bool move_point(uint32_t tick, uint32_t to, unsigned param, unsigned pad);
    // The most lanes going in a line at any one tick (at most motion_ramps_at_once play: an edit keeps under it).
    unsigned ramps_at_once() const;
    // The audio thread's: ramp_plan worked out (any thread but the audio thread); whether it holds for the points
    // there are now; and ramp_end() looked up in it (worked out afresh when it does not: never on the audio thread,
    // which plays no ramps of a pattern not planned).
    void plan_ramps();
    bool ramps_planned() const { return ramp_plan.planned && ramp_plan.points == motion.size(); }
    size_t planned_ramp_end(size_t n) const {
        if (!ramps_planned()) return ramp_end(n);
        return n < ramp_plan.ends.size() && ramp_plan.ends[n] < motion.size() ? ramp_plan.ends[n] : motion.size();
    }
    void sort_motion();

    // Kit lanes (roadmap P1): the ticks `key` loops over (its own, else the pattern's), whether it has its own, and
    // setting it (the pattern's length or more, or 0: none of its own).
    uint32_t lane_length(unsigned key) const {
        for (const auto& l : lanes) if (l.key == key) return std::max<uint32_t>(std::min(l.length, length), 1);
        return length;
    }
    bool own_lane(unsigned key) const { return lane_length(key) < length; }
    void set_lane_length(unsigned key, uint32_t ticks);
    void fit_lanes(); // after the pattern's length changed: lanes as long as it, or longer, go
};

// A groove's source in Feel::groove_from, and back: the track and pattern it was taken from. Files keep it as before
// the banks (1 + track * 16 + pattern, legacy_groove) where that reaches it, in GRVB where it does not.
constexpr uint16_t groove_source(unsigned track, unsigned pattern) { return uint16_t(1 + track * pattern_count + pattern % pattern_count); }
constexpr unsigned groove_track(uint16_t from) { return from ? (from - 1u) / pattern_count : 0; }
constexpr unsigned groove_pattern(uint16_t from) { return from ? (from - 1u) % pattern_count : 0; }
constexpr uint16_t legacy_groove(uint16_t from) { // 0 when its pattern is past the sixteenth
    return from && groove_pattern(from) < legacy_patterns ? uint16_t(1 + groove_track(from) * legacy_patterns + groove_pattern(from)) : 0;
}
constexpr uint16_t groove_of_legacy(uint16_t from) {
    return from ? groove_source((from - 1u) / legacy_patterns, (from - 1u) % legacy_patterns) : 0;
}

// A track's playback speed (roadmap P1, Elektron's per track speed): its patterns play at num / den of the tempo, the
// notes as written. The audio thread plays them scaled (speed_pattern: ticks times den / num); the pages and the file
// keep them as written.
struct Speed { uint8_t num, den; };
constexpr unsigned speed_count = 11, speed_normal = 7;
constexpr Speed speeds[speed_count] {{1, 64}, {1, 32}, {1, 16}, {1, 8}, {1, 4}, {1, 2}, {3, 4}, {1, 1}, {3, 2}, {2, 1}, {4, 1}};
inline Speed speed_of(unsigned speed) { return speeds[speed < speed_count ? speed : speed_normal]; }
inline const char* speed_name(unsigned speed) {
    static const char* names[] {"\xc3\x97" "1/64", "\xc3\x97" "1/32", "\xc3\x97" "1/16", "\xc3\x97" "1/8", "\xc3\x97\xc2\xbc",
                                "\xc3\x97\xc2\xbd", "\xc3\x97\xc2\xbe", "\xc3\x97" "1", "\xc3\x97" "1\xc2\xbd", "\xc3\x97" "2", "\xc3\x97" "4"}; // x1/64 .. x4
    return names[speed < speed_count ? speed : speed_normal];
}
// Files and builds from before x1/64 .. x4 numbered five speeds, x1/2, x3/4, x1, x3/2 and x2 (TSPD): one of those to
// today's, and today's to the nearest an older build plays (x1 for one it does not have).
inline unsigned speed_of_legacy(unsigned legacy) { return legacy < 5 ? legacy + 5 : speed_normal; }
inline unsigned legacy_speed(unsigned speed) { return speed >= 5 && speed <= 9 ? speed - 5 : 2; }
// A place in a pattern as written, in the transport's ticks of a track at `speed` (to the nearest, as speed_pattern()
// scales), and back (as the audio thread publishes a track's place, EngineHost::position_of()).
inline uint64_t played_ticks(uint64_t written, unsigned speed) { Speed s = speed_of(speed); return (written * s.den + s.num / 2) / s.num; }
inline uint64_t written_ticks(uint64_t played, unsigned speed) { Speed s = speed_of(speed); return played * s.num / s.den; }
Pattern speed_pattern(const Pattern& p, unsigned speed);
// What the audio thread plays of a pattern on a track at `speed`: its feel laid on, then its speed.
Pattern played_pattern(const Pattern& p, unsigned speed);

// Roadmap P1, the feel: the pattern as the audio thread plays it (its notes moved and their velocities scaled as its
// Feel says; the same pattern when plain), and a groove taken from a pattern's notes (none when it has none).
Pattern feel_pattern(const Pattern& p);
void groove_of(const Pattern& source, Feel& feel);

// The events starting in step `step` of a pattern's Q: events[first, first + count) (Step Edit lists them).
struct StepEvents { size_t first = 0; unsigned count = 0; };
StepEvents step_events(const Pattern& pattern, unsigned step);

// Edits of a whole pattern (Shift + Pad 2, 3, 6, 7 and Time Correct). Each
// keeps the pattern's notes in order; two notes landing on the same tick
// with the same key become one.
bool double_pattern(Pattern& p, uint32_t max_length, bool copy = true); // Double Seq: twice as long, the notes twice (or the new half empty); false past max_length
void half_pattern(Pattern& p);                        // Half Seq: the first half
void scale_pattern(Pattern& p, bool slower);          // Half / Double Speed: every note twice as far apart, or half; the length stays
// A file from before kept punches ended with their pattern: one reaching past the end was played on from the start.
// Each as it was heard, in two parts (lay_punch); one longer than its pattern (left by Half Seq then) ends with it.
void split_punches(Pattern& p);
// The tick a recorded note lands on with Rec Quantize: the nearest step of
// Q; past the last step, the first (next time round).
uint32_t quantise_tick(const Pattern& p, uint32_t tick);
// The same onto a grid of `grid` ticks.
uint32_t quantise_tick(const Pattern& p, uint32_t tick, unsigned grid);
// Time Correct: notes `chosen` (a bit a key, 0..191) onto the nearest step
// of Q, every second step of a straight Q late by swing (50..75 %), then
// moved by shift ticks (wrapping inside the pattern, or the note's kit lane
// of its own length; a note hidden past either's end stays as it is).
template <class Chosen> void time_correct(Pattern& p, Chosen&& chosen, unsigned swing, int shift);
void time_correct_all(Pattern& p, const std::array<uint64_t, 3>* keys, unsigned swing, int shift); // keys nullptr: every note

} // namespace melodee

namespace melodee {
template <class Chosen> void time_correct(Pattern& p, Chosen&& chosen, unsigned swing, int shift) {
    unsigned q = p.q();
    double late = division_triplet(p.division) ? 0 : q * (double(swing) - 50) / 50.0;
    std::vector<Event> moved;
    moved.reserve(p.events.size());
    for (auto e : p.events) {
        int64_t loop = p.lane_length(e.key); // the loop it plays in: the pattern's, or its kit lane's own
        if (e.tick < loop && chosen(e.key)) {
            uint32_t step = (e.tick + q / 2) / q;
            int64_t at = int64_t(step) * q + (step % 2 ? int64_t(late + 0.5) : 0) + shift;
            e.tick = uint32_t(((at % loop) + loop) % loop);
        }
        moved.push_back(e);
    }
    p.events.clear();
    for (const auto& e : moved) p.add(e);
}

} // namespace melodee
