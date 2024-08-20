#!/bin/bash

set -e

MODE=${1:-debug}
STEP=$(ls step*.cc | sort | tail -n 1)

if [ "$MODE" == "clean" ] ; then
  rm -rf .debug .release
  exit
fi

echo "Testing $STEP in $MODE mode."
if [ "$MODE" == "debug" ] ; then
  mkdir -p .debug
  g++ -g -O0 -DDEBUG -std=c++20 $STEP -o .debug/binary
  .debug/binary
elif [ "$MODE" == "release" ] ; then
  mkdir -p .release
  g++ -O3 -DNDEBUG -std=c++20 $STEP -o .release/binary
  .release/binary
else
  echo '$MODE must be `debug` or `release`.'
  exit 1
fi
