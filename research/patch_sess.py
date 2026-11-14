
p = "/opt/arm64/build/chiaki-ng-1.10.1/lib/src/session.c"
s = open(p).read()
old = """		case CHIAKI_TARGET_PS5_1:
			return "1.0";"""
new = """		case CHIAKI_TARGET_PS5_1:
			/* PS5 新固件要求 Rp-Version >= 8.0；发 1.0 会被回 0x80108bff(unknown)。
			 * 只有请求头用这个值，解析响应走 chiaki_rp_version_parse，不受影响。 */
			return "9.0";"""
assert old in s, "pattern not found"
open(p,"w").write(s.replace(old, new))
print("session.c patched")
