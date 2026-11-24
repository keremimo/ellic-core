// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/model/pattern.h"
#include <unordered_map>
#include <cmath>

namespace melodee {
namespace {
bool before(const Event& a, const Event& b) {
    return a.tick != b.tick ? a.tick < b.tick : a.key != b.key ? a.key < b.key : a.pitch != b.pitch ? a.pitch < b.pitch : a.slice < b.slice;
}
} // namespace

bool Pattern::add(const Event& e) { // the same note (two slices of one pad are two notes) is replaced
    auto at = std::lower_bound(events.begin(), events.end(), e, before);
    if (at != events.end() && at->tick == e.tick && at->key == e.key && at->pitch == e.pitch && at->slice == e.slice) { *at = e; return true; }
    if (events.size() >= max_pattern_events) return false;
    events.insert(at, e);
    return true;
}

bool Pattern::remove(uint32_t tick, unsigned key) {
    for (size_t n = lower(tick); n < events.size() && events[n].tick == tick; ++n)
        if (events[n].key == key) { events.erase(events.begin() + long(n)); return true; }
    return false;
}

Step Pattern::step(unsigned s) const {
    Step out;
    uint32_t start = uint32_t(s) * q(), end = std::min<uint32_t>(start + q(), length);
    unsigned count = 0;
    for (size_t n = lower(start); n < events.size() && events[n].tick < end && count < notes_per_step; ++n) {
        const auto& e = events[n];
        out.notes[count++] = {e.key, e.velocity, uint16_t(e.tick - start)};
        if (count == 1) {
            out.length = uint8_t(std::clamp<unsigned>((e.length + q() / 2) / q(), 1, 255));
            out.chance = e.chance; out.flags = e.flags; out.ratchet = uint8_t(e.hits()); out.ramp = e.ramp; out.condition = e.condition;
        }
    }
    return out;
}

void Pattern::set_step(unsigned s, const Step& value) {
    uint32_t start = uint32_t(s) * q();
    Step old = step(s);
    // The events the step showed, kept for their lengths.
    std::array<Event, notes_per_step> was {};
    unsigned count = 0;
    for (const auto& n : old.notes) {
        if (!n.velocity) continue;
        for (size_t k = lower(start + n.offset); k < events.size() && events[k].tick == start + n.offset; ++k)
            if (events[k].key == n.key) { was[count++] = events[k]; break; }
        remove(start + n.offset, n.key);
    }
    for (const auto& n : value.notes) {
        if (!n.velocity) continue;
        Event e;
        e.tick = start + std::min<unsigned>(n.offset, q() - 1);
        e.length = note_ticks(uint64_t(value.length) * q());
        if (value.length == old.length)
            for (unsigned k = 0; k < count; ++k) if (was[k].key == n.key) e.length = was[k].length;
        e.key = n.key; e.velocity = n.velocity; e.chance = value.chance; e.flags = value.flags;
        e.ratchet = uint8_t(std::clamp<unsigned>(value.ratchet, 1, max_ratchet)); e.ramp = value.ramp;
        e.condition = uint8_t(value.condition < trig_count ? value.condition : 0);
        bool same_ratchet = value.ratchet == old.ratchet && value.ramp == old.ramp, same_condition = value.condition == old.condition;
        for (unsigned k = 0; k < count; ++k) // the note it was keeps its pitch, and its ratchet while the step's is unchanged
            if (was[k].key == n.key && was[k].velocity) {
                e.pitch = was[k].pitch; e.pitch_ramp = was[k].pitch_ramp;
                e.slice = was[k].slice; e.sample = was[k].sample; // a slice hit stays its slice, a step's sample its own
                if (same_ratchet) { e.ratchet = was[k].ratchet; e.ramp = was[k].ramp; }
                if (same_condition) e.condition = was[k].condition;
                was[k].velocity = 0;
                break;
            }
        add(e);
    }
}

void Pattern::sort() { std::stable_sort(events.begin(), events.end(), before); }

void Pattern::cut(uint32_t tick) {
    ramp_plan.planned = false;
    events.erase(events.begin() + long(lower(tick)), events.end());
    motion.erase(motion.begin() + long(motion_lower(tick)), motion.end());
    punches.erase(std::remove_if(punches.begin(), punches.end(), [tick](const PatternPunch& e) { return e.tick >= tick; }), punches.end()); // pad-fx C
    for (auto& e : punches) e.length = std::min(e.length, tick - e.tick); // one reaching past it ends there
}

bool Pattern::clear_notes_and_motion() {
    if (events.empty() && motion.empty()) return false;
    ramp_plan.planned = false;
    events.clear();
    motion.clear();
    return true;
}

namespace {
bool motion_before(const MotionPoint& a, const MotionPoint& b) {
    return a.tick != b.tick ? a.tick < b.tick : a.param != b.param ? a.param < b.param : a.pad < b.pad;
}
} // namespace

bool Pattern::write_motion(const MotionPoint& m) {
    ramp_plan.planned = false; // a plan holds for the points it was made for
    auto at = std::lower_bound(motion.begin(), motion.end(), m, motion_before);
    if (at != motion.end() && at->tick == m.tick && at->on(m.param, m.pad)) { *at = m; return true; }
    if (motion.size() >= motion_points) return false;
    motion.insert(at, m);
    return true;
}

bool Pattern::clear_lane(unsigned param, unsigned pad) {
    ramp_plan.planned = false;
    auto before = motion.size();
    motion.erase(std::remove_if(motion.begin(), motion.end(), [&](const MotionPoint& m) { return m.on(param, pad); }), motion.end());
    return motion.size() != before;
}

unsigned Pattern::lane_points(unsigned param, unsigned pad) const {
    unsigned n = 0;
    for (const auto& m : motion) n += m.on(param, pad);
    return n;
}

const MotionPoint* Pattern::lock_at(uint32_t tick, unsigned param, unsigned pad) const {
    for (size_t n = motion_lower(tick); n < motion.size() && motion[n].tick == tick; ++n)
        if (motion[n].on(param, pad) && motion[n].lock()) return &motion[n];
    return nullptr;
}

int Pattern::lane_value(uint32_t tick, unsigned param, unsigned pad, int own) const {
    for (size_t n = std::min(motion_lower(tick + 1), motion.size()); n-- > 0;) {
        if (!motion[n].on(param, pad) || motion[n].lock()) continue;
        size_t to = planned_ramp_end(n); // looked up on a planned copy (the audio thread's), else worked out
        return to < motion.size() ? ramp_value(motion[n], motion[to], tick) : motion[n].value;
    }
    // Before the first point: its value, when it was set by hand (a planned copy keeps those points in its leads).
    if (ramps_planned()) {
        for (uint32_t n : ramp_plan.leads)
            if (n < motion.size() && motion[n].on(param, pad)) return motion[n].value;
        return own;
    }
    const MotionPoint* first = first_point(param, pad);
    return first && first->ramp ? first->value : own;
}

const MotionPoint* Pattern::first_point(unsigned param, unsigned pad) const {
    for (const auto& m : motion) {
        if (m.tick > length) break;
        if (m.on(param, pad) && !m.lock() && m.within(length)) return &m;
    }
    return nullptr;
}

size_t Pattern::ramp_end(size_t n) const {
    if (n >= motion.size() || !motion[n].ramp || motion[n].lock()) return motion.size();
    const auto& from = motion[n];
    for (size_t k = n + 1; k < motion.size() && motion[k].tick <= length; ++k)
        if (motion[k].on(from.param, from.pad) && !motion[k].lock()) return motion[k].tick > from.tick ? k : motion.size();
    return motion.size();
}

std::vector<uint32_t> Pattern::ramp_ends() const {
    std::vector<uint32_t> ends(motion.size(), uint32_t(motion.size()));
    std::unordered_map<uint16_t, uint32_t> next; // a lane (param, pad): its next point after the one in hand, locks aside
    for (size_t n = motion.size(); n-- > 0;) {
        const auto& m = motion[n];
        if (m.lock()) continue;
        uint16_t lane = uint16_t(m.param << 8 | m.pad);
        if (m.ramp)
            if (auto it = next.find(lane); it != next.end() && motion[it->second].tick > m.tick) ends[n] = it->second;
        if (m.tick <= length) next[lane] = uint32_t(n); // past the end: no ramp goes there
    }
    return ends;
}

int Pattern::ramp_value(uint32_t from, int a, uint32_t to, int b, uint32_t tick) {
    if (tick <= from || to <= from) return a;
    if (tick >= to) return b;
    int64_t span = int64_t(to - from), along = int64_t(tick - from), rise = int64_t(b) - a;
    int64_t step = rise * along * 2 / span; // twice the rise so far, to round half away from zero
    return a + int((step + (step < 0 ? -1 : 1)) / 2);
}

const MotionPoint* Pattern::point_at(uint32_t tick, unsigned param, unsigned pad) const {
    for (size_t n = motion_lower(tick); n < motion.size() && motion[n].tick == tick; ++n)
        if (motion[n].on(param, pad) && !motion[n].lock()) return &motion[n];
    return nullptr;
}

bool Pattern::erase_point(uint32_t tick, unsigned param, unsigned pad) {
    auto* m = point_at(tick, param, pad);
    if (!m) return false;
    ramp_plan.planned = false;
    motion.erase(motion.begin() + (m - motion.data()));
    return true;
}

bool Pattern::move_point(uint32_t tick, uint32_t to, unsigned param, unsigned pad) {
    auto* m = point_at(tick, param, pad);
    if (!m || to > length) return false;
    if (to == tick) return true;
    for (size_t n = motion_lower(to); n < motion.size() && motion[n].tick == to; ++n)
        if (motion[n].on(param, pad)) return false; // the lane holds one there already (a lock too)
    MotionPoint moved = *m;
    moved.tick = to;
    motion.erase(motion.begin() + (m - motion.data()));
    return write_motion(moved); // drops the plan
}

unsigned Pattern::ramps_at_once() const {
    std::vector<std::pair<uint32_t, int>> edges; // +1 where a ramp starts, -1 where it ends (ends first on a tick)
    auto ends = ramp_ends();
    for (size_t n = 0; n < motion.size(); ++n)
        if (size_t to = ends[n]; to < motion.size()) { edges.push_back({motion[n].tick, 1}); edges.push_back({motion[to].tick, -1}); }
    std::sort(edges.begin(), edges.end());
    int now = 0, most = 0;
    for (const auto& e : edges) most = std::max(most, now += e.second);
    return unsigned(most);
}

void Pattern::plan_ramps() {
    ramp_plan = {};
    ramp_plan.planned = true;
    ramp_plan.points = uint32_t(motion.size());
    bool any = false;
    for (const auto& m : motion) any |= m.ramp && !m.lock();
    if (!any) return;
    ramp_plan.ends = ramp_ends();
    std::vector<bool> seen(256 * 256); // a lane: param, pad
    for (size_t n = 0; n < motion.size(); ++n) {
        const auto& m = motion[n];
        if (m.tick > length) break;
        if (m.lock() || !m.within(length)) continue;
        size_t lane = size_t(m.param) * 256 + m.pad;
        if (seen[lane]) continue;
        seen[lane] = true;
        if (m.ramp && m.tick > 0) ramp_plan.leads.push_back(uint32_t(n));
    }
}

void Pattern::sort_motion() {
    ramp_plan.planned = false;
    std::stable_sort(motion.begin(), motion.end(), motion_before);
}

void Pattern::set_lane_length(unsigned key, uint32_t ticks) {
    lanes.erase(std::remove_if(lanes.begin(), lanes.end(), [&](const LaneLength& l) { return l.key == key; }), lanes.end());
    if (!ticks || ticks >= length) return;
    LaneLength l;
    l.key = uint8_t(key); l.length = ticks;
    lanes.insert(std::lower_bound(lanes.begin(), lanes.end(), l, [](const LaneLength& a, const LaneLength& b) { return a.key < b.key; }), l);
}

void Pattern::fit_lanes() {
    lanes.erase(std::remove_if(lanes.begin(), lanes.end(), [&](const LaneLength& l) { return !l.length || l.length >= length; }), lanes.end());
}

bool double_pattern(Pattern& p, uint32_t max_length, bool copy) {
    if (p.length * 2 > max_length) return false;
    std::vector<MotionPoint> ends; // points on the end: the first half's, and with a copy the new end's too
    for (size_t n = p.motion_lower(p.length); n < p.motion.size() && p.motion[n].tick == p.length; ++n)
        if (p.motion[n].within(p.length)) ends.push_back(p.motion[n]);
    p.cut(p.length); // nothing hidden past the end comes back
    size_t count = copy ? p.events.size() : 0;
    for (size_t n = 0; n < count && p.events.size() < max_pattern_events; ++n) { Event e = p.events[n]; e.tick += p.length; p.events.push_back(e); }
    size_t points = copy ? p.motion.size() : 0; // its motion too, as far as there is room
    for (size_t n = 0; n < points && p.motion.size() < motion_points; ++n) { MotionPoint m = p.motion[n]; m.tick += p.length; p.motion.push_back(m); }
    for (auto m : ends) {
        if (!p.point_at(m.tick, m.param, m.pad) && !p.lock_at(m.tick, m.param, m.pad)) p.write_motion(m); // the copy's own point or lock there plays instead
        if (copy) { m.tick += p.length; p.write_motion(m); }
    }
    size_t kept = copy ? p.punches.size() : 0; // and its kept punches (pad-fx C)
    for (size_t n = 0; n < kept && p.punches.size() < max_pattern_punches; ++n) { PatternPunch e = p.punches[n]; e.tick += p.length; p.punches.push_back(e); }
    p.length *= 2;
    return true;
}

void half_pattern(Pattern& p) {
    p.length = std::max<uint32_t>(p.length / 2, 1);
    p.cut(p.length); // a kept punch running into the second half ends with the first
    p.fit_lanes();
}

void split_punches(Pattern& p) {
    std::vector<PatternPunch> punches;
    for (auto e : p.punches) {
        if (e.tick >= p.length) { punches.push_back(e); continue; } // past the end: not played, as it was
        if (e.length > p.length) e.length = p.length - e.tick;
        if (punches.size() < max_pattern_punches) lay_punch(punches, e, p.length);
    }
    std::stable_sort(punches.begin(), punches.end(), [](const PatternPunch& a, const PatternPunch& b) { return a.tick < b.tick; });
    p.punches = std::move(punches);
}

void scale_pattern(Pattern& p, bool slower) {
    std::vector<Event> moved;
    moved.reserve(p.events.size());
    for (auto e : p.events) {
        if (e.tick >= p.length) continue;
        e.tick = slower ? e.tick * 2 : e.tick / 2;
        e.length = note_ticks(slower ? uint64_t(e.length) * 2 : e.length / 2u);
        if (e.tick < p.length) moved.push_back(e);
    }
    p.events.clear();
    for (const auto& e : moved) p.add(e);
    std::vector<MotionPoint> points; // its motion with them
    for (auto m : p.motion) {
        if (!m.within(p.length)) continue; // hidden past the end: not brought in at double speed, as the notes
        m.tick = slower ? m.tick * 2 : m.tick / 2;
        if (m.hold) m.hold = note_ticks(slower ? uint64_t(m.hold) * 2 : m.hold / 2u);
        if (m.within(p.length)) points.push_back(m);
    }
    p.motion.clear();
    for (const auto& m : points) p.write_motion(m);
    for (auto& l : p.lanes) l.length = slower ? l.length * 2 : std::max<uint32_t>(l.length / 2, 1); // its lanes with it
    p.fit_lanes();
    std::vector<PatternPunch> punches; // and its kept punches (pad-fx C)
    for (auto e : p.punches) {
        if (e.tick >= p.length) continue; // hidden past the end
        e.tick = slower ? e.tick * 2 : e.tick / 2;
        e.length = slower ? e.length * 2 : std::max<uint32_t>(e.length / 2, 1);
        if (e.tick >= p.length) continue;
        e.length = std::min(e.length, p.length - e.tick); // twice as long, it ends with the pattern
        punches.push_back(e);
    }
    p.punches = std::move(punches);
}

uint32_t quantise_tick(const Pattern& p, uint32_t tick) { return quantise_tick(p, tick, p.q()); }

uint32_t quantise_tick(const Pattern& p, uint32_t tick, unsigned grid) {
    grid = std::max(grid, 1u);
    uint32_t snapped = (tick + grid / 2) / grid * grid;
    return snapped >= p.length ? 0 : snapped; // past the last step: the first, next time round
}

void time_correct_all(Pattern& p, const std::array<uint64_t, 3>* keys, unsigned swing, int shift) {
    time_correct(p, [keys](unsigned key) { return !keys || (key < 192 && (((*keys)[key / 64] >> (key % 64)) & 1)); }, swing, shift);
}

namespace {
// A note's own random numbers, from where it is (the same each time, so a pattern plays the same feel pass after pass
// until its amount changes): two in -1..1, mostly small (the sum of two). Seeded by the tick on the 96-a-quarter
// clock files kept before, so a note on one of those ticks draws what it drew then; one between them, its own.
double humanize_draw(const Event& e, unsigned salt) {
    constexpr uint32_t fine = ticks_per_quarter / file_ticks_per_quarter;
    uint32_t h = (e.tick / fine) * 2654435761u ^ (uint32_t(e.key) << 16 | uint32_t(uint8_t(e.pitch)) << 8) ^ salt * 40503u
               ^ (e.tick % fine) * 0x9e3779b9u;
    auto next = [&] { h ^= h << 13; h ^= h >> 17; h ^= h << 5; return double(h % 10001) / 10000.0; };
    return next() + next() - 1.0;
}
} // namespace

Pattern feel_pattern(const Pattern& p) {
    if (p.feel.plain()) return p;
    Pattern out = p;
    const auto& f = p.feel;
    double q = p.q(), late = division_triplet(p.division) ? 0 : q * (double(std::clamp<unsigned>(f.swing, 50, 75)) - 50) / 50.0;
    unsigned steps = unsigned(f.groove_offsets.size());
    out.events.clear();
    out.events.reserve(p.events.size());
    for (auto e : p.events) {
        if (e.tick >= p.lane_length(e.key)) continue; // hidden past the pattern's end, or its kit lane's own
        double t = e.tick;
        if (f.strength) { // toward the nearest step of Q, the second of each pair late by the swing
            double step = std::floor(t / q + 0.5), target = step * q + (std::fmod(step, 2.0) == 1.0 ? late : 0.0);
            t += (target - t) * f.strength / 100.0;
        }
        double velocity = e.velocity;
        if (steps && f.groove_amount) { // the groove's step nearest the note as written
            unsigned at = unsigned(std::floor(double(e.tick) / std::max<uint16_t>(f.groove_step, 1) + 0.5)) % steps;
            t += f.groove_offsets[at] * f.groove_amount / 100.0;
            double level = (at < f.groove_levels.size() ? f.groove_levels[at] : 100) / 100.0;
            velocity *= 1.0 + (level - 1.0) * f.groove_amount / 100.0;
        }
        if (f.humanize) t += humanize_draw(e, 1) * q / 4.0 * f.humanize / 100.0;
        if (f.humanize_velocity) velocity += humanize_draw(e, 2) * 20.0 * f.humanize_velocity / 100.0;
        int64_t loop = std::max<uint32_t>(p.lane_length(e.key), 1), at = int64_t(std::floor(t + 0.5));
        e.tick = uint32_t(((at % loop) + loop) % loop); // round its loop (a kit lane's own): early on the first step plays at the end
        e.velocity = uint8_t(std::clamp(int(velocity + 0.5), 1, 127));
        out.add(e);
    }
    return out;
}

Pattern speed_pattern(const Pattern& p, unsigned speed) {
    if (speed >= speed_count || speed == speed_normal) return p;
    Speed s = speed_of(speed);
    auto scale = [&](uint64_t ticks) { return (ticks * s.den + s.num / 2) / s.num; }; // to the nearest tick
    Pattern out = p;
    out.length = uint32_t(std::max<uint64_t>(scale(p.length), 1));
    // A note's length and a lock's hold as they play: at x1/64 longer than any pattern as written (note_ticks' cap).
    const uint64_t longest = std::max<uint64_t>(max_pattern_ticks, out.length);
    auto held = [&](uint64_t ticks) { return uint32_t(std::clamp<uint64_t>(scale(ticks), 1, longest)); };
    out.events.clear();
    for (auto e : p.events) {
        if (e.tick >= p.length) continue;
        e.tick = uint32_t(std::min<uint64_t>(scale(e.tick), out.length - 1));
        e.length = held(e.length);
        out.add(e);
    }
    out.motion.clear();
    for (auto m : p.motion) {
        if (!m.within(p.length)) continue; // hidden past the end: not played on the last tick
        m.tick = m.tick == p.length ? out.length : uint32_t(std::min<uint64_t>(scale(m.tick), out.length - 1)); // the end stays the end
        if (m.hold) m.hold = held(m.hold);
        out.write_motion(m);
    }
    for (auto& l : out.lanes) l.length = uint32_t(std::max<uint64_t>(scale(l.length), 1));
    out.fit_lanes();
    out.punches.clear();
    for (auto e : p.punches) { // pad-fx C: kept punches where they fall at this speed, none past the end
        if (e.tick >= p.length) continue;
        e.tick = uint32_t(std::min<uint64_t>(scale(e.tick), out.length - 1));
        e.length = uint32_t(std::clamp<uint64_t>(scale(e.length), 1, out.length - e.tick));
        out.punches.push_back(e);
    }
    return out;
}

Pattern played_pattern(const Pattern& p, unsigned speed) {
    Pattern out = speed_pattern(feel_pattern(p), speed);
    out.plan_ramps(); // for the audio thread, which only looks it up
    return out;
}

void groove_of(const Pattern& source, Feel& feel) {
    unsigned step = std::max<unsigned>(feel.groove_step, 1);
    unsigned steps = std::clamp<unsigned>((std::min<uint32_t>(source.length, 64 * step) + step / 2) / step, 1, 64);
    double span = double(steps) * step;
    std::vector<double> offsets(steps, 0.0), levels(steps, 0.0);
    std::vector<unsigned> counts(steps, 0);
    double heard = 0;
    unsigned count = 0;
    for (const auto& e : source.events) { // each hit to the step nearest it, a longer pattern folded onto the first steps
        if (e.tick >= source.length) break;
        double at = std::fmod(double(e.tick), span), near = std::floor(at / step + 0.5);
        unsigned index = unsigned(near) % steps;
        offsets[index] += at - near * step;
        levels[index] += e.velocity;
        ++counts[index];
        heard += e.velocity;
        ++count;
    }
    feel.groove_offsets.clear(); feel.groove_levels.clear();
    if (!count) return;
    double mean = heard / count;
    for (unsigned n = 0; n < steps; ++n) {
        double offset = counts[n] ? offsets[n] / counts[n] : 0.0, level = counts[n] ? levels[n] / counts[n] / mean : 1.0;
        feel.groove_offsets.push_back(int16_t(std::clamp(int(std::lround(offset)), -int(step) / 2, int(step) / 2)));
        feel.groove_levels.push_back(uint8_t(std::clamp(int(std::lround(level * 100)), 25, 175)));
    }
}

StepEvents step_events(const Pattern& pattern, unsigned step) {
    uint32_t from = step * pattern.q(), to = std::min<uint32_t>(from + pattern.q(), pattern.length);
    StepEvents out;
    out.first = pattern.lower(from);
    size_t end = out.first;
    while (end < pattern.events.size() && pattern.events[end].tick < to) ++end;
    out.count = unsigned(end - out.first);
    return out;
}

} // namespace melodee
