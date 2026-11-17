// REQUIRES: x86_64-linux, memprof-dynamic-runtime, lld-available

// Only the two shared libraries are MemProf-instrumented. -Bsymbolic ensures
// each DSO calls its own externally visible shared_allocate definition.
// RUN: split-file %s %t
// RUN: %clangxx_memprof -O1 -g -fPIC -c %t/a.cpp -o %t/a.o
// RUN: %clangxx_memprof -O1 -g -fPIC -c %t/b.cpp -o %t/b.o
// RUN: %clangxx_memprof -O1 -g -fPIC -shared -fuse-ld=lld -Wl,--build-id \
// RUN:     -Wl,-z,separate-code -Wl,-Bsymbolic %t/a.o %t/b.o -o %t.lib1.so
// RUN: %clangxx_memprof -O1 -g -fPIC -c %t/c.cpp -o %t/c.o
// RUN: %clangxx_memprof -O1 -g -fPIC -c %t/d.cpp -o %t/d.o
// RUN: %clangxx_memprof -O1 -g -fPIC -shared -fuse-ld=lld -Wl,--build-id \
// RUN:     -Wl,-z,separate-code -Wl,-Bsymbolic %t/c.o %t/d.o -o %t.lib2.so
// RUN: %clangxx -O1 %t/main.cpp %t.lib1.so %t.lib2.so -Wl,-rpath,%T \
// RUN:     -o %t.main
// RUN: rm -f %t.raw.*
// RUN: env MEMPROF_OPTIONS=log_path=%t.raw LD_PRELOAD=%shared_libmemprof \
// RUN:     %run %t.main
// RUN: cp %t.lib1.so %t.duplicate.so
// RUN: not llvm-profdata show --memory %t.raw.* \
// RUN:     --profiled-binary %t.lib1.so \
// RUN:     --profiled-binary %t.duplicate.so -o /dev/null 2>&1 | \
// RUN:     FileCheck %s --check-prefix=DUPLICATE
// RUN: llvm-profdata show --memory %t.raw.* \
// RUN:     --profiled-binary %t.lib1.so --profiled-binary %t.lib2.so \
// RUN:     -o %t.yaml
// RUN: FileCheck %s --check-prefix=RAW < %t.yaml
// RUN: llvm-profdata show --memory %t.raw.* --profiled-binary %t.lib2.so \
// RUN:     --profiled-binary %t.lib1.so -o %t.reversed.yaml
// RUN: cmp %t.yaml %t.reversed.yaml
// RUN: llvm-objcopy --remove-section=.note.llvm.memprof.module-id \
// RUN:     %t.lib1.so %t.no-module.so
// RUN: not llvm-profdata show --memory %t.raw.* \
// RUN:     --profiled-binary %t.no-module.so --profiled-binary %t.lib2.so \
// RUN:     -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSING-ID
// RUN: llvm-profdata merge %t.raw.* --profiled-binary %t.lib1.so \
// RUN:     --profiled-binary %t.lib2.so -o %t.memprofdata
// RUN: llvm-profdata show --memory %t.memprofdata | \
// RUN:     FileCheck %s --check-prefix=INDEXED

// The shared_allocate linkage name is identical in both DSOs. Two allocation
// functions proves their module-qualified GUIDs did not collapse into one key.
// RAW:      NumMibInfo: 2
// RAW-NEXT: NumAllocFunctions: 2
// RAW-DAG:  SymbolName: shared_allocate
// RAW-DAG:  TotalSize: 111
// RAW-DAG:  SymbolName: shared_allocate
// RAW-DAG:  TotalSize: 222

// INDEXED: NumMibInfo: 2
// INDEXED-NEXT: NumAllocFunctions: 2
// INDEXED-DAG: TotalSize: 111
// INDEXED-DAG: TotalSize: 222

// MISSING-ID: error: No MemProf module ID found in binary
// DUPLICATE: error: Duplicate profiled binary build id:

//--- main.cpp

extern "C" void lib1_entry();
extern "C" void lib2_entry();

int main() {
  lib1_entry();
  lib2_entry();
}

//--- a.cpp

#include <cstdlib>

extern "C" __attribute__((noinline)) void shared_allocate() {
  volatile char *P = static_cast<char *>(std::malloc(111));
  for (int I = 0; I != 111; ++I)
    P[I] = static_cast<char>(I);
  std::free(const_cast<char *>(P));
}

//--- b.cpp
extern "C" void shared_allocate();
extern "C" __attribute__((noinline)) void lib1_entry() { shared_allocate(); }

//--- c.cpp

#include <cstdlib>

extern "C" __attribute__((noinline)) void shared_allocate() {
  volatile char *P = static_cast<char *>(std::malloc(222));
  for (int I = 0; I != 222; ++I)
    P[I] = static_cast<char>(I);
  std::free(const_cast<char *>(P));
}

//--- d.cpp
extern "C" void shared_allocate();
extern "C" __attribute__((noinline)) void lib2_entry() { shared_allocate(); }
