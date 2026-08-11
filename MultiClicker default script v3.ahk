#Requires AutoHotkey v2.0
; -------------------------------------------------------------------------
; Generic Auto-Clicker (v3 - Per-Click Delays)
; -------------------------------------------------------------------------
; HOW IT WORKS:
; 1. If no .ini exists, Setup runs automatically.
; 2. During setup you choose a hotkey, number of clicks, and a delay
;    for each individual click position.
; 3. Right-click each position to record it, enter its delay, repeat.
; 4. Delete the .ini file at any time to re-run setup.
; -------------------------------------------------------------------------

; Config file is named after this script -- rename the script for a new config.
; e.g. "MyMacro.ahk" -> "MyMacro.ini"
ConfigFile := RegExReplace(A_ScriptName, "\.[^.]+$", "") . ".ini"

; =========================================================================
; PHASE 1: FIRST-RUN SETUP
; =========================================================================
if !FileExist(ConfigFile)
{
    ; --- HOTKEY ---
    HotkeyInput := InputBox(
        "FIRST RUN SETUP`n`n"
        "Enter your preferred hotkey:`n"
        "(e.g., F10, ^j, !z, ^!c)`n`n"
        "Modifiers:  ^ = Ctrl   ! = Alt   + = Shift",
        "Step 1 of 2 - Set Hotkey", "w320 h185", "F10")

    if (HotkeyInput.Result == "Cancel" or HotkeyInput.Value == "") {
        MsgBox "Setup cancelled. Exiting."
        ExitApp
    }
    SavedHotkey := HotkeyInput.Value

    ; --- NUMBER OF CLICKS ---
    ClickCountInput := InputBox(
        "How many click positions do you want to record?`n`n"
        "Enter a number between 1 and 20.",
        "Step 2 of 2 - Number of Clicks", "w320 h150", "3")

    if (ClickCountInput.Result == "Cancel" or ClickCountInput.Value == "") {
        MsgBox "Setup cancelled. Exiting."
        ExitApp
    }
    NumClicks := Integer(ClickCountInput.Value)
    if (NumClicks < 1 or NumClicks > 20) {
        MsgBox "Invalid number. Please enter a value between 1 and 20.`nExiting."
        ExitApp
    }

    ; --- TEACHING MODE: RECORD POSITIONS + PER-CLICK DELAYS ---
    CoordMode "Mouse", "Screen"
    MsgBox(
        "TEACHING MODE`n`n"
        "You will now record " NumClicks " click position(s).`n`n"
        "For each step:`n"
        "  1. Right-click the target position to record it.`n"
        "  2. Enter the delay (ms) to wait AFTER that click.`n`n"
        "Click OK to begin.",
        "Teaching Mode")

    WinClass := ""
    TeachX := 0
    TeachY := 0
    TeachW := 0
    TeachH := 0

    loop NumClicks {
        StepNum := A_Index

        if (StepNum == 1)
            MsgBox("STEP 1 of " NumClicks "`n`nRight-click position #1.`n`n(The window under your cursor will be remembered.)")
        else
            MsgBox("STEP " StepNum " of " NumClicks "`n`nRight-click position #" StepNum ".")

        KeyWait "RButton", "D"
        MouseGetPos &CX, &CY, &WinID

        ; On the first click, capture the window class and dimensions
        if (StepNum == 1 and WinID) {
            WinClass := WinGetClass("ahk_id " WinID)
            WinGetPos &TeachX, &TeachY, &TeachW, &TeachH, "ahk_id " WinID
        }

        KeyWait "RButton", "U"
        SoundBeep 750, 100

        IniWrite(CX, ConfigFile, "Coords", "Click" StepNum "X")
        IniWrite(CY, ConfigFile, "Coords", "Click" StepNum "Y")

        WN := InputBox(
            "Delay after click #" StepNum " (ms):`n`n"
            "200 = fast   500 = half second   1500 = slow/dialog",
            "Click " StepNum " - Delay", "w300 h135", "200")
        IniWrite((WN.Result == "Cancel" ? 200 : Integer(WN.Value)), ConfigFile, "Coords", "Click" StepNum "Wait")
    }

    ; --- SAVE SETTINGS ---
    IniWrite(SavedHotkey, ConfigFile, "Settings", "UserHotkey")
    IniWrite(NumClicks,   ConfigFile, "Settings", "NumClicks")
    IniWrite(WinClass,    ConfigFile, "Settings", "TargetClass")
    IniWrite(TeachX,      ConfigFile, "Settings", "TeachX")
    IniWrite(TeachY,      ConfigFile, "Settings", "TeachY")
    IniWrite(TeachW,      ConfigFile, "Settings", "TeachW")
    IniWrite(TeachH,      ConfigFile, "Settings", "TeachH")

    MsgBox(
        "SETUP COMPLETE!`n`n"
        "Hotkey:   " SavedHotkey "`n"
        "Clicks:   " NumClicks "`n`n"
        "Each click has its own delay stored in the .ini.`n"
        "Press your hotkey anytime to run the sequence.`n"
        "Delete '" ConfigFile "' to reconfigure.",
        "Setup Complete")
}

; =========================================================================
; PHASE 2: LOAD SETTINGS AND REGISTER HOTKEY
; =========================================================================
SavedHotkey := IniRead(ConfigFile, "Settings", "UserHotkey", "Error")

if (SavedHotkey != "Error") {
    try {
        Hotkey SavedHotkey, RunClickSequence
    } catch {
        MsgBox "Error: The saved hotkey '" SavedHotkey "' is invalid.`nDelete '" ConfigFile "' to reconfigure."
        ExitApp
    }
} else {
    MsgBox "Configuration error. Delete '" ConfigFile "' and restart."
    ExitApp
}

TrayTip "Auto-Clicker Ready", "Press " SavedHotkey " to run the click sequence.", 3
return

; =========================================================================
; PHASE 3: CLICK SEQUENCE
; =========================================================================
RunClickSequence(*)
{
    global ConfigFile

    NumClicks   := Integer(IniRead(ConfigFile, "Settings", "NumClicks",  1))
    TargetClass := IniRead(ConfigFile, "Settings", "TargetClass", "")
    TeachX      := Integer(IniRead(ConfigFile, "Settings", "TeachX",     0))
    TeachY      := Integer(IniRead(ConfigFile, "Settings", "TeachY",     0))
    TeachW      := Integer(IniRead(ConfigFile, "Settings", "TeachW",     0))
    TeachH      := Integer(IniRead(ConfigFile, "Settings", "TeachH",     0))

    ; Activate and snap the target window ONCE before any clicks begin.
    ; Do NOT re-activate mid-sequence -- it interrupts click registration.
    if (TargetClass != "" and WinExist("ahk_class " TargetClass)) {
        WinActivate "ahk_class " TargetClass
        WinWaitActive "ahk_class " TargetClass, , 2

        WinGetPos &CurX, &CurY, &CurW, &CurH, "ahk_class " TargetClass
        MinMaxState := WinGetMinMax("ahk_class " TargetClass)

        BlockInput true
        if (MinMaxState == 1)
            WinRestore "ahk_class " TargetClass
        if (TeachW > 0 and TeachH > 0)
            WinMove TeachX, TeachY, TeachW, TeachH, "ahk_class " TargetClass
        Sleep 300
    } else {
        CurX := 0
        CurY := 0
        CurW := 0
        CurH := 0
        MinMaxState := 0
        BlockInput true
    }

    ; Perform all recorded clicks, each followed by its own delay
    CoordMode "Mouse", "Screen"
    loop NumClicks {
        CX     := Integer(IniRead(ConfigFile, "Coords", "Click" A_Index "X",    0))
        CY     := Integer(IniRead(ConfigFile, "Coords", "Click" A_Index "Y",    0))
        WaitMs := Integer(IniRead(ConfigFile, "Coords", "Click" A_Index "Wait", 200))

        MouseClick "Left", CX, CY
        Sleep WaitMs
    }

    ; Restore window to its original position and state
    if (TargetClass != "" and WinExist("ahk_class " TargetClass)) {
        if (MinMaxState == 1)
            WinMaximize "ahk_class " TargetClass
        else
            WinMove CurX, CurY, CurW, CurH, "ahk_class " TargetClass
    }

    BlockInput false

    ToolTip "Clicked " NumClicks " position(s)"
    SetTimer () => ToolTip(), -1500
}
