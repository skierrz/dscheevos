#!/usr/bin/env python3
"""Mock RetroAchievements server for DScheevos tests.

Answers the dorequest.php calls rcheevos makes (login2, achievementsets,
startsession, awardachievement, submitlbentry, ping) so tests can run hardcore
sessions without a real account. Every request and unlock is appended to a
JSON-lines log, and state lives in memory, so restarting resets everything.

    python3 server.py --port 8080 --set christmasrush-set.json --log events.jsonl

--set takes an achievementsets response (as the real server returns it). The
set is served for any hash; pass --hash to only accept one.
"""
import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

TOKEN = "MockToken"


class State:
    def __init__(self, set_json, only_hash, log_path):
        self.set_json = set_json
        self.only_hash = only_hash.lower() if only_hash else None
        self.log_path = log_path
        self.unlocks = {}      # achievement id -> (hardcore, time)
        self.session = None

    def log(self, event):
        event["t"] = round(time.time(), 3)
        with open(self.log_path, "a") as f:
            f.write(json.dumps(event) + "\n")


def ok(**fields):
    return {"Success": True, **fields}


def fail(error, code="invalid_parameter", status=400):
    return {"Success": False, "Error": error, "Code": code, "Status": status}


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
        state.log({"event": "unlock", "achievement": int(aid), "hardcore": hardcore, "validation": p.get("v")})
        state.unlocks[aid] = (hardcore or state.unlocks.get(aid, (False, 0))[0], int(time.time()))
        return ok(Score=0, SoftcoreScore=0, AchievementID=int(aid), AchievementsRemaining=0)

    if r == "submitlbentry":
        state.log({"event": "leaderboard", "leaderboard": int(p.get("i", "0")), "score": int(p.get("s", "0"))})
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
    args = ap.parse_args()

    with open(args.set) as f:
        state = State(json.load(f), args.hash, args.log)

    class Handler(BaseHTTPRequestHandler):
        def _reply(self, params):
            body = json.dumps(handle(state, params)).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
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

    print(f"mock RA server on http://{args.bind}:{args.port}/dorequest.php, log {args.log}")
    ThreadingHTTPServer((args.bind, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
