// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner
//
// ambixfile-info: print what an ambiX file holds.

#include <ambixfile/ambixfile.hpp>

#include <cstdio>
#include <cstring>

using namespace ambixfile;

namespace {

const char* formatName(SampleFormat f) {
    switch (f) {
    case SampleFormat::Int16: return "16 bit integer";
    case SampleFormat::Int24: return "24 bit integer";
    case SampleFormat::Int32: return "32 bit integer";
    case SampleFormat::Float32: return "32 bit float";
    case SampleFormat::Float64: return "64 bit float";
    }
    return "?";
}

int show(const char* path, bool printMatrix) {
    Reader reader;
    const Status status = reader.open(path);
    if (status != Status::Ok) {
        std::fprintf(stderr, "%s: %s\n", path, toString(status));
        return 1;
    }
    const Info& info = reader.info();
    std::printf("%s\n", path);
    if (info.container == Container::WavPack) {
        if (info.lossy) std::printf("  container:       WavPack, lossy (%.1f bit/sample)\n", static_cast<double>(info.lossyBitsPerSample));
        else std::printf("  container:       WavPack, lossless\n");
    } else {
        std::printf("  container:       CAF, %s-endian\n", info.bigEndian ? "big" : "little");
    }
    std::printf("  format:          %s\n", info.fileFormat == FileFormat::Basic      ? "ambiX basic"
                                           : info.fileFormat == FileFormat::Extended ? "ambiX extended"
                                                                                     : "not ambiX");
    std::printf("  sample rate:     %g Hz\n", info.sampleRate);
    std::printf("  samples:         %s\n", formatName(info.sampleFormat));
    std::printf("  frames:          %llu", static_cast<unsigned long long>(info.frames));
    if (info.sampleRate > 0.0) std::printf(" (%.3f s)", static_cast<double>(info.frames) / info.sampleRate);
    std::printf("\n");
    std::uint32_t order = 0;
    if (channelsToOrder(info.fullAmbiChannels, order) && info.fullAmbiChannels)
        std::printf("  Ambisonics:      order %u, %u channels stored, %u after the adaptor matrix\n", order,
                    info.ambiChannels, info.fullAmbiChannels);
    std::printf("  extra channels:  %u\n", info.extraChannels);

    const Matrix& m = reader.adaptorMatrix();
    if (!m.empty()) {
        std::uint32_t h = 0, v = 0;
        std::printf("  adaptor matrix:  %u x %u", m.rows(), m.cols());
        if (m.isIdentity()) std::printf(" (identity)");
        else if (mixedOrderOf(m, h, v) && v == 0) std::printf(" (horizontal only, order %u)", h);
        else if (mixedOrderOf(m, h, v)) std::printf(" (mixed order %uH%uV)", h, v);
        std::printf("\n");
        if (printMatrix) {
            for (std::uint32_t r = 0; r < m.rows(); ++r) {
                std::printf("   ");
                for (std::uint32_t c = 0; c < m.cols(); ++c) std::printf(" %9.6f", static_cast<double>(m(r, c)));
                std::printf("\n");
            }
        }
    }
    for (const Marker& marker : reader.markers())
        std::printf("  marker:          %.0f  %s\n", marker.position, marker.name.c_str());
    for (const Region& region : reader.regions())
        std::printf("  region:          %.0f - %.0f  %s\n", region.start, region.end, region.name.c_str());
    for (const Chunk& chunk : reader.chunks())
        std::printf("  chunk:           '%c%c%c%c', %llu bytes\n", static_cast<char>(chunk.type >> 24),
                    static_cast<char>(chunk.type >> 16), static_cast<char>(chunk.type >> 8),
                    static_cast<char>(chunk.type), static_cast<unsigned long long>(chunk.data.size()));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool printMatrix = false;
    int files = 0, failed = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-m") || !std::strcmp(argv[i], "--matrix")) printMatrix = true;
    }
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') continue;
        ++files;
        failed += show(argv[i], printMatrix);
    }
    if (!files) {
        std::fprintf(stderr, "usage: ambixfile-info [-m|--matrix] file...\n"
                             "  -m, --matrix   also print the adaptor matrix\n");
        return 2;
    }
    return failed ? 1 : 0;
}
