#!/bin/ash
set -eu

ARCHIVE_URL="https://pub-75c297bdc96c44508b1722f344be665f.r2.dev/rorserver244-linux.zip"
SERVER_DIR="/mnt/server"
TMP_DIR="$(mktemp -d /tmp/rorserver244-install.XXXXXX)"
ARCHIVE="$TMP_DIR/rorserver244-linux.zip"

cleanup() {
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT INT TERM

apk add --no-cache wget unzip binutils coreutils
mkdir -p "$SERVER_DIR"
wget -O "$ARCHIVE" "$ARCHIVE_URL"
unzip -t "$ARCHIVE"
unzip -q "$ARCHIVE" -d "$TMP_DIR/extracted"

if [ -f "$TMP_DIR/extracted/rorserver" ]; then
    PACKAGE_DIR="$TMP_DIR/extracted"
elif [ -f "$TMP_DIR/extracted/rorserver244-linux/rorserver" ]; then
    PACKAGE_DIR="$TMP_DIR/extracted/rorserver244-linux"
else
    echo "ERROR: archive contains neither flat nor rorserver244-linux/rorserver layout" >&2
    exit 1
fi

PERSISTENT_ROOT_FILES="
server.cfg
server.auth
authorizations.txt
admins.txt
banned-players.json
server.motd
motd.txt
server.rules
rules.txt
server.log
"

is_persistent_root_file() {
    candidate="$1"
    for persistent in $PERSISTENT_ROOT_FILES; do
        [ "$candidate" = "$persistent" ] && return 0
    done
    return 1
}

for source_entry in "$PACKAGE_DIR"/*; do
    [ -e "$source_entry" ] || continue
    entry="$(basename "$source_entry")"

    if is_persistent_root_file "$entry" && [ -e "$SERVER_DIR/$entry" ]; then
        echo "Preserving existing $entry"
        continue
    fi

    if [ "$entry" = "logs" ]; then
        if [ -d "$SERVER_DIR/logs" ]; then
            echo "Preserving existing logs/"
        else
            cp -a "$source_entry" "$SERVER_DIR/logs"
        fi
        continue
    fi

    if [ "$entry" = "resources" ]; then
        if [ -d "$SERVER_DIR/resources/scripts/storage" ]; then
            echo "Preserving existing resources/scripts/storage/"
            rm -rf "$source_entry/scripts/storage"
        fi
        mkdir -p "$SERVER_DIR/resources"
        cp -a "$source_entry"/. "$SERVER_DIR/resources"/
        continue
    fi

    if [ -d "$source_entry" ]; then
        mkdir -p "$SERVER_DIR/$entry"
        cp -a "$source_entry"/. "$SERVER_DIR/$entry"/
    else
        cp -f "$source_entry" "$SERVER_DIR/$entry"
    fi
done

[ -f "$SERVER_DIR/rorserver" ] || { echo "ERROR: rorserver was not installed" >&2; exit 1; }

chmod 755 "$SERVER_DIR/rorserver"
[ ! -f "$SERVER_DIR/RunRoR.sh" ] || chmod 755 "$SERVER_DIR/RunRoR.sh"
[ ! -f "$SERVER_DIR/libmysocketw.so" ] || chmod 755 "$SERVER_DIR/libmysocketw.so"
[ ! -d "$SERVER_DIR/resources" ] || find "$SERVER_DIR/resources" -type d -exec chmod 755 {} +
[ ! -d "$SERVER_DIR/resources" ] || find "$SERVER_DIR/resources" -type f -exec chmod 644 {} +

[ "$(stat -c '%a' "$SERVER_DIR/rorserver")" = "755" ] || { echo "ERROR: rorserver mode is not 755" >&2; exit 1; }
[ ! -f "$SERVER_DIR/RunRoR.sh" ] || [ "$(stat -c '%a' "$SERVER_DIR/RunRoR.sh")" = "755" ] || { echo "ERROR: RunRoR.sh mode is not 755" >&2; exit 1; }
[ ! -f "$SERVER_DIR/libmysocketw.so" ] || [ "$(stat -c '%a' "$SERVER_DIR/libmysocketw.so")" = "755" ] || { echo "ERROR: libmysocketw.so mode is not 755" >&2; exit 1; }

strings "$SERVER_DIR/rorserver" | grep -F 'RoRnet_2.44' >/dev/null
if strings "$SERVER_DIR/rorserver" | grep -F 'RoRnet_2.45' >/dev/null; then
    echo "ERROR: refusing to install a RoRNet 2.45 binary" >&2
    exit 1
fi

echo "Installed binary metadata:"
file "$SERVER_DIR/rorserver" 2>/dev/null || true
echo "Installed binary SHA256:"
sha256sum "$SERVER_DIR/rorserver"
if [ -f "$SERVER_DIR/libmysocketw.so" ]; then
    echo "SocketW SHA256:"
    sha256sum "$SERVER_DIR/libmysocketw.so"
fi
echo "Final server file listing:"
find "$SERVER_DIR" -maxdepth 3 -exec ls -ld {} \; | sort
echo "RoRNet 2.44 installation completed successfully."
