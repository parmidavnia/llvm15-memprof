add_llvm_component_library(LLVMProfileData
  GCOV.cpp
  InstrProf.cpp
  InstrProfCorrelator.cpp
  InstrProfReader.cpp
  InstrProfWriter.cpp
  MemProf.cpp
  ProfileSummaryBuilder.cpp
  SampleProf.cpp
  SampleProfReader.cpp
  SampleProfWriter.cpp
  RawMemProfReader.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/ProfileData

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Core
  Object
  Support
  Demangle
  Symbolize
  DebugInfoDWARF
  )

# Mock feature: the raw MemProf reader is compiled in LLVMProfileData.
if(LLVM_ENABLE_CUSTOM_MEMPROF)
  target_compile_definitions(LLVMProfileData PRIVATE LLVM_CUSTOM_MEMPROF)
endif()

add_subdirectory(Coverage)
