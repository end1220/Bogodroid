import sys

path, target = sys.argv[1], int(sys.argv[2], 16)
best = ("", -1)
for line in open(path):
    p = line.split()
    if len(p) < 3:
        continue
    try:
        a = int(p[0], 16)
    except ValueError:
        continue
    if a <= target and a > best[1]:
        best = (p[2], a)
print("closest: %s + 0x%x" % (best[0], target - best[1]))
