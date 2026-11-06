#!/bin/bash
# Launch Minecraft with the PowerVR Vulkan compatibility shim and *enforce*
# that the game really runs on Vulkan.
#
#   mc-vulkan.sh [--debug] [--no-repair] [--opengl] [--vulkan] [--world <name>]
#
# Enforcement (no llvmpipe, ever):
#   1. the instance is launched with "--graphicsBackend vulkan" (instance.cfg),
#      so Minecraft ignores its own option and the "unexpected shutdown ->
#      force OpenGL" reset;
#   2. the shim aborts the process if the Vulkan device cannot be created
#      (VKC_REQUIRE_VULKAN=1) instead of letting the game fall back to GL;
#   3. this script watches the game log and kills the game if it ends up on
#      the OpenGL backend, and reports the failure.
#
# Environment:
#   VKC_DEBUG=1    verbose shim logging
#   VKC_REPAIR=0   do not attempt to repair failing pipelines
#   VKC_DIR=<dir>  shim log + SPIR-V dump directory (default /tmp/vkcompat)

set -u
export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-/run/user/1000/gdm/Xauthority}"
export SDL_VIDEODRIVER=x11
export VKC_DIR="${VKC_DIR:-/tmp/vkcompat}"
export VKC_CAPTURE_STDERR=1
export VKC_REQUIRE_VULKAN=1
# Mesa's device_select layer hangs when it is combined with the compatibility
# layer; it only selects between several GPUs and this system has one.
export NODEVICE_SELECT=1
# The NeoForge/FML early display window is created with OpenGL (llvmpipe here).
# It is disabled through the instance's JvmArgs setting instead: PrismLauncher
# strips JAVA_TOOL_OPTIONS from the environment it hands to the JVM.

REPAIR=1
BACKEND=vulkan
WORLD=""
for a in "$@"; do
    case "$a" in
        --no-repair) REPAIR=0 ;;
        --debug)     export VKC_DEBUG=1 ;;
        --opengl)    BACKEND=opengl ;;
        --vulkan)    BACKEND=vulkan ;;
        --world)     shift; WORLD="$1" ;;
    esac
done
export VKC_REPAIR="$REPAIR"

MCDIR="$HOME/.local/share/PrismLauncher/instances/26.3/minecraft"
LOG="$MCDIR/logs/latest.log"

# stop any running instance (launcher UI and game)
/home/orangepi/.local/bin/mc-kill.sh

# force the backend; Minecraft resets it to "default" after an unclean exit
# The settings below are not cosmetic: the driver cannot do the order
# independent transparency passes at all, and the GPU is a mobile one, so a
# 32 chunk render distance means one frame per second.
python3 - "$MCDIR/options.txt" "$BACKEND" <<'PY'
import sys
p, backend = sys.argv[1], sys.argv[2]
FORCE = {
    'preferredGraphicsBackend': '"%s"' % backend,
    'startedCleanly': 'true',
    'improvedTransparency': 'false',   # the OIT passes do not work on this driver
    'renderDistance': '8',
    'simulationDistance': '6',
    'cloudRange': '16',
    'maxFps': '60',
    'exclusiveFullscreen': 'false',   # so that the screen can be captured
    'entityShadows': 'false',
    'mipmapLevels': '0',
    'graphicsPreset': 'custom',
}
lines, seen = [], set()
try:
    src = open(p).read().splitlines()
except OSError:
    src = []
for line in src:
    key = line.split(':', 1)[0]
    if key in FORCE:
        line = '%s:%s' % (key, FORCE[key])
    lines.append(line)
    seen.add(key)
for k, v in FORCE.items():
    if k not in seen:
        lines.append('%s:%s' % (k, v))
open(p, 'w').write('\n'.join(lines) + '\n')
print('options ->', [(k, FORCE[k]) for k in FORCE])
PY

# PrismLauncher rewrites the instance settings on every launch and puts the
# JVM arguments into quotes, which makes it pass a single (broken) argument.
# Write them back without quotes so the flag actually reaches the JVM.
python3 - "$HOME/.local/share/PrismLauncher/instances/26.3/instance.cfg" <<'PY2'
import re, sys
p = sys.argv[1]
try:
    s = open(p).read()
except OSError:
    sys.exit(0)
s = re.sub(r'^JvmArgs=.*$', 'JvmArgs=-Dfml.earlyprogresswindow=false', s, flags=re.M)
if 'JvmArgs=' not in s:
    s = s.replace('[General]\n', '[General]\nJvmArgs=-Dfml.earlyprogresswindow=false\n')
open(p, 'w').write(s)
PY2

rm -f "$VKC_DIR/shim.log"
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.prev" 2>/dev/null

# Minecraft's quickPlay arguments have to reach the main class; PrismLauncher
# ignores the instance setting for them, so they are appended by the Java
# wrapper.
JAVA_WRAP="$HOME/.local/bin/java-fml.sh"
REAL_JAVA="/home/orangepi/.var/app/org.prismlauncher.PrismLauncher/data/PrismLauncher/java/java-runtime-epsilon/bin/java"
CFG="$HOME/.local/share/PrismLauncher/instances/26.3/instance.cfg"
if [ -n "$WORLD" ]; then
    export MC_EXTRA_ARGS="--quickPlaySingleplayer $WORLD"
    python3 - "$CFG" "$JAVA_WRAP" <<'PY2'
import re, sys
p, java = sys.argv[1], sys.argv[2]
s = open(p).read()
s = re.sub(r'^JavaPath=.*$', 'JavaPath=' + java, s, flags=re.M)
open(p, 'w').write(s)
PY2
    "$HOME/Applications/PrismLauncher-aarch64.AppImage" --launch 26.3 &
else
    "$HOME/Applications/PrismLauncher-aarch64.AppImage" --launch 26.3 &
fi
LAUNCHER_PID=$!

# ---------------------------------------------------------------------------
# watchdog: refuse to let the game run on the OpenGL (llvmpipe) backend
# ---------------------------------------------------------------------------
echo "watching $LOG for the graphics backend ..."
VULKAN_OK=0
DEADLINE=$((SECONDS + 3600))
while [ $SECONDS -lt $DEADLINE ]; do
    sleep 2
    if [ -f "$LOG" ]; then
        if [ "$VULKAN_OK" -eq 0 ] && grep -q "Using graphics device:" "$LOG" 2>/dev/null; then
            dev=$(grep -m1 "Using graphics device:" "$LOG")
            case "$dev" in
                *PowerVR*)
                    echo "OK: $dev"
                    VULKAN_OK=1
                    ;;
                *)
                    echo "FAIL: the game is using a software/other renderer - killing it."
                    echo "      $dev"
                    /home/orangepi/.local/bin/mc-kill.sh
                    exit 1
                    ;;
            esac
        fi
        # the game must never end up on the OpenGL (llvmpipe) backend
        if grep -qE "Using graphics backend (OpenGL|OPENGL)" "$LOG" 2>/dev/null; then
            echo "FAIL: the game fell back to the OpenGL (llvmpipe) backend - killing it."
            /home/orangepi/.local/bin/mc-kill.sh
            exit 1
        fi
    fi
    if ! kill -0 "$LAUNCHER_PID" 2>/dev/null; then
        sleep 2
        break
    fi
done

if [ "$VULKAN_OK" -ne 1 ]; then
    if grep -qE "Using graphics backend (OpenGL|OPENGL)" "$LOG" 2>/dev/null; then
        echo "FAIL: the game fell back to the OpenGL (llvmpipe) backend - killing it."
        /home/orangepi/.local/bin/mc-kill.sh
        exit 1
    fi
    echo "FAIL: Vulkan was not initialised - the game is not running on the GPU."
    echo "      see $LOG and $VKC_DIR/shim.log"
    /home/orangepi/.local/bin/mc-kill.sh
    exit 1
fi

echo "Minecraft is running on Vulkan (PowerVR)."
wait "$LAUNCHER_PID"

if [ -n "$WORLD" ]; then
    python3 - "$CFG" "$REAL_JAVA" <<'PY2'
import re, sys
p, java = sys.argv[1], sys.argv[2]
s = open(p).read()
s = re.sub(r'^JavaPath=.*$', 'JavaPath=' + java, s, flags=re.M)
open(p, 'w').write(s)
PY2
fi
