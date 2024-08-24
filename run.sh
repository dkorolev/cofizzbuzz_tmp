#!/bin/bash

set -e

LAST_STEP=$(ls step*.cc | sort | tail -n 1)

MODE=${1:-debug}
STEP=${2:-$LAST_STEP}

if [ "$MODE" == "clean" ] ; then
  rm -rf .debug .release
  exit
fi

echo "Testing $STEP in $MODE mode."
if [ "$MODE" == "debug" ] ; then
  mkdir -p .debug
  clang++ -g -O0 -DDEBUG -std=c++20 $STEP -o .debug/binary -Wno-unqualified-std-cast-call
  .debug/binary
elif [ "$MODE" == "release" ] ; then
  mkdir -p .release
  clang++ -O3 -DNDEBUG -std=c++20 $STEP -o .release/binary -Wno-unqualified-std-cast-call
  .release/binary
else
  echo '$MODE must be `debug` or `release`.'
  exit 1
fi
