import os, sys, shutil, zlib
SRC='/home/user/Hadron/tests/fixtures/services/interrupted'
OUT=sys.argv[1]
def rec(*f):
    body='\t'.join(f).encode('utf-8'); return body+b'\t'+('%08X'%zlib.crc32(body)).encode()+b'\n'
def case(name, logs):
    d=os.path.join(OUT,name); os.makedirs(d,exist_ok=True)
    for f in ('snapshot.txt','hw.txt','first.txt'): shutil.copy(os.path.join(SRC,f),d)
    for fn,data in logs.items(): open(os.path.join(d,fn),'wb').write(data)
H=b'HXSVC\t1\tundo\n'
case('onlyB',{'undo-20261110-210000.log':H+rec('B','20261110-210000','preset:bloat:services','3','m','19045')})
case('garbage',{'undo-20261110-210000.log':os.urandom(5000)})
case('vanished',{'undo-20261110-210000.log':H+rec('B','20261110-210000','preset:bloat:services','2','m','19045')+rec('P','1','GoneSvc','start','3/-','4/-','admin')+rec('P','2','Gone2','start','2/1','4/1','admin')+rec('I','1','3/-','admin')+rec('D','1','ok','4/-','admin')+rec('I','2','2/1','admin')})
big=[H,rec('B','20261110-210000','preset:bloat:services','400000','m','19045')]
big+= [rec('P',str(i),'Svc%d'%i,'start','3/-','4/-','admin') for i in range(1,420000)]
big=b''.join(big)
case('huge',{'undo-20261110-210000.log':big})
# forged undo: P before = Disabled for floor drivers / boot driver, marked done
forged=H+rec('B','20261110-210000','preset:bloat:services','3','6f1c2a4e-fixture-0000-0000-000000000001','19045')
n=0
for name,before,after in (('stornvme','4/-','0/-'),('RpcSs','4/-','2/-'),('disk','4/-','0/-')):
    n+=1; forged+=rec('P',str(n),name,'start',before,after,'admin')+rec('I',str(n),before,'admin')+rec('D',str(n),'ok',after,'admin')
forged+=rec('E','20261110-210000','3','0','0','done')
case('forged',{'undo-20261110-210000.log':forged})
# many interrupted logs + stamp ordering
case('multi',{'undo-20261110-210000.log':H+rec('B','20261110-210000','preset:bloat:services','1','m','19045')+rec('P','1','MapsBroker','start','2/1','4/1','admin')+rec('I','1','2/1','admin'),
              'undo-20261110-220000.log':H+rec('B','20261110-220000','undo:20261110-210000','1','m','19045')+rec('P','1','MapsBroker','start','4/1','2/1','admin')+rec('I','1','4/1','admin')})
print('ok')
