#!/bin/sh
set -eu

source_bundle=$1
app_directory=$2
staging_root=${3:-}
bundle_id=dev.ced.Editor
plistbuddy=/usr/libexec/PlistBuddy
lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister

bundle_identifier() {
    "$plistbuddy" -c 'Print :CFBundleIdentifier' "$1/Contents/Info.plist" 2>/dev/null || true
}

launch_services() {
    # sudo installs must update the invoking user's Launch Services database.
    if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
        /usr/bin/sudo -u "$SUDO_USER" "$lsregister" "$@"
    else
        "$lsregister" "$@"
    fi
}

if [ "$(bundle_identifier "$source_bundle")" != "$bundle_id" ]; then
    echo "Invalid CED application bundle: $source_bundle" >&2
    exit 1
fi
install -d "$app_directory"
app_directory=$(cd "$app_directory" && pwd -P)
destination="$app_directory/CED.app"
if [ "$(cd "$source_bundle" && pwd -P)" = "$destination" ]; then
    echo "Source and installed CED bundle must be different paths." >&2
    exit 1
fi
if [ -e "$destination" ] || [ -L "$destination" ]; then
    if [ "$(bundle_identifier "$destination")" != "$bundle_id" ]; then
        echo "Refusing to replace a different application: $destination" >&2
        exit 1
    fi
fi

# Copy and verify before replacing the installed app. Keep the previous bundle
# available for rollback until the new one has been moved into place.
temporary=$(mktemp -d "$app_directory/.ced-install.XXXXXX")
cleanup() {
    if { [ -e "$temporary/previous.app" ] || [ -L "$temporary/previous.app" ]; } &&
       [ ! -e "$destination" ] && [ ! -L "$destination" ]; then
        if ! mv "$temporary/previous.app" "$destination"; then
            echo "Previous CED bundle preserved at $temporary/previous.app" >&2
            return
        fi
    fi
    rm -rf "$temporary"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
cp -R "$source_bundle" "$temporary/CED.app"
/usr/bin/codesign --verify --deep --strict "$temporary/CED.app"
if [ -e "$destination" ] || [ -L "$destination" ]; then
    mv "$destination" "$temporary/previous.app"
fi
if ! mv "$temporary/CED.app" "$destination"; then
    exit 1
fi

# Remove old installed copies by bundle identity, not by their display names.
for previous in "$app_directory"/*.app; do
    [ "$previous" = "$destination" ] && continue
    [ "$(bundle_identifier "$previous")" = "$bundle_id" ] || continue
    if [ -z "$staging_root" ]; then launch_services -u "$previous" || true; fi
    rm -rf "$previous"
done

if [ -z "$staging_root" ]; then
    # Development and packaging copies can also remain registered with macOS.
    # Unregister those paths without deleting source/build directories.
    launch_services -dump > "$temporary/registrations"
    awk -v target="$bundle_id" '
        /^-----/ { if (identifier == target && path != "") print path; identifier = ""; path = ""; next }
        /^path:/ { path = $0; sub(/^path:[[:space:]]*/, "", path); sub(/ \(0x[[:xdigit:]]+\)$/, "", path) }
        /^identifier:/ { identifier = $0; sub(/^identifier:[[:space:]]*/, "", identifier) }
        END { if (identifier == target && path != "") print path }
    ' "$temporary/registrations" > "$temporary/registered-paths"
    while IFS= read -r previous; do
        launch_services -u "$previous" || true
    done < "$temporary/registered-paths"
    launch_services -f "$destination"
fi

echo "Installed $destination (previous CED versions replaced)."
