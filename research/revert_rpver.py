p = "/opt/arm64/build/chiaki-ng-1.10.1/lib/src/session.c"
s = open(p, encoding="utf-8").read()

i = s.find("case CHIAKI_TARGET_PS5_1:")
if i < 0:
    raise SystemExit("case not found")

j = s.find("return", i)
k = s.find(";", j)
old = s[j:k + 1]
s = s[:j] + 'return "1.0"' + s[k:]
open(p, "w", encoding="utf-8").write(s)
print("replaced:", old.strip(), "->", 'return "1.0"')
