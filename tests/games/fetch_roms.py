#!/usr/bin/env python3
"""Download the homebrew test games from GameBrew into $DSC_ROMS (default ./roms).

Only files whose rcheevos DS hash matches the RetroAchievements hash in
games.txt are kept. Game files are never committed to this repo.
Needs a RetroAchievements DS hasher: pass --hasher (built from rcheevos,
see tests/games/README.md).
"""
import argparse, os, re, subprocess, sys, tempfile, urllib.parse, urllib.request, zipfile

UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/128 Safari/537.36"

def get(url, referer=None):
    h = {"User-Agent": UA}
    if referer: h["Referer"] = referer
    return urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=60).read()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hasher", required=True)
    ap.add_argument("--out", default=os.environ.get("DSC_ROMS", "roms"))
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    ok = True
    for line in open(os.path.join(here, "games.txt")):
        if not line.strip() or line.startswith("#"): continue
        name, path, want, page, *_ = [x.strip() for x in line.split("|")]
        dest = os.path.join(a.out, path)
        if os.path.exists(dest):
            print(f"{name}: already present"); continue
        page_url = "https://www.gamebrew.org/wiki/" + page
        html = get(page_url).decode("utf-8", "replace")
        links = list(dict.fromkeys(re.findall(r"https://dlhb\.gamebrew\.org/dshomebrew[^\"' ]+\.(?:zip|7z)", html)))
        found = False
        for link in links:
            with tempfile.TemporaryDirectory() as tmp:
                arc = os.path.join(tmp, os.path.basename(link))
                open(arc, "wb").write(get(link, page_url))
                x = os.path.join(tmp, "x")
                if arc.endswith(".zip"): zipfile.ZipFile(arc).extractall(x)
                else: subprocess.run(["7z", "x", "-y", "-o" + x, arc], capture_output=True)
                for root, _, files in os.walk(x):
                    for f in files:
                        if not f.lower().endswith(".nds"): continue
                        p = os.path.join(root, f)
                        h = subprocess.run([a.hasher, p], capture_output=True, text=True).stdout.split()
                        if h and h[0].lower() == want:
                            os.makedirs(os.path.dirname(dest), exist_ok=True)
                            os.replace(p, dest)
                            found = True
                            break
                    if found: break
            if found: break
        print(f"{name}: {'ok' if found else 'NO MATCHING FILE'}")
        ok &= found
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
