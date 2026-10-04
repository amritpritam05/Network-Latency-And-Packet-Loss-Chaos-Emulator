// packet_monitor.hpp - tiny AF_PACKET sniffer (packet count, size, IPs, protocol).
#pragma once
#include <cstdint>

class PacketMonitor {
public:
    // Opens an AF_PACKET socket bound to one interface. Throws std::runtime_error.
    explicit PacketMonitor(int ifindex);
    ~PacketMonitor();
    PacketMonitor(const PacketMonitor&) = delete;
    PacketMonitor& operator=(const PacketMonitor&) = delete;

    // Print one line per frame and a summary at the end. Stops after max_packets
    // frames (0 = no limit), after max_seconds (0 = no limit), or on Ctrl+C.
    void run(uint64_t max_packets, uint32_t max_seconds);

private:
    int fd_;
};
