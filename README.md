# ambixfile

A small C++17 library for reading and writing **ambiX** files, the Ambisonics
exchange format: Core Audio Format (CAF) files holding Ambisonics signals in
ACN channel order with SN3D normalisation.

- MIT licensed, usable in open and closed source projects
- no dependencies beyond the C++ standard library (no libsndfile)
- basic and extended ambiX files, including adaptor matrices and extra channels
- 16/24/32-bit integer and 32/64-bit float samples, either byte order
- file access through a small `Stream` interface (files, memory, or your own)
- no exceptions for error reporting; `read()`/`write()` do not allocate
- adaptor matrices for N3D, SID and Furse-Malham (FuMa) material

Status: early. The API may still change. See [Roadmap](#roadmap).

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
`config.adaptorMatrix = ambixfile::fumaToAmbix(1);`

## Building and testing

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

An optional test checks interoperability with
[libambix](https://github.com/iem-projects/libambix) in both directions. Point
`-DAMBIXFILE_LIBAMBIX_DIR=<libambix source tree>` at a checkout with a CMake
build to enable it. libambix is used only by that test and is not part of this
library.

## Roadmap

- markers and regions (CAF `mark`/`regn`/`strg` chunks); today these are
  available as raw chunks through `Reader::chunks()`
- WavPack-compressed ambiX as an optional backend
- adaptor matrices for reduced FuMa channel sets
- a C API for bindings to other languages

## Licence

MIT, see [LICENSE](LICENSE). Written from the published ambiX and CAF format
descriptions; it contains no code from libambix.
