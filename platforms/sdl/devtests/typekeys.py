#!/usr/bin/env python3
"""Prints a --keys script that types NAME on the firmware's on-screen keyboard, with the cursor moves
taken from valueMap in tracker-ce/core/source/mtKeyboardManager.h.

    typekeys.py mtKeyboardManager.h NAME START_MS STEP_MS

The keyboard must be open with the name field focused (as Save As opens it): Down leaves the field and
puts the cursor on the first key, screen1 types the selected key. The first key typed replaces the
whole preselected name. Lower case letters, digits, - + @ and space. Prints the end time on stderr.
"""
import sys, re
from collections import deque
h = open(sys.argv[1]).read()
body = h[h.index("valueMap[4][42]"):]
body = body[body.index("{")+1: body.index("};")]
maps = [list(map(int, re.findall(r"\d+", blk))) for blk in re.findall(r"\{([^{}]*)\}", body)]
assert len(maps) == 4 and all(len(m) == 42 for m in maps)
names = ["left", "right", "up", "down"]          # valueMap direction order (0 left, 1 right, 2 up, 3 down)
pos_of = {}
for i, c in enumerate("1234567890"): pos_of[c] = i
for i, c in enumerate("qwertyuiop-+"): pos_of[c] = 11 + i
for i, c in enumerate("asdfghjkl@"): pos_of[c] = 23 + i
for i, c in enumerate("zxcvbnm"): pos_of[c] = 34 + i
pos_of[" "] = 41
def path(a, b):
    q, seen = deque([(a, [])]), {a}
    while q:
        p, k = q.popleft()
        if p == b: return k
        for d in range(4):
            if d == 2 and p <= 10:   # makeMove: up from the number row goes to the name field
                continue
            n = maps[d][p]
            if n not in seen: seen.add(n); q.append((n, k + [names[d]]))
name, t, step = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
keys, cur = ["%d:down" % t], 0   # leave the name field: the cursor lands on position 0
t += step
for c in name:
    for k in path(cur, pos_of[c]):
        keys.append("%d:%s" % (t, k)); t += step
    keys.append("%d:screen1" % t); t += step
    cur = pos_of[c]
print(",".join(keys)); print(t, file=sys.stderr)
