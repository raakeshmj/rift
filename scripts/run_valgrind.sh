#!/bin/bash
set -e

# Run Valgrind on a small transfer test
echo "Running Valgrind memory leak check on rift_metrics..."
valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
  ./build/rift_metrics --size 102400 --mode rift

if [ $? -eq 0 ]; then
    echo "SUCCESS: No memory leaks detected."
else
    echo "FAILURE: Memory leaks detected."
    exit 1
fi
