// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner
//
// C API: a thin layer over the C++ classes. No exception leaves this file.

#include "ambixfile/ambixfile.h"
#include "ambixfile/ambixfile.hpp"

#include <cstring>
#include <new>

using namespace ambixfile;

struct ambixfile_reader {
    Reader reader;
};
struct ambixfile_writer {
    Writer writer;
};

namespace {

class CallbackStream final : public Stream {
public:
    explicit CallbackStream(const ambixfile_stream& callbacks) : cb_(callbacks) {}
    std::size_t read(void* dst, std::size_t bytes) override { return cb_.read ? cb_.read(cb_.user, dst, bytes) : 0; }
    std::size_t write(const void* src, std::size_t bytes) override {
        return cb_.write ? cb_.write(cb_.user, src, bytes) : 0;
    }
    bool seek(std::int64_t position) override { return cb_.seek && cb_.seek(cb_.user, position) != 0; }
    std::int64_t tell() override { return cb_.tell ? cb_.tell(cb_.user) : -1; }
    std::int64_t size() override { return cb_.size ? cb_.size(cb_.user) : -1; }
    bool flush() override { return !cb_.flush || cb_.flush(cb_.user) != 0; }

private:
    ambixfile_stream cb_;
};

void setStatus(ambixfile_status* out, Status status) {
    if (out) *out = static_cast<ambixfile_status>(status);
}

void fillInfo(const Info& in, ambixfile_info* out) {
    if (!out) return;
    out->sample_rate = in.sampleRate;
    out->frames = in.frames;
    out->ambi_channels = in.ambiChannels;
    out->extra_channels = in.extraChannels;
    out->full_ambi_channels = in.fullAmbiChannels;
    out->sample_format = static_cast<int>(in.sampleFormat);
    out->file_format = static_cast<int>(in.fileFormat);
    out->container = static_cast<int>(in.container);
    out->big_endian = in.bigEndian ? 1 : 0;
    out->lossy = in.lossy ? 1 : 0;
    out->lossy_bits_per_sample = in.lossyBitsPerSample;
}

bool toConfig(const ambixfile_writer_config* in, WriterConfig& out) {
    if (!in) return false;
    if (in->sample_format < 0 || in->sample_format > static_cast<int>(SampleFormat::Float64)) return false;
    if (in->container < 0 || in->container > static_cast<int>(Container::WavPack)) return false;
    out.sampleRate = in->sample_rate;
    out.sampleFormat = static_cast<SampleFormat>(in->sample_format);
    out.bigEndian = in->big_endian != 0;
    out.ambiChannels = in->ambi_channels;
    out.extraChannels = in->extra_channels;
    if (in->adaptor_matrix) {
        const std::size_t count = static_cast<std::size_t>(in->adaptor_rows) * in->adaptor_cols;
        if (count == 0) return false;
        out.adaptorMatrix =
            Matrix(in->adaptor_rows, in->adaptor_cols, std::vector<float>(in->adaptor_matrix, in->adaptor_matrix + count));
    }
    out.container = static_cast<Container>(in->container);
    out.wavpackHybridBits = in->wavpack_hybrid_bits;
    out.wavpackThreads = in->wavpack_threads;
    return true;
}

std::size_t copyMatrix(const Matrix& m, float* values, std::size_t capacity, std::uint32_t* rows, std::uint32_t* cols) {
    const std::size_t count = static_cast<std::size_t>(m.rows()) * m.cols();
    if (rows) *rows = m.rows();
    if (cols) *cols = m.cols();
    if (count && values && capacity >= count) std::memcpy(values, m.data(), count * sizeof(float));
    return count;
}

// Run 'body', turning any exception (in practice: out of memory) into 'fallback'.
template <typename Result, typename Body>
Result guarded(Result fallback, Body body) noexcept {
    try {
        return body();
    } catch (...) {
        return fallback;
    }
}

} // namespace

extern "C" {

const char* ambixfile_status_string(ambixfile_status status) { return toString(static_cast<Status>(status)); }
int ambixfile_wavpack_available(void) { return wavpackAvailable() ? 1 : 0; }
uint32_t ambixfile_version(void) {
    return (AMBIXFILE_VERSION_MAJOR << 16) | (AMBIXFILE_VERSION_MINOR << 8) | AMBIXFILE_VERSION_PATCH;
}

// ---- reader ----

ambixfile_reader* ambixfile_reader_open(const char* utf8_path, int wavpack_threads, ambixfile_status* status) {
    setStatus(status, Status::InvalidArgument);
    if (!utf8_path) return nullptr;
    return guarded<ambixfile_reader*>(nullptr, [&]() -> ambixfile_reader* {
        std::unique_ptr<ambixfile_reader> handle(new ambixfile_reader);
        ReaderConfig config;
        config.wavpackThreads = wavpack_threads;
        const Status result = handle->reader.open(utf8_path, config);
        setStatus(status, result);
        return result == Status::Ok ? handle.release() : nullptr;
    });
}

ambixfile_reader* ambixfile_reader_open_stream(const ambixfile_stream* stream, int wavpack_threads,
                                               ambixfile_status* status) {
    setStatus(status, Status::InvalidArgument);
    if (!stream) return nullptr;
    return guarded<ambixfile_reader*>(nullptr, [&]() -> ambixfile_reader* {
        std::unique_ptr<ambixfile_reader> handle(new ambixfile_reader);
        ReaderConfig config;
        config.wavpackThreads = wavpack_threads;
        const Status result = handle->reader.open(std::unique_ptr<Stream>(new CallbackStream(*stream)), config);
        setStatus(status, result);
        return result == Status::Ok ? handle.release() : nullptr;
    });
}

void ambixfile_reader_close(ambixfile_reader* reader) { delete reader; }

void ambixfile_reader_get_info(const ambixfile_reader* reader, ambixfile_info* info) {
    if (reader) fillInfo(reader->reader.info(), info);
}

int ambixfile_reader_get_adaptor_matrix(const ambixfile_reader* reader, uint32_t* rows, uint32_t* cols,
                                        const float** values) {
    if (!reader || reader->reader.adaptorMatrix().empty()) return 0;
    const Matrix& m = reader->reader.adaptorMatrix();
    if (rows) *rows = m.rows();
    if (cols) *cols = m.cols();
    if (values) *values = m.data();
    return 1;
}

int64_t ambixfile_reader_read_float(ambixfile_reader* reader, float* interleaved, int64_t frames) {
    return reader ? reader->reader.read(interleaved, frames) : 0;
}
int64_t ambixfile_reader_read_double(ambixfile_reader* reader, double* interleaved, int64_t frames) {
    return reader ? reader->reader.read(interleaved, frames) : 0;
}
int64_t ambixfile_reader_read_ambisonics_float(ambixfile_reader* reader, float* ambi, float* extra, int64_t frames) {
    return reader ? reader->reader.readAmbisonics(ambi, extra, frames) : 0;
}
int64_t ambixfile_reader_read_ambisonics_double(ambixfile_reader* reader, double* ambi, double* extra,
                                                int64_t frames) {
    return reader ? reader->reader.readAmbisonics(ambi, extra, frames) : 0;
}
int ambixfile_reader_seek(ambixfile_reader* reader, uint64_t frame) {
    return reader && reader->reader.seek(frame) ? 1 : 0;
}
uint64_t ambixfile_reader_tell(const ambixfile_reader* reader) { return reader ? reader->reader.tell() : 0; }

uint32_t ambixfile_reader_marker_count(const ambixfile_reader* reader) {
    return reader ? static_cast<uint32_t>(reader->reader.markers().size()) : 0;
}
int ambixfile_reader_get_marker(const ambixfile_reader* reader, uint32_t index, double* position, const char** name) {
    if (!reader || index >= reader->reader.markers().size()) return 0;
    const Marker& marker = reader->reader.markers()[index];
    if (position) *position = marker.position;
    if (name) *name = marker.name.c_str();
    return 1;
}
uint32_t ambixfile_reader_region_count(const ambixfile_reader* reader) {
    return reader ? static_cast<uint32_t>(reader->reader.regions().size()) : 0;
}
int ambixfile_reader_get_region(const ambixfile_reader* reader, uint32_t index, double* start, double* end,
                                const char** name) {
    if (!reader || index >= reader->reader.regions().size()) return 0;
    const Region& region = reader->reader.regions()[index];
    if (start) *start = region.start;
    if (end) *end = region.end;
    if (name) *name = region.name.c_str();
    return 1;
}
uint32_t ambixfile_reader_chunk_count(const ambixfile_reader* reader) {
    return reader ? static_cast<uint32_t>(reader->reader.chunks().size()) : 0;
}
int ambixfile_reader_get_chunk(const ambixfile_reader* reader, uint32_t index, uint32_t* type, const uint8_t** data,
                               uint64_t* size) {
    if (!reader || index >= reader->reader.chunks().size()) return 0;
    const Chunk& chunk = reader->reader.chunks()[index];
    if (type) *type = chunk.type;
    if (data) *data = chunk.data.data();
    if (size) *size = chunk.data.size();
    return 1;
}

// ---- writer ----

void ambixfile_writer_config_init(ambixfile_writer_config* config) {
    if (!config) return;
    std::memset(config, 0, sizeof(*config));
    const WriterConfig defaults;
    config->sample_rate = defaults.sampleRate;
    config->sample_format = static_cast<int>(defaults.sampleFormat);
    config->big_endian = defaults.bigEndian ? 1 : 0;
    config->container = static_cast<int>(defaults.container);
}

ambixfile_writer* ambixfile_writer_open(const char* utf8_path, const ambixfile_writer_config* config,
                                        ambixfile_status* status) {
    setStatus(status, Status::InvalidArgument);
    if (!utf8_path) return nullptr;
    return guarded<ambixfile_writer*>(nullptr, [&]() -> ambixfile_writer* {
        WriterConfig converted;
        if (!toConfig(config, converted)) return nullptr;
        std::unique_ptr<ambixfile_writer> handle(new ambixfile_writer);
        const Status result = handle->writer.open(utf8_path, converted);
        setStatus(status, result);
        return result == Status::Ok ? handle.release() : nullptr;
    });
}

ambixfile_writer* ambixfile_writer_open_stream(const ambixfile_stream* stream, const ambixfile_writer_config* config,
                                               ambixfile_status* status) {
    setStatus(status, Status::InvalidArgument);
    if (!stream) return nullptr;
    return guarded<ambixfile_writer*>(nullptr, [&]() -> ambixfile_writer* {
        WriterConfig converted;
        if (!toConfig(config, converted)) return nullptr;
        std::unique_ptr<ambixfile_writer> handle(new ambixfile_writer);
        const Status result = handle->writer.open(std::unique_ptr<Stream>(new CallbackStream(*stream)), converted);
        setStatus(status, result);
        return result == Status::Ok ? handle.release() : nullptr;
    });
}

ambixfile_status ambixfile_writer_close(ambixfile_writer* writer) {
    if (!writer) return AMBIXFILE_NOT_OPEN;
    const Status status = guarded<Status>(Status::IoError, [&] { return writer->writer.close(); });
    delete writer;
    return static_cast<ambixfile_status>(status);
}

void ambixfile_writer_get_info(const ambixfile_writer* writer, ambixfile_info* info) {
    if (writer) fillInfo(writer->writer.info(), info);
}

int64_t ambixfile_writer_write_float(ambixfile_writer* writer, const float* interleaved, int64_t frames) {
    if (!writer) return 0;
    return guarded<int64_t>(0, [&] { return writer->writer.write(interleaved, frames); });
}
int64_t ambixfile_writer_write_double(ambixfile_writer* writer, const double* interleaved, int64_t frames) {
    if (!writer) return 0;
    return guarded<int64_t>(0, [&] { return writer->writer.write(interleaved, frames); });
}

ambixfile_status ambixfile_writer_add_marker(ambixfile_writer* writer, double position, const char* name) {
    if (!writer) return AMBIXFILE_NOT_OPEN;
    return static_cast<ambixfile_status>(guarded<Status>(Status::InvalidArgument, [&] {
        Marker marker;
        marker.position = position;
        if (name) marker.name = name;
        return writer->writer.addMarker(std::move(marker));
    }));
}

ambixfile_status ambixfile_writer_add_region(ambixfile_writer* writer, double start, double end, const char* name) {
    if (!writer) return AMBIXFILE_NOT_OPEN;
    return static_cast<ambixfile_status>(guarded<Status>(Status::InvalidArgument, [&] {
        Region region;
        region.start = start;
        region.end = end;
        if (name) region.name = name;
        return writer->writer.addRegion(std::move(region));
    }));
}

ambixfile_status ambixfile_writer_add_chunk(ambixfile_writer* writer, uint32_t type, const void* data, uint64_t size) {
    if (!writer) return AMBIXFILE_NOT_OPEN;
    if (!data && size) return AMBIXFILE_INVALID_ARGUMENT;
    return static_cast<ambixfile_status>(guarded<Status>(Status::InvalidArgument, [&] {
        Chunk chunk;
        chunk.type = type;
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
        chunk.data.assign(bytes, bytes + size);
        return writer->writer.addChunk(std::move(chunk));
    }));
}

// ---- matrices ----

size_t ambixfile_convention_matrix(ambixfile_convention convention, uint32_t order, float* values, size_t capacity) {
    if (order > 100) return 0;
    return guarded<size_t>(0, [&]() -> size_t {
        Matrix m;
        switch (convention) {
        case AMBIXFILE_N3D_TO_SN3D: m = n3dToSn3d(order); break;
        case AMBIXFILE_SN3D_TO_N3D: m = sn3dToN3d(order); break;
        case AMBIXFILE_SID_TO_ACN: m = sidToAcn(order); break;
        case AMBIXFILE_ACN_TO_SID: m = acnToSid(order); break;
        case AMBIXFILE_FUMA_TO_AMBIX: m = fumaToAmbix(order); break;
        case AMBIXFILE_AMBIX_TO_FUMA: m = ambixToFuma(order); break;
        default: return 0;
        }
        return copyMatrix(m, values, capacity, nullptr, nullptr);
    });
}

size_t ambixfile_fuma_channels_matrix(uint32_t fuma_channels, int to_fuma, float* values, size_t capacity,
                                      uint32_t* rows, uint32_t* cols) {
    return guarded<size_t>(0, [&] {
        const Matrix m = to_fuma ? ambixToFumaChannels(fuma_channels) : fumaChannelsToAmbix(fuma_channels);
        return copyMatrix(m, values, capacity, rows, cols);
    });
}

int ambixfile_matrix_pseudo_inverse(const float* values, uint32_t rows, uint32_t cols, float* out) {
    const std::size_t count = static_cast<std::size_t>(rows) * cols;
    if (!values || !out || count == 0) return 0;
    return guarded<int>(0, [&] {
        const Matrix m(rows, cols, std::vector<float>(values, values + count));
        Matrix inverse;
        if (!m.pseudoInverse(inverse)) return 0;
        std::memcpy(out, inverse.data(), count * sizeof(float));
        return 1;
    });
}

} // extern "C"
