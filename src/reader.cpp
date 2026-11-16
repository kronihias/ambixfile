// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner

#include "internal.hpp"

namespace ambixfile {

bool wavpackAvailable() noexcept {
#ifdef AMBIXFILE_ENABLE_WAVPACK
    return true;
#else
    return false;
#endif
}

struct Reader::Impl {
    std::unique_ptr<Stream> owned;
    Stream* stream = nullptr;
    detail::ParsedCaf file;
    bool passThrough = true; // the Ambisonics channels need no matrixing
    std::uint32_t bytesPerFrame = 0;
    std::uint32_t blockFrames = 0;
    std::uint64_t pos = 0;
    std::vector<std::uint8_t> raw;
    std::vector<float> scratchF;
    std::vector<double> scratchD;
#ifdef AMBIXFILE_ENABLE_WAVPACK
    detail::WvDecoder* wv = nullptr;
    std::vector<std::int32_t> ints;
    int wvBits = 0;
    bool wvFloat = false;
#endif

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
#ifdef AMBIXFILE_ENABLE_WAVPACK
        if (wv) detail::wvCloseDecoder(wv);
#endif
    }

    Status begin(const ReaderConfig& config);

    template <typename T>
    std::int64_t readRaw(T* dst, std::int64_t frames) {
        if (!stream || !dst || frames <= 0) return 0;
        const Info& info = file.info;
        const std::uint32_t channels = info.fileChannels();
        std::int64_t total = 0;
        while (frames > 0 && pos < info.frames) {
            std::uint64_t want = static_cast<std::uint64_t>(frames);
            if (want > blockFrames) want = blockFrames;
            if (want > info.frames - pos) want = info.frames - pos;
            std::uint64_t got = 0;
#ifdef AMBIXFILE_ENABLE_WAVPACK
            if (wv) {
                got = detail::wvUnpack(wv, ints.data(), static_cast<std::uint32_t>(want));
                const std::size_t count = static_cast<std::size_t>(got) * channels;
                if (wvFloat) {
                    for (std::size_t i = 0; i < count; ++i) dst[i] = static_cast<T>(detail::bitCast<float>(ints[i]));
                } else {
                    const double scale = 1.0 / static_cast<double>(std::uint64_t(1) << (wvBits - 1));
                    for (std::size_t i = 0; i < count; ++i) dst[i] = static_cast<T>(ints[i] * scale);
                }
            } else
#endif
            {
                const std::size_t bytes = stream->read(raw.data(), static_cast<std::size_t>(want) * bytesPerFrame);
                got = bytes / bytesPerFrame;
                detail::decodeSamples(raw.data(), dst, static_cast<std::size_t>(got) * channels, info.sampleFormat,
                                      info.bigEndian);
                if (got < want) // short read: realign the stream to a frame boundary
                    stream->seek(file.dataStart + static_cast<std::int64_t>(pos + got) * bytesPerFrame);
            }
            dst += got * channels;
            pos += got;
            total += static_cast<std::int64_t>(got);
            frames -= static_cast<std::int64_t>(got);
            if (got < want) break;
        }
        return total;
    }

    template <typename T>
    std::int64_t readAmbi(T* ambi, T* extra, std::int64_t frames, std::vector<T>& scratch) {
        const Info& info = file.info;
        const std::uint32_t channels = info.fileChannels();
        const std::uint32_t inAmbi = info.ambiChannels;
        const std::uint32_t outAmbi = info.fullAmbiChannels;
        const std::uint32_t numExtra = info.extraChannels;
        const float* matrix = file.adaptor.data();
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

Status Reader::Impl::begin(const ReaderConfig& config) {
    Stream& s = *stream;
    std::uint8_t magic[4] = {};
    if (!s.seek(0)) return Status::IoError;
    const bool isWavPack = s.read(magic, 4) == 4 && std::memcmp(magic, "wvpk", 4) == 0;

    if (isWavPack) {
#ifdef AMBIXFILE_ENABLE_WAVPACK
        Status status = Status::Ok;
        wv = detail::wvOpenDecoder(s, config.wavpackThreads, file, wvBits, wvFloat, status);
        if (!wv) return status;
#else
        (void)config;
        return Status::UnsupportedFormat;
#endif
    } else {
        const Status status = detail::parseCaf(s, false, file);
        if (status != Status::Ok) return status;
        if (!s.seek(file.dataStart)) return Status::IoError;
    }

    const Info& info = file.info;
    const std::uint32_t channels = info.fileChannels();
    passThrough = file.adaptor.empty() || file.adaptor.isIdentity(0.0f);
    bytesPerFrame = channels * detail::bytesPerSample(info.sampleFormat);
    blockFrames = detail::blockFramesFor(channels);
    scratchF.resize(static_cast<std::size_t>(blockFrames) * channels);
    scratchD.resize(static_cast<std::size_t>(blockFrames) * channels);
#ifdef AMBIXFILE_ENABLE_WAVPACK
    if (wv) ints.resize(static_cast<std::size_t>(blockFrames) * channels);
    else
#endif
        raw.resize(static_cast<std::size_t>(blockFrames) * bytesPerFrame);
    pos = 0;
    return Status::Ok;
}

Reader::Reader() : impl_(new Impl) {}
Reader::~Reader() = default;
Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;

Status Reader::open(const char* utf8Path, const ReaderConfig& config) {
    std::unique_ptr<Stream> stream = openFileStream(utf8Path, false);
    if (!stream) return Status::IoError;
    return open(std::move(stream), config);
}

Status Reader::open(std::unique_ptr<Stream> stream, const ReaderConfig& config) {
    if (!stream) return Status::InvalidArgument;
    Stream& ref = *stream;
    const Status status = open(ref, config);
    if (status == Status::Ok) impl_->owned = std::move(stream);
    return status;
}

Status Reader::open(Stream& stream, const ReaderConfig& config) {
    impl_.reset(new Impl);
    impl_->stream = &stream;
    const Status status = impl_->begin(config);
    if (status != Status::Ok) impl_.reset(new Impl);
    return status;
}

void Reader::close() { impl_.reset(new Impl); }
bool Reader::isOpen() const noexcept { return impl_ && impl_->stream; }

const Info& Reader::info() const noexcept {
    static const Info none;
    return impl_ ? impl_->file.info : none;
}
const Matrix& Reader::adaptorMatrix() const noexcept {
    static const Matrix none;
    return impl_ ? impl_->file.adaptor : none;
}
const std::vector<Chunk>& Reader::chunks() const noexcept {
    static const std::vector<Chunk> none;
    return impl_ ? impl_->file.chunks : none;
}
const std::vector<Marker>& Reader::markers() const noexcept {
    static const std::vector<Marker> none;
    return impl_ ? impl_->file.markers : none;
}
const std::vector<Region>& Reader::regions() const noexcept {
    static const std::vector<Region> none;
    return impl_ ? impl_->file.regions : none;
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
    Impl& impl = *impl_;
    if (frame > impl.file.info.frames) frame = impl.file.info.frames;
#ifdef AMBIXFILE_ENABLE_WAVPACK
    if (impl.wv) {
        // WavPack cannot seek to the very end; reading there returns nothing anyway.
        if (frame < impl.file.info.frames && !detail::wvSeek(impl.wv, frame)) return false;
        impl.pos = frame;
        return true;
    }
#endif
    if (!impl.stream->seek(impl.file.dataStart + static_cast<std::int64_t>(frame) * impl.bytesPerFrame)) return false;
    impl.pos = frame;
    return true;
}

std::uint64_t Reader::tell() const noexcept { return impl_ ? impl_->pos : 0; }

} // namespace ambixfile
