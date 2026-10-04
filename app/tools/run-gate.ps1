# Runs the eight-family gate and says, per family, whether it passed.
#
#   .\tools\run-gate.ps1                      both configs, as built
#   .\tools\run-gate.ps1 -Config Release      one config
#   .\tools\run-gate.ps1 -Build               build each config first
#   .\tools\run-gate.ps1 -Screen DELL         put the test windows there
#
# WHERE THE TEST WINDOWS OPEN when -Screen is not given: on the Cintiq, the
# fixed test screen (tests/TestHarness.cpp), or on the primary screen if it
# is not connected. Each log's TESTSCREEN line says which and why.
# THIS CONSOLE MINIMISES ITSELF while the tests run and comes back when they
# finish, so it never sits over a test window on that screen - a person
# reaching for the console, or for a test window covering it, is how a run
# gets disturbed. The logs of the run before this one are kept (see below).
#
# Run it from a PowerShell window opened from the Start menu when the point
# is to guard YOUR real settings: Lifecycle section (l) prints which store it
# is guarding, and it is the real one only outside the Claude desktop app.
# Close SankoTV first, and leave the machine alone while it runs - the
# families open real windows (on one screen: see TESTSCREEN in the output).
#
# WHY A SCRIPT RATHER THAN EIGHT COMMANDS: a test that cannot start because
# a DLL is missing never runs a line of its own code, so only its LAUNCHER
# can stop Windows raising a "was not found" dialog and waiting for a click.
# This sets the process error mode that children inherit, so that failure
# comes back as an exit code like every other.

param(
    [string[]]$Config = @('Release', 'Debug'),
    [switch]$Build,
    [string]$Screen = '',
    [string]$Qt = 'C:\Qt\6.11.1\msvc2022_64',
    [string]$CMake = 'C:\Qt\Tools\CMake_64\bin\cmake.exe'
)

$app = Split-Path -Parent $PSScriptRoot
Set-Location $app
# NO SETUP STEPS: this works from a brand-new PowerShell window. Qt's bin
# folder goes on PATH here, for this run only (nothing is changed outside
# this process), and CMake is called by its full path - neither is assumed
# to be on PATH already.
if (Test-Path "$Qt\bin") {
    $env:PATH = "$Qt\bin;$env:PATH"
} else {
    "NOTE: Qt was not found at $Qt - pass -Qt <folder>. Continuing with the Qt DLLs deployed beside the test executables."
}
if ($Screen) { $env:SANKO_TEST_SCREEN = $Screen }

Add-Type -Namespace Sanko -Name ErrorMode -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);
'@
# SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX
[void][Sanko.ErrorMode]::SetErrorMode(0x8003)

$families = 'SankoBrushLibraryTest', 'SankoPaintPixelLock', 'SankoCanvasBrushLock',
            'SankoQuickShapeGeometryLock', 'SankoDevRecorderTest', 'SankoCanvasEdgeLock',
            'SankoCanvasSizeLock', 'SankoProjectLifecycle'
$out = Join-Path $env:TEMP 'sanko_gate'
New-Item -ItemType Directory -Force $out | Out-Null
# THE RUN BEFORE THIS ONE IS KEPT. The logs used to be overwritten in place,
# and on 2026-10-04 a failing Release run was lost to the re-run that was
# meant to investigate it. Whatever is in the folder now moves to
# runs\<the time of its newest log>\ first; the ten newest are kept.
$old = @(Get-ChildItem -LiteralPath $out -File -Filter '*.txt')
$kept = ''
if ($old.Count -gt 0) {
    $stamp = ($old | Sort-Object LastWriteTime | Select-Object -Last 1).LastWriteTime.ToString('yyyyMMdd_HHmmss')
    $kept = Join-Path (Join-Path $out 'runs') $stamp
    New-Item -ItemType Directory -Force $kept | Out-Null
    $old | Move-Item -Destination $kept -Force
    Get-ChildItem -LiteralPath (Join-Path $out 'runs') -Directory | Sort-Object Name -Descending |
        Select-Object -Skip 10 | Remove-Item -Recurse -Force
}
$failing = 0

# OUT OF THE WAY WHILE THE TESTS RUN. Minimised without activating anything
# (SW_SHOWMINNOACTIVE), and restored in the finally below - so it comes back
# after a failure and after Ctrl+C too. Only a classic console has a window
# of its own to minimise: under Windows Terminal the console window is a
# hidden stand-in, IsWindowVisible says no, and nothing is done.
Add-Type -Namespace Sanko -Name Console -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int how);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
'@
$console = [Sanko.Console]::GetConsoleWindow()
$minimisedHere = $false
if ($console -ne [IntPtr]::Zero -and [Sanko.Console]::IsWindowVisible($console) -and
    -not [Sanko.Console]::IsIconic($console)) {
    "This window minimises itself while the tests run and returns when they finish."
    Start-Sleep -Milliseconds 400
    [void][Sanko.Console]::ShowWindow($console, 7)
    $minimisedHere = $true
} else {
    "NOTE: this console has no window of its own to minimise (Windows Terminal?) - keep it off the test screen."
}

try {
foreach ($cfg in $Config) {
    if ($Build) {
        & $CMake --build build --config $cfg | Out-Null
        if ($LASTEXITCODE -ne 0) { "BUILD FAILED ($cfg), exit $LASTEXITCODE"; $failing++; continue }
    }
    foreach ($family in $families) {
        $exe = Join-Path $app "build\$cfg\$family.exe"
        $log = Join-Path $out "${family}_$cfg.txt"
        if (-not (Test-Path $exe)) { "{0,-30} {1,-8} NOT BUILT" -f $family, $cfg; $failing++; continue }
        $watch = [Diagnostics.Stopwatch]::StartNew()
        # cmd does the redirection, so the log is plain bytes whatever this
        # PowerShell's own encoding rules are.
        cmd /c "`"$exe`" > `"$log`" 2>&1"
        $code = $LASTEXITCODE
        "{0,-30} {1,-8} exit {2,-11} {3,4}s" -f $family, $cfg, $code, [int]$watch.Elapsed.TotalSeconds
        if ($code -ne 0) {
            $failing++
            # The reason, without opening the log: a startup failure, or the
            # checks that failed.
            $why = @(Select-String -Path $log -Pattern 'STARTUP FAILED|\*\*FAIL\*\*|^FAIL ' |
                Select-Object -First 6 | ForEach-Object { $_.Line.Trim() })
            # A test window that was minimised, or lost the foreground to
            # another program, during the run explains failures that are
            # not defects: say it first.
            $disturbed = @(Select-String -Path $log -Pattern '^TESTWINDOW: a test window (was minimised|lost the foreground)' |
                Select-Object -First 2 | ForEach-Object { $_.Line.Trim() })
            $why = $disturbed + $why
            if ($why.Count -eq 0) {
                # Nothing recognisable (a Debug build reports a failed start
                # in Qt's own words, a crash in the C runtime's): the last
                # lines of the log are the reason.
                $why = @(Get-Content $log | Where-Object { $_.Trim() } |
                    Select-Object -Last 3 | ForEach-Object { $_.Trim() })
            }
            $why | ForEach-Object { "      " + $_.Substring(0, [Math]::Min(300, $_.Length)) }
            if ($code -lt 0 -or $code -gt 255) {
                $note = if ($code -eq -1073741515) { 'a DLL was not found' }
                        elseif ($code -eq -1073740791) { 'the program aborted' }
                        else { 'the process did not end normally' }
                "      (exit 0x{0:X8}: {1})" -f $code, $note
            }
        }
    }
    $life = Join-Path $out "SankoProjectLifecycle_$cfg.txt"
    if (Test-Path $life) {
        Select-String -Path $life -Pattern '^TESTSCREEN: test windows|^TESTSCREEN: not pinned|GUARDING:' |
            ForEach-Object { "   $cfg  " + $_.Line.Trim() }
    }
}
} finally {
    if ($minimisedHere) { [void][Sanko.Console]::ShowWindow($console, 9) } # SW_RESTORE
}

"FAMILIES FAILING: $failing   (logs in $out)"
if ($kept) { "the run before this one is kept in $kept" }
exit $failing
