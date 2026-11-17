; REQUIRES: x86
; RUN: split-file %s %t
; RUN: llvm-as %t/a.ll -o %t/a.bc
; RUN: llvm-as %t/b.ll -o %t/b.bc
; RUN: ld.lld -shared %t/a.bc %t/b.bc -o %t/full.so
; RUN: ld.lld -shared %t/b.bc %t/a.bc -o %t/reversed.so
; RUN: opt -module-summary %t/a.ll -o %t/a.thin.bc
; RUN: opt -module-summary %t/b.ll -o %t/b.thin.bc
; RUN: ld.lld -shared %t/a.thin.bc %t/b.thin.bc -o %t/thin.so
; RUN: llvm-readelf --notes %t/full.so | FileCheck %s
; RUN: llvm-readelf --notes %t/thin.so | FileCheck %s
; RUN: llvm-objcopy --dump-section .note.llvm.memprof.module-id=%t/full.note %t/full.so
; RUN: llvm-objcopy --dump-section .note.llvm.memprof.module-id=%t/reversed.note %t/reversed.so
; RUN: llvm-objcopy --dump-section .note.llvm.memprof.module-id=%t/thin.note %t/thin.so
; RUN: cmp %t/full.note %t/reversed.note
; RUN: cmp %t/full.note %t/thin.note
; CHECK: Displaying notes found in: .note.llvm.memprof.module-id
; CHECK: LLVM
; CHECK-SAME: 0x00000010

;--- a.ll
source_filename = "a.cpp"
target triple = "x86_64-unknown-linux-gnu"
@__memprof_profile_filename = weak constant [1 x i8] zeroinitializer
define i32 @a() { ret i32 1 }

;--- b.ll
source_filename = "b.cpp"
target triple = "x86_64-unknown-linux-gnu"
@__memprof_profile_filename = weak constant [1 x i8] zeroinitializer
define i32 @b() { ret i32 2 }
