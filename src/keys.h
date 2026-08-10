// Key name <-> Windows Virtual-Key tables. Names are lowercase and stable:
// they appear in the config file and in the UI.
#pragma once
#include <string>
#include <vector>

namespace keys {

// Resolve a name (case-insensitive, aliases accepted) to a VK code.
// Returns 0 if the name is unknown.
int toVk(const std::string& name);

// Human-readable name for a VK code, e.g. "num1". Unknown codes come back
// as "vk_0x##" so nothing is ever silently lost.
std::string vkName(int vk);

// Pretty label for the UI, e.g. "Num 1" / "Ctrl".
std::string displayName(const std::string& keyName);

// "ctrl+shift+t" -> {VK_CONTROL, VK_SHIFT, 'T'}; empty on error.
std::vector<int> parseCombo(const std::string& combo, std::string* error = nullptr);

// Split a combo like "ctrl+alt+m" into RegisterHotKey modifier flags and the
// one non-modifier key. Returns false if a name isn't recognised, if there is
// no final key, or if there are no modifiers - a system-wide shortcut with no
// modifier would swallow that key everywhere.
bool parseHotkey(const std::string& combo, unsigned& modifiers, int& vk,
                 std::string* error = nullptr);

// Format a combo the way Windows writes one: "ctrl+alt+shift+m" becomes
// "Ctrl+Alt+Shift+M". Anything unparseable comes back unchanged.
std::string prettyCombo(const std::string& combo);

// Every canonical key name, sorted - used to populate pickers.
const std::vector<std::string>& allNames();

bool isModifier(int vk);
bool needsExtendedFlag(int vk);

}  // namespace keys
