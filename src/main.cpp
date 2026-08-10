#include "app.h"
#include "gdiplus_inc.h"
#include "util.h"

#include <exception>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int showCmd) {
    // One instance only - two engines fighting over the same keyboard hook
    // would be confusing and would double-fire every macro.
    HANDLE once = CreateMutexW(nullptr, TRUE, L"MacroDeck.SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"MacroDeckMain", nullptr)) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartupInput gdiInput;
    if (Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"MacroDeck could not start its graphics engine.",
                    L"MacroDeck", MB_ICONERROR);
        return 1;
    }

    int rc = 1;
    try {
        rc = ui::runApp(inst, showCmd);
    } catch (const std::exception& e) {
        MessageBoxW(nullptr, widen(e.what()).c_str(),
                    L"MacroDeck - unexpected error", MB_ICONERROR);
    }

    // Every GDI+ object must already be gone by this point; the window class
    // releases its caches in WM_DESTROY.
    Gdiplus::GdiplusShutdown(gdiToken);
    if (once) CloseHandle(once);
    return rc;
}
