// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Matthias Kronlachner

#include "internal.hpp"

namespace ambixfile {

Matrix::Matrix(std::uint32_t rows, std::uint32_t cols)
    : rows_(rows), cols_(cols), values_(static_cast<std::size_t>(rows) * cols, 0.0f) {
    if (values_.empty()) rows_ = cols_ = 0;
}

Matrix::Matrix(std::uint32_t rows, std::uint32_t cols, std::vector<float> values) {
    if (!values.empty() && values.size() == static_cast<std::size_t>(rows) * cols) {
        rows_ = rows;
        cols_ = cols;
        values_ = std::move(values);
    }
}

Matrix Matrix::identity(std::uint32_t size) {
    Matrix m(size, size);
    for (std::uint32_t i = 0; i < size; ++i) m(i, i) = 1.0f;
    return m;
}

Matrix Matrix::transposed() const {
    Matrix t(cols_, rows_);
    for (std::uint32_t r = 0; r < rows_; ++r)
        for (std::uint32_t c = 0; c < cols_; ++c) t(c, r) = (*this)(r, c);
    return t;
}

Matrix Matrix::multiplied(const Matrix& rhs) const {
    if (empty() || rhs.empty() || cols_ != rhs.rows_) return Matrix();
    Matrix out(rows_, rhs.cols_);
    for (std::uint32_t r = 0; r < rows_; ++r)
        for (std::uint32_t c = 0; c < rhs.cols_; ++c) {
            double sum = 0.0;
            for (std::uint32_t k = 0; k < cols_; ++k)
                sum += static_cast<double>((*this)(r, k)) * static_cast<double>(rhs(k, c));
            out(r, c) = static_cast<float>(sum);
        }
    return out;
}

bool Matrix::isIdentity(float tolerance) const noexcept {
    if (empty() || rows_ != cols_) return false;
    for (std::uint32_t r = 0; r < rows_; ++r)
        for (std::uint32_t c = 0; c < cols_; ++c)
            if (std::fabs((*this)(r, c) - (r == c ? 1.0f : 0.0f)) > tolerance) return false;
    return true;
}

bool Matrix::approxEquals(const Matrix& other, float tolerance) const noexcept {
    if (rows_ != other.rows_ || cols_ != other.cols_) return false;
    for (std::size_t i = 0; i < values_.size(); ++i)
        if (!(std::fabs(values_[i] - other.values_[i]) <= tolerance)) return false;
    return true;
}

namespace detail {

// In-place Gauss-Jordan inversion with partial pivoting of an n x n row-major
// matrix. Returns false if the matrix is (numerically) singular.
inline bool invertSquare(std::vector<double>& a, std::uint32_t n) {
    double scale = 0.0;
    for (double v : a) scale = std::fabs(v) > scale ? std::fabs(v) : scale;
    if (!(scale > 0.0) || !std::isfinite(scale)) return false;
    const double tiny = scale * 1e-11;

    std::vector<double> inv(static_cast<std::size_t>(n) * n, 0.0);
    for (std::uint32_t i = 0; i < n; ++i) inv[static_cast<std::size_t>(i) * n + i] = 1.0;

    for (std::uint32_t col = 0; col < n; ++col) {
        std::uint32_t pivot = col;
        for (std::uint32_t r = col + 1; r < n; ++r)
            if (std::fabs(a[static_cast<std::size_t>(r) * n + col]) >
                std::fabs(a[static_cast<std::size_t>(pivot) * n + col]))
                pivot = r;
        if (std::fabs(a[static_cast<std::size_t>(pivot) * n + col]) <= tiny) return false;
        if (pivot != col)
            for (std::uint32_t k = 0; k < n; ++k) {
                std::swap(a[static_cast<std::size_t>(pivot) * n + k], a[static_cast<std::size_t>(col) * n + k]);
                std::swap(inv[static_cast<std::size_t>(pivot) * n + k], inv[static_cast<std::size_t>(col) * n + k]);
            }
        const double d = 1.0 / a[static_cast<std::size_t>(col) * n + col];
        for (std::uint32_t k = 0; k < n; ++k) {
            a[static_cast<std::size_t>(col) * n + k] *= d;
            inv[static_cast<std::size_t>(col) * n + k] *= d;
        }
        for (std::uint32_t r = 0; r < n; ++r) {
            if (r == col) continue;
            const double f = a[static_cast<std::size_t>(r) * n + col];
            if (f == 0.0) continue;
            for (std::uint32_t k = 0; k < n; ++k) {
                a[static_cast<std::size_t>(r) * n + k] -= f * a[static_cast<std::size_t>(col) * n + k];
                inv[static_cast<std::size_t>(r) * n + k] -= f * inv[static_cast<std::size_t>(col) * n + k];
            }
        }
    }
    a.swap(inv);
    return true;
}

// Degree l of the spherical harmonic at ACN index 'acn'.
inline std::uint32_t acnDegree(std::uint32_t acn) noexcept {
    std::uint32_t l = 0;
    while ((l + 1) * (l + 1) <= acn) ++l;
    return l;
}

// SID index of the harmonic at ACN index 'acn'. Within a degree l the SID
// sequence is m = +l, -l, +(l-1), -(l-1), ..., +1, -1, 0.
inline std::uint32_t acnToSidIndex(std::uint32_t acn) noexcept {
    const std::uint32_t l = acnDegree(acn);
    const std::int64_t m = static_cast<std::int64_t>(acn) - static_cast<std::int64_t>(l) * l - l;
    const std::uint32_t absM = static_cast<std::uint32_t>(m < 0 ? -m : m);
    const std::uint32_t offset = m == 0 ? 2 * l : 2 * (l - absM) + (m < 0 ? 1u : 0u);
    return l * l + offset;
}

// Furse-Malham channel index (W X Y Z R S T U V K L M N O P Q) per ACN index.
constexpr std::uint32_t kAcnToFumaIndex[16] = {0, 2, 3, 1, 8, 6, 4, 5, 7, 15, 13, 11, 9, 10, 12, 14};

// Gain that turns an SN3D signal into the Furse-Malham (maxN, W at -3 dB)
// weighting, per Furse-Malham channel index.
inline double fumaGain(std::uint32_t fumaIndex) noexcept {
    switch (fumaIndex) {
    case 0: return std::sqrt(0.5);                    // W
    case 5: case 6: case 7: case 8: return 2.0 / std::sqrt(3.0);   // S T U V
    case 10: case 11: return std::sqrt(45.0 / 32.0);  // L M
    case 12: case 13: return 3.0 / std::sqrt(5.0);    // N O
    case 14: case 15: return std::sqrt(8.0 / 5.0);    // P Q
    default: return 1.0;                              // X Y Z R K
    }
}

// Furse-Malham channel indices of the reduced sets used by .amb files.
// Returns the number of channels in the set, or 0 if there is none of that size.
inline std::uint32_t fumaChannelSet(std::uint32_t channels, const std::uint32_t*& set, std::uint32_t& order) noexcept {
    static const std::uint32_t all[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static const std::uint32_t h1[3] = {0, 1, 2};                            // WXY
    static const std::uint32_t h2[5] = {0, 1, 2, 7, 8};                      // WXYUV
    static const std::uint32_t h2v1[6] = {0, 1, 2, 3, 7, 8};                 // WXYZUV
    static const std::uint32_t h3[7] = {0, 1, 2, 7, 8, 14, 15};              // WXYUVPQ
    static const std::uint32_t h3v1[8] = {0, 1, 2, 3, 7, 8, 14, 15};         // WXYZUVPQ
    static const std::uint32_t h3v2[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 14, 15}; // WXYZRSTUVPQ
    switch (channels) {
    case 1: set = all; order = 0; return 1;
    case 3: set = h1; order = 1; return 3;
    case 4: set = all; order = 1; return 4;
    case 5: set = h2; order = 2; return 5;
    case 6: set = h2v1; order = 2; return 6;
    case 7: set = h3; order = 3; return 7;
    case 8: set = h3v1; order = 3; return 8;
    case 9: set = all; order = 2; return 9;
    case 11: set = h3v2; order = 3; return 11;
    case 16: set = all; order = 3; return 16;
    default: return 0;
    }
}

} // namespace detail

bool Matrix::pseudoInverse(Matrix& out) const {
    if (empty()) return false;
    const bool tall = rows_ >= cols_;
    const std::uint32_t n = tall ? cols_ : rows_;
    // Gram matrix: A^T A for tall matrices, A A^T for wide ones.
    std::vector<double> gram(static_cast<std::size_t>(n) * n, 0.0);
    for (std::uint32_t i = 0; i < n; ++i)
        for (std::uint32_t j = i; j < n; ++j) {
            double sum = 0.0;
            if (tall)
                for (std::uint32_t k = 0; k < rows_; ++k)
                    sum += static_cast<double>((*this)(k, i)) * static_cast<double>((*this)(k, j));
            else
                for (std::uint32_t k = 0; k < cols_; ++k)
                    sum += static_cast<double>((*this)(i, k)) * static_cast<double>((*this)(j, k));
            gram[static_cast<std::size_t>(i) * n + j] = sum;
            gram[static_cast<std::size_t>(j) * n + i] = sum;
        }
    if (!detail::invertSquare(gram, n)) return false;

    Matrix result(cols_, rows_);
    for (std::uint32_t r = 0; r < cols_; ++r)
        for (std::uint32_t c = 0; c < rows_; ++c) {
            double sum = 0.0;
            if (tall) // (A^T A)^-1 A^T
                for (std::uint32_t k = 0; k < n; ++k)
                    sum += gram[static_cast<std::size_t>(r) * n + k] * static_cast<double>((*this)(c, k));
            else // A^T (A A^T)^-1
                for (std::uint32_t k = 0; k < n; ++k)
                    sum += static_cast<double>((*this)(k, r)) * gram[static_cast<std::size_t>(k) * n + c];
            result(r, c) = static_cast<float>(sum);
        }
    out = std::move(result);
    return true;
}

std::uint32_t orderToChannels(std::uint32_t order) noexcept { return (order + 1) * (order + 1); }

bool channelsToOrder(std::uint32_t channels, std::uint32_t& order) noexcept {
    if (channels == 0) return false;
    const std::uint32_t root = static_cast<std::uint32_t>(std::lround(std::sqrt(static_cast<double>(channels))));
    if (static_cast<std::uint64_t>(root) * root != channels) return false;
    order = root - 1;
    return true;
}

Matrix n3dToSn3d(std::uint32_t order) {
    const std::uint32_t n = orderToChannels(order);
    Matrix m(n, n);
    for (std::uint32_t i = 0; i < n; ++i)
        m(i, i) = static_cast<float>(1.0 / std::sqrt(2.0 * detail::acnDegree(i) + 1.0));
    return m;
}

Matrix sn3dToN3d(std::uint32_t order) {
    const std::uint32_t n = orderToChannels(order);
    Matrix m(n, n);
    for (std::uint32_t i = 0; i < n; ++i)
        m(i, i) = static_cast<float>(std::sqrt(2.0 * detail::acnDegree(i) + 1.0));
    return m;
}

Matrix sidToAcn(std::uint32_t order) {
    const std::uint32_t n = orderToChannels(order);
    Matrix m(n, n);
    for (std::uint32_t acn = 0; acn < n; ++acn) m(acn, detail::acnToSidIndex(acn)) = 1.0f;
    return m;
}

Matrix acnToSid(std::uint32_t order) { return sidToAcn(order).transposed(); }

Matrix fumaToAmbix(std::uint32_t order) {
    if (order > 3) return Matrix();
    const std::uint32_t n = orderToChannels(order);
    Matrix m(n, n);
    for (std::uint32_t acn = 0; acn < n; ++acn) {
        const std::uint32_t fuma = detail::kAcnToFumaIndex[acn];
        m(acn, fuma) = static_cast<float>(1.0 / detail::fumaGain(fuma));
    }
    return m;
}

Matrix ambixToFuma(std::uint32_t order) {
    if (order > 3) return Matrix();
    const std::uint32_t n = orderToChannels(order);
    Matrix m(n, n);
    for (std::uint32_t acn = 0; acn < n; ++acn) {
        const std::uint32_t fuma = detail::kAcnToFumaIndex[acn];
        m(fuma, acn) = static_cast<float>(detail::fumaGain(fuma));
    }
    return m;
}

Matrix fumaChannelsToAmbix(std::uint32_t fumaChannels) {
    const std::uint32_t* set = nullptr;
    std::uint32_t order = 0;
    const std::uint32_t count = detail::fumaChannelSet(fumaChannels, set, order);
    if (!count) return Matrix();
    const std::uint32_t full = orderToChannels(order);
    Matrix m(full, count);
    for (std::uint32_t acn = 0; acn < full; ++acn)
        for (std::uint32_t col = 0; col < count; ++col)
            if (set[col] == detail::kAcnToFumaIndex[acn])
                m(acn, col) = static_cast<float>(1.0 / detail::fumaGain(set[col]));
    return m;
}

Matrix ambixToFumaChannels(std::uint32_t fumaChannels) {
    const std::uint32_t* set = nullptr;
    std::uint32_t order = 0;
    const std::uint32_t count = detail::fumaChannelSet(fumaChannels, set, order);
    if (!count) return Matrix();
    const std::uint32_t full = orderToChannels(order);
    Matrix m(count, full);
    for (std::uint32_t acn = 0; acn < full; ++acn)
        for (std::uint32_t row = 0; row < count; ++row)
            if (set[row] == detail::kAcnToFumaIndex[acn]) m(row, acn) = static_cast<float>(detail::fumaGain(set[row]));
    return m;
}

} // namespace ambixfile
