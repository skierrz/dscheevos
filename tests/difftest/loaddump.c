/* loaddump SET.json: load the set through rc_client and print what the load built: the
 * game's memref lists (modified memrefs with their operands) and every achievement's and
 * leaderboard's conditions, memrefs as indexes in those lists. Built against stock and
 * patched rcheevos (loadcmp.sh); for a patch to the load path, the two outputs must match
 * byte for byte on every set: then evaluation runs on the same structures. */
#include "rc_client.h"
#include "client/rc_client_internal.h"
#include "runtime/rc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char* set_json;
static uint32_t rd(uint32_t a, uint8_t* b, uint32_t n, rc_client_t* c){(void)a;(void)c; memset(b,0,n); return n;}
static const char* answer(const char* p){
  if(strstr(p,"r=login2")) return "{\"Success\":true,\"User\":\"User\",\"Token\":\"ApiToken\",\"Score\":1,\"Messages\":0}";
  if(strstr(p,"r=achievementsets")) return set_json;
  if(strstr(p,"r=startsession")) return "{\"Success\":true,\"Unlocks\":[],\"HardcoreUnlocks\":[]}";
  return "{\"Success\":true}";}
static void srv(const rc_api_request_t* r, rc_client_server_callback_t cb, void* d, rc_client_t* c){
  rc_api_server_response_t s; memset(&s,0,sizeof s); s.body=answer(r->post_data); s.body_length=strlen(s.body); s.http_status_code=200; cb(&s,d);}
static void done(int r,const char*m,rc_client_t*c,void*u){(void)c;(void)u; if(r) printf("cb %d %s\n",r,m?m:"");}
static rc_memrefs_t* M;
static void ref(const rc_memref_t* m){
  const rc_memref_list_t* l; const rc_modified_memref_list_t* ml; uint32_t base=0;
  if(!m){printf("null");return;}
  for(l=&M->memrefs;l;l=l->next){ if(m>=l->items&&m<l->items+l->count){printf("D%u",base+(uint32_t)(m-l->items));return;} base+=l->count; }
  base=0;
  for(ml=&M->modified_memrefs;ml;ml=ml->next){ const rc_modified_memref_t* p=(const rc_modified_memref_t*)m;
    if(p>=ml->items&&p<ml->items+ml->count){printf("M%u",base+(uint32_t)(p-ml->items));return;} base+=ml->count; }
  printf("X(%u,%u)",m->address,m->value.size);   /* outside the shared list */
}
static void op(const rc_operand_t* o){
  printf("[t%u s%u a%u ",o->type,o->size,o->memref_access_type);
  if(o->type==RC_OPERAND_CONST) printf("%u",o->value.num);
  else if(o->type==RC_OPERAND_FP) printf("%.17g",o->value.dbl);
  else if(rc_operand_is_memref(o)||o->type==RC_OPERAND_RECALL||o->type==RC_OPERAND_ADDRESS) ref(o->value.memref);
  else printf("?%u",o->value.num);
  printf("]");
}
static void condsets(const rc_condset_t* cs){
  for(;cs;cs=cs->next){ const rc_condition_t* c; printf("  set p%u r%u h%u o%u\n",cs->num_pause_conditions,cs->num_reset_conditions,cs->num_hittarget_conditions,cs->num_other_conditions);
    for(c=cs->conditions;c;c=c->next){ printf("   c t%u o%u h%u ",c->type,c->oper,c->required_hits); op(&c->operand1); op(&c->operand2); printf("\n"); } }
}
static void trig(const char* n,const rc_trigger_t* t){ if(!t){printf(" %s none\n",n);return;} printf(" %s mt%u\n",n,t->measured_target); condsets(t->requirement); condsets(t->alternative); }
int main(int argc,char**argv){
  FILE*f=fopen(argv[1],"rb"); long n; fseek(f,0,2); n=ftell(f); fseek(f,0,0); set_json=malloc(n+1); n=fread(set_json,1,n,f); set_json[n]=0; fclose(f);
  rc_client_t* c=rc_client_create(rd,srv); rc_client_set_hardcore_enabled(c,1);
  rc_client_begin_login_with_token(c,"User","ApiToken",done,NULL);
  rc_client_begin_load_game(c,"0123456789ABCDEF0123456789ABCDEF",done,NULL);
  if(!c->game){printf("not loaded\n");return 0;}
  M=c->game->runtime.memrefs;
  { const rc_memref_list_t* l; const rc_modified_memref_list_t* ml; uint32_t i;
    for(l=&M->memrefs;l;l=l->next) for(i=0;i<l->count;i++) printf("D %u s%u t%u\n",l->items[i].address,l->items[i].value.size,l->items[i].value.type);
    for(ml=&M->modified_memrefs;ml;ml=ml->next) for(i=0;i<ml->count;i++){ const rc_modified_memref_t* m=&ml->items[i];
      printf("M s%u t%u mt%u d%u a%u ",m->memref.value.size,m->memref.value.type,m->modifier_type,m->depth,m->memref.address); op(&m->parent); op(&m->modifier); printf("\n"); } }
  { const rc_client_subset_info_t* s; for(s=c->game->subsets;s;s=s->next){ uint32_t i;
    for(i=0;i<s->public_.num_achievements;i++){ const rc_client_achievement_info_t* a=&s->achievements[i];
      printf("ach %u state%u bucket%u\n",a->public_.id,a->public_.state,a->public_.bucket); trig("trigger",a->trigger); }
    for(i=0;i<s->public_.num_leaderboards;i++){ const rc_client_leaderboard_info_t* b=&s->leaderboards[i];
      printf("lb %u state%u\n",b->public_.id,b->public_.state); if(b->lboard){ trig("start",&b->lboard->start); trig("submit",&b->lboard->submit); trig("cancel",&b->lboard->cancel); printf(" value\n"); condsets(b->lboard->value.conditions);} } } }
  return 0;}
