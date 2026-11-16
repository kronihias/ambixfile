// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner
//
// ambixfile - read and write ambiX (Ambisonics exchange) files.
//
// ambiX files are Core Audio Format (CAF) files holding Ambisonics signals in
// ACN channel order with SN3D normalisation. "Basic" files hold a full 3D set
// of (N+1)^2 channels. "Extended" files carry an adaptor matrix that expands a
// reduced channel set to the full set, and may hold extra non-Ambisonics
// channels after the Ambisonics ones.
//
// No exceptions are thrown to report errors; functions return Status values.
// The read() and write() calls do not allocate.

#ifndef AMBIXFILE_AMBIXFILE_HPP
#define AMBIXFILE_AMBIXFILE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#define AMBIXFILE_VERSION_MAJOR 0
#define AMBIXFILE_VERSION_MINOR 1
#define AMBIXFILE_VERSION_PATCH 0

namespace ambixfile {

// ---------------------------------------------------------------- status ---

enum class Status {
    Ok = 0,
    InvalidArgument,      // a parameter or configuration is not usable
    IoError,              // the stream failed to read, write or seek
    NotCaf,               // the stream does not start with a CAF header
    InvalidFile,          // the CAF structure is damaged or truncated
    UnsupportedFormat,    // valid CAF, but not linear PCM we can decode
    InvalidAdaptorMatrix, // the ambiX adaptor matrix does not fit the file
    NotOpen               // the reader or writer is not open
};

const char* toString(Status status) noexcept;

// --------------------------------------------------------------- streams ---

// Byte stream used for all file access. Implement this to read from or write
// to something other than a plain file.
class Stream {
public:
    virtual ~Stream() = default;
    // Return the number of bytes actually transferred.
    virtual std::size_t read(void* dst, std::size_t bytes) = 0;
    virtual std::size_t write(const void* src, std::size_t bytes) = 0;
    // Seek to an absolute byte position.
    virtual bool seek(std::int64_t position) = 0;
    virtual std::int64_t tell() = 0;
    // Total size in bytes, or -1 if unknown.
    virtual std::int64_t size() = 0;
    // Push buffered output to its destination. Called when a Writer closes.
    virtual bool flush() { return true; }
};

// Open a file on disk. The path is UTF-8 on every platform.
// Returns null on failure.
std::unique_ptr<Stream> openFileStream(const char* utf8Path, bool forWriting);

// Growable in-memory stream.
class MemoryStream final : public Stream {
public:
    MemoryStream() = default;
    explicit MemoryStream(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}

    std::size_t read(void* dst, std::size_t bytes) override;
    std::size_t write(const void* src, std::size_t bytes) override;
    bool seek(std::int64_t position) override;
    std::int64_t tell() override;
    std::int64_t size() override;

    const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
    std::vector<std::uint8_t>& bytes() noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t pos_ = 0;
};

// ---------------------------------------------------------------- matrix ---

// Dense row-major float matrix.
class Matrix {
public:
    Matrix() = default;
    Matrix(std::uint32_t rows, std::uint32_t cols); // zero filled
    // 'values' is row-major. If its size is not rows*cols the result is empty.
    Matrix(std::uint32_t rows, std::uint32_t cols, std::vector<float> values);

    static Matrix identity(std::uint32_t size);

    std::uint32_t rows() const noexcept { return rows_; }
    std::uint32_t cols() const noexcept { return cols_; }
    bool empty() const noexcept { return values_.empty(); }

    float& operator()(std::uint32_t row, std::uint32_t col) noexcept {
        return values_[static_cast<std::size_t>(row) * cols_ + col];
    }
    float operator()(std::uint32_t row, std::uint32_t col) const noexcept {
        return values_[static_cast<std::size_t>(row) * cols_ + col];
    }
    const float* data() const noexcept { return values_.data(); }
    float* data() noexcept { return values_.data(); }

    Matrix transposed() const;
    // Matrix product. Returns an empty matrix if the sizes do not match.
    Matrix multiplied(const Matrix& rhs) const;
    // Moore-Penrose pseudo-inverse of a full-rank matrix. Returns false (and
    // leaves 'out' untouched) if the matrix is empty or rank deficient.
    bool pseudoInverse(Matrix& out) const;
    bool isIdentity(float tolerance = 1e-6f) const noexcept;
    bool approxEquals(const Matrix& other, float tolerance = 1e-6f) const noexcept;

private:
    std::uint32_t rows_ = 0;
    std::uint32_t cols_ = 0;
    std::vector<float> values_;
};

// ----------------------------------------------------------- conventions ---

// Number of channels of a full 3D set: (order + 1)^2.
std::uint32_t orderToChannels(std::uint32_t order) noexcept;
// Sets 'order' and returns true if 'channels' is a full 3D set.
bool channelsToOrder(std::uint32_t channels, std::uint32_t& order) noexcept;

// Adaptor matrices between ambiX (ACN order, SN3D) and other conventions.
// Each returns a square matrix for a full 3D set of the given order. Apply as
// out = M * in, or store an "...ToAmbix" matrix as a file's adaptor matrix.
Matrix n3dToSn3d(std::uint32_t order);
Matrix sn3dToN3d(std::uint32_t order);
// SID channel order (Daniel), e.g. W X Y Z for first order.
Matrix sidToAcn(std::uint32_t order);
Matrix acnToSid(std::uint32_t order);
// Furse-Malham (channel order W X Y Z R S T U V K L M N O P Q, maxN weights).
// Defined up to third order; returns an empty matrix above that.
Matrix fumaToAmbix(std::uint32_t order);
Matrix ambixToFuma(std::uint32_t order);

// ------------------------------------------------------------- file info ---

enum class SampleFormat { Int16, Int24, Int32, Float32, Float64 };

enum class FileFormat {
    None,    // a CAF file that is not valid ambiX (still readable as audio)
    Basic,   // full 3D set, no adaptor matrix
    Extended // adaptor matrix present
};

struct Info {
    double sampleRate = 0.0;
    SampleFormat sampleFormat = SampleFormat::Float32;
    bool bigEndian = false;
    FileFormat fileFormat = FileFormat::None;
    // Ambisonics channels stored in the file (the adaptor matrix's columns).
    std::uint32_t ambiChannels = 0;
    // Non-Ambisonics channels stored after the Ambisonics channels.
    std::uint32_t extraChannels = 0;
    // Ambisonics channels after applying the adaptor matrix (its rows).
    // Equal to ambiChannels for basic files.
    std::uint32_t fullAmbiChannels = 0;
    std::uint64_t frames = 0;

    std::uint32_t fileChannels() const noexcept { return ambiChannels + extraChannels; }
};

constexpr std::uint32_t fourCC(char a, char b, char c, char d) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d));
}

// A CAF chunk other than the audio description, the audio data and the ambiX
// adaptor matrix. 'data' is the raw chunk payload (big-endian per CAF).
struct Chunk {
    std::uint32_t type = 0;
    std::vector<std::uint8_t> data;
};

// ---------------------------------------------------------------- reader ---

class Reader {
public:
    Reader();
    ~Reader();
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;

    Status open(const char* utf8Path);
    Status open(std::unique_ptr<Stream> stream);
    // The stream must outlive the reader (or its close()).
    Status open(Stream& stream);
    void close();
    bool isOpen() const noexcept;

    const Info& info() const noexcept;
    // Empty for files without an adaptor matrix.
    const Matrix& adaptorMatrix() const noexcept;
    const std::vector<Chunk>& chunks() const noexcept;

    // Read the channels as stored: interleaved, info().fileChannels() wide.
    // Returns the number of frames read (less than asked at end of file).
    std::int64_t read(float* interleaved, std::int64_t frames);
    std::int64_t read(double* interleaved, std::int64_t frames);

    // Read with the adaptor matrix applied. 'ambi' receives
    // info().fullAmbiChannels interleaved channels, 'extra' receives
    // info().extraChannels interleaved channels. Either may be null.
    std::int64_t readAmbisonics(float* ambi, float* extra, std::int64_t frames);
    std::int64_t readAmbisonics(double* ambi, double* extra, std::int64_t frames);

    bool seek(std::uint64_t frame);
    std::uint64_t tell() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------- writer ---

struct WriterConfig {
    double sampleRate = 48000.0;
    SampleFormat sampleFormat = SampleFormat::Float32;
    // Byte order of the audio samples. Big-endian is what ambiX tools expect;
    // little-endian is valid CAF but some ambiX readers (including libambix's
    // built-in CAF reader) cannot decode it.
    bool bigEndian = true;
    // Ambisonics channels to store in the file.
    std::uint32_t ambiChannels = 0;
    // Non-Ambisonics channels stored after them (extended files only).
    std::uint32_t extraChannels = 0;
    // Leave empty for a basic file: ambiChannels must then be (N+1)^2 and
    // extraChannels 0. Otherwise the file is extended: the matrix must have
    // ambiChannels columns and (N+1)^2 rows. As a convenience, an empty matrix
    // with extraChannels > 0 and a full set writes an identity matrix.
    Matrix adaptorMatrix;
    // Additional chunks to write before the audio data.
    std::vector<Chunk> chunks;
};

class Writer {
public:
    Writer();
    ~Writer(); // closes the file if still open
    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;

    Status open(const char* utf8Path, const WriterConfig& config);
    Status open(std::unique_ptr<Stream> stream, const WriterConfig& config);
    // The stream must outlive the writer (or its close()).
    Status open(Stream& stream, const WriterConfig& config);
    bool isOpen() const noexcept;

    // The resulting layout, including the file format that will be written.
    const Info& info() const noexcept;

    // Write interleaved frames, info().fileChannels() wide. Integer formats
    // clip at full scale. Returns the number of frames written.
    std::int64_t write(const float* interleaved, std::int64_t frames);
    std::int64_t write(const double* interleaved, std::int64_t frames);

    // Queue a chunk to be written after the audio data when the file is
    // closed (for metadata only known at the end, such as markers).
    Status addTrailingChunk(Chunk chunk);

    // Finish the file. Returns IoError if the stream could not be completed.
    Status close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ambixfile

#endif // AMBIXFILE_AMBIXFILE_HPP
