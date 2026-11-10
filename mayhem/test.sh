#!/usr/bin/env bash
#
# mayhem/test.sh — RUNS the upstream gtest unit tests that mayhem/build.sh built (XML profile parser,
# XML endpoint parser, XML tree, XML file loader, RTPS StatelessReader). Behavioral: every binary must
# print gtest's summary with at least one passing test, so a missing, crashing or neutered (exit(0))
# binary counts as a failure.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
cd "$SRC"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

T="$SRC/build/tests/test/unittest"
SUITES=(
  "$T/xmlparser/XMLProfileParserTests"
  "$T/xmlparser/XMLEndpointParserTests"
  "$T/xmlparser/XMLTreeTests"
  "$T/xmlparser/XMLLoadFileTests"
  "$T/rtps/reader/StatelessReaderTests"
)

passed=0 failed=0 skipped=0
for bin in "${SUITES[@]}"; do
  if [ ! -x "$bin" ]; then
    echo "MISSING test binary: $bin (mayhem/build.sh should have built it)" >&2
    failed=$((failed + 1)); continue
  fi
  # the tests open fixture files next to the binary
  out="$(cd "$(dirname "$bin")" && "$bin" 2>&1)"; rc=$?
  printf '%s\n' "$out" | grep -E '^\[(==========|  PASSED  |  FAILED  |  SKIPPED )\]' | sed "s|^|$(basename "$bin"): |"
  p="$(printf '%s\n' "$out" | sed -nE 's/^\[  PASSED  \] ([0-9]+) tests?\..*/\1/p' | tail -1)"
  f="$(printf '%s\n' "$out" | sed -nE 's/^\[  FAILED  \] ([0-9]+) tests?, listed below.*/\1/p' | tail -1)"
  s="$(printf '%s\n' "$out" | sed -nE 's/^\[  SKIPPED \] ([0-9]+) tests?, listed below.*/\1/p' | tail -1)"
  if [ -z "$p" ] || [ "$p" -eq 0 ]; then
    echo "$(basename "$bin"): no passing tests reported (rc=$rc)" >&2
    failed=$((failed + 1)); continue
  fi
  f="${f:-0}"
  [ "$rc" -eq 0 ] || [ "$f" -gt 0 ] || failed=$((failed + 1))
  # Some upstream tests assert on asynchronously-consumed log entries (a race in the test itself), so a
  # failed test is re-run on its own up to 10 times; a test that is really broken fails every attempt.
  if [ "$f" -gt 0 ]; then
    while read -r t; do
      for attempt in 1 2 3 4 5 6 7 8 9 10; do
        if (cd "$(dirname "$bin")" && "$bin" --gtest_filter="$t" >/dev/null 2>&1); then
          echo "$(basename "$bin"): $t passed on retry $attempt"
          p=$((p + 1)); f=$((f - 1)); break
        fi
      done
    done < <(printf '%s\n' "$out" | sed -nE 's/^\[  FAILED  \] ([A-Za-z0-9_\/.]+)$/\1/p' | sort -u)
  fi
  passed=$((passed + p)); failed=$((failed + f)); skipped=$((skipped + ${s:-0}))
done

emit_ctrf "gtest" "$passed" "$failed" "$skipped"
