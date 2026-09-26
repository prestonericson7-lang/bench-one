# boot.cmd -- U-Boot boot script for the PZ7020-StarLite SD card with the FPGA-GPU (compiled to
# boot.scr by zynq/make_sd_image.sh with U-Boot's mkimage). It is the board's boot.cmd with two
# FPGA-GPU additions, both marked "FPGA-GPU":
#  1. fdt_high: by default U-Boot copies the device tree to the top of RAM, i.e. into
#     0x1E000000..0x1FFFFFFF, the GPU's reserved-memory window. Linux memblock-reserves the DT
#     blob before it scans /reserved-memory, and a no-map region that overlaps an existing
#     reservation is refused (-EBUSY): the window would stay ordinary RAM and the GPU would write
#     over the kernel's memory. 0x1e000000 keeps the relocated device tree below the window.
#  2. fpgagpu.pl_loaded=1 on the kernel command line, only when "fpga loadb" succeeded: Linux's
#     zynq-fpga driver clears devcfg PCFG_DONE when it probes, so this is how fpgagpud learns
#     that U-Boot configured the PL (zynq/backend_hw.c).
# Partition 1 (FAT32): boot.bin u-boot.img boot.scr zImage zynq-pz7020-starlite.dtb [pl.bit]
# Partition 2 (ext4):  root filesystem
# If a PL bitstream named pl.bit is present it is loaded BEFORE Linux, so the fabric is alive
# from the first second. Absent file = plain PS boot.

setenv bootargs "console=ttyPS0,115200 earlycon root=/dev/mmcblk0p2 rw rootwait net.ifnames=0"

# FPGA-GPU: keep the device tree out of the GPU window 0x1E000000..0x1FFFFFFF
setenv fdt_high 0x1e000000

if load mmc 0:1 0x10000000 pl.bit; then
    echo "Loading PL bitstream pl.bit (${filesize} bytes)"
    if fpga loadb 0 0x10000000 ${filesize}; then
        # FPGA-GPU: tell fpgagpud that the PL is configured
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
