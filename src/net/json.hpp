// A little JSON, for the lobby's messages: values, a parser, a writer.
// Numbers as doubles; objects keep their keys in order.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace net {

class Json {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(bool b) : kind_(Kind::Bool), number_(b ? 1 : 0) {}
    Json(double n) : kind_(Kind::Number), number_(n) {}
    Json(int n) : kind_(Kind::Number), number_(n) {}
    Json(const char* s) : kind_(Kind::String), string_(s) {}
    Json(std::string s) : kind_(Kind::String), string_(std::move(s)) {}
    static Json array() { Json j; j.kind_ = Kind::Array; return j; }
    static Json object() { Json j; j.kind_ = Kind::Object; return j; }

    Kind kind() const { return kind_; }
    bool is_object() const { return kind_ == Kind::Object; }
    bool is_array() const { return kind_ == Kind::Array; }
    bool as_bool() const { return kind_ == Kind::Bool && number_ != 0; }
    double as_number(double otherwise = 0) const { return kind_ == Kind::Number ? number_ : otherwise; }
    const std::string& as_string() const {
        static const std::string none;
        return kind_ == Kind::String ? string_ : none;
    }
    const std::vector<Json>& items() const { return items_; }

    // Objects: a member (null if missing), set one; arrays: append.
    const Json& operator[](std::string_view key) const {
        static const Json none;
        for (const auto& [k, v] : members_)
            if (k == key) return v;
        return none;
    }
    Json& set(std::string key, Json value) {
        for (auto& [k, v] : members_)
            if (k == key) return v = std::move(value);
        members_.push_back({std::move(key), std::move(value)});
        return members_.back().second;
    }
    Json& push(Json value) {
        items_.push_back(std::move(value));
        return items_.back();
    }

    std::string dump() const {
        std::string out;
        write(out);
        return out;
    }

    static std::optional<Json> parse(std::string_view text) {
        std::size_t at = 0;
        Json j;
        if (!read(text, at, j, 0)) return std::nullopt;
        skip(text, at);
        if (at != text.size()) return std::nullopt;
        return j;
    }

private:
    void write(std::string& out) const {
        switch (kind_) {
        case Kind::Null: out += "null"; break;
        case Kind::Bool: out += number_ != 0 ? "true" : "false"; break;
        case Kind::Number: {
            char buf[32];
            if (number_ == static_cast<double>(static_cast<long long>(number_)))
                std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(number_));
            else
                std::snprintf(buf, sizeof buf, "%.17g", number_);
            out += buf;
            break;
        }
        case Kind::String: quote(out, string_); break;
        case Kind::Array:
            out += '[';
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i) out += ',';
                items_[i].write(out);
            }
            out += ']';
            break;
        case Kind::Object:
            out += '{';
            for (std::size_t i = 0; i < members_.size(); ++i) {
                if (i) out += ',';
                quote(out, members_[i].first);
                out += ':';
                members_[i].second.write(out);
            }
            out += '}';
            break;
        }
    }
    static void quote(std::string& out, const std::string& s) {
        out += '"';
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') out += '\\', out += static_cast<char>(c);
            else if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else out += static_cast<char>(c);
        }
        out += '"';
    }
    static void skip(std::string_view t, std::size_t& at) {
        while (at < t.size() && (t[at] == ' ' || t[at] == '\t' || t[at] == '\n' || t[at] == '\r')) ++at;
    }
    static bool literal(std::string_view t, std::size_t& at, std::string_view word) {
        if (t.substr(at, word.size()) != word) return false;
        at += word.size();
        return true;
    }
    static bool read_string(std::string_view t, std::size_t& at, std::string& out) {
        if (at >= t.size() || t[at] != '"') return false;
        ++at;
        while (at < t.size() && t[at] != '"') {
            char c = t[at++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= t.size()) return false;
            c = t[at++];
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (at + 4 > t.size()) return false;
                const unsigned code = static_cast<unsigned>(std::strtoul(std::string(t.substr(at, 4)).c_str(), nullptr, 16));
                at += 4;
                if (code < 0x80) out += static_cast<char>(code); // (names are ASCII: anything else as UTF-8)
                else if (code < 0x800) out += static_cast<char>(0xC0 | code >> 6), out += static_cast<char>(0x80 | (code & 0x3F));
                else out += static_cast<char>(0xE0 | code >> 12), out += static_cast<char>(0x80 | ((code >> 6) & 0x3F)),
                     out += static_cast<char>(0x80 | (code & 0x3F));
                break;
            }
            default: out += c; break;
            }
        }
        if (at >= t.size()) return false;
        ++at;
        return true;
    }
    static bool read(std::string_view t, std::size_t& at, Json& out, int depth) {
        if (depth > 16) return false;
        skip(t, at);
        if (at >= t.size()) return false;
        const char c = t[at];
        if (c == 'n') return literal(t, at, "null");
        if (c == 't') return literal(t, at, "true") && (out = Json(true), true);
        if (c == 'f') return literal(t, at, "false") && (out = Json(false), true);
        if (c == '"') {
            std::string s;
            if (!read_string(t, at, s)) return false;
            out = Json(std::move(s));
            return true;
        }
        if (c == '[') {
            out = array();
            ++at;
            skip(t, at);
            if (at < t.size() && t[at] == ']') return ++at, true;
            for (;;) {
                Json v;
                if (!read(t, at, v, depth + 1)) return false;
                out.items_.push_back(std::move(v));
                skip(t, at);
                if (at < t.size() && t[at] == ',') { ++at; continue; }
                if (at < t.size() && t[at] == ']') return ++at, true;
                return false;
            }
        }
        if (c == '{') {
            out = object();
            ++at;
            skip(t, at);
            if (at < t.size() && t[at] == '}') return ++at, true;
            for (;;) {
                skip(t, at);
                std::string key;
                if (!read_string(t, at, key)) return false;
                skip(t, at);
                if (at >= t.size() || t[at] != ':') return false;
                ++at;
                Json v;
                if (!read(t, at, v, depth + 1)) return false;
                out.members_.push_back({std::move(key), std::move(v)});
                skip(t, at);
                if (at < t.size() && t[at] == ',') { ++at; continue; }
                if (at < t.size() && t[at] == '}') return ++at, true;
                return false;
            }
        }
        const std::string rest(t.substr(at, 40));
        char* end = nullptr;
        const double n = std::strtod(rest.c_str(), &end);
        if (end == rest.c_str()) return false;
        at += static_cast<std::size_t>(end - rest.c_str());
        out = Json(n);
        return true;
    }

    Kind kind_ = Kind::Null;
    double number_ = 0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

} // namespace net
