#!/usr/bin/env bash

set -e

SOURCE="emerald.c"
BINARY="./emerald"
GIF="emerald.gif"
TAPE="emerald.tape"

echo "Compiling $SOURCE..."

cc -std=gnu11 \
    -O2 \
    "$SOURCE" \
    -o "$BINARY" \
    -lm

echo "Creating VHS recording script..."

cat > "$TAPE" <<'EOF'
Output emerald.gif

Set Shell "bash"
Set FontSize 18
Set Width 960
Set Height 540
Set Framerate 30

Type "./emerald"
Enter

Sleep 5s

Ctrl+C

Sleep 500ms
EOF

echo "Recording GIF..."

vhs "$TAPE"

rm "$TAPE"

echo
echo "Generated:"
echo "  $GIF"
