# powervr-vkshim — Minecraft Java (Vulkan) on the PowerVR BXM-4-64

Hardware-accelerated Vulkan for **Minecraft Java Edition** on the
**Orange Pi Zero 3W** (RK3562-class SG2002/…, aarch64) using the vendor
proprietary **PowerVR BXM-4-64 driver (IMGTEC DDK 24.2 / Vulkan 1.3.277)**.
No llvmpipe, no software rendering.

`vkimg_compat.c` builds an implicit **Vulkan layer**
(`libVkLayer_powervr_compat.so`) that sits between the game and the
proprietary driver and repairs several driver bugs by pre-emptive
SPIR-V/state/protocol rewriting, because the driver's pipeline compiler
crashes irrecoverably on retry.

## TL;DR — the four driver bugs

| # | Bug | Symptom | Fix in this layer |
|---|-----|---------|-------------------|
| 1 | **Pipeline compiler chokes on OpKill** (discard) | process killed asynchronously, garbage fragments in GUI text | SPIR-V rewrite of `OpKill` → branch + transparent store (default **off**, env `VKC_KILL_REWRITE=1` to re-enable) |
| 2 | **Texel buffers (`OpTypeImage dim=Buffer`) unimplemented** | clouds pipeline fails to build; `binding UNIFORM_TEXEL_BUFFER` crashes pipeline compile | rewrite texel-buffer descriptors → storage buffer (`pvr_texelbuf.c`) |
| 3 | **Only ONE copy executes per command buffer**: a 2nd/3rd/… `vkCmdCopyBufferToImage`/`vkCmdCopyImage`/`vkCmdBlitImage` recorded directly into the same primary is dropped or truncated to its first rows | scrambled psychedelic world, x-ray blocks, missing HUD/text — *Minecraft stitches its atlases with hundreds of copies* | route every copy/blit through our own **secondary command buffer** + `vkCmdExecuteCommands` (`pvr_copysec.c`), env `VKC_NO_COPYSEC=1` to disable |
| 4 | **Implicit-`LOD` sampling broken**: sampler lands on garbage mip levels | flat colour patches — *this was the "psychedelic terrain"* | clamp every sampler to **LOD 0** at `vkCreateSampler` (`VKC_CLAMP_LOD0`, default on in `mc-vulkan.sh`) |
| 5 | LINEAR images cannot be sampled | all texture garbage | force sampled LINEAR images to OPTIMAL tiling at `vkCreateImage` (`VKC_NO_LINEAR_OPTIMAL` to disable) |
| 6 | atlas content verified **correct** via an in-layer readback dump (`VKC_DUMP_ATLAS`) — copying never was the problem | — | — |

Patch quest (all verified with the `tests/primtest*.c` reproducers):

* primitive copies work **perfectly** when isolated: `imageOffset`,
  `bufferOffset`, `bufferRowLength`, `CopyImage`/`BlitImage`/`CopyImageToBuffer`
  with offsets — all correct (primtest2).
* the failure correlates with **multiple copy commands in one command
  buffer** (primtest3), not with parameters (primtest1 was a red herring;
  its "offsets are ignored" conclusion was wrong).
* `vkCmdCopyBuffer` (buffer→buffer) **repeats fine** (primtest4 A).
* executing the copies via **secondary command buffers works**:
  primtest4 D/E and primtest5 W1 = **100-copy atlas stitch, 0/100 tiles
  wrong**.

## Layout

    src/    the layer
      vkimg_compat.c          main layer: protocol, device chaining, hooks
      pvr_texelbuf.c/.h       texel-buffer → storage-buffer SPIR-V rewrite
      pvr_lod.c/.h            forced LOD0 SPIR-V experiment (retired fix option)
      pvr_spirv_descriptors.h SPIR-V descriptor parser
      pvr_copysec.c/.h        copy→secondary routing (atlas fix)
      minimal_fs.h, gen_minimal_fs.py, vkimg_serialize.c  helpers
    tests/  standalone reproducers for each driver quirk
    manifests/  layer manifest + ICD pointer (copied from /usr/share/vulkan)
    tools/  mc-vulkan.sh (launcher/watchdog), mc-kill.sh, run-matrix.sh
    docs/powervr-vulkan-findings.md  running engineering log

## Build & install

    make            # layer + test reproducers
    sudo make install   # puts libVkLayer_powervr_compat.so into /usr/lib/

The layer is an implicit layer (manifest in
`/usr/share/vulkan/implicit_layer.d/VkLayer_powervr_compat.json`) that
chains through `VK_LAYER_LINK_INFO` + the loader's
`VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK`. The ICD json
(`/usr/share/vulkan/icd.d/img_icd.json`) still points at the original
`/usr/lib/libVK_IMG.so` — the shim is only a layer; no ICD shims.

Note: `VK_LAYER_MESA_device_select` deadlocks with this layer and Mesa
software Vulkan ICDs (`lvp_icd.json`, `powervr_mesa_icd.json`) must stay
disabled, otherwise the game can pick llvmpipe — see
`manifests/` for what the system looks like.

## Env switches

    VKC_CLAMP_LOD0=1       clamp every sampler to LOD 0 (driver mips broken)
    VKC_NO_COPYSEC=1     disable the copy→secondary workaround
    VKC_NO_TEXEL=1       disable the texel-buffer rewrite
    VKC_KILL_REWRITE=1   enable the OpKill rewrite (orig. default-on experiment)
    VKC_NO_KILL_REWRITE=1  leave OpKill alone (currently the stable default)
    VKC_OIT_FIX=1        enable the over-broad OIT state fix (10 FPS; off)
    VKC_LOD0=1           force implicit LODs to 0 (causes driver crash — do not)
    VKC_DUMP_PATCHED=1   dump patched SPIR-V modules (spirv-val them!)
    VKC_DUMP_PIPELINES=1 pipeline log
    VKC_LOG_IMAGES=1     image/copy/view/sampler logging
    VKC_REQUIRE_VULKAN=1 watchdog hard-guarantee (no llvmpipe)

## Reproducing the driver bugs

    cd tests && make -C .. 
    DISABLE_VK_LAYER_POWERVR_COMPAT=1 ./primtest5   # raw driver
    ./primtest5                                     # with everything loaded 

## Status

* layer works end to end (watchdog: `Using graphics device: PowerVR
  B-Series BXM-4-64 MC1 (IMGTEC)`), menu ~59 FPS.
* texel-buffer fix (clouds) verified in game.
* atlas/copy fix verified standalone (`primtest5`, W1 0/100 wrong).
* in-world with `graphicsPreset:"fast"`: ~28 FPS, geometry **and** textures
  (sharp LOD-0 sampling) now correct.
* remaining: colour tint oddities, cloud flicker/jumping, HUD icon
  garbling, in-world FPS on `fancy` ~10.

## Board

    Orange Pi Zero 3W, aarch64, 6.6.98-sun60iw2, Ubuntu 26.04, Xfce/Xorg,
    PowerVR BXM-4-64 MC1, DDK 24.2@6603887, Vulkan 1.3.277.
