/* Runtime helpers for stdlib time, hashing, and JSON. */
#include "runtime_internal.h"
#include <ctype.h>

Value em_utc_date(Value seconds) {
    if (!is_num(seconds)) rt_fatal("utc_date() seconds must be numeric");
    time_t t = (time_t)as_double(seconds);
    struct tm tm;
    if (!gmtime_r(&t, &tm)) rt_fatal("utc_date() cannot represent timestamp");
    return em_rec_litn(6, "year", em_int(tm.tm_year + 1900),
        "month", em_int(tm.tm_mon + 1), "day", em_int(tm.tm_mday),
        "hour", em_int(tm.tm_hour), "minute", em_int(tm.tm_min),
        "second", em_int(tm.tm_sec));
}

Value em_fnv1a(Value text) {
    if (!is_str(text)) rt_fatal("fnv1a() argument must be str");
    uint64_t h = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)str_data(&text);
    for (size_t i = 0; i < str_len(&text); i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return em_int((int64_t)h);
}

/* SHA-256, kept here rather than depending on OpenSSL (the compiler has no
 * external dependencies). */
static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static const uint32_t K[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
Value em_sha256(Value text) {
    if (!is_str(text)) rt_fatal("sha256() argument must be str");
    size_t n = str_len(&text), total = ((n + 9 + 63) / 64) * 64;
    unsigned char *msg = xmalloc(total); memset(msg, 0, total);
    memcpy(msg, str_data(&text), n); msg[n] = 0x80;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) msg[total - 1 - i] = (unsigned char)(bits >> (i * 8));
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = ((uint32_t)msg[off+4*i]<<24)|((uint32_t)msg[off+4*i+1]<<16)|((uint32_t)msg[off+4*i+2]<<8)|msg[off+4*i+3];
        for (int i = 16; i < 64; i++) { uint32_t a=w[i-15],b=w[i-2]; w[i]=w[i-16]+(rotr(a,7)^rotr(a,18)^(a>>3))+w[i-7]+(rotr(b,17)^rotr(b,19)^(b>>10)); }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for (int i=0;i<64;i++) { uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25), ch=(e&f)^((~e)&g), t1=z+S1+ch+K[i]+w[i]; uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22), maj=(a&b)^(a&c)^(b&c), t2=S0+maj; z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2; }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
    }
    free(msg); char out[65]; for (int i=0;i<8;i++) snprintf(out+i*8,9,"%08x",h[i]); out[64]='\0'; return em_str_new(out);
}

static void jput(SB *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        while (b->cap < b->len + n + 1) b->cap *= 2;
        b->buf = xrealloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, s, n); b->len += n; b->buf[b->len] = '\0';
}
static void jputs(SB *b, const char *s) { jput(b, s, strlen(s)); }
typedef struct { const char *p, *end; } JP;
static Value jstr(JP *j);
static void ws(JP *j) { while (j->p < j->end && isspace((unsigned char)*j->p)) j->p++; }
static Value jval(JP *j);

static Value jarray(JP *j) {
    j->p++;
    Value out = em_list_litn(0), roots[1] = {out};
    RootFrame fr; rt_push_frame(&fr, roots, 1);
    ws(j);
    if (j->p < j->end && *j->p == ']') { j->p++; rt_pop_frame(); return out; }
    while (1) {
        Value item = jval(j);
        em_append(out, item);
        ws(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == ']') { j->p++; rt_pop_frame(); return out; }
        rt_fatal("invalid JSON array");
    }
}

static Value jobject(JP *j) {
    j->p++;
    Value out = em_dict_litn(0), key = em_none(), value = em_none();
    Value roots[3] = {out, key, value};
    RootFrame fr; rt_push_frame(&fr, roots, 3);
    ws(j);
    if (j->p < j->end && *j->p == '}') { j->p++; rt_pop_frame(); return out; }
    while (1) {
        ws(j);
        if (j->p >= j->end || *j->p != '"') rt_fatal("JSON object key must be string");
        key = jstr(j); roots[1] = key;
        ws(j);
        if (j->p >= j->end || *j->p++ != ':') rt_fatal("invalid JSON object");
        value = jval(j); roots[2] = value;
        em_dict_set(out, key, value);
        ws(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == '}') { j->p++; rt_pop_frame(); return out; }
        rt_fatal("invalid JSON object");
    }
}
static Value jstr(JP *j) {
    j->p++; SB b={0};
    while (j->p < j->end && *j->p != '"') { unsigned char c=(unsigned char)*j->p++; if(c!='\\'){ char q=c; jput(&b,(char *)&q,1); continue; } if(j->p>=j->end) rt_fatal("invalid JSON string"); c=(unsigned char)*j->p++; char q; switch(c){case '"':q='"';break;case '\\':q='\\';break;case '/':q='/';break;case 'b':q='\b';break;case 'f':q='\f';break;case 'n':q='\n';break;case 'r':q='\r';break;case 't':q='\t';break;default:rt_fatal("unsupported JSON escape");} jput(&b,&q,1); }
    if(j->p>=j->end) rt_fatal("unterminated JSON string"); j->p++; Value v=str_copy(b.buf?b.buf:"",b.len); free(b.buf); return v;
}
static Value jval(JP *j) {
    ws(j); if(j->p>=j->end) rt_fatal("unexpected end of JSON");
    if(*j->p=='"') return jstr(j);
    if(*j->p=='t' && j->end-j->p>=4 && !strncmp(j->p,"true",4)){j->p+=4;return em_bool(true);}
    if(*j->p=='f' && j->end-j->p>=5 && !strncmp(j->p,"false",5)){j->p+=5;return em_bool(false);}
    if(*j->p=='n' && j->end-j->p>=4 && !strncmp(j->p,"null",4)){j->p+=4;return em_none();}
    if (*j->p == '[') return jarray(j);
    if(*j->p=='['){j->p++;Value out=em_list_litn(0);ws(j);if(j->p<j->end&&*j->p==']'){j->p++;return out;}while(1){em_append(out,jval(j));ws(j);if(j->p<j->end&&*j->p==','){j->p++;continue;}if(j->p<j->end&&*j->p==']'){j->p++;return out;}rt_fatal("invalid JSON array");}}
    if (*j->p == '{') return jobject(j);
    if(*j->p=='{'){j->p++;Value out=em_dict_litn(0);ws(j);if(j->p<j->end&&*j->p=='}'){j->p++;return out;}while(1){ws(j);if(j->p>=j->end||*j->p!='"')rt_fatal("JSON object key must be string");Value k=jstr(j);ws(j);if(j->p>=j->end||*j->p++!=':')rt_fatal("invalid JSON object");em_dict_set(out,k,jval(j));ws(j);if(j->p<j->end&&*j->p==','){j->p++;continue;}if(j->p<j->end&&*j->p=='}'){j->p++;return out;}rt_fatal("invalid JSON object");}}
    char *e; double d=strtod(j->p,&e); if(e==j->p)rt_fatal("invalid JSON value"); bool real=false;for(const char*q=j->p;q<e;q++)if(*q=='.'||*q=='e'||*q=='E')real=true;j->p=e;return real?em_float(d):em_int((int64_t)d);
}
Value em_json_parse(Value text) { if(!is_str(text))rt_fatal("json_parse() argument must be str"); JP j={str_data(&text),str_data(&text)+str_len(&text)};Value v=jval(&j);ws(&j);if(j.p!=j.end)rt_fatal("trailing JSON input");return v; }

static void jwrite(SB *b, Value v) {
    if(v.tag==V_NONE){jputs(b,"null");return;} if(v.tag==V_BOOL){jputs(b,v.as.b?"true":"false");return;} if(v.tag==V_INT||v.tag==V_FLOAT){write_value(b,v,false);return;}
    if(is_str(v)){jputs(b,"\"");for(size_t i=0;i<str_len(&v);i++){unsigned char c=(unsigned char)str_data(&v)[i];if(c=='"'||c=='\\'){char q='\\';jput(b,&q,1);}if(c=='\n')jputs(b,"\\n");else if(c=='\r')jputs(b,"\\r");else if(c=='\t')jputs(b,"\\t");else jput(b,(char *)&c,1);}jputs(b,"\"");return;}
    if(is_list(v)||is_tuple(v)){jputs(b,"[");for(size_t i=0;i<v.as.o->as.list.len;i++){if(i)jputs(b,",");jwrite(b,v.as.o->as.list.items[i]);}jputs(b,"]");return;}
    if(v.tag==V_OBJ&&v.as.o->tag==O_DICT){jputs(b,"{");for(size_t i=0;i<v.as.o->as.dict.len;i++){if(i)jputs(b,",");jwrite(b,v.as.o->as.dict.keys[i]);jputs(b,":");jwrite(b,v.as.o->as.dict.vals[i]);}jputs(b,"}");return;}
    rt_fatal("value is not JSON-serializable");
}
Value em_json_stringify(Value value){SB b={0};jwrite(&b,value);Value s=str_copy(b.buf?b.buf:"",b.len);free(b.buf);return s;}
