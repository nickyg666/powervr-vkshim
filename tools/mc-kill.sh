#!/bin/bash
# Kill any running PrismLauncher / Minecraft processes.
for pat in 'org.prismlauncher.EntryPoint' 'prismlauncher --launch' 'PrismLauncher-aarch64.AppImage' 'org.prismlauncher.PrismLauncher'; do
    pids=$(pgrep -f "$pat" | grep -v "^$$\$")
    [ -n "$pids" ] && kill -TERM $pids 2>/dev/null
done
sleep 5
for pat in 'org.prismlauncher.EntryPoint' 'prismlauncher --launch' 'PrismLauncher-aarch64.AppImage' 'org.prismlauncher.PrismLauncher'; do
    pids=$(pgrep -f "$pat" | grep -v "^$$\$")
    [ -n "$pids" ] && kill -KILL $pids 2>/dev/null
done
sleep 1
exit 0
