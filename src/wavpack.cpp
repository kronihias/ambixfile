// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner
//
// Optional WavPack backend. An ambiX WavPack file is a WavPack stream whose
// wrapper holds the CAF header the uncompressed file would have had (file
// header, 'desc', the ambiX 'uuid' chunk, markers, ..., and the data chunk
// header), so all ambiX metadata survives compression.

#include "internal.hpp"

#ifdef AMBIXFILE_ENABLE_WAVPACK

#if defined(__has_include)
#if __has_include(<wavpack/wavpack.h>)
#include <wavpack/wavpack.h>
#else
#include <wavpack.h>
#endif
#else
#include <wavpack.h>
#endif

#include <cstdio>

namespace ambixfile {
namespace detail {

// ---- stream callbacks for decoding ----

inline std::int32_t wvReadBytes(void* id, void* data, std::int32_t count) {
    return count > 0 ? static_cast<std::int32_t>(static_cast<Stream*>(id)->read(data, static_cast<std::size_t>(count))) : 0;
}
inline std::int32_t wvWriteBytes(void*, void*, std::int32_t) { return 0; }
inline std::int64_t wvGetPos(void* id) { return static_cast<Stream*>(id)->tell(); }
inline int wvSetPosAbs(void* id, std::int64_t pos) { return static_cast<Stream*>(id)->seek(pos) ? 0 : -1; }
inline int wvSetPosRel(void* id, std::int64_t delta, int mode) {
    Stream* s = static_cast<Stream*>(id);
    std::int64_t base = 0;
    if (mode == SEEK_CUR) base = s->tell();
    else if (mode == SEEK_END) base = s->size();
    if (base < 0) return -1;
    return s->seek(base + delta) ? 0 : -1;
}
inline int wvPushBackByte(void* id, int c) {
    Stream* s = static_cast<Stream*>(id);
    const std::int64_t pos = s->tell();
    return pos > 0 && s->seek(pos - 1) ? c : EOF;
}
inline std::int64_t wvGetLength(void* id) {
    const std::int64_t size = static_cast<Stream*>(id)->size();
    return size < 0 ? 0 : size;
}
inline int wvCanSeek(void* id) { return static_cast<Stream*>(id)->size() >= 0 ? 1 : 0; }
inline int wvTruncateHere(void*) { return -1; }
inline int wvCloseStream(void*) { return 0; }

inline int wvThreadFlags(int threads) noexcept {
#ifdef OPEN_THREADS_SHFT
    if (threads < 0) threads = 0;
    if (threads > 15) threads = 15;
    return threads << OPEN_THREADS_SHFT;
#else
    (void)threads;
    return 0;
#endif
}

struct WvDecoder {
    WavpackStreamReader64 callbacks;
    WavpackContext* context = nullptr;
};

WvDecoder* wvOpenDecoder(Stream& stream, int threads, ParsedCaf& out, int& bits, bool& isFloat, Status& status) {
    std::unique_ptr<WvDecoder> decoder(new WvDecoder);
    decoder->callbacks = WavpackStreamReader64{wvReadBytes, wvWriteBytes, wvGetPos,    wvSetPosAbs,    wvSetPosRel,
                                               wvPushBackByte, wvGetLength, wvCanSeek, wvTruncateHere, wvCloseStream};
    if (!stream.seek(0)) {
        status = Status::IoError;
        return nullptr;
    }
    char error[81] = {};
    // OPEN_ALT_TYPES is needed to be handed non-WAV wrappers such as CAF.
    WavpackContext* wpc = WavpackOpenFileInputEx64(&decoder->callbacks, &stream, nullptr, error,
                                                   OPEN_WRAPPER | OPEN_NORMALIZE | OPEN_ALT_TYPES | wvThreadFlags(threads), 0);
    if (!wpc) {
        status = Status::InvalidFile;
        return nullptr;
    }
    decoder->context = wpc;
    const auto fail = [&](Status s) -> WvDecoder* {
        WavpackCloseFile(wpc);
        status = s;
        return nullptr;
    };

    // Metadata: the wrapper is a CAF header; chunks stored after the audio
    // arrive as trailing wrapper data.
    WavpackSeekTrailingWrapper(wpc);
    const std::uint32_t wrapperSize = WavpackGetWrapperBytes(wpc);
    const std::uint8_t* wrapper = WavpackGetWrapperData(wpc);
    ParsedCaf parsed;
    Status parseStatus = Status::NotCaf;
    if (wrapper && wrapperSize) {
        MemoryStream memory(std::vector<std::uint8_t>(wrapper, wrapper + wrapperSize));
        parseStatus = parseCaf(memory, true, parsed);
    }
    WavpackFreeWrapper(wpc);
    if (parseStatus == Status::InvalidAdaptorMatrix) return fail(parseStatus);
    if (parseStatus != Status::Ok) parsed = ParsedCaf(); // not an ambiX wrapper: plain audio

    // The WavPack stream itself is authoritative for the audio format.
    const int channels = WavpackGetNumChannels(wpc);
    const int bytes = WavpackGetBytesPerSample(wpc);
    const int mode = WavpackGetMode(wpc);
    const std::int64_t frames = WavpackGetNumSamples64(wpc);
    if (channels <= 0 || bytes < 1 || bytes > 4) return fail(Status::UnsupportedFormat);
#ifdef QMODE_DSD_AUDIO
    if (WavpackGetQualifyMode(wpc) & QMODE_DSD_AUDIO) return fail(Status::UnsupportedFormat);
#endif

    isFloat = (mode & MODE_FLOAT) != 0;
    bits = bytes * 8;
    parsed.channels = static_cast<std::uint32_t>(channels);
    parsed.info.sampleRate = static_cast<double>(WavpackGetSampleRate(wpc));
    parsed.info.sampleFormat = isFloat ? SampleFormat::Float32
                                       : bytes <= 2 ? SampleFormat::Int16 : bytes == 3 ? SampleFormat::Int24 : SampleFormat::Int32;
    parsed.info.frames = frames > 0 ? static_cast<std::uint64_t>(frames) : 0;
    parsed.info.container = Container::WavPack;
    parsed.info.lossy = (mode & MODE_HYBRID) != 0 && (mode & MODE_LOSSLESS) == 0;
    const Status layout = applyLayout(parsed.info, parsed.adaptor, parsed.channels);
    if (layout != Status::Ok) return fail(layout);

    out = std::move(parsed);
    status = Status::Ok;
    return decoder.release();
}

void wvCloseDecoder(WvDecoder* decoder) noexcept {
    if (!decoder) return;
    if (decoder->context) WavpackCloseFile(decoder->context);
    delete decoder;
}

std::uint32_t wvUnpack(WvDecoder* decoder, std::int32_t* buffer, std::uint32_t frames) {
    return WavpackUnpackSamples(decoder->context, buffer, frames);
}

bool wvSeek(WvDecoder* decoder, std::uint64_t frame) {
    return WavpackSeekSample64(decoder->context, static_cast<std::int64_t>(frame)) != 0;
}

// ---- encoding ----

struct WvEncoder {
    Stream* stream = nullptr;
    WavpackContext* context = nullptr;
    std::int64_t origin = 0;
    // The first block is kept so its sample count can be filled in at the end.
    std::vector<std::uint8_t> firstBlock;
    bool haveFirstBlock = false;
    bool failed = false;
};

inline int wvBlockOutput(void* id, void* data, std::int32_t count) {
    WvEncoder* encoder = static_cast<WvEncoder*>(id);
    if (count <= 0) return 1;
    if (!encoder->haveFirstBlock) {
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
        encoder->firstBlock.assign(bytes, bytes + count);
        encoder->haveFirstBlock = true;
    }
    if (encoder->stream->write(data, static_cast<std::size_t>(count)) != static_cast<std::size_t>(count)) {
        encoder->failed = true;
        return 0;
    }
    return 1;
}

WvEncoder* wvOpenEncoder(Stream& stream, const Info& info, float hybridBits, int threads,
                         const std::vector<std::uint8_t>& wrapper, Status& status) {
    std::unique_ptr<WvEncoder> encoder(new WvEncoder);
    encoder->stream = &stream;
    const std::int64_t origin = stream.tell();
    encoder->origin = origin < 0 ? 0 : origin;

    WavpackContext* wpc = WavpackOpenFileOutput(wvBlockOutput, encoder.get(), nullptr);
    status = Status::IoError;
    if (!wpc) return nullptr;
    encoder->context = wpc;

    char extension[] = "caf";
    WavpackSetFileInformation(wpc, extension, WP_FORMAT_CAF);

    const int bytes = static_cast<int>(bytesPerSample(info.sampleFormat));
    WavpackConfig config;
    std::memset(&config, 0, sizeof(config));
    config.bytes_per_sample = bytes;
    config.bits_per_sample = bytes * 8;
    config.num_channels = static_cast<int>(info.fileChannels());
    config.sample_rate = static_cast<std::int32_t>(info.sampleRate);
    config.channel_mask = 0; // no speaker layout: these are Ambisonics channels
    config.float_norm_exp = info.sampleFormat == SampleFormat::Float32 ? 127 : 0;
    config.qmode = info.bigEndian ? QMODE_BIG_ENDIAN : 0;
#ifdef OPEN_THREADS_SHFT
    config.worker_threads = threads < 0 ? 0 : threads > 15 ? 15 : threads;
#else
    (void)threads;
#endif
    if (hybridBits > 0.0f) {
        config.flags |= CONFIG_HYBRID_FLAG;
        config.bitrate = hybridBits;
    }

    std::vector<std::uint8_t> wrapperCopy = wrapper; // the API takes a non-const pointer
    if (!WavpackSetConfiguration64(wpc, &config, -1, nullptr) ||
        !WavpackAddWrapper(wpc, wrapperCopy.data(), static_cast<std::uint32_t>(wrapperCopy.size())) ||
        !WavpackPackInit(wpc)) {
        WavpackCloseFile(wpc);
        return nullptr;
    }
    status = Status::Ok;
    return encoder.release();
}

bool wvPack(WvEncoder* encoder, std::int32_t* buffer, std::uint32_t frames) {
    return encoder && !encoder->failed && WavpackPackSamples(encoder->context, buffer, frames) != 0;
}

Status wvCloseEncoder(WvEncoder* raw, const std::vector<std::uint8_t>& trailer) {
    std::unique_ptr<WvEncoder> encoder(raw);
    if (!encoder) return Status::NotOpen;
    WavpackContext* wpc = encoder->context;
    bool ok = !encoder->failed && WavpackFlushSamples(wpc) != 0;
    if (ok && !trailer.empty()) {
        std::vector<std::uint8_t> copy = trailer;
        ok = WavpackAddWrapper(wpc, copy.data(), static_cast<std::uint32_t>(copy.size())) != 0 &&
             WavpackFlushSamples(wpc) != 0;
    }
    // Fill the total sample count into the first block. Without it (when the
    // stream cannot seek) the file is still valid; readers then find the
    // length by scanning.
    if (ok && encoder->haveFirstBlock) {
        Stream& stream = *encoder->stream;
        const std::int64_t end = stream.tell();
        if (end >= 0 && stream.seek(encoder->origin)) {
            WavpackUpdateNumSamples(wpc, encoder->firstBlock.data());
            ok = stream.write(encoder->firstBlock.data(), encoder->firstBlock.size()) == encoder->firstBlock.size() &&
                 stream.seek(end);
        }
    }
    WavpackCloseFile(wpc);
    return ok && !encoder->failed ? Status::Ok : Status::IoError;
}

} // namespace detail
} // namespace ambixfile

#endif // AMBIXFILE_ENABLE_WAVPACK
