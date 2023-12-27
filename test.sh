#!/bin/sh

if [ $# -lt 3 ]; then
  echo "Usage.. ./test plugin_name test_file test_pass "
  echo "e.g. ./test TicToc.cpp TestTicTok.c tic-tok "
  exit 1
fi


if [ ! -d "build" ]; then
  BUILD_CASE=$1 cmake -B build -G Ninja -DLT_LLVM_INSTALL_DIR=$LLVM_DIR -DCMAKE_EXPORT_COMPILE_COMMANDS=1
  cmake --build build 
fi

test_file=$2
output_file="${test_file%.*}.ll"
output_bin="${test_file%.*}.bin"

clang -emit-llvm -S -O0 -Xclang -disable-O0-optnone ${test_file} -o ${output_file}

system_name=$(uname -s)

if [ "$system_name" == "Darwin" ]; then
  opt -load-pass-plugin=./build/libTtarget.dylib -passes="$3" ${output_file} -S -o ${output_bin} 
elif [ "$system_name" == "Linux" ]; then
  opt -load-pass-plugin=./build/libTtarget.so -passes="$3" ${output_file} -S -o ${output_bin} 
else
  echo "F*** Windows!"
fi


