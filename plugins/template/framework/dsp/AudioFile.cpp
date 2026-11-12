// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "dsp/AudioFile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Files.h"
#include "dr_flac.h"
#include "dr_wav.h"
#include "dsp/Fir.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace supple::dsp {

namespace {

#ifdef _WIN32
// UTF-8 paths, as the framework keeps them, to the wide ones Windows opens.
std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 1)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(std::max(n - 1, 0)));
    return w;
}
#endif

void fail(std::string* error, const std::string& message) {
    if (error) *error = message;
}

// dr_wav's own file writer ignores a failed header rewrite and a failed
// close, which is where a full disk shows on a buffered file; this one
// remembers every failure.
struct WavSink {
    std::FILE* file = nullptr;
    bool failed = false;
    static size_t write(void* user, const void* data, size_t bytes) {
        auto& sink = *static_cast<WavSink*>(user);
        const size_t written = std::fwrite(data, 1, bytes, sink.file);
        if (written != bytes) sink.failed = true;
        return written;
    }
    static drwav_bool32 seek(void* user, int offset, drwav_seek_origin origin) {
        auto& sink = *static_cast<WavSink*>(user);
        const int whence = origin == DRWAV_SEEK_CUR ? SEEK_CUR : origin == DRWAV_SEEK_END ? SEEK_END : SEEK_SET;
        if (std::fseek(sink.file, offset, whence) == 0) return DRWAV_TRUE;
        sink.failed = true;
        return DRWAV_FALSE;
    }
};

}  // namespace

// --- AudioReader -------------------------------------------------------------------

struct AudioReader::Decoder {
    drwav wav{};
    bool isWav = false;
    drflac* flac = nullptr;

    ~Decoder() { close(); }
    void close() {
        if (isWav) drwav_uninit(&wav);
        if (flac) drflac_close(flac);
        isWav = false;
        flac = nullptr;
    }
};

AudioReader::AudioReader() = default;
AudioReader::~AudioReader() = default;

bool AudioReader::open(const std::string& path, std::string* error) {
    close();
    auto d = std::make_unique<Decoder>();
    if (files::isFactoryReference(path)) {
        // The product's own file, read where it lies in the binary.
        const assets::Asset a = files::factoryFile(path);
        if (!a) {
            fail(error, "the product has no factory file of that name");
            return false;
        }
        d->isWav = drwav_init_memory(&d->wav, a.data, a.size, nullptr) == DRWAV_TRUE;
        if (!d->isWav) d->flac = drflac_open_memory(a.data, a.size, nullptr);
    } else {
#ifdef _WIN32
        const std::wstring w = wide(path);
        d->isWav = drwav_init_file_w(&d->wav, w.c_str(), nullptr) == DRWAV_TRUE;
        if (!d->isWav) d->flac = drflac_open_file_w(w.c_str(), nullptr);
#else
        d->isWav = drwav_init_file(&d->wav, path.c_str(), nullptr) == DRWAV_TRUE;
        if (!d->isWav) d->flac = drflac_open_file(path.c_str(), nullptr);
#endif
    }
    if (!d->isWav && !d->flac) {
        fail(error, "not a WAV, AIFF or FLAC file this can read");
        return false;
    }
    if ((d->isWav && (d->wav.channels == 0 || d->wav.sampleRate == 0)) ||
        (d->flac && (d->flac->channels == 0 || d->flac->sampleRate == 0))) {
        fail(error, "the file has no channels or no sample rate");
        return false;
    }
    decoder_ = std::move(d);
    return true;
}

void AudioReader::close() { decoder_.reset(); }
bool AudioReader::isOpen() const { return decoder_ != nullptr; }
int AudioReader::channels() const {
    if (!decoder_) return 0;
    return decoder_->isWav ? decoder_->wav.channels : decoder_->flac->channels;
}
double AudioReader::sampleRate() const {
    if (!decoder_) return 0.0;
    return decoder_->isWav ? decoder_->wav.sampleRate : decoder_->flac->sampleRate;
}
uint64_t AudioReader::frames() const {
    if (!decoder_) return 0;
    return decoder_->isWav ? decoder_->wav.totalPCMFrameCount : decoder_->flac->totalPCMFrameCount;
}
size_t AudioReader::read(float* interleaved, size_t frames) {
    if (!decoder_) return 0;
    const size_t read = static_cast<size_t>(decoder_->isWav ? drwav_read_pcm_frames_f32(&decoder_->wav, frames, interleaved)
                                                          : drflac_read_pcm_frames_f32(decoder_->flac, frames, interleaved));
    for (size_t n = 0; n < read * static_cast<size_t>(channels()); ++n)
        if (!std::isfinite(interleaved[n])) interleaved[n] = 0.0f;
    return read;
}
bool AudioReader::seek(uint64_t frame) {
    if (!decoder_) return false;
    return decoder_->isWav ? drwav_seek_to_pcm_frame(&decoder_->wav, frame) == DRWAV_TRUE
                           : drflac_seek_to_pcm_frame(decoder_->flac, frame) == DRFLAC_TRUE;
}

// --- whole files ---------------------------------------------------------------------

bool loadAudioFile(const std::string& path, AudioData& out, std::string* error, double maxSeconds) {
    AudioReader reader;
    if (!reader.open(path, error)) return false;
    const int channels = reader.channels();
    // A file that does not say how long it is is read to its end, or to the limit.
    uint64_t limit = UINT64_MAX;
    if (const double most = maxSeconds * reader.sampleRate(); most > 0.0 && most < 9.0e18)
        limit = static_cast<uint64_t>(std::ceil(most));
    std::vector<float> interleaved;
    std::vector<float> chunk(static_cast<size_t>(channels) * 65536);
    for (uint64_t total = 0; total < limit;) {
        const size_t n = reader.read(chunk.data(), static_cast<size_t>(std::min<uint64_t>(65536, limit - total)));
        if (n == 0) break;
        interleaved.insert(interleaved.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n * static_cast<size_t>(channels)));
        total += n;
    }
    const size_t frames = interleaved.size() / static_cast<size_t>(channels);
    if (frames == 0) {
        fail(error, "the file holds no audio");
        return false;
    }
    out.sampleRate = reader.sampleRate();
    out.channel.assign(static_cast<size_t>(channels), std::vector<float>(frames));
    for (size_t i = 0; i < frames; ++i)
        for (size_t c = 0; c < static_cast<size_t>(channels); ++c) out.channel[c][i] = interleaved[i * static_cast<size_t>(channels) + c];
    return true;
}

bool saveWavFile(const std::string& path, const AudioData& audio, std::string* error) {
    if (audio.channels() == 0 || !std::isfinite(audio.sampleRate) ||
        std::round(audio.sampleRate) < 1.0 || std::round(audio.sampleRate) > 4294967295.0) {
        fail(error, "nothing to write");
        return false;
    }
    for (const auto& channel : audio.channel) {
        if (channel.size() != audio.frames()) {
            fail(error, "the channels have different lengths");
            return false;
        }
    }
    drwav_data_format format{};
    format.container = drwav_container_riff;
    format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
    format.channels = static_cast<drwav_uint32>(audio.channels());
    format.sampleRate = static_cast<drwav_uint32>(std::round(audio.sampleRate));
    format.bitsPerSample = 32;
    WavSink sink;
#ifdef _WIN32
    sink.file = _wfopen(wide(path).c_str(), L"wb");
#else
    sink.file = std::fopen(path.c_str(), "wb");
#endif
    drwav wav;
    if (!sink.file || drwav_init_write(&wav, &format, WavSink::write, WavSink::seek, &sink, nullptr) != DRWAV_TRUE) {
        if (sink.file) std::fclose(sink.file);
        fail(error, "cannot write " + path);
        return false;
    }
    const size_t frames = audio.frames();
    std::vector<float> interleaved(frames * static_cast<size_t>(audio.channels()));
    for (size_t i = 0; i < frames; ++i)
        for (size_t c = 0; c < audio.channel.size(); ++c) interleaved[i * audio.channel.size() + c] = audio.channel[c][i];
    const drwav_uint64 written = drwav_write_pcm_frames(&wav, frames, interleaved.data());
    const bool finished = drwav_uninit(&wav) == DRWAV_SUCCESS;
    const bool flushed = std::fflush(sink.file) == 0;
    const bool closed = std::fclose(sink.file) == 0;
    if (written != frames || !finished || !flushed || !closed || sink.failed) {
        fail(error, "the disk took only part of " + path);
        return false;
    }
    return true;
}

// --- resampling -------------------------------------------------------------------

AudioData resample(const AudioData& in, double sampleRate) {
    if (in.sampleRate <= 0.0 || sampleRate <= 0.0 || in.sampleRate == sampleRate || in.frames() == 0) return in;
    const double ratio = sampleRate / in.sampleRate;
    const double scale = std::min(1.0, ratio);  // of the input's band, what survives
    // In input samples: the cutoff halfway across the transition from 0.45
    // to 0.5 of the lower rate, and a kernel as wide as that steepness needs.
    const double cutoff = 0.475 * scale, width = 0.05 * scale, attenuation = 100.0;
    const double halfWidth = 0.5 * kaiserTaps(attenuation, width);
    const double beta = kaiserBeta(attenuation);
    // The window, tabulated: evaluating the Bessel function per tap is slow.
    constexpr int kTable = 8192;
    std::vector<double> window(kTable + 2);
    for (int k = 0; k <= kTable + 1; ++k) window[static_cast<size_t>(k)] = kaiser(std::min(1.0, static_cast<double>(k) / kTable), beta);
    auto kernel = [&](double u) {
        const double a = std::fabs(u) / halfWidth;
        if (a >= 1.0) return 0.0;
        const double x = a * kTable;
        const int i = static_cast<int>(x);
        const double w = window[static_cast<size_t>(i)] + (x - i) * (window[static_cast<size_t>(i) + 1] - window[static_cast<size_t>(i)]);
        return 2.0 * cutoff * sinc(2.0 * cutoff * u) * w;
    };

    AudioData out;
    out.sampleRate = sampleRate;
    const size_t inFrames = in.frames();
    const size_t outFrames = static_cast<size_t>(std::max(1.0, std::round(static_cast<double>(inFrames) * ratio)));
    out.channel.assign(in.channel.size(), std::vector<float>(outFrames));
    // An output sample's tap weights, worked out once for every channel.
    std::vector<double> weights(static_cast<size_t>(2.0 * halfWidth) + 2);
    for (size_t n = 0; n < outFrames; ++n) {
        const double t = static_cast<double>(n) / ratio;  // in input samples
        const long long first = std::max(0LL, static_cast<long long>(std::ceil(t - halfWidth)));
        const long long last = std::min(static_cast<long long>(std::floor(t + halfWidth)), static_cast<long long>(inFrames) - 1);
        const size_t taps = last >= first ? static_cast<size_t>(last - first + 1) : 0;
        // The first channel is summed as they are worked out: a mono file
        // goes through its taps once.
        double* w = weights.data();
        const float* x0 = in.channel[0].data() + first;
        double sum0 = 0.0;
        for (size_t i = 0; i < taps; ++i) {
            w[i] = kernel(t - static_cast<double>(first + static_cast<long long>(i)));
            sum0 += x0[i] * w[i];
        }
        out.channel[0][n] = static_cast<float>(sum0);
        for (size_t c = 1; c < in.channel.size(); ++c) {
            const float* x = in.channel[c].data() + first;
            double sum = 0.0;
            for (size_t i = 0; i < taps; ++i) sum += x[i] * w[i];
            out.channel[c][n] = static_cast<float>(sum);
        }
    }
    return out;
}

// --- DiskStream -------------------------------------------------------------------

bool DiskStream::open(const std::string& path, double headSeconds, std::string* error) {
    close();
    if (!reader_.open(path, error)) return false;
    channels_ = reader_.channels();
    sampleRate_ = reader_.sampleRate();
    frames_ = reader_.frames();
    if (frames_ == 0) {
        close();
        fail(error, "stream length is unknown or empty; use loadAudioFile instead");
        return false;
    }
    headFrames_ = std::min<uint64_t>(frames_, static_cast<uint64_t>(std::max(0.0, headSeconds) * sampleRate_));
    head_.assign(static_cast<size_t>(headFrames_) * static_cast<size_t>(channels_), 0.0f);
    headFrames_ = reader_.read(head_.data(), static_cast<size_t>(headFrames_));
    chunks_.assign(kChunks, Chunk{});
    spare_.clear();
    spare_.reserve(kChunks);
    for (int k = 0; k < kChunks; ++k) {
        chunks_[static_cast<size_t>(k)].data.assign(kChunkFrames * static_cast<size_t>(channels_), 0.0f);
        spare_.push_back(k);
    }
    int stale = 0;
    while (filled_.pop(stale)) {}
    while (free_.pop(stale)) {}
    wanted_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    generation_ = serviceGeneration_ = 0;
    position_ = 0;
    current_ = -1;
    offset_ = 0;
    next_ = headFrames_;
    open_ = true;
    service();  // the read-ahead, before anything plays
    return true;
}

void DiskStream::close() {
    reader_.close();
    open_ = false;
    chunks_.clear();
    head_.clear();
    frames_ = headFrames_ = 0;
    channels_ = 0;
}

void DiskStream::restart() {
    if (current_ >= 0) free_.push(current_);
    current_ = -1;
    ++generation_;
    wanted_.store(generation_, std::memory_order_release);
    position_ = 0;
}

int DiskStream::read(double* const* out, int channels, int frames) {
    if (!open_ || channels <= 0) return 0;
    // Chunks read before the last restart go straight back.
    int index = 0;
    while (current_ < 0 && filled_.peek(index) && chunks_[static_cast<size_t>(index)].generation != generation_) {
        filled_.consume();
        free_.push(index);
    }
    const auto copy = [&](const float* source, int at, size_t n) {
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < channels; ++c)
                out[c][static_cast<size_t>(at) + i] = source[i * static_cast<size_t>(channels_) + static_cast<size_t>(std::min(c, channels_ - 1))];
    };
    int written = 0;
    while (written < frames && position_ < frames_) {
        size_t n = 0;
        if (position_ < headFrames_) {
            n = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(frames - written), headFrames_ - position_));
            copy(head_.data() + position_ * static_cast<uint64_t>(channels_), written, n);
        } else {
            if (current_ < 0) {
                if (!filled_.pop(index)) {
                    underruns_.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                const Chunk& c = chunks_[static_cast<size_t>(index)];
                if (c.generation != generation_ || c.start != position_) {
                    free_.push(index);
                    continue;
                }
                current_ = index;
                offset_ = 0;
            }
            const Chunk& c = chunks_[static_cast<size_t>(current_)];
            n = std::min(static_cast<size_t>(frames - written), c.frames - offset_);
            copy(c.data.data() + offset_ * static_cast<size_t>(channels_), written, n);
            offset_ += n;
            if (offset_ == c.frames) {
                free_.push(current_);
                current_ = -1;
            }
        }
        position_ += n;
        written += static_cast<int>(n);
    }
    return written;
}

void DiskStream::service() {
    if (!open_) return;
    const uint32_t generation = wanted_.load(std::memory_order_acquire);
    if (generation != serviceGeneration_) {
        serviceGeneration_ = generation;
        next_ = headFrames_;
        reader_.seek(headFrames_);
    }
    int index = 0;
    while (free_.pop(index)) spare_.push_back(index);
    while (!spare_.empty() && next_ < frames_ && filled_.writable()) {
        index = spare_.back();
        Chunk& c = chunks_[static_cast<size_t>(index)];
        const size_t n = reader_.read(c.data.data(), kChunkFrames);
        if (n == 0) {
            next_ = frames_;
            break;
        }
        spare_.pop_back();
        c.generation = generation;
        c.start = next_;
        c.frames = n;
        next_ += n;
        filled_.push(index);
        // A restart while reading: what follows belongs to it.
        if (wanted_.load(std::memory_order_acquire) != generation) break;
    }
}

}  // namespace supple::dsp
