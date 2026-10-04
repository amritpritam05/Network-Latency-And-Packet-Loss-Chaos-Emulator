#include "traffic_control.hpp"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <linux/pkt_sched.h>   // tc_netem_qopt, TCA_NETEM_*, TC_H_ROOT, TC_H_MAKE

namespace {
constexpr uint32_t kQueueLimit = 1000;           // packets; same default as `tc`
constexpr int64_t  kNsPerMs    = 1000000;
constexpr double   kProbScale  = 4294967295.0;   // netem probabilities: 0xFFFFFFFF == 100%
}

void TrafficControl::apply(int ifindex, const NetemConfig& cfg) {
    if (ifindex <= 0)          throw std::invalid_argument("invalid interface index");
    if (cfg.delay_ms > kMaxMs) throw std::invalid_argument("delay out of range (0-60000 ms)");
    if (cfg.jitter_ms > kMaxMs) throw std::invalid_argument("jitter out of range (0-60000 ms)");
    if (!(cfg.loss_pct >= 0.0 && cfg.loss_pct <= 100.0))
        throw std::invalid_argument("loss out of range (0-100 %)");

    // Fixed part of the netem options. Loss is a 32-bit probability.
    tc_netem_qopt qopt{};
    qopt.limit = kQueueLimit;
    qopt.loss  = static_cast<uint32_t>(std::llround(cfg.loss_pct / 100.0 * kProbScale));
    // qopt.latency / qopt.jitter stay 0; the 64-bit nanosecond attributes below carry
    // the real values (this avoids the old scheduler-tick conversion).

    int64_t latency_ns = static_cast<int64_t>(cfg.delay_ms)  * kNsPerMs;
    int64_t jitter_ns  = static_cast<int64_t>(cfg.jitter_ms) * kNsPerMs;

    // Message layout:
    // [nlmsghdr][tcmsg][TCA_KIND "netem"][TCA_OPTIONS: tc_netem_qopt, LATENCY64, JITTER64]
    NlMessage m(RTM_NEWQDISC, NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE);

    tcmsg t{};
    t.tcm_family  = AF_UNSPEC;
    t.tcm_ifindex = ifindex;
    t.tcm_parent  = TC_H_ROOT;                // attach to the egress root of the device
    t.tcm_handle  = TC_H_MAKE(1U << 16, 0);   // handle "1:"
    m.put(&t, sizeof(t));

    m.addattr(TCA_KIND, "netem", sizeof("netem"));   // includes the NUL

    size_t opts = m.nest_start(TCA_OPTIONS);
    m.put(&qopt, sizeof(qopt));                      // fixed struct first ...
    m.addattr(TCA_NETEM_LATENCY64, &latency_ns, sizeof(latency_ns));   // ... then attributes
    m.addattr(TCA_NETEM_JITTER64,  &jitter_ns,  sizeof(jitter_ns));
    m.nest_end(opts);

    try {
        nl_.transact(m);   // throws with the kernel's errno text on failure
    } catch (const NetlinkError& e) {
        // ENOENT on RTM_NEWQDISC means the kernel has no qdisc called "netem".
        if (e.code() == ENOENT)
            throw std::runtime_error("this kernel has no 'netem' qdisc (kernel said: No such file or "
                                     "directory). Load it with 'sudo modprobe sch_netem'; if that fails "
                                     "your kernel was built without CONFIG_NET_SCH_NETEM.");
        throw;
    }
}

std::optional<NetemConfig> TrafficControl::status(int ifindex) {
    if (ifindex <= 0) throw std::invalid_argument("invalid interface index");

    std::optional<NetemConfig> result;

    // Dump every qdisc (the same request `tc qdisc show` sends) and filter in user space.
    NlMessage m(RTM_GETQDISC, NLM_F_REQUEST | NLM_F_DUMP);
    tcmsg t{};
    t.tcm_family = AF_UNSPEC;
    m.put(&t, sizeof(t));

    nl_.transact(m, [&](const nlmsghdr* h) {
        if (h->nlmsg_type != RTM_NEWQDISC) return;
        if (h->nlmsg_len < NLMSG_LENGTH(sizeof(tcmsg))) return;

        auto* tc = static_cast<const tcmsg*>(NLMSG_DATA(h));
        if (tc->tcm_ifindex != ifindex || tc->tcm_parent != TC_H_ROOT) return;

        int len = static_cast<int>(h->nlmsg_len - NLMSG_LENGTH(sizeof(tcmsg)));
        const rtattr* tb[TCA_MAX + 1];
        parse_attrs(TCA_RTA(tc), len, tb, TCA_MAX);

        if (!tb[TCA_KIND] || !tb[TCA_OPTIONS]) return;
        if (std::strcmp(static_cast<const char*>(RTA_DATA(tb[TCA_KIND])), "netem") != 0) return;

        // TCA_OPTIONS payload for netem = struct tc_netem_qopt, then rtattrs.
        const rtattr* o = tb[TCA_OPTIONS];
        int olen = static_cast<int>(RTA_PAYLOAD(o));
        constexpr int kQopt = static_cast<int>(NLMSG_ALIGN(sizeof(tc_netem_qopt)));
        if (olen < static_cast<int>(sizeof(tc_netem_qopt))) return;

        tc_netem_qopt q;
        std::memcpy(&q, RTA_DATA(o), sizeof(q));

        NetemConfig c;
        c.loss_pct = q.loss * 100.0 / kProbScale;

        if (olen > kQopt) {
            const rtattr* na[TCA_NETEM_MAX + 1];
            parse_attrs(reinterpret_cast<const rtattr*>(
                            static_cast<const char*>(RTA_DATA(o)) + kQopt),
                        olen - kQopt, na, TCA_NETEM_MAX);

            auto read_ms = [](const rtattr* a) -> uint32_t {
                int64_t ns = 0;
                if (!a || RTA_PAYLOAD(a) < sizeof(ns)) return 0;
                std::memcpy(&ns, RTA_DATA(a), sizeof(ns));   // memcpy: may be unaligned
                return static_cast<uint32_t>((ns + kNsPerMs / 2) / kNsPerMs);
            };
            c.delay_ms  = read_ms(na[TCA_NETEM_LATENCY64]);
            c.jitter_ms = read_ms(na[TCA_NETEM_JITTER64]);
        }
        result = c;
    });
    return result;
}

bool TrafficControl::reset(int ifindex) {
    if (!status(ifindex)) return false;     // nothing of ours to remove

    NlMessage m(RTM_DELQDISC, NLM_F_REQUEST);
    tcmsg t{};
    t.tcm_family  = AF_UNSPEC;
    t.tcm_ifindex = ifindex;
    t.tcm_parent  = TC_H_ROOT;              // handle 0: delete whatever is at the root
    m.put(&t, sizeof(t));

    nl_.transact(m);   // the kernel re-attaches the interface's default qdisc
    return true;
}
