# Counts guest instructions between the harness markers in a qemu-arm log
# (-d in_asm,exec,nochain). qemu logs each translation block once when it is
# translated (IN: plus its bytes) and every time it runs (Trace). A block's
# instruction count comes from its bytes: 4 per ARM instruction, 2 per Thumb
# one, and a Thumb BL/BLX prefix+suffix pair counts once, as qemu runs it.
#
#   awk -v map=<file> -v marks=<file> -f count.awk <log>
#   map:   "<hex address> a|t" per ARM/Thumb mapping symbol, sorted
#   marks: "<hex address> <name>" for mark_vblank, mark_vblank_queued,
#          mark_idle, mark_end
# Output: "N <phase> <instructions>" per bracketed call, then
#         "PC <phase> <hex block start> <instructions>" summed per block.
function hex(s,   i, n) {
  s = tolower(s); sub(/^0x/, "", s); n = 0
  for (i = 1; i <= length(s); i++) n = n * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1
  return n
}
# Block addresses are keyed as in the Trace lines: 16 hex digits, lower case.
function key(s) { s = tolower(s); sub(/^0x/, "", s); sub(/:$/, "", s); return substr("0000000000000000" s, length(s) + 1) }
function thumb(addr,   lo, hi, mid) {
  lo = 1; hi = nmap
  if (hi == 0 || addr < maddr[1]) return 0
  while (lo < hi) { mid = int((lo + hi + 1) / 2); if (maddr[mid] <= addr) lo = mid; else hi = mid - 1 }
  return mmode[lo] == "t"
}
function byte(s, i) { return hex(substr(s, 2 * i - 1, 2)) }
function insns(pc, s,   n, b, i, h, h2) {
  b = length(s) / 2
  if (!thumb(hex(pc))) return int(b / 4)
  n = 0
  for (i = 1; i + 1 <= b; i += 2) {
    h = byte(s, i + 1) * 256 + byte(s, i)
    if (int(h / 2048) == 30 && i + 3 <= b) {        # BL/BLX prefix: 11110
      h2 = byte(s, i + 3) * 256 + byte(s, i + 2)
      if (int(h2 / 2048) == 31 || int(h2 / 2048) == 29) i += 2
    }
    n++
  }
  return n
}
BEGIN {
  while ((getline line < map) > 0) { split(line, f, " "); nmap++; maddr[nmap] = hex(f[1]); mmode[nmap] = f[2] }
  while ((getline line < marks) > 0) { split(line, f, " "); a = hex(f[1]); mark[key(sprintf("%x", a - a % 2))] = f[2] }
}
# Trace lines are nearly all of the log: keep this rule first and cheap.
# "Trace 0: 0x... [00800480/<16 hex pc>/...] name"
/^Trace / {
  pc = substr($4, 11, 16)
  if (pc in mark) {
    if (mark[pc] == "mark_end") { if (phase != "") print "N", phase, n; phase = "" }
    else { phase = mark[pc]; n = 0 }
  }
  else if (phase != "") { n += size[pc]; tb[phase SUBSEP pc] += size[pc] }
  next
}
/^IN:/ { inblk = 1; blk = ""; bytes = ""; next }
inblk && /^0x[0-9a-fA-F]+:/ { if (blk == "") blk = key($1); next }
inblk && /^OBJD-/ { bytes = bytes $2; next }
inblk && /^$/ { if (blk != "") size[blk] = insns(blk, bytes); inblk = 0; next }
/^(-+)?$/ { next }
{ print }   # the harness's own output
END { for (k in tb) { split(k, f, SUBSEP); print "PC", f[1], f[2], tb[k] } }
