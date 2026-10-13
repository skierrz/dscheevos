#!/usr/bin/env python3
"""The bottom-edge strip's allow list: M1 survey x idle census.

usage: allow_list.py SURVEY.md IDLE_CENSUS.md

SURVEY.md is docs/display-research/survey.md (its "Re-score for the
bottom-edge strip" table); IDLE_CENSUS.md is idle-coverage's
docs/idle-census.md (its per-game table, column "waiting by line 156").

A title is a candidate when all hold:
- it reached play in the survey and the bottom re-score says "yes";
- the module gets time in it today (census class "oshalt");
- it waits for VBlank by line 156 on >= ALLOW of frames, so a notice
  (which needs 30 idle frames in a row to start, R19) can show steadily.
MARGINAL..ALLOW is "marginal": notices would often be cut short.
"""
import re
import sys

ALLOW = 80
MARGINAL = 50


def norm(name):
    return re.sub(r"[^a-z0-9]", "", name.lower())


def tables(path):
    """Every markdown table row in the file, as lists of stripped cells."""
    rows = []
    for line in open(path, encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith("|") and not re.match(r"^\|[-| ]+\|$", line):
            rows.append([c.strip() for c in line.strip("|").split("|")])
    return rows


def main():
    survey_path, census_path = sys.argv[1:3]
    text = open(survey_path, encoding="utf-8").read()
    start = text.find("## Re-score for the bottom-edge strip")
    if start < 0:
        sys.exit("no bottom-edge section in " + survey_path)
    tmp = text[start:]
    survey = {}
    for row in (r for r in (l for l in tmp.splitlines()) if r.startswith("| ")):
        cells = [c.strip() for c in row.strip("|").split("|")]
        if len(cells) >= 3 and cells[0] not in ("Title", "Factor"):
            survey[norm(cells[0])] = (cells[0], cells[1], cells[2])

    census = {}
    for cells in tables(census_path):
        if len(cells) >= 9 and cells[0] not in ("Game", "Class"):
            m = re.match(r"(\d+)%", cells[8])
            if m:
                census[norm(cells[0])] = (cells[0], cells[4], cells[5], int(m.group(1)))

    rows = []
    for key, (name, run, cls, by156) in sorted(census.items(), key=lambda kv: kv[1][0]):
        s = survey.get(key)
        play, bottom = (s[1], s[2]) if s else ("not surveyed", "?")
        if cls != "oshalt":
            verdict = "no: the module gets no time today (%s)" % cls
        elif s is None:
            verdict = "unknown: not in the M1 survey"
        elif play != "yes":
            verdict = "unknown: didn't reach play in M1"
        elif bottom != "yes":
            verdict = "no: " + bottom.replace("no: ", "")
        elif by156 >= ALLOW:
            verdict = "**candidate**"
        elif by156 >= MARGINAL:
            verdict = "marginal (waiting by 156 on %d%%)" % by156
        else:
            verdict = "no: busy at line 156 on %d%% of frames" % (100 - by156)
        rows.append((name, cls, run, by156, play, bottom, verdict))

    print("| Game | Wait class | Census run | Waiting by line 156 | M1 reached play | M1 bottom re-score | Verdict |")
    print("|---|---|---|---|---|---|---|")
    for r in rows:
        print("| %s | %s | %s | %d%% | %s | %s | %s |" % r)
    cand = [r for r in rows if r[6] == "**candidate**"]
    marg = [r for r in rows if r[6].startswith("marginal")]
    known = [r for r in rows if r[1] == "oshalt" and r[4] == "yes"]
    print("\nCandidates: %d (%s). Marginal: %d. Of %d oshalt titles that reached play in M1, %d are candidates."
          % (len(cand), ", ".join(r[0] for r in cand), len(marg), len(known), len(cand)))


if __name__ == "__main__":
    main()
