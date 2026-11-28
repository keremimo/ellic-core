// SPDX-License-Identifier: MIT
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/model/print.h"
#include "melodee/model/project_file.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>

namespace melodee {

const PrintRecord* PrintTable::find(unsigned track, unsigned pattern) const {
    for (const auto& r : records)
        if (r.track == track && r.pattern == pattern) return &r;
    return nullptr;
}

void PrintTable::put(const PrintRecord& record) {
    auto at = std::lower_bound(records.begin(), records.end(), record, [](const PrintRecord& a, const PrintRecord& b) {
        return a.track != b.track ? a.track < b.track : a.pattern < b.pattern;
    });
    if (at != records.end() && at->track == record.track && at->pattern == record.pattern) *at = record;
    else records.insert(at, record);
}

Mix print_mix(const Track&) { return Mix {}; }

bool hears_transport(const Project& project, unsigned track) {
    const Track& tr = project.tracks[track % track_count];
    return tr.engine == EngineKind::Plugin && tr.plugin < max_plugins && project.plugins[tr.plugin].used() &&
           project.plugins[tr.plugin].transport;
}

void print_project(Project& project, PatternBank& bank, unsigned track, unsigned pattern) {
    const unsigned t = track % track_count, p = pattern % pattern_count;
    for (unsigned k = 0; k < track_count; ++k)
        for (unsigned q = 0; q < pattern_count; ++q)
            if (k != t || q != p) bank.at[k][q] = Pattern {};
    auto& kept = bank.at[t][p];
    kept.punches.clear();      // punches stay live
    kept.follow = PatternFollow {}; // the pattern plays on
    kept.motion.erase(std::remove_if(kept.motion.begin(), kept.motion.end(), [](const MotionPoint& m) { return is_mix_lane(m.param) || is_strip_lane(m.param); }),
                      kept.motion.end());

    // The project as new, with only what one track's sound hears of it.
    auto out = std::make_unique<Project>();
    out->tempo = uint16_t(project.tempo_of(p));
    out->signature = project.signature;
    out->swing = project.swing;
    out->root = project.root;
    out->scale = project.scale;
    Track& tr = out->tracks[t];
    tr = project.tracks[t];
    const Track plain {};
    tr.set_name("");
    tr.mix = print_mix(project.tracks[t]); // unity and centred
    tr.strip = Strip {}; // the mixer's strip stays live on a frozen track (docs/MIXER_PLAN.md decision 1): never printed
    tr.pattern = uint8_t(p);
    tr.chain = {}; tr.chain_count = 0; tr.chain_at = 0;
    tr.in_scene = {}; tr.scene_mutes = {}; tr.off = false; // which pattern it plays in each scene, muted or not: not part of its sound
    tr.last_note = plain.last_note; tr.last_pad = plain.last_pad;
    tr.fader_target = plain.fader_target; tr.fader_on = plain.fader_on; tr.fader_learned = plain.fader_learned; tr.fader_pin = plain.fader_pin;
    tr.wheel = 0; tr.bend = 0;
    tr.midi_channel = 0;
    std::vector<bool> locked(sample_slots, false); // step SOUND: the samples its notes play of their own
    for (const auto& e : kept.events) if (e.sample && e.sample <= sample_slots) locked[e.sample - 1u] = true;
    for (unsigned n = 0; n < sample_slots; ++n) {
        bool used = locked[n];
        for (const auto& pad : tr.pads) used |= pad.plays(uint16_t(n));
        if (!used) continue;
        out->samples.edit(n) = project.samples[n];
        if (project.chops[n].used()) out->chops.edit(n) = project.chops[n];
    }
    for (unsigned e = 0; e < max_plugins; ++e) {
        bool used = tr.engine == EngineKind::Plugin && tr.plugin == e;
        for (const auto& s : tr.fx.slots) used |= s.kind == plugin_effect_kind && unsigned(s.params[0]) == e;
        if (used) out->plugins[e] = project.plugins[e];
    }
    for (unsigned m = 0; m < modulator_count; ++m) { // only targets on the track's own sound
        Modulator mod = project.modulators[m];
        for (auto& target : mod.targets)
            if (target.used() && (target.track != t || is_mix_lane(target.param) || is_strip_lane(target.param))) target = ModTarget {};
        if (mod.target_count()) out->modulators[m] = mod;
    }
    project = *out;
}

uint64_t print_hash(const Project& project, const PatternBank& bank, const std::array<std::vector<uint8_t>, max_plugins>& states,
                    PrintTap tap, PrintTail tail, unsigned rate) {
    std::array<std::vector<uint8_t>, max_plugins> used;
    for (unsigned e = 0; e < max_plugins; ++e)
        if (project.plugins[e].used()) used[e] = states[e];
    auto bytes = save_project(project, bank, &used);
    // Without the sequence links (SEQL): which pattern plays in which scene, and the mutes kept there, are not the
    // print's sound, and the hashes stay those of prints made before links.
    for (size_t at = 8; at + 8 <= bytes.size();) {
        size_t size = size_t(bytes[at + 4]) | size_t(bytes[at + 5]) << 8 | size_t(bytes[at + 6]) << 16 | size_t(bytes[at + 7]) << 24;
        if (at + 8 + size > bytes.size()) break;
        if (std::memcmp(bytes.data() + at, "SEQL", 4) == 0) { bytes.erase(bytes.begin() + long(at), bytes.begin() + long(at + 8 + size)); break; }
        at += 8 + size;
    }
    bytes.push_back(uint8_t(tap));
    bytes.push_back(uint8_t(tail));
    for (unsigned k = 0; k < 4; ++k) bytes.push_back(uint8_t(rate >> (8 * k)));
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : bytes) { h ^= b; h *= 0x100000001b3ull; }
    return h;
}

std::string prints_folder(const std::string& project_path) {
    if (project_path.empty()) return {};
    std::filesystem::path p(project_path);
    return (p.parent_path() / (p.stem().string() + " Prints")).string();
}

std::string print_pattern_name(unsigned pattern) {
    pattern %= pattern_count;
    return std::string(1, char('A' + pattern / pattern_pads)) + std::to_string(pattern % pattern_pads + 1);
}

std::string print_file_name(unsigned track, unsigned pattern) {
    return "Track " + std::to_string(track % track_count + 1) + " " + print_pattern_name(pattern) + ".wav";
}

} // namespace melodee
