#!/bin/sh
# Sign the application with the existing stable development identity so macOS keeps TCC grants
# (for example access to the game volume) across rebuilds. An ad-hoc signature changes its hash
# with every build and makes macOS ask again every launch.
#
# The identity is created once in the login keychain. Without it, sign the entire bundle
# ad-hoc as well: the linker's executable-only signature does not seal Info.plist/resources.

app="$1"
identity="6C9E7575E75C68304DFF7F12C5E3BA23E4A11CEB"

if [ -z "$app" ] || [ ! -d "$app" ]; then
    exit 0
fi

# A local self-signed certificate can sign the bundle even if find-identity -v does not
# classify it as a trusted identity. Look up its fingerprint without depending on its old label.
if ! security find-certificate -a -Z "$HOME/Library/Keychains/login.keychain-db" 2>/dev/null | grep -Fq "$identity"; then
    echo "sign-app: no stable development identity; using an ad-hoc bundle signature"
    identity="-"
fi

# ExFAT volumes add AppleDouble sidecars for extended attributes; they are unsealed content in
# the bundle and make codesign refuse to seal it.
find "$app" -name '._*' -delete 2>/dev/null
xattr -cr "$app" 2>/dev/null

if codesign --force --sign "$identity" --timestamp=none "$app"; then
    echo "sign-app: signed $app"
else
    echo "sign-app: signing $app failed" >&2
    exit 1
fi

exit 0
