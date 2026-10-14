#!/usr/bin/env python3
"""difftest-cmp A.log B.log: exit 0 if the logs are identical (comment lines
'#' aside, which hold the run's mode/split/features). Otherwise print the first
differing frame, both sides' records for it, and the 10 frames before it; exit 1.
Logs whose inputs (H lines) differ are refused (exit 2)."""
import sys


def frames(path):
    head, recs, cur = [], [], None
    with open(path, errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('#'):
                continue
            if line.startswith('H '):
                head.append(line)
                continue
            if line.startswith('F ') or line.startswith('END'):
                cur = [line]
                recs.append(cur)
                continue
            if cur is None:
                cur = []
                recs.append(cur)
            cur.append(line)
    return head, recs


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ha, ra = frames(sys.argv[1])
    hb, rb = frames(sys.argv[2])
    if ha != hb:
        print('inputs differ:')
        for x, y in zip(ha, hb):
            if x != y:
                print('  A: %s\n  B: %s' % (x, y))
        sys.exit(2)
    for i in range(max(len(ra), len(rb))):
        a = ra[i] if i < len(ra) else ['<missing>']
        b = rb[i] if i < len(rb) else ['<missing>']
        if a != b:
            print('first difference in record %d (%s)' % (i, a[0] if a else '?'))
            for j in range(max(0, i - 10), i):
                print('  before: %s' % ' | '.join(ra[j]))
            print('A:'); print('\n'.join('  ' + x for x in a))
            print('B:'); print('\n'.join('  ' + x for x in b))
            sys.exit(1)
    print('identical: %d records' % len(ra))


if __name__ == '__main__':
    main()
