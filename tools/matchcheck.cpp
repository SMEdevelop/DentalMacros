// Feeds a real device path and virtual key through the same lookup the engine
// does, against a real macros.json. Answers one question and no other: with
// this config, would a press of this key on this device find a macro?
//
//     build\matchcheck.exe <macros.json> <devicePath> <vk-hex>
#include <cstdio>
#include <string>

#include "config.h"
#include "keys.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: matchcheck <macros.json> <devicePath> <vk-hex>\n");
        return 2;
    }
    const std::string file = argv[1];
    const std::string path = argv[2];
    const int vk = (int)strtol(argv[3], nullptr, 16);

    cfg::Config c;
    c.load(file);

    std::printf("config:        %s\n", file.c_str());
    std::printf("decks:         %d\n", (int)c.keyboards.size());
    for (const auto& k : c.keyboards) {
        std::printf("  \"%s\" match=%s isMouse=%d block=%d blockAll=%d "
                    "softwareOnly=%d pages=%d activePage=%d\n",
                    k.alias.c_str(), k.match.c_str(), (int)k.isMouse,
                    (int)k.block, (int)k.blockAllKeys, (int)k.softwareOnly,
                    (int)k.pages.size(), k.activePage);
        for (size_t p = 0; p < k.pages.size(); ++p) {
            std::printf("    page %d \"%s\": ", (int)p, k.pages[p].name.c_str());
            for (const auto& b : k.pages[p].bindings)
                std::printf("[%s:%d steps] ", b.key.c_str(), (int)b.steps.size());
            std::printf("\n");
        }
    }

    const std::string name = keys::vkName(vk);
    std::printf("\nevent:         vk=0x%02X -> keys::vkName = \"%s\"\n", vk,
                name.c_str());
    std::printf("device:        %s\n", path.c_str());

    const cfg::Keyboard* kb = c.keyboardForDevice(path);
    std::printf("keyboardFor:   %s\n",
                kb ? ("MATCHED \"" + kb->alias + "\"").c_str() : "NO MATCH");
    if (!kb) return 1;

    const cfg::Binding* b = kb->findKey(name);
    std::printf("findKey(\"%s\"): %s\n", name.c_str(),
                b ? "FOUND" : "not on the active page");
    if (b) std::printf("steps:         %d\n", (int)b->steps.size());
    std::printf("anyPageHasMacro: %d\n", (int)kb->anyPageHasMacro(name));
    std::printf("couldBlockVk:  %d\n", (int)c.couldBlockVk(vk));
    std::printf("blockedForKey: %d\n", (int)c.blockedForKey(path, vk));
    return b && !b->steps.empty() ? 0 : 1;
}
