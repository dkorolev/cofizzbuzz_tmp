#!/bin/bash

set -e

for i in $(ls step??.cc | grep -v step83.cc); do
  echo $i
  cp $i step83.cc
  git add step83.cc
  git commit -m "83 is now $i"
  git push
  sleep 45
done
