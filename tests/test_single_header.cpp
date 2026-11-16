// SPDX-License-Identifier: MIT
// Builds the generated single header with its implementation.
#define AMBIXFILE_IMPLEMENTATION
#include "ambixfile.hpp"

#include "check.hpp"

bool secondTranslationUnitWorks(); // single_header_second_tu.cpp

int main() {
    using namespace ambixfile;
    MemoryStream stream;
    WriterConfig config;
    config.ambiChannels = 4;
    Writer writer;
    CHECK(writer.open(stream, config) == Status::Ok);
    const float frame[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    CHECK(writer.write(frame, 1) == 1);
    CHECK(writer.close() == Status::Ok);

    Reader reader;
    CHECK(reader.open(stream) == Status::Ok);
    float back[4] = {};
    CHECK(reader.read(back, 1) == 1);
    CHECK(back[0] == 0.1f && back[3] == 0.4f);
    CHECK(secondTranslationUnitWorks());
#ifdef AMBIXFILE_ENABLE_WAVPACK
    CHECK(wavpackAvailable());
    MemoryStream packed;
    config.container = Container::WavPack;
    CHECK(writer.open(packed, config) == Status::Ok);
    CHECK(writer.write(frame, 1) == 1);
    CHECK(writer.close() == Status::Ok);
    CHECK(reader.open(packed) == Status::Ok);
    CHECK(reader.info().container == Container::WavPack && reader.info().frames == 1);
#else
    CHECK(!wavpackAvailable());
#endif
    return finish("test_single_header");
}
