#!/usr/bin/env bash
# Run this script with Bash found through the user's PATH.

# Stop immediately if a command fails (-e), an unset variable is used (-u),
# or a command in a pipeline fails (pipefail). This prevents a failed build
# from being mistaken for a successful one.
set -euo pipefail

# Resolve the directory containing this script. BASH_SOURCE[0] refers to this
# file even when the script is called from another working directory.
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

# The repository root is the parent directory of scripts/.
repo_dir="$(cd -- "${script_dir}/.." && pwd)"

# Configure the project from the repository root and place generated build
# files in build/. The -S option selects the source tree and -B selects the
# build tree; this keeps generated files separate from the source files.
cmake -S "${repo_dir}" -B "${repo_dir}/build"

# Compile all configured targets. -j allows CMake to use parallel compilation
# where supported, making the build faster on multi-core machines.
cmake --build "${repo_dir}/build" -j
