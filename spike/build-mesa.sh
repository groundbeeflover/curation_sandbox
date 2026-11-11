#!/usr/bin/env bash
# Builds only lavapipe from a pinned Mesa release tarball into $2 (spike M1-03, issue #38).
# Usage: build-mesa.sh <version> <sha256> <prefix>
set -euo pipefail
version=$1 sha=$2 prefix=$3
work=$(mktemp -d)
cd "$work"
curl -sSfL --retry 3 -o mesa.tar.xz "https://archive.mesa3d.org/mesa-${version}.tar.xz"
echo "${sha}  mesa.tar.xz" | sha256sum -c -
tar xf mesa.tar.xz
cd "mesa-${version}"
meson setup build --prefix="$prefix" --libdir=lib --buildtype=release -Db_ndebug=true \
  -Dplatforms= -Dgallium-drivers=llvmpipe -Dvulkan-drivers=swrast -Dvulkan-layers= \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled \
  -Dllvm=enabled -Dshared-llvm=enabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dxmlconfig=disabled -Dzstd=enabled -Dbuild-tests=false -Dvideo-codecs= -Dtools= \
  -Dgallium-va=disabled -Dgallium-rusticl=false -Dteflon=false -Dmesa-clc=auto -Dperfetto=false
meson install -C build
