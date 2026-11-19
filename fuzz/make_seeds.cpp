// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner
//
// Writes a few small, valid ambiX files to start the fuzzer from.
// Usage: make_seeds <directory>

#include <ambixfile/ambixfile.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ambixfile;

namespace {

bool writeSeed(const std::string& path, WriterConfig config, bool metadata) {
    Writer writer;
    if (writer.open(path.c_str(), config) != Status::Ok) return false;
    if (metadata) {
        writer.addMarker({3.0, "marker"});
        writer.addRegion({1.0, 9.0, "region"});
        writer.addChunk({fourCC('i', 'n', 'f', 'o'), {0, 0, 0, 1, 'k', 0, 'v', 0}});
    }
    const std::uint32_t channels = writer.info().fileChannels();
    std::vector<float> audio(static_cast<std::size_t>(channels) * 16);
    for (std::size_t i = 0; i < audio.size(); ++i) audio[i] = 0.5f * std::sin(0.3f * static_cast<float>(i));
    writer.write(audio.data(), 16);
    if (metadata) writer.addMarker({12.0, "late marker"}); // stored after the audio
    return writer.close() == Status::Ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: make_seeds <directory>\n");
        return 2;
    }
    const std::string dir = std::string(argv[1]) + "/";
    int failed = 0;
    const SampleFormat formats[] = {SampleFormat::Int16, SampleFormat::Int24, SampleFormat::Int32,
                                    SampleFormat::Float32, SampleFormat::Float64};
    int index = 0;
    for (SampleFormat format : formats) {
        for (int variant = 0; variant < 3; ++variant) {
            WriterConfig config;
            config.sampleFormat = format;
            config.bigEndian = (index % 2) == 0;
            if (variant == 0) {
                config.ambiChannels = 4;
            } else if (variant == 1) {
                config.adaptorMatrix = horizontalToAmbix(2);
                config.ambiChannels = config.adaptorMatrix.cols();
                config.extraChannels = 2;
            } else {
                config.adaptorMatrix = hemisphereToAmbix(1);
                config.ambiChannels = config.adaptorMatrix.cols();
            }
            failed += !writeSeed(dir + "seed" + std::to_string(index) + ".caf", config, variant != 0);
            if (wavpackAvailable() && format != SampleFormat::Float64) {
                config.container = Container::WavPack;
                if (format == SampleFormat::Int24 && variant == 0) config.wavpackHybridBits = 4.0f;
                failed += !writeSeed(dir + "seed" + std::to_string(index) + ".wv", config, variant != 0);
            }
            ++index;
        }
    }
    return failed ? 1 : 0;
}
