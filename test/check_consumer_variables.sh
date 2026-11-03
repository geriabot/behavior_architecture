#!/bin/bash
# Copyright 2026 Rodrigo Pérez-Rodríguez
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Prints what a package that finds behavior_architecture, without asking for interpretation,
# receives: the default variables (behavior_architecture_TARGETS, _LIBRARIES, _INCLUDE_DIRS,
# _DEPENDENCIES, _RECURSIVE_DEPENDENCIES, _DEFINITIONS, _LINK_FLAGS, _LIBRARY_DIRS) and whether
# the aggregate target behavior_architecture::behavior_architecture exists. One list item per
# line; the package's install prefix is printed as <prefix>, so that two installs compare equal.
#
# The opt-in variables of interpretation are printed to stderr, for information only.
#
# Usage (with the environment and the install of behavior_architecture sourced):
#   test/check_consumer_variables.sh                   print to stdout
#   test/check_consumer_variables.sh -o FILE           write to FILE
#   test/check_consumer_variables.sh -c BASELINE       compare with BASELINE (exit 1 if they differ)
# A BASELINE is the output of this script for the package built without interpretation (main),
# in the same environment.
set -euo pipefail

out=""
baseline=""
while getopts "o:c:h" opt; do
  case $opt in
    o) out=$OPTARG ;;
    c) baseline=$OPTARG ;;
    *) sed -n '16,29p' "$0"; exit 2 ;;
  esac
done

probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT

cat > "$probe/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.16)
project(behavior_architecture_consumer_probe)
find_package(behavior_architecture REQUIRED)
get_filename_component(_prefix "${behavior_architecture_DIR}/../../.." ABSOLUTE)
set(_lines "")
foreach(_var TARGETS LIBRARIES INCLUDE_DIRS DEPENDENCIES RECURSIVE_DEPENDENCIES DEFINITIONS
    LINK_FLAGS LIBRARY_DIRS)
  list(LENGTH behavior_architecture_${_var} _n)
  string(APPEND _lines "behavior_architecture_${_var} (${_n})\n")
  foreach(_item IN LISTS behavior_architecture_${_var})
    string(REPLACE "${_prefix}" "<prefix>" _item "${_item}")
    string(APPEND _lines "  ${_item}\n")
  endforeach()
endforeach()
if(TARGET behavior_architecture::behavior_architecture)
  string(APPEND _lines "target behavior_architecture::behavior_architecture: defined\n")
else()
  string(APPEND _lines "target behavior_architecture::behavior_architecture: not defined\n")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/variables.txt" "${_lines}")
message(STATUS "behavior_architecture_DIR=${behavior_architecture_DIR}")
message(STATUS "behavior_architecture_INTERPRETATION_FOUND=${behavior_architecture_INTERPRETATION_FOUND}")
message(STATUS "behavior_architecture_INTERPRETATION_TARGETS=[${behavior_architecture_INTERPRETATION_TARGETS}]")
message(STATUS "behavior_architecture_INTERPRETATION_INTERFACE_TARGETS=[${behavior_architecture_INTERPRETATION_INTERFACE_TARGETS}]")
CMAKE

if ! cmake -S "$probe" -B "$probe/build" > "$probe/cmake.log" 2>&1; then
  cat "$probe/cmake.log" >&2
  echo "check_consumer_variables: the probe failed to configure" >&2
  exit 2
fi
grep -E "behavior_architecture_(DIR|INTERPRETATION)" "$probe/cmake.log" | sed 's/^-- /# /' >&2

result="$probe/build/variables.txt"
if [ -n "$out" ]; then
  cp "$result" "$out"
fi
if [ -n "$baseline" ]; then
  if diff -u "$baseline" "$result"; then
    echo "check_consumer_variables: identical to $baseline" >&2
  else
    echo "check_consumer_variables: DIFFERENT from $baseline" >&2
    exit 1
  fi
elif [ -z "$out" ]; then
  cat "$result"
fi
