#!/usr/bin/env bash
set -euo pipefail

mapfile -t CPP_FILES < <(git ls-files '*.h' '*.hpp' '*.c' '*.cpp')
if [ "${#CPP_FILES[@]}" -gt 0 ]; then
  clang-format -i "${CPP_FILES[@]}"
fi