#include "json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace js {

Value& Value::operator[](const std::string& key) {
    if (type != Type::Obj) { type = Type::Obj; obj.clear(); }
    for (auto& kv : obj)
        if (kv.first == key) return kv.second;
    obj.emplace_back(key, Value());
    return obj.back().second;
}

const Value* Value::find(const std::string& key) const {
    if (type != Type::Obj) return nullptr;
    for (auto& kv : obj)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

void Value::erase(const std::string& key) {
    for (size_t i = 0; i < obj.size(); ++i)
        if (obj[i].first == key) { obj.erase(obj.begin() + (long)i); return; }
}

std::string Value::asStr(const std::string& def) const {
    return type == Type::Str ? s : def;
}
double Value::asNum(double def) const { return type == Type::Num ? n : def; }
bool Value::asBool(bool def) const { return type == Type::Bool ? b : def; }

std::string Value::str(const std::string& key, const std::string& def) const {
    const Value* v = find(key);
    return v ? v->asStr(def) : def;
}
double Value::num(const std::string& key, double def) const {
    const Value* v = find(key);
    return v ? v->asNum(def) : def;
}
int Value::integer(const std::string& key, int def) const {
    const Value* v = find(key);
    return v ? (int)llround(v->asNum(def)) : def;
}
bool Value::boolean(const std::string& key, bool def) const {
    const Value* v = find(key);
    return v ? v->asBool(def) : def;
}

// ------------------------------------------------------------------ writer
static void escape(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;  // UTF-8 passes through verbatim
                }
        }
    }
    out += '"';
}

static void numToString(double n, std::string& out) {
    if (std::isnan(n) || std::isinf(n)) { out += "0"; return; }
    if (n == (double)(long long)n && std::fabs(n) < 1e15) {
        out += std::to_string((long long)n);
        return;
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.10g", n);
    out += buf;
}

static void dumpTo(const Value& v, std::string& out, int indent, int depth) {
    const bool pretty = indent > 0;
    std::string pad, padIn;
    if (pretty) {
        pad.assign((size_t)(indent * depth), ' ');
        padIn.assign((size_t)(indent * (depth + 1)), ' ');
    }
    switch (v.type) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.b ? "true" : "false"; break;
        case Type::Num:  numToString(v.n, out); break;
        case Type::Str:  escape(v.s, out); break;
        case Type::Arr: {
            if (v.arr.empty()) { out += "[]"; break; }
            out += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out += ',';
                if (pretty) { out += '\n'; out += padIn; }
                dumpTo(v.arr[i], out, indent, depth + 1);
            }
            if (pretty) { out += '\n'; out += pad; }
            out += ']';
            break;
        }
        case Type::Obj: {
            if (v.obj.empty()) { out += "{}"; break; }
            out += '{';
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out += ',';
                if (pretty) { out += '\n'; out += padIn; }
                escape(v.obj[i].first, out);
                out += pretty ? ": " : ":";
                dumpTo(v.obj[i].second, out, indent, depth + 1);
            }
            if (pretty) { out += '\n'; out += pad; }
            out += '}';
            break;
        }
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dumpTo(*this, out, indent, 0);
    if (indent > 0) out += '\n';
    return out;
}

// ------------------------------------------------------------------ parser
namespace {

struct Parser {
    const std::string& t;
    size_t i = 0;
    std::string err;

    explicit Parser(const std::string& text) : t(text) {}

    void ws() {
        while (i < t.size()) {
            char c = t[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i; continue; }
            // tolerate // and /* */ comments so hand-edited files still load
            if (c == '/' && i + 1 < t.size()) {
                if (t[i + 1] == '/') {
                    while (i < t.size() && t[i] != '\n') ++i;
                    continue;
                }
                if (t[i + 1] == '*') {
                    i += 2;
                    while (i + 1 < t.size() && !(t[i] == '*' && t[i + 1] == '/')) ++i;
                    i = (i + 1 < t.size()) ? i + 2 : t.size();
                    continue;
                }
            }
            break;
        }
    }

    bool fail(const std::string& m) {
        if (err.empty()) {
            size_t line = 1;
            for (size_t k = 0; k < i && k < t.size(); ++k)
                if (t[k] == '\n') ++line;
            err = m + " (line " + std::to_string(line) + ")";
        }
        return false;
    }

    bool lit(const char* word) {
        size_t n = std::char_traits<char>::length(word);
        if (t.compare(i, n, word) != 0) return false;
        i += n;
        return true;
    }

    void appendUtf8(unsigned cp, std::string& out) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& out) {
        if (i + 4 > t.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t[i + (size_t)k];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        i += 4;
        return true;
    }

    bool parseString(std::string& out) {
        if (i >= t.size() || t[i] != '"') return fail("expected string");
        ++i;
        out.clear();
        while (i < t.size()) {
            char c = t[i];
            if (c == '"') { ++i; return true; }
            if (c == '\\') {
                ++i;
                if (i >= t.size()) break;
                char e = t[i++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!hex4(cp)) return fail("bad \\u escape");
                        // surrogate pair
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < t.size() &&
                            t[i] == '\\' && t[i + 1] == 'u') {
                            size_t save = i;
                            i += 2;
                            unsigned lo = 0;
                            if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else
                                i = save;
                        }
                        appendUtf8(cp, out);
                        break;
                    }
                    default: return fail("bad escape");
                }
                continue;
            }
            out += c;
            ++i;
        }
        return fail("unterminated string");
    }

    bool parseValue(Value& v, int depth) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (i >= t.size()) return fail("unexpected end of input");
        char c = t[i];
        if (c == '{') {
            ++i;
            v = Value::object();
            ws();
            if (i < t.size() && t[i] == '}') { ++i; return true; }
            while (true) {
                ws();
                std::string key;
                if (!parseString(key)) return false;
                ws();
                if (i >= t.size() || t[i] != ':') return fail("expected ':'");
                ++i;
                Value child;
                if (!parseValue(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                ws();
                if (i < t.size() && t[i] == ',') { ++i; ws();
                    if (i < t.size() && t[i] == '}') { ++i; return true; }  // trailing comma
                    continue; }
                if (i < t.size() && t[i] == '}') { ++i; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i;
            v = Value::array();
            ws();
            if (i < t.size() && t[i] == ']') { ++i; return true; }
            while (true) {
                Value child;
                if (!parseValue(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (i < t.size() && t[i] == ',') { ++i; ws();
                    if (i < t.size() && t[i] == ']') { ++i; return true; }
                    continue; }
                if (i < t.size() && t[i] == ']') { ++i; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v = Value();
            v.type = Type::Str;
            return parseString(v.s);
        }
        if (lit("true")) { v = Value(true); return true; }
        if (lit("false")) { v = Value(false); return true; }
        if (lit("null")) { v = Value(); return true; }
        // number
        {
            const char* start = t.c_str() + i;
            char* end = nullptr;
            double d = std::strtod(start, &end);
            if (end == start) return fail("unexpected character");
            i += (size_t)(end - start);
            v = Value(d);
            return true;
        }
    }
};

}  // namespace

Value Value::parse(const std::string& text, std::string* error) {
    Parser p(text);
    Value v;
    if (!p.parseValue(v, 0)) {
        if (error) *error = p.err;
        return Value();
    }
    p.ws();
    if (p.i != text.size()) {
        // trailing junk is a real error - better to tell the user than to
        // silently load half a file
        if (error) *error = "unexpected trailing data";
        return Value();
    }
    if (error) error->clear();
    return v;
}

}  // namespace js
