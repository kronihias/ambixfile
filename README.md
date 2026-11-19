# ambixfile

A small C++17 library for reading and writing **ambiX** files, the Ambisonics
exchange format: Core Audio Format (CAF) files holding Ambisonics signals in
ACN channel order with SN3D normalisation.

- MIT licensed, usable in open and closed source projects
- no dependencies beyond the C++ standard library (no libsndfile)
- basic and extended ambiX files, including adaptor matrices and extra channels
- 16/24/32-bit integer and 32/64-bit float samples, either byte order; read and
  written from `float`, `double`, `int16_t` or `int32_t` buffers
- markers and regions with names
- optional WavPack container (lossless or hybrid lossy), auto-detected on read
- a C API for C programs and language bindings
- file access through a small `Stream` interface (files, memory, or your own)
- no exceptions for error reporting; `read()`/`write()` do not allocate
- adaptor matrices for N3D, SID and Furse-Malham (FuMa) material, including
  the reduced FuMa channel sets of `.amb` files

Status: early. The API may still change.

## Using it

With CMake:

```cmake
add_subdirectory(ambixfile)
target_link_libraries(your_target PRIVATE ambixfile::ambixfile)
```

Or as a single header: run `python3 scripts/amalgamate.py` to generate
`single_include/ambixfile.hpp`, copy it into your project, and in exactly one
source file write:

```cpp
#define AMBIXFILE_IMPLEMENTATION
#include "ambixfile.hpp"
```

## Reading

```cpp
#include <ambixfile/ambixfile.hpp>

ambixfile::Reader reader;
if (reader.open("recording.caf") != ambixfile::Status::Ok) return;

const ambixfile::Info& info = reader.info();
std::vector<float> ambi(info.fullAmbiChannels * 1024);
std::vector<float> extra(info.extraChannels * 1024);

// Full Ambisonics set (adaptor matrix applied) and the extra channels:
while (reader.readAmbisonics(ambi.data(), extra.data(), 1024) > 0) { /* ... */ }
```

`reader.read()` returns the channels exactly as stored instead, and
`reader.adaptorMatrix()` gives the matrix.

## Writing

```cpp
ambixfile::WriterConfig config;
config.sampleRate = 48000.0;
config.sampleFormat = ambixfile::SampleFormat::Int24;
config.ambiChannels = 16; // third order, basic format

ambixfile::Writer writer;
if (writer.open("out.caf", config) != ambixfile::Status::Ok) return;
writer.write(interleaved, frames);
writer.close();
```

Set `config.adaptorMatrix` (and optionally `config.extraChannels`) to write an
extended file. For example, to store B-format in FuMa order unchanged:
`config.adaptorMatrix = ambixfile::fumaToAmbix(1);`, or for a five-channel
horizontal `.amb` set (WXYUV): `ambixfile::fumaChannelsToAmbix(5)`.

## Markers and regions

```cpp
writer.addMarker({48000.0, "verse"});          // position in frames
writer.addRegion({0.0, 96000.0, "intro"});     // start, end

for (const ambixfile::Marker& m : reader.markers()) { /* m.position, m.name */ }
```

Metadata added before the first `write()` is stored ahead of the audio, where
every ambiX reader finds it. Metadata added later, for example markers set
during a recording, is stored after the audio when the file is closed; some
readers (libambix among them) do not look there.

## Reduced channel sets

An extended file can store fewer channels than the full set, with an adaptor
matrix that expands them. The library has the matrices from the ambiX paper's
examples:

```cpp
Matrix horizontal = horizontalToAmbix(3);       // 7 channels, 2D material
Matrix mixed      = mixedOrderToAmbix(3, 1);    // 3H1V: 8 channels
Matrix hemisphere = hemisphereToAmbix(3);       // 10 channels, orders 1 to 7

WriterConfig config;
config.adaptorMatrix = hemisphere;
config.ambiChannels  = hemisphere.cols();
// To reduce a full set for storing: stored = hemisphere.transposed() * full
// (all three have orthonormal columns).
```

`mixedOrderOf()` recognises a horizontal-only or mixed-order matrix in a file.
`scripts/generate_adaptor_matrices.py` (needs numpy) prints the same matrices
as text or CSV, computes hemisphere matrices for other orders and cap sizes
(`--el-min`), and regenerates the table the library ships.

## Command-line tool

`ambixfile-info [-m] file...` prints a file's container, format, channels,
adaptor matrix (`-m` for its values), markers, regions and other chunks.

## WavPack

Build with `-DAMBIXFILE_ENABLE_WAVPACK=ON` (CMake) or define
`AMBIXFILE_ENABLE_WAVPACK` (single header) and link
[WavPack](https://www.wavpack.com) 5. CMake uses an existing `wavpack` target,
an installed WavPack, or a source tree given as `-DAMBIXFILE_WAVPACK_DIR=...`.

```cpp
config.container = ambixfile::Container::WavPack;
config.wavpackHybridBits = 0.0f; // lossless; 2.0 to 23.9 selects lossy mode
```

The reader recognises WavPack files by itself. The ambiX metadata (adaptor
matrix, markers, other chunks) is kept in the WavPack wrapper as the CAF
header the uncompressed file would have, the same layout libambix uses.
WavPack stores 64-bit float as 32-bit float and cannot hold a file with no
audio at all.

## C API

`#include <ambixfile/ambixfile.h>` and link the library (the C API is not part
of the single header). It mirrors the C++ API:

```c
ambixfile_status status;
ambixfile_reader* reader = ambixfile_reader_open("recording.caf", 0, &status);
if (!reader) { puts(ambixfile_status_string(status)); return; }
ambixfile_info info;
ambixfile_reader_get_info(reader, &info);
/* ambixfile_reader_read_ambisonics_float(reader, ambi, extra, frames) ... */
ambixfile_reader_close(reader);
```

## Building and testing

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

An optional test checks interoperability with
[libambix](https://github.com/iem-projects/libambix) in both directions:
audio, adaptor matrices, markers, regions, the convention matrices and (when
enabled) WavPack files. Point `-DAMBIXFILE_LIBAMBIX_DIR=<libambix source tree>`
at a checkout with a CMake build to enable it. libambix is used only by that
test and is not part of this library.

### Fuzzing

The reader is fuzzed with libFuzzer (Clang):

```sh
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang \
      -DAMBIXFILE_BUILD_FUZZERS=ON -DAMBIXFILE_BUILD_TESTS=OFF \
      "-DCMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link,address,undefined"
cmake --build build-fuzz
mkdir corpus && build-fuzz/make_seeds corpus
build-fuzz/fuzz_reader corpus -max_len=4096
```

CI runs it for a minute on every change. Inputs that once crashed it live in
`tests/fuzz_regressions` and run as a normal test.

### Installing

`cmake --install build` installs the library, the headers and
`ambixfile-info`. For builds without WavPack it also installs a CMake package
(`find_package(ambixfile)`, target `ambixfile::ambixfile`).

## Format reference

The ambiX format is defined in

> C. Nachbar, F. Zotter, E. Deleflie, A. Sontacchi: *ambiX - A Suggested
> Ambisonics Format*. Ambisonics Symposium 2011, Lexington, KY.
> <https://ambisonics.iem.at/proceedings-of-the-ambisonics-symposium-2011/ambix-a-suggested-ambisonics-format>

The container is Apple's Core Audio Format. Two things here go beyond the
paper: 64-bit float samples (the paper names 16/24/32-bit integer and 32-bit
float) and the markers and regions, which are plain CAF chunks.

## Licence

MIT, see [LICENSE](LICENSE). Written from the published ambiX and CAF format
descriptions; it contains no code from libambix. WavPack, if you enable it, is
a separate library under the BSD 3-clause licence.
