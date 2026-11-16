/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Matthias Kronlachner
 *
 * ambixfile - C API.
 *
 * A thin C layer over the C++ library (ambixfile.hpp), for C programs and for
 * bindings to other languages. See ambixfile.hpp for the full description of
 * the concepts; the functions here map one-to-one onto it.
 *
 * Conventions:
 *  - Functions returning ambixfile_status return AMBIXFILE_OK (0) on success.
 *  - Pointers handed out by a reader (names, matrix, chunk data) stay valid
 *    until that reader is closed.
 *  - All paths and names are UTF-8.
 */

#ifndef AMBIXFILE_AMBIXFILE_H
#define AMBIXFILE_AMBIXFILE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ambixfile_status {
    AMBIXFILE_OK = 0,
    AMBIXFILE_INVALID_ARGUMENT,
    AMBIXFILE_IO_ERROR,
    AMBIXFILE_NOT_CAF,
    AMBIXFILE_INVALID_FILE,
    AMBIXFILE_UNSUPPORTED_FORMAT,
    AMBIXFILE_INVALID_ADAPTOR_MATRIX,
    AMBIXFILE_NOT_OPEN
} ambixfile_status;

typedef enum ambixfile_sample_format {
    AMBIXFILE_INT16 = 0,
    AMBIXFILE_INT24,
    AMBIXFILE_INT32,
    AMBIXFILE_FLOAT32,
    AMBIXFILE_FLOAT64
} ambixfile_sample_format;

typedef enum ambixfile_file_format {
    AMBIXFILE_FORMAT_NONE = 0, /* readable audio, but not valid ambiX */
    AMBIXFILE_FORMAT_BASIC,    /* full 3D set, no adaptor matrix */
    AMBIXFILE_FORMAT_EXTENDED  /* adaptor matrix present */
} ambixfile_file_format;

typedef enum ambixfile_container {
    AMBIXFILE_CONTAINER_CAF = 0,
    AMBIXFILE_CONTAINER_WAVPACK
} ambixfile_container;

typedef struct ambixfile_info {
    double sample_rate;
    uint64_t frames;
    uint32_t ambi_channels;      /* Ambisonics channels stored in the file */
    uint32_t extra_channels;     /* other channels stored after them */
    uint32_t full_ambi_channels; /* Ambisonics channels after the adaptor matrix */
    int sample_format;           /* ambixfile_sample_format */
    int file_format;             /* ambixfile_file_format */
    int container;               /* ambixfile_container */
    int big_endian;
    int lossy;
} ambixfile_info;

/* Custom byte stream. 'read', 'seek', 'tell' and 'size' are needed for
 * reading; 'write' and 'seek' for writing. 'size' may return -1 if unknown.
 * 'seek' and 'flush' return non-zero on success. 'flush' may be NULL. The
 * struct is copied; 'user' must stay valid until the reader/writer is closed. */
typedef struct ambixfile_stream {
    void* user;
    size_t (*read)(void* user, void* dst, size_t bytes);
    size_t (*write)(void* user, const void* src, size_t bytes);
    int (*seek)(void* user, int64_t position);
    int64_t (*tell)(void* user);
    int64_t (*size)(void* user);
    int (*flush)(void* user);
} ambixfile_stream;

const char* ambixfile_status_string(ambixfile_status status);
/* Non-zero if the library was built with WavPack support. */
int ambixfile_wavpack_available(void);
uint32_t ambixfile_version(void); /* (major << 16) | (minor << 8) | patch */

/* ------------------------------------------------------------- reader --- */

typedef struct ambixfile_reader ambixfile_reader;

/* Return NULL on failure; '*status' (if not NULL) receives the reason.
 * 'wavpack_threads' is the number of worker threads WavPack may use (0: none). */
ambixfile_reader* ambixfile_reader_open(const char* utf8_path, int wavpack_threads, ambixfile_status* status);
ambixfile_reader* ambixfile_reader_open_stream(const ambixfile_stream* stream, int wavpack_threads,
                                               ambixfile_status* status);
void ambixfile_reader_close(ambixfile_reader* reader);

void ambixfile_reader_get_info(const ambixfile_reader* reader, ambixfile_info* info);
/* Returns non-zero and fills the outputs if the file has an adaptor matrix.
 * '*values' is row-major, rows * cols floats. */
int ambixfile_reader_get_adaptor_matrix(const ambixfile_reader* reader, uint32_t* rows, uint32_t* cols,
                                        const float** values);

/* Channels as stored: interleaved, ambi_channels + extra_channels wide.
 * Return the number of frames read. */
int64_t ambixfile_reader_read_float(ambixfile_reader* reader, float* interleaved, int64_t frames);
int64_t ambixfile_reader_read_double(ambixfile_reader* reader, double* interleaved, int64_t frames);
/* With the adaptor matrix applied: 'ambi' gets full_ambi_channels, 'extra'
 * gets extra_channels interleaved channels. Either may be NULL. */
int64_t ambixfile_reader_read_ambisonics_float(ambixfile_reader* reader, float* ambi, float* extra, int64_t frames);
int64_t ambixfile_reader_read_ambisonics_double(ambixfile_reader* reader, double* ambi, double* extra,
                                                int64_t frames);
int ambixfile_reader_seek(ambixfile_reader* reader, uint64_t frame); /* non-zero on success */
uint64_t ambixfile_reader_tell(const ambixfile_reader* reader);

uint32_t ambixfile_reader_marker_count(const ambixfile_reader* reader);
/* Return non-zero if 'index' is valid. Outputs may be NULL. */
int ambixfile_reader_get_marker(const ambixfile_reader* reader, uint32_t index, double* position, const char** name);
uint32_t ambixfile_reader_region_count(const ambixfile_reader* reader);
int ambixfile_reader_get_region(const ambixfile_reader* reader, uint32_t index, double* start, double* end,
                                const char** name);
uint32_t ambixfile_reader_chunk_count(const ambixfile_reader* reader);
int ambixfile_reader_get_chunk(const ambixfile_reader* reader, uint32_t index, uint32_t* type, const uint8_t** data,
                               uint64_t* size);

/* ------------------------------------------------------------- writer --- */

typedef struct ambixfile_writer_config {
    double sample_rate;
    int sample_format; /* ambixfile_sample_format */
    int big_endian;    /* CAF sample byte order; keep 1 for compatibility */
    uint32_t ambi_channels;
    uint32_t extra_channels;
    /* NULL for a basic file, else a row-major adaptor_rows x adaptor_cols
     * matrix with adaptor_cols == ambi_channels. */
    const float* adaptor_matrix;
    uint32_t adaptor_rows;
    uint32_t adaptor_cols;
    int container;             /* ambixfile_container */
    float wavpack_hybrid_bits; /* 0: lossless, 2.0 .. 23.9: lossy */
    int wavpack_threads;
} ambixfile_writer_config;

/* Set the defaults: 48 kHz, float32, big-endian, CAF, lossless, no channels. */
void ambixfile_writer_config_init(ambixfile_writer_config* config);

typedef struct ambixfile_writer ambixfile_writer;

ambixfile_writer* ambixfile_writer_open(const char* utf8_path, const ambixfile_writer_config* config,
                                        ambixfile_status* status);
ambixfile_writer* ambixfile_writer_open_stream(const ambixfile_stream* stream, const ambixfile_writer_config* config,
                                               ambixfile_status* status);
/* Finishes the file and frees the writer. */
ambixfile_status ambixfile_writer_close(ambixfile_writer* writer);

void ambixfile_writer_get_info(const ambixfile_writer* writer, ambixfile_info* info);
/* Interleaved frames, ambi_channels + extra_channels wide. Return the number
 * of frames written. */
int64_t ambixfile_writer_write_float(ambixfile_writer* writer, const float* interleaved, int64_t frames);
int64_t ambixfile_writer_write_double(ambixfile_writer* writer, const double* interleaved, int64_t frames);
/* Metadata added before the first write is stored ahead of the audio (the
 * most compatible place), anything later after the audio. */
ambixfile_status ambixfile_writer_add_marker(ambixfile_writer* writer, double position, const char* name);
ambixfile_status ambixfile_writer_add_region(ambixfile_writer* writer, double start, double end, const char* name);
ambixfile_status ambixfile_writer_add_chunk(ambixfile_writer* writer, uint32_t type, const void* data, uint64_t size);

/* ----------------------------------------------------------- matrices --- */

typedef enum ambixfile_convention {
    AMBIXFILE_N3D_TO_SN3D = 0,
    AMBIXFILE_SN3D_TO_N3D,
    AMBIXFILE_SID_TO_ACN,
    AMBIXFILE_ACN_TO_SID,
    AMBIXFILE_FUMA_TO_AMBIX, /* full sets, up to third order */
    AMBIXFILE_AMBIX_TO_FUMA
} ambixfile_convention;

/* Square conversion matrix for a full set of the given order. Writes the
 * row-major (order+1)^2 x (order+1)^2 values to 'values' if 'capacity' (in
 * floats) is large enough. Returns the number of floats needed, or 0 if the
 * conversion is not defined for that order. */
size_t ambixfile_convention_matrix(ambixfile_convention convention, uint32_t order, float* values, size_t capacity);

/* Adaptor matrix expanding a Furse-Malham channel set (1, 3, 4, 5, 6, 7, 8,
 * 9, 11 or 16 channels, as in .amb files) to full ambiX; with 'to_fuma'
 * non-zero, the reverse. Same return convention as above; '*rows' and '*cols'
 * (may be NULL) receive the shape. */
size_t ambixfile_fuma_channels_matrix(uint32_t fuma_channels, int to_fuma, float* values, size_t capacity,
                                      uint32_t* rows, uint32_t* cols);

/* Pseudo-inverse of a full-rank rows x cols matrix; 'out' receives cols x
 * rows floats. Returns non-zero on success. */
int ambixfile_matrix_pseudo_inverse(const float* values, uint32_t rows, uint32_t cols, float* out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AMBIXFILE_AMBIXFILE_H */
