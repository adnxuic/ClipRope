param([Parameter(Mandatory)][string]$RestoreExe, [string]$Report)
$ErrorActionPreference = 'Stop'

# Bind only a sheet belonging to the process we started on this window station.
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class ExcelProbeWindow {
    private delegate bool Visitor(IntPtr hwnd, IntPtr arg);
    [DllImport("user32")] private static extern bool EnumWindows(Visitor visit, IntPtr arg);
    [DllImport("user32")] private static extern bool EnumChildWindows(IntPtr hwnd, Visitor visit, IntPtr arg);
    [DllImport("user32")] private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr hwnd, StringBuilder name, int size);
    [DllImport("user32")] private static extern IntPtr GetProcessWindowStation();
    [DllImport("user32")] private static extern bool OpenClipboard(IntPtr owner);
    [DllImport("user32")] private static extern bool CloseClipboard();
    [DllImport("user32")] private static extern bool EmptyClipboard();
    [DllImport("user32")] private static extern uint GetClipboardSequenceNumber();
    public static bool ReleaseSample(uint sequence) {
        if (sequence == 0 || !OpenClipboard(IntPtr.Zero)) return false;
        try { return sequence != GetClipboardSequenceNumber() || EmptyClipboard(); }
        finally { CloseClipboard(); }
    }
    [DllImport("user32", CharSet=CharSet.Unicode)] private static extern bool GetUserObjectInformation(IntPtr obj, int index, StringBuilder name, int size, out int needed);
    [DllImport("oleacc")] private static extern int AccessibleObjectFromWindow(IntPtr hwnd, uint id, ref Guid iid, [MarshalAs(UnmanagedType.IDispatch)] out object target);
    public static bool PrivateStation() {
        var name = new StringBuilder(256);
        int needed;
        return GetUserObjectInformation(GetProcessWindowStation(), 2, name, name.Capacity*2, out needed)
            && !name.ToString().Equals("WinSta0", StringComparison.OrdinalIgnoreCase)
            && name.ToString().Equals(Environment.GetEnvironmentVariable("CLIPROPE_PROBE_STATION"), StringComparison.OrdinalIgnoreCase);
    }
    public static object Find(int process) {
        object sheet = null;
        EnumWindows(delegate(IntPtr top, IntPtr arg) {
            uint pid;
            GetWindowThreadProcessId(top, out pid);
            if (pid != process) return true;
            EnumChildWindows(top, delegate(IntPtr child, IntPtr unused) {
                var name = new StringBuilder(32);
                GetClassName(child, name, name.Capacity);
                if (name.ToString() == "EXCEL7") {
                    var iid = new Guid("00020400-0000-0000-C000-000000000046");
                    AccessibleObjectFromWindow(child, 0xFFFFFFF0, ref iid, out sheet);
                }
                return sheet == null;
            }, IntPtr.Zero);
            return sheet == null;
        }, IntPtr.Zero);
        return sheet;
    }
}
'@
if (-not [ExcelProbeWindow]::PrivateStation()) { throw 'Run through cliprope_station_test.' }
$excelPath = (Get-Item 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\excel.exe').GetValue('')
$fixture = Join-Path $env:TEMP ("ClipRope-{0}.csv" -f [guid]::NewGuid())
$app = $null
$process = $null
$restoreSequence = 0
function Record([string]$line) {
    Write-Output $line
    if ($Report) { [IO.File]::AppendAllText($Report, $line + [Environment]::NewLine) }
}
try {
    [IO.File]::WriteAllText($fixture, "P0 synthetic range`r`n")
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $excelPath
    $start.Arguments = "/x /safe `"$fixture`""
    $start.UseShellExecute = $false
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $process = [Diagnostics.Process]::Start($start)
    $deadline = [DateTime]::UtcNow.AddSeconds(40)
    do {
        if ($process.HasExited) { throw "The dedicated Excel process exited ($($process.ExitCode))." }
        $window = [ExcelProbeWindow]::Find($process.Id)
        if ($window) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $window) { throw 'Could not bind a worksheet window on the private desktop.' }
    $app = $window.Application
    $app.DisplayAlerts = $false
    $book = $window.ActiveSheet.Parent
    if ($book.FullName -ne $fixture) { throw 'Unexpected workbook in the dedicated test instance.' }
    $sheet = $book.Worksheets.Item(1)
    $sheet.Range('A1').Formula = '=2+3'
    $sheet.Range('A1').Font.Bold = $true
    $sheet.Range('A1').Interior.Color = 65535
    $sheet.Range('B1').Value2 = 'P0 sample'
    function Paste-Observation([string]$address) {
        $dest = $sheet.Range($address)
        $sheet.Paste($dest)
        $sameValue = ($dest.Value2 -eq 5)
        $sameFormula = ($dest.Formula -eq '=2+3')
        $sameBold = [bool]$dest.Font.Bold
        $sameFill = ($dest.Interior.Color -eq 65535)
        $next = $dest.Offset(0,1)
        $secondCell = ($next.Value2 -ceq 'P0 sample')
        $dest.Formula = '=3+4'
        $editable = ($dest.Value2 -eq 7)
        Record "paste=$address value=$sameValue adjacentText=$secondCell formula=$sameFormula bold=$sameBold fill=$sameFill editableCell=$editable"
        if ($address -eq 'D3' -and -not ($sameValue -and $secondCell -and $sameFormula -and $sameBold -and $sameFill -and $editable)) {
            throw 'The native baseline did not match the fixture.'
        }
    }
    Record ("Excel file version " + [Diagnostics.FileVersionInfo]::GetVersionInfo($excelPath).FileVersion)
    $sheet.Range('A1:B1').Copy()
    Paste-Observation 'D3'
    $sheet.Range('A1:B1').Copy()
    $restoreOutput = & $RestoreExe $process.Id
    foreach ($line in $restoreOutput) {
        Record $line
        if ($line -match '^restore-sequence=(\d+)$') { $restoreSequence = [uint32]$Matches[1] }
    }
    if ($LASTEXITCODE) { throw 'The Office restoration probe failed.' }
    if (!$restoreSequence) { throw 'The restored sequence was unavailable.' }
    Paste-Observation 'D5'
} finally {
    try {
        if ($app) { $app.DisplayAlerts = $false; $app.Quit() }
    } finally {
        if ($process -and -not $process.WaitForExit(3000)) { $process.Kill(); $process.WaitForExit() }
        if ([IO.File]::Exists($fixture)) { [IO.File]::Delete($fixture) }
        if ($restoreSequence -and -not [ExcelProbeWindow]::ReleaseSample($restoreSequence)) {
            throw 'Could not release the synthetic clipboard sample.'
        }
    }
}
