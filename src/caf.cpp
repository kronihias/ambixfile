// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner
//
// CAF header parsing and building, including the ambiX adaptor matrix chunk
// and the marker, region and string chunks.

#include "internal.hpp"

#include <limits>

namespace ambixfile {
namespace detail {

// Chunks larger than this are skipped instead of being loaded into memory.
constexpr std::int64_t kMaxStoredChunkSize = std::int64_t(1) << 28;
constexpr std::uint32_t kMaxChannels = 65536;

// CAF chunk payloads are big-endian, but files exist whose metadata chunks
// were written in little-endian order. Each parser below is therefore run in
// big-endian order first and in little-endian order if that does not fit.
inline std::uint32_t read32(const std::uint8_t* p, bool littleEndian) noexcept {
    return littleEndian ? loadLE32(p) : loadBE32(p);
}
inline std::uint64_t read64(const std::uint8_t* p, bool littleEndian) noexcept {
    return littleEndian ? loadLE64(p) : loadBE64(p);
}

// Payload after the UUID: rows (uint32), cols (uint32), rows*cols float32 in
// row-major order.
inline bool parseAdaptorPayload(const std::uint8_t* p, std::size_t size, Matrix& out) {
    if (size < 8) return false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool le = attempt == 1;
        const std::uint32_t rows = read32(p, le);
        const std::uint32_t cols = read32(p + 4, le);
        if (rows == 0 || cols == 0) continue;
        if (static_cast<std::uint64_t>(rows) * cols * 4 + 8 != size) continue;
        Matrix m(rows, cols);
        float* values = m.data();
        const std::size_t count = static_cast<std::size_t>(rows) * cols;
        for (std::size_t i = 0; i < count; ++i) values[i] = bitCast<float>(read32(p + 8 + i * 4, le));
        out = std::move(m);
        return true;
    }
    return false;
}

struct RawMarker {
    std::uint32_t type = 0;
    double position = 0.0;
    std::uint32_t id = 0;
};
struct RawRegion {
    std::vector<RawMarker> markers;
};
struct RawString {
    std::uint32_t id = 0;
    std::string text;
};

// CAFMarker: type (4), frame position (float64), marker ID (4), SMPTE time
// (8), channel (4).
inline RawMarker readMarker(const std::uint8_t* p, bool le) noexcept {
    RawMarker m;
    m.type = read32(p, le);
    m.position = bitCast<double>(read64(p + 4, le));
    m.id = read32(p + 12, le);
    return m;
}

// 'mark': SMPTE time type (4), marker count (4), markers.
inline bool parseMarkChunk(const std::vector<std::uint8_t>& data, bool le, std::vector<RawMarker>& out) {
    if (data.size() < 8) return false;
    const std::uint64_t count = read32(data.data() + 4, le);
    if (8 + count * kMarkerSize > data.size()) return false;
    for (std::uint64_t i = 0; i < count; ++i) out.push_back(readMarker(data.data() + 8 + i * kMarkerSize, le));
    return true;
}

// 'regn': SMPTE time type (4), region count (4), then per region: region ID
// (4), flags (4), marker count (4), markers.
inline bool parseRegnChunk(const std::vector<std::uint8_t>& data, bool le, std::vector<RawRegion>& out) {
    if (data.size() < 8) return false;
    const std::uint64_t count = read32(data.data() + 4, le);
    if (8 + count * 12 > data.size()) return false;
    std::vector<RawRegion> regions;
    std::size_t offset = 8;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (offset + 12 > data.size()) return false;
        const std::uint64_t markers = read32(data.data() + offset + 8, le);
        offset += 12;
        if (markers * kMarkerSize > data.size() - offset) return false;
        RawRegion region;
        for (std::uint64_t k = 0; k < markers; ++k, offset += kMarkerSize)
            region.markers.push_back(readMarker(data.data() + offset, le));
        regions.push_back(std::move(region));
    }
    for (RawRegion& region : regions) out.push_back(std::move(region));
    return true;
}

// 'strg': entry count (4), then per entry: string ID (4), byte offset into
// the string data (int64); then the NUL-terminated UTF-8 strings.
inline bool parseStrgChunk(const std::vector<std::uint8_t>& data, bool le, std::vector<RawString>& out) {
    if (data.size() < 4) return false;
    const std::uint64_t count = read32(data.data(), le);
    if (4 + count * 12 > data.size()) return false;
    const std::size_t base = 4 + static_cast<std::size_t>(count) * 12;
    const std::size_t blob = data.size() - base;
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint8_t* entry = data.data() + 4 + i * 12;
        const std::uint64_t offset = read64(entry + 4, le);
        if (offset >= blob) continue;
        RawString s;
        s.id = read32(entry, le);
        const char* text = reinterpret_cast<const char*>(data.data() + base + offset);
        std::size_t length = 0;
        while (offset + length < blob && text[length] != '\0') ++length;
        s.text.assign(text, length);
        out.push_back(std::move(s));
    }
    return true;
}

template <typename Parser, typename Output>
inline bool parseEitherOrder(Parser parser, const std::vector<std::uint8_t>& data, Output& out) {
    return parser(data, false, out) || parser(data, true, out);
}

inline std::string findString(const std::vector<RawString>& strings, std::uint32_t id) {
    for (const RawString& s : strings)
        if (s.id == id) return s.text;
    return std::string();
}

Status applyLayout(Info& info, const Matrix& adaptor, std::uint32_t channels) {
    std::uint32_t order = 0;
    if (!adaptor.empty()) {
        if (adaptor.cols() > channels || !channelsToOrder(adaptor.rows(), order)) return Status::InvalidAdaptorMatrix;
        info.fileFormat = FileFormat::Extended;
        info.ambiChannels = adaptor.cols();
        info.fullAmbiChannels = adaptor.rows();
    } else if (channelsToOrder(channels, order)) {
        info.fileFormat = FileFormat::Basic;
        info.ambiChannels = info.fullAmbiChannels = channels;
    } else {
        info.fileFormat = FileFormat::None;
        info.ambiChannels = info.fullAmbiChannels = 0;
    }
    info.extraChannels = channels - info.ambiChannels;
    return Status::Ok;
}

Status parseCaf(Stream& s, bool wrapper, ParsedCaf& out) {
    const std::int64_t streamSize = s.size();
    std::uint8_t header[kDescSize];

    if (!s.seek(0)) return Status::IoError;
    if (s.read(header, kFileHeaderSize) != kFileHeaderSize || std::memcmp(header, "caff", 4) != 0)
        return Status::NotCaf;
    if (loadBE16(header + 4) != 1) return Status::UnsupportedFormat;

    bool haveDesc = false, haveData = false, haveAdaptor = false, badAdaptor = false;
    std::vector<RawMarker> rawMarkers;
    std::vector<RawRegion> rawRegions;
    std::vector<RawString> rawStrings;
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
            haveData = true;
            out.dataStart = payload + 4; // the payload starts with a 4-byte edit count
            if (wrapper) {
                offset = payload + 4;
                continue;
            }
            // A size of -1 means the audio runs to the end of the file.
            if (size == -1 || (streamSize >= 0 && size > streamSize - payload)) {
                if (streamSize < 0) return Status::UnsupportedFormat;
                dataBytes = streamSize - payload - 4;
            } else {
                dataBytes = size - 4;
            }
            if (size < -1 || dataBytes < 0) return Status::InvalidFile;
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
            const std::uint32_t channels = loadBE32(header + 24);
            const std::uint32_t bits = loadBE32(header + 28);

            if (formatId != kFormatLpcm) return Status::UnsupportedFormat;
            if (!(sampleRate > 0.0) || !std::isfinite(sampleRate)) return Status::InvalidFile;
            if (channels == 0 || channels > kMaxChannels) return Status::UnsupportedFormat;
            const bool isFloat = (flags & kLpcmFlagFloat) != 0;
            if (isFloat && bits == 32) out.info.sampleFormat = SampleFormat::Float32;
            else if (isFloat && bits == 64) out.info.sampleFormat = SampleFormat::Float64;
            else if (!isFloat && bits == 16) out.info.sampleFormat = SampleFormat::Int16;
            else if (!isFloat && bits == 24) out.info.sampleFormat = SampleFormat::Int24;
            else if (!isFloat && bits == 32) out.info.sampleFormat = SampleFormat::Int32;
            else return Status::UnsupportedFormat;
            if (framesPerPacket != 1 || bytesPerPacket != channels * (bits / 8)) return Status::UnsupportedFormat;
            out.info.sampleRate = sampleRate;
            out.info.bigEndian = (flags & kLpcmFlagLittleEndian) == 0;
            out.channels = channels;
            haveDesc = true;
        } else if (type != kChunkFree && size <= kMaxStoredChunkSize) {
            Chunk chunk;
            chunk.type = type;
            chunk.data.resize(static_cast<std::size_t>(size));
            if (s.read(chunk.data.data(), chunk.data.size()) != chunk.data.size()) {
                if (haveData) break;
                return Status::InvalidFile;
            }
            bool consumed = false;
            if (type == kChunkUuid && chunk.data.size() >= kUuidSize &&
                (std::memcmp(chunk.data.data(), kAmbixUuid, kUuidSize) == 0 ||
                 std::memcmp(chunk.data.data(), kAmbixUuidLegacy, kUuidSize) == 0)) {
                consumed = true;
                if (!haveAdaptor) {
                    haveAdaptor = true;
                    badAdaptor = !parseAdaptorPayload(chunk.data.data() + kUuidSize, chunk.data.size() - kUuidSize,
                                                      out.adaptor);
                }
            } else if (type == kChunkMark) {
                consumed = parseEitherOrder(parseMarkChunk, chunk.data, rawMarkers);
            } else if (type == kChunkRegn) {
                consumed = parseEitherOrder(parseRegnChunk, chunk.data, rawRegions);
            } else if (type == kChunkStrg) {
                consumed = parseEitherOrder(parseStrgChunk, chunk.data, rawStrings);
            }
            if (!consumed) out.chunks.push_back(std::move(chunk));
        }
        offset = payload + size;
    }

    if (!haveDesc || !haveData) return Status::InvalidFile;
    if (badAdaptor) return Status::InvalidAdaptorMatrix;

    const std::uint32_t bytesPerFrame = out.channels * bytesPerSample(out.info.sampleFormat);
    out.info.frames = static_cast<std::uint64_t>(dataBytes) / bytesPerFrame;
    out.info.container = Container::Caf;

    for (const RawMarker& raw : rawMarkers) {
        Marker marker;
        marker.position = raw.position;
        marker.name = findString(rawStrings, raw.id);
        out.markers.push_back(std::move(marker));
    }
    for (const RawRegion& raw : rawRegions) {
        if (raw.markers.empty()) continue;
        const RawMarker* start = &raw.markers.front();
        const RawMarker* end = &raw.markers.back();
        for (const RawMarker& m : raw.markers) {
            if (m.type == kMarkerRegionStart) start = &m;
            if (m.type == kMarkerRegionEnd) end = &m;
        }
        Region region;
        region.start = start->position;
        region.end = end->position;
        region.name = findString(rawStrings, start->id);
        out.regions.push_back(std::move(region));
    }

    return applyLayout(out.info, out.adaptor, out.channels);
}

// ---- building ----

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
inline void appendChunkHeader(std::vector<std::uint8_t>& out, std::uint32_t type, std::size_t size) {
    appendBE32(out, type);
    appendBE64(out, static_cast<std::uint64_t>(size));
}
inline void appendMarker(std::vector<std::uint8_t>& out, std::uint32_t type, double position, std::uint32_t id) {
    appendBE32(out, type);
    appendBE64(out, bitCast<std::uint64_t>(position));
    appendBE32(out, id);
    appendBE64(out, 0); // SMPTE time: unused
    appendBE32(out, 0); // channel: all
}

void appendMetadataChunks(std::vector<std::uint8_t>& out, const std::vector<Chunk>& chunks,
                          const std::vector<Marker>& markers, const std::vector<Region>& regions,
                          std::uint32_t& nextId) {
    // Names live in the strings chunk, keyed by marker ID. A region's start
    // and end markers share one ID, which also keys the region's name.
    std::vector<std::uint8_t> table, text;
    const auto addString = [&](std::uint32_t id, const std::string& name) {
        appendBE32(table, id);
        appendBE64(table, text.size());
        appendBytes(text, name.c_str(), std::strlen(name.c_str()) + 1);
    };

    if (!markers.empty()) {
        appendChunkHeader(out, kChunkMark, 8 + markers.size() * kMarkerSize);
        appendBE32(out, 0); // SMPTE time type: none
        appendBE32(out, static_cast<std::uint32_t>(markers.size()));
        for (const Marker& marker : markers) {
            appendMarker(out, 0, marker.position, nextId);
            addString(nextId++, marker.name);
        }
    }
    if (!regions.empty()) {
        appendChunkHeader(out, kChunkRegn, 8 + regions.size() * (12 + 2 * kMarkerSize));
        appendBE32(out, 0);
        appendBE32(out, static_cast<std::uint32_t>(regions.size()));
        std::uint32_t regionId = 1;
        for (const Region& region : regions) {
            appendBE32(out, regionId++);
            appendBE32(out, 0); // flags
            appendBE32(out, 2); // markers
            appendMarker(out, kMarkerRegionStart, region.start, nextId);
            appendMarker(out, kMarkerRegionEnd, region.end, nextId);
            addString(nextId++, region.name);
        }
    }
    if (!table.empty()) {
        appendChunkHeader(out, kChunkStrg, 4 + table.size() + text.size());
        appendBE32(out, static_cast<std::uint32_t>(table.size() / 12));
        appendBytes(out, table.data(), table.size());
        appendBytes(out, text.data(), text.size());
    }
    for (const Chunk& chunk : chunks) {
        appendChunkHeader(out, chunk.type, chunk.data.size());
        appendBytes(out, chunk.data.data(), chunk.data.size());
    }
}

std::vector<std::uint8_t> buildCafHead(const Info& info, const Matrix& adaptor, const std::vector<Chunk>& chunks,
                                       const std::vector<Marker>& markers, const std::vector<Region>& regions,
                                       std::uint32_t& nextId, std::int64_t& dataSizeOffset) {
    const std::uint32_t channels = info.fileChannels();
    const std::uint32_t sampleBytes = bytesPerSample(info.sampleFormat);
    const bool isFloat = info.sampleFormat == SampleFormat::Float32 || info.sampleFormat == SampleFormat::Float64;

    std::vector<std::uint8_t> head;
    appendBytes(head, "caff", 4);
    appendBytes(head, "\0\1\0\0", 4); // version 1, flags 0

    appendChunkHeader(head, kChunkDesc, kDescSize);
    appendBE64(head, bitCast<std::uint64_t>(info.sampleRate));
    appendBE32(head, kFormatLpcm);
    appendBE32(head, (isFloat ? kLpcmFlagFloat : 0u) | (info.bigEndian ? 0u : kLpcmFlagLittleEndian));
    appendBE32(head, channels * sampleBytes);
    appendBE32(head, 1);
    appendBE32(head, channels);
    appendBE32(head, sampleBytes * 8);

    if (!adaptor.empty()) {
        const std::size_t count = static_cast<std::size_t>(adaptor.rows()) * adaptor.cols();
        appendChunkHeader(head, kChunkUuid, kUuidSize + 8 + count * 4);
        appendBytes(head, kAmbixUuid, kUuidSize);
        appendBE32(head, adaptor.rows());
        appendBE32(head, adaptor.cols());
        for (std::size_t i = 0; i < count; ++i) appendBE32(head, bitCast<std::uint32_t>(adaptor.data()[i]));
    }

    appendMetadataChunks(head, chunks, markers, regions, nextId);

    // The data size stays -1 ("until end of file") until the file is closed,
    // so a file from an interrupted recording is still readable.
    appendBE32(head, kChunkData);
    dataSizeOffset = static_cast<std::int64_t>(head.size());
    appendBE64(head, static_cast<std::uint64_t>(std::int64_t(-1)));
    appendBE32(head, 0); // edit count
    return head;
}

} // namespace detail
} // namespace ambixfile
