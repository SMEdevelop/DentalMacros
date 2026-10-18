// Runs the real engine against a real macros.json, with the log and the key
// monitor both printed. No window, no UI - just the input path, so "did the
// press reach the engine, and did it find a macro" can be answered without
// reading anything off a screen.
//
//     build\enginetest.exe <macros.json>
//
// Every key event on every device prints one line. A press that matches a
// deck says so, and a press that has a macro says that too, which is exactly
// the difference between "Dental Macros never saw it" and "Dental Macros saw it and
// had nothing bound to it".
#include <windows.h>

#include <cstdio>
#include <string>

#include "config.h"
#include "engine.h"
#include "keys.h"

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::printf("usage: enginetest <macros.json>\n");
        return 2;
    }

    cfg::Config c;
    c.load(argv[1]);
    std::printf("Loaded %s - %d deck(s)\n\n", argv[1], (int)c.keyboards.size());

    eng::Engine e;
    e.setLog([](const std::string& m) {
        std::printf("LOG      %s\n", m.c_str());
        std::fflush(stdout);
    });
    e.setMonitor([](const eng::MonitorEvent& ev) {
        std::printf("%-4s vk=0x%02X %-10s deck=%-14s macro=%s\n",
                    ev.down ? "DOWN" : "UP", ev.vk, ev.keyName.c_str(),
                    ev.alias.empty() ? "(none)" : ev.alias.c_str(),
                    ev.hasMacro ? "YES" : "no");
        std::fflush(stdout);
    });
    e.updateConfig(c);

    try {
        e.start();
    } catch (const std::exception& ex) {
        std::printf("START FAILED: %s\n", ex.what());
        return 1;
    }
    std::printf("\nEngine running. Press the buttons. Ctrl+C to stop.\n\n");
    std::fflush(stdout);

    // The engine's own threads do the work; this one only has to stay alive.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    e.stop();
    return 0;
}
