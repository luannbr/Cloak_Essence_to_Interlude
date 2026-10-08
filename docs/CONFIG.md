# cloakhook.ini reference / Referência do cloakhook.ini

Generated from the source (`source/src/cloakhook.cpp`). The file lives next to `dsetup.dll`.
Every `Essence*` key is **re-read within a second of saving the file while the game runs** (the pack file names are only used when a pack is loaded); the basics and the legacy keys are read when the game starts.
Gerado a partir do código-fonte. O arquivo fica ao lado da `dsetup.dll`. Todas as chaves `Essence*` são relidas até 1 segundo depois de salvar o arquivo, com o jogo aberto; as chaves básicas e as legadas só são lidas ao iniciar o jogo.

**Default** = value used when the key is missing from the ini. The shipped `cloakhook.ini` sets the recommended values (for example `Essence=1`).

Values: numbers or `a,b,c` lists (game units; "left/front/up" in the effect offsets are the character's own axes).

## Basics

| Key | Default | What it does |
|---|---|---|
| `ConstProbe` | `0` | debug: log the vertex-shader constants set while the cloak is drawn (GPU skinning) |
| `Debug` | `0` | verbose, rate-limited tracing of every stage |
| `Log` | `1` | write cloakhook.log next to the dll |
| `MatSway` | `0` | swing the whole cloak (rigid sections: one matrix) around its top edge |
| `ScanIds` | `0` | debug: find where the client keeps the equipped item ids |

## Essence cloaks (animated mantles, cloth cloaks, wings, effects)

| Key | Default | What it does |
|---|---|---|
| `Essence` | `0` | master switch of the Essence cloaks (baked mantles, cloth cloaks, wings, effects); the shipped ini sets 1 |
| `EssenceAttach` | `1` | the mantle root follows the pawn's real Spine2 bone (read from the engine) instead of the baked Essence body |
| `EssenceBlend` | `0.2` | seconds to blend between two animation sequences (only when the engine does not tween itself) |
| `EssenceBright` | `0.85` | unlit: texture x this factor |
| `EssenceCloth` | `1` | enable the simulated cloth cloaks (needs `EssenceClothPack`) |
| `EssenceClothBase` | `10` | (item id - FirstId) below this = animated mantle designs, from here on = look (rel - base) of the cloth pack |
| `EssenceClothDamp` | `2.8` | velocity damping per second |
| `EssenceClothDump` | `1` | debug: dump every particle and collider of the first running / sitting samples (torso angle > 55 deg) to the log |
| `EssenceClothFollow` | `0.6` | fraction of the torso rotation (since the previous frame) that the whole cloth follows |
| `EssenceClothGravity` | `70` | units/s^2 (the Essence notifies carry (0, -10..-45, -50)) |
| `EssenceClothGuide` | `0,0.10,0.16,0,0.10` | pull of the cloth towards the baked cape deformation (per 1/90 s step) while idle, walking, running, sitting, attacking |
| `EssenceClothInelastic` | `1` | contacts keep the pre-contact velocity (a moving collider cannot launch the cloth) |
| `EssenceClothIter` | `8` | solver iterations per 1/90 s step |
| `EssenceClothMaxStep` | `1.0` | cap on a particle's displacement per 1/90 s step (units): 1.0 = 90 units/s |
| `EssenceClothPack` | `essence_cloth.bin` | file name of the cloth pack (cloth sets, collars, looks, crest backgrounds) |
| `EssenceClothPelvisSit` | `1` | a pelvis capsule between the thighs while sitting (the cloak rests on the seat behind, never through it) |
| `EssenceClothRef` | `1` | pin relative to the pawn's own idle Spine2 orientation (cancels any axis-mapping error) instead of the Essence bind |
| `EssenceClothSkin` | `0.35` | extra radius around the body capsules |
| `EssenceClothSnug` | `0` | moves the pinned top towards the chest (+Y), units |
| `EssenceClothStiff` | `0.85` | spring stiffness per iteration (Essence 0.6 .. 0.85) |
| `EssenceClothTopRamp` | `9` | height over which the torso collider grows from the thickness of the pinned rows to its full radius (0 = plain capsule) |
| `EssenceClothTorso` | `1` | orient the cloak by the real torso (shoulder line + spine) instead of assuming the idle pose faces straight ahead |
| `EssenceClothWiden` | `1` | how far the top of the cloth is widened towards the width of its collar (0 = as cut, 1 = to the collar), drawn only |
| `EssenceClothWidth` | `0.08` | pull that keeps the cloak as wide as its cut (0 = off) |
| `EssenceClothWind` | `-9,-20,-36,0,-26` | backwards acceleration while idle, walking, running, sitting, attacking |
| `EssenceCollar` | `1` | draw the rigid collar / shoulder piece of the cloth cloaks |
| `EssenceComboLook` | `11` | design 16 (Ranker wings) is worn over this cloth look: 11 = Cloak of Darkness for the Chosen |
| `EssenceCrest` | `1` | draw the clan's own crest in the window of the clan cloaks (UNetworkHandler::GetPledgeCrestTex) |
| `EssenceDesignMap` | `0,1,2,3,4,5,6,7,8,9` | pack design ids in item order (8 = Rus2024, 9 = design 0 in red; 10 = aegis exists in the pack but is not listed) |
| `EssenceFirstId` | `9400` | item mesh id essenceFirstId + N plays design NewMantleNN |
| `EssenceForceDesign` | `-1` | >= 0: every cloak id plays this design (handy to test with whatever item is equipped) |
| `EssenceFrameMode` | `0` | 0 auto (fraction if <= 1), 1 AnimFrame is a 0..1 fraction, 2 AnimFrame is a frame number |
| `EssenceFx` | `1` | material effects of the cloaks: environment-map shine, animated fire / glow layers |
| `EssenceFxAxis` | `0` | how the effect's own axes sit on the torso: 0 x = front, z = up / 1 x = back, z = up / 2 x = up, z = back / 3 x = down, z = front |
| `EssenceFxClothOffset` | `0,-3,1.5` | same for the cloth cloaks: from the middle of the top edge of the cloak |
| `EssenceFxForce` | `` | test: show this effect class on EVERY Essence cloak (e.g. d_cloth_deco_d), changed live |
| `EssenceFxGain` | `1` | brightness of the additive effect layers |
| `EssenceFxMap` | `` | item id : effect class |
| `EssenceFxOffset` | `0,-11,6` | where the effect sits on the torso (animated mantles): left, front, up from Spine2 (units) |
| `EssenceFxPack` | `essence_fx.bin` | file name of the particle effect pack (wings, rays, glows) |
| `EssenceFxParticleGain` | `1` | brightness of the particles |
| `EssenceFxParticles` | `1` | particle effects of the cloaks (wings, rays, glows; essence_fx.bin) |
| `EssenceFxScale` | `1` | size of the effects |
| `EssenceFxWingFlip` | `1` | the animated wings (wing_high) sweep backwards (0 = forwards) |
| `EssenceLight` | `2` | 0 unlit, 1 always lit by the engine's own D3D lights, 2 lit when the engine had lighting on for the carrier |
| `EssenceLitBright` | `1` | lit: material colour (diffuse and ambient) |
| `EssenceOffset` | `0,0,0` | shifts the mantle in actor space (units) |
| `EssencePack` | `essence_capes.bin` | file name of the mantle pack (mesh + skeleton + baked animations + textures) |
| `EssenceRide` | `1` | mounts: the rider plays a fixed 'strider' / 'wyvern' / 'pet' sequence, so the cloaks take their wind (walk / run) from the speed of the character |
| `EssenceRideSpeed` | `250` | speed (units/s) at which a ridden cloak gets the full run wind |
| `EssenceScale` | `1` | scale of the animated mantles |
| `EssenceShowCarrier` | `0` | draw the old (rigid/cloth) carrier cloak too: it sits correctly on the back, a reference for EssenceOffset |
| `EssenceSnug` | `0` | units the upper part of the mantle is pulled towards the chest (the lower part hangs free) |
| `EssenceSway` | `1` | secondary motion of the animated mantles (hem lags the torso, idle flutter, trailing in walk/run): 0 = off, 1 = default, 2 = double |
| `EssenceTween` | `1` | follow the engine's own tween (negative AnimFrame) so the mantle blends exactly like the body |
| `EssenceWingBase` | `61` | (item id - FirstId) from here on = the wing-like cloaks listed in EssenceWingMap (cycled) |
| `EssenceWingMap` | `11,12,13,14,10,15` | pack design ids: 11 Valakas wings, 12/13 Death Knight bat wings, 14 DK ranker winged mantle, 10 Eigis blades, 15 MG ranker angel ribbons |

## Legacy experiments (the first rigid-mesh sway / vertex deformation; keep the defaults)

| Key | Default | What it does |
|---|---|---|
| `CapeAmp` | `25` | degrees |
| `CapeAxis` | `0` | 0 pitch, 1 yaw, 2 roll |
| `CapeFreq` | `0.5` | Hz |
| `CapeSpace` | `0` | `Space` argument of SetBoneRotation |
| `CapeTest` | `0` | experiment: oscillate the pawn's Cape_Bone with SetBoneRotation (sine) |
| `ClothDelay` | `0.18` | seconds the hem lags behind the top (the pose travels down the cloth) |
| `ClothDrag` | `1.0` | air push per unit of body speed (forward); lateral/vertical are scaled from it |
| `ClothGravity` | `90` | pull towards "down" when the torso tilts (kneeling, leaning); zero while it is upright |
| `ClothGust` | `35` | fluttering acceleration (grows with speed) |
| `ClothInertia` | `0.4` | how much of the body's acceleration the cloth feels |
| `ClothKHem` | `14` | spring back to the rest shape at the hem (rad/s^2: 14 -> ~0.6 Hz) |
| `ClothKN` | `60` | coupling between neighbouring points (waves travel through this; low = floppy, high = plank) |
| `ClothKTop` | `500` | ... and at the top edge |
| `ClothMax` | `14` | limit of the displacement (units) |
| `ClothPin` | `0.04` | top fraction of the height that is glued to the body |
| `ClothPow` | `1.5` | how fast the bend grows with the distance from the top edge |
| `ClothSmooth` | `20` | 1/s: how quickly neighbours equalise their velocities (damps ripples) |
| `ClothTether` | `0.03` | cloth does not stretch: a point may be at most this much farther from the top edge than at rest |
| `ClothTurn` | `0.5` | how strongly turning (yaw acceleration, centrifugal) throws the cloth |
| `ClothZMax` | `8` | vertical displacement limit (units) |
| `ClothZeta` | `0.15` | damping ratio of the spring to rest |
| `RippleAmp` | `1.2` | units of ripple across the width |
| `RippleLen` | `26` | units between ripple crests |
| `SwayAmp` | `12` | degrees |
| `SwayAxis` | `0` | local axis the pendulum rotates around (0 x, 1 y, 2 z) |
| `SwayDamp` | `0.35` | damping ratio (<1 = it overshoots and settles) |
| `SwayFreq` | `0.6` | Hz |
| `SwayFwd` | `1` | local axis that points forward (the other horizontal axis is lateral) |
| `SwayFwdSign` | `1` | sign of the forward term of the legacy physics sway |
| `SwayIdle` | `1.5` | degrees of idle breathing |
| `SwayKs` | `0.12` | degrees of sideways tilt per unit/s of lateral speed |
| `SwayKv` | `0.2` | degrees of backward tilt per unit/s of forward speed |
| `SwayKw` | `6` | degrees per rad/s of turning |
| `SwayMax` | `40` | clamp (degrees) |
| `SwayPhysics` | `1` | legacy: physics-driven sway of the rigid carrier cloak |
| `SwaySideSign` | `1` | sign of the sideways term of the legacy physics sway |
| `SwayStiff` | `9` | spring natural frequency (rad/s) |
| `SwayTurnSign` | `1` | sign of the turning term of the legacy physics sway |
| `SwayUp` | `2` | local axis that points "up" (the pivot sits at the max of this axis) |
| `VBAmp` | `25` | degrees of hem swing in mode 0 |
| `VBDeform` | `0` | legacy: deform the carrier cloak vertex buffer (hem bends, top edge glued) |
| `VBFreq` | `0.5` | Hz in mode 0 |
| `VBMode` | `1` | 0 = calibration sine (VBAmp/VBFreq), 1 = physics (the spring above) |
| `VertAmp` | `6` | units |
| `VertAxis` | `0` | coordinate to displace (0 x, 1 y, 2 z) |
| `VertFreq` | `0.5` | Hz |
| `VertTest` | `0` | experiment: deform the cloak's skinned vertices (sine sway of the hem) |
| `VertUp` | `2` | coordinate that is "height" in the skinned vertex space |

## Useful settings

* `EssenceClothWiden` (1) - how much the top of the cloth is stretched sideways to meet the collar. `0` = as cut.
* `EssenceClothTopRamp` (9) - height over which the torso collider grows from the thickness of the pinned rows to its full radius (removes the step under the shoulders). `0` = plain capsule.
* `EssenceFxOffset` / `EssenceFxClothOffset` - where the particle effects (wings, rays) sit relative to the character (left,front,up).
* `EssenceFxWingFlip` (1) - the animated wings sweep backwards; `0` flips them.
* `EssenceFxAxis` (0..3) - orientation preset of the effects' own axes if an effect lies flat instead of standing.
* `Debug` / `EssenceClothDump` - verbose logging in `cloakhook.log` (turn off for normal play: the log grows fast).
