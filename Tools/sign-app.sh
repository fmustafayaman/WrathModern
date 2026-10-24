#!/bin/sh
# Sign the application with the existing stable development identity so macOS keeps TCC grants
# (for example access to the game volume) across rebuilds. An ad-hoc signature changes its hash
# with every build and makes macOS ask again every launch.
#
# The identity is created once in the login keychain; see AGENTS.md. Without it this script does
# nothing, and the build stays ad-hoc.

app="$1"
identity="6C9E7575E75C68304DFF7F12C5E3BA23E4A11CEB"

if [ -z "$app" ] || [ ! -d "$app" ]; then
    exit 0
fi

if ! security find-identity -v -p codesigning "$HOME/Library/Keychains/login.keychain-db" 2>/dev/null | grep -q "$identity"; then
    echo "sign-app: no stable development identity; leaving $app ad-hoc"
    exit 0
fi

# ExFAT volumes add AppleDouble sidecars for extended attributes; they are unsealed content in
# the bundle and make codesign refuse to seal it.
find "$app" -name '._*' -delete 2>/dev/null
xattr -cr "$app" 2>/dev/null

if codesign --force --sign "$identity" --timestamp=none "$app"; then
    echo "sign-app: signed $app"
else
    echo "sign-app: signing $app failed; leaving ad-hoc" >&2
fi

exit 0
