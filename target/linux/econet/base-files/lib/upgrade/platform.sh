RAMFS_COPY_BIN='tr'

case "$(board_name)" in
tplink,archer-xr500v-v1)
	REQUIRE_IMAGE_METADATA=1
	;;
esac

xr500v_upgrade_fail() {
	rm -f /tmp/xr500v-kernel1.bin /tmp/xr500v-rootfs1.bin
	echo "XR500v upgrade aborted: $*" >&2
	exit 1
}

xr500v_image_hex() {
	local image="$1"
	local offset="$2"
	local count="$3"

	dd if="$image" bs=1 skip="$offset" count="$count" 2>/dev/null |
		hexdump -v -e '1/1 "%02x"'
}

xr500v_write_verify() {
	local image="$1"
	local partition="$2"
	local index device size blocks image_md5 flash_md5

	mtd -f write "$image" "$partition" ||
		xr500v_upgrade_fail "write failed for $partition"

	index=$(find_mtd_index "$partition")
	[ -n "$index" ] ||
		xr500v_upgrade_fail "cannot resolve $partition after writing"
	device="/dev/mtd$index"
	size=$(wc -c < "$image")
	[ $((size % 131072)) -eq 0 ] ||
		xr500v_upgrade_fail "$partition image is not eraseblock aligned"
	blocks=$((size / 131072))
	image_md5=$(md5sum "$image")
	image_md5=${image_md5%% *}
	flash_md5=$(dd if="$device" bs=131072 count="$blocks" 2>/dev/null |
		md5sum)
	flash_md5=${flash_md5%% *}
	if [ -z "$image_md5" ] || [ "$image_md5" != "$flash_md5" ]; then
		xr500v_upgrade_fail "readback verification failed for $partition"
	fi
}

platform_check_image() {
	local board
	local image="$1"
	local base_image base_size rootfs_payload payload_hex rootfs_hex
	local gap_nonzero squashfs_hex

	board=$(board_name)

	case "$board" in
	chinamobile,gs3101)
		return 0
		;;
	tplink,archer-xr500v-v1)
		base_image=$(mktemp /tmp/xr500v-image.XXXXXX) || return 1
		if ! fwtool -q -T -i /dev/null "$image" > "$base_image"; then
			rm -f "$base_image"
			echo "Invalid image: OpenWrt metadata trailer is missing or corrupt"
			return 1
		fi

		base_size=$(wc -c < "$base_image") || {
			rm -f "$base_image"
			return 1
		}
		rootfs_payload=$((base_size - 0x300200))
		if [ "$rootfs_payload" -lt 96 ] ||
		   [ "$rootfs_payload" -gt $((0x1000000)) ]; then
			rm -f "$base_image"
			echo "Invalid image: rootfs payload does not fit rootfs1"
			return 1
		fi

		if [ "$(dd if="$base_image" bs=1 skip=$((0x300200)) count=4 \
			2>/dev/null | hexdump -v -e '1/1 "%02x"')" != "68737173" ]; then
			rm -f "$base_image"
			echo "Invalid image: no squashfs at 0x300200"
			return 1
		fi

		payload_hex=$(printf '%08x' $((base_size - 0x200)))
		rootfs_hex=$(printf '%02x%02x%02x%02x00000000' \
			$((rootfs_payload & 0xff)) \
			$(((rootfs_payload >> 8) & 0xff)) \
			$(((rootfs_payload >> 16) & 0xff)) \
			$(((rootfs_payload >> 24) & 0xff)))
		squashfs_hex=$(xr500v_image_hex "$base_image" \
			$((0x300200 + 40)) 8)
		if [ "$squashfs_hex" != "$rootfs_hex" ]; then
			rm -f "$base_image"
			echo "Invalid image: SquashFS bytes_used does not match payload"
			return 1
		fi

		gap_nonzero=$(dd if="$base_image" bs=1 skip=$((0x300000)) \
			count=$((0x200)) 2>/dev/null | tr -d '\000' | wc -c)
		if [ "$gap_nonzero" -ne 0 ]; then
			rm -f "$base_image"
			echo "Invalid image: gap at 0x300000 is not all zero"
			return 1
		fi

		if [ "$(xr500v_image_hex "$base_image" $((0x00)) 13)" != \
		     "030000007665722e20322e3000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x34)) 8)" != \
		     "0ec6000100000001" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x60)) 8)" != \
		     "4c3d2e1faa55aa55" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x68)) 4)" != "80020000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x6c)) 4)" != "80020000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x70)) 4)" != "01300000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x74)) 4)" != "00000200" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x78)) 4)" != "$payload_hex" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x7c)) 4)" != "00300000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x80)) 4)" != "01000000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x88)) 4)" != "00000000" ] ||
		   [ "$(xr500v_image_hex "$base_image" $((0x8c)) 8)" != \
		     "55aa0101a5000000" ]; then
			rm -f "$base_image"
			echo "Invalid image: incomplete XR500v TrendChip header"
			return 1
		fi

		rm -f "$base_image"
		return 0
		;;
	esac

	return 1
}

platform_do_upgrade() {
	local board
	local kernel_image=/tmp/xr500v-kernel1.bin
	local rootfs_image=/tmp/xr500v-rootfs1.bin

	board=$(board_name)

	case "$board" in
	chinamobile,gs3101)
		CI_KERNPART="tclinux_kernel"
		nand_do_upgrade "$1"
		;;
	tplink,archer-xr500v-v1)
		[ -z "$UPGRADE_BACKUP" ] ||
			xr500v_upgrade_fail "the first persistent installation requires sysupgrade -n"

		# fwtool metadata is for sysupgrade validation only.  Strip it before
		# slicing so the hardware receives the exact BLDR-validated container.
		fwtool -q -t -i /dev/null "$1" ||
			xr500v_upgrade_fail "could not strip the metadata trailer"

		rm -f "$kernel_image" "$rootfs_image"
		dd if="$1" of="$kernel_image" bs=131072 count=24 2>/dev/null ||
			xr500v_upgrade_fail "could not extract kernel1"
		[ "$(wc -c < "$kernel_image")" -eq $((0x300000)) ] ||
			xr500v_upgrade_fail "kernel1 slice is not exactly 3 MiB"

		# Fill all of rootfs1 with 0xff before overlaying the squashfs payload.
		# This erases stale JFFS2/config data from earlier installations.
		dd if=/dev/zero bs=131072 count=128 2>/dev/null |
			tr '\000' '\377' > "$rootfs_image" ||
			xr500v_upgrade_fail "could not allocate the rootfs1 image"
		dd if="$1" of="$rootfs_image" bs=512 skip=6145 \
			conv=notrunc 2>/dev/null ||
			xr500v_upgrade_fail "could not extract rootfs1"
		[ "$(wc -c < "$rootfs_image")" -eq $((0x1000000)) ] ||
			xr500v_upgrade_fail "rootfs1 slice is not exactly 16 MiB"

		# Rootfs first, then the boot-critical kernel.  Every write is read back
		# before continuing; xr500v_upgrade_fail exits stage2 on any mismatch.
		xr500v_write_verify "$rootfs_image" rootfs1
		xr500v_write_verify "$kernel_image" kernel1

		sync
		rm -f "$kernel_image" "$rootfs_image"
		return 0
		;;
	esac
}
