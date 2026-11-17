// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/ims/sip.h"

#include <cctype>
#include <charconv>
#include <random>

namespace shannond::ims {

namespace {

constexpr size_t kMaxHeaders = 256;
constexpr size_t kMaxMessage = 65535;

char lower(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// RFC 3261 7.3.3 and later RFCs
struct Compact {
    char letter;
    const char *name;
};
constexpr Compact kCompact[] = {
    {'a', "Accept-Contact"}, {'b', "Referred-By"},   {'c', "Content-Type"},
    {'d', "Request-Disposition"}, {'e', "Content-Encoding"}, {'f', "From"},
    {'i', "Call-ID"},        {'j', "Reject-Contact"}, {'k', "Supported"},
    {'l', "Content-Length"}, {'m', "Contact"},        {'o', "Event"},
    {'r', "Refer-To"},       {'s', "Subject"},        {'t', "To"},
    {'u', "Allow-Events"},   {'v', "Via"},            {'x', "Session-Expires"},
    {'y', "Identity"},
};

// Headers whose grammar is a comma separated list
constexpr const char *kListHeaders[] = {
    "Via", "Contact", "Route", "Record-Route", "Service-Route", "Path", "P-Associated-URI",
    "Supported", "Require", "Proxy-Require", "Unsupported", "Allow", "Allow-Events", "Accept",
    "Accept-Encoding", "Accept-Language", "Security-Server", "Security-Client",
    "Security-Verify", "P-Asserted-Identity", "P-Preferred-Identity", "Accept-Contact",
    "Reject-Contact", "Request-Disposition", "Privacy", "Content-Encoding",
};

bool isListHeader(std::string_view name)
{
    for (const char *h : kListHeaders)
        if (iequals(name, h))
            return true;
    return false;
}

std::string expandName(std::string_view name)
{
    if (name.size() == 1)
        for (const Compact &c : kCompact)
            if (lower(name[0]) == c.letter)
                return c.name;
    return std::string(name);
}

bool parsePort(std::string_view s, uint16_t &port)
{
    unsigned value = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() || p != s.data() + s.size() || value == 0 || value > 65535)
        return false;
    port = static_cast<uint16_t>(value);
    return true;
}

// host[:port], host may be [IPv6]
bool parseHostPort(std::string_view s, std::string &host, uint16_t &port)
{
    port = 0;
    if (s.empty())
        return false;
    if (s[0] == '[') {
        size_t close = s.find(']');
        if (close == std::string_view::npos || close == 1)
            return false;
        host = std::string(s.substr(1, close - 1));
        std::string_view rest = s.substr(close + 1);
        if (rest.empty())
            return true;
        return rest[0] == ':' && parsePort(rest.substr(1), port);
    }
    size_t colon = s.find(':');
    host = std::string(s.substr(0, colon));
    if (host.empty())
        return false;
    return colon == std::string_view::npos || parsePort(s.substr(colon + 1), port);
}

std::string hostString(const std::string &host)
{
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

// The position of c outside quotes (and angle brackets if asked)
size_t findOutside(std::string_view s, char c, size_t from = 0, bool brackets = false)
{
    bool quoted = false;
    int depth = 0;
    for (size_t i = from; i < s.size(); i++) {
        char ch = s[i];
        if (quoted) {
            if (ch == '\\')
                i++;
            else if (ch == '"')
                quoted = false;
            continue;
        }
        if (ch == '"')
            quoted = true;
        else if (brackets && ch == '<')
            depth++;
        else if (brackets && ch == '>' && depth > 0)
            depth--;
        else if (ch == c && depth == 0)
            return i;
    }
    return std::string_view::npos;
}

}  // namespace

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++)
        if (lower(a[i]) != lower(b[i]))
            return false;
    return true;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

std::optional<std::string> Params::get(std::string_view name) const
{
    for (const auto &[k, v] : items)
        if (iequals(k, name))
            return v;
    return std::nullopt;
}

void Params::set(std::string name, std::string value)
{
    for (auto &[k, v] : items)
        if (iequals(k, name)) {
            v = std::move(value);
            return;
        }
    items.emplace_back(std::move(name), std::move(value));
}

void Params::remove(std::string_view name)
{
    std::erase_if(items, [&](const auto &p) { return iequals(p.first, name); });
}

std::string Params::str() const
{
    std::string out;
    for (const auto &[k, v] : items) {
        out += ';';
        out += k;
        if (!v.empty()) {
            out += '=';
            out += v;
        }
    }
    return out;
}

Params Params::parse(std::string_view s)
{
    Params p;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = findOutside(s, ';', pos);
        std::string_view item = trim(s.substr(pos, end == std::string_view::npos ? s.size() - pos : end - pos));
        if (!item.empty()) {
            size_t eq = item.find('=');
            if (eq == std::string_view::npos)
                p.items.emplace_back(std::string(item), std::string());
            else
                p.items.emplace_back(std::string(trim(item.substr(0, eq))),
                                     std::string(trim(item.substr(eq + 1))));
        }
        if (end == std::string_view::npos)
            break;
        pos = end + 1;
    }
    return p;
}

std::vector<std::string> splitList(std::string_view value)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t end = findOutside(value, ',', pos, true);
        std::string_view item = trim(value.substr(pos, end == std::string_view::npos ? value.size() - pos : end - pos));
        if (!item.empty())
            out.emplace_back(item);
        if (end == std::string_view::npos)
            break;
        pos = end + 1;
    }
    return out;
}

std::string unquote(std::string_view s)
{
    s = trim(s);
    if (s.size() < 2 || s.front() != '"' || s.back() != '"')
        return std::string(s);
    std::string out;
    for (size_t i = 1; i + 1 < s.size(); i++) {
        if (s[i] == '\\' && i + 2 < s.size())
            i++;
        out += s[i];
    }
    return out;
}

std::string quote(std::string_view s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + '"';
}

std::string Uri::str() const
{
    std::string out = scheme + ":";
    if (scheme == "tel")
        return out + user + params.str();
    if (!user.empty())
        out += user + "@";
    out += hostString(host);
    if (port)
        out += ":" + std::to_string(port);
    return out + params.str();
}

std::optional<Uri> Uri::parse(std::string_view s)
{
    s = trim(s);
    size_t colon = s.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return std::nullopt;
    Uri u;
    for (char c : s.substr(0, colon))
        u.scheme += lower(c);
    std::string_view rest = s.substr(colon + 1);
    rest = rest.substr(0, rest.find('?'));  // headers are not used
    if (u.scheme == "tel") {
        size_t semi = rest.find(';');
        u.user = std::string(rest.substr(0, semi));
        if (u.user.empty())
            return std::nullopt;
        if (semi != std::string_view::npos)
            u.params = Params::parse(rest.substr(semi + 1));
        return u;
    }
    if (u.scheme != "sip" && u.scheme != "sips")
        return std::nullopt;
    size_t at = rest.find('@');
    if (at != std::string_view::npos) {
        u.user = std::string(rest.substr(0, at));
        rest = rest.substr(at + 1);
    }
    size_t semi = rest.find(';');
    if (!parseHostPort(rest.substr(0, semi), u.host, u.port))
        return std::nullopt;
    if (semi != std::string_view::npos)
        u.params = Params::parse(rest.substr(semi + 1));
    return u;
}

std::string NameAddr::str() const
{
    std::string out;
    if (!display.empty())
        out = quote(display) + " ";
    return out + "<" + uri + ">" + params.str();
}

std::optional<NameAddr> NameAddr::parse(std::string_view s)
{
    s = trim(s);
    NameAddr n;
    size_t open = findOutside(s, '<');
    if (open == std::string_view::npos) {
        // addr-spec: parameters after the URI belong to the header
        size_t semi = s.find(';');
        n.uri = std::string(trim(s.substr(0, semi)));
        if (semi != std::string_view::npos)
            n.params = Params::parse(s.substr(semi + 1));
    } else {
        size_t close = s.find('>', open);
        if (close == std::string_view::npos)
            return std::nullopt;
        n.display = unquote(s.substr(0, open));
        n.uri = std::string(trim(s.substr(open + 1, close - open - 1)));
        std::string_view rest = trim(s.substr(close + 1));
        if (!rest.empty()) {
            if (rest[0] != ';')
                return std::nullopt;
            n.params = Params::parse(rest.substr(1));
        }
    }
    if (n.uri.empty())
        return std::nullopt;
    return n;
}

std::string Via::str() const
{
    std::string out = "SIP/2.0/" + transport + " " + hostString(host);
    if (port)
        out += ":" + std::to_string(port);
    return out + params.str();
}

std::optional<Via> Via::parse(std::string_view s)
{
    s = trim(s);
    size_t space = s.find_first_of(" \t");
    if (space == std::string_view::npos)
        return std::nullopt;
    std::string_view proto = s.substr(0, space);
    size_t slash = proto.rfind('/');
    if (slash == std::string_view::npos || !iequals(proto.substr(0, slash), "SIP/2.0"))
        return std::nullopt;
    Via v;
    v.transport = std::string(proto.substr(slash + 1));
    for (char &c : v.transport)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::string_view rest = trim(s.substr(space));
    size_t semi = rest.find(';');
    if (!parseHostPort(trim(rest.substr(0, semi)), v.host, v.port))
        return std::nullopt;
    if (semi != std::string_view::npos)
        v.params = Params::parse(rest.substr(semi + 1));
    return v;
}

SipMessage SipMessage::request(std::string method, std::string uri)
{
    SipMessage m;
    m.method_ = std::move(method);
    m.uri_ = std::move(uri);
    return m;
}

SipMessage SipMessage::response(int status, std::string reason)
{
    SipMessage m;
    m.status_ = status;
    m.reason_ = std::move(reason);
    return m;
}

std::optional<std::string> SipMessage::header(std::string_view name) const
{
    for (const auto &[k, v] : headers_)
        if (iequals(k, name)) {
            if (isListHeader(name)) {
                auto items = splitList(v);
                if (!items.empty())
                    return items[0];
                continue;
            }
            return v;
        }
    return std::nullopt;
}

std::vector<std::string> SipMessage::headers(std::string_view name) const
{
    std::vector<std::string> out;
    const bool list = isListHeader(name);
    for (const auto &[k, v] : headers_) {
        if (!iequals(k, name))
            continue;
        if (list) {
            for (auto &item : splitList(v))
                out.push_back(std::move(item));
        } else {
            out.push_back(v);
        }
    }
    return out;
}

void SipMessage::add(std::string name, std::string value)
{
    headers_.emplace_back(std::move(name), std::move(value));
}

void SipMessage::set(std::string name, std::string value)
{
    for (auto it = headers_.begin(); it != headers_.end(); ++it)
        if (iequals(it->first, name)) {
            it->second = std::move(value);
            ++it;
            while (it != headers_.end()) {
                if (iequals(it->first, name))
                    it = headers_.erase(it);
                else
                    ++it;
            }
            return;
        }
    add(std::move(name), std::move(value));
}

void SipMessage::remove(std::string_view name)
{
    std::erase_if(headers_, [&](const auto &h) { return iequals(h.first, name); });
}

std::optional<std::pair<uint32_t, std::string>> SipMessage::cseq() const
{
    auto v = header("CSeq");
    if (!v)
        return std::nullopt;
    std::string_view s = trim(*v);
    size_t space = s.find_first_of(" \t");
    if (space == std::string_view::npos)
        return std::nullopt;
    uint32_t n = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + space, n);
    if (ec != std::errc() || p != s.data() + space)
        return std::nullopt;
    std::string_view method = trim(s.substr(space));
    if (method.empty())
        return std::nullopt;
    return std::make_pair(n, std::string(method));
}

std::optional<std::string> SipMessage::branch() const
{
    auto top = header("Via");
    if (!top)
        return std::nullopt;
    auto via = Via::parse(*top);
    if (!via)
        return std::nullopt;
    return via->params.get("branch");
}

bool SipMessage::hasOption(std::string_view name, std::string_view tag) const
{
    for (const auto &v : headers(name))
        if (iequals(trim(v), tag))
            return true;
    return false;
}

std::string SipMessage::serialize() const
{
    std::string out;
    if (isRequest())
        out = method_ + " " + uri_ + " SIP/2.0\r\n";
    else
        out = "SIP/2.0 " + std::to_string(status_) + " " + reason_ + "\r\n";
    for (const auto &[k, v] : headers_)
        if (!iequals(k, "Content-Length"))
            out += k + ": " + v + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    out += body;
    return out;
}

std::optional<SipMessage> SipMessage::parse(std::string_view text)
{
    if (text.size() > kMaxMessage)
        return std::nullopt;
    size_t end = text.find("\r\n\r\n");
    if (end == std::string_view::npos)
        return std::nullopt;
    std::string_view head = text.substr(0, end);
    std::string_view rest = text.substr(end + 4);

    size_t lineEnd = head.find("\r\n");
    std::string_view first = head.substr(0, lineEnd);
    SipMessage m;
    if (first.starts_with("SIP/2.0 ")) {
        std::string_view s = first.substr(8);
        if (s.size() < 3)
            return std::nullopt;
        int status = 0;
        auto [p, ec] = std::from_chars(s.data(), s.data() + 3, status);
        if (ec != std::errc() || p != s.data() + 3 || status < 100 || status > 699)
            return std::nullopt;
        m.status_ = status;
        m.reason_ = std::string(trim(s.substr(3)));
    } else {
        size_t a = first.find(' ');
        size_t b = first.rfind(' ');
        if (a == std::string_view::npos || a == b || first.substr(b + 1) != "SIP/2.0")
            return std::nullopt;
        m.method_ = std::string(first.substr(0, a));
        m.uri_ = std::string(trim(first.substr(a + 1, b - a - 1)));
        if (m.method_.empty() || m.uri_.empty())
            return std::nullopt;
        for (char c : m.method_)
            if (!std::isupper(static_cast<unsigned char>(c)) && c != '-')
                return std::nullopt;
    }

    size_t pos = lineEnd == std::string_view::npos ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        size_t next = head.find("\r\n", pos);
        std::string_view line = head.substr(pos, next == std::string_view::npos ? head.size() - pos : next - pos);
        pos = next == std::string_view::npos ? head.size() : next + 2;
        if (line.empty())
            continue;
        if ((line[0] == ' ' || line[0] == '\t')) {  // folded onto the previous line
            if (m.headers_.empty())
                return std::nullopt;
            m.headers_.back().second += " " + std::string(trim(line));
            continue;
        }
        size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            return std::nullopt;
        std::string_view name = trim(line.substr(0, colon));
        if (name.empty())
            return std::nullopt;
        if (m.headers_.size() >= kMaxHeaders)
            return std::nullopt;
        m.headers_.emplace_back(expandName(name), std::string(trim(line.substr(colon + 1))));
    }

    if (auto length = m.header("Content-Length")) {
        size_t n = 0;
        std::string_view l = trim(*length);
        auto [p, ec] = std::from_chars(l.data(), l.data() + l.size(), n);
        if (ec != std::errc() || p != l.data() + l.size() || n > rest.size())
            return std::nullopt;
        rest = rest.substr(0, n);
    }
    m.body = std::string(rest);
    return m;
}

SipMessage SipMessage::makeResponse(int status, std::string reason) const
{
    SipMessage r = response(status, std::move(reason));
    for (const auto &[k, v] : headers_)
        if (iequals(k, "Via") || iequals(k, "From") || iequals(k, "To") || iequals(k, "Call-ID") ||
            iequals(k, "CSeq"))
            r.add(k, v);
    return r;
}

std::optional<size_t> sipFrameLength(std::string_view buffer)
{
    // Keep-alives (RFC 5626): a CRLF pair, or the CRLFCRLF ping
    size_t skip = 0;
    while (skip + 1 < buffer.size() && buffer[skip] == '\r' && buffer[skip + 1] == '\n')
        skip += 2;
    if (skip)
        return skip;
    size_t end = buffer.find("\r\n\r\n");
    if (end == std::string_view::npos)
        return buffer.size() > kMaxMessage ? std::nullopt : std::optional<size_t>(0);
    // Content-Length is mandatory on streams (RFC 3261 18.3)
    std::optional<std::string_view> value;
    std::string_view head = buffer.substr(0, end);
    for (size_t pos = head.find("\r\n"); pos != std::string_view::npos && pos < head.size();) {
        size_t next = head.find("\r\n", pos + 2);
        std::string_view line = head.substr(pos + 2, next == std::string_view::npos ? head.size() - pos - 2
                                                                                  : next - pos - 2);
        pos = next;
        size_t colon = line.find(':');
        if (colon != std::string_view::npos && iequals(expandName(trim(line.substr(0, colon))), "Content-Length"))
            value = line.substr(colon + 1);
    }
    if (!value)
        return std::nullopt;
    size_t length = 0;
    std::string_view l = trim(*value);
    auto [p, ec] = std::from_chars(l.data(), l.data() + l.size(), length);
    if (ec != std::errc() || p != l.data() + l.size() || length > kMaxMessage)
        return std::nullopt;
    if (buffer.size() < end + 4 + length)
        return 0;
    return end + 4 + length;
}

std::string randomHex(size_t n)
{
    static std::random_device device;
    static std::mt19937_64 engine(((uint64_t)device() << 32) ^ device());
    static const char digits[] = "0123456789abcdef";
    std::string out;
    while (out.size() < n) {
        uint64_t v = engine();
        for (int i = 0; i < 16 && out.size() < n; i++, v >>= 4)
            out += digits[v & 15];
    }
    return out;
}

std::string newBranch()
{
    return "z9hG4bK" + randomHex(16);
}

}  // namespace shannond::ims
