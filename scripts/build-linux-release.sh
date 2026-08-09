#!/bin/bash

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "${script_dir}/.."

cmake --preset="Linux Release Config" -DENABLE_TESTS=OFF -DBUILD_TOOLS=OFF
cmake --build --preset="Linux Release Build"

if [ ! -f "/usr/lib/libfmod.so" ]; then
    echo "install libfmod.so."
    sudo cp $(find ./out/linux/release/ -name libfmodL.so) /usr/lib/
    sudo cp $(find ./out/linux/release/ -name libfmod.so) /usr/lib/
    sudo ln -s /usr/lib/libfmod.so /usr/lib/libfmod.so.6
fi
