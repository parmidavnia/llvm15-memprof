#ifndef LLVM_PROFILEDATA_RAWMEMPROFREADER_H_
#define LLVM_PROFILEDATA_RAWMEMPROFREADER_H_
//===- MemProfReader.h - Instrumented memory profiling reader ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains support for reading MemProf profiling data.
//
//===----------------------------------------------------------------------===//

// Borrow a caller-owned list of binary paths without prescribing its container.
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/Symbolize/SymbolizableModule.h"
#include "llvm/DebugInfo/Symbolize/Symbolize.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/ProfileData/MemProf.h"
#include "llvm/ProfileData/MemProfData.inc"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cstddef>

namespace llvm {
namespace memprof {

// Map from id (recorded from sanitizer stack depot) to virtual addresses for
// each program counter address in the callstack.
using CallStackMap = llvm::DenseMap<uint64_t, llvm::SmallVector<uint64_t>>;

class RawMemProfReader {
public:
  RawMemProfReader(const RawMemProfReader &) = delete;
  RawMemProfReader &operator=(const RawMemProfReader &) = delete;

  // Prints the contents of the profile in YAML format.
  void printYAML(raw_ostream &OS);

  // Return true if the \p DataBuffer starts with magic bytes indicating it is
  // a raw binary memprof profile.
  static bool hasFormat(const MemoryBuffer &DataBuffer);
  // Return true if the file at \p Path starts with magic bytes indicating it is
  // a raw binary memprof profile.
  static bool hasFormat(const StringRef Path);

  // Create a RawMemProfReader after sanity checking the contents of the file at
  // \p Path. The binary from which the profile has been collected is specified
  // via a path in \p ProfiledBinary.
  static Expected<std::unique_ptr<RawMemProfReader>>
  create(const Twine &Path, const StringRef ProfiledBinary,
         bool KeepName = false);
  // This reader converts runtime addresses into the MemProf records consumed
  // by llvm-profdata. Supply every DSO whose frames should be retained; the
  // single-binary overload above remains available to existing callers.
  static Expected<std::unique_ptr<RawMemProfReader>>
  create(const Twine &Path, ArrayRef<StringRef> ProfiledBinaries,
         bool KeepName = false);
  // Accept an already-read profile for format-dispatching callers such as
  // show --memory. Ownership transfer also avoids trying to read stdin twice.
  static Expected<std::unique_ptr<RawMemProfReader>>
  create(std::unique_ptr<MemoryBuffer> Buffer,
         ArrayRef<StringRef> ProfiledBinaries, bool KeepName = false);

  using GuidMemProfRecordPair = std::pair<GlobalValue::GUID, MemProfRecord>;
  using Iterator = InstrProfIterator<GuidMemProfRecordPair, RawMemProfReader>;
  Iterator end() { return Iterator(); }
  Iterator begin() {
    Iter = FunctionProfileData.begin();
    return Iterator(this);
  }

  Error readNextRecord(GuidMemProfRecordPair &GuidRecord);

  // The RawMemProfReader only holds memory profile information.
  InstrProfKind getProfileKind() const { return InstrProfKind::MemProf; }

  // Constructor for unittests only.
  RawMemProfReader(std::unique_ptr<llvm::symbolize::SymbolizableModule> Sym,
                   llvm::SmallVectorImpl<SegmentEntry> &Seg,
                   llvm::MapVector<uint64_t, MemInfoBlock> &Prof,
                   CallStackMap &SM, bool KeepName = false)
      : Symbolizer(std::move(Sym)), SegmentInfo(Seg.begin(), Seg.end()),
        CallstackProfileData(Prof), StackMap(SM), KeepSymbolName(KeepName) {
    // We don't call initialize here since there is no raw profile to read. The
    // test should pass in the raw profile as structured data.

    // If there is an error here then the mock symbolizer has not been
    // initialized properly.
    if (Error E = symbolizeAndFilterStackFrames())
      report_fatal_error(std::move(E));
    if (Error E = mapRawProfileToRecords())
      report_fatal_error(std::move(E));
  }

  // Return a const reference to the internal Id to Frame mappings.
  const llvm::DenseMap<FrameId, Frame> &getFrameMapping() const {
    return IdToFrame;
  }

  // Return a const reference to the internal function profile data.
  const llvm::MapVector<GlobalValue::GUID, IndexedMemProfRecord> &
  getProfileData() const {
    return FunctionProfileData;
  }

private:
  // Keep image-specific state together. The process may load several DSOs
  // with identical symbol names, so a PC must select both its owning image
  // and that image's debug information before we can construct a frame.
  struct ProfiledModule {
    // Take ownership first so symbolization never outlives the object bytes.
    explicit ProfiledModule(object::OwningBinary<object::Binary> &&Bin)
        : Binary(std::move(Bin)) {}

    // Keep the binary and its backing buffer alive for the lifetime of the
    // module symbolizer.
    object::OwningBinary<object::Binary> Binary;
    // Symbolizer created from this module's object file and DWARF context.
    std::unique_ptr<symbolize::SymbolizableModule> Symbolizer;
    // Build ID connects this exact binary build to its recorded runtime range.
    // It answers where this image was loaded, not its identity across builds.
    llvm::SmallVector<uint8_t, 20> BuildId;
    // Stable module ID answers which library a function belongs to. Unlike
    // BuildId, it is derived from source composition rather than generated code.
    llvm::SmallVector<uint8_t, 16> ModuleId;
    // The ELF-side address anchors translation from randomized runtime PCs
    // back to the coordinates used by this image's debug information.
    uint64_t PreferredTextSegmentAddress = 0;
    // Runtime address range of this module's executable segment, recovered
    // from the raw profile by matching the build ID.
    uint64_t ProfiledTextSegmentStart = 0;
    uint64_t ProfiledTextSegmentEnd = 0;
  };

  // Factories create an empty reader, then validate and populate all modules
  // together. This replaces the old assumption of one constructor-owned image.
  explicit RawMemProfReader(bool KeepName) : KeepSymbolName(KeepName) {}
  // Open the supplied images, read mappings from DataBuffer, then symbolize
  // stacks and assemble records. Keep raw decoding separate from image lookup.
  Error initialize(std::unique_ptr<MemoryBuffer> DataBuffer,
                   ArrayRef<StringRef> ProfiledBinaries);
  // Read and parse the contents of the `DataBuffer` as a binary format profile.
  Error readRawProfile(std::unique_ptr<MemoryBuffer> DataBuffer);
  // Symbolize and cache all the virtual addresses we encounter in the
  // callstacks from the raw profile. Also prune callstack frames which we can't
  // symbolize or those that belong to the runtime. For profile entries where
  // the entire callstack is pruned, we drop the entry from the profile.
  Error symbolizeAndFilterStackFrames();
  // Construct memprof records for each function and store it in the
  // `FunctionProfileData` map. A function may have allocation profile data or
  // callsite data or both.
  Error mapRawProfileToRecords();
  // Match every supplied image to its runtime text segment by build ID and
  // establish unambiguous ranges before any address is routed to a symbolizer.
  Error setupForSymbolization();

  // A helper method to extract the frame from the IdToFrame map.
  const Frame &idToFrame(const FrameId Id) const {
    auto It = IdToFrame.find(Id);
    assert(It != IdToFrame.end() && "Id not found in map.");
    return It->getSecond();
  }

  // Translate a PC only after its owner has been chosen; the same numerical
  // object-relative offset may be valid in several different DSOs.
  object::SectionedAddress getModuleOffset(uint64_t VirtualAddress,
                                           const ProfiledModule &Module) const;
  // Return the image owning this runtime PC, or nullptr for unlisted images.
  // Depends on setupForSymbolization sorting the half-open executable ranges.
  ProfiledModule *findModule(uint64_t VirtualAddress) const;

  // Own all binary/debug contexts for this reader, sorted by runtime range end
  // after setup. Object addresses stay stable while the owning pointers move.
  SmallVector<std::unique_ptr<ProfiledModule>, 4> ProfiledModules;
  // Used only by the mock-symbolizer unit-test constructor.
  std::unique_ptr<llvm::symbolize::SymbolizableModule> Symbolizer;

  // The contents of the raw profile.
  llvm::SmallVector<SegmentEntry, 16> SegmentInfo;
  // A map from callstack id (same as key in CallStackMap below) to the heap
  // information recorded for that allocation context.
  llvm::MapVector<uint64_t, MemInfoBlock> CallstackProfileData;
  CallStackMap StackMap;

  // Cached symbolization from PC to Frame.
  llvm::DenseMap<uint64_t, llvm::SmallVector<FrameId>> SymbolizedFrame;
  llvm::DenseMap<FrameId, Frame> IdToFrame;

  llvm::MapVector<GlobalValue::GUID, IndexedMemProfRecord> FunctionProfileData;
  llvm::MapVector<GlobalValue::GUID, IndexedMemProfRecord>::iterator Iter;

  // Whether to keep the symbol name for each frame after hashing.
  bool KeepSymbolName = false;
  // A mapping of the hash to symbol name, only used if KeepSymbolName is true.
  llvm::DenseMap<uint64_t, std::string> GuidToSymbolName;
};
} // namespace memprof
} // namespace llvm

#endif // LLVM_PROFILEDATA_RAWMEMPROFREADER_H_
