# Generates compile_commands.json for clangd from the same repo-root *.rsp
# response files build.bat reads, so editor tooling and the
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

# release-flags.rsp holds build.bat's code-generation flags for every source.
$releaseFlags = (Read-Rsp 'release-flags.rsp').Flags
$cmark = Read-Rsp 'cmark-gfm-sources.rsp'
$lua = Read-Rsp 'lua-sources.rsp'

# MarkEdit's own sources carry no per-file flags in the .rsp (build.bat adds
# them on the command line); mirror that command line here. It also carries
# the cmark-gfm and Lua -D/-I flags, which markdown.c and the plugin host need.
$markeditFlags = @('-std=c11') + $releaseFlags + @(
    '-Wall', '-Wextra', '-Wno-unknown-pragmas',
    '-I.', '-isystem', 'third_party/webview2/include',
    '-isystem', 'third_party/lexilla/include',
    '-isystem', 'third_party/scintilla/include'
) + $cmark.Flags + $lua.Flags

# cmark-gfm is compiled apart from the app sources, as in build.bat.
$cmarkFlags = @('-std=c11') + $releaseFlags + @('-Wall', '-Wextra', '-Wno-unknown-pragmas', '-Wno-unused-parameter')

# Lexilla's .rsp lists only files; the flags come from build.bat.
$lexillaFlags = @('-std=c++17') + $releaseFlags + @(
    '-DNDEBUG',
    '-Ithird_party/lexilla/include', '-Ithird_party/lexilla/lexlib',
    '-Ithird_party/scintilla/include'
)

$entries = [System.Collections.Generic.List[object]]::new()
$entries.AddRange((New-Entries -Driver @('zig','cc')  -BaseFlags $markeditFlags -Rsp (Read-Rsp 'markedit-sources.rsp')))
$entries.AddRange((New-Entries -Driver @('zig','cc')  -BaseFlags $cmarkFlags    -Rsp $cmark))
$entries.AddRange((New-Entries -Driver @('zig','cc')  -BaseFlags @('-std=c11')  -Rsp $lua))
$entries.AddRange((New-Entries -Driver @('zig','c++') -BaseFlags $lexillaFlags  -Rsp (Read-Rsp 'lexilla-sources.rsp')))

$output = Join-Path $root 'compile_commands.json'
$entries | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $output -Encoding utf8
Write-Host ("Wrote {0} entries to {1}" -f $entries.Count, $output)
