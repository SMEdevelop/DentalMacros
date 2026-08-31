#include "keys.h"
#include "util.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>

namespace keys {
namespace {

struct Tables {
    std::unordered_map<std::string, int> nameToVk;
    std::map<int, std::string> vkToName;
    std::unordered_map<std::string, std::string> aliases;
    std::vector<std::string> sortedNames;
    std::set<int> modifiers;
    std::set<int> extended;

    void add(const std::string& n, int vk) {
        nameToVk[n] = vk;
        if (!vkToName.count(vk)) vkToName[vk] = n;  // first wins
    }

    Tables() {
        for (int c = 0; c < 26; ++c) add(std::string(1, (char)('a' + c)), 0x41 + c);
        for (int d = 0; d < 10; ++d) add(std::string(1, (char)('0' + d)), 0x30 + d);
        for (int f = 1; f <= 24; ++f) add("f" + std::to_string(f), 0x70 + f - 1);
        for (int d = 0; d < 10; ++d) add("num" + std::to_string(d), 0x60 + d);

        add("nummultiply", 0x6A); add("numadd", 0x6B); add("numsubtract", 0x6D);
        add("numdecimal", 0x6E);  add("numdivide", 0x6F); add("numlock", 0x90);

        add("shift", 0x10); add("ctrl", 0x11); add("alt", 0x12);
        add("lshift", 0xA0); add("rshift", 0xA1);
        add("lctrl", 0xA2); add("rctrl", 0xA3);
        add("lalt", 0xA4); add("ralt", 0xA5);
        add("win", 0x5B); add("lwin", 0x5B); add("rwin", 0x5C);
        add("apps", 0x5D);

        add("up", 0x26); add("down", 0x28); add("left", 0x25); add("right", 0x27);
        add("home", 0x24); add("end", 0x23);
        add("pageup", 0x21); add("pagedown", 0x22);
        add("insert", 0x2D); add("delete", 0x2E);

        add("backspace", 0x08); add("tab", 0x09); add("enter", 0x0D);
        add("esc", 0x1B); add("space", 0x20); add("capslock", 0x14);
        add("scrolllock", 0x91); add("pause", 0x13); add("printscreen", 0x2C);

        add("semicolon", 0xBA); add("equals", 0xBB); add("comma", 0xBC);
        add("minus", 0xBD); add("period", 0xBE); add("slash", 0xBF);
        add("backtick", 0xC0); add("lbracket", 0xDB); add("backslash", 0xDC);
        add("rbracket", 0xDD); add("quote", 0xDE);

        add("volumemute", 0xAD); add("volumedown", 0xAE); add("volumeup", 0xAF);
        add("medianext", 0xB0); add("mediaprev", 0xB1); add("mediastop", 0xB2);
        add("mediaplay", 0xB3);
        add("browserback", 0xA6); add("browserforward", 0xA7);
        add("browserrefresh", 0xA8);

        // Pointing devices. The buttons keep the virtual keys Windows already
        // reserves for them, which no keyboard ever sends, so a mouse button
        // is a key like any other everywhere above here. The wheel has no
        // virtual key of its own and borrows four from the unassigned block -
        // see device.h.
        add("mouseleft", 0x01); add("mouseright", 0x02);
        add("mousemiddle", 0x04);
        add("mouse4", 0x05); add("mouse5", 0x06);
        add("wheelup", 0x88); add("wheeldown", 0x89);
        add("wheelleft", 0x8A); add("wheelright", 0x8B);
        // The extra buttons a multi-button mouse carries. Numbered the way
        // the hardware and its own software number them, continuing straight
        // on from the thumb buttons.
        for (int b = 6; b <= 20; ++b)
            add("mouse" + std::to_string(b), 0xC1 + (b - 6));

        aliases = {
            {"escape", "esc"}, {"return", "enter"}, {"control", "ctrl"},
            {"windows", "win"}, {"del", "delete"}, {"ins", "insert"},
            {"pgup", "pageup"}, {"pgdn", "pagedown"}, {"spacebar", "space"},
            {"plus", "equals"}, {"dash", "minus"}, {"grave", "backtick"},
            {"tilde", "backtick"}, {"prtsc", "printscreen"},
            {"break", "pause"}, {"caps", "capslock"},
            // the keycap next to right Ctrl is VK_APPS; VK_MENU is Alt
            {"menu", "apps"}, {"contextmenu", "apps"},
            // Mice get called all sorts of things. "Back" and "forward" are
            // what the two thumb buttons do on nearly every mouse that has
            // them, so they are worth accepting even though the hardware only
            // ever calls them 4 and 5.
            {"lmb", "mouseleft"}, {"rmb", "mouseright"},
            {"mmb", "mousemiddle"},
            {"mouse1", "mouseleft"}, {"mouse2", "mouseright"},
            {"mouse3", "mousemiddle"},
            {"mouseback", "mouse4"}, {"mouseforward", "mouse5"},
            {"scrollup", "wheelup"}, {"scrolldown", "wheeldown"},
        };

        modifiers = {0x10, 0x11, 0x12, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5,
                     0x5B, 0x5C};
        extended = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
                    0x2D, 0x2E, 0x5B, 0x5C, 0x5D, 0x6F, 0x90, 0xA3, 0xA5};

        for (auto& kv : nameToVk) sortedNames.push_back(kv.first);
        std::sort(sortedNames.begin(), sortedNames.end());
    }
};

const Tables& tab() {
    static Tables t;
    return t;
}

}  // namespace

int toVk(const std::string& name) {
    std::string n = lower(trim(name));
    if (n.empty()) return 0;
    auto& T = tab();
    auto a = T.aliases.find(n);
    if (a != T.aliases.end()) n = a->second;
    auto it = T.nameToVk.find(n);
    if (it != T.nameToVk.end()) return it->second;
    // accept the vk_0x## fallback form so round-tripping never loses a key
    if (n.rfind("vk_0x", 0) == 0) {
        int v = (int)strtol(n.c_str() + 5, nullptr, 16);
        if (v > 0 && v < 256) return v;
    }
    return 0;
}

std::string vkName(int vk) {
    auto& T = tab();
    auto it = T.vkToName.find(vk);
    if (it != T.vkToName.end()) return it->second;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "vk_0x%02X", vk & 0xFF);
    return buf;
}

std::string displayName(const std::string& keyName) {
    std::string n = lower(trim(keyName));
    auto& T = tab();
    auto a = T.aliases.find(n);
    if (a != T.aliases.end()) n = a->second;

    static const std::unordered_map<std::string, std::string> pretty = {
        {"numadd", "Num +"}, {"numsubtract", "Num −"},
        {"nummultiply", "Num ×"}, {"numdivide", "Num ÷"},
        {"numdecimal", "Num ."}, {"numlock", "NumLk"},
        {"pageup", "PgUp"}, {"pagedown", "PgDn"},
        {"printscreen", "PrtSc"}, {"scrolllock", "ScrLk"},
        {"capslock", "Caps"}, {"backspace", "Bksp"},
        {"esc", "Esc"}, {"enter", "Enter"}, {"space", "Space"},
        {"apps", "Menu"}, {"insert", "Ins"}, {"delete", "Del"},
        {"mouseleft", "Left click"}, {"mouseright", "Right click"},
        {"mousemiddle", "Middle click"},
        {"mouse4", "Back"}, {"mouse5", "Forward"},
        {"mouse6", "Button 6"}, {"mouse7", "Button 7"},
        {"mouse8", "Button 8"}, {"mouse9", "Button 9"},
        {"mouse10", "Button 10"}, {"mouse11", "Button 11"},
        {"mouse12", "Button 12"},
        {"wheelup", "Wheel \u2191"}, {"wheeldown", "Wheel \u2193"},
        {"wheelleft", "Wheel \u2190"}, {"wheelright", "Wheel \u2192"},
        {"semicolon", ";"}, {"equals", "="}, {"comma", ","}, {"minus", "-"},
        {"period", "."}, {"slash", "/"}, {"backtick", "`"},
        {"lbracket", "["}, {"backslash", "\\"}, {"rbracket", "]"},
        {"quote", "'"},
        {"volumemute", "Mute"}, {"volumedown", "Vol −"},
        {"volumeup", "Vol +"}, {"medianext", "Next"}, {"mediaprev", "Prev"},
        {"mediastop", "Stop"}, {"mediaplay", "Play"},
        {"browserback", "Back"}, {"browserforward", "Fwd"},
        {"browserrefresh", "Reload"},
        {"up", "↑"}, {"down", "↓"}, {"left", "←"},
        {"right", "→"},
    };
    auto p = pretty.find(n);
    if (p != pretty.end()) return p->second;

    if (n.size() > 3 && n.rfind("num", 0) == 0 && isdigit((unsigned char)n[3]))
        return "Num " + n.substr(3);
    if (n.size() == 1) return std::string(1, (char)toupper((unsigned char)n[0]));
    if (!n.empty() && n[0] == 'f' && n.size() <= 3 &&
        isdigit((unsigned char)n[1]))
        return "F" + n.substr(1);
    if (!n.empty())
        return std::string(1, (char)toupper((unsigned char)n[0])) + n.substr(1);
    return keyName;
}

std::vector<int> parseCombo(const std::string& combo, std::string* error) {
    std::vector<int> out;
    std::string cleaned = replaceAll(combo, " ", "");
    if (cleaned.empty()) {
        if (error) *error = "empty key combo";
        return out;
    }
    for (const std::string& part : split(cleaned, '+')) {
        if (part.empty()) continue;
        int vk = toVk(part);
        if (!vk) {
            if (error) *error = "unknown key name: " + part;
            out.clear();
            return out;
        }
        out.push_back(vk);
    }
    if (out.empty() && error) *error = "empty key combo";
    return out;
}

bool parseHotkey(const std::string& combo, unsigned& modifiers, int& vk,
                 std::string* error) {
    modifiers = 0;
    vk = 0;
    for (const std::string& part : split(lower(replaceAll(combo, " ", "")), '+')) {
        if (part.empty()) continue;
        if (part == "ctrl" || part == "control") modifiers |= MOD_CONTROL;
        else if (part == "alt") modifiers |= MOD_ALT;
        else if (part == "shift") modifiers |= MOD_SHIFT;
        else if (part == "win" || part == "windows") modifiers |= MOD_WIN;
        else {
            int v = toVk(part);
            if (!v) {
                if (error) *error = "unknown key name: " + part;
                return false;
            }
            if (vk) {
                if (error)
                    *error = "a shortcut can only have one non-modifier key";
                return false;
            }
            vk = v;
        }
    }
    if (!vk) {
        if (error) *error = "the shortcut needs a key, not just modifiers";
        return false;
    }
    if (!modifiers) {
        if (error)
            *error = "add Ctrl, Alt, Shift or Win - a shortcut without one "
                     "would take that key away from every other program";
        return false;
    }
    return true;
}

std::string prettyCombo(const std::string& combo) {
    unsigned mods = 0;
    int vk = 0;
    if (!parseHotkey(combo, mods, vk, nullptr)) return trim(combo);
    std::string out;
    // Windows' own order, not whatever order the user typed them in.
    if (mods & MOD_CONTROL) out += "Ctrl+";
    if (mods & MOD_ALT) out += "Alt+";
    if (mods & MOD_SHIFT) out += "Shift+";
    if (mods & MOD_WIN) out += "Win+";
    return out + displayName(vkName(vk));
}

const std::vector<std::string>& allNames() { return tab().sortedNames; }

bool isModifier(int vk) { return tab().modifiers.count(vk) > 0; }
bool needsExtendedFlag(int vk) { return tab().extended.count(vk) > 0; }

}  // namespace keys
