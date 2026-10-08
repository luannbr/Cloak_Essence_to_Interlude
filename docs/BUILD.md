# Building and regenerating the data

## 1. The DLL (C++)

Requirements: Visual Studio 2022 with the C++ x86 tools, CMake 3.16+, internet access for the first configure (MinHook v1.3.4 is fetched with `FetchContent`).

```powershell
cd source
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release          # -> build\Release\dsetup.dll
```

The client is 32-bit: the project refuses to configure for x64. The static CRT is used, so the DLL needs no runtime next to the game. Install a DLL you built with `source\deploy.ps1 -Target "C:\Lineage II\System"` (keeps the original as `dsetup_ms.dll`; the packs and `cloakhook.ini` go next to it) or copy it over the one installed by `install\install.ps1`.

The diagnostics in `source/tests/*.cpp` are single-file programs, for example:

```powershell
cl /nologo /std:c++17 /EHsc /O2 /D_CRT_SECURE_NO_WARNINGS tests\fx_test.cpp /Fe:fx_test.exe     # from a "x86 Native Tools" prompt
```

## 2. Python tools

Python 3.10+ with `numpy` and `Pillow`; `scipy` is needed only for the wing vertex meshes (`essence_vertmesh.py`), `pefile` / `capstone` only for `re_tools.py`. Run them with `python -I` from the `source/tools` folder (they add their own folder to `sys.path`).

Environment variables (the sources no longer contain any machine specific path):

| Variable | Meaning |
|---|---|
| `L2_ESSENCE_ROOT` | root of a **Lineage II Essence client** (the folder with `Animations\`, `SysTextures\`, `StaticMeshes\`, `system\`) |
| `L2_ESSENCE_CRYPT_XML` | `cryptVersion.xml` of the L2s DatEditor (`...\UGX_System\dat542\data\config\cryptVersion.xml`), used by `essence_dat.py` to decrypt Lineage2Ver413 `.dat` files |
| `CLOAK_DATA` | folder with the working data (default `source\..\data`; `essence_dat\` inside it holds `Armorgrp.txt` and `ItemName-eu.txt` unpacked from the Essence client) |

`tools/l2pkg.py` reads the UE2 packages (Ver111 / Ver121, per-file XOR key); the rest of the tools sit on top of it.

## 3. Regenerating the packs

The command lines used for the packs (paths shortened; `$R` = `L2_ESSENCE_ROOT`):

```powershell
# mantles: essence_capes.bin (ECP1)
python -I build_capes.py "$R\Animations\LineageNewMantles.ukx" essence_capes.bin `
  --utx "$R\SysTextures\LineageNewMantlesTex.utx" --utx "$R\SysTextures\LineageCustomTex8.utx" `
  --utx "$R\SysTextures\lineagecustomtex16.utx" --utx "$R\SysTextures\LineageCustomtex21.utx" `
  --aegis-ukx "$R\Animations\LineageCustom8.ukx" `
  --src "cust16=$R\Animations\LineageCustom16.ukx" --src "cust21=$R\Animations\LineageCustom21.ukx" `
  --bodies MFighter,FFighter,MMagic,FMagic,MElf,FElf,MDarkElf,FDarkElf,MDwarf,FDwarf,MOrc,FOrc,MShaman,FShaman `
  --designs 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15          # 121 MB

# cloth cloaks: essence_cloth.bin (ECL2) - needs CLOAK_DATA\essence_dat\Armorgrp.txt + ItemName-eu.txt
python -I build_cloth.py essence_cloth_base.bin

# crest-window backgrounds + logos (rewrites only the *_t02_bg_tint textures of the cloth pack)
python -I patch_crest_bg.py essence_cloth_base.bin essence_cloth.bin --scale 0.8 `
  --logo "^H_(grow|pvp|raid|eco)_1_t02=..\assets\logos\Twitch2.png" `
  --logo "^H_(grow|pvp|raid|eco)_2_t02=..\assets\logos\Youtube.png" `
  --logo "^H_(grow|pvp|raid|eco)_3_t02=..\assets\logos\TIKTOK.png" `
  --logo "^H_(grow|pvp|raid|eco)_4_t02=..\assets\logos\KICK.png"
#   without --logo: pvp and eco windows become plain fabric (taken from the cloth texture), growth and combat get the colour-matched Essence background

# particle effects: essence_fx.bin (EFX3)
python -I build_fx.py essence_fx.bin
```

`build_capes.py --help`, `build_cloth.py` and `build_fx.py` (docstrings) describe the options and the file layouts.

## 4. Items, icons and client `.dat` files

```powershell
# catalog (ids 9400-9409 mantles, 9410.. cloth looks with the official Essence names and icons)
python -I make_catalog.py essence_cloth.bin --out ..\data\cloak_catalog.csv

# icons: needs the client's systextures\icon.utx as template and the icon PNGs; one --icon name=file.png per catalog row
python -I make_icon_utx.py --template "C:\Lineage II\systextures\icon.utx" --out cloakicons.utx --icon cloak_00=a.png --icon cloak_01=b.png ...

# server (Lucera) item XML, slot BACK
python -I gen_items.py 9400-9499.xml --slot BACK --catalog ..\data\cloak_catalog.csv

# client dats: take the CURRENT dats of your client as --src
python -I build_cloak_armorgrp.py --src "C:\Lineage II\System\armorgrp.dat" --ukx "C:\Lineage II\animations\LineShieldCloaks.ukx" --catalog ..\data\cloak_catalog.csv --out armorgrp.dat
python -I build_cloak_itemname.py --src "C:\Lineage II\System\ItemName-e.dat" --catalog ..\data\cloak_catalog.csv --out ItemName-e.dat
```

The icon PNGs and the Essence `.dat` files are not part of this repository (they come from the Essence client).

## 5. Inspecting packages

`essence_collar.py`, `essence_sim.py`, `essence_static.py`, `essence_vertmesh.py`, `fx_program.py`, `ue_props.py` decode the Essence meshes, simulation meshes, static meshes, vertex-animated meshes, material graphs and tagged properties; `essence_gallery.py` / `essence_preview*.py` render previews. The scripts in `tools/research/` are one-off exploration scripts kept for reference (two of them need a helper module, `bones`, that was not kept).
