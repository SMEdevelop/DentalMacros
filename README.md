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

You need a C++17 compiler. If you don't have one:

```bash
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

That installs a self-contained GCC toolchain (no admin, no Visual Studio).
Then:

```bash
build.bat
```

The result is `build\MacroDeck.exe` — a genuinely single executable. It is
linked statically (`-static`), so it depends on nothing but Windows itself:
copy it, or the whole `build` folder, to any Windows 10/11 PC and it runs.
Settings live next to the .exe.

To check everything works on your machine:

```bash
run_tests.bat
```

This runs 187 checks covering JSON, the config model, the step engine, key
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
   - ⚠️ Pick the *spare* keyboard. Whichever you choose will stop typing into
     your apps while MacroDeck is listening.
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

Settings save themselves as you work, and the previous day's file is kept in
`backups\` the first time each run saves.

**Ctrl+Alt+Shift+M starts and stops listening from anywhere in Windows**, so a
pad that has stopped typing can always be handed back without finding the
window. The Stop button spells the shortcut out while it is listening. Change
or clear it under **Settings → Start / stop listening shortcut**.

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
**Record clicks...** builds a click sequence for you: say how many clicks,
then for each one click the spot on screen you mean (the small status window
stays out of the way) and say how long to wait afterwards. It adds a `click`
step per click and a `sleep` step for each delay above 0ms.

It also **locks the sequence to the window you clicked first**. See
[Why recorded clicks keep working](#why-recorded-clicks-keep-working) — this
is what makes a recorded sequence still work tomorrow.

**Bottom — activity log.** Every macro that fires, and the exact reason for
anything that fails.

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
    "toggleHotkey": "ctrl+alt+shift+m"
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
  selftest.cpp        187 checks over every non-UI subsystem
  render_preview.cpp  renders the icons and tiles to a PNG
tools/
  make_icon.cpp       exports src\app.ico from theme.cpp's drawAppIcon()
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
set MACRODECK_SIGN_CMD=powershell -File C:\path\to\sign.ps1
```

This matters more than the packaging does. Unsigned, everyone you send this to
meets SmartScreen's *"Windows protected your PC"* box and has to click
**More info → Run anyway**, and Smart App Control may refuse the file outright.
A macro tool that installs a low-level keyboard hook is also exactly the shape
antivirus heuristics flag. Since 2023 code-signing keys have to live on
FIPS-validated hardware, so a traditional certificate means a physical token;
cloud services such as Azure Trusted Signing avoid that and cost far less than
the old certificates did.

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
