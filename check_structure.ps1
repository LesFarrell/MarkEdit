# Structural guard for MarkEdit's source layout. Run from the repository root
# (test.bat calls it) or via `pwsh -File check_structure.ps1`.
#
# It enforces the conventions described in docs/architecture.md:
#   1. Every src\*.c file is listed in a repo-root *.rsp response file, so the
#      release build and the test build agree on the source set.
#   2. The private src\internal\*.inc components are pulled into the unity
#      translation unit by src\markedit.c and nowhere else.
#   3. Every src\internal\*.inc file is actually reached from markedit.c (no
#      orphaned component left behind by an extraction).
#   4. Every src\*.h file is #included from somewhere, so a header left behind
#      by a deleted or renamed .c file is caught instead of drifting silently.
#
# The checks are structural, not stylistic; they exist so a new file cannot
# silently drift out of the build or the include surface.

$ErrorActionPreference = 'Stop'

$root = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).ProviderPath }
$failures = [System.Collections.Generic.List[string]]::new()

# 1. Every compiled source is named by at least one *.rsp (a few, such as
#    file_io.c, are in both markedit-sources.rsp and plugin-sources.rsp).
$rspSources = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($rsp in Get-ChildItem -LiteralPath $root -Filter '*.rsp') {
    foreach ($line in Get-Content -LiteralPath $rsp.FullName) {
        $entry = $line.Trim()
        if ($entry -match '\.c$') { [void]$rspSources.Add($entry.Replace('\', '/')) }
    }
}
foreach ($c in Get-ChildItem -LiteralPath (Join-Path $root 'src') -Recurse -Filter '*.c') {
    $rel = $c.FullName.Substring($root.Length + 1).Replace('\', '/')
    if (-not $rspSources.Contains($rel)) {
        $failures.Add("src/$($c.Name) is not listed in any *.rsp response file")
    }
}

$unity = Join-Path $root 'src\markedit.c'
$includePattern = '#\s*include\s+"internal/'

# 2. Only the unity translation unit includes the *.inc components.
foreach ($src in Get-ChildItem -LiteralPath (Join-Path $root 'src') -Recurse -Filter '*.c') {
    if ($src.FullName -eq $unity) { continue }
    $hits = Select-String -LiteralPath $src.FullName -Pattern $includePattern
    if ($hits) {
        $failures.Add("$($src.Name) includes an internal/*.inc component; only src/markedit.c may")
    }
}

# 3. Every component is included by the unity translation unit.
$unityText = Get-Content -LiteralPath $unity -Raw
foreach ($inc in Get-ChildItem -LiteralPath (Join-Path $root 'src\internal') -Filter '*.inc') {
    if ($unityText -notmatch [regex]::Escape("internal/$($inc.Name)")) {
        $failures.Add("src/internal/$($inc.Name) is not included by src/markedit.c")
    }
}

# 4. No header is left orphaned by a deleted or renamed .c file.
$allSources = @(Get-ChildItem -LiteralPath (Join-Path $root 'src') -Recurse -File -Include '*.c', '*.inc')
$includedHeaders = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)
foreach ($src in $allSources) {
    foreach ($hit in Select-String -LiteralPath $src.FullName -Pattern '#\s*include\s+["<]([^">]+\.h)[">]') {
        [void]$includedHeaders.Add((Split-Path -Leaf $hit.Matches[0].Groups[1].Value))
    }
}
foreach ($h in Get-ChildItem -LiteralPath (Join-Path $root 'src') -Recurse -Filter '*.h') {
    if (-not $includedHeaders.Contains($h.Name)) {
        $failures.Add("src/$($h.Name) is not #included by any .c or .inc file")
    }
}

if ($failures.Count -gt 0) {
    Write-Host 'Structure check failed:'
    foreach ($f in $failures) { Write-Host "  - $f" }
    exit 1
}

Write-Host 'Structure check passed.'
