# Generates compile_commands.json for clangd from the same repo-root *.rsp
# response files build.bat passes to the compiler, so editor tooling and the
# release build never disagree about the source set or flags.
#
#   pwsh -File gen_compdb.ps1
#
# Re-run after adding a source file to a .rsp. The output is a build artifact
# and is git-ignored.

$ErrorActionPreference = 'Stop'

$root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).ProviderPath }

function Read-Rsp {
    param([string]$Name)
    $sources = [System.Collections.Generic.List[string]]::new()
    $flags = [System.Collections.Generic.List[string]]::new()
    foreach ($line in Get-Content -LiteralPath (Join-Path $root $Name)) {
        $entry = $line.Trim()
        if ($entry -eq '') { continue }
        if ($entry -match '\.(c|cc|cpp|cxx)$') { $sources.Add($entry) }
        elseif ($entry.StartsWith('-')) { $flags.Add($entry) }
    }
    [pscustomobject]@{ Sources = $sources; Flags = $flags }
}

function New-Entries {
    param([string[]]$Driver, [string[]]$BaseFlags, $Rsp)
    $entries = [System.Collections.Generic.List[object]]::new()
    foreach ($src in $Rsp.Sources) {
        $arguments = $Driver + $BaseFlags + $Rsp.Flags + @('-c', $src)
        $entries.Add([ordered]@{
            directory = $root
            file      = $src
            arguments = $arguments
        })
    }
    return $entries
}

# MarkEdit's own sources carry no per-file flags in the .rsp (build.bat adds
# them on the command line); mirror that command line here.
$markeditFlags = @(
    '-std=c11', '-Os', '-ffunction-sections', '-fdata-sections',
    '-funwind-tables', '-fasynchronous-unwind-tables', '-fno-ident',
    '-fmerge-all-constants', '-Wall', '-Wextra', '-Wno-unknown-pragmas',
    '-I.', '-isystem', 'third_party/webview2/include',
    '-isystem', 'third_party/lexilla/include',
    '-isystem', 'third_party/scintilla/include'
)

# Lexilla's .rsp lists only files; the flags come from premake5.lua.
$lexillaFlags = @(
    '-std=c++17', '-DNDEBUG', '-fno-exceptions', '-fno-rtti',
    '-Ithird_party/lexilla/include', '-Ithird_party/lexilla/lexlib',
    '-Ithird_party/scintilla/include'
)

$entries = [System.Collections.Generic.List[object]]::new()
$entries.AddRange((New-Entries -Driver @('zig','cc')  -BaseFlags $markeditFlags -Rsp (Read-Rsp 'markedit-sources.rsp')))
$entries.AddRange((New-Entries -Driver @('zig','cc')  -BaseFlags @('-std=c11')  -Rsp (Read-Rsp 'cmark-gfm-sources.rsp')))
$entries.AddRange((New-Entries -Driver @('zig','cc')  -BaseFlags @('-std=c11')  -Rsp (Read-Rsp 'lua-sources.rsp')))
$entries.AddRange((New-Entries -Driver @('zig','c++') -BaseFlags $lexillaFlags  -Rsp (Read-Rsp 'lexilla-sources.rsp')))

$output = Join-Path $root 'compile_commands.json'
$entries | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $output -Encoding utf8
Write-Host ("Wrote {0} entries to {1}" -f $entries.Count, $output)
