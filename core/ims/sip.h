// SPDX-License-Identifier: GPL-2.0-or-later
//
// SIP messages (RFC 3261) as an IMS phone needs them: parsing what the
// network sends, building requests and responses. Header names are kept in
// their long form (compact forms are expanded when parsing) and compared
// without regard to case. Everything here is network data: the parser
// checks every length and never trusts a count.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shannond::ims {

bool iequals(std::string_view a, std::string_view b);
std::string_view trim(std::string_view s);

// ";a=b;c" style parameters, in order
struct Params {
    std::vector<std::pair<std::string, std::string>> items;

    std::optional<std::string> get(std::string_view name) const;
    bool has(std::string_view name) const { return get(name).has_value(); }
    void set(std::string name, std::string value = {});
    void remove(std::string_view name);
    std::string str() const;  // with the leading ';'

    // Parses parameters after the first ';' (quoted values keep their quotes)
    static Params parse(std::string_view s);
};

// Splits a header value at commas outside quotes and angle brackets
std::vector<std::string> splitList(std::string_view value);
// Without surrounding quotes and backslash escapes
std::string unquote(std::string_view s);
std::string quote(std::string_view s);

// A sip:/sips:/tel: URI, enough to route and compare
struct Uri {
    std::string scheme;  // "sip", "sips", "tel"
    std::string user;    // also the tel: number
    std::string host;    // without brackets for IPv6
    uint16_t port = 0;   // 0 = not given
    Params params;

    std::string str() const;
    static std::optional<Uri> parse(std::string_view s);
};

// "Display" <uri>;params, as in From, To, Contact, Route
struct NameAddr {
    std::string display;
    std::string uri;  // as written
    Params params;

    std::string str() const;
    static std::optional<NameAddr> parse(std::string_view s);
};

// One Via entry
struct Via {
    std::string transport;  // "UDP", "TCP"
    std::string host;
    uint16_t port = 0;
    Params params;

    std::string str() const;
    static std::optional<Via> parse(std::string_view s);
};

class SipMessage {
public:
    static SipMessage request(std::string method, std::string uri);
    static SipMessage response(int status, std::string reason);

    bool isRequest() const { return status_ == 0; }
    const std::string &method() const { return method_; }  // requests
    const std::string &requestUri() const { return uri_; }
    void setRequestUri(std::string uri) { uri_ = std::move(uri); }
    int status() const { return status_; }  // responses
    const std::string &reason() const { return reason_; }

    // The first value of a header, if present
    std::optional<std::string> header(std::string_view name) const;
    // Every value, header lines and comma lists both split (only for the
    // headers whose grammar is a list)
    std::vector<std::string> headers(std::string_view name) const;
    bool has(std::string_view name) const { return header(name).has_value(); }
    void add(std::string name, std::string value);
    // Replaces every line of the header with one
    void set(std::string name, std::string value);
    void remove(std::string_view name);
    const std::vector<std::pair<std::string, std::string>> &allHeaders() const { return headers_; }

    std::string body;

    // CSeq number and method
    std::optional<std::pair<uint32_t, std::string>> cseq() const;
    std::optional<std::string> callId() const { return header("Call-ID"); }
    // The top Via's branch
    std::optional<std::string> branch() const;
    // Option tags of Require/Supported/Proxy-Require
    bool hasOption(std::string_view header, std::string_view tag) const;

    // With Content-Length set from the body
    std::string serialize() const;
    static std::optional<SipMessage> parse(std::string_view text);

    // A response to this request: Via, From, To, Call-ID, CSeq copied
    SipMessage makeResponse(int status, std::string reason) const;

private:
    std::string method_;
    std::string uri_;
    int status_ = 0;
    std::string reason_;
    std::vector<std::pair<std::string, std::string>> headers_;
};

// For stream transports: the length of the first whole message in buffer,
// 0 if more is needed, nullopt if the stream is broken (no recovery: the
// connection must be dropped). Leading CRLF keep-alives count as a message.
std::optional<size_t> sipFrameLength(std::string_view buffer);

// Random lowercase hex, n characters (for tags, branches, Call-ID)
std::string randomHex(size_t n);
// A new Via branch with the RFC 3261 magic cookie
std::string newBranch();

// The methods we take, in every Allow header (REGISTER, calls, OPTIONS answers)
inline constexpr const char *kAllow = "INVITE, ACK, CANCEL, BYE, UPDATE, PRACK, NOTIFY, OPTIONS, INFO";

}  // namespace shannond::ims
