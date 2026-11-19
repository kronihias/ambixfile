// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner
//
// libFuzzer target: open arbitrary bytes as an ambiX file and use everything
// the reader offers. Build with -DAMBIXFILE_BUILD_FUZZERS=ON (needs Clang).

#include <ambixfile/ambixfile.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

using namespace ambixfile;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    MemoryStream stream(std::vector<std::uint8_t>(data, data + size));
    Reader reader;
    if (reader.open(stream) != Status::Ok) return 0;

    const Info info = reader.info();
    // Keep a hostile header from asking for huge buffers: the library itself
    // only allocates per block, but the buffers below scale with the channels.
    if (info.fileChannels() == 0 || info.fileChannels() > 512 || info.fullAmbiChannels > 1024) return 0;

    const std::int64_t frames = 64;
    std::vector<float> raw(static_cast<std::size_t>(frames) * info.fileChannels());
    std::vector<double> ambi(static_cast<std::size_t>(frames) * info.fullAmbiChannels);
    std::vector<double> extra(static_cast<std::size_t>(frames) * info.extraChannels);
    std::vector<std::int16_t> ints(raw.size());

    reader.read(raw.data(), frames);
    reader.readAmbisonics(ambi.empty() ? nullptr : ambi.data(), extra.empty() ? nullptr : extra.data(), frames);
    reader.read(ints.data(), frames);
    reader.seek(info.frames / 2);
    reader.read(raw.data(), frames);
    reader.seek(info.frames);
    reader.read(raw.data(), frames);

    std::size_t total = reader.markers().size() + reader.regions().size();
    for (const Chunk& chunk : reader.chunks()) total += chunk.data.size();
    std::uint32_t h = 0, v = 0;
    (void)mixedOrderOf(reader.adaptorMatrix(), h, v);
    (void)total;
    return 0;
}
