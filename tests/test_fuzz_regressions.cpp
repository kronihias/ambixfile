// SPDX-License-Identifier: MIT
// Runs the fuzz target over inputs that once crashed it (tests/fuzz_regressions).
// Passing means not crashing; the sanitizer builds are where this bites.
#include "check.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
    CHECK(argc > 2);
    for (int i = 2; i < argc; ++i) {
        const std::string path = std::string(argv[1]) + "/" + argv[i];
        std::FILE* file = std::fopen(path.c_str(), "rb");
        CHECK(file != nullptr);
        if (!file) continue;
        std::vector<std::uint8_t> bytes;
        std::uint8_t block[4096];
        for (std::size_t n; (n = std::fread(block, 1, sizeof(block), file)) > 0;) bytes.insert(bytes.end(), block, block + n);
        std::fclose(file);
        CHECK(!bytes.empty());
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    }
    return finish("test_fuzz_regressions");
}
