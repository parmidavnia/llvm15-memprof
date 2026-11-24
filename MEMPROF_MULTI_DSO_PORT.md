# LLVM 15 multi-DSO MemProf port

This ports collection/processing support from commits 7073c1e8f5fd,
106c145cda9e, 66674d82da5d, and cb0cd014689e in the llvm-for-pr checkout.
The starting source tree is the private baseline a9e59df1a2b9 (identical to the
original local baseline 5dc64f474220).

## What changed

- RawMemProfReader owns a binary and symbolizer per input image, matches images
  to runtime executable segments using build IDs, and routes each PC accordingly.
- llvm-profdata merge/show accept repeated --profiled-binary arguments.
- Clang emits TU identity notes; LLD combines sorted TU identities into a stable
  DSO module ID. Native, full-LTO, and ThinLTO links use that identity scheme.
- Multi-binary profiles qualify function GUIDs with the stable module ID to
  distinguish identical function names across DSOs. Single-binary GUIDs remain
  compatible with the older name-only scheme.
- Indexed memory profiles can be inspected with llvm-profdata show --memory,
  retaining the LLVM 15 numeric frame prefix and record layout. An optional
  frame-payload suffix now preserves symbol names for standalone inspection.

The existing runtime, SIGUSR2 dump behavior, MIB extensions, and raw profile
layout are retained. Indexed frame payloads may now carry optional symbol names
following the unchanged numeric prefix. Keep the LLVM 15 runtime and tools paired; this
port does not make their raw format compatible with LLVM 24's runtime.

## LLVM 15 adaptations and scope

The old reader is RawMemProfReader.{h,cpp}, uses a local build-ID reader and
older ELF-note APIs, and stores half-open executable ranges. LLD uses global
linker state, llvm::Optional, and Writer.cpp for synthetic-section creation.
The mock-symbolizer test constructor is retained. Module-note errors explicitly
consume LLVM Error values, including successful ones, as required by LLVM 15.

As in the source patch, multi-binary inputs must contain a MemProf module ID:
rebuild profiled DSOs with the ported Clang and LLD. Supply both --build-id and
-z separate-code when linking. Only one executable PT_LOAD segment per binary
is supported. An unlisted image's frames are filtered out.

LLD generates module ID notes for shared objects, not executables. An ordinary
executable cannot be added to a multi-input list without further executable-ID
support. The regression deliberately supplies only the two DSOs.

LLVM 15 in this checkout has no MemProfUse pass. Therefore the newer patch's
MemProfUse changes and --lto-memprof-profile option are not transplanted: doing
so would require backporting the profile-use pass itself and its prerequisites.
This port supports collection, stack symbolization, merging, inspection, and
stable identities for profiled DSOs; it does not add compiler profile-use
optimization. The module-qualified GUID overload is available to custom consumers.

A module identity depends on the instrumented translation-unit filenames. Use
consistent source names and TU composition between corresponding builds. Default
ELF symbol interposition is unchanged; -Bsymbolic in the regression ensures both
duplicate implementations are actually exercised.

## Build (run manually)

No build was run while preparing this port. Existing build-memprof15 has Clang
and compiler-rt enabled but needs LLD added. Retain the rest of its cached options:

```sh
cmake -S /home/parmida/Desktop/tmpllvm/llvm-project/llvm \
  -B /home/parmida/Desktop/tmpllvm/build-memprof15 \
  -DLLVM_ENABLE_PROJECTS="clang;lld;compiler-rt"
ninja -C /home/parmida/Desktop/tmpllvm/build-memprof15 -j1 \
  clang lld llvm-profdata memprof
```

## Smoke test after the build

The standalone runner compiles a.o+b.o into lib1.so and c.o+d.o into lib2.so,
with shared_allocate defined in a.cpp and c.cpp. It checks both DSO stacks,
distinct duplicate-name GUIDs, exact allocation counts and sizes, indexed
roundtrip preservation, and argument-order independence. Requires PyYAML.
Use a fresh output directory on each run:

```sh
python3 /home/parmida/Desktop/tmpllvm/llvm-project/utils/memprof/test-multiple-dsos.py \
  --tool-dir /home/parmida/Desktop/tmpllvm/build-memprof15/bin \
  --output-dir /tmp/memprof15-multi-dso-smoke
```

Lit tests are also provided for Clang TU notes, LLD native/LTO IDs, and the
runtime two-DSO flow (including duplicate-binary and missing-note rejection).
ProfileData unit tests cover module-qualified GUIDs and indexed roundtrip.
The existing build has LLVM_INCLUDE_TESTS and COMPILER_RT_INCLUDE_TESTS OFF;
these tests require enabling those options and building the relevant test tools.

Validation: the manually built toolchain passed native, full-LTO, and ThinLTO
multi-DSO profile checks, plus the complex workload's 14 stack checks and exact
allocation totals. Runtime startup is still intermittent: two of five additional
runs failed an AddrIsInMem assertion in memprof_allocator.cpp before main. The
cause has not been established. This comment-only update does not address that
failure; no LLVM build was run for the documentation changes.

## Reading the implementation

Follow the data through these layers:

1. `clang/lib/CodeGen/CodeGenModule.cpp` knows the source translation unit and
   emits its identity note. `llvm/BinaryFormat/ELF.h` is the common note-tag
   contract used by the producer, linker, and reader.
2. LLD's `SyntheticSections` helpers collect those identities. `LTO` preserves
   the original input composition before optimization; `Driver` exposes that
   saved value; `Writer` chooses it or the native fallback and emits one DSO note.
3. `RawMemProfReader` uses build IDs to locate runtime image ranges and module
   IDs to distinguish function names. Those are different jobs: a build ID
   identifies a particular binary build, while a module ID depends on source
   composition. Per-module symbolizers translate addresses to source frames.
4. The `IndexedMemProfRecord::getGUID` overload in `MemProf` defines the shared
   module-qualified key policy. The existing writer stores those keys and frames.
5. `llvm-profdata` forwards a borrowed list of image paths only to raw MemProf
   decoding. `mergeInstrProfile` remains the common merge coordinator; ordinary
   instrumentation PGO uses its existing counter reader and merge operations.
6. `InstrProfReader::printMemProfYAML` exposes already-indexed records for
   inspection, without re-symbolization. FrameWriterTrait and FrameLookupTrait
   preserve optional names in the indexed payload, after its numeric prefix.

Tests follow the same boundaries: CodeGen checks TU production, LLD native/LTO
checks identity composition, ProfileData unit tests check hashing and serialized
records, and the runtime/standalone regressions connect all of those layers.
Inline comments explain the declarations and handoffs at their actual locations.

## Symbol names in merged profiles

Raw merges request names from the symbolizer and retain them on the frames
passed to the writer. The frame hash-table payload contains its original
17-byte numeric prefix followed by the optional name bytes. The table's existing
payload length bounds the name; no new length field, runtime format, or ordinary
PGO record change is needed. Hashing/equality still depend only on numeric frame
identity. When writer contexts combine, an existing name is retained and a
missing name can be filled from the other context.

This is a local indexed-MemProf payload extension. This updated reader also
accepts legacy numeric-only frames, displaying `<None>` for them. The original
LLVM 15 frame lookup reads the unchanged prefix and ignores a suffix because the
table already carries payload lengths. Compatibility with unrelated newer LLVM
MemProf formats is not implied. Existing indexed files have already lost their
names: regenerate them from raw profiles with the updated llvm-profdata.

The standalone smoke test now compares symbol names as well as full frame
identities and allocation counts/sizes after merge -> show. Unit tests cover old
numeric-only frames, bounded name decoding, duplicate names under different
module GUIDs, and combining named/unnamed copies of a frame.
