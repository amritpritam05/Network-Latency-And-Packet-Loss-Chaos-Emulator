# Network Latency and Packet Loss Chaos Emulator

A small Linux command-line tool (C++17) that makes a network interface behave like a bad
network - added latency, jitter and packet loss - by talking to the kernel's traffic-control
subsystem **directly over Netlink**. There is no `system("tc ...")` anywhere in the code.

## 1. Project objective

Study how applications and protocols behave on poor networks, safely, without touching the
host's real Internet connection. Impairments are applied to a `veth` pair inside network
namespaces, so only the test traffic is affected.

## 2. Features

- **Latency, jitter, packet loss** via the kernel's NetEm qdisc (`--delay`, `--jitter`, `--loss`)
- **`--status`** reads the live configuration back from the kernel (not from a local file)
- **`--reset`** removes NetEm and restores normal networking; it only ever removes a `netem` qdisc
- **`--monitor`** small `AF_PACKET` sniffer: packet count, size, source/destination IP, protocol
- Strict input validation, refuses real (non-virtual) interfaces unless `--force` is given
- Clear error messages (missing interface, no permission, kernel without netem, ...)
- ~740 lines of C++ in 4 source files, no third-party libraries

## 3. Architecture

```
chaos-emulator (user space)
  main.cpp ............ CLI parsing + validation, interface lookup, output
  traffic_control.cpp . builds RTM_NEWQDISC / RTM_GETQDISC / RTM_DELQDISC with netem options
  netlink.cpp ......... AF_NETLINK socket, message + attribute builder, ACK / error handling
  packet_monitor.cpp .. AF_PACKET socket, Ethernet/IPv4 header parsing
=============================== kernel boundary ===============================
  rtnetlink -> traffic-control core -> netem qdisc -> net_device (veth0) -> veth peer
```

Test network (built by `scripts/setup_env.sh`, test tooling only):

```
 [ns client]                          [ns server]
  veth0 10.0.0.1/24  <==== veth ====>  veth1 10.0.0.2/24
  (netem attached here, egress)
```

The emulator is a *controller*, not a device in the data path: it configures a qdisc, and the
kernel does the delaying and dropping. Run it inside the client namespace with
`ip netns exec client ...`.

```
include/  netlink.hpp  traffic_control.hpp  packet_monitor.hpp
src/      main.cpp  netlink.cpp  traffic_control.cpp  packet_monitor.cpp
scripts/  setup_env.sh  teardown_env.sh  run_experiments.sh     (test tooling, not the project)
docs/     viva.md
```

## 4. Linux concepts used

**Netlink.** A socket family (`AF_NETLINK`, `NETLINK_ROUTE`) for configuring kernel networking
from user space. `ip` and `tc` are Netlink clients. A message is `[nlmsghdr][tcmsg][attributes]`;
the kernel answers with an ACK or an error code. We build these bytes ourselves.

**qdisc and NetEm.** A qdisc (queueing discipline) sits between the IP layer and the device and
decides when a packet is handed to the driver. NetEm is a qdisc that holds each packet for
`delay ± jitter` and randomly drops a percentage. It acts on **egress** only, so a 200 ms delay
on `veth0` makes a ping RTT about 200 ms (the reply path is not delayed).

**`struct net_device`.** The kernel's object for any network interface, physical or virtual. It
carries the name, index, MTU, queues, the attached qdisc and the driver callbacks
(`ndo_start_xmit` for transmit). `veth` is a virtual driver: its transmit function hands the
packet straight to the peer device's receive path. No hardware, DMA or interrupts are involved.

**`sk_buff`.** The kernel's packet buffer. One `sk_buff` travels the whole path; netem keeps it
in its queue until its release time, or frees it when the packet is "lost".

**Packet path (TX):**
`app -> socket -> TCP/UDP -> IP -> routing -> qdisc (netem) -> net_device -> driver -> wire (or veth peer RX)`.
On RX a real NIC raises an interrupt and NAPI polls the received frames up into the stack.

**What this project implements vs. what Linux provides.** We implement the user-space controller
(Netlink client + monitor). The NetEm qdisc, `net_device`, the veth driver and the packet path
are existing Linux kernel code. A hardware NIC driver is out of scope: it needs specific hardware
and DMA/interrupt code, and this project's goal is controlling the kernel's traffic-control layer.

## 5. Installation

```bash
# Debian / Ubuntu / WSL2 Ubuntu
sudo apt install build-essential iproute2 iputils-ping linux-libc-dev
# Fedora: sudo dnf install gcc-c++ make iproute iputils kernel-headers

sudo modprobe sch_netem && lsmod | grep netem      # the kernel must provide netem
```

`iproute2` and `ping` are used only for setting up the namespaces and for measuring.

## 6. Compilation

```bash
make            # build ./chaos-emulator   (g++ -std=c++17 -Wall -Wextra -O2)
make clean
make debug      # -g, AddressSanitizer      make release   # optimised + stripped
```

## 7. Usage

```bash
sudo scripts/setup_env.sh                          # create namespaces + veth pair (once per boot)

NS="sudo ip netns exec client"
$NS ./chaos-emulator --interface veth0 --delay 200 --jitter 30 --loss 10
$NS ./chaos-emulator --status  veth0
$NS ./chaos-emulator --monitor veth0 --count 20    # in another terminal, while pinging
$NS ./chaos-emulator --reset   veth0

sudo scripts/teardown_env.sh                       # remove everything
```

Options: `--delay <ms>` (0-60000), `--jitter <ms>` (0-60000), `--loss <percent>` (0-100, decimals
allowed), `--count <n>` / `--seconds <s>` for `--monitor`, `--force` for physical interfaces.
Invalid input (negative, non-numeric, out of range, odd interface names) is rejected before any
Netlink message is sent. Exit codes: 0 ok, 1 runtime error, 2 usage error.

## 8. Testing

```bash
sudo scripts/setup_env.sh
sudo scripts/run_experiments.sh          # 50 pings per test, prints a results table
```

| Test | Configuration | Expected |
|---|---|---|
| 1 Normal | no netem | RTT ~0.05 ms, 0% loss |
| 2 High latency | `--delay 200` | RTT ~200 ms, 0% loss |
| 3 Packet loss | `--loss 10` | RTT ~0.05 ms, about 10% loss |
| 4 Combined | `--delay 200 --jitter 30 --loss 10` | RTT 170-230 ms, about 10% loss |

Cross-check any state with the independent tool: `sudo ip netns exec client tc qdisc show dev veth0`.

## 9. Results

Measured with `ping` on Ubuntu (WSL2), veth pair, impairments set by `chaos-emulator`:

| Test | Result |
|---|---|
| 1 Normal | 3 pings: avg 0.061 ms, 0% loss |
| 2 High latency | *not measured yet - run `scripts/run_experiments.sh` and paste here* |
| 3 Packet loss | *not measured yet - run `scripts/run_experiments.sh` and paste here* |
| 4 Combined | 20 pings: min/avg/max = 174.2 / 202.4 / 227.2 ms, mdev 14.7 ms, 2 of 20 lost (10%) |

`tc qdisc show` confirmed the kernel held `delay 200ms 30ms loss 10%`. Interpretation: the average
matches the configured delay, the spread stays inside delay ± jitter, and loss is random, so a small
sample (20 pings) only approximates 10%; use more pings for a tighter estimate. Observed vs. estimated:
RTT and loss counts are measured by `ping`; "jitter" here is the spread of those samples.

## 10. Limitations

- NetEm is applied to **egress** only; for symmetric delay apply it on both veth ends.
- Delay and jitter are whole milliseconds; rate limiting, duplication, corruption and reordering are not exposed.
- `apply` replaces the interface's root qdisc. On a real NIC that would replace `mq`/`fq_codel`, which is why non-virtual interfaces need `--force`. `--reset` restores the kernel default qdisc, which may not be the one you had before.
- The monitor shows IPv4/ARP only. `AF_PACKET` taps run just before the driver transmits, i.e. after the qdisc, so dropped packets are never seen.
- Needs root (`CAP_NET_ADMIN` / `CAP_NET_RAW`). Namespaces do not survive a reboot or WSL restart.
- The code uses the 64-bit NetEm attributes (`TCA_NETEM_LATENCY64`/`JITTER64`). Check your headers: `grep LATENCY64 /usr/include/linux/pkt_sched.h`.

## 11. Future scope

Bandwidth limiting (`TCA_NETEM_RATE`), duplication / corruption / reordering, a symmetric
"both directions" mode, packet timestamps and latency statistics in the monitor, TCP/UDP test
programs, and an optional kernel module exposing counters through `/proc`.
