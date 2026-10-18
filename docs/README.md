# docs

Reference material. Nothing here is compiled or shipped with the application.

| File | What it is |
|---|---|
| `ui-preview.png` | The tiles and action icons, rendered by `tests/render_preview.cpp`. `run_tests.bat` refreshes it. The main README shows it. |

## Where click recording came from

A recorded click is an absolute screen coordinate. It only lands where it was
recorded while the target window is exactly where it was — which is why click
macros usually stop working after a day. Dental Macros's **Record clicks...** is
modelled on an earlier AutoHotkey v2 auto-clicker script (not included here),
which solves it by remembering the window on the first recorded click and
forcing it back to that exact position and size before replaying.

### How it maps onto Dental Macros's steps

| Script | Dental Macros |
|---|---|
| `WinGetClass` / `WinGetPos` on the first recorded click | recorder stores the class, program, position and size |
| `WinActivate` + `WinWaitActive ,,2` | `window_prepare` — activates, then polls until the window really is in front |
| `WinGetPos` / `WinGetMinMax` into `Cur*` | `window_prepare` remembers the current geometry and maximised state |
| `BlockInput true` | `window_prepare`, `block_input` field |
| `WinRestore` when maximised, then `WinMove Teach*` | `window_prepare` un-maximises, then moves to the recorded rectangle |
| `Sleep 300` | `window_prepare`, `settle_ms` field |
| `MouseClick` + `Sleep WaitMs` per position | a `click` step and a `sleep` step per position |
| `WinMaximize` / `WinMove Cur*` | `window_release` |
| `BlockInput false` | `window_release`, `unblock_input` field |

### Where Dental Macros deliberately differs

- **`BlockInput` is checked.** Windows refuses it unless the process is
  elevated. The script calls it and ignores the result, so on a normal
  account that line does nothing. Dental Macros reports the refusal in the
  activity log instead of implying the sequence is protected.
- **The block is always lifted.** The script has no `try`/`finally`: if a
  click throws after `BlockInput true`, the keyboard and mouse stay dead
  until the script exits. Dental Macros releases the block at the end of every
  sequence, including when a step fails.
- **Windows are matched on class *and* program.** The script matches
  `ahk_class` alone, which is ambiguous for shared classes such as
  `Chrome_WidgetWin_1`. Clearing the *Program* field on the lock step gives
  script-identical behaviour.

See "Why recorded clicks keep working" in the main [README](../README.md).
