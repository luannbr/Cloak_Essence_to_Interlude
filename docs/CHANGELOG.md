# Changelog

The project grew in short "rounds" (build, install, look in game, adjust). Dates are 2026-10-06 to 2026-10-08.

## Foundations (2026-10-06)

* Analysis of how the L2BR/LineShield client draws cloaks on Interlude: the `dsetup.dll` proxy, the engine's existing but unused cloak path, the `<Body>_Cloak_<id>` carrier meshes, the skeleton-prefix rule, slot BACK (paperdoll 13).
* First own cloak-only proxy (`cloakhook`): two MinHook detours + Direct3D 9 device hooks; test mode `ForceId`.
* Rigid-cloak experiments (matrix sway, vertex-buffer deformation, spring cloth on the carrier) — kept as legacy keys in the ini.

## Essence content (2026-10-07)

* Offline readers for the Essence UE2 packages (`l2pkg.py`, `essence*.py`): meshes, skeletons, baked animations, DXT textures, material graphs, simulation meshes; decryption of the Lineage2Ver413 `.dat` files.
* **Animated mantles** (`essence_capes.bin`): CPU skinning on the pawn's own animation, 14 bodies, designs 0–15, hem sway, shine layers.
* **Cloth cloaks** (`essence_cloth.bin`): position-based cloth with capsule colliders, wind per action, rigid collars, clan cloaks, baked-cape guide while running, item catalog (ids 9400–9466), icons, item XML / `armorgrp.dat` / `ItemName-e.dat` generators.
* Robustness fixes: per-pawn animation state keyed by animation set; a failing cloak id is blacklisted instead of disabling everything; pack validation tool (`mantle_check`).
* Torso frame from the arm/neck/pelvis bones: cloth cloaks centred on the back instead of sitting on the right shoulder.

## Effects and polish (2026-10-07 .. 2026-10-08)

* The wings of the Essence cloaks turned out to be **particle effects**: pack `essence_fx.bin`, emitter simulation (`fx.h`), vertex-animated wing mesh with a panning second texture, item → effect map, offsets and orientation settings in the ini.
* Cloth top: stretched to the collar width (`EssenceClothWiden`) and a thinner torso collider under the pinned rows (`EssenceClothTopRamp`) to remove the step / "contraction" under the shoulders.
* Clan cloaks: the crest window gets the cloth's shine, a fabric-matched background and, for War / Growth / Combat / Economy levels 1–4, the Twitch / YouTube / TikTok / Kick logos. The real clan crest is not available in the tested client.
* Public package: tools no longer contain machine specific paths (environment variables instead), installer with backups and uninstall, generated configuration reference.

## Known open items

* Real clan crest in the window (the client's crest texture object is empty).
* Effects for Sayha's Special (91719–91721) and Heavenly Cloak (72514); items 34996 / 34997 / 47917 are not in the catalog (all 67 slots are used).
* Only a Lucera Interlude client / server was used for testing.
