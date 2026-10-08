<#
  Instala o sistema de capas num cliente Lineage II Interlude  |  Installs the cloak system into a Lineage II Interlude client.

    .\install.ps1 -Client "C:\Lineage II"                          # pasta que tem System\, animations\, systextures\
    .\install.ps1 -Client "C:\Lineage II" -Packs "D:\packs"        # pasta com essence_capes.bin / essence_cloth.bin / essence_fx.bin que voce gerou
    .\install.ps1 -Client "C:\Lineage II" -Dll "D:\build\Release\dsetup.dll"
    .\install.ps1 -Client "C:\Lineage II" -WithDats                # tambem troca armorgrp.dat / ItemName-e.dat (so para o cliente Lucera/L2BR, veja o README)
    .\install.ps1 -Client "C:\Lineage II" -Uninstall               # devolve o dsetup.dll original

  O que faz / What it does
    * a dsetup.dll deste projeto e um proxy: ela repassa a funcao da DirectX para a original, que fica guardada como dsetup_ms.dll
    * copia dsetup.dll, essence_capes.bin, essence_cloth.bin, essence_fx.bin e (se nao existir) cloakhook.ini para a pasta System
    * copia cloakicons.utx para systextures (icones dos itens), se voce tiver o arquivo
    * nada e sobrescrito sem copia de seguranca (*.bak_AAAAMMDD_HHMMSS); a dsetup.dll deste projeto e substituida sem backup
    * o jogo precisa estar fechado
  Onde ele procura / where it looks
    -Dll    : client\System\dsetup.dll (pacote pronto)  ou  ..\source\build\Release\dsetup.dll (compilada com o CMake)
    -Packs  : client\System\ (pacote pronto)  -  senao informe a pasta onde voce gerou os .bin (docs\BUILD.md)
    -Icons  : client\systextures\cloakicons.utx (opcional)
  animations\LineShieldCloaks.ukx (pacote do L2BR/LineShield) nao vem aqui: o cliente L2BR ja tem; senao baixe LineShieldCloaks_carrier_package_v1.0.zip na Release v1.0.
#>
param(
    [Parameter(Mandatory = $true)][string]$Client,
    [string]$SystemFolder = 'System',
    [string]$Dll,
    [string]$Packs,
    [string]$Icons,
    [string]$DatsFolder,
    [string]$Ini,
    [switch]$WithDats,
    [switch]$ForceIni,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sys  = Join-Path $Client $SystemFolder
if (-not (Test-Path -LiteralPath (Join-Path $sys 'Engine.dll'))) { throw "$sys nao parece uma pasta System do Lineage II (sem Engine.dll) / does not look like a Lineage II System folder (no Engine.dll)" }

$running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path -like ((Resolve-Path -LiteralPath $Client).Path + '*') }
if ($running) { throw "Feche o jogo antes / close the game first: $($running[0].Path)" }

$cur = Join-Path $sys 'dsetup.dll'
$ms  = Join-Path $sys 'dsetup_ms.dll'
function IsMicrosoftDsetup($p) { (Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p).VersionInfo.CompanyName -match 'Microsoft') -and ((Get-Item -LiteralPath $p).Length -lt 200KB) }
function IsOurDll($p) {                                           # our proxy writes cloakhook.log / reads cloakhook.ini (UTF-16 strings in the binary)
    if (-not (Test-Path -LiteralPath $p)) { return $false }
    $b = [System.IO.File]::ReadAllBytes($p); $needle = [System.Text.Encoding]::Unicode.GetBytes('cloakhook.ini')
    for ($i = 0; $i -le $b.Length - $needle.Length; $i++) { if ($b[$i] -eq $needle[0]) { $k = 1; while ($k -lt $needle.Length -and $b[$i + $k] -eq $needle[$k]) { $k++ }; if ($k -eq $needle.Length) { return $true } } }
    return $false
}
function Backup($p) { if (Test-Path -LiteralPath $p) { $bak = "$p.bak_{0:yyyyMMdd_HHmmss}" -f (Get-Date); Copy-Item -LiteralPath $p -Destination $bak; "  backup: $bak" } }
function FirstExisting([string[]]$c) { foreach ($x in $c) { if ($x -and (Test-Path -LiteralPath $x)) { return $x } } return $null }

if ($Uninstall) {
    if (-not (Test-Path -LiteralPath $ms)) { throw "dsetup_ms.dll nao encontrada: nada para desfazer / not found: nothing to undo" }
    if (Test-Path -LiteralPath $cur) { if (IsOurDll $cur) { Remove-Item -LiteralPath $cur } else { throw "dsetup.dll atual nao e a deste projeto; nao mexi / the current dsetup.dll is not ours; left alone" } }
    Move-Item -LiteralPath $ms -Destination $cur
    'dsetup.dll original restaurada. Os arquivos essence_*.bin e cloakhook.ini continuam na pasta (pode apagar) / original dsetup.dll restored; essence_*.bin and cloakhook.ini are still there (delete if you like).'
    return
}

# ---- inputs
if (-not $Dll) { $Dll = FirstExisting @((Join-Path $here 'client\System\dsetup.dll'), (Join-Path $here '..\source\build\Release\dsetup.dll'), (Join-Path $here '..\build\Release\dsetup.dll')) }
if (-not $Dll -or -not (Test-Path -LiteralPath $Dll)) { throw "dsetup.dll deste projeto nao encontrada: compile (docs\BUILD.md) ou use -Dll / our dsetup.dll not found: build it (docs\BUILD.md) or pass -Dll" }
if (-not $Packs) { $Packs = Join-Path $here 'client\System' }
foreach ($f in 'essence_capes.bin', 'essence_cloth.bin', 'essence_fx.bin') {
    if (-not (Test-Path -LiteralPath (Join-Path $Packs $f))) { throw "arquivo ausente / missing: $(Join-Path $Packs $f) - gere os pacotes (docs\TUTORIAL.md, passo 3) e use -Packs / generate the packs (docs\TUTORIAL.md, step 3) and pass -Packs" }
}
if (-not $Ini) { $Ini = FirstExisting @((Join-Path $here 'client\System\cloakhook.ini'), (Join-Path $here 'cloakhook.ini')) }
if (-not $Icons) { $Icons = FirstExisting @((Join-Path $here 'client\systextures\cloakicons.utx')) }
if (-not $DatsFolder) { $DatsFolder = Join-Path $here 'client\dat_lucera_client' }

# 1) keep the original dsetup.dll as dsetup_ms.dll
if (-not (Test-Path -LiteralPath $ms)) {
    if (IsMicrosoftDsetup $cur) { Copy-Item -LiteralPath $cur -Destination $ms; 'dsetup_ms.dll criada a partir da dsetup.dll original / created from the original dsetup.dll' }
    else { throw "Nao achei a dsetup.dll original da Microsoft (cerca de 62 KB). Coloque-a em $sys como dsetup_ms.dll e rode de novo. / No original Microsoft dsetup.dll (about 62 KB) found: put it next to the game as dsetup_ms.dll and run again." }
}
# 2) our proxy
if ((Test-Path -LiteralPath $cur) -and -not (IsMicrosoftDsetup $cur) -and -not (IsOurDll $cur)) { Write-Host 'dsetup.dll existente (outra, nao e a original nem a deste projeto) / existing dsetup.dll (neither the original nor ours):'; Backup $cur }
Copy-Item -LiteralPath $Dll -Destination $cur -Force
# 3) data packs and ini
foreach ($f in 'essence_capes.bin', 'essence_cloth.bin', 'essence_fx.bin') { Copy-Item -LiteralPath (Join-Path $Packs $f) -Destination (Join-Path $sys $f) -Force }
$iniDst = Join-Path $sys 'cloakhook.ini'
if ($Ini) {
    if ($ForceIni -or -not (Test-Path -LiteralPath $iniDst)) { if (Test-Path -LiteralPath $iniDst) { Backup $iniDst }; Copy-Item -LiteralPath $Ini -Destination $iniDst -Force }
    else { 'cloakhook.ini ja existe: mantido (use -ForceIni para trocar) / already there: kept (-ForceIni replaces it)' }
} else { Write-Warning 'cloakhook.ini do projeto nao encontrado: sem ele o hook usa os padroes internos (Essence desligado) / project cloakhook.ini not found: the hook then uses built-in defaults (Essence off)' }
# 4) icons
$tex = Join-Path $Client 'systextures'
if ($Icons) {
    if (Test-Path -LiteralPath $tex) { $dst = Join-Path $tex 'cloakicons.utx'; Backup $dst; Copy-Item -LiteralPath $Icons -Destination $dst -Force } else { "pasta systextures nao encontrada em ${Client}: copie cloakicons.utx manualmente / systextures not found: copy cloakicons.utx by hand" }
} else { 'sem cloakicons.utx: os itens ficarao com o icone padrao (gere com tools\make_icon_utx.py) / no cloakicons.utx: items keep the default icon (build it with tools\make_icon_utx.py)' }
# 5) dats (only for the client they were built from)
if ($WithDats) {
    foreach ($f in 'armorgrp.dat', 'ItemName-e.dat') {
        $srcDat = Join-Path $DatsFolder $f
        if (-not (Test-Path -LiteralPath $srcDat)) { throw "arquivo ausente / missing: $srcDat (use -DatsFolder; gere com tools\build_cloak_armorgrp.py / build_cloak_itemname.py)" }
        $dst = Join-Path $sys $f; Backup $dst; Copy-Item -LiteralPath $srcDat -Destination $dst -Force
    }
    'ATENCAO: use .dat gerados a partir do .dat do SEU cliente (tools\build_cloak_armorgrp.py / build_cloak_itemname.py). / Use .dat files built from YOUR client''s dats.'
}
# 6) carrier package
$ukx = Join-Path $Client 'animations\LineShieldCloaks.ukx'
if (-not (Test-Path -LiteralPath $ukx)) { Write-Warning "animations\LineShieldCloaks.ukx nao encontrado: o sistema precisa dele (pacote do L2BR/LineShield; baixe LineShieldCloaks_carrier_package_v1.0.zip na Release v1.0 do GitHub). / not found: the system needs it (L2BR/LineShield package; download LineShieldCloaks_carrier_package_v1.0.zip from the GitHub v1.0 release)." }
"instalado em / installed in $sys  -  log: $(Join-Path $sys 'cloakhook.log')"
