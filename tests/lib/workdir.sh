# Private work directories for test runs (source this file; POSIX sh).
#
# Work dirs hold SD images, session files and RAM dumps, which contain the RA session token, and
# a run can last many minutes. They live under private directories, never in $TMPDIR (where a generic
# cleanup of tmp.* from another session or agent deleted running tests' SD cards):
#   ${DSC_WORK:-$HOME/Developer/DScheevos/.work}/<label>-XXXXXX     holder: a marker dir (README only), mode 700
#   <that root>.d/<label>-XXXXXX                                     the run dir dsc_mkwork prints (mode 700)
#
#   dsc_mkwork LABEL      create the run dir + its holder, print the run dir. LABEL is the slot/pid name,
#                         e.g. dsc-run-p8443.
#   dsc_rmwork PATH...    remove run dirs made by dsc_mkwork (or their holders) plus the runner's sibling
#                         PATH.img / PATH.img.idx. Clears the soft lock first. Refuses anything else.
#   dsc_softterm LABEL    soft process lock for a test script: install the TERM/HUP bounce (see below).
#   dsc_runner_flags      print the runner flags the test scripts add (--soft-term when the runner has it).
#
# SOFT LOCKS (owner, 2026-10-14): an accidental cleanup or kill bounces off, anyone who means it can still
# stop or delete things in one line. They are guards against mistakes, not security.
#
#  1. Work dirs. The holder under .work gets the macOS user-immutable flag (chflags uchg; on Linux, where
#     chattr needs root, it is skipped quietly). `rm -rf .work/*` or `rm -rf .work/<holder>` then fails with
#     "Operation not permitted" on the live run's holder. The holder holds only a README (what it is, the
#     owner pid and label, the override); the run's files live in the sibling tree <root>.d, outside any
#     glob over .work, because rm -rf recurses into a mutable child before it fails on the immutable parent
#     (measured: a holder with the run dir inside lost every file). The run writes freely in its own dir.
#     Override, one line (the README prints it with the real paths):
#         chflags -R nouchg <holder> && rm -rf <holder> <run dir>
#     tests/lib/workdir-gc.sh removes holders whose owner pid is dead (and only those).
#  2. Processes. dsc_softterm (shell scripts), tests/perf/hardtimeout and `melonds-headless --soft-term` ignore
#     a first SIGTERM/SIGHUP, log "<label>: ignored SIGTERM from outside; send it again within 10 s, or use
#     SIGINT/SIGKILL, to stop" (also to <root>.d/softterm.log, with a timestamp and, in the runner, the
#     sender pid) and keep running. A second TERM/HUP within 10 s, SIGINT (Ctrl-C) or SIGKILL stops as
#     before. hardtimeout's own time-limit kill sends SIGINT first, so it is never bounced.
#     Limit: a POSIX shell runs its trap only when the foreground child ends, so a script waiting on the
#     runner defers its own bounce; the runner and hardtimeout answer at once.
#
# Every dir is also appended to $DSC_WORK_LIST when set; tests/perf/hardtimeout sets it and removes
# the listed dirs after the run, so a run killed by the time limit (no EXIT trap after SIGKILL)
# leaves nothing behind either.
dsc_work_root() {
  r=${DSC_WORK:-$HOME/Developer/DScheevos/.work}
  [ -d "$r" ] || { (umask 077; mkdir -p "$r") || return 1; }
  chmod 700 "$r"
  [ -d "$r.d" ] || { (umask 077; mkdir -p "$r.d") || return 1; }
  chmod 700 "$r.d"
  printf '%s\n' "$r"
}
dsc_lock() { command -v chflags >/dev/null 2>&1 && chflags uchg "$1" 2>/dev/null; return 0; }
dsc_unlock() { [ ! -e "$1" ] || { command -v chflags >/dev/null 2>&1 && chflags -R nouchg "$1" 2>/dev/null; }; return 0; }
dsc_mkwork() {
  r=$(dsc_work_root) || return 1
  d=$(umask 077; mktemp -d "$r.d/${1:-run}-XXXXXX") || return 1
  chmod 700 "$d"
  h="$r/${d##*/}"
  if (umask 077; mkdir "$h") 2>/dev/null; then
    {
      echo "This marks a live DScheevos test run. Its files are in the run dir below; do not delete it."
      echo "It is a soft lock (user-immutable flag): an accidental rm -rf of .work fails here on purpose."
      echo "pid: $$"
      echo "label: ${1:-run}"
      echo "started: $(date '+%Y-%m-%d %H:%M:%S')"
      echo "run dir: $d"
      echo "override: chflags -R nouchg '$h' && rm -rf '$h' '$d' '$d.img' '$d.img.idx'"
      echo "stale holders (owner pid gone): tests/lib/workdir-gc.sh"
    } > "$h/README"
    dsc_lock "$h"
  fi
  [ -z "${DSC_WORK_LIST:-}" ] || printf '%s\n' "$d" >> "$DSC_WORK_LIST"
  printf '%s\n' "$d"
}
dsc_rmwork() {
  r=$(dsc_work_root) || return 1
  for d in "$@"; do
    case $d in
      "$r"/?*|"$r.d"/?*) n=${d##*/}; dsc_unlock "$r/$n"; rm -rf "$r/$n" "$r.d/$n" "$r.d/$n.img" "$r.d/$n.img.idx" ;;
      *) echo "dsc_rmwork: refusing $d (not under $r)" >&2 ;;
    esac
  done
}

# Soft TERM/HUP bounce for a script. SIGINT stops at once; a second TERM/HUP within 10 s stops.
dsc_softterm() {
  DSC_RUN_LABEL=${1:-${DSC_RUN_LABEL:-$(basename "$0")}}
  export DSC_RUN_LABEL
  _dsc_r=${DSC_WORK:-$HOME/Developer/DScheevos/.work}
  DSC_SOFTTERM_LOG=${DSC_SOFTTERM_LOG:-$_dsc_r.d/softterm.log}
  export DSC_SOFTTERM_LOG
  _dsc_t0=
  trap 'exit 130' INT
  trap '_dsc_term TERM' TERM
  trap '_dsc_term HUP' HUP
}
_dsc_term() {
  _n=$(date +%s)
  if [ -n "$_dsc_t0" ] && [ $((_n - _dsc_t0)) -le 10 ]; then exit 130; fi
  _dsc_t0=$_n
  _m="$DSC_RUN_LABEL: ignored SIG$1 from outside; send it again within 10 s, or use SIGINT/SIGKILL, to stop"
  echo "$_m" >&2
  [ ! -d "${DSC_SOFTTERM_LOG%/*}" ] || echo "$(date '+%Y-%m-%d %H:%M:%S') pid $$ ppid $PPID $_m" >> "$DSC_SOFTTERM_LOG" 2>/dev/null
  return 0
}
# --soft-term when the runner knows it (an older runner build would reject the unknown flag).
dsc_runner_flags() {
  [ -n "${RUNNER:-}" ] && "$RUNNER" 2>&1 | grep -q -- '--soft-term' && printf '%s\n' --soft-term
  return 0
}
