// shim-internal: the application's own web files, served from memory to the window. Only a table lookup: which files exist, their types and the aliases are decided in
// src/assets.clg. Header-only, so the real window (shim/win/assets.cpp) and the test double share the code.
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <zlib.h>

class AssetStore {
    // gz: `bytes` is a gzip stream (scripts/pack_assets.sh stores a file that way when that saves space); it is inflated for each request
    struct Asset { std::string mime; std::shared_ptr<const std::string> bytes; bool gz = false; };
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
    // origin without a trailing slash ("http://app.localhost"); csp "" for none
    void configure(const std::string& origin, const std::string& csp) { std::lock_guard<std::mutex> l(m_); origin_ = origin; csp_ = csp; }
    bool add(const std::string& path, const std::string& mime, std::string bytes) {
        std::lock_guard<std::mutex> l(m_);
        items_[path] = Asset{mime, std::make_shared<const std::string>(std::move(bytes)), false};
        return true;
    }
    // like add, but `gzip` is a gzip stream that is kept as it is and inflated when the file is asked for
    bool addGzip(const std::string& path, const std::string& mime, std::string gzip) {
        std::lock_guard<std::mutex> l(m_);
        items_[path] = Asset{mime, std::make_shared<const std::string>(std::move(gzip)), true};
        return true;
    }
    // false for anything that is not one whole, valid gzip stream (a truncated one, trailing bytes, a wrong checksum)
    static bool inflateGzip(const std::string& in, std::string& out) {
        z_stream z{};
        if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return false;
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
        z.avail_in = static_cast<uInt>(in.size());
        char buf[65536];
        int rc;
        do {
            z.next_out = reinterpret_cast<Bytef*>(buf);
            z.avail_out = sizeof buf;
            rc = inflate(&z, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&z); return false; }
            out.append(buf, sizeof buf - z.avail_out);
        } while (rc != Z_STREAM_END);
        const bool whole = z.avail_in == 0;
        inflateEnd(&z);
        return whole;
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
    // not registered (never a fall-through to the network), 405 for a method other than GET and HEAD, 500 for a stored file that does not inflate.
    bool respond(const std::string& method, const std::string& url, int& status, std::string& headers, std::shared_ptr<const std::string>& body) {
        std::unique_lock<std::mutex> l(m_);
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
        const Asset asset = it->second;                    // copies of the handle and the type: the inflating below runs without the lock
        const std::string policy = csp_;
        l.unlock();
        std::shared_ptr<const std::string> bytes = asset.bytes;
        if (asset.gz) {
            auto plain = std::make_shared<std::string>();
            if (!inflateGzip(*asset.bytes, *plain)) { status = 500; body = std::make_shared<const std::string>("corrupt asset"); headers = "Content-Type: text/plain\r\nContent-Length: 13\r\n" + common; return true; }
            bytes = plain;
        }
        status = 200;
        body = method == "HEAD" ? none : bytes;
        headers = "Content-Type: " + asset.mime + "\r\nContent-Length: " + std::to_string(bytes->size()) + "\r\n" + common;
        if (!policy.empty() && asset.mime.compare(0, 9, "text/html") == 0) headers += "Content-Security-Policy: " + policy + "\r\n";
        return true;
    }
};
