<#
  Installs cloakhook into a Lineage II System folder.
    .\deploy.ps1 -Target "C:\Lineage II\System"
  - keeps the original Microsoft dsetup.dll as dsetup_ms.dll (needed: our dsetup.dll forwards DirectXSetupGetVersion to it)
  - copies build\Release\dsetup.dll and (only if missing) cloakhook.ini
  Nothing is overwritten without a backup. Undo: delete dsetup.dll and rename dsetup_ms.dll back to dsetup.dll.
#>
param([Parameter(Mandatory = $true)][string]$Target, [switch]$ForceIni)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$dll  = Join-Path $here 'build\Release\dsetup.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "build first: cmake --build build --config Release ($dll not found)" }
if (-not (Test-Path -LiteralPath (Join-Path $Target 'Engine.dll'))) { throw "$Target does not look like a L2 System folder (no Engine.dll)" }

$cur = Join-Path $Target 'dsetup.dll'
$ms  = Join-Path $Target 'dsetup_ms.dll'
function IsMicrosoftDsetup($p) { (Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p).VersionInfo.CompanyName -match 'Microsoft') -and ((Get-Item -LiteralPath $p).Length -lt 200KB) }

if (-not (Test-Path -LiteralPath $ms)) {
    if (IsMicrosoftDsetup $cur) { Copy-Item -LiteralPath $cur -Destination $ms; "dsetup_ms.dll created from the current Microsoft dsetup.dll" }
    else { throw "No Microsoft dsetup.dll found. Put the original (about 62 KB) next to the game as dsetup_ms.dll and run again." }
}
if ((Test-Path -LiteralPath $cur) -and -not (IsMicrosoftDsetup $cur)) {
    $bak = "$cur.bak_{0:yyyyMMdd_HHmmss}" -f (Get-Date)
    Move-Item -LiteralPath $cur -Destination $bak
    "existing non-Microsoft dsetup.dll moved to $bak"
} elseif (Test-Path -LiteralPath $cur) { Remove-Item -LiteralPath $cur }   # Microsoft copy already preserved as dsetup_ms.dll

Copy-Item -LiteralPath $dll -Destination $cur
$ini = Join-Path $Target 'cloakhook.ini'
if ($ForceIni -or -not (Test-Path -LiteralPath $ini)) { Copy-Item -LiteralPath (Join-Path $here 'cloakhook.ini') -Destination $ini }
"installed. After starting the game read: $(Join-Path $Target 'cloakhook.log')"
