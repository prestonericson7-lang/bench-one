#!/bin/bash
# repair_rootfs_lib.sh -- undo the /lib-symlink clobber: GNU tar replaced the merged-usr symlink
# /lib -> usr/lib with a real directory when extracting lib/modules, which hid /lib/ld-linux-armhf.so.3
# from every dynamic binary ("No working init found"). Move the content under usr/lib and restore the link.
set -e
R=/root/zynq/rootfs
if [ -d "$R/lib" ] && [ ! -L "$R/lib" ]; then
  mkdir -p "$R/usr/lib/firmware" "$R/usr/lib/modules"
  [ -d "$R/lib/modules" ]  && cp -a "$R/lib/modules/."  "$R/usr/lib/modules/"
  [ -d "$R/lib/firmware" ] && cp -a "$R/lib/firmware/." "$R/usr/lib/firmware/"
  rm -rf "$R/lib"
  ln -s usr/lib "$R/lib"
  echo "restored: lib -> $(readlink "$R/lib")"
else
  echo "lib already a symlink -> $(readlink "$R/lib")"
fi
ls -la "$R/lib/ld-linux-armhf.so.3" "$R/lib/modules" "$R/lib/firmware" | head -8
