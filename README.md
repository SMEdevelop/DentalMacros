# MacroDeck

A **Stream Deck–style macro app for Windows**, written in C++ with no runtime
dependencies. It turns a spare keyboard or numpad into a grid of macro keys
you configure by pointing and clicking.

The trick that makes it work: Windows' Raw Input API reports *which physical
keyboard* every keystroke came from. So MacroDeck can swallow every key on
your macro pad — firing macros instead of typing — while your main keyboard
keeps behaving completely normally.

This is a C++ fork of the Python `macrokb` tool in the `Macros` folder. It has
the same engine but a real graphical deck instead of a form-based editor, and
it drops the Python and OpenCV dependencies entirely.

![What the tiles and icons look like](docs/ui-preview.png)

---

## Before you start: Smart App Control

Smart App Control **may** refuse to run MacroDeck. Where it does, you'll see:

> This command cannot be run due to the error: An Application Control policy
> has blocked this file.

**The way to find out is to run it.** Build the app, launch
`build\MacroDeck.exe`, and see what happens. If the window opens, nothing here
applies to you and you can skip to the next section.

Don't use the registry to predict it:

```bash
powershell -c "(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy').VerifiedAndReputablePolicyState"
```

`0` = off, `1` = on and enforcing, `2` = evaluation mode. A `1` does **not**
mean you're blocked. This has been verified on a PC reporting `1` where
MacroDeck launches, runs and exits perfectly normally — and where a build
copied from a machine that *was* blocked runs equally well. The same
unmodified executable can be refused on one PC and fine on the next, so treat
this value as a clue about why a block happened, not as a forecast that one
will.

**The verdict is per-file, and it is a fresh roll of the dice for every
build.** On one PC reporting `1`, one build of MacroDeck was refused while
another build of the same source, minutes apart, ran perfectly. Copying the
refused file elsewhere did not help; rebuilding it did. So if a build is
blocked:

- **Just build again.** Every link stamps a new timestamp into the executable,
  so `build.bat` produces a different file and gets judged afresh. This is the
  cheapest thing to try and it often works.
- If you plan to hand this to anyone else, that unpredictability is the reason
  to sign it — see the options below.

If you are blocked, it isn't a bug and isn't a false positive to argue with.
Smart App Control blocks *unsigned* programs that install a low-level keyboard
hook — exactly what any macro tool, including this one, has to do to stop your
macro pad from typing into apps. A commercial tool avoids the block by paying
for a code-signing certificate and building up reputation with Microsoft. Your
options, least drastic first:

- **Try it on another PC.** Since the block travels with the machine and not
  the file, this costs nothing and often just works.
- **Sign the executable** with a code-signing certificate you own.
- **Keep using the Python `macrokb`** in the `Macros` folder instead. Scripts
  aren't subject to this check because `python.exe` is already signed and
  trusted, so the Python version runs on a machine where this one won't.
- **Turn Smart App Control off** — Windows Security → App & browser control →
  Smart App Control settings → Off. **This is a one-way door:** Windows will
  not let you switch it back on without a full reset of the PC. Weigh that
  before doing it; it is a real reduction in your machine's protection, and
  the three options above cost you nothing.

Everything except launching the app works regardless — the build and the full
test suite run fine.

---

## Building

You need the Microsoft C++ compiler (MSVC). If you don't have it:

```bash
winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

That is the compiler only — no Visual Studio IDE. `build.bat` finds it by
itself through `vswhere`, so there is no need to open a Developer Command
Prompt first:

```bash
build.bat
```

The result is `build\MacroDeck.exe`.

**It is not a single file any more.** As of beta 0.7 MacroDeck is built with
MSVC and links the C++ runtime dynamically (`/MD`), so the executable needs
three DLLs beside it:

| DLL | |
|---|---|
| `msvcp140.dll` | C++ standard library |
| `vcruntime140.dll` | compiler runtime |
| `vcruntime140_1.dll` | exception handling |

Running `build\MacroDeck.exe` straight out of `build\` works on the machine
that built it, because Visual Studio put those DLLs in `System32`. **That is
exactly why a broken package looks fine here and fails on someone else's PC.**
Do not hand anyone the bare .exe — use `package.bat`, which copies the three
DLLs in beside it. The zip and the installer it produces both carry them, and
need nothing preinstalled on the target machine beyond Windows 10/11 itself.

(The 1.0.0 release was built with GCC and `-static`, which did produce one
self-contained file. The trade is deliberate: MSVC matches the platform it
targets, and the packaged download is self-contained either way.)

Settings live next to the .exe.

To check everything works on your machine:

```bash
run_tests.bat
```

This runs 261 checks covering JSON, the config model, the step engine, key
tables, deck presets, window management, the image matcher, config recovery,
the recorded-click workflow, and importing a macrokb `macros.yaml`. The engine
tests only run steps with no side effects — nothing is typed, clicked or
launched, and the window tests drive a throwaway window of their own.

---

## Quick start

1. Plug in the keyboard or numpad you want to dedicate to macros.
2. Run `build\MacroDeck.exe` and click **Scan for keyboards**.
3. Pick your macro keyboard from the list and give it a name.
   - Not sure which entry is which? Click **Identify by pressing a key** and
     press a key on the pad — the matching entry highlights itself.
   - By default, whichever keyboard you choose stops typing into your apps
     while MacroDeck is listening — so pick the *spare* one. If you'd rather
     put macros on F13–F24 or extra keys of the keyboard you actually type on,
     see [Keeping the rest of the keyboard](#keeping-the-rest-of-the-keyboard)
     below; you can give up only the macro keys instead.
4. Choose a deck layout (top of the deck area). If you're using a numeric
   keypad, pick **Numeric keypad** and you get a keypad-shaped grid.
5. Click a key on the grid, then **Add step** — or drag an action straight
   from the left-hand list onto a key.
6. Click **Start Listening**.

"Listening" is the state where MacroDeck is watching your macro keyboard: its
keys fire macros, and — for any keyboard with *Block these keys from apps*
ticked — they stop typing into your apps. Everything else in the app works
whether or not it is listening; you can build and test macros with it off. The
line under the title always says which state you are in and what it means.

### Keeping the rest of the keyboard

Right-click a keyboard in the sidebar and you get two related options:

| Option | What it does |
|---|---|
| **Block these keys from apps** | Whether this keyboard gives up *anything*. Untick it and macro keys fire *and* type — useful for testing. |
| **Turn off all normal input, not just macro keys** | Only matters when the first is ticked. **Ticked** (the default): the whole keyboard goes silent — right for a dedicated macro pad. **Unticked**: only the keys that actually run a macro stop typing; every other key on that keyboard works normally. |

Untick the second one when your macro keys are F13–F24, extra keys, or
anything else on a keyboard you also type on. You keep the whole keyboard and
lose only the keys you deliberately bound.

A key with a tile but no steps yet is *not* treated as a macro key, so a
half-built binding never silently swallows a key.

The sidebar shows a small badge for anything other than the default: `pass`
when the keyboard gives up nothing, `some` when it gives up only its macro
keys, `click` for software buttons with no keyboard behind them. New keyboards
take their starting values from **Settings → Macro
keyboards**, and this option applies the moment you change it — unlike the
first one, it needs no stop-and-start.

Settings save themselves as you work, and the previous day's file is kept in
`backups\` the first time each run saves.

**Ctrl+Alt+Shift+M starts and stops listening from anywhere in Windows**, so a
pad that has stopped typing can always be handed back without finding the
window. The Stop button spells the shortcut out while it is listening. Change
or clear it under **Settings → Start / stop listening shortcut**.

### What blocking costs the keyboard you type on

Worth understanding before you put macros on your **main** keyboard.

Blocking is done with a Windows low-level keyboard hook, and that hook is
**global and synchronous**: every keystroke from every keyboard in the machine
goes through MacroDeck before the program you are typing into sees it. To
decide whether a key belongs to a macro pad, MacroDeck has to work out which
physical keyboard it came from, and that means waiting briefly for the
matching raw-input event. Measured here, a keystroke that cannot be matched
stalls for about **40ms** before being let through.

On a dedicated macro pad that costs nothing you would notice. On the keyboard
you type on all day it is a different story, and if enough keystrokes queue up
Windows eventually decides the hook is too slow and stops calling it — which
looks exactly like dropped keystrokes.

So MacroDeck asks a cheaper question first: *could any keyboard here possibly
block this key at all?* If the answer is no — and with a handful of macros on
a keyboard set to take only the macro keys, that is very nearly every key you
type — the keystroke goes straight through with no waiting and no lookup. The
answer is worked out whenever the configuration changes, so the hook does a
flat table read and nothing else.

Two cases must still be paid for, because there is no honest shortcut:

- keys that **actually run a macro** on a blocking keyboard, and
- **every** key on a keyboard set to *Turn off all normal input*, since the
  point is that the whole device goes silent, and only the device itself can
  tell you which keyboard a keystroke came from.

If your main keyboard still feels slow, the options in order of how little you
give up: untick **Turn off all normal input** so only bound keys are
inspected; move the macros to **software buttons** (below), which install no
hook at all; or untick **Block these keys from apps**, which means macro keys
fire *and* type.

### Buttons with no keyboard

**＋ Add software buttons** in the sidebar makes a deck with no keyboard
behind it. The buttons live on the deck and on the always-on-top palette, and
you fire them by clicking. There is no key to press, nothing is taken from
anything you type, and **no hook is installed** — so this cannot cost a
keystroke anywhere in Windows, however many buttons you make.

It is the right answer when you want macros but not at the price of touching
your typing, when the macro pad is not plugged in, or when there is no spare
keyboard to dedicate to the job.

They need no **Start Listening**: a click is not a keystroke, so there is
nothing to intercept. If software buttons are all you have configured,
MacroDeck does not register for raw keyboard input at all, and says so in the
activity log rather than claiming to be listening to something.

Everything else about them is an ordinary binding — label, icon, colour,
steps, Test run, and firing from the always-on-top palette.

---

## The interface

**Left sidebar** — your macro keyboards, and the palette of actions. Drag any
action onto a key to add it as a step.

**Middle — the deck.** One tile per key. Each tile shows its key name, an
icon, and the label you give it. Tiles light up green for a moment when their
key fires, so you can see at a glance that the pad is being read correctly.

- **Click** an empty tile to assign a key to it
- **Click** a configured tile to select and edit it
- **Double-click** to rename
- **Right-click** for icon, colour, test run, duplicating the key, copying and
  pasting its steps, and removal
- **Scroll** when a deck has more keys than fit

**Right — the inspector.** The label, icon and colour for the selected key,
plus its list of steps with add / edit / remove / reorder and **Test run**.

Click a step to select it; **double-click** it to open the editor. A selected
step stays selected while you edit, delete or move it, so a run of changes
does not need a re-click between each one.
**Record clicks...** builds a click sequence for you: say how many clicks,
then for each one **right-click** the spot on screen you mean (the small
status window stays out of the way) and say how long to wait afterwards. It
adds a `click` step per click and a `sleep` step for each delay above 0ms.

Right-click, not left, so that **left click keeps working normally while you
record**. You can open the tab, menu or dialog the recorded click needs to
land on, then right-click the target. The right click is swallowed, so no
context menu opens over the thing you are recording — which does mean you
can't open a context menu with the mouse mid-recording; use the **Menu** key
or **Shift+F10** if the spot you want is inside one.

It also **locks the sequence to the window you clicked first**. See
[Why recorded clicks keep working](#why-recorded-clicks-keep-working) — this
is what makes a recorded sequence still work tomorrow.

**Bottom — activity log.** Every macro that fires, and the exact reason for
anything that fails.

### Icons

**Icon...** in the inspector opens a library of 55 built-in icons — Office,
Media, Actions, Arrows and Practice — and **Browse for an image file...**
inside it is still there if you would rather use a picture of your own.

They are drawn in code rather than shipped as image files, for the same
reason the action glyphs are: nothing to install beside the executable,
nothing to lose when the folder is moved, identical on every machine, and
crisp at any size from a 30-pixel launcher button to a full-size deck tile.

A library icon is stored as `"icon": "lib:printer"`. Anything without the
`lib:` prefix is still read as a file path, so decks built before the library
existed keep working exactly as they did.

### Buttons on top of everything

The deck above is an editor. The other half is a launcher: a small window of
just the buttons that stays on top of whatever you are working in, so a macro
can be fired with the mouse when the pad is out of reach — or not plugged in
at all. Open it from **More → Buttons on top of everything**, or from the
notification-area icon when the main window is closed.

Each button is 38 pixels by default, smaller than a desktop icon, and the
whole thing is about the size of a large tooltip. **Drag the strip along the
top** to move it anywhere; there is no title bar. The **✕** at the right of
that strip puts it away.

- **Left-click** a button to run its macro. It works whether or not the
  engine is listening, because clicking one is not a keystroke that needs
  intercepting.
- **Right-click** anywhere on it for **button size** (small / medium /
  large), **buttons per row** (2 to 10), and a way back to the main window.
- **Hover** a button and its full label appears in the top strip. There is no
  room for a long label on a 38-pixel square, so it goes where it can be read.
- Buttons **flash green** when they run, whether you clicked them here or
  pressed the key on the pad.

Two deliberate details. It **never takes focus**: a macro that types or
clicks acts on whatever window is in front, so a palette that grabbed the
foreground on the way to firing one would send the whole macro to itself. And
it stays **out of the taskbar and Alt-Tab**, where a window this size has no
business appearing.

Where it sits, how big it is and whether it was open are remembered in
`macros.json` and restored next launch — including on a second monitor left
of the primary one, where the coordinates are negative. If that monitor is
gone, it comes back on one that still exists rather than off in the void
where a window with no title bar could not be dragged home.

It shows the keys of the **selected keyboard**. One keyboard's worth of
buttons is what fits in a window this size, and guessing between several pads
would be worse than following the one already on screen behind it.

### Deck layouts

| Layout | What it's for |
|---|---|
| Auto | Grows as you add keys. Good for any keyboard. |
| 3×2, 3×3, 4×3, 5×3, 8×4 | Fixed grids matching common macro pads. |
| Numeric keypad | Real keypad shape — the tall `+` and `Enter`, wide `0`. |
| Function keys F1–F12 | Two rows of six. |

The numpad and F-key layouts already know their key names, so clicking an
empty square binds that exact key with no key-pressing needed. On the blank
grids, clicking an empty square asks you to press the key you want there.

---

## Actions

| Action | Notes |
|---|---|
| Type text | Unicode, layout-independent. Optional per-character delay. |
| Press a hotkey | e.g. `ctrl+shift+t`. **Record** captures a combo you press. |
| Mouse click | At a position, or wherever the pointer is. |
| Move the pointer / Scroll the wheel | |
| Find an image and click it | Waits for the picture, then clicks it. |
| Wait for an image | Pauses until something appears on screen. |
| Run a program | Any program or shell command. |
| Open a file or link | Anything with a default app, including URLs. |
| Wait | Pause between steps. |
| Focus / Move / Minimise / Maximise a window | By title, program, window class, or any combination. |
| Lock a window in place for clicking / Release it | Put a window back where recorded clicks expect it, then undo that. |
| Save / Restore the window layout | Snapshot every window, put them back later. |

Steps run top to bottom. If one fails, the macro stops and the log says which
step and why — unless that step has **Keep going if this step fails** ticked,
in which case the failure is logged and the macro carries on.

Holding a macro key down runs its macro **once**, not once per keyboard
repeat.

### Image matching

**More → Snip a template image** dims the screen and lets you drag a box
around a button or icon. That PNG becomes a template that "Find an image and
click it" searches for.

Matching runs entirely on your machine using normalised cross-correlation over
an image pyramid — the same metric OpenCV's `TM_CCOEFF_NORMED` uses, but
implemented directly, so there is no OpenCV dependency. A full 2560×1440
search takes a few tens of milliseconds.

Tips:

- Snip something **distinctive** — the button itself, not the whole dialog. If
  you snip a blank area, MacroDeck refuses it and tells you why, rather than
  silently matching the wrong thing.
- Include detail in **both directions**. Matching ignores brightness and
  contrast, so a plain horizontal band — a toolbar edge, a divider line —
  matches equally well at *every* horizontal position, and the click can land
  anywhere along it. A patch with structure across *and* down pins the
  position exactly. Capturing the button's text or icon is usually enough.
- Avoid anything that changes: a clock, a caret, a progress bar.
- Templates are resolution-dependent. Re-snip if you change display scaling.
- If a match is missed, lower the confidence (0.75 is forgiving, 0.85 is the
  default, 0.95 is strict).

### Why recorded clicks keep working

A recorded click is an absolute screen position. That is only meaningful while
the window you were clicking is exactly where it was when you recorded — move
it, resize it, maximise it, and every click lands somewhere else. This is the
single reason click macros usually rot.

So when you record clicks, MacroDeck notes the **window** the first click
landed in — its class, its program, and its exact position and size — and
wraps the sequence in two extra steps:

| Step | What it does |
|---|---|
| **Lock a window in place for clicking** | Brings that window to the front, waits until it really is in front, remembers where it currently sits, un-maximises it if needed, moves it back to the size and position it had while you were recording, then pauses briefly so it can finish redrawing. |
| ...your clicks and waits... | |
| **Release the locked window** | Puts the window back exactly where it was — including re-maximising it if it was maximised — and gives your keyboard and mouse back. |

The window is matched by its **class**, which is fixed by whoever wrote the
program, rather than by its title, which changes with whatever document is
open. Recording also fills in the program name for good measure. Both are
ordinary fields you can edit or clear on the *Lock* step.

Nothing about this is magic, and all of it is visible: they are normal steps
in the step list. Delete them if you want the old behaviour of clicking at
raw screen positions, or add them by hand to a sequence you built yourself.

**Blocking input.** The *Lock* step can also block your keyboard and mouse for
the duration, so a stray nudge cannot derail the sequence. Windows only allows
a program to do that when it is **running as administrator** — otherwise the
request is refused. MacroDeck says so in the activity log rather than
pretending you are protected. Whatever happens, the block is always lifted
when the macro ends, including when a step fails part way through.

### Window layouts

A "Save the window layout" step records where every window is. "Restore a
window layout" puts them back — matching windows by program name and title
similarity, so it still works after the apps have been restarted.

---

## Coming from the Python macrokb

**More → Import from macrokb (macros.yaml)** reads your existing config:
keyboards, key bindings, every step, and your settings. Nothing is
overwritten — imported keyboards are added alongside anything already set up,
and the original `macros.yaml` is not modified.

What changed in the fork:

| | macrokb (Python) | MacroDeck (C++) |
|---|---|---|
| Needs installed | Python + 4 packages | nothing |
| Editor | Forms and lists | Deck of tiles you click and drag |
| Image matching | OpenCV | Built in |
| Config | `macros.yaml` | `macros.json` (same ideas, adds tile icons/colours/layout) |
| Runs in tray | no | yes |
| Blocked by Smart App Control | no | on some PCs — see the top of this file |

Both read keyboards the same way, so device ids carry over unchanged.

---

## Settings file

`macros.json`, next to the executable:

```json
{
  "settings": {
    "blockMacroKeyboards": true,
    "matchConfidence": 0.85,
    "layoutsDir": "layouts",
    "startEngineOnLaunch": false,
    "minimizeToTray": true,
    "startMinimized": false,
    "toggleHotkey": "ctrl+alt+shift+m",
    "miniDeckVisible": false,
    "miniDeckX": 1520,
    "miniDeckY": 820,
    "miniDeckTile": 38,
    "miniDeckCols": 5
  },
  "keyboards": [
    {
      "alias": "Numpad",
      "match": "VID_046D&PID_C53D",
      "block": true,
      "deck": "numpad",
      "bindings": [
        {
          "key": "num1",
          "title": "Open Slack",
          "color": "#3b82f6",
          "steps": [ { "do": "run", "cmd": "slack.exe" } ]
        }
      ]
    }
  ]
}
```

`matchConfidence` must be between 0.5 and 1.0; anything else is reported and
reset to 0.85. "Run at sign-in" is deliberately *not* here — it lives in the
Windows registry, so a copy of MacroDeck you move or delete can't leave a
stale setting behind.

Hand-editing is fine — comments and trailing commas are tolerated. Nothing you
put in this file can make MacroDeck misbehave: unknown key names, duplicate
keys and out-of-range numbers are each reported in the activity log and
skipped.

**Your file is never quietly destroyed.** If MacroDeck can't read
`macros.json` — damaged JSON, or the file locked by sync or backup software —
it says so, opens empty, and refuses to save until it has copied the original
to `macros.json.bak` first. If even that copy fails, the save is refused
outright. Fix or move the file and use **More → Reload macros.json**.

Separately, the first save of each run copies the previous settings to
`backups\macros-<date>.json`, keeping the ten most recent. **More → Export a
copy** writes one wherever you like.

`match` is a substring of the device path. Prefer the `VID_xxxx&PID_xxxx`
form: it comes from the keyboard's own firmware and survives reboots, driver
updates and USB port changes. MacroDeck only falls back to the longer
port-specific path when you have two identical keyboards attached, and tells
you when it does.

---

## Troubleshooting

**Nothing happens when I press a key on the macro pad.** Is MacroDeck
listening? The status under the title says. With it listening, press a key — if
the tile flashes green, MacroDeck sees it. If it doesn't, the wrong keyboard
is registered: right-click it in the sidebar → *Re-detect this keyboard*.

**Macros don't fire when an admin window is focused.** Windows won't deliver
hook events to a normal program from an elevated one. Run MacroDeck as
administrator if you need that.

**My macro keyboard still types into apps.** Blocking is per keyboard —
right-click it in the sidebar and check *Block these keys from apps*. Stop and
start listening afterwards.

**Only *some* keys on my macro keyboard type through.** That is
*Turn off all normal input, not just macro keys* being unticked, which is
exactly what it does: keys carrying a macro are taken, the rest keep typing.
Tick it to silence the whole keyboard. Note a tile with no steps counts as
"no macro", so finish the binding and the key stops typing.

**I lost my whole keyboard.** Press Ctrl+Alt+Shift+M — that stops listening
from anywhere in Windows and hands every key straight back. Then either pick a
different keyboard, or untick *Turn off all normal input, not just macro keys*
so only your bound keys are taken.

**A key occasionally types through anyway.** Blocking correlates the hook with
Raw Input inside a 30 ms window. In the rare case that correlation fails the
key is deliberately passed through — a key that types when it shouldn't is
much less annoying than one that silently vanishes.

**Macros can't trigger themselves.** Keys sent by MacroDeck are marked as
injected and are never blocked or re-matched.

**The start/stop shortcut does nothing.** Another program has claimed it —
the activity log says so at startup. Pick a different one in Settings.

**I closed the window and it vanished.** By default closing keeps MacroDeck
running in the notification area; click the tray icon to bring it back, or
turn that off in Settings. If Windows refuses to give it a tray icon,
MacroDeck exits instead of hiding with no way back.

---

## How the code is laid out

```
src/
  main.cpp        entry point, single-instance guard, GDI+ lifetime
  app.cpp         main window: deck grid, sidebar, inspector, tray
  minideck.cpp    the small always-on-top window of just the buttons
  iconlib.cpp     the built-in icon library: the table and the drawing
  iconpicker.cpp  the library laid out as a grid you click
  dialogs.cpp     keyboard picker, key capture, step editor, snip overlay
  ui_common.cpp   dark-themed controls and the modal host
  theme.cpp       colours, DPI scaling, hand-drawn icons
  engine.cpp      key routing and step execution
  device.cpp      Raw Input capture + the blocking hook   <- the core trick
  actions.cpp     SendInput typing, hotkeys, clicks, launching
  vision.cpp      screen capture + template matching
  winman.cpp      window find/move + layout save/restore
  config.cpp      document model and the action schema
  import_yaml.cpp macrokb macros.yaml importer
  keys.cpp        key name <-> virtual-key tables
  json.cpp        JSON parser/writer
  util.cpp        strings, paths, files
  app.ico         generated - see tools/make_icon.cpp
tests/
  selftest.cpp        261 checks over every non-UI subsystem
  render_preview.cpp  renders the icons and tiles to a PNG
tools/
  make_icon.cpp       exports src\app.ico from theme.cpp's drawAppIcon()
docs/
  ui-preview.png      generated by the tests; shown at the top of this file
  MultiClicker*.ahk   the AutoHotkey script click recording is modelled on
```

`src\app.ico` is committed, so an ordinary build never regenerates it. Run
`tools\make_icon.bat` after changing how the icon is drawn: it re-exports all
eight sizes (16 to 256) from the very same code the app draws with at runtime,
so the file and the running app can't disagree.

The action schema in `config.cpp` drives both the editor UI and the engine, so
adding an action means adding one entry there and one branch in
`Engine::runStep`.

Everything runs locally. There is no network code in this program.

---

## Giving it to someone else

```bash
package.bat
```

Builds the app, then writes two things to `dist\`:

| | For |
|---|---|
| `MacroDeck-<version>.zip` | Anyone who'd rather keep it portable. Contains the exe, the README, the licence and an `INSTALL.txt` written for a non-technical reader. |
| `MacroDeck-Setup-<version>.exe` | Everyone else. Per-user install to `%LOCALAPPDATA%\Programs\MacroDeck`, Start Menu shortcut, uninstaller, and a "Run MacroDeck when I sign in" tick box. |

The installer needs [Inno Setup](https://jrsoftware.org/isinfo.php)
(`winget install JRSoftware.InnoSetup`); without it `package.bat` still
produces the zip and says so.

The version comes from `FileVersion` in `src\app.rc` — that is the only place
to change it.

**Why a per-user install and not Program Files:** MacroDeck keeps
`macros.json`, `backups\`, `layouts\` and snipped images next to its own
executable, so it has to live somewhere the signed-in user can write.
Installing under `%LOCALAPPDATA%` also means no admin prompt. Uninstalling
removes the program and offers to remove your macros, defaulting to keeping
them.

### Signing

`package.bat` signs nothing unless you point it at a certificate, and warns
you when it hasn't. Set one of:

```bash
set MACRODECK_SIGN_PFX=C:\path\to\cert.pfx
set MACRODECK_SIGN_PASS=...

rem or, for a hardware token or a cloud signing service, a command that
rem takes the file to sign as its only argument:
set MACRODECK_SIGN_CMD=powershell -File packaging\sign.ps1
```

Both the exe and the installer get signed, and both are timestamped so the
signature outlives the certificate. A signing command that fails stops
packaging outright rather than quietly producing an unsigned release.

This matters more than the packaging does. Unsigned, everyone you send this to
meets SmartScreen's *"Windows protected your PC"* box and has to click
**More info → Run anyway**, and Smart App Control may refuse the file outright.
A macro tool that installs a low-level keyboard hook is also exactly the shape
antivirus heuristics flag.

**Getting a certificate.** Since 2023 code-signing keys have to live on
FIPS-validated hardware, so a traditional certificate means a physical USB
token in the post. [Azure Artifact Signing](https://azure.microsoft.com/en-us/products/artifact-signing)
(formerly Trusted Signing) avoids that — roughly $10/month, no token, and
there is an individual-developer path if you aren't signing as a registered
company. `packaging\sign.ps1` is ready to drive it: set
`TRUSTED_SIGNING_ENDPOINT`, `TRUSTED_SIGNING_ACCOUNT` and
`TRUSTED_SIGNING_PROFILE`, give it Azure credentials, and point
`MACRODECK_SIGN_CMD` at it. The script's own header has the full setup.

**Don't buy EV expecting instant trust.** Microsoft's
[SmartScreen documentation](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/smartscreen-reputation)
is explicit that EV certificates no longer grant reputation by default, and
that "paying a premium for EV solely to avoid SmartScreen warnings is no
longer justified." Signing of any kind does **not** silence SmartScreen on day
one; reputation accrues with download volume. What signing buys is a named
publisher, and reputation that carries from one release to the next instead of
resetting on every rebuild.

---

## Disclaimer and release of liability

**MacroDeck is provided "as is", with no warranty of any kind, and you use it
entirely at your own risk.** The governing text is in [LICENSE](LICENSE) —
sections 4 to 6 are the warranty disclaimer, the assumption of risk and
release of liability, and the limitation of liability. Those control; the
summary below is only a plain-language guide to them.

- **It types and clicks for you.** MacroDeck sends synthetic keystrokes and
  mouse input into whatever window happens to have focus, and runs whatever
  programs and commands you tell it to. A macro pointed at the wrong window,
  or fired by accident, can overwrite work, send messages, or delete things.
  Test new macros with **Test run** before binding them to a key.
- **It installs a system-wide keyboard hook.** While it is listening, keys on
  the keyboards you configure stop reaching your other programs. Choose a
  spare keyboard, and remember Ctrl+Alt+Shift+M hands it straight back.
- **You are responsible for what your macros do.** That includes anything
  automated on your behalf in other people's software or services.
- **Automation may be against the rules somewhere you use it.** Plenty of
  games, exam and interview platforms, and workplace systems prohibit input
  automation, and some treat it as grounds for a ban or disciplinary action.
  Check before you point this at anything that isn't yours.
- **The Author is not liable** for any claim, damages, data loss, or other
  liability arising from the software or its use — including anything that
  follows from a third party suspending or closing your accounts.

By installing or using MacroDeck you accept [LICENSE](LICENSE) in full.
Nothing here is legal advice; if you intend to sell or commercially
distribute this, have a lawyer review the terms first.

---

## Licence

**Proprietary. Copyright © 2026 SMEdevelop. All rights reserved.**

This is *not* open-source software. [LICENSE](LICENSE) grants you a personal,
revocable licence to install and run MacroDeck on devices you own or control,
and nothing further. Redistributing it, selling it, modifying it, or reverse
engineering it needs written permission.

The source being readable does not make it reusable — no part of this
repository may be copied into another project without permission.
