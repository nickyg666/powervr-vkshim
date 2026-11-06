#!/bin/bash
# Run pipetest over a matrix of pipeline configurations.  Each combination
# runs in its own process so a driver crash only kills that test.
cd /home/orangepi/.local/src || exit 1
VS=${VS:-/tmp/vkcompat/spv/0001-vertex.spv}
FS=${FS:-/tmp/vkcompat/spv/0002-fragment.spv}

for n_out in 1 2; do
  for fmt in 0x25 0x61 0; do
    for blend in 0 1; do
      for dfmt in 0x7e 0; do
        for dtest in 0 1; do
          out=$(timeout 20 ./pipetest "$n_out" "$fmt" "$blend" "$dfmt" "$dtest" "$VS" "$FS" 2>&1)
          rc=$?
          if [ $rc -eq 139 ]; then
            res="*** CRASH ***"
          elif [ $rc -eq 124 ]; then
            res="*** HANG ***"
          else
            res=$(printf '%s' "$out" | sed -n 's/.*-> \([0-9-]*\).*/r=\1/p')
            [ -z "$res" ] && res="rc=$rc"
          fi
          printf 'n_out=%d fmt=%-5s blend=%d dfmt=%-5s dtest=%d -> %s\n' "$n_out" "$fmt" "$blend" "$dfmt" "$dtest" "$res"
        done
      done
    done
  done
done
