#!/usr/bin/env bash
# Creates a new pronao project from examples/template:
#   bash scripts/new_project.sh <folder> [name]
# The folder must not exist or must be empty. Then:  cd <folder> && bash build.sh
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: new_project.sh <folder> [name]" >&2; exit 2; }
DIR="$1"
NAME="${2:-$(basename "$DIR")}"
case "$NAME" in
    [A-Za-z]*) ;;
    *) echo "the name must start with a letter: $NAME" >&2; exit 2 ;;
esac
case "$NAME" in
    *[!A-Za-z0-9_-]*) echo "the name may contain only letters, digits, - and _: $NAME" >&2; exit 2 ;;
esac
if [ -e "$DIR" ] && [ -n "$(ls -A "$DIR" 2>/dev/null)" ]; then echo "$DIR exists and is not empty" >&2; exit 1; fi
mkdir -p "$DIR"
for f in main.clg index.html build.sh README.md; do
    sed -e "s|__NAME__|$NAME|g" -e "s|__PRONAO__|$ROOT|g" "$ROOT/examples/template/$f" > "$DIR/$f"
done
cp "$ROOT/examples/template/gitignore.txt" "$DIR/.gitignore"
python3 "$ROOT/scripts/make_icon.py" "$DIR/icon.ico" "$NAME" || echo "(no placeholder icon: python3 was not found; the program will show the system's default icon)" >&2
chmod +x "$DIR/build.sh"
echo "created $DIR ($NAME). Next:  cd $DIR && bash build.sh"
