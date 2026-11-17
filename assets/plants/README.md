# Ground cover plants

From Poly Haven (https://polyhaven.com/models), CC0 (public domain), 1k glTF versions:
fern_02, dry_branches_medium_01, shrub_02, shrub_03, nettle_plant, periwinkle_plant, shrub_sorrel_01,
weed_plant_02, dandelion_01, moss_01, celandine_01, shrub_01, shrub_04, grass_medium_01, root_cluster_01.

`textures/<name>_diff_alpha_1k.png` is the diffuse map with the model's alpha map merged into its alpha channel
(the cutout, GroundCover::init loads it as a BC3 atlas). The roughness / metal / AO maps aren't used.

Added later, same source and licence (1k glTF, diffuse + normal + alpha only): grass_medium_02, grass_bermuda_01,
crystalline_iceplant, cheiridopsis_succulent, dead_quiver_branch_01, dead_quiver_branch_02, root_cluster_02. Their
`_diff_alpha_1k.png` were made from the downloaded `_diff_1k.jpg` + `_alpha_1k.png` the same way.

## Game ready packs (CC-BY 4.0)

Converted by `tools/import_packs.py` from `assets/incoming/` (each pack's `license.txt` is kept there). Their nodes are
`<plant>__LOD0/1/2`: GroundCover::init uses the packs' own LODs and never simplifies them.

- This work is based on "Bush models pack (gameready, LODs)" (https://sketchfab.com/3d-models/bush-models-pack-gameready-lods-f2d9ffd3e6a94cf0b9464ccd66a4c2f8) by LOLIPOP (https://sketchfab.com/lolipop_1707) licensed under CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/)
- This work is based on "Cactus pack (18 var, game ready, LOD, realistic)" (https://sketchfab.com/3d-models/cactus-pack-18-var-game-ready-lod-realistic-297e91de8db54640aaa9489f982b088e) by LOLIPOP (https://sketchfab.com/lolipop_1707) licensed under CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/)
- This work is based on "Daisy models pack (realistic, optimized)" (https://sketchfab.com/3d-models/daisy-models-pack-realistic-optimized-0748471f99f2416a8b66b522c293720f) by LOLIPOP (https://sketchfab.com/lolipop_1707) licensed under CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/)
- This work is based on "Grass pack (lowpoly, game-ready)" (https://sketchfab.com/3d-models/grass-pack-lowpoly-game-ready-422f42f618d342c6958b02613dc57403) by LOLIPOP (https://sketchfab.com/lolipop_1707) licensed under CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/)
- This work is based on "Grass (pack of 9 vars, lowpoly, game ready)" (https://sketchfab.com/3d-models/grass-pack-of-9-vars-lowpoly-game-ready-0561204a1fa14c17939300ee1108948b) by LOLIPOP (https://sketchfab.com/lolipop_1707) licensed under CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/)
- This work is based on "Lilac bush pack (12 vars, LODs, game ready)" (https://sketchfab.com/3d-models/lilac-bush-pack-12-vars-lods-game-ready-10312697ec994fc99355cb94f1963a2e) by LOLIPOP (https://sketchfab.com/lolipop_1707) licensed under CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/)
- This work is based on "Lowpoly Mushrooms" (https://sketchfab.com/3d-models/lowpoly-mushrooms-c865fb845bac42c7939da06d4617b7b1) by Loïc Norgeot (https://sketchfab.com/norgeotloic) licensed under CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/)
- This work is based on "Undergrowth pack (lowpoly, game-ready)" (https://sketchfab.com/3d-models/undergrowth-pack-lowpoly-game-ready-c98b4c1a15aa447a97cb76df127c1c78) by LOLIPOP (https://sketchfab.com/lolipop_1707) licensed under CC-BY-4.0 (http://creativecommons.org/licenses/by/4.0/)
