/* Drive rc_client through login -> load -> N frames with synthetic DS RAM.
 * Measures heap (wrapped malloc) and marks phases for qemu instruction counting. */
#include "rc_client.h"
#include "rc_consoles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include SET_HEADER

/* ---- heap accounting (8-byte header: size, caller) ---- */
static void reg(void* p); static void unreg(void* p);
void* __real_malloc(size_t); void __real_free(void*); void* __real_realloc(void*,size_t); void* __real_calloc(size_t,size_t);
static size_t cur, peak, n_alloc, n_free, n_realloc; static int in_frame; static size_t frame_allocs, frame_frees;
static void* g_caller; void* __wrap_malloc(size_t n){ void* ra=g_caller?g_caller:__builtin_return_address(0); g_caller=0; size_t* p=__real_malloc(n+8); if(!p) return 0; p[0]=n; p[1]=(size_t)ra; cur+=n; if(cur>peak) peak=cur; n_alloc++; if(in_frame) frame_allocs++; reg(p); return (char*)p+8; }
void* __wrap_calloc(size_t a,size_t b){ g_caller=__builtin_return_address(0); void* p=__wrap_malloc(a*b); if(p) memset(p,0,a*b); return p; }
void __wrap_free(void* v){ if(!v) return; size_t* p=(size_t*)((char*)v-8); cur-=p[0]; n_free++; if(in_frame) frame_frees++; unreg(p); __real_free(p); }
void* __wrap_realloc(void* v,size_t n){ if(!v){ g_caller=__builtin_return_address(0); return __wrap_malloc(n);} size_t* p=(size_t*)((char*)v-8); size_t old=p[0];
  { size_t* q=p; p=__real_realloc(p,n+8); if(!p) return 0; unreg(q); reg(p); } p[0]=n; p[1]=(size_t)__builtin_return_address(0); cur=cur-old+n; if(cur>peak) peak=cur; n_realloc++; if(in_frame) frame_allocs++; return (char*)p+8; }

#define MAXB 8192
static void* blocks[MAXB];
static void reg(void* p){ int i; for(i=0;i<MAXB;i++) if(!blocks[i]){blocks[i]=p;return;} }
static void unreg(void* p){ int i; for(i=0;i<MAXB;i++) if(blocks[i]==p){blocks[i]=0;return;} }
static void dump_live(void){ int i; for(i=0;i<MAXB;i++) if(blocks[i]){ size_t* p=blocks[i]; printf("LIVE %08x %u\n",(unsigned)p[1],(unsigned)p[0]); } }
/* ---- phase markers: qemu exec trace counts instructions between these ---- */
volatile int marker_sink;
void __attribute__((noinline)) mark_begin(int id){ marker_sink=id; __asm__ volatile(""); }
void __attribute__((noinline)) mark_end(int id){ marker_sink=-id; __asm__ volatile(""); }

/* ---- synthetic DS memory: 4 MB main RAM + 16 KB DTCM, frozen per frame ---- */
static uint8_t ram[0x400000], dtcm[0x4000];
static uint32_t rng=12345; static uint32_t rnd(void){ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static uint32_t read_mem(uint32_t a, uint8_t* b, uint32_t n, rc_client_t* c){ (void)c;
  if (a < 0x400000 && a+n <= 0x400000) { memcpy(b, ram+a, n); return n; }
  if (a >= 0x1000000 && a+n <= 0x1004000) { memcpy(b, dtcm+(a-0x1000000), n); return n; }
  return 0; }
static void server(const rc_api_request_t* r, rc_client_server_callback_t cb, void* d, rc_client_t* c){ (void)c;
  rc_api_server_response_t resp; const char* body="{\"Success\":true}";
  const char* p=r->post_data;
  if (strstr(p,"r=login2")) body="{\"Success\":true,\"User\":\"User\",\"AvatarUrl\":\"http://server/UserPic/USER.png\",\"Token\":\"ApiToken\",\"Score\":12345,\"Messages\":2}";
  else if (strstr(p,"r=achievementsets")) body=patch_json;
  else if (strstr(p,"r=startsession")) body="{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}";
  else if (strstr(p,"r=awardachievement")) body="{\"Success\":true,\"Score\":5432,\"SoftcoreScore\":777,\"AchievementID\":5,\"AchievementsRemaining\":6}";
  resp.body=body; resp.body_length=strlen(body); resp.http_status_code=200;
  cb(&resp,d); }
static int events[32];
static void ev(const rc_client_event_t* e, rc_client_t* c){ (void)c; if(e->type<32) events[e->type]++; }
static void done(int r, const char* m, rc_client_t* c, void* u){ (void)c;(void)u; if(r) printf("callback error %d: %s\n", r, m?m:""); }
static void lg(const char* m, const rc_client_t* c){ (void)c; printf("LOG: %s\n", m); }
static rc_clock_t now_ms; static rc_clock_t clk(const rc_client_t* c){ (void)c; return now_ms; }

int main(int argc, char** argv){
  int frames = argc>1 ? atoi(argv[1]) : 600; int mutate = argc>2 ? atoi(argv[2]) : 256;
  size_t base, after_login, after_load, frame_peak=0, worst_frame_allocs=0; int i;
  char rp[256];
  for (i=0;i<(int)sizeof(ram);i+=4){ uint32_t v=rnd(); memcpy(ram+i,&v,4); }
  /* make pointer slots look like DS pointers (0x02xxxxxx) so AddAddress chains resolve in range */
  for (i=0;i<(int)sizeof(ram);i+=4){ if ((ram[i]&7)==0){ uint32_t v=0x02000000u|(rnd()&0x3FFFFCu); memcpy(ram+i,&v,4);} }
  mark_begin(1);
  rc_client_t* c = rc_client_create(read_mem, server);
  if (argc>3) rc_client_enable_logging(c, atoi(argv[3]), lg);
  rc_client_set_event_handler(c, ev);
  rc_client_set_get_time_millisecs_function(c, clk);
  rc_client_set_hardcore_enabled(c, 1);
  base=cur;
  rc_client_begin_login_with_token(c, "User", "ApiToken", done, NULL);
  after_login=cur;
  mark_end(1);
  mark_begin(2);
  rc_client_begin_load_game(c, "0123456789ABCDEF0123456789ABCDEF", done, NULL);
  mark_end(2);
  after_load=cur;
  if (argc>4) dump_live();
  printf("game loaded: %s\n", rc_client_is_game_loaded(c)?"yes":"NO");
  { rc_client_user_game_summary_t s; rc_client_get_user_game_summary(c,&s); printf("core achievements: %u, unlocked %u\n", s.num_promoted_achievements, s.num_unlocked_achievements); }
  for (i=0;i<frames;i++){
    int k; size_t before=cur;
    for (k=0;k<mutate;k++){ uint32_t a=rnd()&0x3FFFFF; ram[a]=(uint8_t)rnd(); }
    for (k=0;k<mutate/16;k++) dtcm[rnd()&0x3FFF]=(uint8_t)rnd();
    now_ms += 16;
    frame_allocs=frame_frees=0; in_frame=1;
    mark_begin(3);
    rc_client_do_frame(c);
    mark_end(3);
    in_frame=0;
    if (frame_allocs>worst_frame_allocs) worst_frame_allocs=frame_allocs;
    if (cur>before && cur-before>frame_peak) frame_peak=cur-before;
    if (i % 60 == 0) rc_client_idle(c);
  }
  mark_begin(4);
  rc_client_get_rich_presence_message(c, rp, sizeof(rp));
  mark_end(4);
  printf("rich presence: %s\n", rp);
  printf("events: triggered=%d lb_started=%d lb_submitted=%d progress=%d challenge_show=%d\n", events[RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED], events[RC_CLIENT_EVENT_LEADERBOARD_STARTED], events[RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED], events[RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE], events[RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW]);
  printf("HEAP client_create=%u after_login=%u after_load=%u now=%u PEAK=%u\n",(unsigned)base,(unsigned)after_login,(unsigned)after_load,(unsigned)cur,(unsigned)peak);
  printf("HEAP calls: malloc=%u realloc=%u free=%u; worst frame: %u allocs, worst net growth in one frame %u bytes\n",(unsigned)n_alloc,(unsigned)n_realloc,(unsigned)n_free,(unsigned)worst_frame_allocs,(unsigned)frame_peak);
  rc_client_unload_game(c); rc_client_destroy(c);
  printf("HEAP after destroy=%u (leak check)\n",(unsigned)cur);
  return 0;
}
