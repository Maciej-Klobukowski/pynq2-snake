"""
UDP-controller voor PYNQ2-Snake (Windows).

Gebruik:
    python tools/udp_controller.py 192.168.0.196 5000   # speler 1
    python tools/udp_controller.py 192.168.0.196 5001   # speler 2

Toetsen (AZERTY):
    pijltjes of Z/Q/S/D  -> richting (Z=omhoog, Q=links, S=omlaag, D=rechts)
    1 / 2                -> in het menu: 1 of 2 spelers (Q/D werkt ook)
    N                    -> terug naar het menu
    Esc                  -> stoppen (meldt je af bij het board)

Zolang het script draait, stuurt het elke seconde een heartbeat ("J").
Zo weet het board dat je verbonden bent: met 2 spelers start het spel pas
als beide controllers verbonden zijn.
"""
import socket
import sys
import threading

try:
    import msvcrt  # alleen op Windows
except ImportError:
    sys.exit("Dit script gebruikt msvcrt en werkt alleen op Windows.")

ARROWS = {"H": "U", "P": "D", "K": "L", "M": "R"}
# AZERTY: Z = omhoog, Q = links, S = omlaag, D = rechts
# 1/2 = aantal spelers in het menu (AZERTY zonder shift: & en é)
LETTERS = {"z": "U", "q": "L", "s": "D", "d": "R", "n": "N",
           "1": "1", "2": "2", "&": "1", "é": "2"}

HEARTBEAT_S = 1.0


def heartbeat(sock: socket.socket, target: tuple, stop: threading.Event) -> None:
    """Stuur elke seconde 'J' zodat het board weet dat we er nog zijn."""
    while not stop.is_set():
        try:
            sock.sendto(b"J", target)
        except OSError:
            pass
        stop.wait(HEARTBEAT_S)


def main() -> None:
    ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.196"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
    player = 1 if port == 5000 else 2
    target = (ip, port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.2)

    stop = threading.Event()
    threading.Thread(target=heartbeat, args=(sock, target, stop), daemon=True).start()

    print(f"Speler {player} -> {ip}:{port}  (verbonden, heartbeat elke {HEARTBEAT_S:.0f} s)")
    print("Menu: 1/Q = 1 speler, 2/D = 2 spelers")
    print("Spel: pijltjes/ZQSD = sturen, N = menu, Esc = stoppen")

    try:
        while True:
            ch = msvcrt.getwch()
            if ch in ("\x00", "\xe0"):          # speciale toets (pijltje)
                cmd = ARROWS.get(msvcrt.getwch())
            elif ch == "\x1b":                   # Esc
                break
            else:
                cmd = LETTERS.get(ch.lower())

            if not cmd:
                continue

            sock.sendto(cmd.encode(), target)
            try:
                reply, _ = sock.recvfrom(16)
                print(f"{cmd} -> {reply.decode(errors='replace').strip()}")
            except (socket.timeout, ConnectionResetError):
                print(f"{cmd} -> (geen antwoord)")
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        sock.sendto(b"X", target)               # afmelden
        sock.close()
        print("Afgemeld.")


if __name__ == "__main__":
    main()
