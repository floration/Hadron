#include "rescue_core.h"
#include <cstdio>
using namespace rc;
int main(){
  std::vector<Rec> R; u64 usn=0;
  auto add=[&](u64 frn,u64 par,u32 reason,u32 attr,const wchar_t*n,u64 t){Rec r;r.usn=++usn;r.frn=frn;r.parent=par;r.reason=reason;r.attr=attr;r.name=n;r.time=t;R.push_back(r);};
  const u64 ROOT=5, MC=100 /*"minecraft server" (never in journal)*/, PLUG=101 /*plugins (never)*/, OLDW=200, DESK=201, TRIP=300, IMG=900, PROG=901, CODE=902, BIN=999;
  u64 T=1000000000ull;
  // before run: user renames folder 300 "Trip" -> "Trip 2024" (first record OLD)
  add(TRIP,ROOT,R_RENAME_OLD,A_DIRECTORY,L"Trip",T); add(TRIP,ROOT,R_RENAME_NEW,A_DIRECTORY,L"Trip 2024",T+1);
  // unrelated file rename in place (must be ignored)
  add(50,TRIP,R_RENAME_OLD,0,L"a.txt",T+2); add(50,TRIP,R_RENAME_NEW,0,L"b.txt",T+3);
  // organizer run
  u64 S=T+100000000ull;
  add(IMG,ROOT,0x100,A_DIRECTORY,L"Images",S); add(PROG,ROOT,0x100,A_DIRECTORY,L"Programs",S); add(CODE,ROOT,0x100,A_DIRECTORY,L"Code",S);
  add(10,PLUG,R_RENAME_OLD,0,L"EssentialsX-2.20.jar",S+1); add(10,PROG,R_RENAME_NEW,0,L"EssentialsX-2.20.jar",S+2); add(10,PROG,R_RENAME_NEW|0x80000000,0,L"EssentialsX-2.20.jar",S+3);
  add(11,MC,R_RENAME_OLD,0,L"ops.json",S+4); add(11,CODE,R_RENAME_NEW,0,L"ops.json",S+5);
  add(12,DESK,R_RENAME_OLD,0,L"pic.jpg",S+6); add(12,IMG,R_RENAME_NEW,0,L"pic (2).jpg",S+7);
  add(13,TRIP,R_RENAME_OLD,0,L"beach.jpg",S+8); add(13,IMG,R_RENAME_NEW,0,L"beach.jpg",S+9);
  // gather removes emptied folders: desktop then "old windows install"
  add(DESK,OLDW,0x200,A_DIRECTORY,L"desktop",S+10); add(OLDW,ROOT,0x200,A_DIRECTORY,L"old windows install",S+11);
  // later: duplicate removal sends pic (2).jpg to recycle bin (must not override first move)
  add(12,IMG,R_RENAME_OLD,0,L"pic (2).jpg",S+20); add(12,BIN,R_RENAME_NEW,0,L"$R123.jpg",S+21);
  Analyzer a; a.rootFrn=ROOT;
  a.currentPath=[&](u64 f,std::wstring&o){ if(f==MC){o=L"minecraft server";return true;} if(f==PLUG){o=L"minecraft server\\plugins";return true;} if(f==BIN){o=L"$Recycle.Bin\\S-1";return true;} return false; };
  auto m=a.Run(R, 15ull*60*10000000);
  for(auto&x:m) printf("[s%d] %ls\\%ls  <-  %ls\\%ls\n",x.session,x.origDir.c_str(),x.origName.c_str(),x.destDir.c_str(),x.destName.c_str());
  printf("count=%zu\n",m.size());
}
