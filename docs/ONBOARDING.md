# First run, Simple mode, and the click recorder: a redesign

Goal: a front-desk person who has never seen Dental Macros installs it and has
one working button in under a minute, with no manual and no IT call.

**Status (2026-10-27):** all six phases of §11 are built: the menu cleanup
with the regrouped Settings window (§5), the click recorder (§6),
plain-English errors with a status line (§7), the data-folder fallback with
keypad connection dots (§8), Simple mode (§4), and first-run setup with its
checks and dental-program discovery (§1-3). Each phase's notes below say what
was built and where it differs from the plan. Snippets in §9 for the unbuilt parts are written against
the classes that exist today but have not been compiled.

Where the build differs from this plan:

- The recorder is an ordinary always-on-top window, not `WS_EX_NOACTIVATE`,
  so it keeps the normal dialog message loop. Clicking it takes focus from the
  target program until the next left-click there.
- Undo / remove / redo are three buttons under the list (**Undo last**,
  **Remove**, **Redo this spot**) rather than a ✕ on every row and an implicit
  "click a row to redo".
- The toolbar has no Simple | Advanced switch yet (that is phase 5). It is
  `Start Listening · Show/Hide floating buttons · Settings`.
- Settings has no "Run setup again" (no wizard yet). "Show my backups" became
  **Show my settings file** (Explorer with `macros.json` selected).
- The step builder lives in `src/recorder.cpp` (`rec::buildSteps`,
  `rec::makesDouble`), covered by the "Click recorder" self-test section.
- Fixed on the way: recording into a key on a page tied to a program used to
  lose the clicks, because that program coming to the front switched pages and
  cleared the selection. The recorder now remembers the deck, page, key and
  gesture before it starts.

Phase 3, as built:

- Wording lives in `src/friendly.cpp` (`fe::explain`, `fe::stoppedLine`,
  `fe::saveProblem`). Failures are labelled where they happen with
  `fe::Error` (a `std::runtime_error` carrying a Problem, a subject and the
  step number), so the log text is unchanged. The engine reports every run
  through `Engine::setOutcome`. Covered by the "Plain-English problems"
  self-test section.
- The status line is drawn beside the **Activity** heading, so it survives
  Simple mode hiding the log. When the window is out of sight, a stopped
  button raises a notification-area balloon instead (at most one per 10 s).
- The "runs as administrator" check (§3 check 7) is done per action, not in a
  wizard: typing, key presses and clicks compare the target window's process
  integrity level with ours (`act::blockedProgram`) and stop the macro with a
  reason rather than letting Windows drop the input silently. Unknown means
  "carry on", so it can't break a macro that used to work.
- Damaged settings offer **Restore** of the newest daily backup that still
  loads (the damaged file is kept as `macros.json.bak`); a locked file offers
  **Try again**. Shown after the window is created, via a posted message.
- Saves retry a held file quietly (5 tries, 100 ms apart); a lasting failure
  is explained once per problem kind per run, then only on the status line.
  "Saving moved to your profile" waits for phase 4; until then a folder that
  refuses writes is explained as such.
- A taken off-switch shortcut borrows a stand-in for the session without
  changing the saved one.

Phase 4, as built:

- `dataDir()` in util.cpp: beside the exe when a new file can be created there
  and `macros.json` (if any) isn't read-only or denied; otherwise
  `%LOCALAPPDATA%\DentalMacros`, read from the environment variable. Unlike
  §9.2 it is *not* sticky: an exe folder that takes writes always wins, so a
  stray profile copy can never shadow an installed copy's settings.
  `chooseDataDir` takes the file-system checks as function pointers and is
  tested with a fake file system ("Where settings live").
- First move copies `macros.json`, `images\` and `layouts\`, never over an
  existing `macros.json`. A save refused mid-run with access denied moves to
  the profile and saves again (`moveDataToProfile`), reported on the status
  line; the SaveDenied dialog now only appears when the profile refuses too.
- `appPath()` still means the program folder; the user's files go through
  `dataPath()` and stored relative paths through `resolveDataPath()` (data
  folder, then program folder, then the working folder).
- Keypad dots use `WM_DEVICECHANGE`/`DBT_DEVNODES_CHANGED` (sent to every
  top-level window, no registration), debounced 700 ms, then match each
  deck's `match` against `listKeyboards()` + `listMice()`. Not yet checked
  against a Bluetooth keypad going to sleep and waking.

Phase 5, as built:

- `Settings::simpleMode`, saved as `"uiMode"`. Default true only when there is
  no `macros.json` at all; any file that exists - even a damaged one - loads
  as Advanced unless it says `"simple"`.
- `src/simple.cpp` (`smp::rowsFor`) groups steps into rows: Type, Keys, Wait,
  Clicks (lock .. clicks/pauses .. release, or bare clicks with the pauses
  between them), and Advanced (each run of anything else). Every step is in
  exactly one row, so removing or re-recording a row is a range operation.
  Covered by the "Simple mode" self-test section.
- Simple inspector: name, colour, "WHAT IT DOES" list, **+ Type text**,
  **+ Press keys**, **+ Record clicks**, **+ Wait**, **Try it**, **Remove**.
  Double-click edits a row in place (the same prompt that made it); a Clicks
  row is re-recorded into the same range (`recordClicks(first, count)`); an
  Advanced row switches to Advanced with its first step selected.
- Differences from §4: no "Edit" button in Simple (double-click does it);
  colour stays; the bottom band shows only the status line; the keypad
  right-click menu keeps Rename, "Find this keypad again" and Remove; wheel
  (dial) steps are simply not shown in Simple mode.

Phase 6, as built:

- `runSetupWizard` (dialogs.cpp): one window, three pages. The checks run
  when page 2 opens. Settings can be saved (always); for a keypad, an LL
  keyboard hook installs and the raw-input listener starts, and the off
  switch registered by the main window is shown (or a note when there is
  none); then up to two dental programs. A keypad is identified by the next
  key pressed - ignoring Enter, Tab, Esc, Space and modifiers, which the
  window itself answers to - and refused while only one keyboard device
  exists. **Next** stays disabled until nothing is a problem and, for a
  keypad, a key has been pressed.
- `Settings::setupDone` (`"setupDone"`): false only for a brand-new setup; any
  existing file, damaged or not, counts as set up. Setup is posted after the
  window is created, runs once, and is marked done when finished or skipped.
  The empty-deck card's button is now **Get started**, and Settings has **Run
  setup again**, which adds a deck without touching existing ones.
- The result: one deck (`My buttons`, software-only, or the keypad named by
  its product name when short enough, else `My keypad`) with one button -
  `btn1`, or the key that was pressed. Simple mode on; a keypad starts
  listening now and on every launch; the floating buttons open for the
  on-screen route when asked.
- Differs from §1-3: a keypad set up here uses `blockAllKeys = false` (only
  keys carrying a button stop typing), so a wrongly identified keyboard costs
  one key rather than all of them. No network, tray or Smart App Control
  rows. The elevation warning reuses `act::blockedProgram`. Discovery
  (`src/discover.cpp`) matches product names against uninstall entries,
  program file names and window titles, ignoring case and punctuation; the
  list has not been checked on a real office PC.

---

## 0. What the app does today, and what parts of the brief apply

**Today's first run.** No `macros.json` → empty config → the deck area shows
one card, *"Set up your macro keyboard"*, with one button, **Scan for
keyboards**. That opens a list of raw device names, then asks for a name, then
drops the user on an empty deck with a 132-px log, a 30-item action palette,
a deck-preset combo, page tabs, three gesture buttons and nine step buttons.
Someone with only on-screen buttons in mind has to find *Add software buttons*
in the sidebar on their own.

**Things that are already right and should stay:**

- Settings save on every change (`modelChanged()` → `saveConfig()`), with a
  daily rolling backup and a rescue copy when the file is damaged. Nobody ever
  edits JSON by hand.
- `macros.json` is created only when needed, and a missing file means
  "defaults", not an error.
- Right-click records a click so left-click still works for getting to the
  right screen. That is the correct call; the redesign keeps it and explains it
  better.
- `window_prepare` / `window_release` already lock the target window, so
  recorded clicks still land where they should.

**Parts of the brief that don't fit this app.** Dental Macros has no
practice-management data folder, no database, no port and no network
connection. It never reads PMS files and sends nothing anywhere (the About box
says so). So there is no "data folder" to discover and no network or database
check to show. Adding green ticks for those would be checks that can't fail and
mean nothing.

What *does* need discovering here:

| What the user would otherwise have to work out | Discovered from |
|---|---|
| Which device is their keypad | "Press any key on your keypad" (Raw Input already tells us which device sent it) |
| Whether the keypad is even connected | Number of keyboard devices, Bluetooth friendly names (already read for the picker) |
| Which dental programs are on this PC and open | Uninstall registry entries + running windows (`win::listWindows`) |
| Whether Windows will let us type/click into them | Elevation of the target process (a PMS running as administrator blocks `SendInput`) |
| Whether settings can be saved where the app sits | A write test in the app folder |
| Whether the emergency off shortcut is free | `RegisterHotKey` result |
| Whether there is a notification-area icon to hide to | `Shell_NotifyIcon` result |

---

## 1. Flow map

```
 Launch
   │
   ├─ macros.json has keyboards? ── yes ──► Main window (Advanced mode for
   │                                         existing users; nothing changes)
   │  no / missing
   ▼
 ┌──────────────────── STEP 1 · WELCOME ─────────────────────┐
 │  Q1  Where will your buttons be?                           │
 │      (●) On screen only        ( ) On a keypad            │
 │  Q2  What should your first button do?                     │
 │      (●) Type something        ( ) Click through screens  │
 │                                       [ Next ]             │
 └────────────────────────────────────────────────────────────┘
   │  (background checks start the moment this screen opens)
   ▼
 ┌──────────────────── STEP 2 · CHECKING ────────────────────┐
 │  ✓ Your settings can be saved                              │
 │  ✓ Dental Macros can type and click for you                │
 │  ✓ Emergency off switch: Ctrl + Alt + Shift + M            │
 │  ✓ Found Open Dental                       (if installed)  │
 │  ◌ Press any key on your keypad…          (keypad only)   │
 │      → ✓ Got it: "HP 430 Keypad"                           │
 │                                       [ Next ]             │
 └────────────────────────────────────────────────────────────┘
   │  every row is ✓, ⚠ (works, with a note), or ✗ (with a fix button)
   │  Next is enabled once nothing is ✗ (keypad row can be skipped →
   │  falls back to on-screen buttons)
   ▼
 ┌──────────────────── STEP 3 · READY ───────────────────────┐
 │  Make your first button                                    │
 │   Type:  [ Thank you for visiting! ______________ ]        │
 │      or  [ Record my clicks ]                              │
 │  [✓] Start Dental Macros when this computer starts         │
 │                                       [ Finish ]           │
 └────────────────────────────────────────────────────────────┘
   │
   ▼
 Main window, Simple mode, the new button selected,
 listening already on, floating buttons shown (on-screen route).
```

The wizard creates one button, not a tour. Every later button is made from the
main window, which in Simple mode has only what the first button needed.

**Time budget:** Step 1 is two clicks. Step 2 is zero clicks on the on-screen
route and one key press on the keypad route. Step 3 is typing a phrase or
recording clicks, then Finish. Well under 60 s for the typing route.

**Who sees it:** only a config with no keyboards *and* `setupDone == false`.
Anyone upgrading has keyboards, so they go straight to the main window in
Advanced mode, exactly as today. *Settings → Run setup again* reopens it.

---

## 2. The three screens: copy

### Step 1 · Welcome

> **Welcome to Dental Macros**
> Make buttons that type and click for you. Let's set up your first one. It
> takes about a minute.
>
> **Where will your buttons be?**
> ◉ **On my screen** — a small row of buttons that floats over your other
>   programs. *Nothing to plug in.*
> ○ **On a keypad** — a spare keypad or number pad. Its keys run your buttons
>   instead of typing.
>
> **What should your first button do?**
> ◉ **Type something** — a phrase, a note, a code you type all day.
> ○ **Click through screens** — click the same spots in a program for you, in
>   order.
>
> [ Next ]

Tooltips:
- *On my screen:* "Best if you don't have a keypad. You click the buttons with
  your mouse."
- *On a keypad:* "Your keypad's keys will only run buttons, so keep typing on
  your main keyboard."
- *Click through screens:* "You show Dental Macros where to click once, and
  it repeats it whenever you press the button."

### Step 2 · Checking

Heading: **Checking everything works**
Sub-line while running: *This only takes a moment.*
Sub-line when done: *All set.* / *One thing needs your attention.*

Rows and their copy are in §3.

Keypad row while waiting:
> **Press any key on your keypad**
> *We'll recognise it automatically.*
> [ My keypad isn't here ] (link)

Keypad row, recognised:
> ✓ **Found your keypad: HP 430 Programmable Keypad**
> *Its keys will run buttons and won't type.*

Keypad row, the key came from the only keyboard on the PC (see §3, check 5):
> ⚠ **That looks like your main keyboard**
> *Press a key on the keypad you want to use for buttons instead. If it's
> wireless, make sure it's switched on.*

"My keypad isn't here" opens:
> **Let's find your keypad**
> • **Wireless?** Switch it on, then [ Open Bluetooth settings ] and make sure
>   it says *Connected*.
> • **Plugged in?** Try another USB port.
> • Then press any key on it.
>
> [ Use on-screen buttons for now ]

### Step 3 · Ready

> **Make your first button**
>
> *(Type route)*
> **What should it type?**
> [ Thank you for visiting! See you at your next cleaning. ]
> **Name on the button:** [ Thank you ]  *(filled in from the first words)*
>
> *(Click route)*
> **Show us where to click**
> [ Record my clicks ]
> *A small window will guide you.* → after recording: ✓ **3 clicks recorded in
> Open Dental** [ Redo ]
>
> ☑ **Start Dental Macros when this computer starts**
> ☑ **Show my buttons on top of other windows** *(on-screen route only, and
> checked)*
>
> *Tip: if a keypad ever stops typing when you don't want it to, press
> **Ctrl + Alt + Shift + M** to switch Dental Macros off.* *(keypad route only)*
>
> [ Finish ]

After Finish, the status line in the main window says:
> ✓ **You're set. Press your new button to try it.**

---

## 3. The background checks

Each check returns one of `Ok`, `Note` (works, with something worth knowing),
`Fixed` (we changed a setting to make it work), or `Problem` (needs the user).
Only `Problem` blocks Next.

| # | Check | How | ✓ copy | Self-heal / ✗ copy |
|---|---|---|---|---|
| 1 | Settings can be saved | Write and delete `~write-test.tmp` in the data folder | **Your settings can be saved** | Not writable → switch the data folder to `%LOCALAPPDATA%\DentalMacros` and show `Fixed`: *"Your settings will be kept in your Windows profile."* Only ✗ if that fails too: *"Windows won't let Dental Macros save anything on this computer. Ask whoever looks after your computers to check it."* |
| 2 | Can listen to keypads | Start and stop a `dev::RawInputListener` | (folded into row 3's ✓) | ✗ *"Windows didn't let Dental Macros listen for key presses. Restart the computer and open Dental Macros again."* |
| 3 | Can type and click | Install and remove a `WH_KEYBOARD_LL` hook | **Dental Macros can type and click for you** | ✗ *"Your security software stopped Dental Macros from working with the keyboard. Ask whoever manages it to allow Dental Macros."* |
| 4 | Emergency off switch | `RegisterHotKey` for the saved shortcut | **Emergency off switch: Ctrl + Alt + Shift + M** | Taken → try the next of a short list (`ctrl+alt+shift+k`, `ctrl+alt+shift+f12`, …), save the first that registers, show `Fixed`: *"…is used by another program, so yours is Ctrl + Alt + Shift + K."* |
| 5 | Keypad (keypad route) | Listen for one key down; match to `dev::listKeyboards()`; if only one keyboard device exists, flag it | **Found your keypad: \<friendly name\>** | See Step 2 copy. Never a hard ✗; the user can switch to on-screen buttons. |
| 6 | Dental programs | Registry uninstall `DisplayName` + running windows (§9.4) | **Found Open Dental** (one row per program, max 3; hidden when none) | None found → no row. This is a convenience, not a requirement. |
| 7 | Not blocked by "Run as administrator" | For each running dental program, `TokenElevation` of its process vs ours | (no row when fine) | ⚠ *"Open Dental is running as administrator, so Windows won't let other programs type or click into it. Ask whoever set up this computer to run Open Dental normally, or right-click Dental Macros and choose Run as administrator."* |
| 8 | Notification-area icon | `Shell_NotifyIcon(NIM_ADD)` then delete | (no row when fine) | Fails → set `minimizeToTray = false`, no row. Closing the window then exits, which is safer than an unreachable hidden app. |

Deliberately left out: network, database, file-share and port checks (nothing
in the app uses them), and the Smart App Control check (if SAC blocks the exe,
the wizard never runs, so the check can't report anything; that belongs on the
download page and in the installer instead).

Check 7 is the one most likely to bite in a real office: several PMS installs
are set to "Run as administrator", and then every `SendInput` from a normal
process is silently dropped. Today that surfaces only as
`SendInput was blocked: …` in the log.

---

## 4. Simple and Advanced

One switch in the toolbar: **Simple | Advanced** (segmented, remembered in
settings). New installs start in Simple. Existing configs (any keyboard
present on first launch of this version) start in Advanced, so current users
see no change.

| Area | Simple | Advanced (today's UI) |
|---|---|---|
| Sidebar | "Your buttons" list of decks + **Add a keypad** / **Add on-screen buttons** | + Add a mouse or trackpad, the action palette |
| Deck header | hidden (deck preset = auto) | preset combo, page strip, padlock |
| Inspector | Button name, colour, **What it does** list, **+ Type text**, **+ Press keys**, **+ Record clicks**, **+ Wait**, **Try it**, **Remove step** | + Icon, gestures, Edit/Up/Down, every action |
| Step list | Plain sentences: *Type "Thank you…"*, *Press Ctrl + S*, *Click in Open Dental (3 clicks)*, *Wait 1 second* | Today's summaries |
| Bottom | One status line (last event, ✓ or ⚠) | Today's log |
| Right-click on a tile | Rename, Colour, Try it, Remove | today's menu |

Rules that stop Simple mode from hiding or losing work:

- A button that already has advanced steps shows them in Simple as a single
  grey line, *"+ 4 advanced steps"* with **Open in Advanced**. They are never
  dropped, reordered or re-saved differently.
- The recorded-click block (`window_prepare` … clicks … `window_release`) is
  shown as **one** row in Simple: *"Click in Open Dental (3 clicks)"*, with
  **Redo** and **Edit** (opens the recorder pre-filled). Removing it removes
  the whole block.
- Switching mode never changes the config, only what's shown.

---

## 5. The two menus in the top right

**Today** the toolbar has **Settings** (a dialog) and **More** (a menu of 12
items). Overlaps and dead weight:

| Item | Problem | New home |
|---|---|---|
| Save now | Every change already saves | **Delete** |
| Reload macros.json | A developer action; only useful after hand-editing the file | Advanced → Settings → Backup & restore ("Reload from file") |
| Back up settings now | A backup is already made every day automatically; the second one in a day is refused anyway | **Merge** with Export into "Save a copy…" |
| Export a copy… | — | Settings → Backup & restore |
| Import from macrokb… | Legacy, for users of the Python tool | Settings → Backup & restore (Advanced only) |
| Open the Dental Macros folder | Fine, but technical | Settings → Backup & restore ("Show my backups") |
| Buttons on top of everything | Also in the tray menu and palette menu | **Toolbar toggle** "Floating buttons" (tray keeps it) |
| Snip a template image… | Only used by the two image steps, but not reachable from them | **Snip…** button next to Browse in the image field of the step editor |
| Window layouts… | A read-only list of names | The Layout field in the step editor already lists them → **Delete** |
| About | — | Settings footer |
| Exit | Duplicates the tray menu and the window's close | Tray only (plus Settings footer when `minimizeToTray` is off) |

**After:**

```
[ ● Listening ]   [ Floating buttons ]   [ Simple | Advanced ]   [ ⚙ Settings ]
```

One button opens one place. The More menu and its 12 command ids go away.

**Settings dialog, regrouped** (same scroll pane, three headings):

1. **Startup** — Start when this computer starts · Start listening right away ·
   Closing the window keeps it running · Start hidden
2. **Keypads** — Emergency off switch [Record] · New keypads stop typing all
   their keys / only button keys *(radio, not two dependent checkboxes)* ·
   Image match strictness *(Advanced only)*
3. **Backup & restore** — Save a copy… · Load a copy… · Show my backups ·
   Reload from file *(Advanced)* · Import from macrokb *(Advanced)*

Footer: **Run setup again** · **About** · [ Done ]

The "block all keys" pair becomes one radio choice because the second checkbox
only means something when the first is ticked, and the current indented
"...and turn off all their normal input" wording reads as a separate setting.

---

## 6. The click recorder

### What's wrong with it today

1. It asks **"How many clicks do you want to record?"** up front. Nobody knows.
2. Each click opens a fresh modal (432×232) centred on the main window, often
   on top of the very spot to click.
3. After every click it asks **"Delay after click N, in milliseconds"** in a
   text box. Milliseconds is not a front-desk unit.
4. No list of what's been captured, no undo, no way to redo one spot. A wrong
   click means Esc and start over, or editing X/Y numbers by hand.
5. No double-click, which is how most PMS lists open a patient.
6. A right-click on Dental Macros's own window is accepted, then quietly
   dropped as a target window, so the clicks end up unlocked with a log line
   the user can't see in Simple mode.

### The new recorder

One floating panel for the whole recording. Always on top, does not take
focus (`WS_EX_TOPMOST | WS_EX_NOACTIVATE`), placed in the top-right corner of
the monitor the mouse is on. The main window hides while it's open.

```
┌ Recording clicks ─────────────────────────── ✕ ┐
│  1  Left-click as usual to get to the right    │
│     screen.                                    │
│  2  RIGHT-click each spot you want clicked.    │
│     Right-click twice quickly for a            │
│     double-click.                              │
│  3  Press Done.                                │
│ ───────────────────────────────────────────── │
│  ① Click   in Open Dental                  ✕   │
│  ② Double-click   in Open Dental          ✕   │
│  ③ Click   in Open Dental                  ✕   │
│  ▸ Waiting for click 4…                        │
│ ───────────────────────────────────────────── │
│  Speed:  ( ) Fast  (●) Normal  ( ) Slow        │
│  [ ↶ Undo last ]          [ Cancel ] [ Done ]  │
└────────────────────────────────────────────────┘
```

Behaviour:

- **No count up front.** Record until Done.
- **Numbered markers on screen.** Each captured spot gets a small numbered
  circle (a click-through layered topmost window, `WS_EX_TRANSPARENT |
  WS_EX_LAYERED`). Visual confirmation of exactly where each click will land.
  Markers disappear with Undo, ✕, Done or Cancel.
- **Undo last** removes the newest click and its marker. **✕** on a row
  removes that one. **Clicking a row** selects it and the status line becomes
  *"Right-click the new spot for click 2"*: the next right-click replaces that
  row instead of appending. That is the "step back" without starting over.
- **Double-click:** two right-ups within `GetDoubleClickTime()` and
  `SM_CXDOUBLECLK` of each other turn the last row into a double-click.
- **Speed** replaces per-click milliseconds: Fast 0.2 s, Normal 0.5 s, Slow
  1.5 s between clicks (same values as today's hint). Advanced users can still
  edit each Wait step afterwards.
- **Guard rails:** a right-click on any Dental Macros window (the panel, the
  palette) is ignored with *"That's Dental Macros. Right-click inside the
  program you want clicked."* A click in a *different* program from click 1
  is allowed but flagged: *"Click 3 is in Notepad, not Open Dental. That's
  fine if you meant it."*
- **Cancel** with clicks recorded asks *"Throw away 3 clicks?"* [Keep
  recording] [Throw away].
- **Done** builds the same steps as today (`window_prepare`, click, sleep, …,
  `window_release`), so the engine and saved files don't change.

Copy for the panel:

| State | Text |
|---|---|
| Waiting, none yet | ▸ *Waiting for your first right-click…* |
| Waiting | ▸ *Waiting for click 4…* |
| Replacing | ▸ *Right-click the new spot for click 2* |
| Captured | ✓ *Click 3 recorded* (flashes 1 s, then back to Waiting) |
| Own window | ⚠ *That's Dental Macros. Right-click inside the program you want clicked.* |
| No window under the point (desktop) | ⚠ *That spot isn't inside a program. Your clicks will go to that spot on the screen, wherever things are.* |
| Done, locked | ✓ *3 clicks recorded in Open Dental. We'll put its window back where it was each time, so the clicks line up.* |
| Done, target was maximised | add: *It was full-screen; it will be full-screen again afterwards.* |
| Hook failed | ✗ *Dental Macros couldn't watch the mouse. Close it, open it again, and try once more.* |

---

## 7. Plain-English errors

Today, errors reach the user in three ways: raw `e.what()` in a message box
(`Could not start listening`), the log (`Could not save settings: …`), and
`main.cpp`'s catch-all. In Simple mode the log is hidden, so every failure
needs a sentence and a next step.

One module, `friendly.h`, turns a problem into
`{ title, what happened, what to do, button, technical detail }`. The
technical text is never shown unless the user clicks **Show details** (and it
is copied with **Copy details** for support).

| Today's text (where) | Friendly title / body / button |
|---|---|
| `Could not read macros.json: <json error>` (startup) | **Your buttons couldn't be loaded** — *Your settings file is damaged. We've kept it safe and opened with a fresh start. Your automatic backup from \<date\> can bring your buttons back.* [ Restore from \<date\> ] [ Start fresh ] |
| `the file is there but Windows would not open it…` | **Your buttons couldn't be loaded** — *Another program has your settings open, often OneDrive or backup software. Wait a minute and press Try again.* [ Try again ] |
| `Could not save` + Win32 text (save) | Access denied → *Windows won't let Dental Macros save here. Your settings will be kept in your Windows profile instead.* (auto-switch, §8). Disk full → *Your disk is full. Free some space and your changes will save.* Sharing violation → retry silently 3× over 1 s before showing anything. |
| `could not install the keyboard hook: …` (Start) | **Dental Macros couldn't start** — *Your security software may be blocking it. Restart the computer and try again; if it keeps happening, ask whoever manages your security software to allow Dental Macros.* |
| `RegisterRawInputDevices failed: …` | Same as above. |
| `SendInput was blocked: …` (macro) | **Windows blocked a button** — *\<Program\> is running as administrator, so Windows won't let other programs type or click into it.* (+ check-7 advice) |
| `no open window with …` (window_prepare) | **\<Button\> needs \<Open Dental\>** — *Open \<Open Dental\> and press the button again.* |
| `could not run '<cmd>': …` | **\<Button\> couldn't open a program** — *We couldn't find \<name\>. It may have been moved or uninstalled.* [ Fix this button ] |
| image not found (`click_image`) | **\<Button\> couldn't find what to click** — *The picture it looks for isn't on screen. Make sure the right window is showing, then try again.* |
| Hotkey taken (log) | auto-fix, then status line: *Your off switch is now Ctrl + Alt + Shift + K (the old one is used by another program).* |
| `Dental Macros - unexpected error` (main.cpp) | **Something went wrong** — *Dental Macros needs to close. Your buttons are saved. Open it again to carry on.* [ Copy details ] |

Errors from a running macro (on the input thread) go to the status line, not a
modal, so a failed button never steals focus from the patient chart:
*⚠ "Insurance note" stopped at step 3: Open Dental isn't open.*

---

## 8. Self-healing defaults

- **Data folder.** Keep "next to the exe" as the first choice, so the installer
  (`%LOCALAPPDATA%\Programs\Dental Macros`, writable) and portable copies behave
  exactly as today. If the folder isn't writable (portable copy run from
  `Program Files`, a read-only share, a USB stick with protection on), use
  `%LOCALAPPDATA%\DentalMacros` instead, and remember the choice by the
  presence of the file there. `appPath()` becomes `dataPath()` for settings,
  backups, layouts and templates; icons shipped with the app stay on
  `appPath()`. `relativeToApp()` needs the same split, because template and
  icon paths are stored relative.
- **Missing settings fields** already fall back to defaults in `Config::load`;
  new fields (`uiMode`, `setupDone`) follow the same pattern.
- **Out-of-range values** (confidence, palette size) are already clamped on
  load. Add: an unparseable `toggleHotkey` is replaced by the first free
  default instead of being skipped with a log line.
- **A keypad that's gone.** Register Raw Input with `RIDEV_DEVNOTIFY` and handle
  `WM_INPUT_DEVICE_CHANGE`; show *"HP 430 Keypad isn't connected"* as a grey
  dot on its sidebar row, and green when it comes back. Today a sleeping
  Bluetooth keypad looks identical to a working one.
- **Settings the wizard sets:** `startEngineOnLaunch = true`,
  `setRunAtLogin(true)` (checkbox, default on), `miniDeckVisible = true` for the
  on-screen route, `blockAllKeys = true` for a dedicated keypad, `deck = "auto"`.

---

## 9. Code

### 9.1 `config.h`: two new settings

```cpp
struct Settings {
    // ... existing fields ...

    // Which main window layout to show. Not a permission: Simple mode hides
    // controls, it never changes or drops anything in the file.
    enum class UiMode { Simple, Advanced };
    UiMode uiMode = UiMode::Simple;

    // The first-run guide has been finished (or skipped) on this config.
    bool setupDone = false;
};
```

`Config::load`, inside `if (const js::Value* s = root.find("settings"))`:

```cpp
settings.uiMode = s->str("uiMode", "") == "simple"
                      ? Settings::UiMode::Simple
                      : Settings::UiMode::Advanced;
settings.setupDone = s->boolean("setupDone", true);
```

Both defaults are chosen for *existing* files: a `macros.json` written by an
older build has no `uiMode` and no `setupDone`, and its owner should land in
Advanced with no wizard. A brand-new install never reaches this block (the
file is missing), so it keeps the struct defaults, Simple and not set up.

`toJson()`:

```cpp
s["uiMode"] = settings.uiMode == Settings::UiMode::Simple ? "simple" : "advanced";
s["setupDone"] = settings.setupDone;
```

### 9.2 `util.cpp`: a data folder that heals itself

```cpp
namespace {
bool canWriteIn(const std::string& dir) {
    ensureDir(dir);
    const std::string probe = joinPath(dir, "~write-test.tmp");
    if (!writeFileAtomic(probe, "ok")) return false;
    deleteFile(probe);
    return true;
}

std::string profileDataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    return joinPath(narrow(buf), "DentalMacros");
}
}  // namespace

// Where settings, backups, layouts and snipped templates live. Next to the exe
// whenever possible, so installed and portable copies behave as they always
// have. Moves to the user's profile only when the exe folder refuses writes,
// and stays there afterwards: once a macros.json exists in the profile folder,
// that copy wins, so the decision survives the exe folder becoming writable.
const std::string& dataDir() {
    static const std::string dir = [] {
        const std::string profile = profileDataDir();
        if (!profile.empty() && fileExists(joinPath(profile, "macros.json")))
            return profile;
        if (canWriteIn(exeDir())) return exeDir();
        if (!profile.empty() && canWriteIn(profile)) return profile;
        return exeDir();   // check 1 reports the problem
    }();
    return dir;
}

std::string dataPath(const std::string& relative) {
    return joinPath(dataDir(), relative);
}

bool dataDirIsFallback() { return !iequals(dataDir(), exeDir()); }
```

Then `config_.load(appPath("macros.json"))` → `dataPath(...)`, and the same for
`backups`, `layoutsDir` and the snip output folder.

### 9.3 `setup_checks.h`: the checks behind Step 2

```cpp
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace setup {

enum class Status { Running, Ok, Note, Fixed, Problem };

struct CheckResult {
    std::string id;        // "save", "input", "hotkey", "tray", "app:<exe>"...
    Status status = Status::Running;
    std::string headline;  // "Your settings can be saved"
    std::string detail;    // one plain sentence, or ""
    std::string fixLabel;  // button text when there is something to click
    std::function<void()> fix;
    std::string technical; // for "Show details" only
};

// Every check that needs no user action. Fast (well under a second in total),
// so it runs on the UI thread when Step 2 opens; nothing here touches the
// network or waits on a device.
std::vector<CheckResult> runQuickChecks(HWND owner, cfg::Settings& settings);

}  // namespace setup
```

```cpp
// setup_checks.cpp
namespace setup {
namespace {

CheckResult checkSaving() {
    CheckResult r{"save"};
    if (canWriteIn(dataDir())) {
        r.status = dataDirIsFallback() ? Status::Fixed : Status::Ok;
        r.headline = "Your settings can be saved";
        if (dataDirIsFallback())
            r.detail = "They'll be kept in your Windows profile.";
        return r;
    }
    r.status = Status::Problem;
    r.headline = "Dental Macros can't save your settings";
    r.detail = "Windows won't let it save anything on this computer. Ask "
               "whoever looks after your computers to check it.";
    r.technical = "Not writable: " + exeDir() + " and " + dataDir();
    return r;
}

CheckResult checkInput() {
    CheckResult r{"input"};
    r.headline = "Dental Macros can type and click for you";
    try {
        dev::RawInputListener probe;
        probe.start([](const dev::KeyEvent&) {}, false);
        probe.stop();
    } catch (const std::exception& e) {
        r.status = Status::Problem;
        r.headline = "Windows didn't let Dental Macros listen for key presses";
        r.detail = "Restart the computer and open Dental Macros again.";
        r.technical = e.what();
        return r;
    }
    HHOOK h = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        [](int c, WPARAM w, LPARAM l) { return CallNextHookEx(nullptr, c, w, l); },
        GetModuleHandleW(nullptr), 0);
    if (!h) {
        r.status = Status::Problem;
        r.headline = "Your security software stopped Dental Macros";
        r.detail = "Ask whoever manages it to allow Dental Macros.";
        r.technical = "SetWindowsHookEx: " + lastErrorMessage();
        return r;
    }
    UnhookWindowsHookEx(h);
    r.status = Status::Ok;
    return r;
}

// Tries the saved shortcut, then a short list of rarely-used ones. Saves the
// first that Windows hands over, so the off switch printed on screen is always
// one that works.
CheckResult checkHotkey(HWND owner, cfg::Settings& s) {
    static const char* kFallbacks[] = {"ctrl+alt+shift+m", "ctrl+alt+shift+k",
                                       "ctrl+alt+shift+f12", "ctrl+alt+shift+pause"};
    CheckResult r{"hotkey"};
    std::vector<std::string> tries{s.toggleHotkey};
    for (const char* f : kFallbacks) tries.push_back(f);

    for (const std::string& combo : tries) {
        unsigned mods = 0;
        int vk = 0;
        std::string err;
        if (combo.empty() || !keys::parseHotkey(combo, mods, vk, &err)) continue;
        // Probe with a throwaway id; MainWindow registers the real one later.
        if (!RegisterHotKey(owner, 0xBEEF, mods | MOD_NOREPEAT, (UINT)vk)) continue;
        UnregisterHotKey(owner, 0xBEEF);
        const bool changed = combo != s.toggleHotkey;
        s.toggleHotkey = combo;
        r.status = changed ? Status::Fixed : Status::Ok;
        r.headline = "Emergency off switch: " + keys::prettyCombo(combo);
        r.detail = changed ? "The usual one is used by another program." : "";
        return r;
    }
    r.status = Status::Note;   // not fatal: the window and tray still stop it
    r.headline = "No emergency off switch";
    r.detail = "Other programs use all the usual shortcuts. You can still "
               "switch off from the Dental Macros icon by the clock.";
    return r;
}

CheckResult checkTray(HWND owner, cfg::Settings& s) {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = owner;
    nid.uID = 99;
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        return {"tray", Status::Ok};          // no row shown
    }
    s.minimizeToTray = false;   // never hide a window there is no way back to
    s.startMinimized = false;
    return {"tray", Status::Fixed};           // still no row; nothing to say
}

}  // namespace

std::vector<CheckResult> runQuickChecks(HWND owner, cfg::Settings& settings) {
    std::vector<CheckResult> out;
    out.push_back(checkSaving());
    out.push_back(checkInput());
    out.push_back(checkHotkey(owner, settings));
    out.push_back(checkTray(owner, settings));
    for (const auto& app : discover::dentalPrograms())   // §9.4
        out.push_back(discover::describe(app));
    return out;
}

}  // namespace setup
```

The keypad check is not in this list: it waits on a person. Step 2 runs it
with the same listener `AddKeyboardDialog::identify()` uses, and reuses
`cfg::chooseMatch` to build the match string.

```cpp
// In the wizard's Step 2, on a key-down from the listener:
void onKeypadKey(const dev::KeyEvent& ev) {
    auto all = dev::listKeyboards();
    const dev::InputDevice* hit = nullptr;
    for (const auto& d : all)
        if (d.path == ev.devicePath || dev::sameDevice(d.path, ev.devicePath)) {
            hit = &d;
            break;
        }
    if (!hit) return;   // a handle-less event; wait for the next press

    // With one keyboard device in the machine, the key can only have come
    // from the keyboard they type on. Taking it would leave them unable to
    // type, so ask for the keypad instead of accepting it.
    if (all.size() == 1) {
        setRow("keypad", Status::Note, "That looks like your main keyboard",
               "Press a key on the keypad you want to use for buttons "
               "instead. If it's wireless, make sure it's switched on.");
        return;
    }
    bool stable = true;
    keypad_.match = cfg::chooseMatch(*hit, all, &stable);
    keypad_.alias = config_.uniqueAlias(hit->name.empty() ? "Keypad" : hit->name);
    setRow("keypad", Status::Ok, "Found your keypad: " + keypad_.alias,
           "Its keys will run buttons and won't type.");
    listener_.stop();
}
```

### 9.4 `discover.h`: finding dental programs

Two sources, merged by executable name: what is installed (uninstall
registry) and what is open right now (`win::listWindows`). The open-window
source is the reliable one; the registry is a hint for programs that aren't
open yet.

```cpp
#pragma once
#include <string>
#include <vector>

namespace discover {

struct DentalProgram {
    std::string name;       // "Open Dental"
    std::string exe;        // "opendental.exe", or "" if only seen installed
    bool running = false;
    bool elevated = false;  // running as administrator (blocks our input)
};

std::vector<DentalProgram> dentalPrograms();

// A friendly name for a window's executable, used by the click recorder and
// the step list: "Open Dental" instead of "WindowsForms10.Window.8.app.0...".
std::string friendlyName(const std::string& exe, const std::string& title);

setup::CheckResult describe(const DentalProgram& p);

}  // namespace discover
```

```cpp
// discover.cpp
namespace discover {
namespace {

// Matched as a case-insensitive substring of the uninstall DisplayName and of
// the window title. Product names only, which are stable and public. The
// executable is learned from a running window, never guessed from this table.
// Verify each entry on a real office PC before relying on it.
const char* kKnown[] = {
    "Open Dental", "Dentrix", "Eaglesoft", "Easy Dental", "Curve Dental",
    "Denticon", "Dexis", "Carestream", "Romexis", "Sidexis", "Apteryx",
    "XrayVision", "Schick", "VixWin", "Practice-Web", "Dolphin",
};

void fromUninstallKey(HKEY root, const wchar_t* path,
                      std::vector<DentalProgram>& out) {
    HKEY key;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    wchar_t sub[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(key, i, sub, &len, nullptr, nullptr, nullptr,
                          nullptr) != ERROR_SUCCESS)
            break;
        wchar_t name[512];
        DWORD bytes = sizeof(name);
        if (RegGetValueW(key, sub, L"DisplayName", RRF_RT_REG_SZ, nullptr, name,
                         &bytes) != ERROR_SUCCESS)
            continue;
        const std::string display = narrow(name);
        for (const char* k : kKnown) {
            if (!icontains(display, k)) continue;
            // Suites install several entries (Dentrix has one per module), so
            // keep one row per product.
            const bool seen = std::any_of(out.begin(), out.end(),
                                          [&](auto& p) { return p.name == k; });
            if (!seen) out.push_back({k, "", false, false});
            break;
        }
    }
    RegCloseKey(key);
}

bool tokenElevated(HANDLE process) {
    HANDLE tok = nullptr;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool elevated =
        OpenProcessToken(process, TOKEN_QUERY, &tok) &&
        GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n) &&
        e.TokenIsElevated;
    if (tok) CloseHandle(tok);
    return elevated;
}

bool processElevated(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    // Can't even ask: an elevated process refuses this from a normal one on
    // some systems, so treat it as elevated. The advice is the same either way.
    if (!p) return true;
    const bool elevated = tokenElevated(p);
    CloseHandle(p);
    return elevated;
}

bool selfIsElevated() { return tokenElevated(GetCurrentProcess()); }

}  // namespace

std::vector<DentalProgram> dentalPrograms() {
    std::vector<DentalProgram> found;
    const wchar_t* kUninstall =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    fromUninstallKey(HKEY_LOCAL_MACHINE, kUninstall, found);
    fromUninstallKey(HKEY_LOCAL_MACHINE,
                     L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                     found);
    fromUninstallKey(HKEY_CURRENT_USER, kUninstall, found);

    for (const auto& w : win::listWindows()) {
        for (const char* k : kKnown) {
            if (!icontains(w.title, k)) continue;
            auto it = std::find_if(found.begin(), found.end(),
                                   [&](auto& p) { return p.name == k; });
            if (it == found.end()) it = found.insert(found.end(), {k});
            it->exe = w.exe;
            it->running = true;
            it->elevated = it->elevated || processElevated(w.hwnd);
        }
    }
    // Open ones first; the wizard shows at most three.
    std::stable_sort(found.begin(), found.end(),
                     [](auto& a, auto& b) { return a.running > b.running; });
    return found;
}

setup::CheckResult describe(const DentalProgram& p) {
    setup::CheckResult r{"app:" + p.name};
    if (p.running && p.elevated && !selfIsElevated()) {
        r.status = setup::Status::Note;
        r.headline = p.name + " is running as administrator";
        r.detail = "Windows won't let other programs type or click into it. "
                   "Ask whoever set up this computer to run " + p.name +
                   " normally, or right-click Dental Macros and choose "
                   "Run as administrator.";
        return r;
    }
    r.status = setup::Status::Ok;
    r.headline = "Found " + p.name;
    return r;
}

}  // namespace discover
```

Title matching is loose on purpose (a patient's name in a Notepad title could
contain "Schick"), which is why this only ever produces a "Found" row or a
warning, never a setting.

### 9.5 `friendly.h`: one place for error wording

```cpp
#pragma once
#include <string>
#include <windows.h>

namespace ui {

enum class Problem {
    ConfigDamaged, ConfigLocked, SaveDenied, SaveDiskFull,
    InputBlocked, TargetElevated, WindowMissing, ProgramMissing,
    ImageNotFound, Unexpected,
};

struct Friendly {
    std::string title;
    std::string body;        // what happened + what to do, 1-3 sentences
    std::string action;      // primary button text, "" = just OK
    std::string technical;   // shown only under "Show details"
};

// `subject` is the button, program or file the sentence is about, already in
// the user's words ("Insurance note", "Open Dental").
Friendly explain(Problem p, const std::string& subject = "",
                 const std::string& technical = "");

// Map a Win32 save failure to the right Problem. Sharing violations are not
// mapped: the caller retries those quietly first (OneDrive, antivirus).
Problem saveProblem(DWORD win32Error);

// Modal with [action] [OK] and a "Show details" link that reveals and copies
// `technical`. Returns true if the action button was pressed.
bool showFriendly(HWND parent, const Friendly& f);

}  // namespace ui
```

```cpp
Friendly explain(Problem p, const std::string& s, const std::string& tech) {
    switch (p) {
    case Problem::ConfigDamaged:
        return {"Your buttons couldn't be loaded",
                "Your settings file is damaged. We've kept it safe and opened "
                "with a fresh start. Your automatic backup can bring your "
                "buttons back.",
                "Restore from backup", tech};
    case Problem::ConfigLocked:
        return {"Your buttons couldn't be loaded",
                "Another program has your settings open, often OneDrive or "
                "backup software. Wait a minute and press Try again.",
                "Try again", tech};
    case Problem::SaveDenied:
        return {"Saving moved to your profile",
                "Windows won't let Dental Macros save in its own folder, so "
                "your settings are now kept in your Windows profile.",
                "", tech};
    case Problem::SaveDiskFull:
        return {"Your disk is full",
                "Free up some space and your changes will save.", "", tech};
    case Problem::InputBlocked:
        return {"Dental Macros couldn't start",
                "Your security software may be blocking it. Restart the "
                "computer and try again. If it keeps happening, ask whoever "
                "manages your security software to allow Dental Macros.",
                "", tech};
    case Problem::TargetElevated:
        return {"Windows blocked a button",
                s + " is running as administrator, so Windows won't let other "
                "programs type or click into it.",
                "", tech};
    case Problem::WindowMissing:
        return {"This button needs " + s,
                "Open " + s + " and press the button again.", "", tech};
    case Problem::ProgramMissing:
        return {"A program couldn't be found",
                "We couldn't find " + s + ". It may have been moved or "
                "uninstalled.",
                "Fix this button", tech};
    case Problem::ImageNotFound:
        return {"Couldn't find what to click",
                "The picture this button looks for isn't on screen. Make sure "
                "the right window is showing, then try again.",
                "", tech};
    case Problem::Unexpected:
    default:
        return {"Something went wrong",
                "Dental Macros needs to close. Your buttons are saved. Open it "
                "again to carry on.",
                "Copy details", tech};
    }
}
```

Errors from the engine are currently plain strings (`std::runtime_error`),
so the cheapest route is to give `eng::Engine` a typed error for the cases
above (a small `struct MacroError : std::runtime_error { Problem kind;
std::string subject; }` thrown at the existing throw sites in `actions.cpp`
and `engine.cpp`), and let everything else fall through to the log as now.

### 9.6 First-run wiring in `MainWindow::onCreate`

```cpp
config_.load(dataPath("macros.json"));
countPointers();
// ...engine callbacks as today...

const bool brandNew = config_.loadError.empty() && config_.keyboards.empty() &&
                      !config_.settings.setupDone;
if (brandNew) {
    // The wizard owns the first minute. It returns either a finished setup
    // (one deck, one button) or "skipped", which still counts as done so it
    // never reappears uninvited.
    setup::Outcome out = setup::runWizard(hwnd_, config_);
    config_.settings.setupDone = true;
    if (out.finished) {
        config_.keyboards.push_back(std::move(out.deck));
        config_.settings.startEngineOnLaunch = true;
        config_.settings.miniDeckVisible = out.onScreen;
        if (out.runAtLogin) setRunAtLogin(true, nullptr);
    }
    config_.dirty = true;
    saveConfig();
}
// ...the rest of onCreate as today; startEngineOnLaunch now starts listening
// and miniDeckVisible now shows the palette...
```

`paintWelcome()` stays as the empty state for someone who skipped the wizard
and then removed everything, with its button changed to **Set up a button**
(reopens the wizard) rather than **Scan for keyboards**.

### 9.7 Click recorder

Split the work so the part that decides which steps get written is a pure
function that `selftest.cpp` can cover, as the current recorded-click tests do.

```cpp
// dialogs.h
struct RecordedClick {
    ClickCapture at;
    bool isDouble = false;
};

enum class ClickSpeed { Fast, Normal, Slow };

// Shows the floating recorder. Returns false on Cancel or with nothing
// recorded. `existing` pre-fills the list when editing a recorded block.
bool recordClicksPanel(HWND owner, std::vector<RecordedClick>& clicks,
                       ClickSpeed& speed);

// What recordClicksPanel's result turns into. Pure: no windows, no input.
std::vector<cfg::Step> buildClickSteps(const std::vector<RecordedClick>& clicks,
                                       ClickSpeed speed);
```

```cpp
std::vector<cfg::Step> buildClickSteps(const std::vector<RecordedClick>& clicks,
                                       ClickSpeed speed) {
    std::vector<cfg::Step> steps;
    if (clicks.empty()) return steps;

    const double pause = speed == ClickSpeed::Fast   ? 0.2
                         : speed == ClickSpeed::Slow ? 1.5
                                                     : 0.5;
    // The window comes from the first click, as before: later clicks are
    // expected to be inside it, and the recorder has already warned if not.
    const ClickCapture& first = clicks.front().at;
    if (first.hasWindow) {
        cfg::Step prep{"window_prepare"};
        prep.params["class"] = first.cls;
        if (!first.exe.empty()) prep.params["exe"] = first.exe;
        prep.params["x"] = first.winX;
        prep.params["y"] = first.winY;
        prep.params["w"] = first.winW;
        prep.params["h"] = first.winH;
        steps.push_back(prep);
    }
    for (size_t i = 0; i < clicks.size(); ++i) {
        cfg::Step c{"click"};
        c.params["x"] = clicks[i].at.x;
        c.params["y"] = clicks[i].at.y;
        if (clicks[i].isDouble) c.params["double"] = true;
        steps.push_back(c);
        // No pause after the last click: release (or the next step) follows.
        if (i + 1 < clicks.size()) {
            cfg::Step w{"sleep"};
            w.params["seconds"] = pause;
            steps.push_back(w);
        }
    }
    if (first.hasWindow) steps.push_back(cfg::Step{"window_release"});
    return steps;
}
```

(Today's recorder also writes a sleep after the last click; dropping it makes
macros a little snappier. Keep it if any existing test depends on it.)

The panel reuses `ClickCaptureDialog`'s hook almost unchanged. Three
differences:

```cpp
// 1. The hook stays installed for the whole session instead of being one-shot,
//    and posts every right-up with its position and window.
if (wp == WM_RBUTTONUP && g_swallowRightUp) {
    g_swallowRightUp = false;
    if (auto* self = g_activeRecorder) {
        const auto* m = (MSLLHOOKSTRUCT*)lp;
        self->queue({m->pt, WindowFromPoint(m->pt), m->time});
        PostMessageW(self->hwnd(), WM_CLICK_ARRIVED, 0, 0);
    }
    return 1;
}

// 2. On WM_CLICK_ARRIVED, in the panel:
void onClick(const RawClick& rc) {
    ClickCapture cc = describeClick(rc.pt, rc.wnd);   // today's captureClick tail
    if (cc.ownWindow) {                               // new flag: our process
        status("That's Dental Macros. Right-click inside the program you "
               "want clicked.", Tone::Danger);
        return;
    }
    // Second right-up close in time and place: the previous click becomes a
    // double-click instead of a new row.
    if (!clicks_.empty() && replaceIndex_ < 0 &&
        rc.time - lastTime_ <= GetDoubleClickTime() &&
        std::abs(rc.pt.x - lastPt_.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
        std::abs(rc.pt.y - lastPt_.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2) {
        clicks_.back().isDouble = true;
    } else if (replaceIndex_ >= 0) {
        clicks_[(size_t)replaceIndex_].at = cc;      // "redo this spot"
        replaceIndex_ = -1;
    } else {
        clicks_.push_back({cc, false});
    }
    lastTime_ = rc.time;
    lastPt_ = rc.pt;
    markers_.sync(clicks_);   // numbered on-screen circles
    refreshList();
}

// 3. Undo / remove / select-to-replace act on clicks_ and call the same
//    markers_.sync + refreshList. Nothing is written to the config until Done.
```

The panel itself is a `Modal` subclass, with `build()` adding
`WS_EX_TOPMOST | WS_EX_NOACTIVATE` via `SetWindowLongPtrW(GWL_EXSTYLE)` and a
`SetWindowPos` to the top-right of `MonitorFromPoint(cursor)`. `Modal::run`
disables the parent, which is what we want; the caller hides the main window
and the palette before `run` and shows them after.

### 9.8 Toolbar after the menu cleanup

```cpp
// onCreate: "More" goes, two toggles arrive.
btnEngine_   = makeButton(hwnd_, "Start Listening", 0, 0, 132, 34, ID_BTN_ENGINE);
btnFloating_ = makeButton(hwnd_, "Floating buttons", 0, 0, 132, 34, ID_BTN_FLOATING);
btnMode_     = makeButton(hwnd_, "", 0, 0, 150, 34, ID_BTN_MODE);   // "Simple | Advanced"
btnSettings_ = makeButton(hwnd_, "Settings", 0, 0, 92, 34, ID_BTN_SETTINGS);
```

Removed command ids: `IDM_SAVE`, `IDM_LAYOUTS`, `IDM_BACKUP`, `ID_BTN_MENU`,
`IDM_MINIDECK` (the toolbar toggle replaces it; the tray keeps
`IDM_TRAY_MINI`). `IDM_SNIP` moves into `StepDialog` beside the File field's
Browse button for `click_image` and `wait_image`. `IDM_IMPORT`, `IDM_EXPORT`,
`IDM_RELOAD`, `IDM_OPEN_FOLDER`, `IDM_ABOUT` become buttons in
`SettingsDialog`, which returns a small enum for the ones the main window has
to carry out (reload, import) rather than doing them itself.

---

## 10. Copy deck: everything else

**Toolbar**

| Control | Label | Tooltip |
|---|---|---|
| Engine (off) | Start listening | *Your keypad's keys will run your buttons.* |
| Engine (on) | ● Listening — Stop (Ctrl+Alt+Shift+M) | *Press Ctrl + Alt + Shift + M anywhere to stop.* |
| Floating buttons | Floating buttons | *Show your buttons in a small window on top of everything.* |
| Mode | Simple \| Advanced | *Advanced shows every kind of step, pages, and the activity log.* |
| Settings | Settings | — |

**Sidebar (Simple)**

- Heading: **YOUR BUTTONS**
- Row badges: *on screen* · *keypad* · *not connected* (grey)
- Add rows: **+ Add a keypad** · **+ Add on-screen buttons**

**Inspector (Simple)**

- Empty selection: *Pick a button on the left to change what it does.*
- Name field placeholder: *Name on the button*
- Section: **What it does**
- Empty list: *Nothing yet. Add something below.*
- Buttons: **+ Type text** · **+ Press keys** · **+ Record clicks** · **+ Wait**
  · **Try it** · **Remove**
- Try it, while running: *Trying it… (3 steps)*

**Step sentences (Simple step list)**

| Step | Sentence |
|---|---|
| type | Type "Thank you for visiting…" |
| keys | Press Ctrl + S |
| recorded block | Click in Open Dental (3 clicks) |
| click (loose) | Click at a spot on the screen |
| sleep | Wait 1 second / Wait half a second |
| anything else | + 2 advanced steps — **Open in Advanced** |

**Add-step prompts**

- Type text: **What should it type?** — *Tip: press Enter for a new line.*
- Press keys: **Press the keys now** — *For example, hold Ctrl and press S.*
  (reuses `captureKey` combo mode)
- Wait: **How long?** — ( ) Half a second (●) 1 second ( ) 2 seconds ( ) 5
  seconds

**Status line (Simple mode, bottom of window)**

| Event | Text |
|---|---|
| Ready, not listening | Not listening. Press **Start listening** to use your keypad. |
| Listening | ● Listening to HP 430 Keypad |
| Button ran | ✓ "Thank you" ran |
| Button failed | ⚠ "Insurance note" stopped at step 3: Open Dental isn't open. |
| Saved (after an edit) | *(nothing: saving is silent)* |
| Keypad disconnected | ⚠ HP 430 Keypad isn't connected. Switch it on, or check Bluetooth. |
| Keypad back | ✓ HP 430 Keypad is connected again. |
| Off switch moved | Your off switch is now Ctrl + Alt + Shift + K. |

**Confirmations**

| Action | Title | Body | Buttons |
|---|---|---|---|
| Remove a button | Remove "Thank you"? | Its steps will be deleted. | Remove · Keep |
| Remove a keypad | Remove "HP 430 Keypad"? | Its 12 buttons will be deleted. The keypad will type normally again. | Remove · Keep |
| Cancel recording | Throw away 3 clicks? | — | Throw away · Keep recording |
| Close window (tray on), first time only | Dental Macros is still running | Your buttons keep working. Find it by the clock to open it again. | OK |

---

## 11. Build order

Each phase ships on its own and leaves the app working.

1. **Menu cleanup + Settings regroup** (§5). Smallest change, no new concepts.
   Removes `IDM_SAVE`, `IDM_LAYOUTS`, `IDM_BACKUP`, adds Snip to the step
   editor.
2. **Click recorder** (§6, §9.7). Biggest daily-use payoff for existing users
   too. Add `buildClickSteps` tests: single, double, replace, undo, no window,
   speed values.
3. **Friendly errors + status line** (§7, §9.5). Needed before Simple mode,
   which hides the log.
4. **Data folder fallback + device-change dots** (§8, §9.2). Test with the exe
   in a read-only folder.
5. **Simple mode** (§4). Test: open an existing config in Simple, change a name,
   confirm the saved file differs only in that name.
6. **Wizard + checks + discovery** (§1–3, §9.3, §9.4, §9.6). Last, because it
   leans on everything above. Test the keypad route with the J20 (consumer-only
   keys) and the Keydial, since those are the unusual ones.

## 12. Open questions

- **Dental program list.** The names in `kKnown` are product names, not
  verified window titles or uninstall entries. Worth checking on the office
  PC (or the second computer) before the "Found Open Dental" row ships.
- **Should Simple include "Open a program"?** It's the next most likely thing a
  front desk wants (open the imaging software). It would make five step types
  instead of four.
- **Recorded clicks and screen changes.** Recorded clicks are absolute screen
  positions. `window_prepare` fixes window moves, but not a change of display
  scaling or a different monitor arrangement. The recorder could store the
  DPI and monitor layout and warn on mismatch at run time. Not in this
  proposal.
- **Installer.** The installer's "Start now" already launches the app, so the
  wizard follows install directly. The "Run at sign-in" task in the installer
  and the wizard's checkbox overlap; the wizard could skip its checkbox when
  `runAtLoginEnabled()` is already true.
