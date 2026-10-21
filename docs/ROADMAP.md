# Feature roadmap: pages, per-app decks, and what the hardware pads do

Dental Macros already has the hard part — a deck of tiles, a step engine, and
software buttons that cost no keystroke. What it does not have is the thing
every physical macro pad sells itself on: **the buttons mean something
different depending on what you are doing.**

This document is the result of looking at what those devices actually do, and
what of it is worth building here.

---

## What the reference devices do

Four products, one idea each worth stealing.

**Work Louder Creator Micro 2 / Framer Micro** (`worklouder.cc`). Thirteen
keys, and **six layers** you cycle with a capacitive touch strip — so those
thirteen keys are really seventy-eight. Their configurator ("Input") has
**AppSense**: it watches which application has focus and switches to that
app's preset by itself. The RGB changes colour with the app, so you can see
which layer you are on without looking away from your work. The joystick opens
an on-screen **radial menu** of seven slots, and there is a rotary encoder
mapped *per layer*.

**Elgato Stream Deck.** **Profiles** (a whole layout per app), **Smart
Profiles** (that layout switches itself when the app takes focus), **pages**
within a profile, and **folders** — a key that opens a second layer of keys
with a back button, like an app folder on a phone. Multi-Actions are what
Dental Macros already calls steps. Worth noting: keys can have **two states** with
their own icon and label, so one key is mute *and* unmute, and shows you
which.

**Steam Input** (the Steam Deck's binding system, and the most thought-out of
the four). Per-game configs, and two ideas beyond them. **Action set layers**
overlay the base layout instead of replacing it — a layer only says what is
*different*, everything else falls through. And **activators**: one physical
button carries three separate bindings at once — regular press, double press,
and long press — without them colliding.

**Loupedeck / TourBox / Flexbar.** Dials. Continuous parameter control — brush
size, timeline scrub, volume — is the one thing a grid of buttons is genuinely
bad at, and all three sell hardware to fix it.

---

## The two features asked for — built

Both are in as of this writing; the sections below are what was built and why,
and [the README](../README.md#pages) is how to use it. The rest of the list
further down has not been started.

### Pages

A keyboard grows a list of pages. Each page is a full set of bindings for the
same keys.

```
Keyboard
  alias, match, block, softwareOnly, deck    (unchanged)
  pages[]
    id, name, icon, color
    appRule        (optional - see below)
    bindings[]     <- what used to live directly on the keyboard
```

**Why it goes here and not on the binding.** `Keyboard::findKey()` resolves
exactly one binding per key name, and the loader actively rejects duplicates
("only one macro per key is kept"). Pages are precisely the case where one key
legitimately has several bindings, so the page has to sit *above* that lookup,
not inside it. `findKey` becomes `page->findKey`, and the keyboard gains an
active-page index.

**File compatibility.** A file with `bindings` and no `pages` loads as a single
page called "Page 1" — every existing `macros.json` keeps working untouched.
Going the other way, a keyboard with exactly one page is *written* as plain
`bindings`, so a config only grows the new shape once you actually use it, and
a build from before this change can still read it.

**In the UI.** A page strip above the deck: `‹ Design ▾ ›` plus a `+`.
Right-click a page to rename, recolour, duplicate or delete it. The
always-on-top palette gets the same thing, shrunk to page dots in the grip
strip — it already takes a caption there, so this is mostly paint code.

**On the pad itself.** Two new actions, so a physical key can move between
pages the way the touch strip does on the Micro:

| Action | |
|---|---|
| Go to page | Switch to a page by name, or next/previous. |
| Hold for page | Active only while the key is held; back on release. |

### Per-app decks

Each page may carry a rule about when it should be in front:

```json
"appRule": { "exe": "photoshop.exe", "class": "", "title": "" }
```

Those three fields are exactly what `win::WindowInfo` already reports and what
`win::findWindow` already matches on — same semantics, same reasons (class is
stable, title is not). Picking an app means picking from a list of running
windows, not typing a path.

A `SetWinEventHook` on `EVENT_SYSTEM_FOREGROUND` says when the front window
changes. We describe it, find the first page whose rule matches, and make that
page active. Nothing matches → the **default page**, which is whichever page
has no rule.

Three details decide whether this is pleasant or infuriating:

- **Manual choice wins, until the app changes.** Click a page yourself and
  auto-switching stands down; the next time you move to a different
  application it resumes. This is what Stream Deck does and it is right: the
  alternative yanks the page out from under you the moment you touch it.
- **A pin.** A padlock in the page strip suspends app switching entirely until
  you click it again.
- **Dental Macros's own window is never an app change.** Coming here to look at a
  page must not be what moves it. The rule is a process-id check, deliberately
  narrower than Stream Deck's (which switches smart profiles off entirely
  while its window is open) — that would make the feature impossible to try
  out while you are setting it up.

The palette can tint itself with the active page's colour, which is the useful
half of Work Louder's RGB cue — you get told which layer you are on without
looking away from what you are doing.

---

## The rest, ranked

Roughly by value per hour of work, with where the work lands.

| | Feature | Where |
|---|---|---|
| 1 | **Two-state buttons.** One tile, state A and B, each with its own label, icon, colour and steps; pressing alternates. Mute/unmute, record/stop, VPN on/off. Stream Deck's most-used feature after profiles. | `config` + inspector + `engine` |
| 2 | **Long press / double press.** One key, up to three macros: tap, double-tap, hold. Steam Input's activators, and free real estate on a 12-key pad. Needs a press timer in `Engine::onKey`, which already tracks key-down times for auto-repeat. | `engine` |
| 3 | **Folders.** A tile that opens a page and shows a back arrow. Almost entirely free once pages exist — a page, plus a "go to page" action, plus a return button. | `app` |
| 4 | ~~**Scroll wheel as a dial.**~~ **Built.** Every tile carries press, wheel-up and wheel-down; scrolling over it on the deck or the palette turns it. Shipped alongside mouse and trackpad support, which was the other half of the same idea. | `minideck` |
| 5 | **Preset pages.** Ready-made pages for Photoshop, Premiere, VS Code, Excel, Explorer and Chrome, each already app-bound. This is what makes per-app decks worth having on day one rather than after an evening of typing in hotkeys. | data + `config` |
| 6 | **Export / import a page.** One page to a small file, so a setup can be shared or moved between machines. `saveAs` already writes JSON; this is a subset of it. | `config` |
| 7 | **Radial menu.** A hotkey pops a ring of up to eight wedges at the pointer; flick and release to fire. Work Louder's joystick menu, driven by the mouse. The only one here needing real new drawing code, and the most distinctive. | new `radial.cpp` |
| 8 | **Layer overlay.** A page marked "inherit" overrides only the keys it fills; the rest fall through to the default page. Steam Input's action set layers, and it makes an app page a five-key diff instead of a full copy. | `config` + `engine` |
| 9 | **Live button labels.** Label from a command's output, refreshed every N seconds — clock, CPU, battery, now playing. | `engine` + `app` |
| 10 | **Chords.** Two keys pressed together as a binding of their own. | `engine` |

**Also built, and not on the original list:** mice and trackpads as macro
devices. A spare mouse is a deck like any other, because a mouse button
already has a virtual key no keyboard sends — so the only thing that had to
learn about pointers was `device.cpp`. Multi-finger trackpad gestures remain
out: they arrive through a separate HID interface and Windows eats most of the
standard ones first.

**Deliberately not on this list:** a phone app, or anything that talks to a
plugin marketplace. "Everything runs locally. There is no network code in this
program" is a promise worth more than either.

---

## Build order

Pages first, because per-app switching, folders and layer overlays are all
just a rule about *which page is in front*, and none of them can be built
without it. Then per-app rules — both now done. Next are two-state buttons and
long press, which are independent of both and each worth an evening.

### What the page work actually touched

| | |
|---|---|
| `config.h/.cpp` | `Page` and `AppRule`, `pages[]` on `Keyboard`, both file shapes, and blocking widened to ask every page |
| `engine.h/.cpp` | `page_goto` and `page_hold`, the page-change callback, and key-up handling so a held page is put back |
| `app.cpp` | the page strip, its context menu, the padlock, and the `EVENT_SYSTEM_FOREGROUND` hook |
| `minideck.h/.cpp` | the page bar under the grip strip |
| `theme.cpp` | glyphs for the two new actions |
| `selftest.cpp` | 40 checks: rule matching, page selection, both file shapes, blocking across pages |

The one thing worth remembering about the shape of it: **pages sit above the
key lookup, not inside it.** `findKey` resolves one binding per key and the
loader refuses duplicates — so the level that allows one key to mean several
things has to be the level above the one that insists it means exactly one.
