// Importer for the Python macrokb macros.yaml.
//
// This is deliberately not a general YAML implementation - it understands the
// subset macros.yaml actually uses (block maps, block sequences, flow maps,
// quoted and plain scalars), which is enough to migrate a config without
// pulling in a YAML library.
#include "config.h"
#include "keys.h"
#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace cfg {
namespace {

struct Line {
    int indent = 0;
    std::string text;
};

// Strip a trailing comment, respecting quotes.
std::string stripComment(const std::string& s) {
    bool inSingle = false, inDouble = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\'' && !inDouble) inSingle = !inSingle;
        else if (c == '"' && !inSingle) inDouble = !inDouble;
        else if (c == '#' && !inSingle && !inDouble) {
            if (i == 0 || isspace((unsigned char)s[i - 1]))
                return s.substr(0, i);
        }
    }
    return s;
}

std::vector<Line> toLines(const std::string& text) {
    std::vector<Line> out;
    for (std::string raw : split(replaceAll(text, "\r\n", "\n"), '\n')) {
        std::string noComment = stripComment(raw);
        std::string trimmed = trim(noComment);
        if (trimmed.empty()) continue;
        if (trimmed == "---" || trimmed == "...") continue;
        Line l;
        l.indent = 0;
        while (l.indent < (int)noComment.size() && noComment[(size_t)l.indent] == ' ')
            ++l.indent;
        l.text = trimmed;
        out.push_back(std::move(l));
    }
    return out;
}

std::string unquote(const std::string& s) {
    std::string t = trim(s);
    if (t.size() >= 2) {
        if (t.front() == '"' && t.back() == '"') {
            std::string body = t.substr(1, t.size() - 2);
            std::string out;
            for (size_t i = 0; i < body.size(); ++i) {
                if (body[i] == '\\' && i + 1 < body.size()) {
                    char n = body[++i];
                    switch (n) {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        case '\\': out += '\\'; break;
                        case '"': out += '"'; break;
                        default: out += n;
                    }
                } else {
                    out += body[i];
                }
            }
            return out;
        }
        if (t.front() == '\'' && t.back() == '\'')
            return replaceAll(t.substr(1, t.size() - 2), "''", "'");
    }
    return t;
}

js::Value scalar(const std::string& raw) {
    std::string t = trim(raw);
    if (t.empty()) return js::Value("");
    if ((t.front() == '"' && t.back() == '"') ||
        (t.front() == '\'' && t.back() == '\''))
        return js::Value(unquote(t));

    std::string l = lower(t);
    if (l == "true" || l == "yes" || l == "on") return js::Value(true);
    if (l == "false" || l == "no" || l == "off") return js::Value(false);
    if (l == "null" || l == "~") return js::Value();

    char* end = nullptr;
    double d = std::strtod(t.c_str(), &end);
    if (end && *end == '\0' && end != t.c_str()) return js::Value(d);
    return js::Value(t);
}

// Split "a: 1, b: {c: 2}" at top-level commas.
std::vector<std::string> splitTopLevel(const std::string& s, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    bool inSingle = false, inDouble = false;
    std::string cur;
    for (char c : s) {
        if (c == '\'' && !inDouble) inSingle = !inSingle;
        else if (c == '"' && !inSingle) inDouble = !inDouble;
        if (!inSingle && !inDouble) {
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') --depth;
            else if (c == sep && depth == 0) {
                out.push_back(cur);
                cur.clear();
                continue;
            }
        }
        cur += c;
    }
    if (!trim(cur).empty()) out.push_back(cur);
    return out;
}

// Position of the ":" that separates a key from its value, ignoring quotes
// and nested flow collections.
size_t keySplit(const std::string& s) {
    int depth = 0;
    bool inSingle = false, inDouble = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\'' && !inDouble) inSingle = !inSingle;
        else if (c == '"' && !inSingle) inDouble = !inDouble;
        else if (!inSingle && !inDouble) {
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') --depth;
            else if (c == ':' && depth == 0 &&
                     (i + 1 == s.size() || isspace((unsigned char)s[i + 1])))
                return i;
        }
    }
    return std::string::npos;
}

js::Value parseFlow(const std::string& s);

js::Value parseFlowMap(const std::string& body) {
    js::Value m = js::Value::object();
    for (const std::string& part : splitTopLevel(body, ',')) {
        std::string p = trim(part);
        if (p.empty()) continue;
        size_t colon = keySplit(p);
        if (colon == std::string::npos) continue;
        std::string key = unquote(trim(p.substr(0, colon)));
        std::string val = trim(p.substr(colon + 1));
        m[key] = parseFlow(val);
    }
    return m;
}

js::Value parseFlow(const std::string& s) {
    std::string t = trim(s);
    if (t.size() >= 2 && t.front() == '{' && t.back() == '}')
        return parseFlowMap(t.substr(1, t.size() - 2));
    if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
        js::Value a = js::Value::array();
        for (const std::string& part : splitTopLevel(t.substr(1, t.size() - 2), ','))
            if (!trim(part).empty()) a.push(parseFlow(part));
        return a;
    }
    return scalar(t);
}

js::Value parseBlock(const std::vector<Line>& lines, size_t& i, int indent);

js::Value parseSeq(const std::vector<Line>& lines, size_t& i, int indent) {
    js::Value arr = js::Value::array();
    while (i < lines.size() && lines[i].indent == indent &&
           lines[i].text.rfind("-", 0) == 0) {
        std::string item = trim(lines[i].text.substr(1));
        ++i;
        if (item.empty()) {
            if (i < lines.size() && lines[i].indent > indent)
                arr.push(parseBlock(lines, i, lines[i].indent));
            else
                arr.push(js::Value());
            continue;
        }
        if (item.front() == '{' || item.front() == '[') {
            arr.push(parseFlow(item));
            continue;
        }
        size_t colon = keySplit(item);
        if (colon != std::string::npos) {
            // "- do: type" starts a map that may continue on deeper lines
            js::Value m = js::Value::object();
            std::string key = unquote(trim(item.substr(0, colon)));
            std::string val = trim(item.substr(colon + 1));
            m[key] = val.empty() ? js::Value() : parseFlow(val);
            while (i < lines.size() && lines[i].indent > indent &&
                   lines[i].text.rfind("-", 0) != 0) {
                size_t c2 = keySplit(lines[i].text);
                if (c2 == std::string::npos) { ++i; continue; }
                std::string k2 = unquote(trim(lines[i].text.substr(0, c2)));
                std::string v2 = trim(lines[i].text.substr(c2 + 1));
                ++i;
                if (v2.empty() && i < lines.size() &&
                    lines[i].indent > indent)
                    m[k2] = parseBlock(lines, i, lines[i].indent);
                else
                    m[k2] = parseFlow(v2);
            }
            arr.push(std::move(m));
            continue;
        }
        arr.push(scalar(item));
    }
    return arr;
}

js::Value parseBlock(const std::vector<Line>& lines, size_t& i, int indent) {
    if (i < lines.size() && lines[i].text.rfind("-", 0) == 0 &&
        (lines[i].text.size() == 1 || lines[i].text[1] == ' '))
        return parseSeq(lines, i, indent);

    js::Value map = js::Value::object();
    while (i < lines.size() && lines[i].indent == indent) {
        const std::string& text = lines[i].text;
        size_t colon = keySplit(text);
        if (colon == std::string::npos) { ++i; continue; }
        std::string key = unquote(trim(text.substr(0, colon)));
        std::string val = trim(text.substr(colon + 1));
        ++i;
        if (!val.empty()) {
            map[key] = parseFlow(val);
        } else if (i < lines.size() && lines[i].indent > indent) {
            map[key] = parseBlock(lines, i, lines[i].indent);
        } else {
            map[key] = js::Value();
        }
    }
    return map;
}

}  // namespace

bool importMacroKbYaml(const std::string& yamlPath, Config& out,
                       std::string* error) {
    std::string text;
    if (!readFile(yamlPath, text)) {
        if (error) *error = "could not read " + yamlPath;
        return false;
    }

    std::vector<Line> lines = toLines(text);
    if (lines.empty()) {
        if (error) *error = "that file is empty";
        return false;
    }
    size_t i = 0;
    js::Value root = parseBlock(lines, i, lines[0].indent);
    if (!root.isObj()) {
        if (error) *error = "this doesn't look like a macros.yaml";
        return false;
    }

    const js::Value* kbs = root.find("keyboards");
    const js::Value* profiles = root.find("profiles");
    if (!kbs || !kbs->isObj()) {
        if (error)
            *error = "no 'keyboards:' section found - is this a macrokb "
                     "macros.yaml?";
        return false;
    }

    if (const js::Value* s = root.find("settings")) {
        out.settings.blockByDefault = s->boolean("block_macro_keyboards", true);
        double conf = s->num("match_confidence", 0.85);
        if (conf > 0 && conf <= 1) out.settings.matchConfidence = conf;
        std::string dir = s->str("layouts_dir");
        if (!dir.empty()) out.settings.layoutsDir = dir;
    }

    int imported = 0;
    for (const auto& kv : kbs->obj) {
        const std::string& alias = kv.first;
        const js::Value& spec = kv.second;
        if (!spec.isObj()) continue;

        Keyboard kb;
        kb.alias = out.uniqueAlias(alias);
        kb.match = spec.str("contains");
        if (kb.match.empty()) {
            std::string vid = spec.str("vid"), pid = spec.str("pid");
            if (!vid.empty() && !pid.empty()) kb.match = "VID_" + vid + "&PID_" + pid;
        }
        kb.block = spec.boolean("block", out.settings.blockByDefault);
        kb.deck = "auto";

        if (profiles && profiles->isObj()) {
            if (const js::Value* prof = profiles->find(alias)) {
                if (prof->isObj())
                    for (const auto& bkv : prof->obj) {
                        Binding b;
                        b.key = lower(bkv.first);
                        if (b.key.empty() || !keys::toVk(b.key)) continue;
                        if (bkv.second.isArr())
                            for (const js::Value& st : bkv.second.arr) {
                                if (!st.isObj()) continue;
                                Step step;
                                step.action = st.str("do");
                                if (step.action.empty()) continue;
                                if (!findAction(step.action)) continue;
                                step.params = js::Value::object();
                                for (const auto& p : st.obj)
                                    if (p.first != "do")
                                        step.params[p.first] = p.second;
                                b.steps.push_back(std::move(step));
                            }
                        kb.active().bindings.push_back(std::move(b));
                    }
            }
        }
        out.keyboards.push_back(std::move(kb));
        ++imported;
    }

    if (!imported) {
        if (error) *error = "no keyboards found in that file";
        return false;
    }
    out.dirty = true;
    return true;
}

}  // namespace cfg
