# Cloak_Essence_to_Interlude

**PT-BR** | [English](#english)

Capas ao estilo **Lineage II Essence** num cliente **Interlude (C6)**: capas animadas, capas de tecido com física, golas rígidas, capas com asas e efeitos de partículas. É uma `dsetup.dll` (proxy de 32 bits) que desenha **só** as capas — sem anti-cheat, sem overlay, sem rede.

> Desenvolvido e testado no cliente/servidor **Lucera Interlude** (cliente L2BR). Veja as [limitações](#limitações) antes de usar em outro cliente.
> **Download pronto:** a [**Release v1.0**](https://github.com/luannbr/Cloak_Essence_to_Interlude/releases/tag/v1.0) traz a DLL, os pacotes de dados e o instalador — é só descompactar e rodar `install.ps1`.
> O repositório em si tem só o código; os dados do Essence (malhas, texturas, animações) você também pode **gerar do seu próprio cliente Essence** com `source/tools`. Passo a passo: **[docs/TUTORIAL.md](docs/TUTORIAL.md)**.

## O que tem aqui

```
├── docs/            TUTORIAL.md (passo a passo), BUILD.md, CONFIG.md, HOW_IT_WORKS.md, CATALOG.md, THIRD_PARTY.md, CHANGELOG.md
├── install/         install.ps1 (instalador com backup e -Uninstall) e o cloakhook.ini padrão
├── server/lucera/   data/items/9400-9499.xml  (itens 9400-9466, slot BACK)
├── source/          src/ (hook C++), tests/ (diagnóstico C++), tools/ (Python: pacotes UE2, geradores dos .bin / .dat / ícones), assets/logos/
└── data/            cloak_catalog.csv (id, nome e ícone de cada item)
```

## Os itens (ids 9400–9466)

* **9400–9409** – 10 capas animadas (mantos "assados" do Essence).
* **9410–9460** – 51 capas de tecido com física e gola (Festival, Zaken, Freya, Frintezza, Elmoreden/Aden/Elmore/Ferios e as Legendary, Abelius…Lakcis, Hero, Glory, Exalted, Conquest e as **Clan Cloaks**).
* **9461–9466** – capas com asas (Valakas' Wings, Death Knight, Eigis, Light Dragon imbuída).
* As **Clan Cloaks War / Growth / Combat / Economy (Lv. 1–4)** mostram na janela do brasão os logos **Twitch, YouTube, TikTok e Kick** (nível 1 a 4).
* Efeitos de partículas (asas, raios, brilhos) em vários itens.

Tabela completa: [docs/CATALOG.md](docs/CATALOG.md).

## Início rápido

**Opção A — arquivos prontos (mais fácil):** baixe `Cloak_Essence_to_Interlude_client_files_v1.0.zip` na [Release v1.0](https://github.com/luannbr/Cloak_Essence_to_Interlude/releases/tag/v1.0), descompacte e rode `.\install.ps1 -Client "C:\Lineage II"` (jogo fechado). No servidor Lucera, copie o XML de itens que vem no zip. Para o cliente Lucera/L2BR há também o zip com os `.dat` (`-WithDats -DatsFolder <pasta>`).

**Opção B — gerar tudo você mesmo:**

1. **Compile a DLL:** `cd source; cmake -S . -B build -G "Visual Studio 17 2022" -A Win32; cmake --build build --config Release`
2. **Gere os pacotes** `essence_capes.bin`, `essence_cloth.bin`, `essence_fx.bin` do seu cliente Essence ([TUTORIAL, passo 3](docs/TUTORIAL.md)).
3. **Instale** (jogo fechado): `install\install.ps1 -Client "C:\Lineage II" -Packs "C:\caminho\dos\bin"` — faz backup de tudo; `-Uninstall` desfaz.
4. **Servidor Lucera:** copie `server/lucera/data/items/9400-9499.xml` para `dist/gameserver/data/items/` e reinicie o GameServer.
5. Gere o `armorgrp.dat` / `ItemName-e.dat` do **seu** cliente com `source/tools/build_cloak_armorgrp.py` e `build_cloak_itemname.py`.

## Requisitos

* Cliente Lineage II **Interlude** de 32 bits cujo `Engine.dll` tenha as funções nos endereços (RVA) esperados — o hook confere e **se recusa a ligar** se forem outros (o log diz `unsupported engine.dll`). Builds conhecidos: o cliente L2BR/Lucera.
* `animations\LineShieldCloaks.ukx` do cliente L2BR/LineShield (de terceiros): fornece as malhas "carrier" `<Corpo>_Cloak_<id>` que fazem a engine liberar o slot de capa. Quem usa o cliente L2BR/Lucera já o tem; para os outros, o arquivo original (sem alteração) está na [Release v1.0](https://github.com/luannbr/Cloak_Essence_to_Interlude/releases/tag/v1.0) como `LineShieldCloaks_carrier_package_v1.0.zip`.
* A `dsetup.dll` **original da Microsoft** (≈ 62 KB): o instalador a guarda como `dsetup_ms.dll`.
* Um cliente **Essence** (testado com 5.41/5.42 EU) para extrair o conteúdo.
* Servidor: itens no slot **BACK** (paperdoll 13); o XML é para o **Lucera**.
* Windows, PowerShell 5.1, Python 3.10+ (`numpy`, `pillow`, `scipy`), Visual Studio 2022 (x86) e CMake.

## Configuração

`System\cloakhook.ini` — todas as chaves em [docs/CONFIG.md](docs/CONFIG.md). As chaves `Essence*` são relidas ~1 s depois de salvar, **sem reiniciar o jogo**: `EssenceClothWiden`, `EssenceClothTopRamp`, `EssenceFxOffset`/`EssenceFxClothOffset`, `EssenceFxWingFlip`, `Log`, `Debug`...

## Limitações

* **Só testado** num cliente/servidor Lucera Interlude. Outro `Engine.dll` exige re-derivar os endereços (`kEngineChecks` em `cloakhook.cpp`).
* O **brasão real do clã não aparece**: o cliente devolve a textura do brasão vazia; as janelas das Clan Cloaks mostram tecido liso ou os logos.
* Itens do Essence sem tabela de efeito no cliente (Sayha's Special, Heavenly 72514) ficam sem partículas; Radiant Light / Cold Darkness (34996/34997) e 47917 não estão no catálogo.
* Direct3D 9 de pipeline fixo, 32 bits.
* O proxy `dsetup.dll` **substitui** a `dsetup.dll` do cliente (no L2BR, a do LineShield). Servidores com anti-cheat próprio podem recusar o cliente modificado.

## Documentação

[TUTORIAL](docs/TUTORIAL.md) · [BUILD](docs/BUILD.md) · [CONFIG](docs/CONFIG.md) · [HOW_IT_WORKS](docs/HOW_IT_WORKS.md) · [CATALOG](docs/CATALOG.md) · [THIRD_PARTY](docs/THIRD_PARTY.md) · [CHANGELOG](docs/CHANGELOG.md)

## Créditos e avisos

Usa [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause), baixado pelo CMake. **Projeto de fã, sem vínculo com a NCSoft, o L2BR/LineShield nem o Lucera.** "Lineage II" é marca da NCSoft; os logos Twitch/YouTube/TikTok/Kick pertencem aos seus donos. Veja [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md). Licença: a definir pelo autor.

---

<a name="english"></a>
# English

**Essence-style cloaks for a Lineage II Interlude (C6) client**: animated mantles, cloth-simulated cloaks with rigid collars, wing cloaks and particle effects. A 32-bit proxy `dsetup.dll` that draws **only** cloaks — no anti-cheat, no overlay, no networking.

> Built and tested on the **Lucera Interlude** client/server (L2BR client). Read the [limitations](#limitations-1) before using another client.
> **Ready-made download:** the [**v1.0 release**](https://github.com/luannbr/Cloak_Essence_to_Interlude/releases/tag/v1.0) has the DLL, the data packs and the installer — unzip and run `install.ps1`.
> The repository itself holds the code only; you can also **generate the Essence data from your own Essence client** with `source/tools`. Step by step: **[docs/TUTORIAL.md](docs/TUTORIAL.md)**.

## Layout

`docs/` (tutorial, build, config, how it works, catalog, third-party notice, changelog), `install/` (installer + default `cloakhook.ini`), `server/lucera/` (item XML 9400–9466, slot BACK), `source/` (C++ hook, C++ tests, Python tools, logos), `data/cloak_catalog.csv`.

## Items

10 animated mantles (9400–9409), 51 cloth cloaks with collars (9410–9460, all Clan Cloaks included), 6 wing cloaks (9461–9466). The War / Growth / Combat / Economy Clan Cloaks (levels 1–4) show the Twitch / YouTube / TikTok / Kick logos in their crest window. [docs/CATALOG.md](docs/CATALOG.md).

## Quick start

**Option A — ready-made files (easiest):** download `Cloak_Essence_to_Interlude_client_files_v1.0.zip` from the [v1.0 release](https://github.com/luannbr/Cloak_Essence_to_Interlude/releases/tag/v1.0), unzip it and run `.\install.ps1 -Client "C:\Lineage II"` (game closed). On a Lucera server copy the item XML from the zip. For the Lucera/L2BR client there is also a zip with the `.dat` files (`-WithDats -DatsFolder <folder>`).

**Option B — generate everything yourself:**

1. Build the DLL: `cd source; cmake -S . -B build -G "Visual Studio 17 2022" -A Win32; cmake --build build --config Release`
2. Generate `essence_capes.bin`, `essence_cloth.bin`, `essence_fx.bin` from your Essence client ([TUTORIAL, step 3](docs/TUTORIAL.md)).
3. Install (game closed): `install\install.ps1 -Client "C:\Lineage II" -Packs "C:\path\to\bins"` — backs everything up; `-Uninstall` undoes it.
4. Lucera server: copy `server/lucera/data/items/9400-9499.xml` to `dist/gameserver/data/items/` and restart the GameServer.
5. Build `armorgrp.dat` / `ItemName-e.dat` from **your** client with `source/tools/build_cloak_armorgrp.py` and `build_cloak_itemname.py`.

## Requirements

A 32-bit Interlude client whose `Engine.dll` exports sit at the expected RVAs (the hook checks and refuses to start otherwise); `animations\LineShieldCloaks.ukx` from the L2BR/LineShield client (third party; already present in the L2BR/Lucera client, otherwise the unmodified file is in the [v1.0 release](https://github.com/luannbr/Cloak_Essence_to_Interlude/releases/tag/v1.0) as `LineShieldCloaks_carrier_package_v1.0.zip`; it provides the `<Body>_Cloak_<id>` carrier meshes); the original Microsoft `dsetup.dll` (~62 KB); an Essence client (tested with 5.41/5.42 EU); a Lucera server (BACK slot); Windows, PowerShell 5.1, Python 3.10+ (`numpy`, `pillow`, `scipy`), Visual Studio 2022 (x86), CMake.

## Configuration

`System\cloakhook.ini`, every key in [docs/CONFIG.md](docs/CONFIG.md); `Essence*` keys are re-read about a second after saving, no restart.

## Limitations

Tested only on a Lucera Interlude client/server; the real clan crest does not show (the client's crest texture is empty); no effect tables for Sayha's Special and Heavenly 72514; Direct3D 9 fixed-function, 32-bit; the proxy replaces the client's `dsetup.dll` (on L2BR, LineShield's), so servers with their own anti-cheat may refuse the client.

## Credits and notices

Uses [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause), fetched by CMake. **Fan project, not affiliated with NCSoft, L2BR/LineShield or Lucera.** "Lineage II" is a trademark of NCSoft; the Twitch/YouTube/TikTok/Kick logos belong to their owners. See [docs/THIRD_PARTY.md](docs/THIRD_PARTY.md). License: to be defined by the author.
