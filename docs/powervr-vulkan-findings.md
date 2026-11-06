# PowerVR Vulkan on the Orange Pi Zero 3W - findings

Board: Orange Pi Zero 3W, aarch64, kernel `6.6.98-sun60iw2`, Ubuntu 26.04,
GPU: Imagination PowerVR B-Series **BXM-4-64 MC1** (Rogue), vendor 0x1010 device 0x36104183.

**Status: Minecraft 26.3 (NeoForge) runs on the PowerVR GPU through Vulkan at ~55-59 FPS
at 1920x1080, with no llvmpipe anywhere in the game path.**

There is **no** Imagination driver source anywhere on the system - the only driver is the
proprietary binary blob `xserver-xorg-img-bxm-1.21.1-2.deb` (DKK 24.2@6603887):

    /usr/lib/libVK_IMG.so.24.2.6603887   Vulkan ICD
    /usr/lib/libsrv_um.so                user mode service
    /usr/lib/libusc.so                   unified shader compiler
    /usr/lib/libufwriter.so              firmware writer
    /lib/modules/6.6.98-sun60iw2/extra/pvrsrvkm.ko   kernel driver (rogueddk 24.2@6603887)

The kernel driver and the user space driver **match** (both 24.2@6603887) and the firmware
matches as well:

    /lib/firmware/rgx.fw.36.56.104.183   (main firmware, expected by pvrsrvkm)
    /lib/firmware/rgx.sh.36.56.104.183   (shader/BVNC firmware)

## The compatibility shim is a Vulkan *layer*

    /home/orangepi/.local/src/vkimg_compat.c   -> /usr/lib/libVkLayer_powervr_compat.so
    /usr/share/vulkan/implicit_layer.d/VkLayer_powervr_compat.json   (implicit, GLOBAL)

It **must** be a layer and not an ICD wrapper - see "The loader dispatch table trap" below.
The ICD stays the original driver:

    /usr/share/vulkan/icd.d/img_icd.json -> "library_path": "/usr/lib/libVK_IMG.so"

What it intercepts:

* `vkCreateShaderModule`      - dumps every SPIR-V blob to `/tmp/vkcompat/spv/`,
                                 rewrites `OpKill` -> `OpReturn` in Minecraft's
                                 order-independent-transparency shaders and rewrites
                                 texel buffer descriptors into storage buffers
* `vkCreateGraphicsPipelines` - retries (the driver is flaky), then tries a series of
                                 repairs (see below)
* `vkCreateDescriptorSetLayout` - turns `UNIFORM_TEXEL_BUFFER` into `STORAGE_BUFFER`
* `vkCreateBufferView` / `vkCmdPushDescriptorSetKHR` / `vkUpdateDescriptorSets` -
                                 translate the texel buffer descriptor into a storage
                                 buffer descriptor (same VkBuffer, same offset and range)
* `vkCreatePipelineLayout` - logged
* `vkAcquireNextImageKHR` / `vkQueuePresentKHR` - timed and logged, and the presented
                                 frames are counted to get the frame rate
* all driver compilation calls are serialised with a recursive mutex

Environment: `VKC_DEBUG=1`, `VKC_REPAIR=0`, `VKC_RETRY=<n>`, `VKC_DIR=<dir>`,
`VKC_CAPTURE_STDERR=1`, `VKC_REQUIRE_VULKAN=1`.

## Bugs in the proprietary driver (all reproduced standalone)

Reproducers in `/home/orangepi/.local/src/`:
`pipetest.c` / `pt2.c` (pipeline creation against the raw ICD), `spvtest.c` (shader modules),
`gdpatest.c` (loader dispatch), `desctest.c` (descriptor parsing), `pvrquery.c` (DRM query).

1. **`OpKill` (discard) is not supported by the pipeline compiler.**
   `vkCreateGraphicsPipelines` returns `VK_ERROR_UNKNOWN` for fragment shaders that
   contain `OpKill`.  Rewriting `OpKill` -> `OpReturn` makes the very same pipeline
   compile (`pipetest`: `orig -> r=-13`, `kill2ret -> r=0`).

2. **`UNIFORM_TEXEL_BUFFER` descriptors cannot be combined with a buffer image.**
   This is the bug that broke Minecraft's cloud shader:

   * the shader (`assets/minecraft/shaders/core/clouds.vsh`) declares
     `uniform isamplerBuffer CloudFaces;` -> SPIR-V `OpTypeImage %int Buffer`
   * the descriptor set layout declares binding 4 as
     `VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER` (4)
   * `vkCreateGraphicsPipelines` then returns `VK_ERROR_UNKNOWN` - **deterministically**,
     6 out of 6 runs (measured with `pt2`).
   * the same shader compiles fine when the descriptor is a
     `COMBINED_IMAGE_SAMPLER`, and a layout with a `UNIFORM_TEXEL_BUFFER` binding
     compiles fine too as long as the shader does not use a buffer image - so the
     driver rejects exactly the combination "texel buffer descriptor + buffer image".

   The fix is done in the layer: the shader is rewritten so that the buffer is read
   through a **storage buffer** (a runtime array of `int8`, the cloud data is `R8_SINT`),
   the descriptor set layout binding becomes `STORAGE_BUFFER`, and the descriptor write
   is translated into a `VkDescriptorBufferInfo` that points at exactly the same
   `VkBuffer`, offset and range.  The data path is unchanged.

   The rewrite lives in `/home/orangepi/.local/src/pvr_texelbuf.c`
   (`spv_texelbuf_to_storage()`), it produces conformant SPIR-V 1.3
   (`spirv-val --target-env vulkan1.3` accepts it).

3. **The pipeline compiler is non-deterministic.**  The *same* valid pipeline fails with
   `VK_ERROR_UNKNOWN` in roughly 1 out of 6 runs and succeeds otherwise.  Retrying is
   enough to get a valid pipeline - but only before the driver state is poisoned.

4. **The driver hard-crashes the process** on some inputs: a 16 byte `memcpy` from a NULL
   pointer inside `libVK_IMG.so` (`gdb`: `__memcpy_generic <- libVK_IMG.so+0x...`, `x1 = 0`).
   The crash is asynchronous - it happens in the driver's own worker thread, so the
   application dies before it can react.  This is why the repairs have to be
   *pre-emptive* (see "descriptor layout mismatch" below).

5. **No `floatBlend` feature.**  Blending 32 bit float colour attachments
   (`VK_FORMAT_R32G32B32A32_SFLOAT`, used by the OIT passes) is not supported.
   `VK_EXT_blend_operation_advanced` is advertised, but the MINIMUM/MAXIMUM blend
   operations on 32 bit float targets are not accepted.

6. **No `VK_EXT_robustness2` / `nullDescriptor`** - this is why Mesa's Zink cannot be used
   as a GL-on-Vulkan path.

7. **The buffer image path is broken in general**: `OpImageFetch` on a buffer image makes
   the driver crash (`libufwriter.so`) even when the pipeline is created successfully.

## The descriptor layout mismatch in Minecraft's own shaders

Minecraft 26.3 creates descriptor set layouts from the *RenderPipeline* definition but
assigns the shader's binding numbers from the SPIR-V reflection
(`com.mojang.renderpearl.frontend.shaders.PipelineBuilder`).  The two can disagree and
the check that should catch it cannot:

    com.mojang.renderpearl.frontend.shaders.SpvUtil.resourceType():
        case COMBINED_IMAGE_SAMPLER -> 7;
        case TEXEL_BUFFER           -> 7;     <-- indistinguishable

So a `TEXEL_BUFFER` uniform and a `COMBINED_IMAGE_SAMPLER` uniform look the same to the
consistency check, but they need different Vulkan descriptor types (4 vs 1).  The
consequence for the clouds pipeline is bug 2 above.

## The loader dispatch table trap (important)

When the shim is installed as the ICD, the Vulkan loader builds the *device* dispatch table
from the ICD's `vkGetDeviceProcAddr`.  Two things were measured with
`/home/orangepi/.local/src/gdpatest.c`:

* the loader **never calls** the ICD's `vkGetDeviceProcAddr` for the device table - it
  fills the table through `icd_term->dispatch.GetDeviceProcAddr` and
  `vk_icdGetPhysicalDeviceProcAddr`;
* the PowerVR driver's own `vk_icdGetPhysicalDeviceProcAddr` returns **NULL for every
  device level function**, so the table ends up full of NULLs and the application
  (LWJGL) calls through NULL pointers -> SIGSEGV inside the JVM's JIT code.

**A Vulkan layer is the only way to intercept device level calls.**  The loader builds the
device dispatch table from the *layer chain's* `vkGetDeviceProcAddr`
(`loader/loader.c`: `loader_create_device_chain()` -> `nextGDPA` ->
`loader_init_device_dispatch_table()`), so a layer's `vkGetDeviceProcAddr` is called
631 times per run in Minecraft and every device function is intercepted.

The layer protocol (loader 1.4.341 sources, `loader/loader.c`):

* `vkNegotiateLoaderLayerInterfaceVersion()` - gives the layer the loader's
  `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`.
* instance creation: the loader puts a `VK_LAYER_LINK_INFO` struct
  (`VkLayerInstanceCreateInfo`) into the `pNext` chain; `u.pLayerInfo` is a
  `VkLayerInstanceLink` with `pfnNextGetInstanceProcAddr`.
* device creation: the loader puts a `VK_LOADER_LAYER_CREATE_DEVICE_CALLBACK` struct into
  the `pNext` chain; `u.layerDevice.pfnLayerCreateDevice` has to be used to create the
  device, and the `VkLayerDeviceLink` (`VK_LAYER_LINK_INFO` in the device create info)
  gives the next layer's `vkGetDeviceProcAddr`.
  The device chain link has to be read **before** the device is created, because the
  loader fills the device dispatch table through the layer's own
  `vkGetDeviceProcAddr` while the device is being created.

## Vulkan only - never llvmpipe

Four independent layers of enforcement:

1. The Mesa software Vulkan ICD (`/usr/share/vulkan/icd.d/lvp_icd.json`) is **disabled**
   (renamed to `lvp_icd.json.disabled`), so there is no software Vulkan driver left that
   the game could fall back to.  `powervr_mesa_icd.json` is disabled as well.

2. The shim aborts the process (`VKC_REQUIRE_VULKAN=1`) if `vkCreateDevice` fails, instead
   of letting the game silently switch to the OpenGL backend.

3. `/home/orangepi/.local/bin/mc-vulkan.sh` watches `logs/latest.log`, verifies
   `Using graphics device: PowerVR ...`, and kills the game if it ends up on the OpenGL
   backend.  (This caught a real regression: when the ICD json pointed at a deleted
   library the loader fell back to Mesa's llvmpipe and the watchdog killed the game with
   "FAIL: the game is using a software/other renderer - killing it.")

4. `options.txt` -> `preferredGraphicsBackend:"vulkan"` and
   `startedCleanly:true` (Minecraft resets the backend to OpenGL after an unclean exit).

Note: `instances/26.3/instance.cfg` -> `Args=--graphicsBackend vulkan` is *not* honoured
by the installed PrismLauncher build (the `Args` key is ignored), the Vulkan backend is
selected through `options.txt`.

## The OpenGL side has no acceleration at all

There is no usable DRI3/GBM screen for the PowerVR GPU:

    glx: failed to create dri3 screen
    failed to load driver: sunxi-drm

so every OpenGL context on this board is llvmpipe (software).  Two consequences:

* Mesa's `VkLayer_MESA_device_select` **deadlocks** in combination with our layer
  (`NODEVICE_SELECT=1` does not disable it - the manifest
  `/usr/share/vulkan/implicit_layer.d/VkLayer_MESA_device_select.json` was renamed to
  `.json.disabled`).
* The NeoForge/FML *early progress window* is an OpenGL window.  It is disabled with
  `config/fml.toml`:

        earlyWindowControl = false

  (`PrismLauncher` strips `JAVA_TOOL_OPTIONS` from the environment it hands to the JVM and
  ignores its own `JvmArgs`/`JavaArgs` instance settings, so the config file is the only
  reliable way.)

## Frame rate

The layer counts `vkQueuePresentKHR` calls, so the frame rate does not need an injected
keypress (the F3 overlay would need one, and XFCE binds `Alt+F3` to the application
finder - sending F2/F3 to the desktop pops that window up).

Measured on the title screen, 1920x1080 exclusive fullscreen: **55-59 FPS**.

## The Mesa alternative (open source Vulkan driver for this GPU)

Mesa >= 25.3 ships **`libvulkan_powervr_mesa.so`**, an open source Vulkan driver for the
PowerVR B-Series / Rogue (it contains `src/imagination/vulkan/rogue/...` and knows the
`BXM-4-64`).  It is disabled unless

    export PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1

is set (the string is literally in the Mesa binary).  The installed Mesa is **26.0.3**,
which satisfies ">= 25.3".

Mesa only treats a device as conformant for these BVNCs (`pvr_device_is_conformant()`):

    PVR_BVNC_PACK(36, 52, 104, 182)
    PVR_BVNC_PACK(36, 53, 104, 796)

Our GPU reports 36.56.104.183.  `PVR_SHIM_DEVICE_BVNC` does **not** exist in Mesa 26.0.3
(it is a newer Mesa feature), so it was implemented here as an LD_PRELOAD shim:

    /home/orangepi/.local/src/libpvr_bvnc_shim.c -> /usr/local/lib/libpvr_bvnc_shim.so

It intercepts `ioctl(DRM_IOCTL_PVR_DEV_QUERY)` and rewrites the returned `gpu_id`
(B<<48|V<<32|N<<16|C) to the requested BVNC:

    LD_PRELOAD=/usr/local/lib/libpvr_bvnc_shim.so \
    PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 \
    PVR_SHIM_DEVICE_BVNC=36.52.104.182 \
    VK_DRIVER_FILES=/usr/share/vulkan/icd.d/powervr_mesa_icd.json vulkaninfo --summary

### Blocker for the Mesa driver

Mesa's driver cannot see the GPU at all:

    "Device does not use any of the supported pvrsrvkm or powervr kernel driver."

It probes the DRM device with **`DRM_IOCTL_PVR_DEV_QUERY`** (plus
`DRM_IOCTL_PVR_CREATE_VM_CONTEXT`, `..._VM_MAP`, `..._GET_BO_MMAP_OFFSET`, ...).  The
uapi header `/usr/include/drm/pvr_drm.h` is installed, but the Orange Pi kernel's
`pvrsrvkm.ko` does **not** implement those ioctls - the request fails with `ENOTTY`
("Inappropriate ioctl for device"), which was verified directly with
`/home/orangepi/.local/src/pvrquery.c`.

So the Mesa driver needs a kernel that provides the PVR DRM uapi (Mesa's own
`drm_pvrsrvkm`, or a vendor kernel with it).

## Bluetooth input - SOLVED by building the missing kernel module

The Bluetooth *radio* works (the Allwinner AIC8800 chip, `aic8800-bluetooth.service` +
`bluetoothd`), devices pair and connect:

    Device C0:ED:FF:0D:13:5A  Logi K250   Paired: yes  Connected: yes
    Device D1:01:77:A4:B1:7E  Logi M196   Paired: yes  Connected: yes

but no input ever reached the system, because the vendor kernel does not contain the
Bluetooth HID support (`/boot/config-6.6.98-sun60iw2`):

    CONFIG_BT=y
    CONFIG_BT_HIDP=m            <- the HIDP transport (L2CAP PSM 0x11/0x13)
    # CONFIG_UHID is not set    <- no /dev/uhid
    # CONFIG_HIDRAW is not set
    (CONFIG_INPUT_UINPUT is not set either, and it is hidden behind
     CONFIG_INPUT_MISC which the vendor also disabled)

The decisive hint was in the BlueZ log:

    bluetoothd: src/service.c:service_accept() input-hog profile accept failed for C0:ED:FF:0D:13:5A

BlueZ takes the Bluetooth HID service over with its **input-hog** profile, which needs
`/dev/uhid` to hand the HID reports to the kernel - and `/dev/uhid` did not exist.

### Building uhid.ko for the running kernel

The vendor kernel sources are

    https://github.com/orangepi-xunlong/linux-orangepi   (branch orange-pi-6.6-sun60iw2)

(cloned to `/home/orangepi/kernel-src`).  `CONFIG_MODVERSIONS` and `CONFIG_MODULE_SIG`
are **off** in this kernel, so an out-of-tree module can be built without the
kernel's `Module.symvers`; a symbol table can be reconstructed from `/proc/kallsyms`:

    #!/bin/bash
    # in /home/orangepi/kernel-src
    cp /boot/config-6.6.98-sun60iw2 .config
    ./scripts/config --file .config -m UHID -m INPUT_UINPUT
    ./scripts/config --file .config -e INPUT_MISC
    ./scripts/config --file .config --set-str LOCALVERSION "-sun60iw2"
    ./scripts/config --file .config -d LOCALVERSION_AUTO
    # the "+" that setlocalversion adds for a git tree breaks the vermagic:
    mv .git /tmp/kernel-src.git
    make olddefconfig
    # the kernel's exported symbols, needed by modpost:
    awk '$3 ~ /^[A-Za-z_][A-Za-z0-9_]*$/ && !seen[$3]++ { printf "0x00000000\t%s\tkernel\tEXPORT_SYMBOL\t\n", $3 }' \
        /proc/kallsyms > Module.symvers
    make -j8 modules_prepare
    make M=drivers/hid modules            # -> drivers/hid/uhid.ko
    make M=drivers/input/misc modules    # -> drivers/input/misc/uinput.ko
    install -D -m 0644 drivers/hid/uhid.ko /lib/modules/6.6.98-sun60iw2/kernel/drivers/hid/uhid.ko
    depmod -a 6.6.98-sun60iw2
    modprobe uhid

The resulting vermagic has to match the running kernel exactly:

    6.6.98-sun60iw2 SMP preempt mod_unload aarch64

(`//lib/modules/6.6.98-sun60iw2` also holds the vendor's `pvrsrvkm.ko`, whose vermagic
gives the expected format.)

Both modules are loaded automatically via `/etc/modules-load.d/uhid.conf`, so
`/dev/uhid` and `/dev/uinput` exist after a reboot.

### Result

BlueZ's input-hog profile now accepts the HID service and the kernel's HID core
creates real input devices:

    N: Name="Logi K250 Keyboard"   H: Handlers=sysrq kbd leds event9
    N: Name="Logi M196 Mouse"      H: Handlers=event8

Key events from the Bluetooth keyboard were verified on `/dev/input/event9`
(KEY_LEFT_CTRL / KEY_RIGHT_CTRL press and release).

Note: the Bluetooth *HID host* driver (`bt-hid`) does not exist in this kernel at all -
upstream Linux 6.6 has no `drivers/bluetooth/hid.c` and no `CONFIG_BT_HID`; the only
Bluetooth HID code in the tree is `net/bluetooth/hidp/` (the `hidp` module, which is
loaded).  BlueZ's input-hog profile is what fills the gap, and it needs `uhid`.

## Scripts

    /home/orangepi/.local/bin/mc-vulkan.sh    launch Minecraft on Vulkan, enforce Vulkan
                                              (--debug, --world <name>, --no-repair)
    /home/orangepi/.local/bin/mc-kill.sh      stop launcher + game
    /home/orangepi/.local/bin/mc-gdb.sh       launch under gdb to catch the crash
    /home/orangepi/.local/bin/icd.sh orig|shim  switch between the raw driver and the shim
    /home/orangepi/.local/bin/java-fml.sh     Java wrapper (extra JVM flags)
