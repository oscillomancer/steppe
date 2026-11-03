// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>

#include <supple/audio/audio_file.h>
#include <supple/audio/wav_stream.h>
#include <supple/base/files.h>
#include "../../../tests/file_platform.h"
#include "../src/file_platform.h"
#if defined(_WIN32)
#include <winioctl.h>
#endif

using namespace supple::audio;
namespace fs = std::filesystem;

namespace {
AudioData tone(std::size_t frames, double rate, unsigned channels) {
    AudioData d;
    d.sampleRate = rate;
    d.channels.assign(channels, std::vector<float>(frames));
    for (unsigned c = 0; c < channels; ++c)
        for (std::size_t i = 0; i < frames; ++i)
            d.channels[c][i] = static_cast<float>(
                0.5 * std::sin(2 * M_PI * 440.0 * static_cast<double>(i) / rate + c));
    return d;
}
const fs::path kData = fs::path(__FILE__).parent_path() / "data";
}  // namespace

TEST_CASE("float32 WAV round trip is bit-exact") {
    auto d = tone(1000, 48000, 2);
    auto bytes = encodeWav(d);
    REQUIRE(bytes);
    CHECK(detect(*bytes) == Container::Wav);
    auto back = readAudio(*bytes);
    REQUIRE(back);
    CHECK(back->sampleRate == 48000);
    REQUIRE(back->channelCount() == 2);
    REQUIRE(back->frames() == 1000);
    CHECK(back->channels == d.channels);
}

TEST_CASE("RF64 header carries 64-bit sizes without allocating a payload") {
    constexpr std::uint64_t frames = (std::uint64_t{1} << 32) / 8 + 100;
    auto h = wavHeader(frames, 2, 48000, SampleFormat::Float32);
    REQUIRE(h);
    REQUIRE(h->size() == 80);
    CHECK(std::string(reinterpret_cast<const char*>(h->data()), 4) == "RF64");
    CHECK(std::string(reinterpret_cast<const char*>(h->data() + 12), 4) == "ds64");
    CHECK(std::string(reinterpret_cast<const char*>(h->data() + 72), 4) == "data");
    auto get64 = [&](std::size_t at) {
        std::uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | (*h)[at + i];
        return v;
    };
    CHECK(get64(20) == frames * 8 + 72);
    CHECK(get64(28) == frames * 8);
    CHECK(get64(36) == frames);
    auto small = wavHeader(10, 2, 48000, SampleFormat::Pcm16);
    REQUIRE(small);
    CHECK(small->size() == 44);
    CHECK(std::string(reinterpret_cast<const char*>(small->data()), 4) == "RIFF");
}

TEST_CASE("inspect reads little-endian RF64 and BW64 sizes from sparse files") {
    const auto path = fs::temp_directory_path() /
                      ("supple-inspect-rf64-" + std::to_string(supple::test::processId()) + ".wav");
    constexpr std::uint64_t frames = (std::uint64_t{1} << 30) + 3;
    auto header = wavHeader(frames, 1, 48000, SampleFormat::Float32);
    REQUIRE(header);
    for (const auto tag : {"RF64", "BW64"}) {
        std::copy_n(tag, 4, header->begin());
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(header->data()),
                       static_cast<std::streamsize>(header->size()));
            file.seekp(static_cast<std::streamoff>(header->size() + frames * 4 - 1));
            file.put(0);
            REQUIRE(file.good());
        }
        auto info = inspect(path);
        REQUIRE(info);
        CHECK(info->frames == static_cast<std::int64_t>(frames));
        CHECK(info->sampleRate == 48000);
        CHECK_FALSE(info->truncated);
    }
    fs::remove(path);
}

TEST_CASE("inspect rejects short fmt chunks and overflowing chunk skips") {
    const auto path =
        fs::temp_directory_path() /
        ("supple-inspect-invalid-" + std::to_string(supple::test::processId()) + ".wav");
    auto header = wavHeader(0, 1, 48000, SampleFormat::Float32);
    REQUIRE(header);
    (*header)[16] = 0;  // fmt declares no payload, despite bytes that resemble one
    REQUIRE(supple::writeFileAtomic(path, *header));
    CHECK_FALSE(inspect(path));
    header = wavHeader(0, 1, 48000, SampleFormat::Float32);
    REQUIRE(header);
    std::vector<std::uint8_t> oversized{'J', 'U', 'N', 'K', 255, 255, 255, 255};
    header->insert(header->begin() + 12, oversized.begin(), oversized.end());
    REQUIRE(supple::writeFileAtomic(path, *header));
    CHECK_FALSE(inspect(path));  // must not wrap the skip and parse the following fmt
    fs::remove(path);
}

TEST_CASE("streamed WAV validates rates and rejects overflowing append sizes") {
    const auto path =
        fs::temp_directory_path() /
        ("supple-stream-bounds-" + std::to_string(supple::test::processId()) + ".wav");
    fs::remove(path);
    WavStreamWriter writer;
    for (double rate : {0.25, std::numeric_limits<double>::infinity(), 1e20}) {
        CHECK_FALSE(writer.open(path, rate, 2));
        CHECK_FALSE(fs::exists(path));
    }
    REQUIRE(writer.open(path, 48000, 2));
    CHECK(writer.append(nullptr, std::size_t{1} << (sizeof(std::size_t) * 8 - 3)).error().code ==
          "audio.stream_full");
    REQUIRE(writer.close());
    CHECK(readAudio(path)->frames() == 0);
    fs::remove(path);
}

TEST_CASE("inspect rejects malformed FLAC STREAMINFO") {
    const auto path =
        fs::temp_directory_path() /
        ("supple-inspect-flac-invalid-" + std::to_string(supple::test::processId()) + ".flac");
    auto encoded = encodeFlac(tone(32, 48000, 1));
    REQUIRE(encoded);
    REQUIRE(supple::writeFileAtomic(path, *encoded));
    REQUIRE(inspect(path));
    for (const unsigned size : {0u, 33u, 35u}) {
        auto malformed = *encoded;
        malformed[7] = static_cast<std::uint8_t>(size);
        REQUIRE(supple::writeFileAtomic(path, malformed));
        auto result = inspect(path);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "audio.format");
    }
    auto zeroRate = *encoded;
    zeroRate[18] = zeroRate[19] = 0;
    zeroRate[20] &= 0x0f;
    REQUIRE(supple::writeFileAtomic(path, zeroRate));
    CHECK_FALSE(inspect(path));
    fs::remove(path);
}

TEST_CASE("PCM WAV round trips within quantization + dither, deterministic by seed") {
    auto d = tone(4800, 44100, 1);
    for (auto fmt : {SampleFormat::Pcm16, SampleFormat::Pcm24}) {
        const double lsb = fmt == SampleFormat::Pcm16 ? 1.0 / 32768 : 1.0 / 8388608;
        auto a = encodeWav(d, {fmt, true, 7});
        auto b = encodeWav(d, {fmt, true, 7});
        auto c = encodeWav(d, {fmt, true, 8});
        REQUIRE(a);
        CHECK(*a == *b);
        CHECK(*a != *c);
        auto back = readAudio(*a);
        REQUIRE(back);
        double maxErr = 0;
        for (std::size_t i = 0; i < d.frames(); ++i)
            maxErr = std::max(
                maxErr, std::fabs(static_cast<double>(back->channels[0][i] - d.channels[0][i])));
        CHECK(maxErr <= 1.5 * lsb + 1e-9);
    }
    // Without dither, quantization error is at most half an LSB.
    auto nd = readAudio(*encodeWav(d, {SampleFormat::Pcm16, false, 0}));
    REQUIRE(nd);
    double maxErr = 0;
    for (std::size_t i = 0; i < d.frames(); ++i)
        maxErr =
            std::max(maxErr, std::fabs(static_cast<double>(nd->channels[0][i] - d.channels[0][i])));
    CHECK(maxErr <= 0.5 / 32768 + 1e-9);
}

TEST_CASE("clipping at full scale, invalid inputs rejected") {
    AudioData d;
    d.sampleRate = 48000;
    d.channels = {{2.0f, -2.0f, 1.0f}};
    auto back = readAudio(*encodeWav(d, {SampleFormat::Pcm16, false, 0}));
    REQUIRE(back);
    CHECK(static_cast<double>(back->channels[0][0]) == doctest::Approx(32767.0 / 32768));
    CHECK(static_cast<double>(back->channels[0][1]) == -1.0);
    AudioData empty;
    CHECK(encodeWav(empty).error().code == "audio.encode");
    AudioData ragged;
    ragged.channels = {{0.f, 0.f}, {0.f}};
    CHECK_FALSE(encodeWav(ragged));
    std::vector<std::uint8_t> junk = {1, 2, 3, 4, 5};
    CHECK(readAudio(junk).error().code == "audio.format");
    CHECK(readAudio(fs::path("/nonexistent/x.wav")).error().code == "audio.open");
}

TEST_CASE("WAV encoding rejects rates below the header minimum") {
    AudioData data;
    data.channels = {{0.f}};
    for (double rate : {0.25, 0.75, 0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                        std::numeric_limits<double>::infinity()}) {
        data.sampleRate = rate;
        auto encoded = encodeWav(data);
        REQUIRE_FALSE(encoded);
        CHECK(encoded.error().code == "audio.encode");
    }
    data.sampleRate = 1.0;
    auto encoded = encodeWav(data);
    REQUIRE(encoded);
    auto decoded = readAudio(*encoded);
    REQUIRE(decoded);
    CHECK(decoded->sampleRate == 1.0);
}

TEST_CASE("FLAC and MP3 fixtures decode") {
    auto flac = readAudio(kData / "sine440.flac");
    REQUIRE(flac);
    CHECK(flac->sampleRate == 44100);
    CHECK(flac->channelCount() == 2);
    CHECK(flac->frames() == 11025);
    float peak = 0;
    for (float s : flac->channels[0]) peak = std::max(peak, std::fabs(s));
    CHECK(static_cast<double>(peak) == doctest::Approx(0.5).epsilon(0.01));
    auto mp3 = readAudio(kData / "sine440.mp3");
    REQUIRE(mp3);
    CHECK(mp3->sampleRate == 44100);
    CHECK(mp3->channelCount() == 2);
    CHECK(mp3->frames() > 10000);
}

TEST_CASE("writeWav is atomic and creates directories") {
    const fs::path dir = fs::temp_directory_path() / "supple-audio-file-test";
    fs::remove_all(dir);
    const fs::path file = dir / "sub" / "out.wav";
    REQUIRE(writeWav(file, tone(100, 48000, 2)));
    CHECK(fs::exists(file));
    CHECK_FALSE(fs::exists(file.string() + ".tmp"));
    auto back = readAudio(file);
    REQUIRE(back);
    CHECK(back->frames() == 100);
    fs::remove_all(dir);
}

TEST_CASE("truncated WAV data decodes what is there and is flagged") {
    AudioData a;
    a.sampleRate = 48000;
    a.channels.assign(2, std::vector<float>(1000, 0.25f));
    WavOptions o;
    o.format = SampleFormat::Pcm16;
    auto bytes = encodeWav(a, o).value();
    CHECK_FALSE(readAudio(bytes).value().truncated);
    bytes.resize(bytes.size() - 100);
    auto r = readAudio(bytes);
    REQUIRE(r);
    CHECK(r->truncated);
    CHECK(r->frames() == 975);
}

TEST_CASE("PCM encoding maps NaN to zero and clamps infinities") {
    AudioData a;
    a.sampleRate = 48000;
    a.channels = {{std::nanf(""), std::numeric_limits<float>::infinity(),
                   -std::numeric_limits<float>::infinity(), 0.5f}};
    for (auto f : {SampleFormat::Pcm16, SampleFormat::Pcm24}) {
        WavOptions o;
        o.format = f;
        o.dither = false;
        auto back = readAudio(encodeWav(a, o).value());
        REQUIRE(back);
        const auto& c = back->channels[0];
        CHECK(c[0] == 0.0f);
        CHECK(c[1] == doctest::Approx(1.0).epsilon(1e-4));
        CHECK(c[2] == -1.0f);
        CHECK(c[3] == 0.5f);
    }
}

TEST_CASE("resampling preserves channel layout and duration") {
    auto input = tone(4800, 48000, 2);
    auto output = resample(input, 44100);
    REQUIRE(output);
    CHECK(output->sampleRate == 44100);
    CHECK(output->channelCount() == 2);
    CHECK(output->frames() == 4410);
    input.truncated = true;
    CHECK(resample(input, 44100)->truncated);
    CHECK(std::abs(output->seconds() - input.seconds()) < 1e-12);
    CHECK_FALSE(resample(input, -1));
}

TEST_CASE("a streamed WAV is readable after every header update and after close") {
    const auto path = std::filesystem::temp_directory_path() /
                      ("supple-wav-stream-" + std::to_string(supple::test::processId()) + ".wav");
    supple::audio::WavStreamWriter w;
    REQUIRE(w.open(path, 48000, 2));
    std::vector<float> block(2 * 1000);
    for (std::size_t i = 0; i < 1000; ++i) {
        block[2 * i] = static_cast<float>(i) / 1000.0f;
        block[2 * i + 1] = -static_cast<float>(i) / 1000.0f;
    }
    REQUIRE(w.append(block.data(), 1000));
    REQUIRE(w.updateHeader());
    auto partial = supple::audio::readAudio(path);  // as a crash would leave it
    REQUIRE(partial);
    CHECK(partial->frames() == 1000);
    REQUIRE(w.append(block.data(), 500));
    REQUIRE(w.close());
    auto whole = supple::audio::readAudio(path);
    REQUIRE(whole);
    CHECK(whole->frames() == 1500);
    CHECK(whole->channelCount() == 2);
    CHECK(whole->sampleRate == 48000.0);
    CHECK(double(whole->channels[0][999]) == doctest::Approx(0.999));
    CHECK(double(whole->channels[1][1499]) == doctest::Approx(-0.499));
    std::filesystem::remove(path);
}

TEST_CASE("repairing a streamed WAV recovers the frames written after the last header update") {
    const auto path = std::filesystem::temp_directory_path() /
                      ("supple-wav-repair-" + std::to_string(supple::test::processId()) + ".wav");
    {
        supple::audio::WavStreamWriter w;
        REQUIRE(w.open(path, 44100, 1));
        std::vector<float> ramp(1500);
        for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / 2000.0f;
        REQUIRE(w.append(ramp.data(), 1000));
        REQUIRE(w.updateHeader());
        REQUIRE(w.append(ramp.data() + 1000, 500));
        REQUIRE(w.flush());
        // A crash now: the header says 1000 frames, the file holds 1500 (and
        // half a frame the OS got before the end).
        const float half = 0.25f;
        std::ofstream f(path, std::ios::binary | std::ios::app);
        REQUIRE(f);
        f.write(reinterpret_cast<const char*>(&half), 2);
        f.close();
        auto info = supple::audio::inspectWavStream(path);
        REQUIRE(info);
        CHECK(info->headerFrames == 1000);
        CHECK(info->fileFrames == 1500);
        CHECK(info->channels == 1);
        CHECK(info->sampleRate == 44100.0);
        auto repaired = supple::audio::repairWavStream(path);
        REQUIRE(repaired);
        CHECK(*repaired == 1500);
        auto audio = supple::audio::readAudio(path);
        REQUIRE(audio);
        CHECK(audio->frames() == 1500);
        CHECK(double(audio->channels[0][1499]) == doctest::Approx(1499.0 / 2000.0));
        CHECK(*supple::audio::repairWavStream(path) == 1500);  // idempotent
        // (The writer's destructor would rewrite the header; a crash would not.)
        (void)w.close();
    }
    const auto other = path.string() + ".txt";
    REQUIRE(supple::writeFileAtomic(other,
                                    std::string_view("RIFF but not really a wave file at all...."))
                .ok());
    auto bad = supple::audio::inspectWavStream(other);
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "audio.not_stream");
    std::filesystem::remove(path);
    std::filesystem::remove(other);
}

TEST_CASE("inspect reads metadata without decoding, and agrees with readAudio") {
    // The fixtures: FLAC and MP3 metadata must equal what the decoders report.
    auto flac = inspect(kData / "sine440.flac");
    REQUIRE(flac);
    auto flacRef = readAudio(kData / "sine440.flac");
    REQUIRE(flacRef);
    CHECK(flac->container == Container::Flac);
    CHECK(flac->sampleRate == flacRef->sampleRate);
    CHECK(flac->channels == static_cast<int>(flacRef->channelCount()));
    CHECK(flac->frames == static_cast<std::int64_t>(flacRef->frames()));

    auto mp3 = inspect(kData / "sine440.mp3");
    REQUIRE(mp3);
    auto mp3Ref = readAudio(kData / "sine440.mp3");
    REQUIRE(mp3Ref);
    CHECK(mp3->container == Container::Mp3);
    CHECK(mp3->sampleRate == mp3Ref->sampleRate);
    CHECK(mp3->channels == static_cast<int>(mp3Ref->channelCount()));
    // A Xing header (or the frame walk when there is none) must land on the
    // decoder's own frame count.
    CHECK(mp3->frames == static_cast<std::int64_t>(mp3Ref->frames()));

    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-inspect-" + std::to_string(supple::test::processId()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // WAV: float32 (what the recorder writes) and PCM 24.
    AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(12345));
    for (std::size_t i = 0; i < tone.frames(); ++i)
        tone.channels[0][i] = tone.channels[1][i] =
            static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000));
    REQUIRE(writeWav(dir / "float.wav", tone));
    auto wav = inspect(dir / "float.wav");
    REQUIRE(wav);
    CHECK(wav->container == Container::Wav);
    CHECK(wav->sampleRate == 48000);
    CHECK(wav->channels == 2);
    CHECK(wav->frames == 12345);
    CHECK_FALSE(wav->truncated);
    CHECK(wav->bitsPerSample == 32);
    CHECK(wav->floatSamples);
    WavOptions pcm;
    pcm.format = SampleFormat::Pcm24;
    REQUIRE(writeWav(dir / "pcm.wav", tone, pcm));
    auto pcmInfo = inspect(dir / "pcm.wav");
    REQUIRE(pcmInfo);
    CHECK(pcmInfo->frames == 12345);
    CHECK(pcmInfo->sampleRate == 48000);
    CHECK(pcmInfo->channels == 2);
    CHECK(pcmInfo->bitsPerSample == 24);
    CHECK_FALSE(pcmInfo->floatSamples);
    CHECK(flac->bitsPerSample == 0);

    // An interrupted file: the data chunk is shorter than it declares.
    const auto bytes = encodeWav(tone, pcm).value();
    auto shortBytes = bytes;
    shortBytes.resize(shortBytes.size() - 1000);
    REQUIRE(supple::writeFileAtomic(dir / "short.wav", shortBytes));
    auto shortInfo = inspect(dir / "short.wav");
    REQUIRE(shortInfo);
    CHECK(shortInfo->truncated);
    CHECK(shortInfo->frames == 12345 - (1000 + 5) / 6);  // 24-bit: 6 bytes a frame

    // Errors: an unknown container, and a file that is not there.
    REQUIRE(
        supple::writeFileAtomic(dir / "junk.bin", std::string_view("definitely not audio")).ok());
    CHECK(inspect(dir / "junk.bin").error().code == "audio.format");
    CHECK(inspect(dir / "missing.wav").error().code == "audio.open");
    std::filesystem::remove_all(dir);
}

TEST_CASE("FLAC encoding decodes to exactly what the same WAV holds (16 and 24 bit, mono to 3)") {
    for (const int bits : {16, 24}) {
        for (const std::size_t channels : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
            INFO("bits " << bits << ", channels " << channels);
            AudioData data;
            data.sampleRate = 44100;
            const std::size_t frames = 10000;  // two full blocks and a short one
            data.channels.assign(channels, std::vector<float>(frames));
            for (std::size_t i = 0; i < frames; ++i) {
                const double t = static_cast<double>(i) / 44100.0;
                data.channels[0][i] = static_cast<float>(0.5 * std::sin(2 * M_PI * 220 * t));
                if (channels > 1)  // near the left: side coding pays
                    data.channels[1][i] =
                        static_cast<float>(0.48 * std::sin(2 * M_PI * 220 * t + 0.01));
                if (channels > 2)  // noise: verbatim or high-order subframes
                    data.channels[2][i] =
                        static_cast<float>((static_cast<int>(i * 7919 % 2003) - 1001) / 1200.0);
            }
            data.channels[0][100] = 2.0f;  // clipped
            data.channels[0][101] = -2.0f;
            for (std::size_t i = 5000; i < 5200; ++i) data.channels[0][i] = 0.25f;  // constant run
            FlacOptions flac;
            flac.bitsPerSample = bits;
            flac.ditherSeed = 7;
            auto bytes = encodeFlac(data, flac);
            REQUIRE(bytes);
            CHECK(detect(*bytes) == Container::Flac);
            // For checking with the reference decoder (flac -t: CRCs and MD5).
            if (const char* dir = std::getenv("SUPPLE_FLAC_OUT"))
                REQUIRE(supple::writeFileAtomic(
                    std::filesystem::path(dir) /
                        ("t" + std::to_string(bits) + "-" + std::to_string(channels) + ".flac"),
                    *bytes));
            WavOptions wav;
            wav.format = bits == 16 ? SampleFormat::Pcm16 : SampleFormat::Pcm24;
            wav.ditherSeed = 7;
            auto wavBytes = encodeWav(data, wav);
            REQUIRE(wavBytes);
            auto fromFlac = readAudio(*bytes);
            auto fromWav = readAudio(*wavBytes);
            REQUIRE(fromFlac);
            REQUIRE(fromWav);
            CHECK(fromFlac->sampleRate == 44100);
            REQUIRE(fromFlac->channelCount() == channels);
            REQUIRE(fromFlac->frames() == frames);
            std::size_t diffs = 0;
            for (std::size_t c = 0; c < channels; ++c)
                for (std::size_t i = 0; i < frames; ++i)
                    diffs += fromFlac->channels[c][i] != fromWav->channels[c][i];
            CHECK(diffs == 0);
            // Smaller than the PCM it holds (the sine compresses).
            if (channels == 2) CHECK(bytes->size() < wavBytes->size() / 2);
        }
    }
    // Silence and a file shorter than one block.
    AudioData tiny;
    tiny.channels.assign(2, std::vector<float>(37, 0.0f));
    auto bytes = encodeFlac(tiny);
    REQUIRE(bytes);
    auto back = readAudio(*bytes);
    REQUIRE(back);
    CHECK(back->frames() == 37);
    CHECK(encodeFlac(tiny, {.bitsPerSample = 20}).error().code == "audio.encode");
    auto minimumBlock = encodeFlac(tiny, {.blockSize = -1});
    REQUIRE(minimumBlock);
    CHECK((*minimumBlock)[8] == 0);
    CHECK((*minimumBlock)[9] == 16);  // STREAMINFO minimum block size clamps to 16, not 65535
    CHECK(readAudio(*minimumBlock)->frames() == tiny.frames());
}

TEST_CASE("R6 inspect rejects inconsistent PCM and float block alignment") {
    const auto path = fs::temp_directory_path() /
                      ("supple-align-" + std::to_string(supple::test::processId()) + ".wav");
    const std::uint8_t formats[][2] = {{1, 8}, {1, 16}, {1, 24}, {1, 32}, {3, 32}, {3, 64}};
    for (const auto& format : formats) {
        for (unsigned channels : {1u, 2u}) {
            for (bool extensible : {false, true}) {
                auto bytes = wavHeader(4, channels, 48000, SampleFormat::Float32);
                REQUIRE(bytes);
                const auto tag = format[0];
                const auto align = channels * (format[1] / 8);
                (*bytes)[20] = tag;
                (*bytes)[32] = static_cast<std::uint8_t>(align);
                (*bytes)[34] = format[1];
                (*bytes)[40] = static_cast<std::uint8_t>(4 * align);
                bytes->resize(44 + 4 * align);
                const auto byteRate = 48000u * align;
                for (unsigned i = 0; i < 4; ++i)
                    (*bytes)[28 + i] = static_cast<std::uint8_t>(byteRate >> (8 * i));
                if (extensible) {
                    bytes->insert(bytes->begin() + 36, 24, 0);
                    (*bytes)[16] = 40;
                    (*bytes)[20] = 0xfe;
                    (*bytes)[21] = 0xff;
                    (*bytes)[36] = 22;
                    (*bytes)[38] = (*bytes)[34];
                    (*bytes)[44] = tag;
                    // Standard PCM/IEEE-float subformat GUID tail.
                    (*bytes)[50] = 0x10;
                    (*bytes)[52] = 0x80;
                    (*bytes)[55] = 0xaa;
                    (*bytes)[57] = 0x38;
                    (*bytes)[58] = 0x9b;
                    (*bytes)[59] = 0x71;
                    const auto size = static_cast<std::uint32_t>(bytes->size() - 8);
                    for (unsigned i = 0; i < 4; ++i)
                        (*bytes)[4 + i] = static_cast<std::uint8_t>(size >> (8 * i));
                }
                const auto riffSize = static_cast<std::uint32_t>(bytes->size() - 8);
                for (unsigned i = 0; i < 4; ++i)
                    (*bytes)[4 + i] = static_cast<std::uint8_t>(riffSize >> (8 * i));
                REQUIRE(supple::writeFileAtomic(path, *bytes));
                auto good = inspect(path);
                REQUIRE(good);
                CHECK(good->frames == 4);
                for (auto badAlign : {0u, 1u, align + 1}) {
                    if (badAlign == align) continue;
                    (*bytes)[32] = static_cast<std::uint8_t>(badAlign);
                    REQUIRE(supple::writeFileAtomic(path, *bytes));
                    auto bad = inspect(path);
                    REQUIRE_FALSE(bad);
                    CHECK(bad.error().code == "audio.format");
                }
            }
        }
    }
    auto other = encodeWav(tone(4, 48000, 1));
    REQUIRE(other);
    (*other)[20] = 6;
    (*other)[32] = 1;  // Other format tags retain their existing handling.
    REQUIRE(supple::writeFileAtomic(path, *other));
    REQUIRE(inspect(path));
    fs::remove(path);
}

TEST_CASE("recording creation is exclusive and native paths survive recovery") {
    const auto dir = fs::temp_directory_path() /
                     ("supple-native-wav-" + std::to_string(supple::test::processId()));
    fs::create_directories(dir);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove_all(path); }
    } cleanup{dir};
    const auto path = dir / fs::path(std::u8string(u8"take-音.wav"));
    WavStreamWriter writer;
    REQUIRE(writer.open(path, 48000, 2));
    const float frames[] = {0.25f, -0.5f, 0.75f, -1.0f};
    REQUIRE(writer.append(frames, 2));
    REQUIRE(writer.flush());
    WavStreamWriter second;
    CHECK_FALSE(second.open(path, 48000, 2));
    writer.abandon();
    REQUIRE(repairWavStream(path));
    auto restored = readAudio(path);
    REQUIRE(restored);
    CHECK(restored->channels == std::vector<std::vector<float>>{{0.25f, 0.75f}, {-0.5f, -1.0f}});
    const auto alias = dir / "link.wav";
    std::error_code ec;
    fs::create_symlink(path, alias, ec);
    if (!ec) {
        CHECK_FALSE(inspectWavStream(alias));
        CHECK_FALSE(repairWavStream(alias));
        CHECK_FALSE(second.open(alias, 48000, 2));
    } else {
        MESSAGE("symlink probe unavailable: ", ec.message());
    }
}

TEST_CASE("streaming render files sync and publish atomically over native paths") {
    const auto path =
        fs::temp_directory_path() /
        fs::path(std::u8string(u8"supple-render-音-") +
                 fs::path(std::to_string(supple::test::processId())).u8string() + u8".wav");
    REQUIRE(supple::writeFileAtomic(path, "old"));
    fs::path temp;
    auto* file = supple::audio::detail::files::temporary(path, temp);
    REQUIRE(file);
    CHECK(std::fwrite("replacement", 1, 11, file) == 11);
    REQUIRE(supple::audio::detail::files::sync(file));
    CHECK(supple::readTextFile(path).value() == "old");
    REQUIRE(std::fclose(file) == 0);
    REQUIRE(supple::audio::detail::files::publish(temp, path));
    CHECK_FALSE(fs::exists(temp));
    CHECK(supple::readTextFile(path).value() == "replacement");
    fs::remove(path);
}

TEST_CASE("recording recovery uses 64-bit positions past Windows long range") {
    const auto path =
        fs::temp_directory_path() /
        ("supple-large-recording-" + std::to_string(supple::test::processId()) + ".wav");
    WavStreamWriter writer;
    REQUIRE(writer.open(path, 48000, 2));
    REQUIRE(writer.flush());
    writer.abandon();
    struct Cleanup {
        fs::path path;
        ~Cleanup() { fs::remove(path); }
    } cleanup{path};
    auto* stream = supple::audio::detail::files::open(path, true);
    REQUIRE(stream);
#if defined(_WIN32)
    // Do not commit GiB of storage on the runner for a positioning test.
    DWORD bytes = 0;
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
    if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr)) {
        std::fclose(stream);
        MESSAGE("filesystem cannot create a sparse test file");
        return;
    }
#endif
    constexpr std::int64_t end = (std::int64_t{1} << 31) + 44 + 8;
    REQUIRE(supple::audio::detail::files::seek(stream, end - 1, SEEK_SET) == 0);
    REQUIRE(std::fputc(0, stream) != EOF);
    CHECK(supple::audio::detail::files::tell(stream) == end);
    REQUIRE(std::fclose(stream) == 0);
    auto info = inspectWavStream(path);
    REQUIRE(info);
    CHECK(info->fileFrames == static_cast<std::uint64_t>(end - 44) / 8);
    auto repaired = repairWavStream(path);
    REQUIRE(repaired);
    CHECK(*repaired == info->fileFrames);
    CHECK(inspectWavStream(path)->headerFrames == *repaired);
}

#if defined(_WIN32)
TEST_CASE("publish retries while another program holds the target without delete sharing") {
    const fs::path dir = fs::temp_directory_path() / "supple-audio-busy";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path target = dir / "target.wav";
    const fs::path first = dir / "first.tmp";
    const fs::path second = dir / "second.tmp";
    for (const auto& file : {target, first, second}) std::ofstream(file, std::ios::binary) << "x";
    HANDLE holder = ::CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(holder != INVALID_HANDLE_VALUE);
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::CloseHandle(holder);
        holder = INVALID_HANDLE_VALUE;
    });
    bool busy = true;
    const bool published = detail::files::publish(first, target, &busy);
    releaser.join();
    CHECK(published);
    CHECK_FALSE(busy);

    holder = ::CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(holder != INVALID_HANDLE_VALUE);
    busy = false;
    CHECK_FALSE(detail::files::publish(second, target, &busy));
    CHECK(busy);
    ::CloseHandle(holder);
    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif

namespace {
bool contains(const std::vector<std::uint8_t>& bytes, std::string_view text) {
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}
}  // namespace

TEST_CASE("AIFF: 16 and 24 bits decode as the same WAV does, with markers and names") {
    const AudioData d = tone(4410, 44100, 2);
    for (int bits : {16, 24}) {
        AiffOptions aiff;
        aiff.bitsPerSample = bits;
        aiff.ditherSeed = 9;
        aiff.metadata.tags.title = "Song";
        aiff.metadata.cues = {{100, 0, "Verse"}, {2000, 500, "Chorus"}};
        auto bytes = encodeAiff(d, aiff);
        REQUIRE(bytes);
        CHECK(contains(*bytes, "FORM"));
        CHECK(contains(*bytes, "MARK"));
        CHECK(contains(*bytes, "Verse"));
        CHECK(contains(*bytes, "NAME"));
        auto decoded = decodeAiff(*bytes);
        REQUIRE(decoded);
        CHECK(decoded->sampleRate == 44100);
        WavOptions wav{bits == 16 ? SampleFormat::Pcm16 : SampleFormat::Pcm24, true, 9};
        auto reference = readAudio(*encodeWav(d, wav));
        REQUIRE(reference);
        CHECK(decoded->channels == reference->channels);
        CHECK(encodeAiff(d, aiff).value() == *bytes);  // deterministic
    }
    AiffOptions bad;
    bad.bitsPerSample = 32;
    CHECK_FALSE(encodeAiff(d, bad));
    CHECK_FALSE(decodeAiff(*encodeWav(d)));
}

TEST_CASE("noise-shaped dither is deterministic and moves the error up the band") {
    AudioData d;
    d.sampleRate = 48000;
    d.channels.assign(1, std::vector<float>(48000));
    for (std::size_t i = 0; i < d.frames(); ++i)
        d.channels[0][i] =
            static_cast<float>(0.3 * std::sin(2 * M_PI * 997.0 * static_cast<double>(i) / 48000));
    const WavOptions pcm{SampleFormat::Pcm16, true, 3};
    EncodeExtras shaped;
    shaped.shaped = true;
    auto a = encodeWav(d, pcm, shaped);
    REQUIRE(a);
    CHECK(encodeWav(d, pcm, shaped).value() == *a);
    CHECK(*encodeWav(d, pcm) != *a);
    // Error spectrum: the ratio of first-difference energy (high frequencies)
    // to plain energy is larger when the error is shaped.
    auto tilt = [&](const std::vector<std::uint8_t>& bytes) {
        auto back = readAudio(bytes);
        REQUIRE(back);
        double low = 0, high = 0, previous = 0;
        for (std::size_t i = 0; i < d.frames(); ++i) {
            const double e = double(back->channels[0][i]) - double(d.channels[0][i]);
            low += e * e;
            high += (e - previous) * (e - previous);
            previous = e;
        }
        return high / low;
    };
    CHECK(tilt(*a) > 1.5 * tilt(*encodeWav(d, pcm)));
    // FLAC and AIFF quantize alike.
    FlacOptions flac{16, true, 3, 4096};
    CHECK(readAudio(*encodeFlac(d, flac, shaped))->channels == readAudio(*a)->channels);
}

TEST_CASE("WAV and FLAC metadata: INFO, bext v2, cue points and Vorbis comments") {
    const AudioData d = tone(4800, 48000, 2);
    EncodeExtras extras;
    extras.metadata.tags.title = "Song";
    extras.metadata.tags.artist = "Band";
    extras.metadata.tags.isrc = "USRC17607839";
    extras.metadata.tags.software = "Test";
    Bext bext;
    bext.originator = "Test";
    bext.timeReference = 48000ull * 3600;
    bext.loudness = -14.0;
    bext.maxTruePeak = -1.0;
    bext.codingHistory = "A=PCM,F=48000,W=24,M=stereo\r\n";
    extras.metadata.bext = bext;
    extras.metadata.cues = {{0, 0, "Start"}, {2400, 1200, "Hook"}};
    const WavOptions wav{SampleFormat::Pcm24, true, 1};
    auto bytes = encodeWav(d, wav, extras);
    REQUIRE(bytes);
    for (const char* id :
         {"bext", "LIST", "INFO", "INAM", "IART", "ISFT", "cue ", "adtl", "labl", "ltxt", "Hook"})
        CHECK_MESSAGE(contains(*bytes, id), id);
    CHECK((*bytes)[4] + ((*bytes)[5] << 8) + ((*bytes)[6] << 16) + ((*bytes)[7] << 24) ==
          static_cast<int>(bytes->size() - 8));
    // bext: version 2 and the loudness in hundredths right after the time reference.
    const std::string_view id = "bext";
    const auto at = static_cast<std::size_t>(
        std::search(bytes->begin(), bytes->end(), id.begin(), id.end()) - bytes->begin() + 8);
    const std::size_t time = at + 256 + 32 + 32 + 10 + 8;
    std::uint64_t reference = 0;
    for (int i = 0; i < 8; ++i)
        reference |= std::uint64_t((*bytes)[time + static_cast<std::size_t>(i)]) << (8 * i);
    CHECK(reference == 48000ull * 3600);
    CHECK((*bytes)[time + 8] == 2);
    const auto loudness =
        static_cast<std::int16_t>((*bytes)[time + 74] | ((*bytes)[time + 75] << 8));
    CHECK(loudness == -1400);
    // The audio reads back exactly as without metadata.
    CHECK(readAudio(*bytes)->channels == readAudio(*encodeWav(d, wav))->channels);
    auto flac = encodeFlac(d, FlacOptions{}, extras);
    REQUIRE(flac);
    CHECK(contains(*flac, "TITLE=Song"));
    CHECK(contains(*flac, "ISRC=USRC17607839"));
    CHECK(readAudio(*flac)->channels == readAudio(*encodeFlac(d))->channels);
}
