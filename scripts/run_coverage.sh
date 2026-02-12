#!/bin/bash
set -e

echo "Generating code coverage report..."

# Clean and rebuild with coverage flags
make clean
make all CFLAGS="-Wall -Wextra -Werror -std=c11 -O0 -g -fprofile-arcs -ftest-coverage -D_POSIX_C_SOURCE=200809L" LDFLAGS="-fprofile-arcs -ftest-coverage -lm -lpthread"

# Run all tests
./build/rift_integration_test
./build/rift_metrics --size 1048576

# Generate report
if command -v lcov >/dev/null; then
    lcov --capture --directory . --output-file coverage.info
    lcov --remove coverage.info '/usr/*' --output-file coverage.info
    genhtml coverage.info --output-directory coverage_report
    echo "Coverage report generated in coverage_report/index.html"
else
    echo "lcov not found. Please install lcov to generate HTML report."
    echo "gcov files have been generated."
fi
