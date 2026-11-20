# SPDX-License-Identifier: Apache-2.0
#
# vcpkg's x64-windows triplet, as at the builtin-baseline in vcpkg.json, with one change: Embree's
# debug build is inlined. vcpkg-configuration.json puts this directory before vcpkg's own
# triplets, so it is also the host triplet on Windows.
#
# vcpkg's toolchain compiles every port's Debug configuration with /Ob0 /Od /RTC1, and the
# debug Embree comes out without inlining even though Embree adds /Ox after them. Embree compiles
# its SIMD helpers (vboolf4 and the like) once per ISA under the same names and relies on
# inlining them. Out of line, the linker keeps one copy of each, the SSE2 one. On a CPU with
# AVX-512, the AVX-512 kernels, whose vboolf4 is a one-byte mask rather than a 16-byte vector,
# call it and fault (M1-46, #168). vcpkg appends VCPKG_<LANG>_FLAGS_DEBUG after its own flags, so
# /Ob2 inlines them as the release build does.
#
# Any change here changes the ABI hash of every package built with this triplet: Windows then
# rebuilds them all, Embree taking one to two hours.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_PROVIDED_FORTRAN ON)

if(PORT STREQUAL "embree")
    set(VCPKG_C_FLAGS_DEBUG "/Ob2")
    set(VCPKG_CXX_FLAGS_DEBUG "/Ob2")
endif()
