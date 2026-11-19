// SPDX-License-Identifier: MIT
// Adaptor matrices for reduced channel sets, and integer sample I/O.
#include "ambixfile/ambixfile.h"
#include "ambixfile/ambixfile.hpp"
#include "check.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace ambixfile;

namespace {

void testMixedOrder() {
    // horizontal only, order 2: W, then (1,-1) (1,1), then (2,-2) (2,2)
    const Matrix h2 = horizontalToAmbix(2);
    CHECK(h2.rows() == 9 && h2.cols() == 5);
    const std::uint32_t kept[5] = {0, 1, 3, 4, 8};
    for (std::uint32_t r = 0; r < 9; ++r)
        for (std::uint32_t c = 0; c < 5; ++c) CHECK(h2(r, c) == (kept[c] == r ? 1.0f : 0.0f));

    for (std::uint32_t h = 0; h <= 7; ++h) {
        for (std::uint32_t v = 0; v <= h; ++v) {
            const Matrix m = mixedOrderToAmbix(h, v);
            CHECK(m.rows() == (h + 1) * (h + 1) && m.cols() == (v + 1) * (v + 1) + 2 * (h - v));
            CHECK(m.transposed().multiplied(m).isIdentity(0.0f)); // orthonormal columns
            std::uint32_t gotH = 99, gotV = 99;
            CHECK(mixedOrderOf(m, gotH, gotV) && gotH == h && gotV == v);
        }
    }
    CHECK(mixedOrderToAmbix(3, 3).isIdentity(0.0f));
    CHECK(mixedOrderToAmbix(1, 2).empty());

    std::uint32_t h = 0, v = 0;
    CHECK(!mixedOrderOf(Matrix(), h, v));
    CHECK(!mixedOrderOf(hemisphereToAmbix(2), h, v));
    Matrix scaled = horizontalToAmbix(3);
    scaled(0, 0) = 0.5f;
    CHECK(!mixedOrderOf(scaled, h, v));
    CHECK(!mixedOrderOf(fumaToAmbix(1), h, v));
}

void testHemisphere() {
    CHECK(hemisphereToAmbix(0).empty() && hemisphereToAmbix(8).empty());
    for (std::uint32_t order = 1; order <= 7; ++order) {
        const Matrix a = hemisphereToAmbix(order);
        CHECK(a.rows() == (order + 1) * (order + 1) && a.cols() == (order + 1) * (order + 2) / 2);
        CHECK(a.transposed().multiplied(a).isIdentity(1e-5f));
        // reduce -> expand is a projector: doing it twice changes nothing
        const Matrix p = a.multiplied(a.transposed());
        CHECK(p.multiplied(p).approxEquals(p, 1e-5f));
        Matrix pinv;
        CHECK(a.pseudoInverse(pinv) && pinv.approxEquals(a.transposed(), 1e-4f));
    }
    // order 1: W and Z share a column, X and Y pass
    const Matrix a1 = hemisphereToAmbix(1);
    CHECK(a1(1, 1) == 1.0f && a1(3, 2) == 1.0f && a1(0, 0) > 0.7f && a1(2, 0) > 0.6f);

    // usable as a file's adaptor matrix
    MemoryStream stream;
    WriterConfig config;
    config.ambiChannels = 6;
    config.adaptorMatrix = hemisphereToAmbix(2);
    Writer writer;
    CHECK(writer.open(stream, config) == Status::Ok);
    const std::vector<float> frame = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    CHECK(writer.write(frame.data(), 1) == 1);
    CHECK(writer.close() == Status::Ok);
    Reader reader;
    CHECK(reader.open(stream) == Status::Ok);
    CHECK(reader.info().fileFormat == FileFormat::Extended && reader.info().fullAmbiChannels == 9);
    CHECK(reader.adaptorMatrix().approxEquals(config.adaptorMatrix, 0.0f));

    // C API
    float values[64];
    std::uint32_t rows = 0, cols = 0;
    CHECK(ambixfile_mixed_order_matrix(2, 0, values, 64, &rows, &cols) == 45 && rows == 9 && cols == 5);
    CHECK(values[0] == 1.0f && values[1] == 0.0f);
    CHECK(ambixfile_mixed_order_matrix(1, 2, values, 64, &rows, &cols) == 0);
    CHECK(ambixfile_hemisphere_matrix(2, values, 64, &rows, &cols) == 54 && rows == 9 && cols == 6);
    CHECK(ambixfile_hemisphere_matrix(9, values, 64, &rows, &cols) == 0);
}

template <typename I>
std::vector<I> roundTrip(SampleFormat format, const std::vector<I>& in, std::uint32_t channels) {
    MemoryStream stream;
    WriterConfig config;
    config.ambiChannels = channels;
    config.sampleFormat = format;
    Writer writer;
    CHECK(writer.open(stream, config) == Status::Ok);
    const std::int64_t frames = static_cast<std::int64_t>(in.size() / channels);
    CHECK(writer.write(in.data(), frames) == frames);
    CHECK(writer.close() == Status::Ok);
    Reader reader;
    CHECK(reader.open(stream) == Status::Ok);
    std::vector<I> out(in.size() + channels, I(77));
    CHECK(reader.read(out.data(), frames + 1) == frames); // asks for one frame too many
    CHECK(out[in.size()] == I(77));
    out.resize(in.size());
    return out;
}

void testIntegerSamples() {
    const std::uint32_t channels = 4;
    const std::uint32_t frames = 5000; // more than one internal block
    std::vector<std::int16_t> s16(static_cast<std::size_t>(frames) * channels);
    std::vector<std::int32_t> s32(s16.size());
    std::uint32_t state = 12345;
    for (std::size_t i = 0; i < s16.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        s32[i] = static_cast<std::int32_t>(state);
        s16[i] = static_cast<std::int16_t>(state >> 16);
    }
    s16[0] = INT16_MIN; s16[1] = INT16_MAX; s16[2] = 0; s16[3] = -1;
    s32[0] = INT32_MIN; s32[1] = INT32_MAX; s32[2] = 0; s32[3] = -1;

    // bit-exact wherever the file is at least as wide as the samples
    CHECK(roundTrip(SampleFormat::Int16, s16, channels) == s16);
    CHECK(roundTrip(SampleFormat::Int24, s16, channels) == s16);
    CHECK(roundTrip(SampleFormat::Int32, s16, channels) == s16);
    CHECK(roundTrip(SampleFormat::Float32, s16, channels) == s16);
    CHECK(roundTrip(SampleFormat::Float64, s16, channels) == s16);
    CHECK(roundTrip(SampleFormat::Int32, s32, channels) == s32);
    CHECK(roundTrip(SampleFormat::Float64, s32, channels) == s32);

    // narrower file: rounded to its resolution, never wrapped
    const std::vector<std::int32_t> narrow = roundTrip(SampleFormat::Int16, s32, channels);
    bool close = true;
    for (std::size_t i = 0; i < s32.size(); ++i) {
        const std::int64_t d = static_cast<std::int64_t>(narrow[i]) - s32[i];
        if (d > 65536 || d < -65536) close = false;
    }
    CHECK(close);
    CHECK(narrow[1] == INT32_MAX - 65535); // 32767 << 16: clipped, not wrapped

    // float file read as integers: full scale maps to the type's range, clipped
    MemoryStream stream;
    WriterConfig config;
    config.ambiChannels = 1;
    Writer writer;
    CHECK(writer.open(stream, config) == Status::Ok);
    const float f[5] = {0.0f, 0.5f, -0.5f, 2.0f, -2.0f};
    CHECK(writer.write(f, 5) == 5);
    CHECK(writer.close() == Status::Ok);
    Reader reader;
    CHECK(reader.open(stream) == Status::Ok);
    std::int16_t i16[5];
    CHECK(reader.read(i16, 5) == 5);
    CHECK(i16[0] == 0 && i16[1] == 16384 && i16[2] == -16384 && i16[3] == INT16_MAX && i16[4] == INT16_MIN);
    CHECK(reader.seek(0));
    std::int32_t i32[5];
    CHECK(reader.read(i32, 5) == 5);
    CHECK(i32[1] == 1073741824 && i32[3] == INT32_MAX && i32[4] == INT32_MIN);

    // C API
    ambixfile_reader* cr = nullptr;
    (void)cr;
    std::int16_t none[1];
    CHECK(ambixfile_reader_read_int16(nullptr, none, 1) == 0);
    CHECK(ambixfile_writer_write_int32(nullptr, i32, 1) == 0);
}

} // namespace

int main() {
    testMixedOrder();
    testHemisphere();
    testIntegerSamples();
    return finish("test_reduced_sets");
}
