// traffic_control.hpp - configures the NetEm qdisc through Netlink.
#pragma once
#include <cstdint>
#include <optional>
#include "netlink.hpp"

struct NetemConfig {
    uint32_t delay_ms  = 0;     // 0 .. kMaxMs
    uint32_t jitter_ms = 0;     // 0 .. kMaxMs
    double   loss_pct  = 0.0;   // 0 .. 100
};

constexpr uint32_t kMaxMs = 60000;

class TrafficControl {
public:
    explicit TrafficControl(NetlinkSocket& nl) : nl_(nl) {}

    // Install (or replace) a netem root qdisc on the interface (RTM_NEWQDISC).
    void apply(int ifindex, const NetemConfig& cfg);

    // Read back the root qdisc (RTM_GETQDISC). Returns a value only if it is netem.
    std::optional<NetemConfig> status(int ifindex);

    // Remove the root qdisc (RTM_DELQDISC) only if it is netem. Returns false if there
    // was none. Never deletes a qdisc that this tool did not create.
    bool reset(int ifindex);

private:
    NetlinkSocket& nl_;
};
