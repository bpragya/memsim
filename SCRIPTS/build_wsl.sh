#!/bin/bash
# Build memsim for Linux (WSL). Run from anywhere inside the repo:
#   bash SCRIPTS/build_wsl.sh
#
# Builds, into ~/build/bin:
#   memsim_ram2        : memsim + Ramulator2, from the current checkout's src/
#                        (use on integrating-ram2 / implementing-bard)
#   memsim_star        : memsim's own DRAM model, from robaracoch-nCCDS-nCCDL-changes
#   memsim_write_infra : memsim's own DRAM model + WIP write path, from memsim-write-infra
#
# Needs cmake and zlib. Without sudo they can live under ~/local:
#   cmake: extract a cmake-<ver>-linux-x86_64.tar.gz release into ~/local
#   zlib : git clone https://github.com/madler/zlib && ./configure --prefix=$HOME/local && make install
#
# Run simulations from the repo root (traces are TRACES/<name>.mtf.gz).
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
LOCAL=$HOME/local
OUT=$HOME/build/bin
RAM2=$REPO/third_party/ramulator2
RAM2_BUILD=$HOME/build/ramulator2
export PATH=$LOCAL/bin:$PATH
CFLAGS="-O2 -W -Wall -Wno-deprecated -Wno-unused-parameter -Wno-unused-but-set-variable"
mkdir -p $OUT

# Ramulator2 (incremental). Its CMake writes libramulator.so into $RAM2 (gitignored there).
if [ ! -f $RAM2_BUILD/CMakeCache.txt ]; then
  cmake -S $RAM2 -B $RAM2_BUILD -DCMAKE_BUILD_TYPE=Release -DRAMULATOR_PYTHON_BINDINGS=OFF
fi
cmake --build $RAM2_BUILD -j$(nproc)

# memsim + Ramulator2 from the current checkout
if [ -f $REPO/src/dram.c ]; then
  echo "skipping memsim_ram2: current checkout uses memsim's own DRAM model"
else
  (cd $REPO/src && g++ $CFLAGS -std=c++20 -I$RAM2/src -I$LOCAL/include mcore.c os.c mcache.c sim.c memsys.c \
     -o $OUT/memsim_ram2 -L$RAM2 -lramulator -L$LOCAL/lib -Wl,-rpath,$RAM2:$LOCAL/lib -lz -lm)
  echo "built memsim_ram2 from $(git -C $REPO rev-parse --abbrev-ref HEAD) working tree"
fi

# memsim's own DRAM model, built from a branch's committed source
build_star () {  # $1 = branch, $2 = output name
  if ! git -C $REPO rev-parse --verify -q "$1" >/dev/null; then
    echo "skipping $2: branch $1 not found"; return
  fi
  src=$HOME/build/src_$2
  mkdir -p "$src"
  git -C $REPO archive "$1" src | tar -x -C "$src"   # overwrites any previous copy
  (cd "$src/src" && g++ $CFLAGS -I$LOCAL/include dram.c dramchannel.c drambank.c mcore.c os.c mcache.c sim.c memsys.c \
     -o $OUT/$2 -L$LOCAL/lib -Wl,-rpath,$LOCAL/lib -lz -lm)
  echo "built $2 from $1 ($(git -C $REPO rev-parse --short $1))"
}
build_star robaracoch-nCCDS-nCCDL-changes memsim_star
build_star memsim-write-infra memsim_write_infra

ls -la $OUT
