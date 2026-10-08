# How it works

A technical overview for people who want to understand, port or extend the cloak system. Everything below is what the code in `source/` does.

## 1. The proxy DLL

`L2.exe` imports `DSETUP.dll` by ordinal 11 (`DirectXSetupGetVersion`). Windows loads it from the `System` folder, so our `dsetup.dll` is loaded first. It exports ordinal 11 as a forwarder to the original Microsoft DLL, kept next to it as `dsetup_ms.dll`, and starts a worker that waits (up to 30 s) for `Engine.dll` and `Core.dll`.

Before touching anything it checks that six exports of `Engine.dll` sit at the RVAs it was written for (`kEngineChecks` in `cloakhook.cpp`): `USkeletalMeshInstance::Render`, `USkeletalMeshInstance::DrawSection`, `USubSkeletalMeshInstance::Render`, `APawn::GetCloakMesh`, `UMesh::CloakMeshGetInstance`, `USubSkeletalMeshInstance::SetSubMeshIndex`, plus the `USubSkeletalMeshInstance` vtable. A different `Engine.dll` build makes it log `RVA mismatch ... unsupported engine.dll` and stay out of the way.

Two detours (MinHook) are installed on the engine: `USkeletalMeshInstance::Render` (stores the render context) and `DrawSection` (before the first body section it draws the cloak with the engine's own sub-mesh renderer, using the character's skeleton and pose). The Direct3D 9 device is hooked at `IDirect3D9::CreateDevice` by patching the device's vtable (`DrawIndexedPrimitive`, `DrawIndexedPrimitiveUP`, stream/FVF setters, ...) so the cloak's draw calls can be replaced by our own geometry.

## 2. The "carrier" mesh

The Interlude engine already has a cloak path (`PMS_Cloak`, `CloakSkins`, `GetCloakMesh`) that nothing uses. A mesh is accepted as a cloak when its full name is `SkeletalMesh <Package>[_NN].<Body>_Cloak_<id>` (`<Body>` = MFighter FFighter MMagic FMagic MElf FElf MDarkElf FDarkElf MDwarf FDwarf MOrc FOrc MShaman FShaman), the id is inside `IdMin..IdMax` (9400–9466) and the cloak's bone names are an identical **prefix** of the body skeleton's (at least 20 bones). `armorgrp.dat` maps each item id to `LineShieldCloaks.<Body>_Cloak_<id>` for all 14 bodies, so equipping the item (slot BACK, paperdoll 13) makes the engine hand that mesh to the hook. The mesh itself is only a **carrier**: the hook does not draw its geometry, it draws the Essence cloak that belongs to the item id.

## 3. Data packs

Built offline from an Essence client with `source/tools` (see BUILD.md); all little-endian.

| File | Magic | Content |
|---|---|---|
| `essence_capes.bin` | `ECP1` | 14 body skeletons, the baked animation sequences (quaternions stored already conjugated for the children, root raw), the mantle meshes (designs 0–15: the animated mantles, ranker mantles, Valakas wing, Death Knight cloaks, Eigis), DXT textures, `FXS1` shine layers |
| `essence_cloth.bin` | `ECL2` | cloth sets per body and kind (H heavy 84 particles, R robe 91, C clan 98: anchors, springs, capsule colliders by bone name), rigid collars, the "looks" (cloth texture, collar texture, crest-window background, item id), shine layers |
| `essence_fx.bin` | `EFX3` | particle effects of the Essence cloaks: sprite, static-mesh and vertex-animated-mesh emitters with all parameters, their textures and meshes (the wings are the 70-frame `wing_high` vertex mesh) |

## 4. Animated mantles (items 9400–9409)

The hook reads the pawn's current animation channels from the body mesh instance (sequence name and frame), maps them to the pack's baked sequences by name, blends, skins the mantle on the CPU every frame and draws it once with its own vertex/index buffers (fixed-function, DXT textures). The mantle root follows the real `Spine2` bone of the pawn. A small spring "sway" lets the hem lag the torso.

## 5. Cloth cloaks (items 9410–9460)

* **Cloth**: a Verlet / position-based solver (`src/cloth.h`) at a fixed 90 Hz step. Gravity plus a scripted wind per action class (idle / walk / run / sit / attack), spring constraints, capsule colliders built from the body skeleton (torso capsule that always pushes the cloth to the back, legs, a pelvis capsule only while sitting). The first rows (anchors) are pinned to the torso.
* **Torso frame**: the cloak is pinned in a frame built from the upper-arm, neck and pelvis bones, so it is centred on the back on every race.
* **Guide** (`src/clothguide.h`): while moving, each particle is pulled towards the deformation of the baked cape of the same body, which keeps the cloak flat on the back when running.
* **Collar** (`Collar`): the rigid shoulder/collar piece (a `<Body>_<family>` skeletal mesh) is drawn with the same rigid transform as the anchors.
* **Widening** (`src/clothwiden.h`): the cloth is cut narrower at the top than its collar; the drawn cloth is stretched sideways (measured on the simulated cloth) so the top meets the collar and the sides run straight. The torso collider is thinned right under the pinned rows (`cloth::CapsuleWorld::rTop/zRef/ramp`) to avoid a step.
* **Shine**: the Essence material is an environment-map layer (`jihoon_gold`, camera-space normal) multiplied by the cloth's own colour; the hook draws it as an extra additive pass. The same pass is drawn on the clan cloaks' crest window with the window's own picture as the mask.
* **Clan cloaks**: the "window" is a separate section of the mesh (`sec1`). Its background texture is replaced by the logos (see `source/tools/patch_crest_bg.py`). The real clan crest cannot be shown: `UNetworkHandler::GetPledgeCrestTex` is hooked and does return a texture object, but it is an empty header without pixel data in the tested client.

## 6. Wings and effects

The wings of the Essence cloaks are not part of the cloak mesh: they are **particle effects** (`LineageEffect*.u` emitter classes) attached to the item through `EnchantedCloakEffectData.dat`. `src/fx.h` simulates the emitters (spawn rate, lifetime, location shapes, radial velocity, drag, size/colour over life, spin, revolution, sprite orientation modes, sprite sheets, DrawStyle blend modes) and gathers camera-facing sprites, meshes and the vertex-animated wing mesh (frame = particle age) into batches that the hook draws after the cloak with additive / alpha blending. The effect origin follows the torso (centroid of the cloth anchors plus `EssenceFxClothOffset`, or `Spine2` plus `EssenceFxOffset` for mantles); the item → effect map is `EssenceFxMap`.

### Mounts

When the body plays `strider_*`, `wyvern_*`, `pet_*` or `ride*` the sequence says nothing about how fast the mount moves, so the hook uses the speed of the character: the sway state estimated from the cloak's `MeshToWorld` matrix (position difference per frame, low-pass filtered, kept by `StepSway`) is read on the next frame in `DrawCloak` (`t_rideSpeed`) and mapped to the idle / walk / run class (< 25, < 140, above, in units per second). The cloth wind is interpolated between the idle and run values; the baked-cape guide is off; seated rides (not the wyvern) add the pelvis collider used when sitting.

## 7. Item side

* `data/cloak_catalog.csv` lists id, design and icon.
* `tools/gen_items.py` writes the Lucera item XML (slot BACK); `tools/build_cloak_armorgrp.py` and `tools/build_cloak_itemname.py` add the items to a client's `armorgrp.dat` / `ItemName-e.dat` (Lineage2Ver413 layout, RSA + zlib, via `tools/l2dat.py`); `tools/make_icon_utx.py` builds `cloakicons.utx`.

## 8. Reading the log

`cloakhook.log` (next to the DLL): `hooks installed (...)` = the hook is active; `force-load ... -> 0x...` = carrier mesh loaded (`00000000` = package not found); `skeleton mismatch` = carrier skeleton is not a prefix of the body's; `RVA mismatch ... unsupported engine.dll` = other engine build; `exception while drawing cloak` = the hook disables itself for that cloak; every 5 s `essence stats:` / `essence fx:` counters; with `Debug=1`, per-pawn cloth information and, after 4 s standing still, an `IDLE` dump of the cloth shape.

## 9. Diagnostics tools (`source/tests`, C++)

`cloth_real` (cloth on the real baked root motion), `guide_test`, `fx_test` / `fx_dump` (effects; `source/tools/fx_preview.py` renders a moment of one effect), `shape_dump` + `tools/shape_plot.py` (cloth + collar from the back and side), `widen_dump`, `tex_dump`, `fx_layers`, `mantle_check` (validates every packed mantle), `essence_test`.
