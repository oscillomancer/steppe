// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string_view>

#include <supple/codecs/codecs.h>

using namespace supple;

namespace {
audio::AudioData tone(double rate, std::size_t channels, double seconds) {
    audio::AudioData d;
    d.sampleRate = rate;
    d.channels.assign(channels, std::vector<float>(static_cast<std::size_t>(rate * seconds)));
    for (std::size_t c = 0; c < channels; ++c)
        for (std::size_t i = 0; i < d.frames(); ++i)
            d.channels[c][i] = static_cast<float>(
                0.5 * std::sin(2 * M_PI * (440.0 + 110.0 * double(c)) * double(i) / rate));
    return d;
}
bool contains(const std::vector<std::uint8_t>& bytes, std::string_view text) {
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}
double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0;
    for (std::size_t i = from; i < to; ++i) sum += double(x[i]) * double(x[i]);
    return std::sqrt(sum / double(to - from));
}
}  // namespace

TEST_CASE("MP3: CBR and VBR decode to the same length and level, with tags, deterministically") {
    const auto d = tone(48000, 2, 1.0);
    for (bool vbr : {false, true}) {
        codecs::Mp3Options options;
        options.vbr = vbr;
        options.bitrateKbps = 192;
        options.tags.title = "Café";
        options.tags.isrc = "USRC17607839";
        options.tags.software = "Test";
        auto bytes = codecs::encodeMp3(d, options);
        REQUIRE(bytes);
        CHECK(codecs::encodeMp3(d, options).value() == *bytes);  // byte-identical
        CHECK(contains(*bytes, "ID3"));
        CHECK(contains(*bytes, "TIT2"));
        CHECK(contains(*bytes, "TSRC"));
        CHECK(contains(*bytes, vbr ? "Xing" : "Info"));
        CHECK(contains(*bytes, "LAME3.100"));
        auto decoded = audio::readAudio(*bytes);
        REQUIRE(decoded);
        CHECK(decoded->sampleRate == 48000);
        CHECK(decoded->channelCount() == 2);
        // Gapless: the LAME header's delay and padding give back the length.
        CHECK(std::abs(double(decoded->frames()) - double(d.frames())) <= 1152);
        CHECK(rms(decoded->channels[0], 4800, 43200) ==
              doctest::Approx(rms(d.channels[0], 4800, 43200)).epsilon(0.05));
    }
    auto mono = codecs::encodeMp3(tone(44100, 1, 0.2));
    REQUIRE(mono);
    CHECK(audio::readAudio(*mono)->channelCount() == 1);
    CHECK_FALSE(codecs::encodeMp3(tone(96000, 2, 0.1)));
    CHECK_FALSE(codecs::encodeMp3(tone(48000, 3, 0.1)));
}

TEST_CASE("Ogg Vorbis: a round trip keeps length and level; comments; deterministic") {
    const auto d = tone(44100, 2, 1.0);
    codecs::VorbisOptions options;
    options.quality = 5;
    options.serial = 1234;
    options.tags.title = "Loop";
    options.tags.picture = {0x89, 'P', 'N', 'G'};
    options.tags.pictureMime = "image/png";
    auto bytes = codecs::encodeVorbis(d, options);
    REQUIRE(bytes);
    CHECK(codecs::encodeVorbis(d, options).value() == *bytes);
    CHECK(contains(*bytes, "OggS"));
    CHECK(contains(*bytes, "TITLE=Loop"));
    CHECK(contains(*bytes, "METADATA_BLOCK_PICTURE="));
    auto decoded = codecs::decodeVorbis(*bytes);
    REQUIRE(decoded);
    CHECK(decoded->sampleRate == 44100);
    CHECK(decoded->frames() == d.frames());  // granule positions trim exactly
    CHECK(rms(decoded->channels[1], 4410, 39690) ==
          doctest::Approx(rms(d.channels[1], 4410, 39690)).epsilon(0.05));
    options.serial = 99;
    CHECK(codecs::encodeVorbis(d, options).value() != *bytes);
    CHECK_FALSE(codecs::decodeVorbis(std::vector<std::uint8_t>{1, 2, 3}));
    options.quality = 11;
    CHECK_FALSE(codecs::encodeVorbis(d, options));
}
