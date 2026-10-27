#include "actions.h"
#include "friendly.h"
#include "keys.h"
#include "util.h"

#include <windows.h>
#include <shellapi.h>

#include <stdexcept>
#include <thread>

namespace act {
namespace {

void sendAll(std::vector<INPUT>& in) {
    if (in.empty()) return;
    UINT sent = SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
    if (sent != in.size())
        throw fe::Error(fe::Problem::InputRefused, "",
                        "SendInput was blocked: " + lastErrorMessage());
}

// The mandatory integrity level of a process (the last sub-authority of its
// label: 0x2000 medium, 0x3000 high...), or 0 if Windows won't say.
DWORD integrityOf(HANDLE process) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &tok)) return 0;
    DWORD level = 0;
    DWORD len = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &len);
    if (len > 0) {
        std::vector<BYTE> buf(len);
        if (GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), len,
                                &len)) {
            PSID sid = ((TOKEN_MANDATORY_LABEL*)buf.data())->Label.Sid;
            const UCHAR n = *GetSidSubAuthorityCount(sid);
            if (n > 0) level = *GetSidSubAuthority(sid, (DWORD)(n - 1));
        }
    }
    CloseHandle(tok);
    return level;
}

// Typing or clicking into a program that would silently throw it away is
// worse than not trying: the macro carries on as if it had worked. So stop
// with a reason instead.
void ensureReachable(HWND target) {
    const std::string program = blockedProgram(target);
    if (!program.empty())
        throw fe::Error(fe::Problem::TargetElevated, fe::programName(program),
                        program + " runs at a higher integrity level than "
                                  "Dental Macros, so Windows drops input sent "
                                  "to it");
}

INPUT keyInput(int vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) |
                    (keys::needsExtendedFlag(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    return in;
}

INPUT unicodeInput(wchar_t ch, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;
    in.ki.wScan = (WORD)ch;
    in.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
    return in;
}

void sleepMs(int ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

std::string blockedProgram(HWND target) {
    HWND top = target ? GetAncestor(target, GA_ROOT) : nullptr;
    if (!top) return "";
    DWORD pid = 0;
    GetWindowThreadProcessId(top, &pid);
    if (!pid || pid == GetCurrentProcessId()) return "";

    static const DWORD self = integrityOf(GetCurrentProcess());
    if (!self) return "";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return "";   // can't tell: let it try, as it always has
    std::string name;
    const DWORD level = integrityOf(p);
    if (level > self) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        name = QueryFullProcessImageNameW(p, 0, buf, &n) ? narrow(buf)
                                                         : "That program";
    }
    CloseHandle(p);
    return name;
}

void pressCombo(const std::string& combo, int holdMs) {
    std::string err;
    std::vector<int> vks = keys::parseCombo(combo, &err);
    if (vks.empty()) throw std::runtime_error(err.empty() ? "bad key combo" : err);
    ensureReachable(GetForegroundWindow());

    std::vector<INPUT> down;
    down.reserve(vks.size());
    for (int vk : vks) down.push_back(keyInput(vk, false));
    sendAll(down);

    sleepMs(holdMs);

    std::vector<INPUT> up;
    up.reserve(vks.size());
    for (auto it = vks.rbegin(); it != vks.rend(); ++it)
        up.push_back(keyInput(*it, true));
    sendAll(up);
}

void typeText(const std::string& text, int intervalMs) {
    ensureReachable(GetForegroundWindow());
    std::wstring w = widen(text);
    std::vector<INPUT> batch;
    for (size_t i = 0; i < w.size(); ++i) {
        wchar_t ch = w[i];
        if (ch == L'\r') continue;
        if (ch == L'\n') {
            if (!batch.empty()) { sendAll(batch); batch.clear(); }
            pressCombo("enter");
            sleepMs(intervalMs);
            continue;
        }
        batch.push_back(unicodeInput(ch, false));
        batch.push_back(unicodeInput(ch, true));
        // With no per-character delay we can send the whole run in one call,
        // which is dramatically faster and less prone to interleaving.
        if (intervalMs > 0) {
            sendAll(batch);
            batch.clear();
            sleepMs(intervalMs);
        } else if (batch.size() >= 200) {
            sendAll(batch);
            batch.clear();
        }
    }
    if (!batch.empty()) sendAll(batch);
}

void moveMouse(int x, int y) {
    if (!SetCursorPos(x, y))
        throw std::runtime_error("could not move the cursor: " +
                                 lastErrorMessage());
}

void getMousePos(int& x, int& y) {
    POINT p{};
    GetCursorPos(&p);
    x = p.x;
    y = p.y;
}

void click(bool hasPos, int x, int y, const std::string& button,
           bool doubleClick, bool restorePos) {
    int oldX = 0, oldY = 0;
    if (restorePos) getMousePos(oldX, oldY);
    if (hasPos) {
        moveMouse(x, y);
        sleepMs(10);
    }

    DWORD downFlag, upFlag;
    std::string b = lower(button);
    if (b == "right") { downFlag = MOUSEEVENTF_RIGHTDOWN; upFlag = MOUSEEVENTF_RIGHTUP; }
    else if (b == "middle") { downFlag = MOUSEEVENTF_MIDDLEDOWN; upFlag = MOUSEEVENTF_MIDDLEUP; }
    else if (b == "left" || b.empty()) { downFlag = MOUSEEVENTF_LEFTDOWN; upFlag = MOUSEEVENTF_LEFTUP; }
    else throw std::runtime_error("unknown mouse button: " + button);

    try {
        POINT at{};
        GetCursorPos(&at);
        ensureReachable(WindowFromPoint(at));
    } catch (...) {
        if (restorePos) moveMouse(oldX, oldY);
        throw;
    }

    auto oneClick = [&] {
        std::vector<INPUT> in(2);
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = downFlag;
        in[1].type = INPUT_MOUSE;
        in[1].mi.dwFlags = upFlag;
        sendAll(in);
    };
    oneClick();
    if (doubleClick) {
        sleepMs(50);
        oneClick();
    }
    if (restorePos) {
        sleepMs(20);
        moveMouse(oldX, oldY);
    }
}

void scroll(int notches) {
    std::vector<INPUT> in(1);
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_WHEEL;
    in[0].mi.mouseData = (DWORD)(notches * WHEEL_DELTA);
    sendAll(in);
}

void runCommand(const std::string& cmd, bool wait) {
    if (trim(cmd).empty()) throw std::runtime_error("no command given");

    // Route through the shell so PATH lookup, .bat files and arguments all
    // behave the way they would if typed into Run.
    std::wstring comspec;
    {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"ComSpec", buf, MAX_PATH);
        comspec = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n)
                                          : L"cmd.exe";
    }
    std::wstring line = L"\"" + comspec + L"\" /c " + widen(cmd);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;   // don't flash a console window
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> mutableLine(line.begin(), line.end());
    mutableLine.push_back(L'\0');

    if (!CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, widen(exeDir()).c_str(),
                        &si, &pi))
        throw std::runtime_error("could not run '" + cmd + "': " +
                                 lastErrorMessage());
    if (wait) WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

void openPath(const std::string& target) {
    if (trim(target).empty()) throw std::runtime_error("nothing to open");
    HINSTANCE r = ShellExecuteW(nullptr, L"open", widen(target).c_str(),
                                nullptr, widen(exeDir()).c_str(), SW_SHOWNORMAL);
    const INT_PTR code = (INT_PTR)r;
    if (code == SE_ERR_FNF || code == SE_ERR_PNF)
        throw fe::Error(fe::Problem::ProgramMissing, baseName(target),
                        "could not open '" + target + "': not found");
    if (code <= 32)
        throw std::runtime_error("could not open '" + target + "' (error " +
                                 std::to_string((long long)code) + ")");
}

}  // namespace act
