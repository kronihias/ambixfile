// SPDX-License-Identifier: MIT
#include "ambixfile/ambixfile.hpp"
#include "check.hpp"

#include <cmath>

using namespace ambixfile;

int main() {
    // construction
    CHECK(Matrix().empty());
    CHECK(Matrix(2, 3, {1, 2, 3}).empty()); // wrong value count
    const Matrix a(2, 3, {1, 2, 3, 4, 5, 6});
    CHECK(a.rows() == 2 && a.cols() == 3 && a(1, 0) == 4.0f);
    CHECK(Matrix::identity(4).isIdentity());
    CHECK(!a.isIdentity());

    // transpose and product
    const Matrix at = a.transposed();
    CHECK(at.rows() == 3 && at.cols() == 2 && at(2, 1) == 6.0f);
    CHECK(a.multiplied(at).approxEquals(Matrix(2, 2, {14, 32, 32, 77})));
    CHECK(a.multiplied(a).empty()); // size mismatch

    // pseudo-inverse: wide, tall and square
    Matrix pinv;
    CHECK(a.pseudoInverse(pinv));
    CHECK(pinv.rows() == 3 && pinv.cols() == 2);
    CHECK(a.multiplied(pinv).isIdentity(1e-4f));
    CHECK(a.multiplied(pinv).multiplied(a).approxEquals(a, 1e-4f));
    Matrix pinvT;
    CHECK(at.pseudoInverse(pinvT));
    CHECK(pinvT.multiplied(at).isIdentity(1e-4f));
    const Matrix sq(3, 3, {2, 0, 1, 1, 3, 2, 1, 0, 0});
    Matrix inv;
    CHECK(sq.pseudoInverse(inv));
    CHECK(sq.multiplied(inv).isIdentity(1e-5f));
    CHECK(inv.multiplied(sq).isIdentity(1e-5f));
    // rank deficient
    Matrix unused;
    CHECK(!Matrix(2, 2, {1, 2, 2, 4}).pseudoInverse(unused));
    CHECK(!Matrix(2, 2).pseudoInverse(unused));
    CHECK(!Matrix().pseudoInverse(unused));

    // orders
    std::uint32_t order = 99;
    CHECK(orderToChannels(0) == 1 && orderToChannels(3) == 16 && orderToChannels(7) == 64);
    CHECK(channelsToOrder(16, order) && order == 3);
    CHECK(channelsToOrder(1, order) && order == 0);
    CHECK(!channelsToOrder(0, order) && !channelsToOrder(15, order));

    // normalisation
    const Matrix n2s = n3dToSn3d(2);
    CHECK(n2s.rows() == 9);
    CHECK(std::fabs(n2s(0, 0) - 1.0f) < 1e-6f);
    CHECK(std::fabs(n2s(3, 3) - 1.0f / std::sqrt(3.0f)) < 1e-6f);
    CHECK(std::fabs(n2s(8, 8) - 1.0f / std::sqrt(5.0f)) < 1e-6f);
    CHECK(n2s.multiplied(sn3dToN3d(2)).isIdentity(1e-6f));

    // SID: first order is W X Y Z, ACN is W Y Z X
    const Matrix sid = sidToAcn(1);
    CHECK(sid.approxEquals(Matrix(4, 4, {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0})));
    CHECK(sidToAcn(3).multiplied(acnToSid(3)).isIdentity());
    // second order SID: W X Y Z U V S T R; ACN 4..8 is V T R S U
    const Matrix sid2 = sidToAcn(2);
    CHECK(sid2(4, 5) == 1.0f && sid2(5, 7) == 1.0f && sid2(6, 8) == 1.0f && sid2(7, 6) == 1.0f && sid2(8, 4) == 1.0f);

    // Furse-Malham
    const Matrix f1 = fumaToAmbix(1);
    const float r2 = std::sqrt(2.0f);
    CHECK(f1.approxEquals(Matrix(4, 4, {r2, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0})));
    for (std::uint32_t o = 0; o <= 3; ++o) {
        CHECK(fumaToAmbix(o).multiplied(ambixToFuma(o)).isIdentity(1e-6f));
        CHECK(ambixToFuma(o).multiplied(fumaToAmbix(o)).isIdentity(1e-6f));
    }
    const Matrix f3 = ambixToFuma(3);
    CHECK(std::fabs(f3(4, 6) - 1.0f) < 1e-6f);                       // R from ACN 6
    CHECK(std::fabs(f3(8, 4) - 2.0f / std::sqrt(3.0f)) < 1e-6f);     // V from ACN 4
    CHECK(std::fabs(f3(9, 12) - 1.0f) < 1e-6f);                      // K from ACN 12
    CHECK(std::fabs(f3(15, 9) - std::sqrt(8.0f / 5.0f)) < 1e-6f);    // Q from ACN 9
    CHECK(fumaToAmbix(4).empty());

    return finish("test_matrix");
}
