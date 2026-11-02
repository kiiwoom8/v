# mk.py <uia.c> <out.c> - builds the simulation used by run.sh. It copies
# uia.c's own geometry, filter_containers, cover_overlays and popup_onscreen,
# rescue_rect and rescue_pick (when that uia.c has them) and target_add, and
# adds a main() that repeats scan_window_uia's per-element checks, its taking
# back of offscreen elements into a dialog or menu (rescue_offscreen, as with
# log = 2) and its overlay collection on conv.py's input instead of UI
# Automation, and the focusable-only test of scan_filter (focusable_left_out,
# for an actionable flag of 2; the scroll-area part cannot be simulated).
# Keep that main() in step with scan_window_uia. A function that moved or
# was renamed in uia.c makes this fail loudly.
import sys
src, out = sys.argv[1], sys.argv[2]
s = open(src).read()
def grab(a, b):
    i = s.index(a); j = s.index(b, i) + len(b); return s[i:j]
code = grab("static LONG64 rect_area(const RECT *r)", "return click_points_close(click_a, click_b);\n}\n")
code += "\n" + grab("static BOOL target_add(ScanResult *result", "    return TRUE;\n}\n")
z = grab("// zrects: bounds of the scanned-or-skipped", "static BOOL inside_any(")
code += "\n" + z[:-len("static BOOL inside_any(")]
code += grab("static BOOL inside_any(const RECT *rects", "    free(grid.start);\n}\n")
if "rescue_pick" in s:
    code += "\n" + grab("#define RESCUE_ONSCREEN_MAX", "    return picked;\n}\n")
    code += "\n#define HAVE_RESCUE 1\n"
if "cover_overlays" in s:
    code += "\n" + grab("// A dialog, menu or frame drawn over a web page.", "            fate[i] = COVER_DROPPED;\n    }\n}\n")
    code += "\n#define HAVE_COVER 1\n"
shim = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef int BOOL; typedef long LONG; typedef long long LONG64; typedef wchar_t WCHAR;
#define TRUE 1
#define FALSE 0
#define UIA_MAX_TARGETS 2000
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
static BOOL PtInRect(const RECT *r, POINT p){ return p.x>=r->left && p.x<r->right && p.y>=r->top && p.y<r->bottom; }
static void SetRectEmpty(RECT *r){ memset(r,0,sizeof *r); }
static BOOL EqualRect(const RECT *a, const RECT *b){ return a->left==b->left&&a->top==b->top&&a->right==b->right&&a->bottom==b->bottom; }
static BOOL UnionRect(RECT *o, const RECT *a, const RECT *b){ RECT r; if(a->right<=a->left||a->bottom<=a->top){*o=*b;return TRUE;} if(b->right<=b->left||b->bottom<=b->top){*o=*a;return TRUE;} r.left=a->left<b->left?a->left:b->left; r.top=a->top<b->top?a->top:b->top; r.right=a->right>b->right?a->right:b->right; r.bottom=a->bottom>b->bottom?a->bottom:b->bottom; *o=r; return TRUE; }
#define UIA_DocumentControlTypeId 50030
#define UIA_WindowControlTypeId 50032
#define UIA_MenuControlTypeId 50009
static BOOL IntersectRect(RECT *o, const RECT *a, const RECT *b){ RECT r={a->left>b->left?a->left:b->left, a->top>b->top?a->top:b->top, a->right<b->right?a->right:b->right, a->bottom<b->bottom?a->bottom:b->bottom}; if(r.right<=r.left||r.bottom<=r.top){memset(o,0,sizeof *o);return FALSE;} *o=r; return TRUE; }
typedef struct { RECT rc; POINT click; } ClickTarget;
typedef struct { ClickTarget target; int control_type; BOOL actionable; int order; WCHAR name[128]; } ScanTarget;
typedef struct { ScanTarget *items; int count; int cap; BOOL fg_done; } ScanResult;
'''
main = r'''
#define T_DOC 50030
#define T_SCROLLBAR 50014
#define T_MENU 50009
#define T_WINDOW 50032
#define T_TEXT 50020
#define T_PANE 50033
#define T_LIST 50008
#define T_TABLE 50036
#define T_TREE 50023
static struct { int type, act, off; RECT rc; char name[80]; } el[3000];
static RECT raw[3000], taken[3000]; static int types[3000]; static int page_doc=-1;
int main(void){
  RECT monitor={0,0,1600,1000}; RECT zrects[1]={{0,0,1600,1000}};
  RECT documents[16]; int doc_order[16]; int document_count=0; RECT scrollbars[1]; int scrollbar_count=0;
  static ScanTarget cands[3000]; static char names[3000][80]; int n=0, i, len=0;
  int type, act, off; long l,t,w,h; char name[80];
  if (scanf(" #monitor %ld %ld", &w, &h) == 2) { monitor.right=w; monitor.bottom=h; zrects[0]=monitor; }
  while (scanf("%d %d %d %ld %ld %ld %ld %79[^\n]", &type,&act,&off,&l,&t,&w,&h,name)==8) {
    el[len].type=type; el[len].act=act; el[len].off=off; el[len].rc=(RECT){l,t,l+w,t+h}; strcpy(el[len].name,name); len++; }
  for (i=0;i<len;i++){ types[i]=el[i].type; if(!el[i].off) raw[i]=el[i].rc; }
  for (i=0;i<len;i++) if (el[i].type==T_DOC && !el[i].off && document_count<16) { doc_order[document_count]=i; documents[document_count++]=el[i].rc; }
  for (i=1;i<document_count;++i){int d; for(d=0;d<i;++d){RECT inter; if(IntersectRect(&inter,&documents[i],&documents[d])) documents[i]=inter;}}
  { int d; LONG64 pa=0; page_doc=-1; for(d=0;d<document_count;d++){RECT on; LONG64 a=IntersectRect(&on,&documents[d],&monitor)?rect_area(&on):0; if(a>pa){pa=a;page_doc=d;}} }
#ifdef HAVE_RESCUE
  /* rescue_offscreen, as with log = 2: the offscreen elements in a dialog's or menu's run */
  if (document_count) { int page=0, d, k, taken_until=-1; LONG64 pa=0;
    for(d=0;d<document_count;d++){RECT on; LONG64 a=IntersectRect(&on,&documents[d],&monitor)?rect_area(&on):0; if(a>pa){pa=a;page=d;}}
    for (k=0;k<len;k++){ RECT inter, offr[256]; int idx[256], end, onscreen, nn=0, e, picked; static BOOL take[256];
      if ((el[k].type!=T_MENU&&el[k].type!=T_WINDOW)||el[k].off||rect_area(&el[k].rc)<=0||k<taken_until) continue;
      if(!IntersectRect(&inter,&el[k].rc,&documents[page])||rect_area(&el[k].rc)*10>=pa*9) continue;
      onscreen=popup_onscreen(raw,types,len,k,&el[k].rc,&end);
      if (onscreen>RESCUE_ONSCREEN_MAX) continue;
      for(e=k+1;e<end&&nn<256;e++){ RECT clip; if(rect_area(&raw[e])>0||!el[e].off) continue;
        if(types[e]==T_DOC||types[e]==T_MENU||types[e]==T_WINDOW||types[e]==T_SCROLLBAR) continue;
        if(!rescue_rect(&el[e].rc,&el[k].rc,&clip)) continue; idx[nn]=e; offr[nn++]=clip; }
      picked=rescue_pick(raw,types,k,end,&el[k].rc,onscreen,offr,nn,take);
      for(e=0;e<nn;e++) if(take[e]){ taken[idx[e]]=offr[e]; printf("  %-36s offscreen, taken back into its dialog or menu\n", el[idx[e]].name); }
      if(picked) taken_until=end; }
    for (i=0;i<len;i++) if (rect_area(&taken[i])>0) raw[i]=taken[i]; }
#endif
  for (i=0;i<len;i++){
    RECT rc=el[i].rc, clip; POINT click; LONG64 orig_area, clip_area, monitor_area; const char *why=NULL;
    if (el[i].off && rect_area(&taken[i])<=0) { printf("  %-36s offscreen\n", el[i].name); continue; }
    if (rect_area(&taken[i])>0) rc=taken[i];
    if (el[i].type==T_DOC || el[i].type==T_MENU || el[i].type==T_WINDOW) continue;
    if (el[i].act==2) { /* focusable_left_out */
      RECT inter, on; int t=el[i].type;
      if (t==T_TEXT||t==T_PANE||t==T_LIST||t==T_TABLE||t==T_TREE) why="focusable only: text or a container";
      else if (page_doc<0||!IntersectRect(&inter,&rc,&documents[page_doc])) why="focusable only: outside the page";
      else if (IntersectRect(&on,&documents[page_doc],&monitor) && rect_area(&rc)*4>rect_area(&on)) why="focusable only: too large";
      if (why){ printf("  %-36s %s\n", el[i].name, why); continue; } }
    orig_area=rect_area(&rc);
    if (orig_area<=0) why="zero area";
    else if (el[i].type==T_SCROLLBAR || inside_any(scrollbars,scrollbar_count,&rc)) why="scrollbar";
    else if (!IntersectRect(&clip,&rc,&monitor)) why="off monitor";
    if (!why){ clip_area=rect_area(&clip); monitor_area=rect_area(&monitor);
      if (clip_area*2<orig_area) why="mostly off monitor"; }
    if (!why){ const RECT *best=NULL; LONG64 ba=0; int d; for(d=0;d<document_count;++d){RECT dm,in; LONG64 a; if(!IntersectRect(&dm,&documents[d],&monitor)||!IntersectRect(&in,&rc,&documents[d])) continue; a=rect_area(&dm); if(a>ba){ba=a;best=&documents[d];}} if(best) IntersectRect(&clip,&clip,best);
      clip_area=rect_area(&clip);
      if (clip.right-clip.left<4||clip.bottom-clip.top<4) why="too small";
      else if (monitor_area>0 && clip_area*100>monitor_area*40) why="too large"; }
    if (!why){ click=choose_click_point(zrects,0,&clip); if(click.x<0&&click.y<0) why="hidden"; }
    if (why){ printf("  %-36s %s\n", el[i].name, why); continue; }
    memset(&cands[n],0,sizeof cands[n]); cands[n].target.rc=clip; cands[n].target.click=click; cands[n].control_type=el[i].type; cands[n].actionable=el[i].act==1; cands[n].order=i; strcpy(names[n],el[i].name); n++;
  }
  { static BOOL drop[3000]; static POINT nc[3000]; ScanResult res; int kept=0; memset(&res,0,sizeof res);
    filter_containers(cands,n,zrects,0,NULL,drop,nc);
    for (i=0;i<n;i++){ if(drop[i]){ printf("  %-36s container\n", names[i]); continue;} cands[i].target.click=nc[i]; cands[kept]=cands[i]; strcpy(names[kept],names[i]); kept++; }
#ifdef HAVE_COVER
    { Overlay ovs[32]; int ov=0, d, k, page=0; LONG64 pa=0; static int fate[3000]; int m=0;
      for(d=0;d<document_count;d++){RECT on; LONG64 a=IntersectRect(&on,&documents[d],&monitor)?rect_area(&on):0; if(a>pa){pa=a;page=d;}}
      for(d=0;d<document_count&&ov<32;d++){ if(d==page||rect_area(&documents[d])<=0) continue; ovs[ov].rc=documents[d]; ovs[ov].order=doc_order[d]; ovs[ov].drops=FALSE; ov++; }
      for(k=0;k<len&&ov<32;k++){ RECT inter; if((el[k].type!=T_MENU&&el[k].type!=T_WINDOW)||el[k].off||rect_area(&el[k].rc)<=0) continue; if(!document_count) continue;
        if(!IntersectRect(&inter,&el[k].rc,&documents[page])||rect_area(&el[k].rc)*10>=pa*9) continue; ovs[ov].rc=el[k].rc; ovs[ov].order=k; ovs[ov].drops=TRUE; ov++; }
      for(k=0;k<ov;k++){ int e=ovs[k].order+1; RECT inter; while(e<len&&(rect_area(&raw[e])<=0||IntersectRect(&inter,&raw[e],&ovs[k].rc))) ++e; ovs[k].end=e;
        printf("  overlay %s %ld,%ld %ldx%ld tree %d-%d\n", ovs[k].drops?"dialog/menu":"frame", ovs[k].rc.left, ovs[k].rc.top, ovs[k].rc.right-ovs[k].rc.left, ovs[k].rc.bottom-ovs[k].rc.top, ovs[k].order, e); }
      memset(fate,0,sizeof fate);
      if (ov) cover_overlays(cands,kept,ovs,ov,zrects,0,fate);
      for(i=0;i<kept;i++){ if(fate[i]==COVER_DROPPED){ printf("  %-36s under a dialog or menu\n", names[i]); continue;} if(fate[i]==COVER_MOVED) printf("  %-36s moved off an overlay\n", names[i]); cands[m]=cands[i]; strcpy(names[m],names[i]); m++; }
      kept=m; }
#endif
    for (i=0;i<kept;i++){ ScanTarget t=cands[i]; strncpy((char*)t.name, names[i], 120);
        if(!target_add(&res,&t)) printf("  %-36s duplicate\n", names[i]); }
    printf("LABELLED:\n");
    for (i=0;i<res.count;i++) printf("  %-36s label %ld,%ld click %ld,%ld\n", (char*)res.items[i].name, res.items[i].target.rc.left, res.items[i].target.rc.top, res.items[i].target.click.x, res.items[i].target.click.y);
  }
  return 0; }
'''
open(out,'w').write(shim+code+main)
