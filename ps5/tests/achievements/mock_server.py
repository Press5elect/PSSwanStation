#!/usr/bin/env python3
"""PSSwanStation - a stand-in for RetroAchievements' server, for the
achievements test.

The test must not talk to retroachievements.org, so this answers the requests
rcheevos 12.5 makes (src/rapi in its sources names them and the JSON it
reads): login2, gameid, achievementsets, patch, startsession,
awardachievement, submitlbentry and ping on /dorequest.php, with the
parameters in the query (the console has HTTP GET only) or in a POST body,
and the badge pictures under /Badge/. Errors come as the real server sends
them: an HTTP status and a JSON body with "Error" and "Code".

It knows one user and one game: three official achievements, one unofficial
one, one leaderboard and a rich presence script, over the memory addresses
the test pokes. What the user earned is remembered until the server ends.

/control lets the test set it up and ask what it saw:
  op=known&hash=H      the game's disc has this hash
  op=down&value=1|0    drop every connection without an answer, or stop that
  op=slow&value=S      wait S seconds before each answer
  op=forget            the user has earned nothing
  op=get&key=K         a count or a value, as text (see Handler.control)

  mock_server.py --port-file FILE      serve on a free port, written to FILE
  mock_server.py --hash IMAGE.cue      print the image's RetroAchievements
                                       hash, computed here from the hash's
                                       definition, and end

Standard library only.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

USER = {"name": "Tester", "password": "s3cret p&ss/1", "token": "tok3nABC123xyz", "score": 1200, "softcore": 340}

GAME_ID = 4242
GAME_TITLE = "Swan Test Disc"
PLAYSTATION = 12

# id, title, description, flags (3 official, 5 unofficial), points, definition, badge
ACHIEVEMENTS = [
    (1001, "First Step", "Set the first flag", 3, 5, "0xH000010=1", "90001"),
    (1002, "Collector", "Collect five things", 3, 10, "M:0xH000011>=5", "90002"),
    (1003, "Triple", "Hold the scratchpad flag for ten frames", 3, 25, "M:0xH200010=1.10.", "90003"),
    (1004, "Draft", "Not published yet", 5, 1, "0xH000014=1_T:0xH000015=1", "90004"),
]
LEADERBOARD = (2001, "Speed Run", "Finish with the highest value",
               "STA:0xH000020=1::CAN:0xH000020=2::SUB:0xH000020=3::VAL:0xH000021", "VALUE")
RICH_PRESENCE = "Display:\r\nLevel @Number(0xH000030)\r\n"

lock = threading.Lock()
state = {
    "hashes": set(),
    "down": False,
    "slow": 0.0,
    "softcore": {},		# achievement id: when it was earned
    "hardcore": {},
    "awards": [],		# (id, hardcore, seconds late) of accepted unlock calls
    "best": None,
    "entries": [],		# scores submitted
    "requests": {},		# request name: how often
    "badges": {},		# path: how often
    "badge_now": 0,
    "badge_most": 0,	# most badge requests at one time
    "refused": 0,		# bad signatures and unknown requests
    "ping": "",			# the last ping's rich presence
    "looked_up": [],	# hashes asked for with gameid
}


def png(colour):
    """A 1 x 1 picture of one colour."""
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    header = struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(b"\x00" + bytes(colour))) + chunk(b"IEND", b""))


def md5(*parts):
    return hashlib.md5("".join(str(part) for part in parts).encode()).hexdigest()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send(self, status, kind, body):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def json(self, value, status=200):
        self.send(status, "application/json", json.dumps(value).encode())

    def error(self, status, message, code=None):
        body = {"Success": False, "Error": message, "Status": status}
        if code:
            body["Code"] = code
        self.json(body, status)

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        self.handle_request(self.rfile.read(length).decode())

    def do_GET(self):
        self.handle_request("")

    def handle_request(self, body):
        url = urlsplit(self.path)
        params = {key: values[0] for key, values in parse_qs(url.query, keep_blank_values=True).items()}
        params.update({key: values[0] for key, values in parse_qs(body, keep_blank_values=True).items()})
        if url.path == "/control":
            return self.control(params)
        with lock:
            down, slow = state["down"], state["slow"]
        if down:
            # No answer at all: the client sees a failed request.
            self.close_connection = True
            return None
        if slow:
            time.sleep(slow)
        if url.path == "/dorequest.php":
            return self.api(params)
        if url.path.startswith("/Badge/") and url.path.endswith(".png"):
            return self.badge(url.path)
        if url.path.startswith(("/UserPic/", "/Images/")):
            return self.send(200, "image/png", png((90, 90, 200)))
        return self.send(404, "text/plain", b"not here")

    def badge(self, path):
        with lock:
            state["badges"][path] = state["badges"].get(path, 0) + 1
            state["badge_now"] += 1
            state["badge_most"] = max(state["badge_most"], state["badge_now"])
        time.sleep(0.05)	# long enough for requests made at the same time to overlap
        with lock:
            state["badge_now"] -= 1
        name = os.path.basename(path)[:-4]
        if name.replace("_lock", "") not in [a[6] for a in ACHIEVEMENTS]:
            return self.send(404, "text/plain", b"no such badge")
        return self.send(200, "image/png", png((80, 80, 80) if name.endswith("_lock") else (240, 180, 40)))

    def control(self, params):
        op = params.get("op", "")
        answer = "ok"
        with lock:
            if op == "known":
                state["hashes"].add(params["hash"].lower())
            elif op == "down":
                state["down"] = params["value"] == "1"
            elif op == "slow":
                state["slow"] = float(params["value"])
            elif op == "forget":
                state["softcore"].clear()
                state["hardcore"].clear()
                del state["awards"][:]
            elif op == "get":
                answer = str(self.value(params["key"]))
            else:
                answer = "unknown"
        return self.send(200, "text/plain", answer.encode())

    @staticmethod
    def value(key):
        """Called with the lock held."""
        kind, _, rest = key.partition(":")
        if kind == "award":			# award:ID:HARDCORE - accepted unlock calls
            ident, hard = rest.split(":")
            return sum(1 for a in state["awards"] if a[0] == int(ident) and a[1] == int(hard))
        if kind == "late":			# late:ID - accepted unlock calls that said how late they are
            return sum(1 for a in state["awards"] if a[0] == int(rest) and a[2] is not None)
        if kind == "requests":		# requests:NAME
            return state["requests"].get(rest, 0)
        if kind == "entries":		# the scores submitted, in order
            return ",".join(str(score) for score in state["entries"])
        if kind == "badge_most":
            return state["badge_most"]
        if kind == "badge_repeats":	# badge pictures asked for more than once
            return sum(1 for count in state["badges"].values() if count > 1)
        if kind == "badge_count":
            return len(state["badges"])
        if kind == "refused":
            return state["refused"]
        if kind == "ping":
            return state["ping"]
        if kind == "looked_up":
            return ",".join(state["looked_up"])
        return "unknown"

    def refuse(self, status, message, code=None):
        with lock:
            state["refused"] += 1
        return self.error(status, message, code)

    def api(self, p):
        name = p.get("r", "")
        with lock:
            state["requests"][name] = state["requests"].get(name, 0) + 1
        host = "http://" + self.headers.get("Host", "127.0.0.1")

        if name == "login2":
            right = p.get("u", "").lower() == USER["name"].lower() and (
                ("p" in p and p["p"] == USER["password"]) or ("t" in p and p["t"] == USER["token"]))
            if not right:
                return self.error(401, "Invalid User/Password combination. Please try again", "invalid_credentials")
            with lock:
                return self.json({
                    "Success": True, "User": USER["name"], "AvatarUrl": host + "/UserPic/" + USER["name"] + ".png",
                    "Token": USER["token"], "Score": USER["score"], "SoftcoreScore": USER["softcore"],
                    "Messages": 0, "Permissions": 1, "AccountType": "Registered"})

        if name == "gameid":
            wanted = p.get("m", "").lower()
            with lock:
                state["looked_up"].append(wanted)
                return self.json({"Success": True, "GameID": GAME_ID if wanted in state["hashes"] else 0})

        # Everything else is for a user who is logged in.
        if p.get("u", "").lower() != USER["name"].lower() or p.get("t") != USER["token"]:
            return self.error(401, "Invalid user/token combination.", "invalid_credentials")

        if name in ("achievementsets", "patch"):
            with lock:
                known = p.get("g") == str(GAME_ID) or p.get("m", "").lower() in state["hashes"]
            if not known:
                return self.error(404, "Unknown game", "not_found")
            achievements = []
            for ident, title, description, flags, points, definition, badge in ACHIEVEMENTS:
                entry = {
                    "ID": ident, "Title": title, "Description": description, "Flags": flags, "Points": points,
                    "MemAddr": definition, "Author": "Mock", "BadgeName": badge, "Created": 1700000000,
                    "Modified": 1700000000, "Type": None, "Rarity": 50.0, "RarityHardcore": 25.0}
                # The first two name their pictures as the server does now; for
                # the others rcheevos makes the address from the badge's name.
                if ident in (1001, 1002):
                    entry["BadgeURL"] = "%s/Badge/%s.png" % (host, badge)
                    entry["BadgeLockedURL"] = "%s/Badge/%s_lock.png" % (host, badge)
                achievements.append(entry)
            ident, title, description, definition, form = LEADERBOARD
            leaderboards = [{"ID": ident, "Title": title, "Description": description, "Mem": definition,
                             "Format": form, "LowerIsBetter": False, "Hidden": False}]
            icon = host + "/Images/004242.png"
            if name == "patch":
                return self.json({"Success": True, "PatchData": {
                    "ID": GAME_ID, "Title": GAME_TITLE, "ConsoleID": PLAYSTATION, "ImageIcon": "/Images/004242.png",
                    "ImageIconURL": icon, "RichPresencePatch": RICH_PRESENCE,
                    "Achievements": achievements, "Leaderboards": leaderboards}})
            return self.json({
                "Success": True, "GameId": GAME_ID, "Title": GAME_TITLE, "ConsoleId": PLAYSTATION,
                "ImageIconUrl": icon, "RichPresenceGameId": GAME_ID, "RichPresencePatch": RICH_PRESENCE,
                "Sets": [{"AchievementSetId": 9001, "GameId": GAME_ID, "Title": None, "Type": "core",
                          "ImageIconUrl": icon, "Achievements": achievements, "Leaderboards": leaderboards}]})

        if name == "startsession":
            if p.get("g") != str(GAME_ID):
                return self.refuse(404, "Unknown game", "not_found")
            with lock:
                return self.json({
                    "Success": True,
                    "Unlocks": [{"ID": i, "When": when} for i, when in sorted(state["softcore"].items())],
                    "HardcoreUnlocks": [{"ID": i, "When": when} for i, when in sorted(state["hardcore"].items())],
                    "ServerNow": int(time.time())})

        if name == "awardachievement":
            ident, hard, late = int(p.get("a", "0")), int(p.get("h", "0")), p.get("o")
            signature = md5(ident, p["u"], hard) if late is None else md5(ident, p["u"], hard, ident, late)
            official = {a[0]: a for a in ACHIEVEMENTS if a[3] == 3}
            if ident not in official or p.get("v") != signature:
                return self.refuse(422, "The unlock was not accepted")
            with lock:
                earned = state["hardcore"] if hard else state["softcore"]
                had = ident in earned
                if not had:
                    earned[ident] = int(time.time())
                    if hard:
                        state["softcore"].setdefault(ident, earned[ident])
                    USER["score" if hard else "softcore"] += official[ident][4]
                state["awards"].append((ident, hard, late))
                answer = {"Success": not had, "Score": USER["score"], "SoftcoreScore": USER["softcore"],
                          "AchievementID": ident, "AchievementsRemaining": len(official) - len(earned)}
                if had:
                    answer["Error"] = "User already has this achievement awarded."
                return self.json(answer)

        if name == "submitlbentry":
            ident, score, late = int(p.get("i", "0")), int(p.get("s", "0")), p.get("o")
            signature = md5(ident, p["u"], score) if late is None else md5(ident, p["u"], score, late)
            if ident != LEADERBOARD[0] or p.get("v") != signature:
                return self.refuse(422, "The score was not accepted")
            with lock:
                state["entries"].append(score)
                state["best"] = score if state["best"] is None else max(state["best"], score)
                return self.json({"Success": True, "Response": {
                    "Score": score, "BestScore": state["best"],
                    "RankInfo": {"Rank": 2, "NumEntries": "7"},
                    "TopEntries": [{"User": "Somebody", "Rank": 1, "Score": 255},
                                   {"User": USER["name"], "Rank": 2, "Score": state["best"]}]}})

        if name == "ping":
            with lock:
                state["ping"] = p.get("m", "")
            return self.json({"Success": True})

        return self.refuse(400, "Unknown Request: '%s'" % name)


def playstation_hash(cue):
    """RetroAchievements' hash of a PlayStation disc, from its definition: the
    MD5 of the boot executable's name (as SYSTEM.CNF's BOOT line gives it) and
    of its content, the 2048-byte header and as many bytes as the header says
    follow it."""
    image = os.path.splitext(cue)[0] + ".bin"
    with open(image, "rb") as f:
        data = f.read()

    def sector(number):		# MODE2/2352: 24 bytes before the 2048 of user data
        start = number * 2352 + 24
        return data[start:start + 2048]

    def find(name):
        volume = sector(16)
        number = int.from_bytes(volume[158:162], "little")
        directory, at = sector(number), 0
        while at < 2048 and directory[at]:
            length = directory[at]
            entry = directory[at + 33:at + 33 + directory[at + 32]].decode("ascii", "replace")
            if entry.split(";")[0].upper() == name.upper():
                return (int.from_bytes(directory[at + 2:at + 6], "little"),
                        int.from_bytes(directory[at + 10:at + 14], "little"))
            at += length
        raise SystemExit("%s: no %s" % (image, name))

    where, size = find("SYSTEM.CNF")
    boot = None
    for line in sector(where)[:size].decode("ascii", "replace").splitlines():
        key, _, value = line.partition("=")
        if key.strip() == "BOOT":
            boot = value.strip().replace("cdrom:", "").lstrip("\\").split(";")[0]
    where, size = find(boot)
    header = sector(where)
    if header[:8] == b"PS-X EXE":
        size = int.from_bytes(header[28:32], "little") + 2048
    content = b"".join(sector(where + i) for i in range((size + 2047) // 2048))[:size]
    return hashlib.md5(boot.encode() + content).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port-file")
    parser.add_argument("--hash")
    args = parser.parse_args()
    if args.hash:
        print(playstation_hash(args.hash))
        return 0
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    if args.port_file:
        with open(args.port_file + ".tmp", "w") as f:
            f.write(str(server.server_address[1]))
        os.replace(args.port_file + ".tmp", args.port_file)
    print("mock RetroAchievements server on 127.0.0.1:%d" % server.server_address[1], flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
