#include "packet_monitor.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <linux/if_packet.h>   // sockaddr_ll, PACKET_OUTGOING

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

const char* proto_name(uint8_t p, char* buf, size_t n) {
    switch (p) {
        case IPPROTO_TCP:  return "TCP";
        case IPPROTO_UDP:  return "UDP";
        case IPPROTO_ICMP: return "ICMP";
        default: std::snprintf(buf, n, "IP-proto-%u", p); return buf;
    }
}
}

PacketMonitor::PacketMonitor(int ifindex) : fd_(-1) {
    // SOCK_RAW: we receive whole Ethernet frames, headers included.
    // ETH_P_ALL: every protocol (the argument must be in network byte order).
    fd_ = ::socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
    if (fd_ < 0) {
        std::string msg = std::string("socket(AF_PACKET) failed: ") + std::strerror(errno);
        if (errno == EPERM || errno == EACCES) msg += " (packet capture needs root / CAP_NET_RAW)";
        throw std::runtime_error(msg);
    }

    sockaddr_ll addr{};
    addr.sll_family   = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ALL);
    addr.sll_ifindex  = ifindex;                  // only look at this interface
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int e = errno;
        ::close(fd_);
        throw std::runtime_error(std::string("bind(AF_PACKET) failed: ") + std::strerror(e));
    }

    // Wake up every 500 ms so Ctrl+C and the time limit are noticed on an idle link.
    timeval tv{0, 500000};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

PacketMonitor::~PacketMonitor() {
    if (fd_ >= 0) ::close(fd_);
}

void PacketMonitor::run(uint64_t max_packets, uint32_t max_seconds) {
    g_stop = 0;
    struct sigaction sa{};
    sa.sa_handler = on_signal;                    // no SA_RESTART: recv() returns EINTR
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    uint64_t packets = 0, bytes = 0;
    std::vector<unsigned char> buf(65536);

    while (!g_stop) {
        if (max_packets && packets >= max_packets) break;
        auto elapsed = std::chrono::duration<double>(clock::now() - start).count();
        if (max_seconds && elapsed >= max_seconds) break;

        sockaddr_ll from{};
        socklen_t fromlen = sizeof(from);
        ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromlen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            throw std::runtime_error(std::string("recvfrom(AF_PACKET) failed: ") + std::strerror(errno));
        }
        if (n < static_cast<ssize_t>(sizeof(ether_header))) continue;   // runt frame

        ++packets;
        bytes += static_cast<uint64_t>(n);

        // PACKET_OUTGOING = frame this host is sending out of the interface.
        const char* dir = (from.sll_pkttype == PACKET_OUTGOING) ? "OUT" : "IN ";

        auto* eth = reinterpret_cast<const ether_header*>(buf.data());
        uint16_t ethertype = ntohs(eth->ether_type);

        char src[INET_ADDRSTRLEN] = "-", dst[INET_ADDRSTRLEN] = "-", pbuf[24];
        const char* proto = "other";

        if (ethertype == ETHERTYPE_IP &&
            n >= static_cast<ssize_t>(sizeof(ether_header) + sizeof(iphdr))) {
            // The IP header starts 14 bytes into the frame, so it is not 4-byte aligned.
            // Copy it out instead of dereferencing a cast pointer (misaligned access is UB).
            iphdr ip;
            std::memcpy(&ip, buf.data() + sizeof(ether_header), sizeof(ip));
            inet_ntop(AF_INET, &ip.saddr, src, sizeof(src));
            inet_ntop(AF_INET, &ip.daddr, dst, sizeof(dst));
            proto = proto_name(ip.protocol, pbuf, sizeof(pbuf));
        } else if (ethertype == ETHERTYPE_ARP) {
            proto = "ARP";
        } else if (ethertype == ETHERTYPE_IPV6) {
            proto = "IPv6";
        } else {
            std::snprintf(pbuf, sizeof(pbuf), "eth-0x%04x", ethertype);
            proto = pbuf;
        }

        std::printf("#%-5llu %s len=%-5zd %-15s -> %-15s %s\n",
                    static_cast<unsigned long long>(packets), dir, n, src, dst, proto);
        std::fflush(stdout);
    }

    double secs = std::chrono::duration<double>(clock::now() - start).count();
    std::printf("\n--- monitor summary ---\n");
    std::printf("packets : %llu\n", static_cast<unsigned long long>(packets));
    std::printf("bytes   : %llu\n", static_cast<unsigned long long>(bytes));
    std::printf("avg size: %.1f bytes\n", packets ? static_cast<double>(bytes) / packets : 0.0);
    std::printf("duration: %.2f s  (%.1f packets/s)\n", secs, secs > 0 ? packets / secs : 0.0);
}
