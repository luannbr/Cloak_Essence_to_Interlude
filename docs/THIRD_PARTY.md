# Third-party content and notices / Conteúdo de terceiros e avisos

**PT-BR** | [English below](#english)

Este é um **projeto de fã, sem vínculo com a NCSoft, com o L2BR/LineShield nem com o Lucera**. "Lineage II" é marca da NCSoft.

## O que **não** está na árvore do repositório

Estes arquivos não são versionados no Git, mas **os pacotes de dados, os ícones, a DLL compilada e os `.dat` do cliente Lucera/L2BR foram publicados como arquivos da [Release v1.0](https://github.com/luannbr/Cloak_Essence_to_Interlude/releases/tag/v1.0)** por decisão do autor, para facilitar o uso. Eles contêm dados convertidos do cliente Essence (NCSoft) e, no caso dos `.dat`, dados do cliente Lucera/L2BR. Se você é titular de direitos sobre algum desses conteúdos e quer a remoção, abra uma issue.

| Item | Por quê | Como obter |
|---|---|---|
| `essence_capes.bin`, `essence_cloth.bin`, `essence_fx.bin` | contêm malhas, animações, texturas e parâmetros **extraídos do cliente Lineage II Essence (NCSoft)** | gere do seu cliente Essence: [TUTORIAL.md](TUTORIAL.md), passo 3 |
| `cloakicons.utx` | ícones de itens do Essence | `source/tools/make_icon_utx.py` |
| `armorgrp.dat`, `ItemName-e.dat` | derivados dos `.dat` do seu cliente (contêm os dados de itens do jogo inteiro) | `build_cloak_armorgrp.py` / `build_cloak_itemname.py` |
| `animations/LineShieldCloaks.ukx` (≈ 294 MB) | pacote do **L2BR/LineShield** | do seu próprio cliente |
| `dsetup.dll` compilada | binário | compile com o CMake ([BUILD.md](BUILD.md)) |

## O que está aqui

* O **código-fonte** (`source/src`, `tests`, `tools`) e a documentação, escritos para este projeto. **A licença ainda não foi definida pelo autor.**
* `server/lucera/data/items/9400-9499.xml`: definições de itens geradas por `gen_items.py` (usam nomes de itens do Essence).
* `source/assets/logos/*.png`: logos de **Twitch, YouTube, TikTok e Kick** — marcas dos respectivos donos, usadas só para decorar a janela das Clan Cloaks. Remova se preferir.
* **MinHook** (https://github.com/TsudaKageyu/minhook, BSD-2-Clause): baixado pelo CMake na compilação, não incluído. A DLL compilada o liga estaticamente; se você a redistribuir, inclua o aviso de copyright do MinHook (texto completo em `source/third_party_licenses/MinHook-LICENSE.txt`).


---

<a name="english"></a>
# English

This is a **fan project, not affiliated with NCSoft, L2BR/LineShield or Lucera**. "Lineage II" is a trademark of NCSoft.

**Not in the repository tree** (but published as files of the [v1.0 release](https://github.com/luannbr/Cloak_Essence_to_Interlude/releases/tag/v1.0) by the author's decision, to make it easy to use; they contain data converted from the Essence client (NCSoft) and, for the `.dat` files, data from the Lucera/L2BR client; if you hold rights over any of it and want it removed, open an issue): `essence_*.bin` (they contain meshes, animations, textures and parameters extracted from the Lineage II Essence client — generate them from your own client, [TUTORIAL.md](TUTORIAL.md) step 3); `cloakicons.utx` (Essence icons, `make_icon_utx.py`); `armorgrp.dat` / `ItemName-e.dat` (derived from your client's dats; use the builders); `animations/LineShieldCloaks.ukx` (~294 MB, L2BR/LineShield package, take it from your own client); the compiled `dsetup.dll` (build it with CMake).

**In the repository:** the source code and documentation written for this project (**license not chosen by the author yet**); the Lucera item XML; the Twitch / YouTube / TikTok / Kick logos (trademarks of their owners, only used to decorate the Clan Cloak crest window — remove them if you prefer); MinHook (BSD-2-Clause) is fetched by CMake and statically linked into the DLL: if you redistribute a built DLL, include MinHook's copyright notice (full text in `source/third_party_licenses/MinHook-LICENSE.txt`).
