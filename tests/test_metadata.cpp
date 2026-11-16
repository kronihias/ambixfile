// SPDX-License-Identifier: MIT
// Markers, regions and chunk placement.
#include "ambixfile/ambixfile.hpp"
#include "check.hpp"

#include <cstring>

using namespace ambixfile;

namespace {

Marker marker(double position, const char* name) {
    Marker m;
    m.position = position;
    m.name = name;
    return m;
}
Region region(double start, double end, const char* name) {
    Region r;
    r.start = start;
    r.end = end;
    r.name = name;
    return r;
}

std::size_t findBytes(const std::vector<std::uint8_t>& haystack, const char* needle) {
    const std::size_t n = std::strlen(needle);
    for (std::size_t i = 0; i + n <= haystack.size(); ++i)
        if (std::memcmp(haystack.data() + i, needle, n) == 0) return i;
    return haystack.size();
}

void testHeaderAndTrailing() {
    MemoryStream stream;
    WriterConfig config;
    config.ambiChannels = 4;
    config.sampleFormat = SampleFormat::Int16;
    config.markers.push_back(marker(100.0, "first"));
    config.regions.push_back(region(10.0, 300.0, "intro"));

    Writer writer;
    CHECK(writer.open(stream, config) == Status::Ok);
    // Added before the first write: still part of the header.
    CHECK(writer.addMarker(marker(2000.5, "")) == Status::Ok);
    CHECK(writer.addRegion(region(400.0, 500.0, "zw\xC3\xB6lf")) == Status::Ok);
    const std::vector<float> audio(4 * 1000, 0.25f);
    CHECK(writer.write(audio.data(), 1000) == 1000);
    const std::size_t headerEnd = findBytes(stream.bytes(), "data");
    CHECK(findBytes(stream.bytes(), "mark") < headerEnd && findBytes(stream.bytes(), "regn") < headerEnd);
    // Added during the recording: stored after the audio.
    CHECK(writer.addMarker(marker(900.0, "late")) == Status::Ok);
    CHECK(writer.addRegion(region(700.0, 800.0, "outro")) == Status::Ok);
    CHECK(writer.close() == Status::Ok);
    CHECK(writer.addMarker(marker(1.0, "x")) == Status::NotOpen);

    Reader reader;
    CHECK(reader.open(stream) == Status::Ok);
    CHECK(reader.info().frames == 1000);
    CHECK(reader.chunks().empty()); // marker chunks are not exposed as raw chunks
    const std::vector<Marker>& markers = reader.markers();
    CHECK(markers.size() == 3);
    if (markers.size() == 3) {
        CHECK(markers[0].position == 100.0 && markers[0].name == "first");
        CHECK(markers[1].position == 2000.5 && markers[1].name.empty());
        CHECK(markers[2].position == 900.0 && markers[2].name == "late");
    }
    const std::vector<Region>& regions = reader.regions();
    CHECK(regions.size() == 3);
    if (regions.size() == 3) {
        CHECK(regions[0].start == 10.0 && regions[0].end == 300.0 && regions[0].name == "intro");
        CHECK(regions[1].start == 400.0 && regions[1].end == 500.0 && regions[1].name == "zw\xC3\xB6lf");
        CHECK(regions[2].start == 700.0 && regions[2].end == 800.0 && regions[2].name == "outro");
    }
    // The audio is intact around the metadata.
    std::vector<float> back(audio.size());
    CHECK(reader.read(back.data(), 1000) == 1000);
    CHECK(back.front() == 0.25f && back.back() == 0.25f);
}

void testNoMetadataNoChunks() {
    MemoryStream stream;
    WriterConfig config;
    config.ambiChannels = 1;
    Writer writer;
    CHECK(writer.open(stream, config) == Status::Ok);
    CHECK(writer.close() == Status::Ok);
    CHECK(findBytes(stream.bytes(), "mark") == stream.bytes().size());
    CHECK(findBytes(stream.bytes(), "strg") == stream.bytes().size());
    Reader reader;
    CHECK(reader.open(stream) == Status::Ok);
    CHECK(reader.markers().empty() && reader.regions().empty());
}

// Files exist whose marker chunks are little-endian. Flip the three chunks
// of a file we wrote and check it still reads the same.
void testLittleEndianChunks() {
    MemoryStream stream;
    WriterConfig config;
    config.ambiChannels = 1;
    config.markers.push_back(marker(123.0, "a"));
    config.markers.push_back(marker(456.0, "bee"));
    config.regions.push_back(region(1.0, 2.0, "r"));
    Writer writer;
    CHECK(writer.open(stream, config) == Status::Ok);
    CHECK(writer.close() == Status::Ok);

    std::vector<std::uint8_t> bytes = stream.bytes();
    const auto flip = [&](std::size_t at, std::size_t n) {
        for (std::size_t i = 0; i < n / 2; ++i) std::swap(bytes[at + i], bytes[at + n - 1 - i]);
    };
    const auto flipMarker = [&](std::size_t at) {
        flip(at, 4);      // type
        flip(at + 4, 8);  // position
        flip(at + 12, 4); // id
    };
    std::size_t at = findBytes(bytes, "mark") + 12;
    flip(at + 4, 4);
    flipMarker(at + 8);
    flipMarker(at + 8 + 28);
    at = findBytes(bytes, "regn") + 12;
    flip(at + 4, 4);
    flip(at + 8, 4);
    flip(at + 16, 4);
    flipMarker(at + 20);
    flipMarker(at + 20 + 28);
    at = findBytes(bytes, "strg") + 12;
    flip(at, 4);
    for (int i = 0; i < 3; ++i) {
        flip(at + 4 + i * 12, 4);
        flip(at + 8 + i * 12, 8);
    }

    MemoryStream flipped(bytes);
    Reader reader;
    CHECK(reader.open(flipped) == Status::Ok);
    CHECK(reader.markers().size() == 2 && reader.regions().size() == 1);
    if (reader.markers().size() == 2 && reader.regions().size() == 1) {
        CHECK(reader.markers()[0].position == 123.0 && reader.markers()[0].name == "a");
        CHECK(reader.markers()[1].position == 456.0 && reader.markers()[1].name == "bee");
        CHECK(reader.regions()[0].start == 1.0 && reader.regions()[0].end == 2.0 && reader.regions()[0].name == "r");
    }
}

// Damaged marker chunks must not break reading (and are kept as raw chunks).
void testDamagedChunks() {
    MemoryStream stream;
    WriterConfig config;
    config.ambiChannels = 1;
    Chunk bad;
    bad.type = fourCC('m', 'a', 'r', 'k');
    bad.data = {0, 0, 0, 0, 0x7f, 0xff, 0xff, 0x7f, 1, 2, 3};
    config.chunks.push_back(bad);
    bad.type = fourCC('r', 'e', 'g', 'n');
    config.chunks.push_back(bad);
    bad.type = fourCC('s', 't', 'r', 'g');
    bad.data = {0x7f, 0xff, 0xff, 0x7f};
    config.chunks.push_back(bad);
    bad.data.clear();
    config.chunks.push_back(bad);
    Writer writer;
    CHECK(writer.open(stream, config) == Status::Ok);
    CHECK(writer.close() == Status::Ok);
    Reader reader;
    CHECK(reader.open(stream) == Status::Ok);
    CHECK(reader.markers().empty() && reader.regions().empty());
    CHECK(reader.chunks().size() == 4);
}

} // namespace

int main() {
    testHeaderAndTrailing();
    testNoMetadataNoChunks();
    testLittleEndianChunks();
    testDamagedChunks();
    return finish("test_metadata");
}
