"""fake_ch340.py PORT -- a TCP 'serial device' that behaves like another CH340 board on the PC (an STM32
printing debug lines): chatter every second, junk back for anything it receives. It never prints anything
the experiment driver takes for the Zynq board."""
import socket, sys, threading, time

port = int(sys.argv[1])
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port)); srv.listen(1)
print(f"fake CH340 on {port}", flush=True)
c, _ = srv.accept()
print("client connected", flush=True)
stop = False


def rx():
    global stop
    while not stop:
        try:
            d = c.recv(1024)
        except OSError:
            return
        if not d:
            stop = True; return
        print(f"received {d!r}", flush=True)
        c.sendall(b"STM32H743> unknown command\r\n")


threading.Thread(target=rx, daemon=True).start()
n = 0
while not stop:
    try:
        c.sendall(b"STM32H743 tick %d adc=%d sdram ok\r\n" % (n, 1000 + n % 7))
    except OSError:
        break
    n += 1; time.sleep(1)
print("client gone after", n, "lines", flush=True)
