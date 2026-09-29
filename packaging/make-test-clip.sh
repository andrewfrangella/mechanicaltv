#!/usr/bin/env bash
set -euo pipefail
output_file="${1:-test-pattern.mp4}"
if [[ -e "$output_file" ]]; then
  echo "Refusing to overwrite $output_file" >&2
  exit 1
fi
ffmpeg -hide_banner -loglevel error -nostdin -f lavfi -i 'testsrc2=size=320x250:rate=10' -t 5 -c:v libx264 -pix_fmt yuv420p -movflags +faststart "$output_file"
echo "Created $output_file. Upload this file through the Library page."
