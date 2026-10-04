# Viva notes

## Likely questions

**1. What does the project do?**
It injects latency, jitter and packet loss on a Linux interface so we can study how applications
behave on bad networks. It does this by configuring the kernel's NetEm qdisc over Netlink.

**2. What is Netlink and why not just call `tc`?**
Netlink is a socket family (`AF_NETLINK`) for user space to kernel communication about networking.
`tc` and `ip` are only Netlink clients. We build the same messages (`RTM_NEWQDISC`, `RTM_GETQDISC`,
`RTM_DELQDISC`) ourselves, so there is no shell, no `system()`, and no command-injection surface.
Netlink also returns structured errors (errno) and supports kernel-to-user notifications.

**3. What is a qdisc?**
A queueing discipline: the layer between the IP stack and the device driver that decides when and
whether a packet is transmitted. Default ones are simple FIFOs; NetEm adds delay, jitter and loss.

**4. How does NetEm work internally?**
On enqueue it may drop the packet (probability from `loss`), otherwise computes
`send_time = now + delay ± random(jitter)` and queues the `sk_buff` in time order. On dequeue it
releases packets whose time has come. We only configure it; the kernel does the work.

**5. What is the layout of the message you send?**
`[nlmsghdr][tcmsg][TCA_KIND="netem"][TCA_OPTIONS { tc_netem_qopt, TCA_NETEM_LATENCY64, TCA_NETEM_JITTER64 }]`.
`tcmsg` names the interface index, the parent (`TC_H_ROOT`) and the handle (`1:`). Flags are
`NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE | NLM_F_ACK`.

**6. What was the hardest bug?**
The program applied netem but then hung. `NLM_F_REPLACE` has the same value as `NLM_F_ROOT`, and
`NLM_F_DUMP` is `ROOT|MATCH`. My test `flags & NLM_F_DUMP` treated the request as a dump, so no ACK
was requested and `recv()` waited forever. Fix: compare against the full mask.

**7. What is `struct net_device`?**
The kernel's representation of a network interface (physical or virtual): name, ifindex, MTU,
queues, attached qdisc, statistics and driver operations such as `ndo_start_xmit`.

**8. What is `sk_buff`?**
The kernel's packet buffer. The same `sk_buff` is passed through socket, TCP/IP, qdisc and driver;
headers are added/removed by moving pointers instead of copying data.

**9. Why veth and namespaces?**
A veth pair is a virtual Ethernet cable whose ends are real `net_device`s, so qdiscs work exactly
as on a NIC. Putting the ends in separate namespaces isolates the experiment from the host network.

**10. Did you write a network driver?**
No. A hardware NIC driver needs specific hardware plus DMA, interrupts and NAPI polling code. We
use Linux's existing veth driver and control the traffic-control layer above it. The project is
user-space only, and I say so explicitly.

**11. Why does 200 ms delay show up as ~200 ms RTT and not 400?**
NetEm acts on egress of `veth0` only. The request is delayed once; the reply leaves `veth1`, which
has no netem. For 400 ms RTT apply it on both ends.

**12. How is the tool made safe?**
Strict parsing (digits only, ranges, interface-name whitelist), no shell calls, refuses non-virtual
interfaces without `--force`, warns before changing, `--reset` only removes a `netem` qdisc.
Root is required, and root software that edits networking can disconnect the machine, hence the
namespace setup and the guard.

**13. What does the monitor show and what are its limits?**
Packet count, size, IPv4 addresses and protocol from an `AF_PACKET` raw socket. It sees frames after
the qdisc, so dropped packets never appear; it cannot measure latency or loss by itself (that
needs matching packets across two points, or a tool like ping).

## Presentation points

1. Problem: testing apps on bad networks. 2. Architecture: CLI -> Netlink -> tc/netem -> veth.
3. Demo: ping before / after `--delay 200 --jitter 30 --loss 10`, then `--status`, then `--reset`.
4. Show `tc qdisc show` agreeing with `--status`. 5. Packet path and what is ours vs. the kernel's.
6. The `NLM_F_*` bug story. 7. Limits and future scope.

## Resume line

*Network Latency and Packet Loss Chaos Emulator (C++17, Linux)* - Built a CLI that configures the
kernel NetEm qdisc directly through raw rtnetlink messages (no `tc` shell-out) to emulate latency,
jitter and packet loss on veth interfaces inside network namespaces; added an AF_PACKET packet
monitor, strict input validation and safe reset, and validated results with ping experiments.
