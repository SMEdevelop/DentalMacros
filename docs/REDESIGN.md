# Dental Macros redesign: SMEdevelop design system

Plan only. No code has changed yet.

Source of truth: the SMEdevelop design system
(https://claude.ai/artifact/2p6Zv7WYX4krV9DFV7QtQY): `project/README.md`,
`project/tokens.json`, `project/components/bundle.css` and each component's
README. Where this plan and the design system disagree, the design system
wins. The few places a native Windows window can't match it exactly are
listed under "Where Windows can't match it", each with the nearest match.

The current look goes completely: the dark slate theme, the bright blue
accent, 11–13px text, 26–34px controls, 6–10px corners and the dark title
bars.

---

## 1. What changes everywhere

### Colour

`ui::Theme` (theme.h) is replaced by the token set, named as in
`tokens.json`, so code reads `tok().surfaceRaised`, `tok().primary`,
`tok().inkMuted`. Every current use maps like this:

| Now | Becomes |
| --- | --- |
| `bg` (window) | `surface` #f7f3ec |
| `panel` (toolbar, sidebar, inspector, dialogs) | `surface-raised` #fffdf9 |
| `panelAlt`, quiet button fill | `surface-sunken` #efe8dc, hover `surface-hover` |
| `border`, `borderSoft` | `line` #e3d9c8 (decoration only); control edges `line-strong` #9a8a74 |
| `text` / `textDim` / `textFaint` | `ink` #3a3027 / `ink-muted` #6e5f4e (no third, fainter grey: it would fail 5:1) |
| `accent` | `primary` #3d6b8e, hover/pressed `primary-hover` |
| `accentDim` (selection) | `primary-tint` = `sky-100`, outline `primary`, text `primary-ink` |
| `good` | `success` #4f7a52, `success-tint` |
| `warn` | `warning` (edges, icons), `warning-tint`, `warning-ink` (text) |
| `danger` | `danger`, `danger-tint`, `danger-ink` |

- No dark mode. The title bar goes light: no more `DarkMode_Explorer`. On
  Windows 11 the caption is set to `surface-raised` and its text to `ink`.
- Shadows come only from the `shadow-*` tokens, drawn as a few
  soft offset passes in GDI+ (no gradients).
- **Macro key colours: only the seven `msg-*` colours.** The free colour picker
  (`chooseColorHex`, the Windows colour dialog) is removed. In its place is a
  row of seven swatches with names (Terracotta, Sage, Denim, Ochre, Plum,
  Spruce, Walnut) and a tick on the chosen one. Walnut is the default for a
  new key.
  - **Existing keys:** a saved colour that isn't one of the seven is
    *drawn* as the nearest of the seven. The file keeps the old value until
    someone picks a new colour, so nothing is lost and downgrading is safe.
  - Page colours follow the same rule.

### Type

One family, Segoe UI. `uiFont(px, bold)` is replaced by the named styles:

| Style | Size / line / weight | Used for |
| --- | --- | --- |
| `title` | 26 / 32 / 700 | Window and dialog headings ("Which keypad should run shortcuts?") |
| `tile-label` | 24 / 30 / 700 | Keypad choice tiles in setup and "Add a keypad" |
| MessageButton label | 22 / 28 / 700 | Macro key labels on the deck (bundle.css `.sme-msg`) |
| `button` | 18 / 24 / 600 (big: 20 / 26 / 700) | Every button |
| `body` | 17 / 26 / 400 | Lists, step rows, field values, status text, the activity log |
| `label` | 15 / 20 / 600 | Field labels, sub-lines, key badges' neighbours, timestamps |
| `overline` | 13 / 18 / 700, +0.05em, capitals | "STEP 1 OF 3", section kickers ("STARTUP") only |

Nothing a person has to read is smaller than 17px. Today's 9–13px text
(key badges, hints, palette categories, "scroll for more keys") moves up to
`label` or `body`.

### Spacing, shape, sizes

- Spacing only from `space-1…7` (4, 8, 12, 16, 24, 32, 48). The `kPad = 14`
  and the many odd gaps (6, 10, 14, 22…) are replaced.
- Corners: `radius-md` 16 for buttons, fields, keys and tiles; `radius-sm` 8
  for key badges and list rows; `radius-pill` for pills, toasts and step
  badges; `radius-lg` 24 for sheets (see the Windows limits below).
- Outlines are 2px. Fields and checkboxes use `line-strong`.
- **Tap sizes:** nothing clickable under 48px (`target-min`). The main action
  on setup and on prompts is 64px (`target-big`). This makes every control
  roughly 1.5× taller than now, so windows grow (see each screen).
- Keyboard focus: a 3px `focus-ring` outline with a 2px gap on every control,
  including owner-drawn buttons, checkboxes, lists and the deck tiles.

### Interaction

- Hover: filled buttons go one shade deeper; tiles and keys get a `primary`
  outline and `shadow-hover`.
- Press: shrink to 97% (96% on macro keys).
- When a key runs, its tile does the 96% press shrink. That replaces today's
  green flash, which counts as extra motion.
- The only other motion is a slow pulse on the "Listening" dot.

### Buttons: five kinds, one primary per screen

`ui::Tone` becomes the design system's kinds: Primary, Secondary, Quiet,
Success, Danger, plus a Big size. Each screen gets at most one Primary,
placed last (right, or at the bottom). Danger buttons are pale terracotta at
rest and solid on hover. Disabled buttons say why ("Pick a button first").

### Words

- Sentence case everywhere: "Start Listening" → "Start listening", "Test run"
  → "Try it".
- No "OK", "Yes", "No" or "Submit". Every one of the 11 `askYesNo` and 27
  `showMessage` calls is replaced by our own prompt with verb buttons:
  "Delete the 'Insurance note' button?" → [Keep it] [Delete button].
  The 4 raw `MessageBoxW` calls stay Windows message boxes, reworded: they
  run before the app's own windows exist, or after its window failed.
- Setup headings are questions. Errors say what to do next.
- Advanced mode loses its jargon ("HID", "match string", "Raw Input",
  "blocking") from the screen. Those details move behind "More details".
- No exclamation marks and no emoji. A ✓ tick is fine.

### Icons

Every icon is a line icon on a 24px grid with a 2px stroke and round caps
and joins, drawn in the current text colour. The few filled shapes in
`drawActionIcon` and `iconlib` (cursor arrows, the keyboard plate) are
redrawn as outlines. Icons are 22px in buttons and banners and 36px on macro
keys. An icon always sits next to a word. The icon-only "+" page tab and
padlock become pills with words.

---

## 2. Shared pieces built once (ui_common / theme)

Built and checked before any screen changes, so every screen uses the same
code:

1. **Tokens** (`tok()`), the **type styles** (`font(Style)`), the spacing and
   radius constants, and the shadow painter.
2. **Button**: one owner-draw painter for all five kinds and the Big size. It
   handles hover (tracked with `TrackMouseEvent`), press shrink, focus ring,
   disabled state, and an optional leading 22px icon.
3. **Pill**: 48px tall, `line-strong` edge; the active pill is `primary-tint`
   with a `primary` edge.
4. **TextField**: a borderless EDIT inside a painted 2px rounded frame, with
   the label above and the hint below. The error state shows a `danger` edge
   and the hint, rewritten as the fix, in `danger-ink`.
5. **Checkbox** row: 48px tall, a 24px `line-strong` box, and a `primary` box
   with a white tick when on.
6. **List rows**: 48px, `body` text, `radius-sm`. The selected row is
   `primary-tint` with a 2px `primary` edge, so the selection is shown by
   shape as well as colour. Lists lose their sunken 3D edge.
7. **Dropdown** (combo): 48px closed, 48px rows, `line-strong` frame and a
   chevron.
8. **Banner**: danger, warning or info, with a 22px icon and one sentence.
   Pinned at the top of the window until the problem clears.
9. **Toast**: a walnut pill, bottom centre, a few seconds, at most eight
   words, optional Undo. Only one shows at a time.
10. **StepBadge**: the "STEP 1 OF 3" capsule.
11. **Choice tile** (RoomTile): for picking one thing from a set. It has a
    round check, a sub-line, and selected / away states.
12. **Macro key** (MessageButton) painter: shared by the deck and the
    floating buttons.
13. **Sheet** (the `Modal` host): `surface-raised`, the `title` heading
    inside the window, `space-6` (32px) padding, and buttons at the bottom
    with the primary last. Built on it:
    - **Confirm**, replacing `askYesNo`: a question for the heading and two
      verb buttons, with a Danger button when something gets deleted.
    - **Notice**, replacing `showMessage`: a heading, one sentence and one
      button that names its result ("Close").

The self-test gains a section that checks the token values against
`tokens.json`, works out the contrast of every text/background pair actually
used (the design system promises 5:1 for `ink-muted`, 4.7:1 for white on
`msg-*`), and confirms that no button label is "OK", "Yes", "No" or Title
Case.

---

## 3. Screen by screen

### 3.1 Main window: top bar

Today there's a logo square, "Dental Macros", a status line under it, then
Start Listening · Show/Hide floating buttons · Simple|Advanced · Settings.

New, 80px tall, `surface-raised` with a `line` divider:

```
Dental Macros                       [Floating buttons] [Simple | Advanced] [Settings]
● Listening: 2 keypads          [ Stop listening ]
```

- "Dental Macros" in `title`. No logo square (the design system has no logo
  yet).
- Status under it in `body`, with a word and a dot, never the dot alone:
  "● Listening: My keypad", "Not listening: your keypad types normally".
  The "last key pressed" note moves into a Toast.
- **Start listening** is the window's one Primary, shown only while not
  listening. While listening it becomes a Quiet "Stop listening (Ctrl+Alt+
  Shift+M)" button.
- Three Pills on the right, the most allowed: "Floating buttons" (active
  while they're shown), a two-part "Simple | Advanced" pill, and "Settings".

### 3.2 Main window: banner row (new)

The status line in the activity area moves here as a Banner under the top
bar, shown only while something needs attention:

- Danger: "Couldn't save your buttons. Your changes are kept until it works
  again." with a fix button.
- Warning: "My keypad isn't connected. Its buttons won't work until it's
  back."
- Warning: "Not listening. Your buttons won't run until you start listening."
  (Only if listening was turned off by a problem, not by choice.)

One-off successes ("Saved", "Recorded 3 clicks", "'Insurance note' ran") are
Toasts instead.

### 3.3 Main window: left column (keypads)

Today: "YOUR BUTTONS"/"KEYBOARDS", 30px rows with a 7px dot and tiny badges,
blue "+ Add…" links, and (Advanced) a "DRAG ONTO A KEY" list of 26px rows.

New, 280px wide (now 214):

- Overline kicker "YOUR KEYPADS" (Simple: "YOUR BUTTONS").
- Each keypad is a compact choice tile, 64px: name in `button` weight, and a
  sub-line in `label`: "Connected", "Not connected" (dashed edge,
  `warning-ink` sub-line, the tile's away state), or "On screen" for
  on-screen buttons. The selected one is `primary-tint` with a check.
  "pass"/"some"/"mouse" badges become plain sub-lines in Advanced: "Types
  normally too", "Only its button keys are taken".
- Below the tiles: Secondary buttons "Add a keypad", "Add on-screen buttons",
  and (Advanced) "Add a mouse or trackpad".
- Advanced only, the "Drag onto a key" list: 40px rows with a 22px icon and a
  15px label, under `overline` category kickers. It scrolls as now.

### 3.4 Main window: deck (the keys)

Today: dark tiles with any colour, a 9px key badge, an 11px label, a dashed
"+" empty tile, a "no steps yet" note in 9px, 6px page tabs, a dot for
app-linked pages and a padlock glyph.

New:

- **Each key is a MessageButton:** a `msg-*` fill, white text, at least 112px
  tall, `radius-md`, `shadow-soft`, and a 36px line icon above a 22px bold
  label. The key badge ("Num 1") sits top left in a `radius-sm` capsule (white
  at 22%). A dial key shows a "Turn" tag top right instead of a bare glyph.
- **Empty slot:** a transparent tile with a 2px dashed `line-strong` edge and
  "+ Add a macro" in `ink-muted`.
- **Unfinished key:** a "No steps yet" tag in the URGENT tag's style (white
  pill, `danger-ink` text), instead of tiny yellow text.
- **Selected key:** a 3px `primary` outline with a 2px gap (the same as
  focus) and `shadow-hover`.
- Tiles keep their layout positions (numpad shapes stay numpad-shaped), with
  `space-4` (16px) gaps. Big layouts scroll; "Scroll for more keys ↓" is
  `label` size.
- Deck header: the keyboard dropdown (Advanced) is the new 48px dropdown. The
  hint line becomes `body` in `ink-muted`: "Click a key to change it.
  Right-click for more."
- **Pages:** a row of Pills. The current page is the active pill, with a
  small colour dot plus its name. An app-linked page adds the words "(follows
  OpenDental)" to its pill's tooltip, plus a line icon. "+ Add page" is a pill
  with words. The padlock becomes a pill with an icon and words: "Follows the
  program in front" / "Stays on this page".
- **No keypads yet:** the welcome card becomes a `surface-raised` sheet with
  `shadow-soft`. It has the `title` heading "Set up your first button", one
  `body` line, and a Big Primary "Get started".

### 3.5 Main window: right column (the selected key)

Today: "Key: Num 1" in 15px, a 12px sub-line, a name box, Icon/Colour buttons,
Press/Wheel buttons, the step list and a grid of 30px buttons.

New, 400px wide (now 344), `surface-raised`:

```
Num 1                                  (title)
Name on the button                     (label)
[ Insurance note                   ]   (TextField, 48px)
Colour                                 (label)
(●)(●)(✓)(●)(●)(●)(●)  Denim           (7 swatches, 48px, named)
[ Choose an icon ]                     (Quiet; Advanced only)
[ Press | Turn up | Turn down ]        (segmented pill; Advanced only)

[+ Record clicks] [+ Press keys]       (Secondary, 48px)
[+ Type text]     [+ Wait]
WHAT IT DOES                           (overline)
┌────────────────────────────────┐
│ ① Click in OpenDental (3)      │     (48px rows, body)
│ ② Type "Insurance verified…"   │
└────────────────────────────────┘
[ Try it ]           [ Remove step ]   (Quiet, Danger)
```

- Advanced keeps its own buttons, restyled and 40px (§6): "Add step", "Change",
  "Remove step" (Danger), "Move up", "Move down", "Try it", "Record clicks".
- Removing a step or a key asks first: "Remove the 'Type text' step?".
- With nothing selected: "Pick a button to change it" in `body`,
  `ink-muted`.

### 3.6 Main window: activity log (Advanced)

- The "Activity" heading becomes an overline kicker, "ACTIVITY".
- Log lines are 15px in `ink-muted`, on `surface-sunken` with
  `radius-md`. The area grows from 132px to about 160px.
- Simple mode has no log row at all, because its status now lives in the
  banner and toasts.

### 3.7 Window size

- Default 1280 × 820 design px (now 1240 wide), shrunk to fit the screen as
  today.
- Minimum about 1080 × 700 (now 940 × 600), because the three columns all
  grew.

### 3.8 First-run setup (SetupWizard)

Three pages, each with a StepBadge, a question in `title`, one `ink-muted`
line, and a Big Primary at the bottom, with a Quiet "Back" and "Skip setup".

1. **"How do you want to press your buttons?"** Two choice tiles: "On my
   screen" ("Click them with the mouse") and "On a spare keypad" ("Press a
   key; your main keyboard types normally"). Primary: "Next".
2. **"Which keypad should run shortcuts?"** (keypad) or **"Checking this
   computer"** (on-screen). The checks are a list of rows with a ✓ in
   `success`, or a `warning` icon plus the next step, never colour alone.
   "Press any key on the spare keypad now" is shown as an info Banner.
3. **"What should your first button do?"** Then a TextField "Name on the
   button", the four add buttons as Secondary, and the step list. Below them,
   two Checkbox rows: "Start with Windows" and "Show floating buttons".
   Primary: "Finish setup".

The window grows to about 720 × 640 (now 560 × 500).

### 3.9 Click recorder (ClickRecorderPanel and the on-screen markers)

The panel stays a small always-on-top window in a corner. It grows from
400 × 470 to about 440 × 560 and stays narrow.

- Heading `title` "Recording clicks". Then one `body` line: "Right-click
  where you want each click. Left-click works normally."
- The click list uses 48px rows, numbered with ① ② ③ in `primary-ink`.
- Buttons are Quiet "Undo last click" and "Remove", Secondary "Redo this
  spot", and the Primary "Save clicks" last.
- Speed becomes a three-part pill: "Fast | Normal | Slow".
- Cancelling with clicks recorded asks "Throw away 3 recorded clicks?"
  [Keep recording] [Throw away].
- **The on-screen markers** become 36px `primary` circles with a 2px
  `surface-raised` ring and white bold numbers. `shadow-soft` keeps them
  readable on any program.

### 3.10 Floating buttons (minideck)

- Each button is a small MessageButton: a `msg-*` fill, `radius-md` (scaled
  down), and a white icon or short label.
- **Size:** stays 30–46px, as today (decision 2 in §6).
- The grip and close button become Quiet. The close ✕ keeps a hidden "Close"
  label, the one icon-only control the design system allows.
- At these sizes there's room for the icon only. The button's name is
  in its tooltip, as today.

### 3.11 Settings (SettingsDialog)

- Heading `title` "Settings". Sections get overline kickers: "STARTUP",
  "KEYPADS", "BACKUP AND RESTORE", "ABOUT".
- Every option is a 48px Checkbox row with a `body` label and, where needed,
  a `label` hint in `ink-muted` under it.
- The "blocking" dropdown becomes a three-choice list, with plain words:
  "Take every key from the keypad", "Take only keys that have a button",
  "Let every key type normally".
- The off-switch shortcut field becomes a TextField with the hint "Press it
  anywhere to stop or start listening".
- Backup actions are Secondary buttons ("Save a copy", "Load a copy",
  "Reload from file"). "Run setup again" is Quiet.
- Footer: Quiet "Cancel", Primary "Save changes".

### 3.12 Step editor (StepDialog, Advanced)

- Heading: the action's name as a plain name ("Type text").
- The action picker becomes a 48px dropdown. Each parameter is a TextField
  with a visible label and an example hint.
- "Snip…" becomes a Secondary "Take a picture of the screen".
- Footer: Quiet "Cancel", Primary "Save step".

### 3.13 Smaller prompts

| Prompt | New look |
| --- | --- |
| Type text (askLongText) | Heading "What should it type?", a multi-line TextField, Primary "Add text" |
| Wait (chooseWait) | Heading "How long should it wait?", four big choice tiles (Half a second, 1 second, 2 seconds, 5 seconds), Primary "Add wait" |
| Press keys (key capture) | Heading "Press the keys you want it to press", the captured keys shown large in key badges, Primary "Add keys"; "Pick from a list" Quiet |
| Assign a key (key capture, keypad) | Heading "Press the key on your keypad", an info Banner while it waits |
| Add a keypad (AddKeyboardDialog) | Heading "Which keypad should run shortcuts?", each device a choice tile ("Bluetooth keypad", sub-line "Press a key on it to check"), Primary "Add keypad" |
| Choose an icon (IconPicker) | Heading "Choose an icon", a grid of 64px cells with a selected check, Primary "Use this icon" |
| Rename (askText) | TextField with label, Primary "Save name" |
| Problem explained (FriendlyDialog) | A danger or warning Banner at the top, a `title` heading and a `body` paragraph, then the Primary fix button. "Copy details" becomes a Quiet "More details" that shows and copies the technical text |
| Page follows a program (editPageAppRule) | Heading "Which program should bring up this page?", a choice list of open programs, Primary "Save" |
| Snip overlay | The screen dims with `overlay` (warm brown at 40%), and the selection box has a 2px `primary` edge with a size label in a Toast |

### 3.14 Right-click menus and the tray menu

There are 7 menus and 51 items. Each item gets reworded in sentence case,
starting with a verb ("Rename button…", "Delete button…").

They stay standard Windows menus, in Windows' light style. Only the wording
changes (decision 3 in §6).

### 3.15 Tray balloons and app icon

- Balloon wording follows the writing rules. Windows draws the balloons
  themselves, so they can't be styled.
- The app icon (`drawAppIcon`, `src/app.ico`) is redrawn in the new colours
  without changing the shape: a denim (`primary`) rounded square with four
  cream (`surface-raised`) keys.

---

## 4. Where Windows can't match it exactly

| Design system | Native limit | Nearest match |
| --- | --- | --- |
| Sheets have 24px corners (`radius-lg`) | A normal window with a title bar gets Windows 11's own rounded corners (about 8px) | Use Windows' rounding; the 24px corner is used on cards and sheets drawn inside the window |
| `shadow-sheet` behind dialogs | The window shadow is Windows' own | Leave Windows' shadow |
| Toasts float over the window | – | Painted inside the main window, bottom centre (no extra top-level window) |
| `:focus-visible` (ring only for keyboard) | Windows tracks this as "focus cues" | Honour `WM_UPDATEUISTATE`, so the ring shows after Tab, not after a click |
| Focus ring 2px *outside* the control | A child window can't paint outside itself | The ring is drawn at the control's own edge, then the 2px gap, with the control's shape inside. Text fields instead turn their edge `primary` |
| Tray balloons | Drawn by Windows | Wording only |
| 48px targets everywhere | Chosen exceptions (§6) | Advanced's own controls are 40px; floating buttons stay 30–46px |
| 48px menu rows | Standard Windows menus (§6) | Windows' light menus, reworded |

---

## 5. Order of work

Each phase builds, passes the self-test and is checked by screenshot
against the design system before the next starts.

1. **Foundation:** tokens, type styles, every shared piece in §2, light title
   bars, Confirm and Notice in place of every `askYesNo`, `showMessage` and
   `MessageBoxW`. Self-test section for tokens, contrast and wording.
2. **Main window:** top bar, banner row, toasts, left column, deck and
   pages, right column, log, welcome card, window size, the seven-colour
   swatches (and the nearest-colour mapping for old files).
3. **Setup, recorder, floating buttons:** setup wizard, click recorder and
   markers, floating buttons.
4. **Every other dialog:** settings, step editor, the prompts in §3.13 and
   the snip overlay.
5. **Menus, tray, icon, words:** menu wording, tray wording,
   app icon, the final pass over every string, README screenshots and text.

Expect it to touch most of `app.cpp`, `dialogs.cpp`, `ui_common.cpp`,
`theme.cpp`, `minideck.cpp` and `iconpicker.cpp`. Nothing in the engine,
config format (other than how colours are drawn) or actions changes.

---

## 6. Decisions (27 October 2026)

1. **Advanced mode is compact.** Advanced-only screens and controls use 40px
   rows and buttons and 15px text (the `label` size). That covers the drag
   list, the page strip, the step editor, the activity log and Advanced's
   own step buttons. Everything Simple mode shows, every prompt and setup
   keep the full 48px / 17px sizes. Colours, shapes, words and focus rings
   are the same in both.
2. **Floating buttons keep their 30–46px range.** They're the one place
   below `target-min`, by choice: they're a launcher you place yourself, and
   every button there also has a tooltip with its name.
3. **Standard Windows menus**, in their light style, with only the wording
   changed (§3.14, cheaper option).
4. **App icon recoloured** to denim and cream (§3.15).
5. **Drawn at 82% (28 October 2026).** After trying it, the full sizes read
   too big, so the whole app draws the design system at `kUiSize` = 0.82
   (src/theme.h): body text comes out at 14px, buttons at about 39px tall,
   which is the text size the app had before the redesign. Every size and
   gap shrinks together, so the look is unchanged. Exceptions: the main
   window still opens 1280 x 880, so the keypad gets the room; the floating
   buttons stay at their own 30–46px; key icons are about a third of the
   key again, as before the redesign, and key names are 14/12/11px.

## 7. Status

### Part 1, foundation: built (27 October 2026)

- `src/tokens.h/.cpp` (new): every colour, the seven key colours, type
  styles, spacing, corners and tap sizes, with no GDI+ so the self-test reads
  them. `nearestKeyColor()` maps any saved colour to the nearest of the seven
  by hue (greys to walnut).
- `theme.h/.cpp`: `tok()` holds the tokens for GDI+. The old `theme()` roles
  now point at tokens, so every screen not yet redone already uses the new
  colours. There are new painters for buttons (five kinds, Big), the focus
  ring, pills, banners, toasts, step badges, choice tiles, shadows and the
  24-grid line icons (`drawUiIcon`), plus `drawStyled` / `drawSpec` for text
  in the named styles. Keys draw in the nearest key colour. The app icon is
  recoloured, and `src/app.ico` regenerated.
- `ui_common.h/.cpp`:
  - Light title bars (`applyFrame`).
  - Controls take the colour behind them from `setSurface` /
    `surfaceBehind`.
  - Owner-drawn buttons track hover and show the focus ring after Tab.
  - Checkboxes are squares with a tick.
  - Text boxes and lists get a rounded 2px frame drawn in the non-client
    area. Dropdowns paint their closed box and chevron.
  - `confirm()` and `notice()` replace `askYesNo` and the old
    `showMessage`. `askText`'s button says "Save" rather than "OK".
- All 11 yes/no prompts now say what each button does. Delete prompts put
  the first focus, and Enter, on the button that keeps things.
- Self-test: a "SMEdevelop design system" section with 27 checks covering
  token values, contrast, key colour mapping and type sizes. 539 checks
  pass.
- Not yet checked on screen: the main window. The running copy locked the
  .exe, and a second copy won't start alongside it. The prompts, Settings
  and a sheet of every shared piece were checked by screenshot.

### Part 2, main window: built (28 October 2026)

- **Top bar** (80px): "Dental Macros" in the title style. Under it, a status
  line with a word and a shape: a filled dot while listening, an empty ring
  while not. "Start listening" is the one Primary button. While listening it
  turns into a Quiet "Stop listening (shortcut)". It is hidden when there's
  no keypad to listen to. To its right are three pills: "Floating buttons"
  (on while they're shown), a joined "Simple | Advanced" switch, and
  "Settings".
- **Banner and toasts:** the status line beside the activity log is gone.
  - Problems show as a Banner heading the middle column (Danger for saving
    or loading, Warning for a keypad or a button that stopped, Info for
    notices). The banner is tall enough for its sentence and has a Dismiss
    button.
  - Successes are a walnut or sage toast for 4 seconds.
  - A button that runs clears a "button stopped" banner, and only that one.
- **Left column** (296px): each keypad is a small choice tile with a round
  check and a sub-line in words ("Connected", "Not connected" with a dashed
  edge, "On screen"). The "Add" rows are Secondary buttons. Advanced's drag
  list has 40px rows with 22px icons under overline kickers.
- **Keys** are MessageButtons:
  - The seven colours, adius-md, shadow-soft.
  - A white-on-22% key badge, a "Turn" tag on dials, a 36px icon, and a
    bold white label sized to the key.
  - A "No steps yet" tag in the URGENT style.
  - Empty slots are dashed with a plus and the key's name.
  - The selected key gets the focus ring outside it.
  - A key that runs does the 96% press shrink instead of flashing green.
  - Fixed layouts keep keys at 104px or more and scroll, with a "Scroll for
    more keys" chip.
- **Pages** are pills, the current page the active one, with a colour dot
  and a window mark for program-linked pages. "Add page" and "Follow the
  program in front" are pills with words (Advanced).
- **Right column** (412px):
  - The key's name in the title style, then the name field (48px; 40px in
    Advanced).
  - Seven round swatches with a tick, reachable with Tab, and "Colour:
    Walnut" above them. These replace the colour dialog.
  - Simple's four Secondary "+" buttons above "What it does".
  - "Try it" (Quiet) and "Remove step" (Danger).
  - Advanced: "Choose an icon" and "Record clicks" share a row, then a
    joined "Press | Turn up | Turn down" switch. Below the list are 2 rows
    of 40px buttons: Add step, Change, Remove step, Move up, Move down, Try
    it.
  - With nothing picked, only the heading and one line show.
- **Activity log:** Advanced only, 132px, 15px text, under an "Activity"
  overline.
- **Welcome card:** a raised sheet with adius-lg, a heading, one line and
  a 64px Primary "Get started".
- **Window:** 1280 × 880 by default (fitted to the screen), minimum
  1080 × 700.
- **Page colours:** chooseColorHex is now a sheet of the seven swatches
  ("Pick a colour" → "Use this colour").
- **Shared pieces added:** joined pills (makePill, PillPart), swatches
  (makeSwatch), setListRowHeight, setComboHeight. Controls can carry
  their own background colour (setSurface on the control).
- Checked by screenshot in Simple, Advanced, on-screen buttons and the
  welcome card, using a separate test copy with sample buttons. Not yet
  seen: a toast on the real window, listening with a real keypad, and the
  floating buttons.

### Part 3, setup, recorder, floating buttons: built (28 October 2026)

- **Setup wizard** (720 × 720): three steps, one question each. Each step
  has a StepBadge, its question in the title style, one line of help, and
  a 64px Primary at the bottom, with Quiet "Back" and "Skip setup".
  1. "How do you want to press your buttons?" offers two choice tiles, "On
     my screen" and "On a spare keypad", each with a two-line sub-line.
  2. "Checking this computer", or "Which keypad should run shortcuts?" for
     a keypad. Each check is a row with a coloured icon beside the words
     that say it: a sage tick, a honey warning sign, a terracotta cross, or
     denim "info" while it waits for a keypad key. "Use on-screen buttons
     instead" is Secondary. While Next can't be pressed, it says why ("Press
     a keypad key first", "Fix the problem first").
  3. "What should your first button do?" offers two choice tiles, "Type
     something" and "Click through screens". These moved here from step 1,
     which had asked two questions. Below them come the text box or "Record
     my clicks" (Secondary), "Name on the button", the two 48px checkboxes,
     a tip, and "Finish setup".
- **Choice tile control** (makeChoice): the RoomTile as a real button that
  Tab can reach. Its sub-line wraps to three lines, and a long title keeps
  clear of the check.
- **Click recorder** (480 × 664):
  - "Recording clicks" heading, plain instructions in 17px, a list with
    48px rows, and a status line.
  - "Undo last", "Remove" (Danger) and "Redo this spot".
  - A "Pause between clicks" overline above a joined Fast | Normal | Slow
    switch.
  - Quiet "Cancel" and a big Primary "Save clicks" (it was "Done"). While
    there are no clicks it says "Right-click a spot first".
- **On-screen markers:** a denim ring on the spot and a 24px denim number
  badge, both with cream edges. The one being redone is honey-brown
  (warning-ink) instead. 52px marker windows.
- **Floating buttons:** sizes stay 30–46px (decision 2). Each button has a
  quarter-height radius, no border at rest, a 2px denim edge on hover, the
  96% press shrink instead of the green flash, and a dashed empty-slot look
  for one with no steps yet. The grip strip is 24px on surface-sunken with
  a six-dot handle, and its caption is 11px. The close ✕ is the line icon,
  with a round hover. The palette edge is line-strong.
- **Dialogs** now have WS_CLIPCHILDREN, so a dialog that repaints itself
  can't paint over its own controls. The wizard's badge and marks are
  painted through a bitmap. Drawn straight onto the window, its icons went
  missing.
- Checked by screenshot: every wizard step on both paths, the recorder panel
  as it opens, and the floating buttons. Not yet seen on screen: the markers
  and the recorder's list with clicks in it (that needs real right-clicks),
  and a keypad key pressed during setup.

### Part 4, every other dialog: built (28 October 2026)

- **Shared pieces** (ui_common):
  - Every sheet's footer is now the same: 64px buttons inside 32px of
    padding, Quiet "Cancel" on the left and the Primary action on the right
    (`layoutFooter`).
  - A TextField (`makeField`): a 15px label, a 48px box in 17px text, and a
    muted hint. `setFieldError` turns the edge red and puts what to do in
    place of the hint, so the step editor and Settings no longer pop up a
    message for a box that needs fixing.
  - A Banner control (`makeBanner`) that any dialog can hold.
  - Short choice tiles (under 100px) are list rows: the check on the left,
    the title and sub-line beside it. The wizard's tall tiles are unchanged.
- **Settings** (600 wide, scrolls): "Settings" heading; STARTUP, KEYPADS,
  BACKUP AND RESTORE and ABOUT overlines; 48px checkbox rows in 17px; the
  off switch as a TextField with a Secondary "Press keys"; the blocking
  dropdown is now three choice rows ("Take every key from the keypad",
  "Take only keys that have a button", "Let every key type normally");
  six 48px backup buttons, "Run setup again" Quiet; the About text is shown
  in the window instead of behind a button; "Save changes".
- **Step editor:** the action's name as the heading, a labelled 48px
  dropdown, the action's sentence under it, and every parameter a
  TextField. A picture field has "Take a picture of the screen" and "Choose
  a file" under it; a shortcut field has "Press keys" beside it. "Save
  step". A long step's window is kept on screen.
- **Key capture:** the first line of the prompt is the heading and the rest
  sits under it. An info Banner says it is waiting, and turns to a warning
  for left click or a key from the wrong keypad. "Or pick it from a list"
  is a 48px dropdown with a Secondary "Use this key".
  - Unlike the plan, there is no "Add keys" button: the window still closes
    as soon as the keys are pressed. A confirm step would stop Enter, Space
    and Tab from ever being recorded, because they would work the window.
- **Add a keypad:** "Which keypad should run shortcuts?", a warning Banner,
  each keypad a choice row, and it listens from the start: a key pressed on
  a keypad picks it. A keypad that shows up as several inputs is one row;
  two with the same name are numbered. "Look again" rescans. The main
  button reads "Pick a keypad first" until one is picked.
- **Smaller prompts:** "What should it type?" with "Add text" (or "Save
  text" when editing); "How long should it wait?" as four choice tiles and
  "Add wait"; the one-line prompt is a heading and a labelled TextField
  ("Save name", "Add page", "Add buttons"...); "Choose an icon" is an 11 ×
  5 grid of 72px cells with a ticked selection and "No icon", "Use a
  picture file", "Cancel", "Use this icon" along the bottom.
- **Problem explained:** a danger Banner with what went wrong, a paragraph,
  the fix button, and "More details", which shows the technical text in the
  window and copies it.
- **Page follows a program:** the popup menu is now a sheet, "Which program
  should bring up this page?", with "No program" and each open program as a
  choice row.
- **Snip overlay:** the screen dims under the warm overlay token, the box
  has a 2px primary edge just outside it, and the help line and the box's
  size are Toasts.
- Checked by screenshot: Settings (top, bottom, and an off-switch error),
  the step editor for three actions and with a box left empty, both kinds
  of key capture, add a keypad (with real devices), the text, wait, rename,
  icon and program prompts, and the problem sheet. Not yet seen on screen:
  the snip overlay (it covers the whole screen), "More details" opened
  (it writes to the clipboard), and a key press picking a keypad.

### Part 5, words and docs: built (28 October 2026)

- **Menus** stay standard Windows menus (decision 3), reworded in sentence
  case with a verb first and "..." on every item that opens a window or
  asks first:
  - Page: "Rename page...", "Bring up this page for a program...", "Stop
    following the program", "Change page colour...", "Remove page colour",
    "Duplicate page", "Move page left / right", "Delete page...".
  - Keypad: "Rename keypad..." ("Rename these buttons..." for on-screen
    ones), the two blocking options as "Stop its button keys from typing"
    and "Stop every key from typing" (now hidden for on-screen buttons,
    where they meant nothing), "Find this keypad again...", "Delete
    keypad...".
  - Button: "Add a button here...", "Rename button...", "Change icon...",
    "Remove icon", "Change colour...", "Use the standard colour", "Try it"
    in both modes, "Copy to another key...", "Copy steps", "Paste 3 steps"
    (a real plural), "Remove all steps...", "Delete button...".
  - Tray: "Show floating buttons", "Exit Dental Macros". Floating buttons:
    "Hide floating buttons".
- **Tray balloon:** there is one ("A button stopped" and why), already in
  plain words.
- **Words:** the step editor's labels and hints lose "(ms)", "offset",
  "combo", "macro" and "URL" ("Pause between letters, in milliseconds",
  "Keys to press", "Move the click across (X)", "Stop here if it isn't
  found", "web link"). The engine's message when there is nothing to listen
  to, the default names for a new keypad ("Keypad", "Mouse"), the "last key"
  line and the activity log say keypad and button instead of keyboard,
  deck and macro. Action names are unchanged.
- **README:** a paragraph on the look, two new screenshots
  (`docs/screenshot-main.png`, `docs/screenshot-setup.png`), and every
  button, menu and Settings name brought up to date (setup's first step,
  Add a keypad, Try it, Save clicks, Choose an icon, Take a picture of the
  screen, Follow the program in front, the tiles' press instead of the green
  flash). The code map lists tokens.cpp.
- Not checked on screen: the menus themselves (only their words changed).

