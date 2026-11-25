// ViewHistoryPlan test: frames built like the ones logged with the Data menu open (the world's view, then the menu's three
// views; anti-aliasing and light histories alternating between two pictures by frame parity; the previous depth written at the
// end of each view; exposure; shared working pictures; the frame's own inputs and per-frame data). The plan is made from 4
// frames, then 16 frames are replayed with its copies, in mono and in stereo: the world must always read its own last frame
// (its own eye's in stereo). Edge cases follow (unknown states, compute lists, steps not fully seen, steps that run twice...).
//
// Build and run (x64 Native Tools prompt, from the repository's root):
//   cl /std:c++20 /EHsc /I src\CreationEngine tests\ViewHistoryPlanTest.cpp && ViewHistoryPlanTest.exe
// Expected: "0 wrong" on every replay line.
#include "ViewHistoryPlan.h"
#include <cstdio>
#include <cassert>
#include <map>
#include <string>
#include <vector>
using namespace ViewHistoryPlan;

// Resources
enum : uintptr_t { T1=0x10, T2=0x20, FINAL=0x30, PD=0x40, CD=0x50, SM=0x60, H0=0x70, H1=0x80, VA=0x90, VB=0xa0, EX=0xb0, CB=0xc0, PEER=0xd0 };
struct Item { uintptr_t res; uint32_t state; int item; int role; bool resting=false; };
struct Step { void* pass; uint8_t group; std::string name; std::vector<Item> items; std::string view; };
static char passes[4096];
int pass_id=0;
void* NewPass(){ return &passes[pass_id++]; }

// One frame's steps (parity p): the world [2] then menus [3] [5] [4], frame-level before and after.
struct Frame { std::vector<Step> steps; };
std::map<std::string, void*> pass_of;
void* P(const std::string& k){ auto it=pass_of.find(k); if(it!=pass_of.end()) return it->second; return pass_of[k]=NewPass(); }

struct Opt{ bool frame_level_cb=true; bool f_read_pd=false; bool ex_inplace=false; int blind_menu=-1; bool unsteady=false; bool taa_item9=false; bool taa_resting=false; bool prev_tracked=false; };
Frame Build(uint32_t frame, Opt o={}){
  Frame f; uint32_t p=frame&1;
  uintptr_t hp = p? H1:H0, hc = p? H0:H1;   // TAA history: read prev, write cur
  uintptr_t vp = p? VB:VA, vc = p? VA:VB;   // volumetric history
  auto add=[&](const std::string& view,uint8_t g,const std::string& name,std::vector<Item> it){ f.steps.push_back(Step{P(view+name),g,name,it,view}); };
  add("F",kFrame,"FrameSetup",{});
  add("F",kFrame,"ShadowMaps",{{SM,0x8,0,0}});
  if(o.frame_level_cb) add("F",kFrame,"ClearCB",{{CB,0x8,0,0}});
  const char* views[]={"[2]","[3]","[5]","[4]"};
  for(int v=0;v<4;++v){
    std::string V=views[v]; uint8_t g = (uint8_t)v; // (each view its own group: 0 the world, 1.. the menu's)
    add(V,g,"DepthPrepass",{{CD,0,0,0}});
    add(V,g,"ReadCB",{{CB,0xc0,0,0}});
    add(V,g,"WriteCB",{{CB,0x8,0,0}});
    add(V,g,"ProduceAndClearColor",{{T1,0x4,0,0}});
    add(V,g,"VolTemporal",{{PD,0xc0,0,0},{vp,0xc0,1,1},{vc,0x8,2,2}});
    if(o.taa_resting) add(V,g,"TAA",{{hp,0,0,0,true},{T1,0xc0,1,1},{T2,0x8,2,2},{hc,0x8,9,9,true}}); // (as logged in fix124: its history read untracked, state not known)
    else if(o.taa_item9) add(V,g,"TAA",{{hp,0xc0,0,0},{T1,0xc0,1,1},{T2,0x8,2,2},{hc,0x8,9,9}}); // (as logged in fix120: its history written at its own item [9])
    else add(V,g,"TAA",{{hp,0xc0,0,0},{T1,0xc0,1,1},{T2,0x8,2,2},{hc,0x8,100+2*2+(int)p,100+2}});
    add(V,g,"CopyToImagespace",{{T2,0xc0,0,0},{T1,0x4,1,1}});
    if(o.ex_inplace){ add(V,g,"AdaptExposure",{{EX,0x8,0,0},{SM,0xc0,1,1}}); }
    else { add(V,g,"ReadExposure",{{EX,0xc0,0,0},{SM,0xc0,1,1}}); add(V,g,"WriteExposure",{{EX,0x8,0,0}}); }
    if(v==o.blind_menu) add(V,g,"Blind",{{0,0,-2,0}});
    add(V,g,"InjectPrevDepth",{{PD,0,0,0}});
    if(o.unsteady && v==3 && (frame%4)>=2) add(V,g,"UpdatePrevDepthB",{{PD,0x400,0,0},{CD,0x800,1,1}});
    else add(V,g,"UpdatePrevDepth",{{PD,0x400,0,0},{CD,0x800,1,1}});
    add(V,g,"PostSharpen",{{T1,0xc0,0,0},{FINAL,0x4,1,1}});
  }
  add("F",kFrame,"UI",{{FINAL,0xc0,0,0}});
  if(o.f_read_pd) add("F",kFrame,"CullWithPrevDepth",{{PD,0xc0,0,0}});
  if(o.prev_tracked) add("F",kFrame,"ReadPrevColor",{{hp,0xc0,0,0}}); // (the last frame's history read with a tracked state: not resting then)
  return f;
}

std::vector<Use> Uses(uint32_t frame, const Frame& f){
  std::vector<Use> out; int order=0;
  for(auto& s: f.steps){ out.push_back(Use{frame,order,s.group,s.pass,-1,0,0,0,true}); for(auto& it: s.items){ Use u{frame,order,s.group,s.pass,it.item,it.role,it.res,it.state,true}; u.buffer = it.res==EX; u.resting = it.resting; out.push_back(u);} ++order; }
  return out;
}
std::vector<Use> Learn(uint32_t f0, Opt o){ std::vector<Use> u; for(uint32_t fr=f0; fr<f0+4; ++fr){ auto f=Build(fr,o); auto x=Uses(fr,f); u.insert(u.end(),x.begin(),x.end()); } return u; }
void Show(const char* label, const Result& r, std::map<uintptr_t,const char*>& nm, std::map<void*,std::string>& pname, bool actions){
  printf("%s: kept", label); for(auto& k: r.kept) printf(" %s", nm[k.resource]); for(auto& [w,n]: r.skipped) printf(" | %d %s", n, w.c_str()); printf("\n");
  if(actions) for(auto& a: r.actions) printf("    parity %u %s %-24s item %3d res %-3s ahead %u\n", a.parity, a.op==kSave?"SAVE":"LOAD", pname[a.pass].c_str(), a.item, nm[a.resource], a.ahead);
}

int main(){
  // learn over 4 frames
  std::vector<Use> uses; uint32_t f0=101;
  for(uint32_t fr=f0; fr<f0+4; ++fr){ auto f=Build(fr); auto u=Uses(fr,f); uses.insert(uses.end(),u.begin(),u.end()); }
  auto r=Plan(uses);
  printf("resources %d, kept %zu, actions %zu\n", r.resources, r.kept.size(), r.actions.size());
  std::map<uintptr_t,const char*> nm={{T1,"T1"},{T2,"T2"},{FINAL,"FINAL"},{PD,"PD"},{CD,"CD"},{SM,"SM"},{H0,"H0"},{H1,"H1"},{VA,"VA"},{VB,"VB"},{EX,"EX"},{CB,"CB"}};
  std::map<void*,std::string> pname; for(auto& [k,v]: pass_of) pname[v]=k;
  for(auto& k: r.kept) printf("  kept %s\n", nm[k.resource]);
  for(auto& [why,n]: r.skipped) printf("  skipped %d: %s\n", n, why.c_str());
  for(auto& a: r.actions) printf("  parity %u %s %-22s item %3d res %-3s state %#x ahead %u role %llx\n", a.parity, a.op==kSave?"SAVE":"LOAD", pname[a.pass].c_str(), a.item, nm[a.resource], a.state, a.ahead, (unsigned long long)a.role);

  int only_view = -1; // (the replay checks this view alone; -1: every view)
  uint32_t check_mask = 0xffffffffu; // (...and only the views with their bit set)
  auto Simulate=[&](const Result& r, Opt o, const char* label){
  for(int stereo=0; stereo<2; ++stereo){
    struct C{ int who; int frame; int eye; };
    std::map<uintptr_t,C> content; std::map<std::pair<int,uint64_t>,C> slot; std::map<std::pair<int,uint64_t>,int> saved_frame;
    int bad=0, checks=0;
    for(uint32_t fr=f0+4; fr<f0+20; ++fr){
      auto f=Build(fr,o); int eye = stereo? (fr&1):0;
      std::map<std::pair<uint8_t,uintptr_t>,bool> used_this_frame;
      for(auto& s: f.steps){
        for(auto& it: s.items){
          bool write = Writes(it.state) || (it.state==0 && s.name=="DepthPrepass");
          // a view's first read of a history in a frame: what the same view left there (its own eye's, in stereo)
          if(s.group!=kFrame && (only_view<0 || s.group==only_view) && ((check_mask>>s.group)&1u) && !used_this_frame[{s.group,it.res}] && !Writes(it.state) && (it.res==PD||it.res==H0||it.res==H1||it.res==VA||it.res==VB||it.res==EX)){
            ++checks; auto c=content[it.res];
            // (In stereo the world reads its own eye's last frame (two frames on); the menu's views, one copy for both eyes as in the mod,
            // their last frame.)
            const bool per_eye = stereo && s.group==kWorld; int want_eye = eye;
            bool ok = c.who==(int)s.group && (!per_eye || c.eye==want_eye) && (fr - c.frame) == (per_eye?2:1);
            // alternating resources: written 1 frame before in mono; in stereo the loaded copy was saved 2 frames before
            if(!ok && fr >= f0+6){ ++bad; printf("   [%s] frame %u view %d %s reads %s: content of view %d @%d eye %d (want view %d, eye %d)\n", stereo?"stereo":"mono", fr, (int)s.group, s.name.c_str(), nm[it.res], c.who, c.frame, c.eye, (int)s.group, want_eye); }
          }
          if(s.group!=kFrame) used_this_frame[{s.group,it.res}]=true;
          if(write) content[it.res]=C{ s.group==kFrame ? -1 : (int)s.group, (int)fr, eye };
        }
        // actions after the step: saves first (as ViewHistory does them), then loads
        for(int op=0; op<2; ++op) for(auto& a: r.actions){
          if(a.pass!=s.pass || a.parity!=(fr&1) || (int)a.op!=op) continue;
          // verify item
          bool found=false; for(auto& it: s.items) if(it.item==a.item && it.res==a.resource && (it.state==a.state || (it.resting && it.state==0 && a.state==kUav))) found=true;
          if(!found){ printf("   mismatch at %s\n", s.name.c_str()); ++bad; continue; }
          const bool per_eye_a = stereo && a.view==kWorld;
          if(a.op==kSave){ const int e = per_eye_a ? eye : 0; slot[{e,a.role}]=content[a.resource]; saved_frame[{e,a.role}]=fr; }
          else { int e = per_eye_a ? ((fr+a.ahead)&1) : 0; auto k=std::make_pair(e,a.role); if(slot.count(k)) content[a.resource]=slot[k]; }
        }
      }
    }
    printf("  %s %s: %d checks, %d wrong\n", label, stereo?"stereo":"mono", checks, bad);
  }
  };
  Simulate(r, Opt{}, "base");
  // Edge cases
  {
    Opt e; e.f_read_pd=true; auto re=Plan(Learn(f0,e)); Show("(e) frame step reads prev depth after the menus", re, nm, pname, true); Simulate(re, e, "(e)");
    Opt fb; fb.blind_menu=2; auto rf=Plan(Learn(f0,fb)); Show("(f) a menu step not fully seen", rf, nm, pname, false);
    Opt h; h.ex_inplace=true; auto rh=Plan(Learn(f0,h)); Show("(h) exposure adapted in place", rh, nm, pname, false); Simulate(rh, h, "(h)");
    Opt un; un.unsteady=true; auto ru=Plan(Learn(f0,un)); Show("(i) put-back step differs between frames", ru, nm, pname, false);
    Opt k9; k9.taa_item9=true; auto rk=Plan(Learn(f0,k9)); Show("(k) anti-aliasing history written at its item [9]", rk, nm, pname, true); Simulate(rk, k9, "(k)");
    { auto u=Learn(f0,Opt{}); std::vector<Use> extra; for(auto& x: u) if(pname[x.pass]=="[4]VolTemporal" && x.resource==0) extra.push_back(x); u.insert(u.end(),extra.begin(),extra.end()); auto rj=Plan(u); Show("(j) put-back step ran twice (marks)", rj, nm, pname, false); }
    // (g) the menu's last step for prev depth runs twice a frame
    { auto u=Learn(f0,Opt{}); for(auto& x: u) if(pname[x.pass]=="[4]UpdatePrevDepth") x.single=false; auto rg=Plan(u); Show("(g) put-back step runs twice", rg, nm, pname, false); }
    // (a) previous depth written in a state not known: left alone, the rest kept
    std::vector<Use> u2; for(uint32_t fr=f0; fr<f0+4; ++fr){ auto f=Build(fr); for(auto& s: f.steps) for(auto& it: s.items) if(s.name=="UpdatePrevDepth" && it.res==PD) it.state=0; auto u=Uses(fr,f); u2.insert(u2.end(),u.begin(),u.end()); }
    auto r2=Plan(u2); printf("(a) kept %zu:", r2.kept.size()); for(auto& k: r2.kept) printf(" %s", nm[k.resource]); for(auto& [w,n]: r2.skipped) printf(" | %d %s", n, w.c_str()); printf("\n");
    // (b) world only (normal play): nothing
    std::vector<Use> u3; for(auto& x: uses) if(x.group==kWorld || x.group==kFrame) u3.push_back(x);
    auto r3=Plan(u3); printf("(b) world only: kept %zu actions %zu\n", r3.kept.size(), r3.actions.size());
    // (c) save on a compute list: left alone
    std::vector<Use> u4=uses; for(auto& x: u4) if(x.resource==EX) x.direct=false;
    auto r4=Plan(u4); printf("(c) kept %zu (EX gone:", r4.kept.size()); bool ex=false; for(auto& k: r4.kept) ex|=k.resource==EX; printf(" %s)\n", ex?"no":"yes");
    // (d) learned over 3 frames only
    std::vector<Use> u5; for(auto& x: uses) if(x.frame<f0+3) u5.push_back(x);
    auto r5=Plan(u5); printf("(d) 3 frames: kept %zu actions %zu\n", r5.kept.size(), r5.actions.size());
    // (m) the anti-aliasing history read untracked (its state not known: resting, in unordered access): every view keeps its own
    { Opt m; m.taa_resting=true; auto rm=Plan(Learn(f0,m)); int h=0; for(auto& k: rm.kept) h += (k.resource==H0||k.resource==H1); Show("(m) anti-aliasing history read untracked (resting)", rm, nm, pname, false); printf("(m) anti-aliasing histories kept: %d (want 8)\n", h); Simulate(rm, m, "(m)"); }
    // (n) ...and also read with a tracked state in the frame: not resting then, left alone
    { Opt n; n.taa_resting=true; n.prev_tracked=true; auto rn=Plan(Learn(f0,n)); int h=0; for(auto& k: rn.kept) h += (k.view!=kWorld && (k.resource==H0||k.resource==H1)); printf("(n) tracked use in the frame: the menu views' anti-aliasing histories kept: %d (want 0)\n", h); only_view=kWorld; Simulate(rn, n, "(n) the world"); only_view=-1; }
    // (o) the menu's last view left out (its picture not shown: the Data menu's room): the others keep theirs, read their own
    { auto ro=Plan(uses,true,~(1u<<3)); int v3=0; for(auto& k: ro.kept) v3 += k.view==3; printf("(o) last view left out: kept %zu, the left-out view's %d (want 18, 0)\n", ro.kept.size(), v3); check_mask=~(1u<<3); Simulate(ro, Opt{}, "(o)"); check_mask=0xffffffffu; }
    // (l) the world's histories alone (the setting off): as before fix123, the world reads its own
    { auto rl=Plan(uses,false); bool all_world=true; for(auto& k: rl.kept) all_world &= k.view==kWorld; printf("(l) world histories only: kept %zu, all the world's: %s\n", rl.kept.size(), all_world?"yes":"NO"); only_view=kWorld; Simulate(rl, Opt{}, "(l)"); only_view=-1; }
  }
  return 0;
}
