#!/usr/bin/env bash
# TEST TOOLING ONLY - runs the 4 experiments and prints a results table.
# The impairments are applied by ./chaos-emulator (Netlink); `ping` only measures.
# Usage: sudo scripts/run_experiments.sh [ping_count]     (default 50 pings, 0.2 s apart)
set -uo pipefail

COUNT=${1:-50}
BIN=./chaos-emulator
NS="ip netns exec client"
TARGET=10.0.0.2

if [[ $EUID -ne 0 ]]; then echo "run as root: sudo $0" >&2; exit 1; fi
[[ -x $BIN ]] || { echo "build first: make" >&2; exit 1; }
$NS ip link show veth0 >/dev/null 2>&1 || { echo "run scripts/setup_env.sh first" >&2; exit 1; }

run_test() {   # name, expected, emulator args...
    local name=$1 expected=$2; shift 2
    $NS $BIN --reset veth0 >/dev/null 2>&1
    if (( $# )); then
        $NS $BIN --interface veth0 "$@" 2>/dev/null >/dev/null || { echo "$name: emulator failed"; return; }
    fi
    local cfg; cfg=$($NS $BIN --status veth0)
    local out; out=$($NS ping -c "$COUNT" -i 0.2 -W 3 -q $TARGET 2>&1)
    local loss rtt
    loss=$(grep -oE '[0-9.]+% packet loss' <<<"$out")
    rtt=$(grep -E '^rtt|^round-trip' <<<"$out" | sed 's/.*= //')
    printf '%-26s | %-48s | %-16s | %s\n' "$name" "$cfg" "${loss:-n/a}" "${rtt:-n/a}"
    printf '   expected: %s\n' "$expected"
}

echo "pings per test: $COUNT (RTT format: min/avg/max/mdev ms)"
echo
run_test "Test 1 - Normal"       "RTT ~0.05 ms, 0% loss"
run_test "Test 2 - High latency" "RTT ~200 ms, 0% loss"                       --delay 200
run_test "Test 3 - Packet loss"  "RTT ~0.05 ms, ~10% loss (random, +-)"       --loss 10
run_test "Test 4 - Combined"     "RTT ~200 ms (170-230), ~10% loss"           --delay 200 --jitter 30 --loss 10

$NS $BIN --reset veth0
