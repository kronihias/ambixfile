// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner

#include "internal.hpp"

#include <limits>

namespace ambixfile {
namespace detail {

// Chunks larger than this are skipped instead of being loaded into memory.
constexpr std::int64_t kMaxStoredChunkSize = std::int64_t(1) << 28;
constexpr std::uint32_t kMaxChannels = 65536;

// Payload after the UUID: rows (uint32), cols (uint32), rows*cols float32 in
// row-major order. CAF chunk data is big-endian; files with little-endian
// payloads exist, so that order is accepted when it is the one that fits.
inline bool parseAdaptorPayload(const std::uint8_t* p, std::size_t size, Matrix& out) {
    if (size < 8) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool be = attempt == 0;
        const std::uint32_t rows = be ? loadBE32(p) : loadLE32(p);
        const std::uint32_t cols = be ? loadBE32(p + 4) : loadLE32(p + 4);
        if (rows == 0 || cols == 0) continue;
        if (static_cast<std::uint64_t>(rows) * cols * 4 + 8 != size) continue;
        Matrix m(rows, cols);
        float* values = m.data();
        const std::size_t count = static_cast<std::size_t>(rows) * cols;
        for (std::size_t i = 0; i < count; ++i)
            values[i] = bitCast<float>(be ? loadBE32(p + 8 + i * 4) : loadLE32(p + 8 + i * 4));
        out = std::move(m);
        return true;
    }
    return false;
}

} // namespace detail

struct Reader::Impl {
    std::unique_ptr<Stream> owned;
    Stream* stream = nullptr;
    Info info;
    Matrix adaptor;
    std::vector<Chunk> chunks;
    bool passThrough = true; // the Ambisonics channels need no matrixing
    std::int64_t dataStart = 0;
    std::uint32_t bytesPerFrame = 0;
    std::uint32_t blockFrames = 0;
    std::uint64_t pos = 0;
    std::vector<std::uint8_t> raw;
    std::vector<float> scratchF;
    std::vector<double> scratchD;

    Status parse();

    template <typename T>
    std::int64_t readRaw(T* dst, std::int64_t frames) {
        if (!stream || !dst || frames <= 0) return 0;
        const std::uint32_t channels = info.fileChannels();
        std::int64_t total = 0;
        while (frames > 0 && pos < info.frames) {
            std::uint64_t want = static_cast<std::uint64_t>(frames);
            if (want > blockFrames) want = blockFrames;
            if (want > info.frames - pos) want = info.frames - pos;
            const std::size_t bytes = stream->read(raw.data(), static_cast<std::size_t>(want) * bytesPerFrame);
            const std::uint64_t got = bytes / bytesPerFrame;
            detail::decodeSamples(raw.data(), dst, static_cast<std::size_t>(got) * channels, info.sampleFormat,
                                  info.bigEndian);
            dst += got * channels;
            pos += got;
            total += static_cast<std::int64_t>(got);
            frames -= static_cast<std::int64_t>(got);
            if (got < want) {
                // Short read: realign the stream to a frame boundary.
                stream->seek(dataStart + static_cast<std::int64_t>(pos) * bytesPerFrame);
                break;
            }
        }
        return total;
    }

    template <typename T>
    std::int64_t readAmbi(T* ambi, T* extra, std::int64_t frames, std::vector<T>& scratch) {
        const std::uint32_t channels = info.fileChannels();
        const std::uint32_t inAmbi = info.ambiChannels;
        const std::uint32_t outAmbi = info.fullAmbiChannels;
        const std::uint32_t numExtra = info.extraChannels;
        const float* matrix = adaptor.data();
        std::int64_t total = 0;
        while (frames > 0) {
            const std::int64_t want = frames < blockFrames ? frames : static_cast<std::int64_t>(blockFrames);
            const std::int64_t got = readRaw(scratch.data(), want);
            for (std::int64_t f = 0; f < got; ++f) {
                const T* in = scratch.data() + static_cast<std::size_t>(f) * channels;
                if (ambi && outAmbi) {
                    T* out = ambi + static_cast<std::size_t>(total + f) * outAmbi;
                    if (passThrough) {
                        for (std::uint32_t c = 0; c < outAmbi; ++c) out[c] = in[c];
                    } else {
                        for (std::uint32_t r = 0; r < outAmbi; ++r) {
                            const float* row = matrix + static_cast<std::size_t>(r) * inAmbi;
                            T sum = 0;
                            for (std::uint32_t c = 0; c < inAmbi; ++c) sum += static_cast<T>(row[c]) * in[c];
                            out[r] = sum;
                        }
                    }
                }
                if (extra && numExtra) {
                    T* out = extra + static_cast<std::size_t>(total + f) * numExtra;
                    for (std::uint32_t c = 0; c < numExtra; ++c) out[c] = in[inAmbi + c];
                }
            }
            total += got;
            frames -= got;
            if (got < want) break;
        }
        return total;
    }
};

Status Reader::Impl::parse() {
    using namespace detail;
    Stream& s = *stream;
    const std::int64_t streamSize = s.size();
    std::uint8_t header[kDescSize];

    if (!s.seek(0)) return Status::IoError;
    if (s.read(header, kFileHeaderSize) != kFileHeaderSize || std::memcmp(header, "caff", 4) != 0)
        return Status::NotCaf;
    if (loadBE16(header + 4) != 1) return Status::UnsupportedFormat;

    bool haveDesc = false, haveData = false, haveAdaptor = false, badAdaptor = false;
    std::uint32_t channels = 0;
    std::int64_t dataBytes = 0;
    std::int64_t offset = static_cast<std::int64_t>(kFileHeaderSize);

    for (;;) {
        if (streamSize >= 0 && offset + static_cast<std::int64_t>(kChunkHeaderSize) > streamSize) break;
        if (!s.seek(offset)) {
            if (haveData) break;
            return Status::IoError;
        }
        const std::size_t got = s.read(header, kChunkHeaderSize);
        if (got == 0) break;
        if (got != kChunkHeaderSize) {
            if (haveData) break;
            return Status::InvalidFile;
        }
        const std::uint32_t type = loadBE32(header);
        const std::int64_t size = static_cast<std::int64_t>(loadBE64(header + 4));
        const std::int64_t payload = offset + static_cast<std::int64_t>(kChunkHeaderSize);

        if (type == kChunkData) {
            if (!haveDesc || haveData) return Status::InvalidFile;
            // The payload starts with a 4-byte edit count. A size of -1 means
            // the audio runs to the end of the file.
            if (size == -1 || (streamSize >= 0 && size > streamSize - payload)) {
                if (streamSize < 0) return Status::UnsupportedFormat;
                dataBytes = streamSize - payload - 4;
            } else {
                dataBytes = size - 4;
            }
            if (size < -1 || dataBytes < 0) return Status::InvalidFile;
            dataStart = payload + 4;
            haveData = true;
            if (size == -1) break;
            offset = payload + size;
            continue;
        }

        if (size < 0 || size > std::numeric_limits<std::int64_t>::max() - payload) {
            if (haveData) break;
            return Status::InvalidFile;
        }
        if (streamSize >= 0 && size > streamSize - payload) {
            if (haveData) break; // truncated trailing chunk: keep the audio
            return Status::InvalidFile;
        }

        if (type == kChunkDesc) {
            if (haveDesc || size < static_cast<std::int64_t>(kDescSize)) return Status::InvalidFile;
            if (s.read(header, kDescSize) != kDescSize) return Status::InvalidFile;
            const double sampleRate = bitCast<double>(loadBE64(header));
            const std::uint32_t formatId = loadBE32(header + 8);
            const std::uint32_t flags = loadBE32(header + 12);
            const std::uint32_t bytesPerPacket = loadBE32(header + 16);
            const std::uint32_t framesPerPacket = loadBE32(header + 20);
            channels = loadBE32(header + 24);
            const std::uint32_t bits = loadBE32(header + 28);

            if (formatId != kFormatLpcm) return Status::UnsupportedFormat;
            if (!(sampleRate > 0.0) || !std::isfinite(sampleRate)) return Status::InvalidFile;
            if (channels == 0 || channels > kMaxChannels) return Status::UnsupportedFormat;
            const bool isFloat = (flags & kLpcmFlagFloat) != 0;
            if (isFloat && bits == 32) info.sampleFormat = SampleFormat::Float32;
            else if (isFloat && bits == 64) info.sampleFormat = SampleFormat::Float64;
            else if (!isFloat && bits == 16) info.sampleFormat = SampleFormat::Int16;
            else if (!isFloat && bits == 24) info.sampleFormat = SampleFormat::Int24;
            else if (!isFloat && bits == 32) info.sampleFormat = SampleFormat::Int32;
            else return Status::UnsupportedFormat;
            if (framesPerPacket != 1 || bytesPerPacket != channels * (bits / 8)) return Status::UnsupportedFormat;
            info.sampleRate = sampleRate;
            info.bigEndian = (flags & kLpcmFlagLittleEndian) == 0;
            haveDesc = true;
        } else if (type != kChunkFree && size <= kMaxStoredChunkSize) {
            Chunk chunk;
            chunk.type = type;
            chunk.data.resize(static_cast<std::size_t>(size));
            if (s.read(chunk.data.data(), chunk.data.size()) != chunk.data.size()) {
                if (haveData) break;
                return Status::InvalidFile;
            }
            const bool isAmbix = type == kChunkUuid && chunk.data.size() >= kUuidSize &&
                                 (std::memcmp(chunk.data.data(), kAmbixUuid, kUuidSize) == 0 ||
                                  std::memcmp(chunk.data.data(), kAmbixUuidLegacy, kUuidSize) == 0);
            if (!isAmbix) {
                chunks.push_back(std::move(chunk));
            } else if (!haveAdaptor) {
                haveAdaptor = true;
                badAdaptor = !parseAdaptorPayload(chunk.data.data() + kUuidSize, chunk.data.size() - kUuidSize, adaptor);
            }
        }
        offset = payload + size;
    }

    if (!haveDesc || !haveData) return Status::InvalidFile;
    if (badAdaptor) return Status::InvalidAdaptorMatrix;

    bytesPerFrame = channels * bytesPerSample(info.sampleFormat);
    info.frames = static_cast<std::uint64_t>(dataBytes) / bytesPerFrame;

    std::uint32_t order = 0;
    if (haveAdaptor) {
        if (adaptor.cols() > channels || !channelsToOrder(adaptor.rows(), order)) return Status::InvalidAdaptorMatrix;
        info.fileFormat = FileFormat::Extended;
        info.ambiChannels = adaptor.cols();
        info.fullAmbiChannels = adaptor.rows();
        passThrough = adaptor.isIdentity(0.0f);
    } else if (channelsToOrder(channels, order)) {
        info.fileFormat = FileFormat::Basic;
        info.ambiChannels = info.fullAmbiChannels = channels;
    } else {
        info.fileFormat = FileFormat::None;
        info.ambiChannels = info.fullAmbiChannels = 0;
    }
    info.extraChannels = channels - info.ambiChannels;

    blockFrames = blockFramesFor(channels);
    raw.resize(static_cast<std::size_t>(blockFrames) * bytesPerFrame);
    scratchF.resize(static_cast<std::size_t>(blockFrames) * channels);
    scratchD.resize(static_cast<std::size_t>(blockFrames) * channels);

    pos = 0;
    return s.seek(dataStart) ? Status::Ok : Status::IoError;
}

Reader::Reader() : impl_(new Impl) {}
Reader::~Reader() = default;
Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;

Status Reader::open(const char* utf8Path) {
    std::unique_ptr<Stream> stream = openFileStream(utf8Path, false);
    if (!stream) return Status::IoError;
    return open(std::move(stream));
}

Status Reader::open(std::unique_ptr<Stream> stream) {
    if (!stream) return Status::InvalidArgument;
    Stream& ref = *stream;
    const Status status = open(ref);
    if (status == Status::Ok) impl_->owned = std::move(stream);
    else close(); // drop the reference before the stream is destroyed
    return status;
}

Status Reader::open(Stream& stream) {
    impl_.reset(new Impl);
    impl_->stream = &stream;
    const Status status = impl_->parse();
    if (status != Status::Ok) impl_.reset(new Impl);
    return status;
}

void Reader::close() { impl_.reset(new Impl); }
bool Reader::isOpen() const noexcept { return impl_ && impl_->stream; }

const Info& Reader::info() const noexcept {
    static const Info none;
    return impl_ ? impl_->info : none;
}
const Matrix& Reader::adaptorMatrix() const noexcept {
    static const Matrix none;
    return impl_ ? impl_->adaptor : none;
}
const std::vector<Chunk>& Reader::chunks() const noexcept {
    static const std::vector<Chunk> none;
    return impl_ ? impl_->chunks : none;
}

std::int64_t Reader::read(float* interleaved, std::int64_t frames) {
    return impl_ ? impl_->readRaw(interleaved, frames) : 0;
}
std::int64_t Reader::read(double* interleaved, std::int64_t frames) {
    return impl_ ? impl_->readRaw(interleaved, frames) : 0;
}
std::int64_t Reader::readAmbisonics(float* ambi, float* extra, std::int64_t frames) {
    return isOpen() ? impl_->readAmbi(ambi, extra, frames, impl_->scratchF) : 0;
}
std::int64_t Reader::readAmbisonics(double* ambi, double* extra, std::int64_t frames) {
    return isOpen() ? impl_->readAmbi(ambi, extra, frames, impl_->scratchD) : 0;
}

bool Reader::seek(std::uint64_t frame) {
    if (!isOpen()) return false;
    if (frame > impl_->info.frames) frame = impl_->info.frames;
    if (!impl_->stream->seek(impl_->dataStart + static_cast<std::int64_t>(frame) * impl_->bytesPerFrame)) return false;
    impl_->pos = frame;
    return true;
}

std::uint64_t Reader::tell() const noexcept { return impl_ ? impl_->pos : 0; }

} // namespace ambixfile
