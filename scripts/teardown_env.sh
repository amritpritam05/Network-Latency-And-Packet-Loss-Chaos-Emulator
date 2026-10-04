#!/usr/bin/env bash
# TEST TOOLING ONLY - removes the test network (deleting a namespace destroys its veth end).
set -euo pipefail
if [[ $EUID -ne 0 ]]; then echo "run as root: sudo $0" >&2; exit 1; fi
ip netns del client 2>/dev/null || true
ip netns del server 2>/dev/null || true
echo "test network removed"
