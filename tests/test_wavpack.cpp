// SPDX-License-Identifier: MIT
// WavPack container. Without WavPack support this only checks the refusals.
#include "ambixfile/ambixfile.hpp"
#include "check.hpp"

#include <cmath>
#include <string>

using namespace ambixfile;

namespace {

std::vector<float> makeSignal(std::uint32_t channels, std::uint32_t frames) {
    std::vector<float> v(static_cast<std::size_t>(channels) * frames);
    for (std::uint32_t f = 0; f < frames; ++f)
        for (std::uint32_t c = 0; c < channels; ++c)
            v[static_cast<std::size_t>(f) * channels + c] =
                0.7f * static_cast<float>(std::sin(0.003 * (f + 1) * (c + 1) + 0.5 * c));
    return v;
}

float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 1e9f;
    float d = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) d = std::fmax(d, std::fabs(a[i] - b[i]));
    return d;
}

void testUnavailable() {
    MemoryStream stream;
    WriterConfig config;
    config.ambiChannels = 4;
    config.container = Container::WavPack;
    Writer writer;
    CHECK(writer.open(stream, config) == Status::UnsupportedFormat);
    const std::uint8_t fake[] = {'w', 'v', 'p', 'k', 0, 0, 0, 0, 0, 0, 0, 0};
    MemoryStream wv(std::vector<std::uint8_t>(fake, fake + sizeof(fake)));
    Reader reader;
    CHECK(reader.open(wv) == Status::UnsupportedFormat);
}

void testLossless() {
    const std::uint32_t frames = 30000; // more than one WavPack block
    const SampleFormat formats[] = {SampleFormat::Int16, SampleFormat::Int24, SampleFormat::Int32,
                                    SampleFormat::Float32, SampleFormat::Float64};
    for (SampleFormat format : formats)
        for (int threads : {0, 3}) {
            // Reduced set (W Y X) plus two extra channels, with metadata.
            const Matrix adaptor(4, 3, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0.5f});
            const std::vector<float> signal = makeSignal(5, frames);
            MemoryStream stream;
            WriterConfig config;
            config.sampleRate = 44100.0;
            config.sampleFormat = format;
            config.ambiChannels = 3;
            config.extraChannels = 2;
            config.adaptorMatrix = adaptor;
            config.container = Container::WavPack;
            config.wavpackThreads = threads;
            Marker m;
            m.position = 12.0;
            m.name = "start";
            config.markers.push_back(m);
            Chunk note;
            note.type = fourCC('n', 'o', 't', 'e');
            note.data = {9, 8, 7};
            config.chunks.push_back(note);

            Writer writer;
            CHECK(writer.open(stream, config) == Status::Ok);
            CHECK(writer.info().container == Container::WavPack);
            CHECK(writer.write(signal.data(), 10000) == 10000);
            // Metadata added during the recording goes to the trailing wrapper.
            Region r;
            r.start = 5.0;
            r.end = 9000.0;
            r.name = "take";
            CHECK(writer.addRegion(r) == Status::Ok);
            CHECK(writer.write(signal.data() + 10000 * 5, frames - 10000) == frames - 10000);
            CHECK(writer.close() == Status::Ok);
            CHECK(stream.bytes().size() > 4 && stream.bytes()[0] == 'w' && stream.bytes()[3] == 'k');

            Reader reader;
            ReaderConfig readerConfig;
            readerConfig.wavpackThreads = threads;
            CHECK(reader.open(stream, readerConfig) == Status::Ok);
            const Info& info = reader.info();
            CHECK(info.container == Container::WavPack && !info.lossy);
            CHECK(info.fileFormat == FileFormat::Extended);
            CHECK(info.ambiChannels == 3 && info.extraChannels == 2 && info.fullAmbiChannels == 4);
            CHECK(info.frames == frames && info.sampleRate == 44100.0);
            CHECK(info.sampleFormat == (format == SampleFormat::Float64 ? SampleFormat::Float32 : format));
            CHECK(reader.adaptorMatrix().approxEquals(adaptor, 0.0f));
            CHECK(reader.markers().size() == 1 && reader.regions().size() == 1 && reader.chunks().size() == 1);
            if (reader.markers().size() == 1 && reader.regions().size() == 1 && reader.chunks().size() == 1) {
                CHECK(reader.markers()[0].position == 12.0 && reader.markers()[0].name == "start");
                CHECK(reader.regions()[0].end == 9000.0 && reader.regions()[0].name == "take");
                CHECK(reader.chunks()[0].type == note.type && reader.chunks()[0].data == note.data);
            }

            std::vector<float> back(signal.size());
            CHECK(reader.read(back.data(), frames + 5) == frames);
            const float tolerance = format == SampleFormat::Int16 ? 1.0f / 32768.0f
                                    : format == SampleFormat::Int24 ? 1.0f / 8388608.0f : 1e-7f;
            CHECK(maxDiff(signal, back) <= tolerance);
            if (format == SampleFormat::Float32) CHECK(maxDiff(signal, back) == 0.0f); // bit exact

            // Seeking and the adaptor matrix.
            CHECK(reader.seek(20000) && reader.tell() == 20000);
            std::vector<float> ambi(4 * 100), extra(2 * 100);
            CHECK(reader.readAmbisonics(ambi.data(), extra.data(), 100) == 100);
            const float* in = &back[static_cast<std::size_t>(20000) * 5];
            CHECK(ambi[0] == in[0] && ambi[1] == in[1] && ambi[2] == 0.0f && ambi[3] == 0.5f * in[2]);
            CHECK(extra[0] == in[3] && extra[1] == in[4]);
            CHECK(reader.seek(frames));
            CHECK(reader.read(back.data(), 10) == 0);
        }
}

void testHybridAndFiles(const std::string& dir) {
    const std::uint32_t frames = 48000;
    const std::vector<float> signal = makeSignal(4, frames);
    const std::string path = dir + "/ambixfile_hybrid.wv";
    WriterConfig config;
    config.ambiChannels = 4;
    config.sampleFormat = SampleFormat::Int24;
    config.container = Container::WavPack;
    config.wavpackHybridBits = 1.0f;
    Writer writer;
    CHECK(writer.open(path.c_str(), config) == Status::InvalidArgument); // below WavPack's minimum
    config.wavpackHybridBits = 6.0f;
    CHECK(writer.open(path.c_str(), config) == Status::Ok);
    CHECK(writer.info().lossy);
    CHECK(writer.write(signal.data(), frames) == frames);
    CHECK(writer.close() == Status::Ok);

    Reader reader;
    CHECK(reader.open(path.c_str()) == Status::Ok);
    CHECK(reader.info().lossy && reader.info().fileFormat == FileFormat::Basic);
    CHECK(reader.info().lossyBitsPerSample > 1.0f && reader.info().lossyBitsPerSample < 24.0f);
    CHECK(reader.info().frames == frames && reader.info().ambiChannels == 4);
    std::vector<float> back(signal.size());
    CHECK(reader.read(back.data(), frames) == frames);
    const float error = maxDiff(signal, back);
    CHECK(error > 0.0f && error < 0.05f); // lossy, but close
    reader.close();
    std::remove(path.c_str());

    // WavPack cannot hold a file without audio; closing one says so.
    MemoryStream stream;
    config.wavpackHybridBits = 0.0f;
    CHECK(writer.open(stream, config) == Status::Ok);
    CHECK(writer.close() == Status::UnsupportedFormat);
    CHECK(reader.open(stream) != Status::Ok);

    // Fractional sample rates are not representable either.
    config.sampleRate = 44100.5;
    CHECK(writer.open(stream, config) == Status::InvalidArgument);
}

} // namespace

int main(int argc, char** argv) {
    if (!wavpackAvailable()) {
        testUnavailable();
        std::printf("(built without WavPack)\n");
    } else {
        testLossless();
        testHybridAndFiles(argc > 1 ? argv[1] : ".");
    }
    return finish("test_wavpack");
}
