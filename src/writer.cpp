// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner

#include "internal.hpp"

namespace ambixfile {
namespace detail {

inline void appendBytes(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
    const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
    out.insert(out.end(), p, p + size);
}
inline void appendBE32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    std::uint8_t b[4];
    storeBE32(b, v);
    appendBytes(out, b, 4);
}
inline void appendBE64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    std::uint8_t b[8];
    storeBE64(b, v);
    appendBytes(out, b, 8);
}
inline void appendChunkHeader(std::vector<std::uint8_t>& out, std::uint32_t type, std::int64_t size) {
    appendBE32(out, type);
    appendBE64(out, static_cast<std::uint64_t>(size));
}

} // namespace detail

struct Writer::Impl {
    std::unique_ptr<Stream> owned;
    Stream* stream = nullptr;
    Info info;
    std::vector<Chunk> trailing;
    std::int64_t dataSizeOffset = 0; // position of the data chunk's size field
    std::int64_t dataStart = 0;
    std::uint32_t bytesPerFrame = 0;
    std::uint32_t blockFrames = 0;
    bool failed = false;
    std::vector<std::uint8_t> raw;

    Status begin(const WriterConfig& config);
    Status finish();

    template <typename T>
    std::int64_t write(const T* src, std::int64_t frames) {
        if (!stream || !src || frames <= 0 || failed) return 0;
        const std::uint32_t channels = info.fileChannels();
        std::int64_t total = 0;
        while (frames > 0) {
            const std::int64_t n = frames < blockFrames ? frames : static_cast<std::int64_t>(blockFrames);
            detail::encodeSamples(src, raw.data(), static_cast<std::size_t>(n) * channels, info.sampleFormat,
                                  info.bigEndian);
            const std::size_t bytes = static_cast<std::size_t>(n) * bytesPerFrame;
            const std::size_t written = stream->write(raw.data(), bytes);
            const std::int64_t done = static_cast<std::int64_t>(written / bytesPerFrame);
            total += done;
            info.frames += static_cast<std::uint64_t>(done);
            if (written != bytes) {
                failed = true;
                break;
            }
            src += static_cast<std::size_t>(n) * channels;
            frames -= n;
        }
        return total;
    }
};

Status Writer::Impl::begin(const WriterConfig& config) {
    using namespace detail;
    if (!(config.sampleRate > 0.0) || !std::isfinite(config.sampleRate)) return Status::InvalidArgument;
    const std::uint64_t channels64 = static_cast<std::uint64_t>(config.ambiChannels) + config.extraChannels;
    if (config.ambiChannels == 0 || channels64 > 65536) return Status::InvalidArgument;
    const std::uint32_t channels = static_cast<std::uint32_t>(channels64);
    for (const Chunk& chunk : config.chunks)
        if (chunk.type == kChunkDesc || chunk.type == kChunkData) return Status::InvalidArgument;

    std::uint32_t order = 0;
    Matrix adaptor = config.adaptorMatrix;
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
    info.fileFormat = adaptor.empty() ? FileFormat::Basic : FileFormat::Extended;
    info.ambiChannels = config.ambiChannels;
    info.extraChannels = config.extraChannels;
    info.fullAmbiChannels = adaptor.empty() ? config.ambiChannels : adaptor.rows();

    const std::uint32_t sampleBytes = bytesPerSample(config.sampleFormat);
    const bool isFloat = config.sampleFormat == SampleFormat::Float32 || config.sampleFormat == SampleFormat::Float64;
    bytesPerFrame = channels * sampleBytes;
    blockFrames = blockFramesFor(channels);
    raw.resize(static_cast<std::size_t>(blockFrames) * bytesPerFrame);

    std::vector<std::uint8_t> head;
    appendBytes(head, "caff", 4);
    appendBytes(head, "\0\1\0\0", 4); // version 1, flags 0

    appendChunkHeader(head, kChunkDesc, static_cast<std::int64_t>(kDescSize));
    appendBE64(head, bitCast<std::uint64_t>(config.sampleRate));
    appendBE32(head, kFormatLpcm);
    appendBE32(head, (isFloat ? kLpcmFlagFloat : 0u) | (config.bigEndian ? 0u : kLpcmFlagLittleEndian));
    appendBE32(head, bytesPerFrame);
    appendBE32(head, 1);
    appendBE32(head, channels);
    appendBE32(head, sampleBytes * 8);

    if (!adaptor.empty()) {
        const std::size_t count = static_cast<std::size_t>(adaptor.rows()) * adaptor.cols();
        appendChunkHeader(head, kChunkUuid, static_cast<std::int64_t>(kUuidSize + 8 + count * 4));
        appendBytes(head, kAmbixUuid, kUuidSize);
        appendBE32(head, adaptor.rows());
        appendBE32(head, adaptor.cols());
        for (std::size_t i = 0; i < count; ++i) appendBE32(head, bitCast<std::uint32_t>(adaptor.data()[i]));
    }

    for (const Chunk& chunk : config.chunks) {
        appendChunkHeader(head, chunk.type, static_cast<std::int64_t>(chunk.data.size()));
        appendBytes(head, chunk.data.data(), chunk.data.size());
    }

    // The data size stays -1 ("until end of file") until close() patches it,
    // so a file from an interrupted recording is still readable.
    appendBE32(head, kChunkData);
    dataSizeOffset = static_cast<std::int64_t>(head.size());
    appendBE64(head, static_cast<std::uint64_t>(std::int64_t(-1)));
    appendBE32(head, 0); // edit count
    dataStart = static_cast<std::int64_t>(head.size());

    if (stream->write(head.data(), head.size()) != head.size()) return Status::IoError;
    return Status::Ok;
}

Status Writer::Impl::finish() {
    using namespace detail;
    Status status = failed ? Status::IoError : Status::Ok;
    const std::int64_t dataBytes = static_cast<std::int64_t>(info.frames) * bytesPerFrame;
    std::uint8_t size[8];
    storeBE64(size, static_cast<std::uint64_t>(dataBytes + 4));

    // A stream that cannot seek keeps the "until end of file" size, which is
    // valid as long as nothing follows the audio.
    bool sized = false;
    if (stream->seek(dataSizeOffset)) {
        sized = stream->write(size, 8) == 8 && stream->seek(dataStart + dataBytes);
        if (!sized) status = Status::IoError;
    }
    if (!trailing.empty()) {
        if (!sized) {
            status = Status::IoError;
        } else {
            for (const Chunk& chunk : trailing) {
                std::vector<std::uint8_t> header;
                appendChunkHeader(header, chunk.type, static_cast<std::int64_t>(chunk.data.size()));
                if (stream->write(header.data(), header.size()) != header.size() ||
                    stream->write(chunk.data.data(), chunk.data.size()) != chunk.data.size()) {
                    status = Status::IoError;
                    break;
                }
            }
        }
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
    impl_->stream = &stream;
    const Status status = impl_->begin(config);
    if (status != Status::Ok) impl_.reset(new Impl);
    return status;
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

Status Writer::addTrailingChunk(Chunk chunk) {
    if (!isOpen()) return Status::NotOpen;
    if (chunk.type == detail::kChunkDesc || chunk.type == detail::kChunkData) return Status::InvalidArgument;
    impl_->trailing.push_back(std::move(chunk));
    return Status::Ok;
}

Status Writer::close() {
    if (!isOpen()) return Status::NotOpen;
    const Status status = impl_->finish();
    impl_.reset(new Impl);
    return status;
}

} // namespace ambixfile
