[CmdletBinding()]
param(
    [string]$OutputDirectory = "build\msvc"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repo
$output = Join-Path $repo $OutputDirectory

function Get-RspSources([string]$Path) {
    Get-Content -LiteralPath $Path |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -match '\.(c|cc|cpp|cxx)$' } |
        ForEach-Object { Join-Path $repo $_ }
}

function Invoke-Tool([string]$File, [string[]]$Arguments, [string]$Failure) {
    & $File @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "$Failure (exit code $LASTEXITCODE)."
    }
}

function Compile-Sources(
    [string]$Language,
    [string[]]$Sources,
    [string]$ObjectDirectory,
    [string[]]$Options
) {
    New-Item -ItemType Directory -Force -Path $ObjectDirectory | Out-Null
    $standard = if ($Language -eq "C++") { "/std:c++17" } else { "/std:c11" }
    $languageOptions = if ($Language -eq "C++") { @("/EHsc") } else { @() }
    $arguments = @(
        "/nologo", "/c", $standard, "/O2", "/GL", "/Gy", "/Gw", "/MT",
        "/DWIN32", "/DNDEBUG", "/D_CRT_SECURE_NO_WARNINGS", "/utf-8", "/W4"
    ) + $languageOptions + $Options + @("/Fo$ObjectDirectory\") + $Sources
    Invoke-Tool "cl.exe" $arguments "MSVC failed compiling $Language sources"
    foreach ($source in $Sources) {
        Join-Path $ObjectDirectory (([IO.Path]::GetFileNameWithoutExtension($source)) + ".obj")
    }
}

New-Item -ItemType Directory -Force -Path $output | Out-Null
$objectRoot = Join-Path $output "obj"
$commonIncludes = @(
    "/I$repo",
    "/I$(Join-Path $repo 'third_party\webview2\include')",
    "/I$(Join-Path $repo 'third_party\scintilla\include')",
    "/I$(Join-Path $repo 'third_party\lexilla\include')",
    "/I$(Join-Path $repo 'third_party\lua\src')",
    "/I$(Join-Path $repo 'third_party\cmark-gfm\src')",
    "/I$(Join-Path $repo 'third_party\cmark-gfm\extensions')",
    "/DCMARK_GFM_STATIC_DEFINE",
    "/DCMARK_GFM_EXTENSIONS_STATIC_DEFINE"
)

$tinkereditObjects = Compile-Sources "C" (Get-RspSources "tinkeredit-sources.rsp") (Join-Path $objectRoot "tinkeredit") ($commonIncludes + @("/wd4152", "/wd4702", "/Ob3"))
$luaObjects = Compile-Sources "C" (Get-RspSources "lua-sources.rsp") (Join-Path $objectRoot "lua") @(
    "/I$(Join-Path $repo 'third_party\lua\src')", "/wd4244", "/wd4310", "/wd4324", "/wd4701", "/wd4702"
)
$cmarkObjects = Compile-Sources "C" (Get-RspSources "cmark-gfm-sources.rsp") (Join-Path $objectRoot "cmark-gfm") @(
    "/I$(Join-Path $repo 'third_party\cmark-gfm\src')",
    "/I$(Join-Path $repo 'third_party\cmark-gfm\extensions')",
    "/DCMARK_GFM_STATIC_DEFINE", "/DCMARK_GFM_EXTENSIONS_STATIC_DEFINE", "/wd4100"
)
$lexillaObjects = Compile-Sources "C++" (Get-RspSources "lexilla-sources.rsp") (Join-Path $objectRoot "lexilla") @(
    "/I$(Join-Path $repo 'third_party\lexilla\include')",
    "/I$(Join-Path $repo 'third_party\lexilla\lexlib')",
    "/I$(Join-Path $repo 'third_party\scintilla\include')"
)

$resource = Join-Path $output "tinkeredit.res"
Invoke-Tool "rc.exe" @("/nologo", "/fo$resource", (Join-Path $repo "tinkeredit.rc")) "Resource compilation failed"

$executable = Join-Path $output "tinkeredit.exe"
$importLibrary = Join-Path $output "tinkeredit.lib"
$libraries = @(
    (Join-Path $repo "third_party\webview2\x64\WebView2Loader.dll.lib"),
    "gdi32.lib", "msimg32.lib", "user32.lib", "comdlg32.lib", "comctl32.lib",
    "shlwapi.lib", "shell32.lib", "ole32.lib", "mpr.lib", "windowscodecs.lib",
    "uxtheme.lib", "dwmapi.lib"
)
$linkArguments = @(
    "/nologo", "/LTCG", "/OPT:REF", "/OPT:ICF", "/SUBSYSTEM:WINDOWS", "/MANIFEST:NO",
    "/OUT:$executable", "/IMPLIB:$importLibrary"
) + $tinkereditObjects + $luaObjects + $cmarkObjects + $lexillaObjects + @($resource) + $libraries
Invoke-Tool "link.exe" $linkArguments "MSVC failed linking tinkeredit.exe"

$size = (Get-Item -LiteralPath $executable).Length
Write-Host "MSVC build succeeded: $executable ($size bytes)"
