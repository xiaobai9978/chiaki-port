
p = "/opt/arm64/build/chiaki-ng-1.10.1/lib/src/session.c"
s = open(p).read()
s = s.replace(chr(34)+chr(34)+chr(34)+"		case CHIAKI_TARGET_PS5_1:
			/* PS5 新固件要求 Rp-Version >= 8.0；发 1.0 会被回 0x80108bff(unknown)。
			 * 只有请求头用这个值，解析响应走 chiaki_rp_version_parse，不受影响。 */
			return "+chr(34)+"9.0"+chr(34)+";"+chr(34)+chr(34)+chr(34), chr(34)+chr(34)+chr(34)+"		case CHIAKI_TARGET_PS5_1:
			return "+chr(34)+"1.0"+chr(34)+";"+chr(34)+chr(34)+chr(34))
open(p,"w").write(s)
print("reverted")
