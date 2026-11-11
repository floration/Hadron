# Hostile fixture generator for the services module (sniffer). Writes into argv[1]/<case>/.
import os, sys, shutil
SRC = '/home/user/Hadron/tests/fixtures/services'
OUT = sys.argv[1]
base = open(os.path.join(SRC, 'snapshot.txt'), encoding='utf-8').read().split('\n')
hdr = [l for l in base if l.startswith('HXSVC') or l.startswith('M\t')]
body = [l for l in base if l and not (l.startswith('HXSVC') or l.startswith('M\t'))]

def S(name, display='D', typ='10', cfg='3/-', trig='0', state='1', errctl='1', account='LocalSystem',
      image='C:\\\\x.exe', group='', deps='', desc='d', reg='', prot='0', ctl='0', pid='0', fail='', trigs='', ifc='', company='X', rerr=''):
    return '\t'.join(['S', name, display, typ, cfg, trig, state, errctl, account, image, group, deps, desc, reg, prot, ctl, pid, fail, trigs, ifc, company, rerr])

def case(name, lines, hw=True, first=None, extra=None):
    d = os.path.join(OUT, name); os.makedirs(d, exist_ok=True)
    open(os.path.join(d, 'snapshot.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
    if hw: shutil.copy(os.path.join(SRC, 'hw.txt'), d)
    if first is not None:
        open(os.path.join(d, 'first.txt'), 'w', encoding='utf-8', newline='\n').write('\n'.join(first) + '\n')
    for fn, txt in (extra or {}).items():
        open(os.path.join(d, fn), 'wb').write(txt if isinstance(txt, bytes) else txt.encode('utf-8'))

case('empty', ['HXSVC\t1\tsnapshot'])
case('emptyfile', [''])
big = hdr + body + [S('ThirdParty%05d' % i, 'Third party service number %d' % i, deps='ThirdParty%05d' % ((i + 1) % 6000)) for i in range(6000)]
case('big6000', big)
longn = 'L' * 300
huge = 'H' * 40000
case('longnames', hdr + body + [S(longn, 'x' * 300), S('BigDisp', huge, desc=huge, image='C:\\\\' + 'p\\\\' * 20000 + 'x.exe')])
weird = ['Intel(R) TPM Provisioning Service', 'a%PATH%b', 'caret^x', 'amp&calc', 'q"uote', 'pipe|x', 'lt<gt>', '\u05e9\u05dc\u05d5\u05dd', 'emoji\U0001F600',
         'sp ace', 'semi;colon', 'eq=x', 'comma,x', 'bang!x', 'paren)x', 'paren(x', 'trail\\\\', 'tab\\nnl']
case('weirdnames', hdr + body + [S(n, n) for n in weird], first=None)
# duplicate (case-insensitive) names
case('dup', hdr + body + [S('diagtrack')])
case('short', hdr + ['S\tFoo\tbar'])
case('badcfg', hdr + [S('Foo', cfg='9/-')])
case('nohw', hdr + body, hw=False)
# cycles + dependency on missing service; DiagTrack <-> dmwappushservice
cyc = []
for l in body:
    f = l.split('\t')
    if f[0] == 'S' and f[1] == 'DiagTrack': f[11] = 'dmwappushservice;MissingSvc'
    if f[0] == 'S' and f[1] == 'dmwappushservice': f[11] = 'DiagTrack'
    cyc.append('\t'.join(f))
case('cycle', hdr + cyc)
# boot-start / system-start drivers in KB, errctl critical, protected services, KB svc that is a driver type
bootl = []
for l in body:
    f = l.split('\t')
    if f[0] == 'S' and f[1] in ('BTHUSB', 'Serial', 'igfxn', 'IntcDAud', 'luafv', 'HyperVideo', 'hyperkbd'):
        f[4] = '0/-'  # Boot start
    if f[0] == 'S' and f[1] == 'DiagTrack': f[3] = '1'; f[4] = '1/-'  # KB svc entry that is a kernel driver, System start
    if f[0] == 'S' and f[1] == 'MapsBroker': f[7] = '3'  # critical
    if f[0] == 'S' and f[1] == 'lfsvc': f[14] = '2'  # protected
    if f[0] == 'S' and f[1] == 'RetailDemo': f[4] = '0/-'  # a service with Boot start (invalid but possible in reg)
    bootl.append('\t'.join(f))
# add missing KB drivers if not present
names = {l.split('\t')[1].lower() for l in body if l.startswith('S\t')}
for n in ('BTHUSB', 'Serial', 'igfxn', 'IntcDAud', 'luafv', 'HyperVideo', 'hyperkbd'):
    if n.lower() not in names: bootl.append(S(n, n, typ='1', cfg='0/-', image=''))
case('bootdrv', hdr + bootl)
# per-user LUID instances whose template is missing / third-party _hex names
case('luid', hdr + body + [S('CDPUserSvc_ffff', typ='e0', cfg='4/-'), S('NoTemplateSvc_1a2b', typ='e0'), S('Foo_1a2b', typ='10'),
                           S('_abc', typ='e0'), S('X_', typ='e0')])
# Default-first: DelayedAutostart lingering on a Manual service; first says plain Auto
fd = []
for l in body:
    f = l.split('\t')
    if f[0] == 'S' and f[1] == 'Spooler': f[4] = '3/1'
    fd.append('\t'.join(f))
firstl = open(os.path.join(SRC, 'first.txt'), encoding='utf-8').read().rstrip('\n').split('\n')
case('delayedlinger', hdr + fd, first=firstl)
print('ok')
