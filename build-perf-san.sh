#!/bin/bash
# Quick build script for PerfSanitizer development
# Usage:
#   ./build-perf-san.sh          # configure + build
#   ./build-perf-san.sh build    # build only (after initial configure)
#   ./build-perf-san.sh test     # build and test with sample file

set -e

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$SRCDIR/build-perf-san"
CLANG="$BUILDDIR/bin/clang"

configure() {
  cmake -G Ninja -B "$BUILDDIR" -S "$SRCDIR/llvm" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DLLVM_ENABLE_PROJECTS="clang;lld" \
    -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_USE_LINKER=lld \
    -DLLVM_PARALLEL_LINK_JOBS=2 \
    -DLLVM_OPTIMIZED_TABLEGEN=ON \
    -DLLVM_ENABLE_ASSERTIONS=ON \
    -DLLVM_ENABLE_WARNINGS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF \
    -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_ZSTD=OFF \
    -DLLVM_ENABLE_BINDINGS=OFF
}

case "${1:-all}" in
  configure)
    configure
    ;;
  build)
    ninja -C "$BUILDDIR" clang lld
    ;;
  all)
    configure
    ninja -C "$BUILDDIR" clang lld
    ;;
  test)
    ninja -C "$BUILDDIR" clang lld
    echo "=== Testing PerfSanitizer ==="
    if [ -f "$SRCDIR/test-perf-san.cpp" ]; then
      "$CLANG" -fperf-suggest -O2 "$SRCDIR/test-perf-san.cpp" -c -o /dev/null 2>&1
    else
      echo "No test-perf-san.cpp found. Create one to test."
    fi
    ;;
  *)
    echo "Usage: $0 [configure|build|all|test]"
    exit 1
    ;;
esac
