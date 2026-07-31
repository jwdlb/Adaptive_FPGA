#!/usr/bin/env bash
# Run this script with Bash.

# Stop immediately if any command fails or an unset variable is referenced.
set -euo pipefail

# Locate the script and repository independently of the current directory.
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "${script_dir}/.." && pwd)"

# Configure and compile the project before running tests.
"${repo_dir}/scripts/build.sh"

# Run all tests registered with CTest. If a test fails, print its complete
# output instead of only reporting the test name and exit status.
ctest --test-dir "${repo_dir}/build" --output-on-failure
