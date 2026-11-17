# Custom MemProf build-option mock

This is an illustrative build-time guard, not a Clang command-line option.
It does not guard the existing local MemProf extensions or change profiles.

For an LLVM build using LLVM_ENABLE_PROJECTS="clang;compiler-rt", configure:

```sh
cmake -S llvm -B /path/to/build -G Ninja \
  -DLLVM_ENABLE_PROJECTS="clang;compiler-rt" \
  -DLLVM_ENABLE_CUSTOM_MEMPROF=ON \
  -DCOMPILER_RT_ENABLE_CUSTOM_MEMPROF=ON
cmake --build /path/to/build --target clang llvm-profdata memprof
```

Use the same compiler/toolchain and other options as your normal build.
Both options default to OFF; explicitly set both OFF to disable the mock.
Enabled example files emit a compile-time message naming their component.

- LLVM_ENABLE_CUSTOM_MEMPROF defines LLVM_CUSTOM_MEMPROF privately on
  clangDriver and LLVMProfileData.
- COMPILER_RT_ENABLE_CUSTOM_MEMPROF adds LLVM_CUSTOM_MEMPROF to the common
  MemProf runtime definitions before the dynamic definitions are copied.
- The guarded examples live in CommonArgs.cpp, RawMemProfReader.cpp, and
  memprof_rtl.cpp. Replace the pragma message with your guarded code, retaining
  the original implementation in an #else branch when needed.

For standalone compiler-rt, pass COMPILER_RT_ENABLE_CUSTOM_MEMPROF=ON to its
own CMake invocation. For an external LLVM_ENABLE_RUNTIMES build, also forward
that option explicitly to the nested compiler-rt CMake configuration using
that build's runtime CMake arguments; it does not automatically inherit every
parent cache option. The example above uses an in-tree projects build instead.

This mock intentionally does not guard shared format headers, TableGen options,
architecture lists, or unit tests. When guarding real format changes, propagate
the same setting to every consumer, including tests compiling runtime sources.
Writer and reader formats must agree.
