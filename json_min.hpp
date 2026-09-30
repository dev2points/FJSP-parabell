// json_min.hpp
// Bộ phân tích JSON tối giản, tự viết, không phụ thuộc thư viện ngoài.
// Đủ dùng để đọc các file instance FJSP+ (object / array / string / number / bool / null).
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <fstream>
#include <sstream>

namespace json_min {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::unordered_map<std::string, Json> obj;

    bool is_null() const { return type == Type::Null; }
    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }

    bool has(const std::string& key) const {
        return type == Type::Object && obj.find(key) != obj.end();
    }
    const Json& at(const std::string& key) const {
        auto it = obj.find(key);
        if (it == obj.end())
            throw std::runtime_error("JSON: khong tim thay khoa '" + key + "'");
        return it->second;
    }
    const Json* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    long long as_ll() const { return (long long) llround(num); }
    int as_int() const { return (int) llround(num); }
    double as_double() const { return num; }
    const std::string& as_string() const { return str; }
    bool as_bool() const {
        if (type == Type::Bool) return b;
        if (type == Type::Number) return num != 0.0;
        return false;
    }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s), i_(0), n_(s.size()) {}

    Json parse() {
        skip_ws();
        Json v = parse_value();
        return v;
    }

private:
    const std::string& s_;
    size_t i_, n_;

    void skip_ws() {
        while (i_ < n_ && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++;
    }
    char peek() const { return i_ < n_ ? s_[i_] : '\0'; }
    char get() { return s_[i_++]; }
    void expect(char c) {
        if (i_ >= n_ || s_[i_] != c)
            throw std::runtime_error(std::string("JSON: mong doi ky tu '") + c + "' tai vi tri " + std::to_string(i_));
        i_++;
    }

    Json parse_value() {
        skip_ws();
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') { Json v; v.type = Json::Type::String; v.str = parse_raw_string(); return v; }
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        return parse_number();
    }

    Json parse_object() {
        Json v; v.type = Json::Type::Object;
        expect('{');
        skip_ws();
        if (peek() == '}') { i_++; return v; }
        while (true) {
            skip_ws();
            std::string key = parse_raw_string();
            skip_ws();
            expect(':');
            Json val = parse_value();
            v.obj.emplace(std::move(key), std::move(val));
            skip_ws();
            char c = get();
            if (c == ',') continue;
            if (c == '}') break;
            throw std::runtime_error("JSON: loi cu phap trong object");
        }
        return v;
    }

    Json parse_array() {
        Json v; v.type = Json::Type::Array;
        expect('[');
        skip_ws();
        if (peek() == ']') { i_++; return v; }
        while (true) {
            Json val = parse_value();
            v.arr.push_back(std::move(val));
            skip_ws();
            char c = get();
            if (c == ',') continue;
            if (c == ']') break;
            throw std::runtime_error("JSON: loi cu phap trong array");
        }
        return v;
    }

    std::string parse_raw_string() {
        expect('"');
        std::string out;
        while (true) {
            if (i_ >= n_) throw std::runtime_error("JSON: chuoi khong dong");
            char c = get();
            if (c == '"') break;
            if (c == '\\') {
                if (i_ >= n_) throw std::runtime_error("JSON: escape loi");
                char e = get();
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (i_ + 4 > n_) throw std::runtime_error("JSON: \\u loi");
                        std::string hex = s_.substr(i_, 4);
                        i_ += 4;
                        unsigned int cp = (unsigned int) strtoul(hex.c_str(), nullptr, 16);
                        if (cp < 0x80) {
                            out += (char) cp;
                        } else if (cp < 0x800) {
                            out += (char) (0xC0 | (cp >> 6));
                            out += (char) (0x80 | (cp & 0x3F));
                        } else {
                            out += (char) (0xE0 | (cp >> 12));
                            out += (char) (0x80 | ((cp >> 6) & 0x3F));
                            out += (char) (0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    Json parse_bool() {
        Json v; v.type = Json::Type::Bool;
        if (s_.compare(i_, 4, "true") == 0) { v.b = true; i_ += 4; }
        else if (s_.compare(i_, 5, "false") == 0) { v.b = false; i_ += 5; }
        else throw std::runtime_error("JSON: literal khong hop le");
        return v;
    }

    Json parse_null() {
        if (s_.compare(i_, 4, "null") == 0) i_ += 4;
        else throw std::runtime_error("JSON: literal khong hop le");
        return Json();
    }

    Json parse_number() {
        size_t start = i_;
        if (peek() == '-' || peek() == '+') i_++;
        while (i_ < n_ && isdigit((unsigned char) s_[i_])) i_++;
        if (i_ < n_ && s_[i_] == '.') {
            i_++;
            while (i_ < n_ && isdigit((unsigned char) s_[i_])) i_++;
        }
        if (i_ < n_ && (s_[i_] == 'e' || s_[i_] == 'E')) {
            i_++;
            if (i_ < n_ && (s_[i_] == '+' || s_[i_] == '-')) i_++;
            while (i_ < n_ && isdigit((unsigned char) s_[i_])) i_++;
        }
        if (i_ == start) throw std::runtime_error("JSON: gia tri so khong hop le");
        std::string tok = s_.substr(start, i_ - start);
        Json v; v.type = Json::Type::Number;
        v.num = strtod(tok.c_str(), nullptr);
        return v;
    }
};

inline Json parse_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Khong mo duoc file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    Parser p(content);
    return p.parse();
}

} // namespace json_min
