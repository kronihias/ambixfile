// SPDX-License-Identifier: MIT
#include "ambixfile/ambixfile.hpp"
#include "check.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace ambixfile;

namespace {

// Deterministic test signal in [-0.9, 0.9].
std::vector<float> makeSignal(std::uint32_t channels, std::uint32_t frames) {
    std::vector<float> v(static_cast<std::size_t>(channels) * frames);
    for (std::uint32_t f = 0; f < frames; ++f)
        for (std::uint32_t c = 0; c < channels; ++c)
            v[static_cast<std::size_t>(f) * channels + c] =
                0.9f * static_cast<float>(std::sin(0.01 * (f + 1) * (c + 1) + 0.3 * c));
    return v;
}

float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 1e9f;
    float d = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) d = std::fmax(d, std::fabs(a[i] - b[i]));
    return d;
}

float toleranceFor(SampleFormat format) {
    switch (format) {
    case SampleFormat::Int16: return 1.0f / 32768.0f;
    case SampleFormat::Int24: return 1.0f / 8388608.0f;
    default: return 1e-7f;
    }
}

void testBasicFormats() {
    const std::uint32_t channels = 16, frames = 5000; // more than one internal block
    const std::vector<float> signal = makeSignal(channels, frames);
    const SampleFormat formats[] = {SampleFormat::Int16, SampleFormat::Int24, SampleFormat::Int32,
                                    SampleFormat::Float32, SampleFormat::Float64};
    for (SampleFormat format : formats)
        for (bool bigEndian : {false, true}) {
            MemoryStream stream;
            WriterConfig config;
            config.sampleRate = 44100.0;
            config.sampleFormat = format;
            config.bigEndian = bigEndian;
            config.ambiChannels = channels;
            Writer writer;
            CHECK(writer.open(stream, config) == Status::Ok);
            CHECK(writer.info().fileFormat == FileFormat::Basic);
            CHECK(writer.write(signal.data(), 1234) == 1234);
            CHECK(writer.write(signal.data() + 1234 * channels, frames - 1234) == frames - 1234);
            CHECK(writer.close() == Status::Ok);
            CHECK(writer.close() == Status::NotOpen);

            Reader reader;
            CHECK(reader.open(stream) == Status::Ok);
            const Info& info = reader.info();
            CHECK(info.fileFormat == FileFormat::Basic);
            CHECK(info.sampleRate == 44100.0 && info.sampleFormat == format && info.bigEndian == bigEndian);
            CHECK(info.ambiChannels == channels && info.fullAmbiChannels == channels && info.extraChannels == 0);
            CHECK(info.frames == frames);
            CHECK(reader.adaptorMatrix().empty());

            std::vector<float> back(signal.size());
            CHECK(reader.read(back.data(), frames + 10) == frames);
            CHECK(maxDiff(signal, back) <= toleranceFor(format));
            CHECK(reader.read(back.data(), 1) == 0);

            // readAmbisonics on a basic file returns the same channels
            std::vector<float> ambi(signal.size());
            CHECK(reader.seek(0) && reader.tell() == 0);
            CHECK(reader.readAmbisonics(ambi.data(), nullptr, frames) == frames);
            CHECK(maxDiff(back, ambi) == 0.0f);

            // seeking, and the double API
            CHECK(reader.seek(4000));
            std::vector<double> tail(static_cast<std::size_t>(1000) * channels);
            CHECK(reader.read(tail.data(), 2000) == 1000);
            CHECK(std::fabs(tail[0] - signal[static_cast<std::size_t>(4000) * channels]) <= toleranceFor(format));
            CHECK(reader.tell() == frames);
        }
}

void testExtended() {
    // Horizontal-only first order: W Y X in the file, expanded to W Y Z X,
    // followed by two extra channels.
    const std::uint32_t frames = 3000;
    const Matrix adaptor(4, 3, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0.5f});
    const std::vector<float> signal = makeSignal(5, frames);

    MemoryStream stream;
    WriterConfig config;
    config.sampleFormat = SampleFormat::Float32;
    config.ambiChannels = 3;
    config.extraChannels = 2;
    config.adaptorMatrix = adaptor;
    Chunk note;
    note.type = fourCC('i', 'n', 'f', 'o');
    note.data = {0, 0, 0, 0};
    config.chunks.push_back(note);

    Writer writer;
    CHECK(writer.open(stream, config) == Status::Ok);
    CHECK(writer.info().fileFormat == FileFormat::Extended && writer.info().fullAmbiChannels == 4);
    CHECK(writer.write(signal.data(), frames) == frames);
    Chunk tail;
    tail.type = fourCC('t', 'e', 's', 't');
    tail.data = {1, 2, 3, 4, 5};
    CHECK(writer.addTrailingChunk(tail) == Status::Ok);
    Chunk bad;
    bad.type = fourCC('d', 'a', 't', 'a');
    CHECK(writer.addTrailingChunk(bad) == Status::InvalidArgument);
    CHECK(writer.close() == Status::Ok);

    Reader reader;
    CHECK(reader.open(stream) == Status::Ok);
    const Info& info = reader.info();
    CHECK(info.fileFormat == FileFormat::Extended);
    CHECK(info.ambiChannels == 3 && info.extraChannels == 2 && info.fullAmbiChannels == 4);
    CHECK(info.fileChannels() == 5 && info.frames == frames && info.sampleRate == 48000.0);
    CHECK(reader.adaptorMatrix().approxEquals(adaptor, 0.0f));
    CHECK(reader.chunks().size() == 2);
    if (reader.chunks().size() == 2) {
        CHECK(reader.chunks()[0].type == note.type && reader.chunks()[0].data == note.data);
        CHECK(reader.chunks()[1].type == tail.type && reader.chunks()[1].data == tail.data);
    }

    std::vector<float> raw(signal.size());
    CHECK(reader.read(raw.data(), frames) == frames);
    CHECK(maxDiff(signal, raw) == 0.0f);

    std::vector<float> ambi(static_cast<std::size_t>(4) * frames), extra(static_cast<std::size_t>(2) * frames);
    CHECK(reader.seek(0));
    CHECK(reader.readAmbisonics(ambi.data(), extra.data(), frames) == frames);
    bool ok = true;
    for (std::uint32_t f = 0; f < frames; ++f) {
        const float* in = &signal[static_cast<std::size_t>(f) * 5];
        const float* a = &ambi[static_cast<std::size_t>(f) * 4];
        const float* e = &extra[static_cast<std::size_t>(f) * 2];
        ok = ok && a[0] == in[0] && a[1] == in[1] && a[2] == 0.0f && a[3] == 0.5f * in[2];
        ok = ok && e[0] == in[3] && e[1] == in[4];
    }
    CHECK(ok);

    // Only one of the two outputs requested.
    std::vector<double> extraOnly(static_cast<std::size_t>(2) * 10);
    CHECK(reader.seek(100));
    CHECK(reader.readAmbisonics(nullptr, extraOnly.data(), 10) == 10);
    CHECK(static_cast<float>(extraOnly[0]) == signal[100 * 5 + 3]);

    // A full set plus extra channels gets an identity adaptor matrix.
    MemoryStream stream2;
    WriterConfig config2;
    config2.ambiChannels = 4;
    config2.extraChannels = 1;
    Writer writer2;
    CHECK(writer2.open(stream2, config2) == Status::Ok);
    CHECK(writer2.close() == Status::Ok);
    Reader reader2;
    CHECK(reader2.open(stream2) == Status::Ok);
    CHECK(reader2.info().fileFormat == FileFormat::Extended && reader2.adaptorMatrix().isIdentity(0.0f));
    CHECK(reader2.info().frames == 0);
}

void testUnfinishedAndDamaged() {
    const std::vector<float> signal = makeSignal(4, 100);
    MemoryStream stream;
    WriterConfig config;
    config.ambiChannels = 4;
    config.sampleFormat = SampleFormat::Int16;
    {
        Writer writer;
        CHECK(writer.open(stream, config) == Status::Ok);
        CHECK(writer.write(signal.data(), 100) == 100);
        // Snapshot before close(): the data chunk still has the open-ended size.
        MemoryStream unfinished(stream.bytes());
        Reader reader;
        CHECK(reader.open(unfinished) == Status::Ok);
        CHECK(reader.info().frames == 100);
    } // the destructor closes the file
    Reader reader;
    CHECK(reader.open(stream) == Status::Ok);
    CHECK(reader.info().frames == 100);
    reader.close();
    CHECK(!reader.isOpen());

    // Truncated in the middle of the audio: the complete frames stay readable.
    std::vector<std::uint8_t> cut = stream.bytes();
    cut.resize(cut.size() - 8 * 10 - 3);
    MemoryStream truncated(cut);
    CHECK(reader.open(truncated) == Status::Ok);
    CHECK(reader.info().frames == 89);

    // Not CAF, empty, and header-only streams.
    MemoryStream garbage(std::vector<std::uint8_t>(64, 0x55));
    CHECK(reader.open(garbage) == Status::NotCaf);
    MemoryStream empty;
    CHECK(reader.open(empty) == Status::NotCaf);
    MemoryStream headerOnly(std::vector<std::uint8_t>(stream.bytes().begin(), stream.bytes().begin() + 8));
    CHECK(reader.open(headerOnly) == Status::InvalidFile);
    CHECK(!reader.isOpen());
    float sample = 0.0f;
    CHECK(reader.read(&sample, 1) == 0);

    // Non-ambiX channel count is still readable as plain audio.
    std::vector<std::uint8_t> three = stream.bytes();
    three[8 + 12 + 24 + 3] = 3; // channels per frame
    three[8 + 12 + 16 + 3] = 6; // bytes per packet
    MemoryStream plain(three);
    CHECK(reader.open(plain) == Status::Ok);
    CHECK(reader.info().fileFormat == FileFormat::None);
    CHECK(reader.info().ambiChannels == 0 && reader.info().extraChannels == 3);
}

void testWriterValidation() {
    MemoryStream stream;
    Writer writer;
    WriterConfig config;
    CHECK(writer.open(stream, config) == Status::InvalidArgument); // no channels
    config.ambiChannels = 5;
    CHECK(writer.open(stream, config) == Status::InvalidArgument); // not a full set
    config.adaptorMatrix = Matrix(9, 4);
    CHECK(writer.open(stream, config) == Status::InvalidAdaptorMatrix); // wrong column count
    config.adaptorMatrix = Matrix(8, 5);
    CHECK(writer.open(stream, config) == Status::InvalidAdaptorMatrix); // rows not a full set
    config.adaptorMatrix = Matrix(9, 5);
    config.sampleRate = 0.0;
    CHECK(writer.open(stream, config) == Status::InvalidArgument);
    config.sampleRate = 96000.0;
    CHECK(writer.open(stream, config) == Status::Ok);
    CHECK(!writer.isOpen() == false);
    CHECK(writer.close() == Status::Ok);

    // Integer formats clip instead of wrapping.
    MemoryStream clipStream;
    WriterConfig clip;
    clip.ambiChannels = 1;
    clip.sampleFormat = SampleFormat::Int16;
    Writer clipWriter;
    CHECK(clipWriter.open(clipStream, clip) == Status::Ok);
    const float loud[3] = {2.0f, -2.0f, std::nanf("")};
    CHECK(clipWriter.write(loud, 3) == 3);
    CHECK(clipWriter.close() == Status::Ok);
    Reader reader;
    CHECK(reader.open(clipStream) == Status::Ok);
    float back[3] = {};
    CHECK(reader.read(back, 3) == 3);
    CHECK(back[0] == 32767.0f / 32768.0f && back[1] == -1.0f && back[2] == 0.0f);
}

void testFiles(const std::string& dir) {
    const std::string path = dir + "/ambixfile_roundtrip_\xC3\xA4.caf"; // non-ASCII name
    const std::vector<float> signal = makeSignal(9, 2000);
    WriterConfig config;
    config.ambiChannels = 9;
    config.sampleFormat = SampleFormat::Int24;
    Writer writer;
    CHECK(writer.open(path.c_str(), config) == Status::Ok);
    CHECK(writer.write(signal.data(), 2000) == 2000);
    CHECK(writer.close() == Status::Ok);

    Reader reader;
    CHECK(reader.open(path.c_str()) == Status::Ok);
    CHECK(reader.info().frames == 2000 && reader.info().ambiChannels == 9);
    std::vector<float> back(signal.size());
    CHECK(reader.read(back.data(), 2000) == 2000);
    CHECK(maxDiff(signal, back) <= toleranceFor(SampleFormat::Int24));
    Reader moved(std::move(reader));
    CHECK(moved.isOpen() && !reader.isOpen());
    moved.close();
    std::remove(path.c_str());

    CHECK(reader.open((dir + "/does_not_exist.caf").c_str()) == Status::IoError);
    CHECK(std::strcmp(toString(Status::NotCaf), "not a CAF file") == 0);
}

} // namespace

int main(int argc, char** argv) {
    testBasicFormats();
    testExtended();
    testUnfinishedAndDamaged();
    testWriterValidation();
    testFiles(argc > 1 ? argv[1] : ".");
    return finish("test_roundtrip");
}
