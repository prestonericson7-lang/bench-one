# boot.cmd -- U-Boot boot script for the PZ7020-StarLite SD card (compiled to boot.scr by mkimage).
# Partition 1 (FAT32): boot.bin u-boot.img boot.scr zImage zynq-pz7020-starlite.dtb [pl.bit]
# Partition 2 (ext4):  root filesystem
#
# If a PL bitstream named pl.bit is present it is loaded BEFORE Linux, so the fabric (LEDs, fan
# PWM, the SDR accelerator on JM1) is alive from the first second. Absent file = plain PS boot.

setenv bootargs "console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0 uio_pdrv_genirq.of_id=generic-uio"
# Keep the device tree where it is loaded: relocated to the top of RAM it would land in the PL's
# reserved DDR3 (0x1E000000-0x37FFFFFF), which Linux does not map.
setenv fdt_high 0xffffffff
setenv initrd_high 0xffffffff

# Linux's zynq-fpga driver clears devcfg PCFG_DONE when it probes, so after boot that bit cannot say
# whether the PL is configured. fpgagpu.pl_loaded=1 on the command line, only when "fpga loadb"
# succeeded, is how fpgagpud, zaccel-server and zynq-report learn it (with PCFG_INIT_NE still clear).
if load mmc 0:1 0x10000000 pl.bit; then
    echo "Loading PL bitstream pl.bit (${filesize} bytes)"
    if fpga loadb 0 0x10000000 ${filesize}; then
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
