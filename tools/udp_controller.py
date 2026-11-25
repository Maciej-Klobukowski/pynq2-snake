"""
UDP-controller voor PYNQ2-Snake (Windows).

Gebruik (board automatisch zoeken, werkt op elk netwerk):
    python tools/udp_controller.py 5000                 # speler 1
    python tools/udp_controller.py 5001                 # speler 2
Of met een vast IP:
    python tools/udp_controller.py 192.168.0.196 5000

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
DISCOVERY_PORT = 5050          # het board broadcast hier elke seconde "PYNQSNAKE"
DISCOVERY_MSG = b"PYNQSNAKE"


def discover_board(timeout_s: float = 10.0) -> str:
    """Wacht op de broadcast van het board en geef zijn IP-adres terug."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", DISCOVERY_PORT))
    s.settimeout(timeout_s)
    print(f"Board zoeken op het netwerk (UDP {DISCOVERY_PORT})...")
    try:
        while True:
            data, (ip, _) = s.recvfrom(64)
            if data.startswith(DISCOVERY_MSG):
                print(f"Board gevonden op {ip}")
                return ip
    except socket.timeout:
        sys.exit("Geen board gevonden. Staat het board aan en zit je op hetzelfde netwerk?\n"
                 "Laat eventueel Python toe in de Windows Firewall, of geef het IP zelf op.")
    finally:
        s.close()


def heartbeat(sock: socket.socket, target: tuple, stop: threading.Event) -> None:
    """Stuur elke seconde 'J' zodat het board weet dat we er nog zijn."""
    while not stop.is_set():
        try:
            sock.sendto(b"J", target)
        except OSError:
            pass
        stop.wait(HEARTBEAT_S)


def main() -> None:
    args = sys.argv[1:]
    if len(args) >= 2:                      # IP + poort opgegeven
        ip, port = args[0], int(args[1])
    else:                                   # alleen poort (of niets): zelf zoeken
        port = int(args[0]) if args else 5000
        ip = discover_board()
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
