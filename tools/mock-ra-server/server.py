#!/usr/bin/env python3
"""Mock RetroAchievements server for DScheevos tests.

Answers the dorequest.php calls rcheevos makes (login2, achievementsets,
startsession, awardachievement, submitlbentry, ping) so tests can run hardcore
sessions without a real account. Every request and unlock is appended to a
JSON-lines log, and state lives in memory, so restarting resets everything.

    python3 server.py --port 8080 --set christmasrush-set.json --log events.jsonl

--set takes an achievementsets response (as the real server returns it). The
set is served for any hash; pass --hash to only accept one. Events:
server_start, achievementsets (so a spectator run, which starts no session,
still shows it reached the server), startsession, unlock, leaderboard,
refused and ping.

Like the real server with a client it doesn't recognise, it adds the
"unsupported emulator" warning achievement to the set and files hardcore
unlocks as softcore (logged as counted_hardcore false). Pass --known-client
for a client RetroAchievements has approved.

Unlocks and leaderboard entries sent late (from the offline journal) carry
o=, the seconds since they happened, logged as "offset". --fail-awards
answers awardachievement and submitlbentry with HTTP 503 (rcheevos retries
those later), logged as "refused", to test the offline queue.
"""
import copy
import gzip
import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

TOKEN = "MockToken"


class State:
    def __init__(self, set_json, only_hash, log_path, known_client, fail_awards=False):
        self.known_client = known_client
        self.fail_awards = fail_awards
        self.set_json = set_json if known_client else with_warning(set_json)
        self.only_hash = only_hash.lower() if only_hash else None
        self.log_path = log_path
        self.unlocks = {}      # achievement id -> (hardcore, time)
        self.session = None

    def log(self, event):
        event["t"] = round(time.time(), 3)
        with open(self.log_path, "a") as f:
            f.write(json.dumps(event) + "\n")


# What retroachievements.org adds for a client it doesn't recognise (as in rcheevos' tests)
WARNING = {"ID": 101000001, "Title": "Warning: Unsupported Emulator",
           "Description": "Hardcore unlocks cannot be earned using this emulator.", "Flags": 3, "Points": 0,
           "MemAddr": "1=1.300.", "Author": "", "BadgeName": "00000", "Created": 0, "Modified": 0}


def with_warning(set_json):
    s = copy.deepcopy(set_json)
    if s.get("Sets"):
        s["Sets"][0].setdefault("Achievements", []).append(dict(WARNING))
    return s


def ok(**fields):
    return {"Success": True, **fields}


def fail(error, code="invalid_parameter", status=400):
    return {"Success": False, "Error": error, "Code": code, "Status": status}


class Unavailable(Exception):
    """Answer with HTTP 503 (retryable) instead of a JSON reply."""


def offset(p):
    return int(p["o"]) if p.get("o", "").isdigit() else 0


def handle(state, p):
    r = p.get("r", "")
    user = p.get("u", "")
    if r == "login2":
        if not user or not (p.get("p") or p.get("t")):
            return fail("Missing credentials", "invalid_credentials", 401)
        return ok(User=user, AvatarUrl="", Token=TOKEN, Score=0, SoftcoreScore=0, Messages=0)

    if r in ("achievementsets", "startsession", "awardachievement", "submitlbentry", "ping") and p.get("t") != TOKEN:
        return fail("Invalid token", "invalid_credentials", 401)

    if r == "achievementsets":
        h = p.get("m", "").lower()
        state.log({"event": "achievementsets", "hash": h})
        if state.only_hash and h and h != state.only_hash:
            return fail("Unknown game", "not_found", 404)
        return state.set_json

    if r == "startsession":
        hardcore = p.get("h") == "1"
        state.session = {"game": p.get("g"), "hardcore": hardcore}
        state.log({"event": "startsession", "game": p.get("g"), "hardcore": hardcore})
        unlocks = [{"ID": int(a), "When": int(w)} for a, (hc, w) in state.unlocks.items()]
        hc_unlocks = [{"ID": int(a), "When": int(w)} for a, (hc, w) in state.unlocks.items() if hc]
        return ok(Unlocks=unlocks, HardcoreUnlocks=hc_unlocks, ServerNow=int(time.time()))

    if r == "awardachievement":
        aid = p.get("a", "0")
        hardcore = p.get("h") == "1"
        if state.fail_awards:
            state.log({"event": "refused", "request": r, "achievement": int(aid), "offset": offset(p)})
            raise Unavailable()
        counted = hardcore and state.known_client
        state.log({"event": "unlock", "achievement": int(aid), "hardcore": hardcore, "counted_hardcore": counted,
                   "validation": p.get("v"), "offset": offset(p)})
        state.unlocks[aid] = (counted or state.unlocks.get(aid, (False, 0))[0], int(time.time()))
        return ok(Score=0, SoftcoreScore=0, AchievementID=int(aid), AchievementsRemaining=0)

    if r == "submitlbentry":
        if state.fail_awards:
            state.log({"event": "refused", "request": r, "leaderboard": int(p.get("i", "0")), "offset": offset(p)})
            raise Unavailable()
        state.log({"event": "leaderboard", "leaderboard": int(p.get("i", "0")), "score": int(p.get("s", "0")),
                   "offset": offset(p)})
        return ok(Response={"Score": int(p.get("s", "0")), "BestScore": int(p.get("s", "0")),
                            "RankInfo": {"Rank": 1, "NumEntries": 1}, "TopEntries": []})

    if r == "ping":
        state.log({"event": "ping", "rich_presence": p.get("m")})
        return ok()

    return fail("Unsupported request: " + r)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--set", required=True, help="achievementsets JSON response")
    ap.add_argument("--hash", help="only serve the set for this ROM hash")
    ap.add_argument("--log", default="events.jsonl")
    ap.add_argument("--known-client", action="store_true",
                    help="act as for an approved client: no warning achievement, hardcore unlocks count")
    ap.add_argument("--fail-awards", action="store_true",
                    help="answer awardachievement and submitlbentry with HTTP 503 (retryable)")
    ap.add_argument("--tls-cert", help="serve HTTPS with this certificate (PEM, chain first)")
    ap.add_argument("--tls-key", help="key for --tls-cert")
    args = ap.parse_args()

    with open(args.set) as f:
        state = State(json.load(f), args.hash, args.log, args.known_client, args.fail_awards)

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"   # keep-alive, like the real server
        def _reply(self, params):
            try:
                body, status = json.dumps(handle(state, params)).encode(), 200
            except Unavailable:
                body, status = b"<html><body>503 Service Temporarily Unavailable</body></html>", 503
            self.send_response(status)
            self.send_header("Content-Type", "application/json" if status == 200 else "text/html")
            if "gzip" in self.headers.get("Accept-Encoding", ""):   # as the real server (Cloudflare) does
                body = gzip.compress(body)
                self.send_header("Content-Encoding", "gzip")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            data = self.rfile.read(n).decode()
            params = {k: v[0] for k, v in parse_qs(data).items()}
            params.update({k: v[0] for k, v in parse_qs(urlparse(self.path).query).items()})
            self._reply(params)

        def do_GET(self):
            self._reply({k: v[0] for k, v in parse_qs(urlparse(self.path).query).items()})

        def log_message(self, fmt, *a):
            pass

    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    scheme = "http"
    if args.tls_cert:
        import ssl
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(args.tls_cert, args.tls_key)
        server.socket = ctx.wrap_socket(server.socket, server_side=True)
        scheme = "https"
    state.log({"event": "server_start"})   # runs sharing one log can be told apart
    print(f"mock RA server on {scheme}://{args.bind}:{args.port}/dorequest.php, log {args.log}")
    server.serve_forever()


if __name__ == "__main__":
    main()
