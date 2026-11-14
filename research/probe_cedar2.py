import xfer

libs = ['libcdc_base.so', 'libMemAdapter.so', 'libVE.so']
keys = ('Cdc', 'MemAdapter', 'VeInit', 'VideoEngine')

for lib in libs:
    cmd = 'chroot /opt/arm64 /bin/bash -c "cd /tmp && readelf --dyn-syms -W %s"' % lib
    o, e, r = xfer.run('ct', cmd, timeout=60)
    names = []
    for line in o.splitlines():
        if ' FUNC ' not in line:
            continue
        if not any(k in line for k in keys):
            continue
        name = line.split()[-1]
        if name not in names:
            names.append(name)
    print('== %s (%d)' % (lib, len(names)))
    for n in sorted(names)[:20]:
        print('   ', n)
