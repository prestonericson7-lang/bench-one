#!/bin/bash
# inspect_rootfs.sh -- what is actually in the rootfs tree and in the ext4 image (run inside WSL as root)
R=/root/zynq/rootfs; P2=/root/zynq/sdbuild/p2.img
echo "=== tree: $R"; ls -la "$R" | head -24
echo "--- merged-usr links:"; for l in lib bin sbin; do printf "%s -> %s\n" "$l" "$(readlink "$R/$l")"; done
echo "--- init + loader:"
ls -la "$R/usr/sbin/init" "$R/usr/lib/systemd/systemd" "$R/usr/bin/dash" "$R/usr/lib/ld-linux-armhf.so.3" "$R/usr/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3" 2>&1
file "$R/usr/lib/systemd/systemd" "$R/usr/bin/dash" "$R/usr/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3" 2>&1 | cut -c1-150
echo "--- interpreter requested by dash:"; readelf -l "$R/usr/bin/dash" 2>/dev/null | grep -i interpreter
echo "--- debootstrap leftover:"; ls "$R/debootstrap" 2>&1 | head -3
echo "--- dpkg:"; grep -c "^Package:" "$R/var/lib/dpkg/status"; grep -c "Status: install ok installed" "$R/var/lib/dpkg/status"; grep -c "Status: install ok unpacked" "$R/var/lib/dpkg/status"
echo "=== ext4 image $P2 ==="
for f in /lib /sbin/init /usr/lib/systemd/systemd /usr/lib/ld-linux-armhf.so.3 /usr/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3 /usr/bin/dash /lib/firmware/pz7020_ps7_top.bin; do
  printf "%-52s " "$f"; debugfs -R "stat $f" "$P2" 2>/dev/null | grep -E "^Inode:|^Fast link dest|^Size" | tr '\n' ' ' | cut -c1-120; echo
done
