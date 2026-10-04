// chaos-emulator - inject latency, jitter and packet loss on a Linux interface
// by talking to the kernel's traffic-control subsystem over Netlink.
//
//   user space (this program) --Netlink--> kernel: tc core -> netem qdisc -> net_device
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <net/if.h>
#include <optional>
#include <stdexcept>
#include <string>

#include "netlink.hpp"
#include "packet_monitor.hpp"
#include "traffic_control.hpp"

namespace {

enum class Mode { None, Apply, Status, Reset, Monitor };

// Thrown for bad command-line input (exit code 2); everything else exits with 1.
struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Options {
    Mode mode = Mode::None;
    std::string ifname;
    std::optional<uint32_t> delay, jitter, count, seconds;
    std::optional<double> loss;
    bool force = false;
    bool help  = false;
};

void print_usage(FILE* out) {
    std::fputs(
        "chaos-emulator - network latency / jitter / packet-loss emulator (Linux, NetEm via Netlink)\n"
        "\n"
        "Usage:\n"
        "  chaos-emulator --interface <if> [--delay <ms>] [--jitter <ms>] [--loss <percent>] [--force]\n"
        "  chaos-emulator --status  <if>\n"
        "  chaos-emulator --reset   <if>\n"
        "  chaos-emulator --monitor <if> [--count <n>] [--seconds <s>]\n"
        "  chaos-emulator --help\n"
        "\n"
        "Options:\n"
        "  --interface <if>   apply NetEm to the egress of this interface\n"
        "  --delay <ms>       fixed latency, whole milliseconds, 0-60000\n"
        "  --jitter <ms>      random variation around the delay (+/- ms), 0-60000\n"
        "  --loss <percent>   packet loss probability, 0-100 (decimals allowed, e.g. 2.5)\n"
        "  --status <if>      show the active NetEm configuration\n"
        "  --reset <if>       remove NetEm and return to normal networking\n"
        "  --monitor <if>     print packets seen on the interface (AF_PACKET) + summary\n"
        "  --count <n>        monitor: stop after n packets\n"
        "  --seconds <s>      monitor: stop after s seconds\n"
        "  --force            allow changing a non-virtual (physical) interface\n"
        "\n"
        "Needs root (sudo). Intended for veth test interfaces inside a network namespace, e.g.\n"
        "  sudo ip netns exec client ./chaos-emulator --interface veth0 --delay 200 --jitter 30 --loss 10\n",
        out);
}

// ---------- strict input parsing ----------

// Whole non-negative decimal number. Rejects signs, spaces, hex, exponents, overflow.
uint32_t parse_uint(const std::string& name, const char* s, uint64_t max) {
    if (!s || !*s) throw UsageError(name + ": empty value");
    for (const char* p = s; *p; ++p)
        if (*p < '0' || *p > '9')
            throw UsageError(name + ": '" + s + "' is not a non-negative whole number");
    if (std::strlen(s) > 10) throw UsageError(name + ": '" + s + "' has too many digits");
    unsigned long long v = std::strtoull(s, nullptr, 10);
    if (v > max)
        throw UsageError(name + ": " + s + " is out of range (maximum " + std::to_string(max) + ")");
    return static_cast<uint32_t>(v);
}

// Decimal number like 10 or 2.5, range 0-100. Rejects signs, exponents, nan/inf.
double parse_percent(const std::string& name, const char* s) {
    if (!s || !*s) throw UsageError(name + ": empty value");
    int dots = 0, digits = 0;
    for (const char* p = s; *p; ++p) {
        if (*p >= '0' && *p <= '9') ++digits;
        else if (*p == '.' && ++dots == 1) continue;
        else throw UsageError(name + ": '" + s + "' is not a number like 10 or 2.5");
    }
    if (digits == 0 || std::strlen(s) > 12)
        throw UsageError(name + ": '" + s + "' is not a valid percentage");
    double v = std::strtod(s, nullptr);
    if (v < 0.0 || v > 100.0)
        throw UsageError(name + ": " + s + " is out of range (0-100)");
    return v;
}

// Interface names: 1-15 chars of [A-Za-z0-9_.-]. Keeps odd input out of everything below.
std::string parse_ifname(const char* s) {
    std::string n = s ? s : "";
    if (n.empty() || n.size() >= IF_NAMESIZE)
        throw UsageError("interface name must be 1-" + std::to_string(IF_NAMESIZE - 1) + " characters");
    for (char c : n) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '.';
        if (!ok) throw UsageError("interface name '" + n + "' contains an invalid character");
    }
    return n;
}

Options parse_args(int argc, char** argv) {
    Options o;
    auto need_value = [&](int& i, const std::string& opt) -> const char* {
        if (i + 1 >= argc) throw UsageError(opt + " requires a value");
        return argv[++i];
    };
    auto set_mode = [&](Mode m, const std::string& opt, const char* ifname) {
        if (o.mode != Mode::None)
            throw UsageError(opt + ": only one of --interface / --status / --reset / --monitor may be used");
        o.mode = m;
        o.ifname = parse_ifname(ifname);
    };
    auto once = [](auto& field, const std::string& opt) {
        if (field) throw UsageError(opt + " given more than once");
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h")      o.help = true;
        else if (a == "--force")             o.force = true;
        else if (a == "--interface")         set_mode(Mode::Apply,   a, need_value(i, a));
        else if (a == "--status")            set_mode(Mode::Status,  a, need_value(i, a));
        else if (a == "--reset")             set_mode(Mode::Reset,   a, need_value(i, a));
        else if (a == "--monitor")           set_mode(Mode::Monitor, a, need_value(i, a));
        else if (a == "--delay")   { once(o.delay, a);   o.delay   = parse_uint(a, need_value(i, a), kMaxMs); }
        else if (a == "--jitter")  { once(o.jitter, a);  o.jitter  = parse_uint(a, need_value(i, a), kMaxMs); }
        else if (a == "--loss")    { once(o.loss, a);    o.loss    = parse_percent(a, need_value(i, a)); }
        else if (a == "--count")   { once(o.count, a);   o.count   = parse_uint(a, need_value(i, a), 1000000000ULL); }
        else if (a == "--seconds") { once(o.seconds, a); o.seconds = parse_uint(a, need_value(i, a), 86400); }
        else throw UsageError("unknown option '" + a + "'");
    }
    if (o.help) return o;

    if (o.mode == Mode::None) throw UsageError("no action given (use --interface, --status, --reset or --monitor)");

    bool has_netem = o.delay || o.jitter || o.loss;
    if (o.mode != Mode::Apply && (has_netem || o.force))
        throw UsageError("--delay / --jitter / --loss / --force only apply together with --interface");
    if (o.mode != Mode::Monitor && (o.count || o.seconds))
        throw UsageError("--count / --seconds only apply together with --monitor");
    if (o.mode == Mode::Apply && !has_netem)
        throw UsageError("nothing to apply: give at least one of --delay, --jitter, --loss");
    return o;
}

// ---------- helpers ----------

int resolve_ifindex(const std::string& name) {
    unsigned idx = if_nametoindex(name.c_str());
    if (idx == 0)
        throw std::runtime_error("interface '" + name + "' not found in this network namespace "
                                 "(inside a namespace use: ip netns exec <ns> ./chaos-emulator ...)");
    return static_cast<int>(idx);
}

// True if sysfs says the device is virtual (veth, bridge, loopback, ...), not a real NIC.
bool is_virtual_iface(const std::string& name) {
    std::string path = "/sys/class/net/" + name;
    char resolved[PATH_MAX];
    if (!realpath(path.c_str(), resolved)) return false;
    return std::strstr(resolved, "/devices/virtual/") != nullptr;
}

void print_status(const std::string& ifname, const std::optional<NetemConfig>& s) {
    if (s)
        std::printf("%s: netem active   delay=%u ms   jitter=%u ms   loss=%.2f %%\n",
                    ifname.c_str(), s->delay_ms, s->jitter_ms, s->loss_pct);
    else
        std::printf("%s: no netem qdisc (normal networking)\n", ifname.c_str());
}

int run(const Options& o) {
    int ifindex = resolve_ifindex(o.ifname);

    switch (o.mode) {
    case Mode::Apply: {
        if (!is_virtual_iface(o.ifname) && !o.force)
            throw std::runtime_error("'" + o.ifname + "' does not look like a virtual interface. "
                "Changing a real interface can cut your own connection (SSH, Internet). "
                "Use a veth pair in a namespace, or pass --force if you really mean it.");

        std::fprintf(stderr, "WARNING: changing traffic control on '%s' (egress). "
                             "Undo with: --reset %s\n", o.ifname.c_str(), o.ifname.c_str());
        if (o.jitter && o.jitter.value_or(0) > o.delay.value_or(0))
            std::fprintf(stderr, "note: jitter is larger than delay; some packets will get ~0 delay "
                                 "and may be reordered.\n");

        NetemConfig cfg;
        cfg.delay_ms  = o.delay.value_or(0);
        cfg.jitter_ms = o.jitter.value_or(0);
        cfg.loss_pct  = o.loss.value_or(0.0);

        NetlinkSocket nl;
        TrafficControl tc(nl);
        tc.apply(ifindex, cfg);
        std::printf("applied. ");
        print_status(o.ifname, tc.status(ifindex));   // read back what the kernel really holds
        return 0;
    }
    case Mode::Status: {
        NetlinkSocket nl;
        print_status(o.ifname, TrafficControl(nl).status(ifindex));
        return 0;
    }
    case Mode::Reset: {
        NetlinkSocket nl;
        if (TrafficControl(nl).reset(ifindex))
            std::printf("%s: netem removed (normal networking restored)\n", o.ifname.c_str());
        else
            std::printf("%s: no netem qdisc, nothing to reset\n", o.ifname.c_str());
        return 0;
    }
    case Mode::Monitor: {
        PacketMonitor mon(ifindex);
        std::fprintf(stderr, "monitoring %s (Ctrl+C to stop)...\n", o.ifname.c_str());
        mon.run(o.count.value_or(0), o.seconds.value_or(0));
        return 0;
    }
    default: break;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options o = parse_args(argc, argv);
        if (o.help) { print_usage(stdout); return 0; }
        return run(o);
    } catch (const UsageError& e) {
        std::fprintf(stderr, "error: %s\n(try --help)\n", e.what());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        if (std::strstr(e.what(), "Operation not permitted"))
            std::fprintf(stderr, "hint: this needs root privileges (use sudo).\n");
        return 1;
    }
}
