#!/usr/bin/env python3
"""SA Coop — сервер-посредник с кодами комнат (вместо Tailscale).

Хост открывает комнату (6 цифр), друг вводит этот код — сервер пересылает пакеты между ними.
Никаких аккаунтов, ничего не хранит. Нужен компьютер/VPS с открытым UDP-портом (по умолчанию 7777).

Запуск:  python3 relay.py [порт]
В моде: Android/data/com.rockstargames.gtasa/configs/net.sacoop.mod.ini → [Network] Relay=адрес:порт
"""
import socket, sys, time

MAGIC = b"SCRL"          # служебные пакеты посредника
ROOM_TTL = 30.0          # комната живёт, пока хост напоминает о ней (раз в 5 с)

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 7777
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", port))
    rooms = {}           # код -> {"host": addr, "client": addr|None, "seen": t}
    peer_room = {}       # addr -> код
    print(f"SA Coop relay на UDP {port}", flush=True)
    while True:
        data, addr = s.recvfrom(2048)
        now = time.time()
        if data[:4] == MAGIC and len(data) >= 11:
            kind = data[4:5]; room = data[5:11].decode("ascii", "ignore")
            if kind == b"H":                       # хост открыл/держит комнату
                r = rooms.get(room)
                if r is None or r["host"] != addr:
                    rooms[room] = r = {"host": addr, "client": r["client"] if r else None, "seen": now}
                    print(f"комната {room}: хост {addr}", flush=True)
                r["seen"] = now; peer_room[addr] = room
                s.sendto(MAGIC + b"O" + room.encode(), addr)
            elif kind == b"C":                     # друг заходит по коду
                r = rooms.get(room)
                if r is None or now - r["seen"] > ROOM_TTL:
                    s.sendto(MAGIC + b"N" + room.encode(), addr); continue
                if r["client"] != addr:
                    r["client"] = addr; print(f"комната {room}: друг {addr}", flush=True)
                peer_room[addr] = room
                s.sendto(MAGIC + b"O" + room.encode(), addr)
            continue
        room = peer_room.get(addr)
        r = rooms.get(room) if room else None
        if not r: continue
        dst = r["client"] if addr == r["host"] else r["host"] if addr == r["client"] else None
        if dst: s.sendto(data, dst)
        # чистка старых комнат
        for code in [c for c, v in rooms.items() if now - v["seen"] > ROOM_TTL * 4]:
            v = rooms.pop(code)
            peer_room.pop(v["host"], None)
            if v["client"]: peer_room.pop(v["client"], None)

if __name__ == "__main__":
    main()
