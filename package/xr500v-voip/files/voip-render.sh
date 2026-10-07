#!/bin/sh
# Render only generated runtime files. Never modify stored SIP accounts.
set -eu
line=${1:?line}
ip=${2:?LAN address}
source=${3:-/etc/xr500v-voip}
runtime=${4:-/var/run/xr500v-voip}
case "$line" in 1|2) ;; *) exit 2 ;; esac
case "$ip" in ''|*[!0-9.]*) exit 2 ;; esac
case "$source:$runtime" in /*:/*) ;; *) exit 2 ;; esac
umask 077
source=$(readlink -f "$source")
mkdir -p "$runtime"
runtime=$(readlink -f "$runtime")
case "$runtime/" in "$source/"*) exit 2 ;; esac
cfg="$runtime/phone$line"
stored="$source/phone$line"
device=/dev/xr500v-voice
[ "$line" = 2 ] && device=/dev/xr500v-voice1
sip=$((5058 + line * 2))
ctrl=$((4443 + line))
umask 077
[ ! -L "$cfg" ] || exit 2
mkdir -p "$cfg"
for name in config accounts contacts speeddial dialing audio_device; do
	[ ! -L "$cfg/$name" ] || exit 2
done
base="$source/config"
[ ! -f "$stored/config" ] || base="$stored/config"
# Preserve unrelated settings. These fields belong to the physical line.
sed -e '/^[[:space:]]*sip_listen[[:space:]]/d' \
    -e '/^[[:space:]]*ctrl_tcp_listen[[:space:]]/d' \
    -e '/^[[:space:]]*audio_source[[:space:]]/d' \
    -e '/^[[:space:]]*audio_player[[:space:]]/d' \
    -e '/^[[:space:]]*ring_aufile[[:space:]]/d' \
    -e '/^[[:space:]]*callwaiting_aufile[[:space:]]/d' "$base" > "$cfg/config"
# Legacy (unmanaged) lines listen on the LAN address only, never on 0.0.0.0.
printf '\nsip_listen %s:%s\nctrl_tcp_listen 127.0.0.1:%s\naudio_source xr500v,%s\naudio_player xr500v,%s\nring_aufile none\ncallwaiting_aufile none\n' \
	"$ip" "$sip" "$ctrl" "$device" "$device" >> "$cfg/config"
if [ -f "$stored/accounts" ]; then
	cp "$stored/accounts" "$cfg/accounts"
else
	sed "s/@LANIP@/$ip/g" "$stored/accounts.template" > "$cfg/accounts"
fi
if [ -f "$stored/contacts" ]; then
	cp "$stored/contacts" "$cfg/contacts"
else
	cp "$source/contacts" "$cfg/contacts"
fi
# No lab autodial: use only an explicitly configured per-line speed dial.
if [ -f "$stored/speeddial" ]; then
	cp "$stored/speeddial" "$cfg/speeddial"
else
	rm -f "$cfg/speeddial"
fi
chmod 600 "$cfg/config" "$cfg/accounts" "$cfg/contacts"
# Never retain a keypad profile or a stale shared-ring-group jack selection
# left by a previous managed configuration.
rm -f "$cfg/dialing" "$cfg/audio_device" "$cfg/audio_device.tmp"
