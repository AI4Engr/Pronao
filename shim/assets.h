// shim-internal: the application's own web files, served from memory to the window. This is only a table lookup: which files exist,
// what their types are and which short addresses stand for which file are all decided in CeeLing (App.serveSite) and handed over
// with app_asset_add / app_asset_alias. Header-only so the real window (shim/win/assets.cpp) and the test double use the same code.
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>

class AssetStore {
    struct Asset { std::string mime; std::shared_ptr<const std::string> bytes; };
    std::mutex m_;
    std::map<std::string, Asset> items_;
    std::string origin_, csp_;

    // "%41" -> "A"; a bad escape is kept as it is
    static std::string decode(const std::string& s) {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            const auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
            if (s[i] == '%' && i + 2 < s.size() + 0 && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) { out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])); i += 2; }
            else out += s[i];
        }
        return out;
    }
public:
    // origin without a trailing slash ("http://app.local"); csp "" for none
    void configure(const std::string& origin, const std::string& csp) { std::lock_guard<std::mutex> l(m_); origin_ = origin; csp_ = csp; }
    bool add(const std::string& path, const std::string& mime, std::string bytes) {
        std::lock_guard<std::mutex> l(m_);
        items_[path] = Asset{mime, std::make_shared<const std::string>(std::move(bytes))};
        return true;
    }
    std::string origin() { std::lock_guard<std::mutex> l(m_); return origin_; }
    bool alias(const std::string& from, const std::string& to) {
        std::lock_guard<std::mutex> l(m_);
        auto it = items_.find(to);
        if (it == items_.end()) return false;
        items_[from] = it->second;
        return true;
    }
    // false: `url` is not under the origin, leave it to the network. Otherwise the answer: 200 with the file, 404 for anything that was
    // not registered (never a fall-through to the network), 405 for a method other than GET and HEAD.
    bool respond(const std::string& method, const std::string& url, int& status, std::string& headers, std::shared_ptr<const std::string>& body) {
        std::lock_guard<std::mutex> l(m_);
        if (origin_.empty() || url.compare(0, origin_.size(), origin_) != 0 || (url.size() > origin_.size() && url[origin_.size()] != '/' && url[origin_.size()] != '?' && url[origin_.size()] != '#')) return false;
        std::string path = url.substr(origin_.size());
        path = path.substr(0, path.find_first_of("?#"));
        if (path.empty()) path = "/";
        path = decode(path);
        static const auto none = std::make_shared<const std::string>();
        const std::string common = "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
        if (method != "GET" && method != "HEAD") { status = 405; body = none; headers = "Content-Type: text/plain\r\nContent-Length: 0\r\nAllow: GET, HEAD\r\n" + common; return true; }
        auto it = items_.find(path);
        if (it == items_.end()) { status = 404; body = std::make_shared<const std::string>("not found"); headers = "Content-Type: text/plain\r\nContent-Length: 9\r\n" + common; return true; }
        status = 200;
        body = method == "HEAD" ? none : it->second.bytes;
        headers = "Content-Type: " + it->second.mime + "\r\nContent-Length: " + std::to_string(it->second.bytes->size()) + "\r\n" + common;
        if (!csp_.empty() && it->second.mime.compare(0, 9, "text/html") == 0) headers += "Content-Security-Policy: " + csp_ + "\r\n";
        return true;
    }
};
