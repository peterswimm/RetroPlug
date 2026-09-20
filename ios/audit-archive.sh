#!/usr/bin/env bash
set -euo pipefail

archive="${1:-}"
if [ -z "$archive" ] || [ ! -d "$archive" ]; then
  echo "usage: $0 path/to/RetroPlug.xcarchive" >&2
  exit 2
fi

fail=0
while IFS= read -r file; do
  echo "error: bundled ROM-like file: $file" >&2
  fail=1
done < <(find "$archive/Products" -type f \( -iname '*.gb' -o -iname '*.gbc' -o -iname '*.gba' \
  -o -iname '*.nes' -o -iname '*.sms' -o -iname '*.gg' \) -print)

while IFS= read -r binary; do
  if strings "$binary" | grep -Eiq 'mgb_rom|Mesen(Core|Nes|Gba|Sms)|GNU GENERAL PUBLIC LICENSE'; then
    echo "error: excluded ROM/Mesen/GPL marker in Mach-O: $binary" >&2
    fail=1
  fi
done < <(find "$archive/Products" -type f -perm -111 -print)

if [ "$fail" -ne 0 ]; then
  echo "archive audit failed" >&2
  exit 1
fi
echo "archive audit passed: no bundled ROMs, Mesen markers, or GPL license payloads"
