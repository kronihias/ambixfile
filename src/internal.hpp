// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner
//
// Internal helpers: byte order, CAF constants, sample conversion.

#ifndef AMBIXFILE_INTERNAL_HPP
#define AMBIXFILE_INTERNAL_HPP

#include "ambixfile/ambixfile.hpp"

#include <cmath>
#include <cstring>

namespace ambixfile {
namespace detail {

// ---- big-endian field access (all CAF header fields are big-endian) ----

inline std::uint16_t loadBE16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t loadBE32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
inline std::uint32_t loadLE32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[3]) << 24) | (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) | static_cast<std::uint32_t>(p[0]);
}
inline std::uint64_t loadBE64(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(loadBE32(p)) << 32) | loadBE32(p + 4);
}
inline std::uint64_t loadLE64(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(loadLE32(p + 4)) << 32) | loadLE32(p);
}
inline void storeBE16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
inline void storeBE32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}
inline void storeLE32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[3] = static_cast<std::uint8_t>(v >> 24);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[0] = static_cast<std::uint8_t>(v);
}
inline void storeBE64(std::uint8_t* p, std::uint64_t v) noexcept {
    storeBE32(p, static_cast<std::uint32_t>(v >> 32));
    storeBE32(p + 4, static_cast<std::uint32_t>(v));
}
inline void storeLE64(std::uint8_t* p, std::uint64_t v) noexcept {
    storeLE32(p, static_cast<std::uint32_t>(v));
    storeLE32(p + 4, static_cast<std::uint32_t>(v >> 32));
}

template <typename To, typename From>
inline To bitCast(const From& from) noexcept {
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}

// ---- CAF constants ----

constexpr std::uint32_t kChunkDesc = fourCC('d', 'e', 's', 'c');
constexpr std::uint32_t kChunkData = fourCC('d', 'a', 't', 'a');
constexpr std::uint32_t kChunkUuid = fourCC('u', 'u', 'i', 'd');
constexpr std::uint32_t kChunkFree = fourCC('f', 'r', 'e', 'e');
constexpr std::uint32_t kFormatLpcm = fourCC('l', 'p', 'c', 'm');
constexpr std::uint32_t kLpcmFlagFloat = 1u << 0;
constexpr std::uint32_t kLpcmFlagLittleEndian = 1u << 1;

constexpr std::size_t kFileHeaderSize = 8;   // 'caff', version, flags
constexpr std::size_t kChunkHeaderSize = 12; // type, 64-bit size
constexpr std::size_t kDescSize = 32;
constexpr std::size_t kUuidSize = 16;

// UUID of the ambiX adaptor matrix chunk (format version 1): the version 5
// UUID of the URL http://ambisonics.iem.at/xchange/format/1.0
constexpr std::uint8_t kAmbixUuid[kUuidSize] = {0x1a, 0xd3, 0x18, 0xc3, 0x00, 0xe5, 0x55, 0x76,
                                                0xbe, 0x2d, 0x0d, 0xca, 0x24, 0x60, 0xbc, 0x89};
// Identifier used by early ambiX files: the ASCII text "IEM.AT/AMBIX/XML".
constexpr std::uint8_t kAmbixUuidLegacy[kUuidSize] = {'I', 'E', 'M', '.', 'A', 'T', '/', 'A',
                                                      'M', 'B', 'I', 'X', '/', 'X', 'M', 'L'};

inline std::uint32_t bytesPerSample(SampleFormat format) noexcept {
    switch (format) {
    case SampleFormat::Int16: return 2;
    case SampleFormat::Int24: return 3;
    case SampleFormat::Int32: return 4;
    case SampleFormat::Float32: return 4;
    case SampleFormat::Float64: return 8;
    }
    return 0;
}

// Frames handled per internal block; keeps scratch buffers small.
inline std::uint32_t blockFramesFor(std::uint32_t channels) noexcept {
    const std::uint32_t frames = 16384u / (channels ? channels : 1u);
    return frames ? frames : 1u;
}

// ---- sample conversion ----

template <typename T>
inline void decodeSamples(const std::uint8_t* src, T* dst, std::size_t count, SampleFormat format,
                          bool bigEndian) noexcept {
    switch (format) {
    case SampleFormat::Int16:
        for (std::size_t i = 0; i < count; ++i, src += 2) {
            const std::uint16_t u = bigEndian ? static_cast<std::uint16_t>((src[0] << 8) | src[1])
                                              : static_cast<std::uint16_t>((src[1] << 8) | src[0]);
            dst[i] = static_cast<T>(static_cast<std::int16_t>(u)) * static_cast<T>(1.0 / 32768.0);
        }
        break;
    case SampleFormat::Int24:
        for (std::size_t i = 0; i < count; ++i, src += 3) {
            const std::uint32_t u =
                bigEndian ? (static_cast<std::uint32_t>(src[0]) << 16) | (static_cast<std::uint32_t>(src[1]) << 8) | src[2]
                          : (static_cast<std::uint32_t>(src[2]) << 16) | (static_cast<std::uint32_t>(src[1]) << 8) | src[0];
            const std::int32_t v = static_cast<std::int32_t>(u ^ 0x800000u) - 0x800000;
            dst[i] = static_cast<T>(v) * static_cast<T>(1.0 / 8388608.0);
        }
        break;
    case SampleFormat::Int32:
        for (std::size_t i = 0; i < count; ++i, src += 4) {
            const std::int32_t v = bitCast<std::int32_t>(bigEndian ? loadBE32(src) : loadLE32(src));
            dst[i] = static_cast<T>(static_cast<double>(v) * (1.0 / 2147483648.0));
        }
        break;
    case SampleFormat::Float32:
        for (std::size_t i = 0; i < count; ++i, src += 4)
            dst[i] = static_cast<T>(bitCast<float>(bigEndian ? loadBE32(src) : loadLE32(src)));
        break;
    case SampleFormat::Float64:
        for (std::size_t i = 0; i < count; ++i, src += 8)
            dst[i] = static_cast<T>(bitCast<double>(bigEndian ? loadBE64(src) : loadLE64(src)));
        break;
    }
}

// Scale to an integer range, clip, and round to nearest. NaN becomes 0.
inline std::int64_t toFixed(double sample, double scale) noexcept {
    double x = sample * scale;
    if (!(x == x)) return 0;
    if (x > scale - 1.0) x = scale - 1.0;
    if (x < -scale) x = -scale;
    return static_cast<std::int64_t>(std::llrint(x));
}

template <typename T>
inline void encodeSamples(const T* src, std::uint8_t* dst, std::size_t count, SampleFormat format,
                          bool bigEndian) noexcept {
    switch (format) {
    case SampleFormat::Int16:
        for (std::size_t i = 0; i < count; ++i, dst += 2) {
            const std::uint16_t u = static_cast<std::uint16_t>(toFixed(static_cast<double>(src[i]), 32768.0));
            if (bigEndian) {
                dst[0] = static_cast<std::uint8_t>(u >> 8);
                dst[1] = static_cast<std::uint8_t>(u);
            } else {
                dst[1] = static_cast<std::uint8_t>(u >> 8);
                dst[0] = static_cast<std::uint8_t>(u);
            }
        }
        break;
    case SampleFormat::Int24:
        for (std::size_t i = 0; i < count; ++i, dst += 3) {
            const std::uint32_t u = static_cast<std::uint32_t>(toFixed(static_cast<double>(src[i]), 8388608.0));
            if (bigEndian) {
                dst[0] = static_cast<std::uint8_t>(u >> 16);
                dst[1] = static_cast<std::uint8_t>(u >> 8);
                dst[2] = static_cast<std::uint8_t>(u);
            } else {
                dst[2] = static_cast<std::uint8_t>(u >> 16);
                dst[1] = static_cast<std::uint8_t>(u >> 8);
                dst[0] = static_cast<std::uint8_t>(u);
            }
        }
        break;
    case SampleFormat::Int32:
        for (std::size_t i = 0; i < count; ++i, dst += 4) {
            const std::uint32_t u = static_cast<std::uint32_t>(toFixed(static_cast<double>(src[i]), 2147483648.0));
            if (bigEndian) storeBE32(dst, u);
            else storeLE32(dst, u);
        }
        break;
    case SampleFormat::Float32:
        for (std::size_t i = 0; i < count; ++i, dst += 4) {
            const std::uint32_t u = bitCast<std::uint32_t>(static_cast<float>(src[i]));
            if (bigEndian) storeBE32(dst, u);
            else storeLE32(dst, u);
        }
        break;
    case SampleFormat::Float64:
        for (std::size_t i = 0; i < count; ++i, dst += 8) {
            const std::uint64_t u = bitCast<std::uint64_t>(static_cast<double>(src[i]));
            if (bigEndian) storeBE64(dst, u);
            else storeLE64(dst, u);
        }
        break;
    }
}

} // namespace detail
} // namespace ambixfile

#endif // AMBIXFILE_INTERNAL_HPP
