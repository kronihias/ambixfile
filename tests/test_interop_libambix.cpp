// SPDX-License-Identifier: MIT
// Interoperability with libambix (LGPL), used here only as a test oracle.
// Built only when AMBIXFILE_LIBAMBIX_DIR is set; not part of the library.
#include "ambixfile/ambixfile.hpp"
#include "check.hpp"

#include <ambix/ambix.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ambixfile;

namespace {

std::vector<float> makeSignal(std::uint32_t channels, std::uint32_t frames, float phase) {
    std::vector<float> v(static_cast<std::size_t>(channels) * frames);
    for (std::uint32_t f = 0; f < frames; ++f)
        for (std::uint32_t c = 0; c < channels; ++c)
            v[static_cast<std::size_t>(f) * channels + c] =
                0.8f * static_cast<float>(std::sin(0.02 * (f + 1) * (c + 1) + phase));
    return v;
}

float maxDiff(const float* a, const float* b, std::size_t n) {
    float d = 0.0f;
    for (std::size_t i = 0; i < n; ++i) d = std::fmax(d, std::fabs(a[i] - b[i]));
    return d;
}

bool sameMatrix(const ambix_matrix_t* theirs, const Matrix& ours, float tolerance) {
    if (!theirs || theirs->rows != ours.rows() || theirs->cols != ours.cols()) return false;
    for (std::uint32_t r = 0; r < ours.rows(); ++r)
        for (std::uint32_t c = 0; c < ours.cols(); ++c)
            if (std::fabs(theirs->data[r][c] - ours(r, c)) > tolerance) return false;
    return true;
}

ambix_matrix_t* toTheirs(const Matrix& m) {
    ambix_matrix_t* out = ambix_matrix_init(m.rows(), m.cols(), nullptr);
    for (std::uint32_t r = 0; r < m.rows(); ++r)
        for (std::uint32_t c = 0; c < m.cols(); ++c) out->data[r][c] = m(r, c);
    return out;
}

const std::uint32_t kFrames = 4000;
const Matrix kAdaptor(4, 3, {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0.5f}); // W Y X -> W Y Z X

// ambixfile writes, libambix reads.
void oursToTheirs(const std::string& dir) {
    // basic, third order, 24 bit
    {
        const std::string path = dir + "/interop_ours_basic.caf";
        const std::vector<float> signal = makeSignal(16, kFrames, 0.1f);
        WriterConfig config;
        config.sampleRate = 44100.0;
        config.sampleFormat = SampleFormat::Int24;
        config.ambiChannels = 16;
        Writer writer;
        CHECK(writer.open(path.c_str(), config) == Status::Ok);
        CHECK(writer.write(signal.data(), kFrames) == kFrames);
        CHECK(writer.close() == Status::Ok);

        ambix_info_t info = {};
        ambix_t* file = ambix_open(path.c_str(), AMBIX_READ, &info);
        CHECK(file != nullptr);
        if (file) {
            CHECK(info.fileformat == AMBIX_BASIC && info.ambichannels == 16 && info.extrachannels == 0);
            CHECK(info.frames == kFrames && info.samplerate == 44100.0);
            CHECK(info.sampleformat == AMBIX_SAMPLEFORMAT_PCM24);
            std::vector<float> back(signal.size());
            CHECK(ambix_readf_float32(file, back.data(), nullptr, kFrames) == kFrames);
            CHECK(maxDiff(signal.data(), back.data(), signal.size()) <= 1.0f / 8388608.0f);
            ambix_close(file);
        }
    }
    // extended, reduced set plus two extra channels, float
    {
        const std::string path = dir + "/interop_ours_extended.caf";
        const std::vector<float> signal = makeSignal(5, kFrames, 0.2f);
        WriterConfig config;
        config.ambiChannels = 3;
        config.extraChannels = 2;
        config.adaptorMatrix = kAdaptor;
        Writer writer;
        CHECK(writer.open(path.c_str(), config) == Status::Ok);
        CHECK(writer.write(signal.data(), kFrames) == kFrames);
        CHECK(writer.close() == Status::Ok);

        // as stored
        ambix_info_t info = {};
        info.fileformat = AMBIX_EXTENDED;
        ambix_t* file = ambix_open(path.c_str(), AMBIX_READ, &info);
        CHECK(file != nullptr);
        if (file) {
            CHECK(info.fileformat == AMBIX_EXTENDED && info.ambichannels == 3 && info.extrachannels == 2);
            CHECK(info.frames == kFrames && info.sampleformat == AMBIX_SAMPLEFORMAT_FLOAT32);
            CHECK(sameMatrix(ambix_get_adaptormatrix(file), kAdaptor, 0.0f));
            std::vector<float> ambi(static_cast<std::size_t>(3) * kFrames), extra(static_cast<std::size_t>(2) * kFrames);
            CHECK(ambix_readf_float32(file, ambi.data(), extra.data(), kFrames) == kFrames);
            bool ok = true;
            for (std::uint32_t f = 0; f < kFrames; ++f)
                for (std::uint32_t c = 0; c < 5; ++c) {
                    const float theirs = c < 3 ? ambi[static_cast<std::size_t>(f) * 3 + c]
                                               : extra[static_cast<std::size_t>(f) * 2 + (c - 3)];
                    ok = ok && theirs == signal[static_cast<std::size_t>(f) * 5 + c];
                }
            CHECK(ok);
            ambix_close(file);
        }

        // expanded to the full set by each library
        ambix_info_t basicInfo = {};
        basicInfo.fileformat = AMBIX_BASIC;
        file = ambix_open(path.c_str(), AMBIX_READ, &basicInfo);
        CHECK(file != nullptr);
        if (file) {
            CHECK(basicInfo.ambichannels == 4);
            std::vector<float> theirs(static_cast<std::size_t>(4) * kFrames), extra(static_cast<std::size_t>(2) * kFrames);
            CHECK(ambix_readf_float32(file, theirs.data(), extra.data(), kFrames) == kFrames);
            ambix_close(file);
            Reader reader;
            CHECK(reader.open(path.c_str()) == Status::Ok);
            std::vector<float> ours(theirs.size());
            CHECK(reader.readAmbisonics(ours.data(), nullptr, kFrames) == kFrames);
            CHECK(maxDiff(theirs.data(), ours.data(), ours.size()) <= 1e-6f);
        }
    }
}

// libambix writes, ambixfile reads.
void theirsToOurs(const std::string& dir) {
    // basic, second order, 16 bit
    {
        const std::string path = dir + "/interop_theirs_basic.caf";
        const std::vector<float> signal = makeSignal(9, kFrames, 0.3f);
        ambix_info_t info = {};
        info.fileformat = AMBIX_BASIC;
        info.ambichannels = 9;
        info.samplerate = 48000.0;
        info.sampleformat = AMBIX_SAMPLEFORMAT_PCM16;
        ambix_t* file = ambix_open(path.c_str(), AMBIX_WRITE, &info);
        CHECK(file != nullptr);
        if (!file) return;
        CHECK(ambix_writef_float32(file, signal.data(), nullptr, kFrames) == kFrames);
        ambix_close(file);

        Reader reader;
        CHECK(reader.open(path.c_str()) == Status::Ok);
        CHECK(reader.info().fileFormat == FileFormat::Basic && reader.info().ambiChannels == 9);
        CHECK(reader.info().frames == kFrames && reader.info().sampleRate == 48000.0);
        CHECK(reader.info().sampleFormat == SampleFormat::Int16);
        std::vector<float> back(signal.size());
        CHECK(reader.read(back.data(), kFrames) == kFrames);
        CHECK(maxDiff(signal.data(), back.data(), signal.size()) <= 1.0f / 32768.0f);
    }
    // extended, reduced set plus two extra channels, float
    {
        const std::string path = dir + "/interop_theirs_extended.caf";
        const std::vector<float> ambi = makeSignal(3, kFrames, 0.4f), extra = makeSignal(2, kFrames, 0.5f);
        ambix_info_t info = {};
        info.fileformat = AMBIX_EXTENDED;
        info.ambichannels = 3;
        info.extrachannels = 2;
        info.samplerate = 96000.0;
        info.sampleformat = AMBIX_SAMPLEFORMAT_FLOAT32;
        ambix_t* file = ambix_open(path.c_str(), AMBIX_WRITE, &info);
        CHECK(file != nullptr);
        if (!file) return;
        ambix_matrix_t* matrix = toTheirs(kAdaptor);
        CHECK(ambix_set_adaptormatrix(file, matrix) == AMBIX_ERR_SUCCESS);
        ambix_matrix_destroy(matrix);
        CHECK(ambix_writef_float32(file, ambi.data(), extra.data(), kFrames) == kFrames);
        ambix_close(file);

        Reader reader;
        CHECK(reader.open(path.c_str()) == Status::Ok);
        const Info& ours = reader.info();
        CHECK(ours.fileFormat == FileFormat::Extended && ours.ambiChannels == 3 && ours.extraChannels == 2);
        CHECK(ours.fullAmbiChannels == 4 && ours.frames == kFrames && ours.sampleRate == 96000.0);
        CHECK(reader.adaptorMatrix().approxEquals(kAdaptor, 0.0f));
        std::vector<float> raw(static_cast<std::size_t>(5) * kFrames);
        CHECK(reader.read(raw.data(), kFrames) == kFrames);
        bool ok = true;
        for (std::uint32_t f = 0; f < kFrames; ++f)
            for (std::uint32_t c = 0; c < 5; ++c) {
                const float expected = c < 3 ? ambi[static_cast<std::size_t>(f) * 3 + c]
                                             : extra[static_cast<std::size_t>(f) * 2 + (c - 3)];
                ok = ok && raw[static_cast<std::size_t>(f) * 5 + c] == expected;
            }
        CHECK(ok);
    }
}

void conventionMatrices() {
    struct Case {
        ambix_matrixtype_t type;
        Matrix (*make)(std::uint32_t);
        std::uint32_t maxOrder;
    };
    const Case cases[] = {
        {AMBIX_MATRIX_FUMA, fumaToAmbix, 3}, {AMBIX_MATRIX_TO_FUMA, ambixToFuma, 3},
        {AMBIX_MATRIX_N3D, n3dToSn3d, 5},    {AMBIX_MATRIX_TO_N3D, sn3dToN3d, 5},
        {AMBIX_MATRIX_SID, sidToAcn, 5},     {AMBIX_MATRIX_TO_SID, acnToSid, 5},
    };
    for (const Case& c : cases)
        for (std::uint32_t order = 0; order <= c.maxOrder; ++order) {
            const std::uint32_t n = orderToChannels(order);
            ambix_matrix_t* theirs = ambix_matrix_init(n, n, nullptr);
            const bool filled = ambix_matrix_fill(theirs, c.type) != nullptr;
            const bool same = filled && sameMatrix(theirs, c.make(order), 1e-6f);
            if (!same) std::printf("  matrix type 0x%x order %u differs (filled: %d)\n", (unsigned)c.type, order, (int)filled);
            CHECK(same);
            ambix_matrix_destroy(theirs);
        }
}

} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    oursToTheirs(dir);
    theirsToOurs(dir);
    conventionMatrices();
    return finish("test_interop_libambix");
}
