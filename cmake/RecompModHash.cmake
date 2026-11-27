# The portable hash primitive is usable by C-only tools without the package
# runtime, a C++ compiler/runtime, filesystem services or optional crypto DLLs.
include_guard(GLOBAL)
if(NOT TARGET recomp_mod_hash)
  add_library(recomp_mod_hash STATIC "${CMAKE_CURRENT_LIST_DIR}/../src/rmod_sha256.c")
  add_library(recomp-mod-core::hash ALIAS recomp_mod_hash)
  set_target_properties(recomp_mod_hash PROPERTIES EXPORT_NAME hash)
  target_compile_features(recomp_mod_hash PUBLIC c_std_11)
  target_include_directories(recomp_mod_hash PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include>
    $<INSTALL_INTERFACE:include>)
endif()
