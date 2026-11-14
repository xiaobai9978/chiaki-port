import xfer

cmds = [
    "/usr/sbin/sshd -p 2222 2>&1; sleep 2; netstat -ln 2>/dev/null | grep 2222; "
    "pidof sshd; pkill -f 'sshd -p 2222'; sleep 1; echo cleaned",
    "cat /usr/trimui/bin/runtrimui.sh",
    "/etc/init.d/sshd enabled && echo ENABLED || echo DISABLED",
    "logread 2>/dev/null | tail -20",
]
for c in cmds:
    o, e, r = xfer.run('dev', c, timeout=30)
    print('=====', c)
    print(o[:2000], e[:300])
