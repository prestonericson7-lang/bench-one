#!/bin/bash
# qemu_kill_port.sh SIGNAL PORT -- signal the QEMU whose console listens on TCP port PORT (qemu_serial_tcp.sh's
# 0.0.0.0 or qemu_uboot_test.sh's 127.0.0.1). The [t] keeps pkill from matching its own command line.
#     bash qemu_kill_port.sh KILL 5566      bash qemu_kill_port.sh STOP 5561
pkill -"$1" -f "[t]cp:[0-9.]*:$2,server"
