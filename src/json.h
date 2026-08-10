// Minimal JSON parser/writer - just enough for the config and layout files.
// Objects keep insertion order so saved files stay readable and diff cleanly.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace js {

enum class Type { Null, Bool, Num, Str, Arr, Obj };

struct Value {
    Type type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    Value() = default;
    Value(bool v) : type(Type::Bool), b(v) {}
    Value(double v) : type(Type::Num), n(v) {}
    Value(int v) : type(Type::Num), n(v) {}
    Value(const char* v) : type(Type::Str), s(v ? v : "") {}
    Value(std::string v) : type(Type::Str), s(std::move(v)) {}

    static Value object() { Value v; v.type = Type::Obj; return v; }
    static Value array() { Value v; v.type = Type::Arr; return v; }

    bool isNull() const { return type == Type::Null; }
    bool isObj() const { return type == Type::Obj; }
    bool isArr() const { return type == Type::Arr; }
    bool isStr() const { return type == Type::Str; }
    bool isNum() const { return type == Type::Num; }
    bool isBool() const { return type == Type::Bool; }

    // Object access. operator[] inserts if missing (and turns this into an
    // object), find() never mutates.
    Value& operator[](const std::string& key);
    const Value* find(const std::string& key) const;
    bool has(const std::string& key) const { return find(key) != nullptr; }
    void erase(const std::string& key);

    // Typed getters with defaults - the whole config layer reads through
    // these so a malformed or partial file degrades instead of throwing.
    std::string str(const std::string& key, const std::string& def = "") const;
    double num(const std::string& key, double def = 0) const;
    int integer(const std::string& key, int def = 0) const;
    bool boolean(const std::string& key, bool def = false) const;

    std::string asStr(const std::string& def = "") const;
    double asNum(double def = 0) const;
    bool asBool(bool def = false) const;

    void push(Value v) { type = Type::Arr; arr.push_back(std::move(v)); }
    size_t size() const { return type == Type::Arr ? arr.size()
                                                   : (type == Type::Obj ? obj.size() : 0); }

    std::string dump(int indent = 2) const;
    static Value parse(const std::string& text, std::string* error = nullptr);
};

}  // namespace js
