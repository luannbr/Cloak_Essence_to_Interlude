# Tutorial: Essence cloaks in an Interlude client

**PT-BR** | [English below](#english)

Passo a passo do zero até ver a capa no personagem. Leva uma tarde na primeira vez; depois de gerar os pacotes (passo 3) o resto é rápido.

## 0. O que você precisa

| Item | Para quê |
|---|---|
| Um cliente **Interlude (C6) de 32 bits** do Lucera/L2BR | onde as capas vão aparecer (o hook confere o `Engine.dll`, veja o [README](../README.md#limitações)) |
| `animations\LineShieldCloaks.ukx` desse cliente (≈ 294 MB) | malhas "carrier" — **não vem neste repositório** |
| A `dsetup.dll` **original da Microsoft** (≈ 62 KB) | o instalador a guarda como `dsetup_ms.dll` |
| Um cliente **Lineage II Essence** (testado com a versão 5.41/5.42 EU) | de onde saem malhas, animações, texturas e efeitos |
| Windows 10/11, PowerShell 5.1, **Python 3.10+**, **Visual Studio 2022 (x86)** e **CMake** | gerar os dados e compilar |
| Servidor **Lucera Interlude** (opcional para testar) | itens 9400–9466 no slot BACK |

## 1. Baixar o projeto

```powershell
git clone https://github.com/luannbr/Cloak_Essence_to_Interlude.git
cd Cloak_Essence_to_Interlude
pip install numpy pillow scipy
```

## 2. Compilar a DLL

```powershell
cd source
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
cd ..
```

Resultado: `source\build\Release\dsetup.dll` (o `install\install.ps1` já procura nesse caminho).

## 3. Gerar os pacotes de dados (a partir do **seu** cliente Essence)

Os `essence_*.bin` contêm conteúdo da NCSoft e por isso **não estão no repositório**: você os gera do seu cliente Essence.

```powershell
$env:L2_ESSENCE_ROOT = "D:\Lineage2-Essence"                                          # pasta com Animations\, SysTextures\, StaticMeshes\, system\
$env:L2_ESSENCE_CRYPT_XML = "D:\UGX_System\dat542\data\config\cryptVersion.xml"        # só se for decifrar .dat com o essence_dat.py
$env:CLOAK_DATA = "$PWD\work"                                                          # pasta de trabalho
New-Item -ItemType Directory -Force "$env:CLOAK_DATA\essence_dat" | Out-Null
```

**3a. Dados de itens do Essence** (`Armorgrp.txt` e `ItemName-eu.txt` em `work\essence_dat\`): descriptografe `armorgrp.dat` e `itemname-eu.dat` do cliente Essence (`source\tools\essence_dat.py` faz o RSA + zlib) e converta para texto com o **DatEditor CLI** do L2s (`DatEditorCli unpack <arquivo>.dat --out <arquivo>.txt`). Detalhes em `source\tools\essence_dat.py` e `essence_cloaks.py`.

**3b. Os três pacotes** (comandos completos em [BUILD.md](BUILD.md)):

```powershell
cd source\tools
$R = $env:L2_ESSENCE_ROOT
python -I build_capes.py "$R\Animations\LineageNewMantles.ukx" ..\..\work\essence_capes.bin `
  --utx "$R\SysTextures\LineageNewMantlesTex.utx" --utx "$R\SysTextures\LineageCustomTex8.utx" `
  --utx "$R\SysTextures\lineagecustomtex16.utx" --utx "$R\SysTextures\LineageCustomtex21.utx" `
  --aegis-ukx "$R\Animations\LineageCustom8.ukx" `
  --src "cust16=$R\Animations\LineageCustom16.ukx" --src "cust21=$R\Animations\LineageCustom21.ukx" `
  --bodies MFighter,FFighter,MMagic,FMagic,MElf,FElf,MDarkElf,FDarkElf,MDwarf,FDwarf,MOrc,FOrc,MShaman,FShaman `
  --designs 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
python -I build_cloth.py ..\..\work\essence_cloth_base.bin
python -I patch_crest_bg.py ..\..\work\essence_cloth_base.bin ..\..\work\essence_cloth.bin --scale 0.8 `
  --logo "^H_(grow|pvp|raid|eco)_1_t02=..\assets\logos\Twitch2.png" --logo "^H_(grow|pvp|raid|eco)_2_t02=..\assets\logos\Youtube.png" `
  --logo "^H_(grow|pvp|raid|eco)_3_t02=..\assets\logos\TIKTOK.png"  --logo "^H_(grow|pvp|raid|eco)_4_t02=..\assets\logos\KICK.png"
python -I build_fx.py ..\..\work\essence_fx.bin
cd ..\..
```

(`build_capes.py` demora e gera ~120 MB. Sem `--logo` as janelas das Clan Cloaks ficam só com tecido.)

## 4. Itens: servidor, ícones e `.dat` do cliente

* **Servidor Lucera:** use `server\lucera\data\items\9400-9499.xml` (ou gere de novo com `gen_items.py`, veja BUILD.md).
* **`.dat` do cliente** (nomes e aparência dos itens) — gere a partir dos `.dat` do **seu** cliente, não use os de outra pessoa:

```powershell
cd source\tools
python -I build_cloak_armorgrp.py --src "C:\Lineage II\System\armorgrp.dat" --ukx "C:\Lineage II\animations\LineShieldCloaks.ukx" --catalog ..\..\data\cloak_catalog.csv --out ..\..\work\armorgrp.dat
python -I build_cloak_itemname.py --src "C:\Lineage II\System\ItemName-e.dat" --catalog ..\..\data\cloak_catalog.csv --out ..\..\work\ItemName-e.dat
cd ..\..
```

* **Ícones** (opcional): `source\tools\make_icon_utx.py` (veja BUILD.md); sem eles os itens usam o ícone padrão.

## 5. Instalar no cliente

Feche o jogo e rode (o instalador faz backup de tudo):

```powershell
cd install
.\install.ps1 -Client "C:\Lineage II" -Packs "..\work"            # use -SystemFolder "System - Cloaks" se a sua pasta tiver outro nome
Copy-Item ..\work\armorgrp.dat, ..\work\ItemName-e.dat "C:\Lineage II\System\"   # faça backup dos originais antes
```

Desfazer: `.\install.ps1 -Client "C:\Lineage II" -Uninstall`.

## 6. Servidor

Copie `9400-9499.xml` para `dist/gameserver/data/items/` do Lucera e **reinicie o GameServer**. Entregue um item 9400–9466 a um personagem e equipe no slot de capa.

## 7. Testar e resolver problemas

Abra o jogo, entre no mundo e leia `System\cloakhook.log`:

| Linha do log | Significado / o que fazer |
|---|---|
| `hooks installed (...)` | o hook está ativo |
| `force-load LineShieldCloaks.<Corpo>_Cloak_9400 -> 0x...` | malha carrier carregada; `00000000` = `LineShieldCloaks.ukx` não foi achado em `animations\` |
| `RVA mismatch ... unsupported engine.dll` | seu `Engine.dll` é de outra build: o hook se recusa a ligar |
| `skeleton mismatch` | o esqueleto da capa não é prefixo do corpo |
| `exception while drawing cloak` | o hook se desliga para aquela capa; mande o log numa issue |

Teste rápido sem item nem servidor: no `cloakhook.ini` ponha `ForceId=9423` (desenha a Aden Cloak em todo mundo) e volte para `0` depois.

## 8. Ajustar

`System\cloakhook.ini` é relido ~1 s depois de salvar (chaves `Essence*`), sem reiniciar o jogo. Veja [CONFIG.md](CONFIG.md): peso e vento do tecido, posição das asas (`EssenceFxOffset`), alargamento do topo (`EssenceClothWiden`), etc. Para parar de gravar o log: `Log=0`.

## 9. Perguntas frequentes

* **Posso usar em outro cliente?** Só se o `Engine.dll` tiver as funções nos mesmos endereços; senão é preciso re-derivá-los (`kEngineChecks` em `cloakhook.cpp`).
* **E o brasão do clã?** O cliente devolve a textura do brasão vazia; por isso a janela mostra tecido liso ou os logos.
* **Posso trocar os logos?** Sim: ponha PNGs (de preferência com fundo transparente) e rode o `patch_crest_bg.py` com `--logo` (veja BUILD.md).
* **Como adiciono mais capas?** O catálogo (`data\cloak_catalog.csv`) e os packs definem o que existe; veja `HOW_IT_WORKS.md` e `CHANGELOG.md` para os limites atuais.

---

<a name="english"></a>
# English

Step by step from nothing to a cloak on your character. The first time takes an afternoon; once the packs exist (step 3) the rest is quick.

## 0. What you need

An **Interlude (C6) 32-bit client** from Lucera/L2BR whose `Engine.dll` the hook accepts; that client's `animations\LineShieldCloaks.ukx` (~294 MB, **not in this repository**); the original Microsoft `dsetup.dll` (~62 KB); a **Lineage II Essence client** (tested with 5.41/5.42 EU) to extract the content; Windows 10/11, PowerShell 5.1, Python 3.10+, Visual Studio 2022 (x86) and CMake; optionally a Lucera Interlude server.

## 1. Get the project

```powershell
git clone https://github.com/luannbr/Cloak_Essence_to_Interlude.git
cd Cloak_Essence_to_Interlude
pip install numpy pillow scipy
```

## 2. Build the DLL

```powershell
cd source
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

Result: `source\build\Release\dsetup.dll`.

## 3. Generate the data packs from **your** Essence client

The `essence_*.bin` files contain NCSoft content, so they are **not** in the repository. Set `L2_ESSENCE_ROOT` (the Essence folder with `Animations\`, `SysTextures\`, `StaticMeshes\`, `system\`), `L2_ESSENCE_CRYPT_XML` (the DatEditor's `cryptVersion.xml`, only for decrypting `.dat`) and `CLOAK_DATA` (a work folder).

3a. Item data: decrypt the Essence `armorgrp.dat` and `itemname-eu.dat` (`tools/essence_dat.py`), unpack them to text with the L2s **DatEditor CLI**, and save them as `work/essence_dat/Armorgrp.txt` and `ItemName-eu.txt`.

3b. Run `build_capes.py`, `build_cloth.py`, `patch_crest_bg.py` and `build_fx.py` exactly as listed in the Portuguese section above (full options in [BUILD.md](BUILD.md)).

## 4. Items

Server: `server/lucera/data/items/9400-9499.xml`. Client: build `armorgrp.dat` and `ItemName-e.dat` from **your** client's own `.dat` files with `build_cloak_armorgrp.py` / `build_cloak_itemname.py` (commands above); icons are optional (`make_icon_utx.py`).

## 5. Install into the client

Game closed:

```powershell
cd install
.\install.ps1 -Client "C:\Lineage II" -Packs "..\work"       # -SystemFolder "System - Cloaks" if your folder is named differently
```

Copy your generated `armorgrp.dat` / `ItemName-e.dat` into `System\` (back up the originals first). Undo with `-Uninstall`.

## 6. Server

Copy `9400-9499.xml` into `dist/gameserver/data/items/` (Lucera) and **restart the GameServer**; give a 9400–9466 item to a character and equip it in the cloak slot.

## 7. Test and troubleshoot

Read `System\cloakhook.log`: `hooks installed` = active; `force-load ... -> 00000000` = `LineShieldCloaks.ukx` missing from `animations\`; `RVA mismatch ... unsupported engine.dll` = another engine build, the hook stays off; `skeleton mismatch` = carrier skeleton is not a prefix of the body's; `exception while drawing cloak` = the hook disables that cloak (open an issue with the log). Quick test without items: `ForceId=9423` in `cloakhook.ini`, back to `0` afterwards.

## 8. Tune

`System\cloakhook.ini` is re-read about a second after saving (`Essence*` keys), no restart. See [CONFIG.md](CONFIG.md). `Log=0` stops logging.

## 9. FAQ

Other clients: only if their `Engine.dll` has the same function addresses (`kEngineChecks`). Clan crest: the client's crest texture is empty, so the window shows fabric or logos. Custom logos: transparent PNGs + `patch_crest_bg.py --logo`.
