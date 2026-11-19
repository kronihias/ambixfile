// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner

#include "internal.hpp"

#include <cstdio>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace ambixfile {

const char* toString(Status status) noexcept {
    switch (status) {
    case Status::Ok: return "ok";
    case Status::InvalidArgument: return "invalid argument";
    case Status::IoError: return "I/O error";
    case Status::NotCaf: return "not a CAF file";
    case Status::InvalidFile: return "damaged or truncated CAF file";
    case Status::UnsupportedFormat: return "unsupported audio format";
    case Status::InvalidAdaptorMatrix: return "invalid ambiX adaptor matrix";
    case Status::NotOpen: return "not open";
    }
    return "unknown status";
}

namespace detail {

class FileStream final : public Stream {
public:
    explicit FileStream(std::FILE* file) : file_(file) {}
    ~FileStream() override {
        if (file_) std::fclose(file_);
    }
    FileStream(const FileStream&) = delete;
    FileStream& operator=(const FileStream&) = delete;

    std::size_t read(void* dst, std::size_t bytes) override { return bytes ? std::fread(dst, 1, bytes, file_) : 0; }
    std::size_t write(const void* src, std::size_t bytes) override { return bytes ? std::fwrite(src, 1, bytes, file_) : 0; }
    bool seek(std::int64_t position) override {
#if defined(_WIN32)
        return _fseeki64(file_, position, SEEK_SET) == 0;
#else
        return fseeko(file_, static_cast<off_t>(position), SEEK_SET) == 0;
#endif
    }
    std::int64_t tell() override {
#if defined(_WIN32)
        return _ftelli64(file_);
#else
        return static_cast<std::int64_t>(ftello(file_));
#endif
    }
    std::int64_t size() override {
        const std::int64_t here = tell();
        if (here < 0) return -1;
#if defined(_WIN32)
        if (_fseeki64(file_, 0, SEEK_END) != 0) return -1;
#else
        if (fseeko(file_, 0, SEEK_END) != 0) return -1;
#endif
        const std::int64_t end = tell();
        return seek(here) ? end : -1;
    }
    bool flush() override { return std::fflush(file_) == 0; }

private:
    std::FILE* file_;
};

} // namespace detail

std::unique_ptr<Stream> openFileStream(const char* utf8Path, bool forWriting) {
    if (!utf8Path || !*utf8Path) return nullptr;
    std::FILE* file = nullptr;
#if defined(_WIN32)
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, nullptr, 0);
    if (length <= 0) return nullptr;
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, &wide[0], length);
    file = _wfopen(wide.c_str(), forWriting ? L"w+b" : L"rb");
#else
    file = std::fopen(utf8Path, forWriting ? "w+b" : "rb");
#endif
    if (!file) return nullptr;
    return std::unique_ptr<Stream>(new detail::FileStream(file));
}

std::size_t MemoryStream::read(void* dst, std::size_t bytes) {
    if (pos_ >= bytes_.size()) return 0;
    const std::size_t n = bytes < bytes_.size() - pos_ ? bytes : bytes_.size() - pos_;
    if (n == 0) return 0; // also keeps a null 'dst' away from memcpy
    std::memcpy(dst, bytes_.data() + pos_, n);
    pos_ += n;
    return n;
}

std::size_t MemoryStream::write(const void* src, std::size_t bytes) {
    if (bytes == 0) return 0; // also keeps a null 'src' away from memcpy
    if (pos_ + bytes > bytes_.size()) bytes_.resize(pos_ + bytes);
    std::memcpy(bytes_.data() + pos_, src, bytes);
    pos_ += bytes;
    return bytes;
}

bool MemoryStream::seek(std::int64_t position) {
    if (position < 0) return false;
    pos_ = static_cast<std::size_t>(position);
    return true;
}

std::int64_t MemoryStream::tell() { return static_cast<std::int64_t>(pos_); }
std::int64_t MemoryStream::size() { return static_cast<std::int64_t>(bytes_.size()); }

} // namespace ambixfile
