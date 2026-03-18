#!/usr/bin/env bash

set -euo pipefail

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH=/ucrt64 \
  -DUTSURE_BUILD_APP=ON

cmake --build build --target utsure_encoder_core --parallel
cmake --build build --target utsure_encoder_app --parallel

export PATH="/ucrt64/bin:${PATH}"
export QT_PLUGIN_PATH="/ucrt64/share/qt6/plugins"
export QT_QPA_PLATFORM="offscreen"

./build/src/app/utsure.exe --smoke-test
