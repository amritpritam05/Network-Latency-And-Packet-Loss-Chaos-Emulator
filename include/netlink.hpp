// netlink.hpp - minimal rtnetlink (NETLINK_ROUTE) helper.
//
// Netlink is the socket family Linux uses for user space <-> kernel networking
// configuration. `ip` and `tc` are just Netlink clients; this file is the small
// piece of them that we need: build a message, send it, read the kernel's reply.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include <stdexcept>
#include <string>

// The kernel answered a request with an error; code() is the errno value (e.g. ENOENT).
class NetlinkError : public std::runtime_error {
public:
    NetlinkError(int err, const std::string& what) : std::runtime_error(what), err_(err) {}
    int code() const { return err_; }
private:
    int err_;
};

// Builds one Netlink message: [nlmsghdr][payload struct][rtattr ...]
class NlMessage {
public:
    NlMessage(uint16_t type, uint16_t flags);

    // Append raw bytes (e.g. a tcmsg), padded to 4-byte alignment. Returns the offset.
    size_t put(const void* data, size_t len);
    // Append one attribute (struct rtattr + data).
    void addattr(uint16_t type, const void* data, size_t len);
    // Nested attributes (e.g. TCA_OPTIONS): nest_start ... nest_end(offset).
    size_t nest_start(uint16_t type);
    void nest_end(size_t offset);

    nlmsghdr*   hdr()        { return reinterpret_cast<nlmsghdr*>(buf_.data()); }
    const void* data() const { return buf_.data(); }
    size_t      size() const { return buf_.size(); }

private:
    std::vector<char> buf_;  // may reallocate while growing, so nesting uses offsets
};

// RAII wrapper around an AF_NETLINK / NETLINK_ROUTE socket.
class NetlinkSocket {
public:
    using Handler = std::function<void(const nlmsghdr*)>;

    NetlinkSocket();
    ~NetlinkSocket();
    NetlinkSocket(const NetlinkSocket&) = delete;
    NetlinkSocket& operator=(const NetlinkSocket&) = delete;

    // Send msg, call on_msg for each reply message, return on ACK / NLMSG_DONE.
    // Throws std::runtime_error with the kernel's errno text on failure.
    void transact(NlMessage& msg, const Handler& on_msg = nullptr);

private:
    int fd_;
    uint32_t seq_;
};

// Index the attributes of a buffer into tb[type]. Used when parsing replies.
void parse_attrs(const rtattr* rta, int len, const rtattr** tb, int max);
