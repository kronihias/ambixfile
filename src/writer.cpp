// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner

#include "internal.hpp"

namespace ambixfile {

struct Writer::Impl {
    std::unique_ptr<Stream> owned;
    Stream* stream = nullptr;
    Info info;
    Matrix adaptor;
    // Metadata not yet written: bound for the header until the first write,
    // for the end of the file afterwards.
    std::vector<Chunk> chunks;
    std::vector<Marker> markers;
    std::vector<Region> regions;
    std::uint32_t nextId = 1;
    bool started = false;
    bool failed = false;
    float hybridBits = 0.0f;
    int threads = 0;
    std::int64_t dataSizeOffset = 0; // position of the data chunk's size field
    std::int64_t dataStart = 0;
    std::uint32_t bytesPerFrame = 0;
    std::uint32_t blockFrames = 0;
    std::vector<std::uint8_t> raw;
#ifdef AMBIXFILE_ENABLE_WAVPACK
    detail::WvEncoder* wv = nullptr;
    std::vector<std::int32_t> ints;
#endif

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
#ifdef AMBIXFILE_ENABLE_WAVPACK
        if (wv) detail::wvCloseEncoder(wv, std::vector<std::uint8_t>());
#endif
    }

    Status configure(const WriterConfig& config);
    void start();

    std::vector<double> scratch; // for integer input

    template <typename I>
    std::int64_t writeInts(const I* src, std::int64_t frames, double scale) {
        if (!stream || !src) return 0;
        const std::uint32_t channels = info.fileChannels();
        std::int64_t total = 0;
        while (frames > 0) {
            const std::int64_t n = frames < blockFrames ? frames : static_cast<std::int64_t>(blockFrames);
            const std::size_t count = static_cast<std::size_t>(n) * channels;
            for (std::size_t i = 0; i < count; ++i) scratch[i] = static_cast<double>(src[i]) / scale;
            const std::int64_t done = write(scratch.data(), n);
            total += done;
            if (done < n) break;
            src += count;
            frames -= n;
        }
        return total;
    }
    Status finish();

    template <typename T>
    std::int64_t write(const T* src, std::int64_t frames) {
        if (!stream || !src || frames <= 0) return 0;
        if (!started) start();
        if (failed) return 0;
        const std::uint32_t channels = info.fileChannels();
        std::int64_t total = 0;
        while (frames > 0) {
            const std::int64_t n = frames < blockFrames ? frames : static_cast<std::int64_t>(blockFrames);
            const std::size_t count = static_cast<std::size_t>(n) * channels;
            std::int64_t done = 0;
#ifdef AMBIXFILE_ENABLE_WAVPACK
            if (info.container == Container::WavPack) {
                if (info.sampleFormat == SampleFormat::Float32) {
                    for (std::size_t i = 0; i < count; ++i)
                        ints[i] = detail::bitCast<std::int32_t>(static_cast<float>(src[i]));
                } else {
                    const double scale = static_cast<double>(std::uint64_t(1) << (bytesPerFrame / channels * 8 - 1));
                    for (std::size_t i = 0; i < count; ++i)
                        ints[i] = static_cast<std::int32_t>(detail::toFixed(static_cast<double>(src[i]), scale));
                }
                done = detail::wvPack(wv, ints.data(), static_cast<std::uint32_t>(n)) ? n : 0;
            } else
#endif
            {
                detail::encodeSamples(src, raw.data(), count, info.sampleFormat, info.bigEndian);
                const std::size_t bytes = static_cast<std::size_t>(n) * bytesPerFrame;
                done = static_cast<std::int64_t>(stream->write(raw.data(), bytes) / bytesPerFrame);
            }
            total += done;
            info.frames += static_cast<std::uint64_t>(done);
            if (done != n) {
                failed = true;
                break;
            }
            src += count;
            frames -= n;
        }
        return total;
    }
};

Status Writer::Impl::configure(const WriterConfig& config) {
    using namespace detail;
    if (!(config.sampleRate > 0.0) || !std::isfinite(config.sampleRate)) return Status::InvalidArgument;
    const std::uint64_t channels64 = static_cast<std::uint64_t>(config.ambiChannels) + config.extraChannels;
    if (config.ambiChannels == 0 || channels64 > 65536) return Status::InvalidArgument;
    const std::uint32_t channels = static_cast<std::uint32_t>(channels64);
    for (const Chunk& chunk : config.chunks)
        if (chunk.type == kChunkDesc || chunk.type == kChunkData) return Status::InvalidArgument;

    std::uint32_t order = 0;
    adaptor = config.adaptorMatrix;
    if (adaptor.empty()) {
        if (!channelsToOrder(config.ambiChannels, order)) return Status::InvalidArgument;
        if (config.extraChannels > 0) adaptor = Matrix::identity(config.ambiChannels);
    } else if (adaptor.cols() != config.ambiChannels || !channelsToOrder(adaptor.rows(), order)) {
        return Status::InvalidAdaptorMatrix;
    }

    info = Info();
    info.sampleRate = config.sampleRate;
    info.sampleFormat = config.sampleFormat;
    info.bigEndian = config.bigEndian;
    info.container = config.container;
    info.fileFormat = adaptor.empty() ? FileFormat::Basic : FileFormat::Extended;
    info.ambiChannels = config.ambiChannels;
    info.extraChannels = config.extraChannels;
    info.fullAmbiChannels = adaptor.empty() ? config.ambiChannels : adaptor.rows();

    if (config.container == Container::WavPack) {
        if (!wavpackAvailable()) return Status::UnsupportedFormat;
        if (config.wavpackHybridBits != 0.0f &&
            !(config.wavpackHybridBits >= 2.0f && config.wavpackHybridBits <= 23.9f))
            return Status::InvalidArgument;
        // WavPack takes whole-number sample rates and has no 64-bit floats.
        if (config.sampleRate != std::floor(config.sampleRate) || config.sampleRate > 2147483647.0)
            return Status::InvalidArgument;
        if (info.sampleFormat == SampleFormat::Float64) info.sampleFormat = SampleFormat::Float32;
        info.lossy = config.wavpackHybridBits != 0.0f;
        info.lossyBitsPerSample = config.wavpackHybridBits;
        hybridBits = config.wavpackHybridBits;
        threads = config.wavpackThreads;
    }

    chunks = config.chunks;
    markers = config.markers;
    regions = config.regions;
    bytesPerFrame = channels * bytesPerSample(info.sampleFormat);
    blockFrames = blockFramesFor(channels);
    scratch.resize(static_cast<std::size_t>(blockFrames) * channels);
    return Status::Ok;
}

// Write everything that precedes the audio. Deferred until the first write()
// or close() so that metadata added after open() still lands in the header.
void Writer::Impl::start() {
    using namespace detail;
    started = true;
    const std::vector<std::uint8_t> head =
        buildCafHead(info, adaptor, chunks, markers, regions, nextId, dataSizeOffset);
    chunks.clear();
    markers.clear();
    regions.clear();
    const std::uint32_t channels = info.fileChannels();

    if (info.container == Container::WavPack) {
#ifdef AMBIXFILE_ENABLE_WAVPACK
        Status status = Status::Ok;
        wv = wvOpenEncoder(*stream, info, hybridBits, threads, head, status);
        if (!wv) failed = true;
        ints.resize(static_cast<std::size_t>(blockFrames) * channels);
#else
        failed = true;
#endif
        return;
    }
    raw.resize(static_cast<std::size_t>(blockFrames) * channels * bytesPerSample(info.sampleFormat));
    dataStart = static_cast<std::int64_t>(head.size());
    if (stream->write(head.data(), head.size()) != head.size()) failed = true;
}

Status Writer::Impl::finish() {
    using namespace detail;
    if (!started) start();
    Status status = failed ? Status::IoError : Status::Ok;

    std::vector<std::uint8_t> tail;
    appendMetadataChunks(tail, chunks, markers, regions, nextId);

    if (info.container == Container::WavPack) {
#ifdef AMBIXFILE_ENABLE_WAVPACK
        if (wv) {
            const Status closed = wvCloseEncoder(wv, tail);
            wv = nullptr;
            if (closed != Status::Ok) status = closed;
        }
        // WavPack cannot represent a file without audio: no decoder opens it.
        if (status == Status::Ok && info.frames == 0) status = Status::UnsupportedFormat;
#endif
    } else {
        const std::int64_t dataBytes = static_cast<std::int64_t>(info.frames) * bytesPerFrame;
        std::uint8_t size[8];
        storeBE64(size, static_cast<std::uint64_t>(dataBytes + 4));
        // A stream that cannot seek keeps the "until end of file" size, which
        // is valid as long as nothing follows the audio.
        bool sized = false;
        if (stream->seek(dataSizeOffset)) {
            sized = stream->write(size, 8) == 8 && stream->seek(dataStart + dataBytes);
            if (!sized) status = Status::IoError;
        }
        if (!tail.empty() && (!sized || stream->write(tail.data(), tail.size()) != tail.size()))
            status = Status::IoError;
    }
    if (!stream->flush()) status = Status::IoError;
    return status;
}

Writer::Writer() : impl_(new Impl) {}
Writer::~Writer() {
    if (isOpen()) close();
}
Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&& other) noexcept {
    if (this != &other) {
        if (isOpen()) close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

Status Writer::open(const char* utf8Path, const WriterConfig& config) {
    // Check the configuration first so a bad one does not truncate the file.
    Impl probe;
    const Status valid = probe.configure(config);
    if (valid != Status::Ok) return valid;
    std::unique_ptr<Stream> stream = openFileStream(utf8Path, true);
    if (!stream) return Status::IoError;
    return open(std::move(stream), config);
}

Status Writer::open(std::unique_ptr<Stream> stream, const WriterConfig& config) {
    if (!stream) return Status::InvalidArgument;
    Stream& ref = *stream;
    const Status status = open(ref, config);
    if (status == Status::Ok) impl_->owned = std::move(stream);
    return status;
}

Status Writer::open(Stream& stream, const WriterConfig& config) {
    if (isOpen()) close();
    impl_.reset(new Impl);
    const Status status = impl_->configure(config);
    if (status != Status::Ok) {
        impl_.reset(new Impl);
        return status;
    }
    impl_->stream = &stream;
    return Status::Ok;
}

bool Writer::isOpen() const noexcept { return impl_ && impl_->stream; }

const Info& Writer::info() const noexcept {
    static const Info none;
    return impl_ ? impl_->info : none;
}

std::int64_t Writer::write(const float* interleaved, std::int64_t frames) {
    return impl_ ? impl_->write(interleaved, frames) : 0;
}
std::int64_t Writer::write(const double* interleaved, std::int64_t frames) {
    return impl_ ? impl_->write(interleaved, frames) : 0;
}

std::int64_t Writer::write(const std::int16_t* interleaved, std::int64_t frames) {
    return impl_ ? impl_->writeInts(interleaved, frames, 32768.0) : 0;
}
std::int64_t Writer::write(const std::int32_t* interleaved, std::int64_t frames) {
    return impl_ ? impl_->writeInts(interleaved, frames, 2147483648.0) : 0;
}

Status Writer::addMarker(Marker marker) {
    if (!isOpen()) return Status::NotOpen;
    impl_->markers.push_back(std::move(marker));
    return Status::Ok;
}

Status Writer::addRegion(Region region) {
    if (!isOpen()) return Status::NotOpen;
    impl_->regions.push_back(std::move(region));
    return Status::Ok;
}

Status Writer::addChunk(Chunk chunk) {
    if (!isOpen()) return Status::NotOpen;
    if (chunk.type == detail::kChunkDesc || chunk.type == detail::kChunkData) return Status::InvalidArgument;
    impl_->chunks.push_back(std::move(chunk));
    return Status::Ok;
}

Status Writer::close() {
    if (!isOpen()) return Status::NotOpen;
    const Status status = impl_->finish();
    impl_.reset(new Impl);
    return status;
}

} // namespace ambixfile
