#!/usr/bin/env bash
# Builds Fast-DDS (static, sanitized) + its in-tree libFuzzer harnesses (fuzz/C++/*), their
# -standalone reproducers, and an unsanitized build of the XML-parser / message-receiver unit tests.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
# CMake takes the compiler from the exported CC/CXX: passing -DCMAKE_*_COMPILER makes a re-run with a
# different PATH (ccache shim on/off) differ from the cache, which makes CMake drop its cache and every -D.
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# Fast-CDR is a git submodule (gitlink) that a plain `COPY .` leaves empty; the first, online build
# fetches the pinned commit, every later (offline) run finds it in the tree.
if [ ! -f thirdparty/fastcdr/CMakeLists.txt ]; then
  git submodule update --init thirdparty/fastcdr
fi

B="$SRC/build"          # `build/` is gitignored upstream, so `git clean -ffdX` removes it
mkdir -p "$B"

# Every project object carries SanCov (fuzzer-no-link) so libFuzzer sees edges inside the library.
# FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION is upstream's switch that lets MessageReceiver run without a
# participant (what fuzz_processCDRMsg needs), as in the OSS-Fuzz build.
FUZZ_FLAGS="$SANITIZER_FLAGS -fsanitize=fuzzer-no-link $DEBUG_FLAGS -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION"

# --- sanitized + instrumented dependency + library + harnesses -------------------------------------
cmake -S thirdparty/fastcdr -B "$B/fastcdr" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="$B/install" \
  -DCMAKE_C_FLAGS="$FUZZ_FLAGS" -DCMAKE_CXX_FLAGS="$FUZZ_FLAGS"
cmake --build "$B/fastcdr" -j"$MAYHEM_JOBS" --target install

$CXX $SANITIZER_FLAGS -c mayhem/lsan_off.cc -o "$B/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$B/standalone_main.o"

# fuzz/CMakeLists.txt is only added when LIB_FUZZING_ENGINE is set; its value is appended to each
# harness link line, so it carries the engine plus the LSan-off object.
LIB_FUZZING_ENGINE="$LIB_FUZZING_ENGINE $B/lsan_off.o" \
cmake -S . -B "$B/fuzz" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_PREFIX_PATH="$B/install" \
  -DCMAKE_C_FLAGS="$FUZZ_FLAGS" -DCMAKE_CXX_FLAGS="$FUZZ_FLAGS" \
  -DTHIRDPARTY=OFF -DSECURITY=OFF -DSQLITE3_SUPPORT=OFF \
  -DCOMPILE_TOOLS=OFF -DCOMPILE_EXAMPLES=OFF
cmake --build "$B/fuzz" -j"$MAYHEM_JOBS"

for h in fuzz_XMLProfiles fuzz_processCDRMsg fuzz_proxy_cdr; do
  d="$B/fuzz/fuzz/C++/$h"
  cp "$d/$h" "/mayhem/$h"
  # Re-link the same objects against the run-once driver instead of libFuzzer -> reproducer.
  ( cd "$d" && perl -pe "s{-fsanitize=fuzzer(?=\\s|\$)}{$B/standalone_main.o}g; s{-o \\S*/?$h(?=\\s)}{-o /mayhem/$h-standalone}" \
      "CMakeFiles/$h.dir/link.txt" > link-standalone.sh && bash link-standalone.sh )
done

# --- the project's own unit tests, NORMAL flags (no sanitizers / DWARF pinning): the oracle for test.sh -
cmake -S thirdparty/fastcdr -B "$B/fastcdr-plain" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX="$B/install-plain" \
  -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS"
cmake --build "$B/fastcdr-plain" -j"$MAYHEM_JOBS" --target install

env -u LIB_FUZZING_ENGINE \
cmake -S . -B "$B/tests" \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_PREFIX_PATH="$B/install-plain" \
  -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS" \
  -DTHIRDPARTY=OFF -DSECURITY=OFF -DSQLITE3_SUPPORT=OFF \
  -DCOMPILE_TOOLS=OFF -DEPROSIMA_BUILD_TESTS=ON
# XMLParserTests is left out: its locator tests need DNS, which the air-gapped re-run does not have.
cmake --build "$B/tests" -j"$MAYHEM_JOBS" --target \
  XMLProfileParserTests XMLEndpointParserTests XMLTreeTests XMLLoadFileTests MessageReceiverTests
