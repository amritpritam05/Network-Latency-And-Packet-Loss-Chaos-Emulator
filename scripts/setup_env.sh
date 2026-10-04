#!/usr/bin/env bash
# TEST TOOLING ONLY - not part of the C++ implementation.
# Builds the isolated test network:
#
#   [ns client]                         [ns server]
#    veth0 10.0.0.1/24  <== veth ==>  veth1 10.0.0.2/24
#
# Both ends are on the same /24, so the kernel adds the connected route automatically.
# Namespaces live in memory: re-run this script after a reboot / WSL restart.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then echo "run as root: sudo $0" >&2; exit 1; fi

ip netns del client 2>/dev/null || true       # start clean (deleting a ns destroys its veth)
ip netns del server 2>/dev/null || true

ip netns add client
ip netns add server
ip link add veth0 type veth peer name veth1
ip link set veth0 netns client
ip link set veth1 netns server

ip netns exec client ip addr add 10.0.0.1/24 dev veth0
ip netns exec server ip addr add 10.0.0.2/24 dev veth1
ip netns exec client ip link set lo up
ip netns exec client ip link set veth0 up
ip netns exec server ip link set lo up
ip netns exec server ip link set veth1 up

modprobe sch_netem 2>/dev/null || echo "note: could not load sch_netem (may be built in, or missing)" >&2

echo "test network ready:"
ip netns exec client ip -br addr show veth0
ip netns exec server ip -br addr show veth1
ip netns exec client ping -c 2 -W 1 10.0.0.2
