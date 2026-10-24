#!/bin/sh
# Runs the client unattended and collects its screenshots (see WoW/Source/Autotest.h).
#
#   Tools/autotest.sh <output dir> <shots> [timeout seconds]
#     shots: e.g. "glue@10" (login screen only) or "glue@10,world@15,world@25" (logs in)
#
# Environment:
#   WOWRE_APP       application to run (default: $GAME_DIR/WoWReMac.app)
#   GAME_DIR        game directory (default: /Volumes/Yaman SSD/WoW_Wrath)
#   WOWRE_LOGIN     credentials file for the world shots (default: ~/.wrathmodern-wowre-autologin)
#   WOWRE_REALMLIST, WOWRE_REALM, WOWRE_CHARACTER   server, realm and character for the world shots
#   WRATHMODERN_AUTOTEST_NO_ADDONS=1               disable Lua addons for this run (WTF restored below)
#
# The player's WTF folder is restored afterwards, and only the process this script started is
# stopped, so a game the player is running is never touched.

set -u

out="${1:?output directory}"
shots="${2:?shots}"
timeout="${3:-240}"
game="${GAME_DIR:-/Volumes/Yaman SSD/WoW_Wrath}"
app="${WOWRE_APP:-$game/WoWReMac.app}"
binary="$app/Contents/MacOS/WoWReMac"
# Read the bundle metadata rather than assuming a branch-specific executable name.
if [ -f "$app/Contents/Info.plist" ]; then
    executable="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist" 2>/dev/null || true)"
    [ -z "$executable" ] || binary="$app/Contents/MacOS/$executable"
fi

mkdir -p "$out"

# One run at a time: runs share the game directory's WTF and Screenshots folders and the account
lock="/tmp/wrathmodern-autotest.lock"
waited=0
while ! mkdir "$lock" 2>/dev/null; do
    if [ "$waited" -ge 900 ]; then
        echo "autotest: another run holds $lock; giving up"
        exit 2
    fi
    sleep 2
    waited=$((waited + 2))
done
trap 'rmdir "$lock" 2>/dev/null' EXIT

rm -f "$game"/Screenshots/autotest-*.tga

# Keep the player's settings: the client writes WTF on exit
backup="$(mktemp -d)"
cp -R "$game/WTF" "$backup/WTF"

restore() {
    rm -rf "$game/WTF"
    cp -R "$backup/WTF" "$game/WTF"
    rm -rf "$backup"
}

case "$shots" in
    *world*) login=1 ;;
    *) login=0 ;;
esac

env WRATHMODERN_SHOTS="$shots" WRATHMODERN_EXIT_AFTER_SHOTS=1 \
    WRATHMODERN_AUTOLOGIN="$login" \
    WRATHMODERN_AUTOLOGIN_FILE="${WOWRE_LOGIN:-$HOME/.wrathmodern-wowre-autologin}" \
    WRATHMODERN_REALMLIST="${WOWRE_REALMLIST:-100.71.234.118}" \
    WRATHMODERN_REALM="${WOWRE_REALM:-Azeroth}" \
    WRATHMODERN_CHARACTER="${WOWRE_CHARACTER:-Rhon}" \
    "$binary" >"$out/stdout.log" 2>&1 &
pid=$!

elapsed=0
while kill -0 "$pid" 2>/dev/null; do
    if [ "$elapsed" -ge "$timeout" ]; then
        echo "autotest: timed out after ${timeout}s; stopping $pid"
        kill "$pid" 2>/dev/null
        sleep 2
        kill -9 "$pid" 2>/dev/null
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

wait "$pid" 2>/dev/null
status=$?
restore

cp "$game/Logs/autotest.log" "$out/" 2>/dev/null
for log in Mods.log FrameXML.log GlueXML.log; do
    cp "$game/Logs/$log" "$out/" 2>/dev/null
done

count=0
for tga in "$game"/Screenshots/autotest-*.tga; do
    [ -f "$tga" ] || continue
    name="$(basename "$tga" .tga)"
    sips -s format png "$tga" --out "$out/${name#autotest-}.png" >/dev/null 2>&1
    rm -f "$tga"
    count=$((count + 1))
done

echo "autotest: exit $status, $count screenshot(s) in $out"
[ "$count" -gt 0 ]
