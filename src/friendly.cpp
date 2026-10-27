#include "friendly.h"

#include "util.h"

#include <cstdlib>

namespace fe {

Friendly explain(Problem p, const std::string& s, const std::string& tech) {
    Friendly f;
    f.technical = tech;
    switch (p) {
        case Problem::ConfigDamaged:
            f.title = "Your buttons couldn't be loaded";
            f.body = "Your settings file is damaged, so Dental Macros opened "
                     "with no buttons. The file itself hasn't been touched.";
            if (!s.empty()) {
                f.body += "\n\nYou can bring your buttons back from the copy "
                          "saved on " + s + ".";
                f.action = "Restore";
            } else {
                f.body += "\n\nIf it was changed by hand, undo that change, "
                          "then choose Settings > Reload from file.";
            }
            break;
        case Problem::ConfigLocked:
            f.title = "Your buttons couldn't be loaded";
            f.body = "Another program has your settings file open - often "
                     "OneDrive or backup software - so Dental Macros opened "
                     "with no buttons.\n\nWait a moment, then press Try again.";
            f.action = "Try again";
            break;
        case Problem::SaveDenied:
            f.title = "Your changes can't be saved";
            // Only reached once the profile folder has refused as well: a
            // program folder that refuses on its own is handled quietly.
            f.body = "Windows won't let Dental Macros save its settings - not "
                     "in its own folder, and not in your Windows profile "
                     "either - so your changes will be lost when it "
                     "closes.\n\nAsk whoever looks after this computer to "
                     "check it.";
            break;
        case Problem::SaveDiskFull:
            f.title = "Your disk is full";
            f.body = "There is no room left to save your changes. Free up some "
                     "space and they will save with your next change.";
            break;
        case Problem::SaveLocked:
            f.title = "Your changes haven't been saved yet";
            f.body = "Another program has your settings file open - often "
                     "OneDrive or backup software. Your changes will be saved "
                     "with your next change once it lets go.";
            break;
        case Problem::SaveFailed:
            f.title = "Your changes haven't been saved yet";
            f.body = "Dental Macros couldn't save your changes just now. It "
                     "will try again with your next change.";
            break;
        case Problem::InputBlocked:
            f.title = "Dental Macros couldn't start listening";
            f.body = "Windows or your security software stopped it from "
                     "reading your keypad.\n\nRestart the computer and try "
                     "again. If it keeps happening, ask whoever looks after "
                     "your security software to allow Dental Macros.";
            break;
        case Problem::TargetElevated:
            f.title = "Windows blocked a button";
            f.body = (s.empty() ? std::string("The program in front") : s) +
                     " is running as administrator, so Windows won't let "
                     "other programs type or click into it.\n\nAsk whoever set "
                     "up this computer to run it normally, or right-click "
                     "Dental Macros and choose Run as administrator.";
            break;
        case Problem::InputRefused:
            f.title = "Windows blocked a button";
            f.body = "Windows wasn't accepting typing or clicks from other "
                     "programs just then. That happens while the computer is "
                     "locked or a Windows permission box is showing.";
            break;
        case Problem::WindowMissing:
            f.title = "A button needs " + (s.empty() ? "a program" : s);
            f.body = "Open " + (s.empty() ? "the program it works in" : s) +
                     " and press the button again.";
            break;
        case Problem::ProgramMissing:
            f.title = "Something to open couldn't be found";
            f.body = "We couldn't find " + (s.empty() ? "it" : s) +
                     ". It may have been moved, renamed or uninstalled.";
            break;
        case Problem::ImageNotFound:
            f.title = "Couldn't find what to click";
            f.body = "The picture this button looks for isn't on screen. Make "
                     "sure the right window is showing, then try again.";
            break;
        case Problem::ImageFileMissing:
            f.title = "A button's picture is missing";
            f.body = "The picture " + (s.empty() ? "it" : s) +
                     " looks for has been moved or deleted. Snip it again in "
                     "the button's step.";
            break;
        case Problem::Unexpected:
            f.title = "Something went wrong";
            f.body = "Dental Macros needs to close. Your buttons are saved. "
                     "Open it again to carry on.";
            break;
        case Problem::None:
        default:
            f.title = "Something went wrong";
            f.body = tech.empty() ? "Dental Macros ran into a problem." : tech;
            f.technical.clear();   // already in the body
            break;
    }
    return f;
}

Problem saveProblem(unsigned long e) {
    switch (e) {
        case 5:      // ERROR_ACCESS_DENIED
        case 19:     // ERROR_WRITE_PROTECT
            return Problem::SaveDenied;
        case 39:     // ERROR_HANDLE_DISK_FULL
        case 112:    // ERROR_DISK_FULL
            return Problem::SaveDiskFull;
        case 32:     // ERROR_SHARING_VIOLATION
        case 33:     // ERROR_LOCK_VIOLATION
        case 1224:   // ERROR_USER_MAPPED_FILE
            return Problem::SaveLocked;
        default:
            return Problem::SaveFailed;
    }
}

std::string stoppedLine(const std::string& button, int step, Problem p,
                        const std::string& s, const std::string& technical) {
    std::string why;
    switch (p) {
        case Problem::TargetElevated:
            why = (s.empty() ? std::string("the program in front") : s) +
                  " runs as administrator, so Windows blocked the typing and "
                  "clicks.";
            break;
        case Problem::InputRefused:
            why = "Windows wasn't accepting typing or clicks just then.";
            break;
        case Problem::WindowMissing:
            why = (s.empty() ? std::string("the program it needs") : s) +
                  " isn't open.";
            break;
        case Problem::ProgramMissing:
            why = "couldn't find " + (s.empty() ? std::string("what it opens")
                                                : s) + ".";
            break;
        case Problem::ImageNotFound:
            why = "the picture it looks for isn't on screen.";
            break;
        case Problem::ImageFileMissing:
            why = "its picture file is missing.";
            break;
        default:
            why = technical.empty() ? "something went wrong." : technical;
            break;
    }
    std::string line = "\"" + button + "\" stopped";
    if (step > 0) line += " at step " + std::to_string(step);
    return line + ": " + why;
}

std::string programName(const std::string& exeOrPath) {
    std::string name = baseName(exeOrPath);
    if (name.size() > 4 && iequals(name.substr(name.size() - 4), ".exe"))
        name.resize(name.size() - 4);
    if (!name.empty() && name[0] >= 'a' && name[0] <= 'z')
        name[0] = (char)(name[0] - 'a' + 'A');
    return name;
}

std::string friendlyDate(const std::string& d) {
    static const char* kMonths[] = {"January", "February", "March",
                                    "April",   "May",      "June",
                                    "July",    "August",   "September",
                                    "October", "November", "December"};
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return d;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (d[(size_t)i] < '0' || d[(size_t)i] > '9') return d;
    const int month = std::atoi(d.substr(5, 2).c_str());
    const int day = std::atoi(d.substr(8, 2).c_str());
    if (month < 1 || month > 12 || day < 1 || day > 31) return d;
    return std::to_string(day) + " " + kMonths[month - 1] + " " + d.substr(0, 4);
}

}  // namespace fe
