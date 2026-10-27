// What went wrong, in words a front-desk person can act on.
//
// Failures that the app knows how to explain carry a Problem from the place
// they happen (an action, the engine, a save) up to the place that tells the
// user. The wording for every one of them lives here and nowhere else, so it
// stays consistent and the self-test can read it. The original technical text
// always travels alongside, for the activity log and "Copy details".
#pragma once
#include <stdexcept>
#include <string>

namespace fe {

enum class Problem {
    None,               // nothing more specific known: show the technical text

    // the settings file
    ConfigDamaged,      // macros.json is there but is not valid
    ConfigLocked,       // macros.json is there but Windows would not open it
    SaveDenied,         // the folder or file refuses writes
    SaveDiskFull,
    SaveLocked,         // another program has the file open
    SaveFailed,         // any other save failure

    // listening
    InputBlocked,       // the keyboard listener or hook could not start

    // running a button
    TargetElevated,     // the program in front runs at a higher level than us
    InputRefused,       // Windows refused synthetic input (locked, UAC prompt)
    WindowMissing,      // a step needs a program that is not open
    ProgramMissing,     // a file, folder, link or program to open is not there
    ImageNotFound,      // the picture a step looks for is not on screen
    ImageFileMissing,   // the picture file itself is gone

    Unexpected,         // something we did not foresee; the app must close
};

// One explained problem, ready for a dialog.
struct Friendly {
    std::string title;
    std::string body;        // what happened and what to do, 1-3 sentences
    std::string action;      // the button that fixes it; "" = none
    std::string technical;   // for "Copy details" and the log only
};

// `subject` is the program, button, file or date the sentence is about,
// already in the user's words ("OpenDental", "25 October 2026").
Friendly explain(Problem p, const std::string& subject = "",
                 const std::string& technical = "");

// The Problem behind a Win32 error from a failed save.
Problem saveProblem(unsigned long win32Error);

// The status line when a button stopped part-way, for example
//   "Insurance note" stopped at step 3: OpenDental isn't open.
// `step` is 1-based; 0 leaves it out.
std::string stoppedLine(const std::string& button, int step, Problem p,
                        const std::string& subject,
                        const std::string& technical);

// "OpenDental" for "opendental.exe" or a full path to it.
std::string programName(const std::string& exeOrPath);

// "25 October 2026" for "2026-10-25". Anything else comes back unchanged.
std::string friendlyDate(const std::string& isoDate);

// A failure that knows which kind of problem it is. what() is the technical
// text, exactly as plain std::runtime_error failures have always read, so the
// log and anything matching on it are unchanged.
class Error : public std::runtime_error {
public:
    Error(Problem p, std::string subject, const std::string& technical)
        : std::runtime_error(technical), problem(p),
          subject(std::move(subject)) {}

    Problem problem;
    std::string subject;
    int step = 0;   // which step of a macro, 1-based, once known
};

}  // namespace fe
