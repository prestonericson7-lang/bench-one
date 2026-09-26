# boot.cmd -- U-Boot boot script for the PZ7020-StarLite SD card (compiled to boot.scr by mkimage).
# Partition 1 (FAT32): boot.bin u-boot.img boot.scr zImage zynq-pz7020-starlite.dtb [pl.bit]
# Partition 2 (ext4):  root filesystem
#
# If a PL bitstream named pl.bit is present it is loaded BEFORE Linux, so the fabric (LEDs, fan
# PWM, the SDR accelerator on JM1) is alive from the first second. Absent file = plain PS boot.

setenv bootargs "console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0 uio_pdrv_genirq.of_id=generic-uio"
# Keep the device tree where it is loaded: relocated to the top of RAM it would land in the PL's
# reserved DDR3 (0x10000000-0x1FFFFFFF), which Linux does not map.
setenv fdt_high 0xffffffff
setenv initrd_high 0xffffffff
# The kernel (6.12 multi_v7) unpacks to physical 0x00208000 and its BSS ends at 0x01FF8B10, so U-Boot's
# default fdt_addr_r (0x1F00000) lies INSIDE it: the kernel wiped its own device tree and hung with no
# output (qemu_uboot_test.sh, 2026-09-26). zImage and DT go above the unpacked kernel, below 256 MB:
setenv kernel_addr_r 0x03000000
setenv fdt_addr_r 0x02e00000

# Linux's zynq-fpga driver clears devcfg PCFG_DONE when it probes, so after boot that bit cannot say
# whether the PL is configured. fpgagpu.pl_loaded=1 on the command line, only when "fpga loadb"
# succeeded, is how fpgagpud, zaccel-server and zynq-report learn it (with PCFG_INIT_NE still clear).
# pl.bit is staged at 0x08000000: inside Linux's 256 MB, clear of the kernel (0x2000000), the DT
# (0x1F00000) and the PL windows (engine 0x10000000-0x1DFFFFFF, GPU 0x1E000000-0x1FFFFFFF). U-Boot puts
# every reserved-memory region in its LMB and refuses to load a file into one (fs/fs.c, lib/lmb.c).
if load mmc 0:1 0x08000000 pl.bit; then
    echo "Loading PL bitstream pl.bit (${filesize} bytes)"
    if fpga loadb 0 0x08000000 ${filesize}; then
        setenv bootargs "${bootargs} fpgagpu.pl_loaded=1"
    else
        echo "fpga loadb of pl.bit FAILED -- PL not configured"
    fi
else
    echo "No pl.bit on the boot partition -- PL left unconfigured"
fi

load mmc 0:1 ${kernel_addr_r} zImage
load mmc 0:1 ${fdt_addr_r} zynq-pz7020-starlite.dtb
bootz ${kernel_addr_r} - ${fdt_addr_r}
