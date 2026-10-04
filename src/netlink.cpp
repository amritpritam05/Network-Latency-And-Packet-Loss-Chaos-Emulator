#include "netlink.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

// ---------- NlMessage ----------

NlMessage::NlMessage(uint16_t type, uint16_t flags) : buf_(NLMSG_HDRLEN, 0) {
    nlmsghdr* h = hdr();
    h->nlmsg_len   = NLMSG_HDRLEN;
    h->nlmsg_type  = type;
    h->nlmsg_flags = flags;
}

size_t NlMessage::put(const void* data, size_t len) {
    size_t off = buf_.size();
    buf_.resize(off + NLMSG_ALIGN(len), 0);       // zero padding
    if (len) std::memcpy(&buf_[off], data, len);
    hdr()->nlmsg_len = static_cast<uint32_t>(buf_.size());
    return off;
}

void NlMessage::addattr(uint16_t type, const void* data, size_t len) {
    size_t off = buf_.size();
    buf_.resize(off + RTA_SPACE(len), 0);
    rtattr* rta = reinterpret_cast<rtattr*>(&buf_[off]);
    rta->rta_type = type;
    rta->rta_len  = static_cast<unsigned short>(RTA_LENGTH(len));
    if (len) std::memcpy(RTA_DATA(rta), data, len);
    hdr()->nlmsg_len = static_cast<uint32_t>(buf_.size());
}

size_t NlMessage::nest_start(uint16_t type) {
    size_t off = buf_.size();
    addattr(type, nullptr, 0);                    // length is fixed in nest_end()
    return off;
}

void NlMessage::nest_end(size_t offset) {
    rtattr* rta = reinterpret_cast<rtattr*>(&buf_[offset]);
    rta->rta_len = static_cast<unsigned short>(buf_.size() - offset);
}

// ---------- NetlinkSocket ----------

NetlinkSocket::NetlinkSocket()
    : fd_(-1), seq_(static_cast<uint32_t>(std::time(nullptr))) {
    fd_ = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd_ < 0)
        throw std::runtime_error(std::string("socket(AF_NETLINK) failed: ") + std::strerror(errno));

    sockaddr_nl local{};
    local.nl_family = AF_NETLINK;                 // pid 0: the kernel assigns our port id
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        int e = errno;
        ::close(fd_);
        throw std::runtime_error(std::string("bind(AF_NETLINK) failed: ") + std::strerror(e));
    }
}

NetlinkSocket::~NetlinkSocket() {
    if (fd_ >= 0) ::close(fd_);
}

void NetlinkSocket::transact(NlMessage& msg, const Handler& on_msg) {
    nlmsghdr* h = msg.hdr();

    // Ask for an ACK unless this is a dump (dumps end with NLMSG_DONE instead).
    // NLM_F_DUMP is two bits (ROOT|MATCH) and NLM_F_REPLACE equals NLM_F_ROOT, so
    // compare against the full mask; a plain non-zero test would mistake an
    // "add or replace" request for a dump, never ask for an ACK, and hang in recv().
    if ((h->nlmsg_flags & NLM_F_DUMP) != NLM_F_DUMP) h->nlmsg_flags |= NLM_F_ACK;
    h->nlmsg_seq = ++seq_;
    h->nlmsg_pid = 0;

    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;                // destination: the kernel (pid 0)
    if (::sendto(fd_, msg.data(), msg.size(), 0,
                 reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) < 0)
        throw std::runtime_error(std::string("sendto(netlink) failed: ") + std::strerror(errno));

    std::vector<char> buf(32768);
    for (;;) {
        ssize_t n = ::recv(fd_, buf.data(), buf.size(), MSG_TRUNC);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("recv(netlink) failed: ") + std::strerror(errno));
        }
        if (static_cast<size_t>(n) > buf.size())
            throw std::runtime_error("netlink reply larger than receive buffer");

        int len = static_cast<int>(n);
        for (nlmsghdr* r = reinterpret_cast<nlmsghdr*>(buf.data());
             NLMSG_OK(r, len); r = NLMSG_NEXT(r, len)) {
            if (r->nlmsg_seq != seq_) continue;   // not a reply to our request
            if (r->nlmsg_type == NLMSG_DONE) return;
            if (r->nlmsg_type == NLMSG_ERROR) {
                if (r->nlmsg_len < NLMSG_LENGTH(sizeof(nlmsgerr)))
                    throw std::runtime_error("malformed netlink error message");
                auto* e = static_cast<nlmsgerr*>(NLMSG_DATA(r));
                if (e->error == 0) return;        // error == 0 means ACK
                throw NetlinkError(-e->error, std::string("kernel rejected request: ") +
                                              std::strerror(-e->error));
            }
            if (on_msg) on_msg(r);
        }
    }
}

void parse_attrs(const rtattr* rta, int len, const rtattr** tb, int max) {
    std::memset(tb, 0, sizeof(*tb) * (max + 1));
    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len))
        if (rta->rta_type <= max) tb[rta->rta_type] = rta;
}
