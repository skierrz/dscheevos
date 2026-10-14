# Private work directories for test runs (source this file; POSIX sh).
#
# Work dirs hold SD images, session files and RAM dumps, which contain the RA session token, and
# a run can last many minutes. They live under one dedicated private directory, never in $TMPDIR
# (where a generic cleanup of tmp.* from another session or agent deleted running tests' SD cards):
#   ${DSC_WORK:-$HOME/Developer/DScheevos/.work}/<label>-XXXXXX   (mode 700; the root is 700 too)
#
#   dsc_mkwork LABEL      create the dir, print its path. LABEL is the slot/pid name, e.g. dsc-run-p8443.
#   dsc_rmwork PATH...    remove dirs made by dsc_mkwork plus the runner's sibling PATH.img / PATH.img.idx.
#                         Refuses anything outside the work root.
#
# Every dir is also appended to $DSC_WORK_LIST when set; tests/perf/hardtimeout sets it and removes
# the listed dirs after the run, so a run killed by the time limit (no EXIT trap after SIGKILL)
# leaves nothing behind either.
dsc_work_root() {
  r=${DSC_WORK:-$HOME/Developer/DScheevos/.work}
  [ -d "$r" ] || { (umask 077; mkdir -p "$r") || return 1; }
  chmod 700 "$r"
  printf '%s\n' "$r"
}
dsc_mkwork() {
  r=$(dsc_work_root) || return 1
  d=$(umask 077; mktemp -d "$r/${1:-run}-XXXXXX") || return 1
  chmod 700 "$d"
  [ -z "${DSC_WORK_LIST:-}" ] || printf '%s\n' "$d" >> "$DSC_WORK_LIST"
  printf '%s\n' "$d"
}
dsc_rmwork() {
  r=$(dsc_work_root) || return 1
  for d in "$@"; do
    case $d in "$r"/?*) rm -rf "$d" "$d.img" "$d.img.idx" ;; *) echo "dsc_rmwork: refusing $d (not under $r)" >&2 ;; esac
  done
}
