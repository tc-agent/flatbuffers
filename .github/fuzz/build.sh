#!/bin/bash -eu
# Copyright 2020 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
################################################################################

# Build fuzzer only
cd $SRC/flatbuffers
mkdir build
cd build
cmake -DOSS_FUZZ:BOOL=ON -G "Unix Makefiles" ../tests/fuzzer
make

cp ../tests/fuzzer/*.dict $OUT/
cp *.bfbs $OUT/
cp *_fuzzer $OUT/

# OSS-Fuzz expects the dictionary file to share the fuzzer's basename.
mv $OUT/codegen_json.dict $OUT/codegen_fuzzer.dict

# ---------------------------------------------------------------------------
# Seed corpora.
#
# Previously this project shipped no seed corpora, so every harness started
# from an empty corpus and had to rediscover the FlatBuffers grammar / binary
# wire format from scratch within the OSS-Fuzz time budget. The upstream
# repository already contains a large body of valid schemas, JSON documents
# and serialized buffers under tests/; using them as seeds gives every harness
# a realistic starting point.
#
# Text harnesses (parser/scalar/monster/codegen) consume two leading bytes
# (flags + reserved) before the NUL-terminated text, so seeds are prefixed
# with two NUL bytes. The 64bit harness consumes one leading flag byte.
# ---------------------------------------------------------------------------
SRCROOT=$SRC/flatbuffers
TESTS=$SRCROOT/tests
SEEDTMP=$(mktemp -d)

# $1 = corpus dir, $2 = number of prefix bytes, $3.. = source files
add_seeds() {
  local dst="$1"; shift
  local nprefix="$1"; shift
  mkdir -p "$dst"
  local prefix=""
  local i
  for ((i=0; i<nprefix; i++)); do prefix+=$'\x00'; done
  for f in "$@"; do
    [ -f "$f" ] || continue
    local out="$dst/$(echo "$f" | sed 's#[/.]#_#g')"
    if [ "$nprefix" -gt 0 ]; then
      printf '%s' "$prefix" > "$out"
      cat "$f" >> "$out"
    else
      cp "$f" "$out"
    fi
  done
}

# Schema/JSON text harnesses: feed every .fbs schema and .json document.
FBS_FILES=$(find "$TESTS" -name '*.fbs')
JSON_FILES=$(find "$TESTS" -name '*.json')

for fuzzer in parser_fuzzer scalar_fuzzer codegen_fuzzer; do
  add_seeds "$SEEDTMP/$fuzzer" 2 $FBS_FILES $JSON_FILES
done

# Monster JSON harness: monster-shaped JSON documents.
add_seeds "$SEEDTMP/monster_fuzzer" 2 \
  "$TESTS/monsterdata_test.json" \
  "$TESTS/monsterdata_extra.json" \
  "$TESTS/unicode_test.json" \
  $JSON_FILES

# Monster binary verifier: serialized Monster buffers.
add_seeds "$SEEDTMP/verifier_fuzzer" 0 \
  "$TESTS/monsterdata_test.mon" \
  $(find "$TESTS" -name '*.mon')

# FlexBuffers verifier: serialized flexbuffers.
add_seeds "$SEEDTMP/flexverifier_fuzzer" 0 \
  "$TESTS/gold_flexbuffer_example.bin"

# 64bit harness: serialized RootTable buffers, one leading flag byte.
add_seeds "$SEEDTMP/64bit_fuzzer" 1 \
  "$TESTS/64bit/test_64bit.bin"

# Annotator harness: binary buffers annotated against annotated_binary.bfbs,
# including the deliberately malformed buffers from the upstream test suite.
add_seeds "$SEEDTMP/annotator_fuzzer" 0 \
  "$TESTS/annotated_binary/annotated_binary.bin" \
  $(find "$TESTS/annotated_binary" -name '*.bin')

for fuzzer in parser_fuzzer scalar_fuzzer codegen_fuzzer monster_fuzzer \
              verifier_fuzzer flexverifier_fuzzer 64bit_fuzzer annotator_fuzzer; do
  if [ -d "$SEEDTMP/$fuzzer" ]; then
    (cd "$SEEDTMP/$fuzzer" && zip -q -r "$OUT/${fuzzer}_seed_corpus.zip" .)
  fi
done

rm -rf "$SEEDTMP"

# Build unit test
mkdir $SRC/flatbuffers/build-tests
cd $SRC/flatbuffers/build-tests
cmake -DFLATBUFFERS_BUILD_FLATC=ON -DFLATBUFFERS_BUILD_TESTS=ON ..
make flattests -j$(nproc)
