#!/usr/bin/env bash
# Run this script with Bash.

# Exit on errors, unset variables, and failed commands inside pipelines.
set -euo pipefail

# Find the directory containing this script, regardless of the caller's
# current working directory.
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# Move from scripts/ to the repository root.
repo_dir="$(cd -- "${script_dir}/.." && pwd)"

# Build the latest version before attempting to benchmark it.
"${repo_dir}/scripts/build.sh"

# Benchmark functionality is not implemented yet. This message documents the
# current project phase while still confirming that the foundation built.
echo "Benchmarking is introduced in Phase 9. The Phase 0 foundation was built successfully."
