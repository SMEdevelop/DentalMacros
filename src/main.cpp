#include "app.h"
#include "gdiplus_inc.h"
#include "util.h"

#include <exception>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int showCmd) {
    // A copy from before the rename to Dental Macros holds a mutex under its
    // old name, so the check below cannot see it - yet it would hook the
    // same keyboards and double-fire every macro just the same.
    if (HANDLE old = OpenMutexW(SYNCHRONIZE, FALSE, L"MacroDeck.SingleInstance")) {
        CloseHandle(old);
        MessageBoxW(nullptr,
                    L"MacroDeck - the old name of Dental Macros - is still "
                    L"running.\n\nClose it from its icon in the notification "
                    L"area, then start Dental Macros again.",
                    L"Dental Macros", MB_ICONINFORMATION);
        return 0;
    }

    // One instance only - two engines fighting over the same keyboard hook
    // would be confusing and would double-fire every macro.
    HANDLE once = CreateMutexW(nullptr, TRUE, L"DentalMacros.SingleInstance");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(L"DentalMacrosMain", nullptr)) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    migrateRunAtLogin();

    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartupInput gdiInput;
    if (Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"Dental Macros could not start its graphics engine.",
                    L"Dental Macros", MB_ICONERROR);
        return 1;
    }

    int rc = 1;
    try {
        rc = ui::runApp(inst, showCmd);
    } catch (const std::exception& e) {
        MessageBoxW(nullptr, widen(e.what()).c_str(),
                    L"Dental Macros - unexpected error", MB_ICONERROR);
    }

    // Every GDI+ object must already be gone by this point; the window class
    // releases its caches in WM_DESTROY.
    Gdiplus::GdiplusShutdown(gdiToken);
    if (once) CloseHandle(once);
    return rc;
}
