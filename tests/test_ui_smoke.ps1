param(
    [string]$Executable = (Join-Path $PSScriptRoot '..\dist\tinkeredit.exe')
)

$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).ProviderPath

Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class TinkerEditorUiTestNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr GetDlgItem(IntPtr parent, int id);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool GetWindowRect(IntPtr window, out RECT rect);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetWindowPos(IntPtr window, IntPtr insertAfter,
        int x, int y, int width, int height, uint flags);
}
'@

$WM_COMMAND = 0x0111
$WM_CLOSE = 0x0010
$IDM_VIEW_GIT = 414
$ID_OUTPUT_PANEL = 112
$ID_OUTPUT_CLOSE = 120
$ID_OUTPUT_TAB_RUN = 116
$ID_OUTPUT_TAB_PROBLEMS = 122
$ID_OUTPUT_TAB_GIT = 123
$SWP_NOMOVE = 0x0002
$SWP_NOZORDER = 0x0004

function Wait-Until([scriptblock]$Condition, [string]$Failure, [int]$TimeoutMs = 10000) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $TimeoutMs) {
        if (& $Condition) { return }
        Start-Sleep -Milliseconds 50
    }
    throw $Failure
}

function Send-Command([IntPtr]$Window, [int]$Command) {
    [void][TinkerEditorUiTestNative]::SendMessage(
        $Window, $WM_COMMAND, [IntPtr]$Command, [IntPtr]::Zero)
}

function Assert-PanelLayout([IntPtr]$Main, [IntPtr]$Panel) {
    $mainRect = [TinkerEditorUiTestNative+RECT]::new()
    $panelRect = [TinkerEditorUiTestNative+RECT]::new()
    if (-not [TinkerEditorUiTestNative]::GetWindowRect($Main, [ref]$mainRect) -or
        -not [TinkerEditorUiTestNative]::GetWindowRect($Panel, [ref]$panelRect)) {
        throw 'Could not read the main/output panel rectangles.'
    }
    $width = $panelRect.Right - $panelRect.Left
    $height = $panelRect.Bottom - $panelRect.Top
    if ($width -lt 200 -or $height -lt 80 -or
        $panelRect.Left -lt $mainRect.Left -or $panelRect.Right -gt $mainRect.Right -or
        $panelRect.Top -lt $mainRect.Top -or $panelRect.Bottom -gt $mainRect.Bottom) {
        throw "Invalid output panel rectangle after relayout: ${width}x${height}."
    }
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('tinkeredit-ui-' + [guid]::NewGuid().ToString('N'))
$oldAppData = $env:APPDATA
$oldLocalAppData = $env:LOCALAPPDATA
$process = $null
try {
    $appData = Join-Path $tempRoot 'Roaming'
    $localAppData = Join-Path $tempRoot 'Local'
    [void](New-Item -ItemType Directory -Path $appData, $localAppData -Force)
    $env:APPDATA = $appData
    $env:LOCALAPPDATA = $localAppData
    $process = Start-Process -FilePath $executablePath -PassThru
    $env:APPDATA = $oldAppData
    $env:LOCALAPPDATA = $oldLocalAppData

    Wait-Until { $process.Refresh(); $process.MainWindowHandle -ne [IntPtr]::Zero } `
        'Tinker Editor did not create its main window.'
    $main = $process.MainWindowHandle
    $panel = [TinkerEditorUiTestNative]::GetDlgItem($main, $ID_OUTPUT_PANEL)
    if ($panel -eq [IntPtr]::Zero) { throw 'The output panel control was not created.' }

    Send-Command $main $IDM_VIEW_GIT
    Wait-Until { [TinkerEditorUiTestNative]::IsWindowVisible($panel) } `
        'Opening the Git panel did not make the output panel visible.'
    Assert-PanelLayout $main $panel

    Send-Command $panel $ID_OUTPUT_TAB_PROBLEMS
    Send-Command $panel $ID_OUTPUT_TAB_RUN
    Send-Command $panel $ID_OUTPUT_TAB_GIT
    Assert-PanelLayout $main $panel

    Send-Command $panel $ID_OUTPUT_CLOSE
    Wait-Until { -not [TinkerEditorUiTestNative]::IsWindowVisible($panel) } `
        'Closing the output panel did not hide it.'

    Send-Command $main $IDM_VIEW_GIT
    Wait-Until { [TinkerEditorUiTestNative]::IsWindowVisible($panel) } `
        'Reopening the Git panel did not make the output panel visible.'
    Assert-PanelLayout $main $panel

    if (-not [TinkerEditorUiTestNative]::SetWindowPos(
        $main, [IntPtr]::Zero, 0, 0, 1000, 700, $SWP_NOMOVE -bor $SWP_NOZORDER)) {
        throw 'Could not resize the main window.'
    }
    Start-Sleep -Milliseconds 100
    Assert-PanelLayout $main $panel
    Write-Host 'UI smoke tests passed.'
}
finally {
    $env:APPDATA = $oldAppData
    $env:LOCALAPPDATA = $oldLocalAppData
    if ($process -and -not $process.HasExited) {
        [void][TinkerEditorUiTestNative]::SendMessage(
            $process.MainWindowHandle, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
        if (-not $process.WaitForExit(5000)) { $process.Kill() }
    }
    $resolvedTemp = [IO.Path]::GetFullPath($tempRoot)
    $systemTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolvedTemp.StartsWith($systemTemp, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedTemp)) {
        # WebView2 helpers can release their profile lock just after the main
        # process exits. Retry that narrow, verified temp target briefly so a
        # successful UI run is not reported as a cleanup failure.
        for ($attempt = 0; $attempt -lt 50 -and (Test-Path -LiteralPath $resolvedTemp); $attempt++) {
            try {
                Remove-Item -LiteralPath $resolvedTemp -Recurse -Force -ErrorAction Stop
            }
            catch {
                Start-Sleep -Milliseconds 100
            }
        }
        if (Test-Path -LiteralPath $resolvedTemp) {
            Write-Warning "Could not remove temporary WebView2 profile: $resolvedTemp"
        }
    }
}
