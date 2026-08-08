#!/usr/bin/env bash
# Run this script with Bash.

# Stop on errors, unset variables, or failed pipeline commands.
set -euo pipefail

# Locate this script and then derive the repository root from it.
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "${script_dir}/.." && pwd)"

# Ensure the executable and its dependencies are up to date before running it.
"${repo_dir}/scripts/build.sh"

# Run the demo from the repository root so relative paths such as
# config/default.json are resolved relative to the project.
cd "${repo_dir}"

# Execute the compiled live application. "$@" forwards every additional
# argument supplied by the user to the demo unchanged.
"${repo_dir}/build/market_engine_demo" "$@"
