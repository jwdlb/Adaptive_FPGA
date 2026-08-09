#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build}"
event_count="${EVENTS:-100000}"
iterations="${ITERATIONS:-3}"
temporary_dir="$(mktemp -d)"
trap 'rm -rf -- "$temporary_dir"' EXIT

python3 python/generate_events.py --output "$temporary_dir/events.csv" --events "$event_count" --seed 42 >/dev/null
sed 's/"dashboardPort": 8080/"dashboardPort": 0/' config/default.json >"$temporary_dir/config.json"

measure() {
    local mode="$1"
    local total_ns=0
    for ((iteration=0; iteration<iterations; ++iteration)); do
        local started finished
        started="$(date +%s%N)"
        if [[ "$mode" == off ]]; then
            "$build_dir/market_engine_demo" --config "$temporary_dir/config.json" --input "$temporary_dir/events.csv" --no-dashboard --no-gpu >/dev/null
        else
            "$build_dir/market_engine_demo" --config "$temporary_dir/config.json" --input "$temporary_dir/events.csv" --no-gpu >/dev/null
        fi
        finished="$(date +%s%N)"
        total_ns=$((total_ns + finished - started))
    done
    echo $((total_ns / iterations))
}

off_ns="$(measure off)"
on_ns="$(measure on)"
python3 - "$off_ns" "$on_ns" "$event_count" "$iterations" <<'PY'
import sys
off, on, events, iterations = map(int, sys.argv[1:])
print(f"dashboard benchmark: events={events} iterations={iterations}")
print(f"  off: {off / 1e6:.3f} ms")
print(f"  on:  {on / 1e6:.3f} ms")
print(f"  overhead: {(on-off) / 1e6:.3f} ms ({(on/off-1)*100:.2f}%)")
PY
