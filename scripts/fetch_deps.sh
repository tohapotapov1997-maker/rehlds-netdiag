#!/usr/bin/env bash
set -euo pipefail

mkdir -p deps

if [ ! -d deps/ReAPI/.git ]; then
  git clone --depth 1 https://github.com/rehlds/ReAPI.git deps/ReAPI
fi

if [ ! -d deps/Metamod-R/.git ]; then
  git clone --depth 1 https://github.com/rehlds/Metamod-R.git deps/Metamod-R
fi
