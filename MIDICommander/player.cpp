#include "player.hpp"
#include "bundled_music.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

import mm.audio;
import mm.fs;
import mm.mcu;

namespace midicommander {
namespace {

constexpr std::size_t maximum_file = 128 * 1024;
constexpr std::size_t maximum_tracks = 32;
constexpr std::size_t maximum_voices = 8;
constexpr unsigned int requested_rate = 32'000;
constexpr std::array<unsigned int, 12> semitone_ratio_q16{{
    65536, 69433, 73561, 77936, 82570, 87480,
    92682, 98193, 104032, 110218, 116772, 123715
}};

std::array<std::byte, maximum_file> file_bytes{};
const char* last_error = "Ready";

struct Event {
    enum class Kind { None, NoteOn, NoteOff, Sustain, AllOff } kind = Kind::None;
    unsigned int channel = 0;
    unsigned int note = 0;
    unsigned int value = 0;
};

class Song {
public:
    enum class Result { Waiting, Event, Done, Invalid };

    [[nodiscard]] const MidiInfo& info() const { return info_; }

    [[nodiscard]] bool load(std::span<const std::byte> bytes) {
        data_ = bytes;
        count_ = 0;
        tick_ = 0;
        time_us_ = 0;
        remainder_ = 0;
        tempo_ = 500'000;
        info_ = {};
        first_tempo_ = true;
        if (bytes.size() < 14 || !tag(0, "MThd") ||
            be32(4) < 6 || be32(4) > bytes.size() - 8) return false;
        const auto format = be16(8);
        const auto declared = be16(10);
        division_ = be16(12);
        if (format > 1 || declared == 0 || declared > maximum_tracks ||
            (format == 0 && declared != 1) || division_ == 0 ||
            (division_ & 0x8000u) != 0) return false;
        std::size_t position = 8u + be32(4);
        while (count_ < declared && position + 8 <= data_.size()) {
            const auto length = be32(position + 4);
            if (length > data_.size() - position - 8) return false;
            if (tag(position, "MTrk")) {
                Track& track = tracks_[count_++];
                track = {};
                track.pos = position + 8;
                track.end = track.pos + length;
                if (!next_delta(track)) return false;
            }
            position += 8u + length;
        }
        if (count_ != declared) return false;

        info_.format = format;
        info_.tracks = static_cast<unsigned int>(declared);
        info_.division = division_;
        info_.file_size = bytes.size();
        info_.tempo_us = 500'000;
        info_.bpm = 120;
        info_.valid = true;

        const auto saved_tracks = tracks_;
        Event event{};
        unsigned int notes = 0;
        for (unsigned int sim_step = 0; sim_step < 200'000; ++sim_step) {
            const auto res = next(UINT64_MAX, event);
            if (res == Result::Done || res == Result::Invalid) break;
            if (res == Result::Event && event.kind == Event::Kind::NoteOn && event.value > 0) {
                ++notes;
            }
        }
        info_.duration_s = static_cast<unsigned int>(time_us_ / 1'000'000ULL);
        info_.note_count = notes;

        tracks_ = saved_tracks;
        tick_ = 0;
        time_us_ = 0;
        remainder_ = 0;
        tempo_ = 500'000;

        return true;
    }

    [[nodiscard]] Result next(std::uint64_t elapsed_us, Event& event) {
        event = {};
        std::size_t chosen = count_;
        for (std::size_t i = 0; i < count_; ++i)
            if (!tracks_[i].done &&
                (chosen == count_ || tracks_[i].next_tick < tracks_[chosen].next_tick))
                chosen = i;
        if (chosen == count_) return Result::Done;
        const auto next_tick = tracks_[chosen].next_tick;
        const auto scaled = (next_tick - tick_) * tempo_ + remainder_;
        const auto due = time_us_ + scaled / division_;
        if (due > elapsed_us) return Result::Waiting;
        time_us_ = due;
        remainder_ = scaled % division_;
        tick_ = next_tick;
        return consume(tracks_[chosen], event) ? Result::Event : Result::Invalid;
    }

private:
    struct Track {
        std::size_t pos = 0;
        std::size_t end = 0;
        std::uint64_t next_tick = 0;
        std::uint8_t running = 0;
        bool done = false;
    };

    [[nodiscard]] unsigned int byte(std::size_t at) const {
        return static_cast<unsigned int>(data_[at]);
    }
    [[nodiscard]] unsigned int be16(std::size_t at) const {
        return (byte(at) << 8) | byte(at + 1);
    }
    [[nodiscard]] std::uint32_t be32(std::size_t at) const {
        return (byte(at) << 24) | (byte(at + 1) << 16) |
               (byte(at + 2) << 8) | byte(at + 3);
    }
    [[nodiscard]] bool tag(std::size_t at, const char* value) const {
        return at + 4 <= data_.size() && byte(at) == static_cast<unsigned int>(value[0]) &&
               byte(at + 1) == static_cast<unsigned int>(value[1]) &&
               byte(at + 2) == static_cast<unsigned int>(value[2]) &&
               byte(at + 3) == static_cast<unsigned int>(value[3]);
    }
    [[nodiscard]] bool vlq(Track& track, std::uint32_t& value) const {
        value = 0;
        for (unsigned int i = 0; i < 4; ++i) {
            if (track.pos >= track.end) return false;
            const auto next = byte(track.pos++);
            value = (value << 7) | (next & 0x7fu);
            if ((next & 0x80u) == 0) return true;
        }
        return false;
    }
    [[nodiscard]] bool next_delta(Track& track) const {
        if (track.pos == track.end) {
            track.done = true;
            return true;
        }
        std::uint32_t delta = 0;
        if (!vlq(track, delta)) return false;
        track.next_tick += delta;
        return true;
    }
    [[nodiscard]] bool consume(Track& track, Event& event) {
        if (track.pos >= track.end) return false;
        auto status = byte(track.pos++);
        unsigned int first = 0;
        if (status < 0x80u) {
            if (track.running == 0) return false;
            first = status;
            status = track.running;
        } else if (status < 0xf0u) {
            track.running = static_cast<std::uint8_t>(status);
            if (track.pos >= track.end) return false;
            first = byte(track.pos++);
        } else {
            track.running = 0;
            if (status == 0xffu) {
                if (track.pos >= track.end) return false;
                const auto type = byte(track.pos++);
                std::uint32_t length = 0;
                if (!vlq(track, length) || length > track.end - track.pos) return false;
                if (type == 0x51u && length == 3) {
                    const auto tempo = (byte(track.pos) << 16) |
                                       (byte(track.pos + 1) << 8) | byte(track.pos + 2);
                    if (tempo == 0) return false;
                    tempo_ = tempo;
                    if (first_tempo_) {
                        info_.tempo_us = tempo;
                        info_.bpm = (60'000'000u + tempo / 2u) / tempo;
                        first_tempo_ = false;
                    }
                } else if ((type == 0x03u || (type == 0x01u && info_.title[0] == '\0')) &&
                           length > 0 && info_.title[0] == '\0') {
                    const auto copy_len = length < info_.title.size() - 1 ? length : info_.title.size() - 1;
                    for (std::size_t i = 0; i < copy_len; ++i) {
                        const auto c = byte(track.pos + i);
                        info_.title[i] = (c >= 32 && c <= 126) ? static_cast<char>(c) : ' ';
                    }
                    std::size_t end = copy_len;
                    while (end > 0 && info_.title[end - 1] == ' ') --end;
                    info_.title[end] = '\0';
                }
                track.pos += length;
                if (type == 0x2fu) {
                    track.done = true;
                    return true;
                }
                return next_delta(track);
            }
            if (status != 0xf0u && status != 0xf7u) return false;
            std::uint32_t length = 0;
            if (!vlq(track, length) || length > track.end - track.pos) return false;
            track.pos += length;
            return next_delta(track);
        }
        if (first >= 0x80u) return false;
        const auto command = status & 0xf0u;
        unsigned int second = 0;
        if (command != 0xc0u && command != 0xd0u) {
            if (track.pos >= track.end) return false;
            second = byte(track.pos++);
            if (second >= 0x80u) return false;
        }
        event.channel = status & 0x0fu;
        event.note = first;
        event.value = second;
        if (command == 0x90u)
            event.kind = second == 0 ? Event::Kind::NoteOff : Event::Kind::NoteOn;
        else if (command == 0x80u) event.kind = Event::Kind::NoteOff;
        else if (command == 0xb0u && first == 64) event.kind = Event::Kind::Sustain;
        else if (command == 0xb0u && (first == 120 || first == 123))
            event.kind = Event::Kind::AllOff;
        return next_delta(track);
    }

    std::span<const std::byte> data_{};
    std::array<Track, maximum_tracks> tracks_{};
    std::size_t count_ = 0;
    std::uint64_t tick_ = 0;
    std::uint64_t time_us_ = 0;
    std::uint64_t remainder_ = 0;
    std::uint32_t tempo_ = 500'000;
    unsigned int division_ = 480;
    MidiInfo info_{};
    bool first_tempo_ = true;
};

class Synth {
public:
    Synth() : out_(mm::audio::selected_out()) {}
    ~Synth() { stop(); if (configured_) (void)out_.sleep(); }

    [[nodiscard]] bool initialize() {
        if (out_.initialize() != mm::audio::Status::Ok) return false;
        mm::audio::Format actual{};
        if (out_.configure({.rate_hz = requested_rate}, actual) != mm::audio::Status::Ok ||
            actual.rate_hz == 0) return false;
        rate_ = actual.rate_hz;
        configured_ = true;
        return ring_.configure(samples_, actual) == mm::audio::Status::Ok;
    }
    [[nodiscard]] bool start() {
        if (!configured_) return false;
        stop();
        if (ring_.reset() != mm::audio::Status::Ok) return false;
        voices_ = {};
        sustain_ = {};
        ending_ = false;
        fill();
        if (out_.start(ring_) != mm::audio::Status::Ok) return false;
        started_ = true;
        return true;
    }
    void stop() {
        if (started_) (void)out_.stop();
        started_ = false;
    }
    [[nodiscard]] bool active() const { return started_; }
    void set_volume(unsigned int vol) { volume_ = vol > 100u ? 100u : vol; }
    [[nodiscard]] unsigned int volume() const { return volume_; }
    void finish() {
        ending_ = true;
        for (auto& voice : voices_) voice.gate = false;
    }
    void note_on(unsigned int channel, unsigned int note, unsigned int velocity) {
        if (channel == 9 || note > 127) return; // no percussion instrument yet
        Voice* slot = &voices_[0];
        for (auto& voice : voices_) {
            if (!voice.gate && voice.envelope == 0) { slot = &voice; break; }
            if (voice.age > slot->age) slot = &voice;
        }
        const auto frequency = (static_cast<std::uint64_t>(8'176) *
                                semitone_ratio_q16[note % 12] >> 16) << (note / 12);
        slot->step = static_cast<std::uint32_t>((frequency << 32) /
                    (static_cast<std::uint64_t>(rate_) * 1000u));
        slot->phase = 0;
        slot->envelope = 0;
        slot->velocity = velocity;
        slot->channel = channel;
        slot->note = note;
        slot->age = 0;
        slot->gate = true;
        slot->released = false;
    }
    void note_off(unsigned int channel, unsigned int note) {
        for (auto& voice : voices_)
            if ((voice.gate || voice.envelope != 0) &&
                voice.channel == channel && voice.note == note) {
                voice.released = true;
                if (!sustain_[channel]) voice.gate = false;
            }
    }
    void sustain(unsigned int channel, bool on) {
        sustain_[channel] = on;
        if (!on)
            for (auto& voice : voices_)
                if (voice.channel == channel && voice.released) voice.gate = false;
    }
    void all_off(unsigned int channel) {
        sustain_[channel] = false;
        for (auto& voice : voices_)
            if (voice.channel == channel) voice.gate = false;
    }
    [[nodiscard]] bool service() {
        if (!started_) return true;
        fill();
        if (out_.service() != mm::audio::Status::Ok) return false;
        if (ending_ && quiet()) {
            std::size_t pending = 0;
            if (out_.pending(pending) != mm::audio::Status::Ok) return false;
            if (pending == 0 && ring_.readable() == 0) stop();
        }
        return true;
    }

private:
    struct Voice {
        std::uint32_t phase = 0;
        std::uint32_t step = 0;
        unsigned int channel = 0;
        unsigned int note = 0;
        unsigned int velocity = 0;
        unsigned int age = 0;
        int envelope = 0;
        bool gate = false;
        bool released = false;
    };
    static std::int32_t triangle(std::uint32_t phase) {
        const auto x = phase >> 16;
        return static_cast<std::int32_t>((x < 32768u ? x : 65535u - x) * 2u) - 32768;
    }
    [[nodiscard]] bool quiet() const {
        for (const auto& voice : voices_) if (voice.envelope != 0 || voice.gate) return false;
        return true;
    }
    void fill() {
        const auto region = ring_.write_region();
        std::size_t written = 0;
        for (auto& sample : region) {
            if (ending_ && quiet()) break;
            std::int64_t mixed = 0;
            for (auto& voice : voices_) {
                if (voice.gate) {
                    if (voice.envelope < 30000) {
                        voice.envelope += 512;
                        if (voice.envelope > 30000) voice.envelope = 30000;
                    } else if (voice.envelope > 18000) --voice.envelope;
                } else if (voice.envelope > 0) {
                    voice.envelope = voice.envelope > 16 ? voice.envelope - 16 : 0;
                }
                if (voice.envelope == 0) continue;
                const auto p = voice.phase;
                const auto harmonic = 6 * triangle(p) + 2 * triangle(p * 2u) +
                                      triangle(p * 3u);
                mixed += static_cast<std::int64_t>(harmonic) * voice.envelope *
                         voice.velocity * 9000 / (9LL * 32768 * 32768 * 127 * 4);
                voice.phase += voice.step;
                if (voice.age < 0xffff'ffffu) ++voice.age;
            }
            mixed = (mixed * static_cast<std::int64_t>(volume_)) / 100LL;
            if (mixed > 30000) mixed = 30000;
            if (mixed < -30000) mixed = -30000;
            sample = static_cast<std::int16_t>(mixed);
            ++written;
        }
        (void)ring_.commit_write(written);
    }

    mm::audio::Out& out_;
    mm::audio::Ring ring_{};
    std::array<std::int16_t, 1024> samples_{};
    std::array<Voice, maximum_voices> voices_{};
    std::array<bool, 16> sustain_{};
    unsigned int rate_ = requested_rate;
    unsigned int volume_ = 80;
    bool configured_ = false;
    bool started_ = false;
    bool ending_ = false;
};

Song song;
bool playing = false;
std::uint64_t elapsed_us = 0;
unsigned long last_clock_us = 0;

Synth& synth() {
    static Synth output;
    return output;
}

} // namespace

const char* error() { return last_error; }
bool active() { return playing; }

bool initialize() {
    if (synth().initialize()) return true;
    last_error = "Audio output unavailable";
    return false;
}

bool load_bundled(std::size_t index) {
    constexpr std::size_t count = sizeof(bundled_songs) / sizeof(bundled_songs[0]);
    if (index >= count) {
        last_error = "Invalid song index";
        return false;
    }
    playing = false;
    synth().stop();
    const auto& song_def = bundled_songs[index];
    if (!song.load(std::as_bytes(song_def.data))) {
        last_error = "Unsupported/bad MIDI";
        return false;
    }
    last_error = "Ready to play";
    return true;
}

bool load(std::string_view path) {
    if (path.starts_with("builtin:")) {
        std::size_t idx = 0;
        for (std::size_t i = 8; i < path.size(); ++i) {
            if (path[i] >= '0' && path[i] <= '9') {
                idx = idx * 10 + static_cast<std::size_t>(path[i] - '0');
            }
        }
        return load_bundled(idx);
    }
    constexpr std::size_t count = sizeof(bundled_songs) / sizeof(bundled_songs[0]);
    for (std::size_t i = 0; i < count; ++i) {
        if (path == bundled_songs[i].filename) {
            return load_bundled(i);
        }
    }
    playing = false;
    synth().stop();
    mm::fs::File file;
    if (mm::fs::open(path, mm::fs::Access::Read, mm::fs::Disposition::OpenExisting,
                     file) != mm::fs::Status::Ok) {
        last_error = "Cannot open MIDI file";
        return false;
    }
    mm::fs::Stat stat{};
    if (file.stat(stat) != mm::fs::Status::Ok || stat.size > maximum_file ||
        stat.size < 14) {
        last_error = "MIDI must be 14B-128KB";
        return false;
    }
    std::size_t received = 0;
    while (received < stat.size) {
        std::size_t count = 0;
        if (file.read(std::span<std::byte>{file_bytes}.subspan(
                          received, static_cast<std::size_t>(stat.size) - received),
                      count) != mm::fs::Status::Ok || count == 0) {
            last_error = "MIDI read failed";
            return false;
        }
        received += count;
    }
    (void)file.close();
    if (!song.load(std::span<const std::byte>{file_bytes}.first(received))) {
        last_error = "Unsupported/bad MIDI";
        return false;
    }
    last_error = "Ready to play";
    return true;
}

bool start() {
    if (!synth().start()) {
        last_error = "Audio start failed";
        return false;
    }
    if (mm::mcu::ticks_us(last_clock_us) != mm::mcu::Status::Ok) {
        synth().stop();
        last_error = "Clock unavailable";
        return false;
    }
    elapsed_us = 0;
    playing = true;
    return true;
}

void stop() {
    playing = false;
    synth().stop();
}

bool service() {
    if (!playing) return true;
    unsigned long now = 0;
    if (mm::mcu::ticks_us(now) != mm::mcu::Status::Ok) {
        last_error = "Clock failed";
        stop();
        return false;
    }
    elapsed_us += now - last_clock_us;
    last_clock_us = now;
    for (unsigned int i = 0; i < 32; ++i) {
        Event event{};
        const auto result = song.next(elapsed_us, event);
        if (result == Song::Result::Waiting) break;
        if (result == Song::Result::Invalid) {
            last_error = "Invalid MIDI event";
            stop();
            return false;
        }
        if (result == Song::Result::Done) {
            synth().finish();
            break;
        }
        if (event.kind == Event::Kind::NoteOn)
            synth().note_on(event.channel, event.note, event.value);
        else if (event.kind == Event::Kind::NoteOff)
            synth().note_off(event.channel, event.note);
        else if (event.kind == Event::Kind::Sustain)
            synth().sustain(event.channel, event.value >= 64);
        else if (event.kind == Event::Kind::AllOff)
            synth().all_off(event.channel);
    }
    if (!synth().service()) {
        last_error = "Audio service failed";
        stop();
        return false;
    }
    if (!synth().active()) {
        playing = false;
        last_error = "Playback complete";
    }
    return true;
}

const MidiInfo& current_info() {
    return song.info();
}

void set_volume(unsigned int percent) {
    synth().set_volume(percent);
}

unsigned int volume() {
    return synth().volume();
}

} // namespace midicommander
