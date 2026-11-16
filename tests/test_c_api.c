/* SPDX-License-Identifier: MIT */
/* The C API from a plain C99 program. */
#include "ambixfile/ambixfile.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition);       \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

/* A fixed-size in-memory stream for the callback interface. */
typedef struct memory {
    unsigned char bytes[1 << 16];
    size_t size, pos;
} memory;

static size_t mem_read(void* user, void* dst, size_t n) {
    memory* m = (memory*)user;
    if (m->pos >= m->size) return 0;
    if (n > m->size - m->pos) n = m->size - m->pos;
    memcpy(dst, m->bytes + m->pos, n);
    m->pos += n;
    return n;
}
static size_t mem_write(void* user, const void* src, size_t n) {
    memory* m = (memory*)user;
    if (m->pos + n > sizeof(m->bytes)) return 0;
    memcpy(m->bytes + m->pos, src, n);
    m->pos += n;
    if (m->pos > m->size) m->size = m->pos;
    return n;
}
static int mem_seek(void* user, int64_t position) {
    memory* m = (memory*)user;
    if (position < 0 || (size_t)position > sizeof(m->bytes)) return 0;
    m->pos = (size_t)position;
    return 1;
}
static int64_t mem_tell(void* user) { return (int64_t)((memory*)user)->pos; }
static int64_t mem_size(void* user) { return (int64_t)((memory*)user)->size; }

static memory storage;

static void test_file(const char* dir) {
    char path[1024];
    float matrix[9 * 5];
    uint32_t rows = 0, cols = 0;
    float audio[6 * 256], back[6 * 256], ambi[9 * 256], extra[256];
    ambixfile_writer_config config;
    ambixfile_status status = AMBIXFILE_IO_ERROR;
    ambixfile_writer* writer;
    ambixfile_reader* reader;
    ambixfile_info info;
    const float* values = NULL;
    const char* name = NULL;
    double a = 0, b = 0;
    int i;

    snprintf(path, sizeof(path), "%s/ambixfile_c_api.caf", dir);
    for (i = 0; i < 6 * 256; ++i) audio[i] = (float)sin(0.01 * i);

    /* FuMa WXYUV plus one extra channel, stored unchanged with an adaptor matrix. */
    CHECK(ambixfile_fuma_channels_matrix(5, 0, NULL, 0, &rows, &cols) == 45 && rows == 9 && cols == 5);
    CHECK(ambixfile_fuma_channels_matrix(5, 0, matrix, 45, NULL, NULL) == 45);
    CHECK(ambixfile_fuma_channels_matrix(10, 0, matrix, 45, NULL, NULL) == 0);

    ambixfile_writer_config_init(&config);
    CHECK(config.sample_rate == 48000.0 && config.big_endian == 1);
    CHECK(ambixfile_writer_open(path, &config, &status) == NULL && status == AMBIXFILE_INVALID_ARGUMENT);
    config.sample_format = AMBIXFILE_INT24;
    config.ambi_channels = 5;
    config.extra_channels = 1;
    config.adaptor_matrix = matrix;
    config.adaptor_rows = rows;
    config.adaptor_cols = cols;
    writer = ambixfile_writer_open(path, &config, &status);
    CHECK(writer != NULL && status == AMBIXFILE_OK);
    if (!writer) return;
    CHECK(ambixfile_writer_add_marker(writer, 42.0, "cue") == AMBIXFILE_OK);
    CHECK(ambixfile_writer_add_region(writer, 1.0, 99.0, NULL) == AMBIXFILE_OK);
    CHECK(ambixfile_writer_add_chunk(writer, 0x74657374u, "abc", 3) == AMBIXFILE_OK);
    CHECK(ambixfile_writer_write_float(writer, audio, 256) == 256);
    ambixfile_writer_get_info(writer, &info);
    CHECK(info.frames == 256 && info.file_format == AMBIXFILE_FORMAT_EXTENDED);
    CHECK(ambixfile_writer_close(writer) == AMBIXFILE_OK);
    CHECK(ambixfile_writer_close(NULL) == AMBIXFILE_NOT_OPEN);

    reader = ambixfile_reader_open(path, 0, &status);
    CHECK(reader != NULL && status == AMBIXFILE_OK);
    if (!reader) return;
    memset(&info, 0, sizeof(info));
    ambixfile_reader_get_info(reader, &info);
    CHECK(info.sample_rate == 48000.0 && info.frames == 256 && info.sample_format == AMBIXFILE_INT24);
    CHECK(info.ambi_channels == 5 && info.extra_channels == 1 && info.full_ambi_channels == 9);
    CHECK(info.file_format == AMBIXFILE_FORMAT_EXTENDED && info.container == AMBIXFILE_CONTAINER_CAF);
    CHECK(ambixfile_reader_get_adaptor_matrix(reader, &rows, &cols, &values) && rows == 9 && cols == 5);
    CHECK(values && memcmp(values, matrix, sizeof(matrix)) == 0);

    CHECK(ambixfile_reader_marker_count(reader) == 1 && ambixfile_reader_region_count(reader) == 1);
    CHECK(ambixfile_reader_get_marker(reader, 0, &a, &name) && a == 42.0 && strcmp(name, "cue") == 0);
    CHECK(ambixfile_reader_get_region(reader, 0, &a, &b, &name) && a == 1.0 && b == 99.0 && name[0] == '\0');
    CHECK(!ambixfile_reader_get_marker(reader, 1, NULL, NULL));
    {
        uint32_t type = 0;
        const uint8_t* data = NULL;
        uint64_t size = 0;
        CHECK(ambixfile_reader_chunk_count(reader) == 1);
        CHECK(ambixfile_reader_get_chunk(reader, 0, &type, &data, &size) && type == 0x74657374u && size == 3);
        CHECK(data && memcmp(data, "abc", 3) == 0);
    }

    CHECK(ambixfile_reader_read_float(reader, back, 300) == 256);
    for (i = 0; i < 6 * 256; ++i)
        if (fabsf(back[i] - audio[i]) > 1.0f / 8388608.0f) break;
    CHECK(i == 6 * 256);
    CHECK(ambixfile_reader_seek(reader, 0) && ambixfile_reader_tell(reader) == 0);
    CHECK(ambixfile_reader_read_ambisonics_float(reader, ambi, extra, 256) == 256);
    CHECK(fabsf(ambi[0] - back[0] * sqrtf(2.0f)) < 1e-6f); /* W gets +3 dB */
    CHECK(extra[10] == back[10 * 6 + 5]);
    ambixfile_reader_close(reader);
    remove(path);

    CHECK(ambixfile_reader_open(path, 0, &status) == NULL && status == AMBIXFILE_IO_ERROR);
    CHECK(strcmp(ambixfile_status_string(AMBIXFILE_NOT_CAF), "not a CAF file") == 0);
}

static void test_stream(void) {
    ambixfile_stream stream;
    ambixfile_writer_config config;
    ambixfile_status status;
    ambixfile_writer* writer;
    ambixfile_reader* reader;
    ambixfile_info info;
    double frames[4 * 8], back[4 * 8];
    int i;

    stream.user = &storage;
    stream.read = mem_read;
    stream.write = mem_write;
    stream.seek = mem_seek;
    stream.tell = mem_tell;
    stream.size = mem_size;
    stream.flush = NULL;
    for (i = 0; i < 32; ++i) frames[i] = i / 64.0;

    ambixfile_writer_config_init(&config);
    config.sample_format = AMBIXFILE_FLOAT64;
    config.ambi_channels = 4;
    writer = ambixfile_writer_open_stream(&stream, &config, &status);
    CHECK(writer != NULL);
    if (!writer) return;
    CHECK(ambixfile_writer_write_double(writer, frames, 8) == 8);
    CHECK(ambixfile_writer_close(writer) == AMBIXFILE_OK);
    CHECK(storage.size > 8 && memcmp(storage.bytes, "caff", 4) == 0);

    reader = ambixfile_reader_open_stream(&stream, 0, &status);
    CHECK(reader != NULL);
    if (!reader) return;
    ambixfile_reader_get_info(reader, &info);
    CHECK(info.frames == 8 && info.file_format == AMBIXFILE_FORMAT_BASIC && info.sample_format == AMBIXFILE_FLOAT64);
    CHECK(ambixfile_reader_read_double(reader, back, 8) == 8);
    CHECK(memcmp(frames, back, sizeof(frames)) == 0);
    ambixfile_reader_close(reader);

    storage.size = 16;
    memset(storage.bytes, 'x', 16);
    CHECK(ambixfile_reader_open_stream(&stream, 0, &status) == NULL && status == AMBIXFILE_NOT_CAF);
}

static void test_matrices(void) {
    float m[16], inverse[16], wide[6] = {1, 2, 3, 4, 5, 6}, pinv[6];
    CHECK(ambixfile_convention_matrix(AMBIXFILE_FUMA_TO_AMBIX, 1, NULL, 0) == 16);
    CHECK(ambixfile_convention_matrix(AMBIXFILE_FUMA_TO_AMBIX, 1, m, 16) == 16);
    CHECK(fabsf(m[0] - sqrtf(2.0f)) < 1e-6f && m[4 + 2] == 1.0f); /* W gain; ACN 1 (Y) from FuMa channel 2 */
    CHECK(ambixfile_convention_matrix(AMBIXFILE_AMBIX_TO_FUMA, 1, inverse, 16) == 16);
    CHECK(ambixfile_convention_matrix(AMBIXFILE_FUMA_TO_AMBIX, 4, m, 16) == 0);
    CHECK(ambixfile_convention_matrix(AMBIXFILE_N3D_TO_SN3D, 1, m, 16) == 16 && fabsf(m[5] - 1.0f / sqrtf(3.0f)) < 1e-6f);
    CHECK(ambixfile_matrix_pseudo_inverse(wide, 2, 3, pinv));
    CHECK(fabsf(wide[0] * pinv[0] + wide[1] * pinv[2] + wide[2] * pinv[4] - 1.0f) < 1e-4f);
    CHECK(!ambixfile_matrix_pseudo_inverse(wide, 0, 3, pinv));
    CHECK(ambixfile_version() >= 0x000200u);
    CHECK(ambixfile_wavpack_available() == 0 || ambixfile_wavpack_available() == 1);
}

int main(int argc, char** argv) {
    test_file(argc > 1 ? argv[1] : ".");
    test_stream();
    test_matrices();
    if (failures == 0) printf("test_c_api: all checks passed\n");
    else printf("test_c_api: %d check(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
