/*
 * llm_server.c — AIOS LLM Inference PD
 *
 * Freestanding port of karpathy/llama2.c (run.c) for seL4 Microkit.
 * No libc dependencies. Model weights are in shared model_data region.
 * Tokenizer is loaded separately into tokenizer region.
 * Communication with orchestrator via llm_io shared buffer.
 */

#include <stdint.h>
#include <microkit.h>
#include "aios/ipc.h"

/* ══════════════════════════════════════════════
   Shared memory regions
   ══════════════════════════════════════════════ */
uintptr_t model_data;    /* 128 MiB: model weights (read-only) */
uintptr_t llm_io;        /* 4 KiB: IPC with orchestrator */

/* We need a large working memory for RunState (KV cache, activations).
   For stories15M: ~6 MB for KV cache + ~1 MB for activations.
   We'll use a static buffer. */
//#define WORK_MEM_SIZE  (16 * 1024 * 1024)  /* 16 MiB */
//static uint8_t work_mem[WORK_MEM_SIZE] __attribute__((aligned(16)));
//static uint32_t work_mem_used = 0;

#define WORK_MEM_SIZE  (16 * 1024 * 1024)  /* 16 MiB */
static uint8_t *work_mem;
static uint32_t work_mem_used = 0;


/* ══════════════════════════════════════════════
   Freestanding memory/string helpers
   ══════════════════════════════════════════════ */

/* GCC freestanding requires these symbols even with -ffreestanding */
void *memcpy(void *dst, const void *src, unsigned long n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, unsigned long n) {
    uint8_t *d = (uint8_t *)dst;
    for (unsigned long i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}


static void *my_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void *my_memset(void *dst, int val, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = (uint8_t)val;
    return dst;
}

__attribute__((unused)) static int my_strlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

__attribute__((unused)) static void my_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

/* Simple bump allocator */
static void *bump_alloc(uint32_t size) {
    /* align to 16 bytes */
    work_mem_used = (work_mem_used + 15) & ~15u;
    if (work_mem_used + size > WORK_MEM_SIZE) {
        microkit_dbg_puts("LLM: OOM!\n");
        return (void *)0;
    }
    void *ptr = &work_mem[work_mem_used];
    work_mem_used += size;
    return ptr;
}

static void *bump_calloc(uint32_t count, uint32_t size) {
    uint32_t total = count * size;
    void *p = bump_alloc(total);
    if (p) my_memset(p, 0, total);
    return p;
}

//static void bump_reset(void) {
//    work_mem_used = 0;
//}

/* ══════════════════════════════════════════════
   Freestanding math (single-precision)
   ══════════════════════════════════════════════ */

/* sqrtf via Newton's method */
static float my_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    union { float f; uint32_t i; } u = { .f = x };
    u.i = (u.i >> 1) + 0x1FC00000u; /* initial guess */
    float g = u.f;
    for (int i = 0; i < 6; i++) g = 0.5f * (g + x / g);
    return g;
}

/* expf via range reduction + polynomial */
static float my_expf(float x) {
    if (x < -88.0f) return 0.0f;
    if (x >  88.0f) return 1e38f;
    /* exp(x) = 2^(x/ln2) = 2^k * 2^f where k=floor(x/ln2), f=frac */
    float ln2 = 0.6931471805599453f;
    float inv_ln2 = 1.4426950408889634f;
    float t = x * inv_ln2;
    int k = (int)t;
    if (t < 0.0f && t != (float)k) k--;
    float f = x - (float)k * ln2;
    /* exp(f) via Horner, |f| < ln2/2 */
    float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.166666667f + f * (0.041666667f + f * 0.008333333f))));
    /* multiply by 2^k */
    union { float f; uint32_t i; } u;
    u.f = p;
    u.i += (uint32_t)k << 23;
    return u.f;
}

/* sinf/cosf via polynomial (Cephes-style) */
static float my_sinf(float x);
static float my_cosf(float x);

static float _sin_kern(float x) {
    float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.008333333f + x2 * -0.0001984127f)));
}
static float _cos_kern(float x) {
    float x2 = x * x;
    return 1.0f + x2 * (-0.5f + x2 * (0.041666667f + x2 * -0.001388889f));
}

static float my_sinf(float x) {
    float PI = 3.14159265358979323846f;
    /* reduce to [-pi, pi] */
    while (x > PI) x -= 2.0f * PI;
    while (x < -PI) x += 2.0f * PI;
    return _sin_kern(x);
}

static float my_cosf(float x) {
    float PI = 3.14159265358979323846f;
    while (x > PI) x -= 2.0f * PI;
    while (x < -PI) x += 2.0f * PI;
    return _cos_kern(x);
}

static float my_powf(float base, float exp) {
    /* only used for RoPE: powf(10000, x) where x is small fraction */
    return my_expf(exp * (float)(2.302585093f * 4.0f + /* ln(10000) = 9.21034 */
           2.302585093f * 0.0f)); /* oops, let me do this properly */
}

/* ══════════════════════════════════════════════
   Channel IDs
   ══════════════════════════════════════════════ */
#define CH_ORCH  6

/* ══════════════════════════════════════════════
   llm_io layout
   ══════════════════════════════════════════════ */
//#define LLM_CMD       0x000
//#define LLM_STATUS    0x004
//#define LLM_MODELSZ   0x008
//#define LLM_TOKCOUNT  0x00C
//#define LLM_TOKSZ     0x010  /* tokenizer size in bytes */
//#define LLM_STEPS     0x014  /* max generation steps */
//#define LLM_PROMPT    0x100  /* 256 bytes */
//#define LLM_OUTPUT    0x200  /* 3584 bytes */
//#define LLM_OUTPUT_MAX 3584

//#define CMD_LOAD_DONE 1
//#define CMD_LOAD_TOK  2
//#define CMD_GENERATE  3

/* ══════════════════════════════════════════════
   Transformer structures (from run.c)
   ══════════════════════════════════════════════ */

typedef struct {
    int dim;
    int hidden_dim;
    int n_layers;
    int n_heads;
    int n_kv_heads;
    int vocab_size;
    int seq_len;
} Config;

typedef struct {
    float *token_embedding_table;
    float *rms_att_weight;
    float *rms_ffn_weight;
    float *wq;
    float *wk;
    float *wv;
    float *wo;
    float *w1;
    float *w2;
    float *w3;
    float *rms_final_weight;
    float *wcls;
} TransformerWeights;

typedef struct {
    float *x;
    float *xb;
    float *xb2;
    float *hb;
    float *hb2;
    float *q;
    float *k;
    float *v;
    float *att;
    float *logits;
    float *key_cache;
    float *value_cache;
} RunState;

static Config config;
static TransformerWeights weights;
static RunState state;
static int model_ready = 0;

/* ══════════════════════════════════════════════
   Tokenizer structures
   ══════════════════════════════════════════════ */
typedef struct {
    char *str;
    int id;
} TokenIndex;

static char   **vocab = 0;
static float   *vocab_scores = 0;
static TokenIndex *sorted_vocab = 0;
static int      tok_vocab_size = 0;
static unsigned int max_token_length = 0;
static unsigned char byte_pieces[512];
static int tokenizer_ready = 0;

/* ══════════════════════════════════════════════
   Sampler state
   ══════════════════════════════════════════════ */
static unsigned long long rng_state = 12345678ULL;

static unsigned int random_u32(unsigned long long *s) {
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return (unsigned int)((*s * 0x2545F4914F6CDD1Dull) >> 32);
}

static float random_f32(unsigned long long *s) {
    return (float)(random_u32(s) >> 8) / 16777216.0f;
}

/* ══════════════════════════════════════════════
   Neural net blocks
   ══════════════════════════════════════════════ */

static void rmsnorm(float *o, float *x, float *weight, int size) {
    float ss = 0.0f;
    for (int j = 0; j < size; j++) ss += x[j] * x[j];
    ss /= size;
    ss += 1e-5f;
    ss = 1.0f / my_sqrtf(ss);
    for (int j = 0; j < size; j++) o[j] = weight[j] * (ss * x[j]);
}

static void softmax(float *x, int size) {
    float max_val = x[0];
    for (int i = 1; i < size; i++)
        if (x[i] > max_val) max_val = x[i];
    float sum = 0.0f;
    for (int i = 0; i < size; i++) {
        x[i] = my_expf(x[i] - max_val);
        sum += x[i];
    }
    for (int i = 0; i < size; i++) x[i] /= sum;
}

static void matmul(float *xout, float *x, float *w, int n, int d) {
    for (int i = 0; i < d; i++) {
        float val = 0.0f;
        for (int j = 0; j < n; j++)
            val += w[i * n + j] * x[j];
        xout[i] = val;
    }
}

static float *forward_token(int token, int pos) {
    Config *p = &config;
    TransformerWeights *w = &weights;
    RunState *s = &state;
    float *x = s->x;
    int dim = p->dim;
    int kv_dim = (p->dim * p->n_kv_heads) / p->n_heads;
    int kv_mul = p->n_heads / p->n_kv_heads;
    int hidden_dim = p->hidden_dim;
    int head_size = dim / p->n_heads;

    float *content_row = w->token_embedding_table + token * dim;
    my_memcpy(x, content_row, dim * sizeof(float));

    for (int l = 0; l < p->n_layers; l++) {
        rmsnorm(s->xb, x, w->rms_att_weight + l * dim, dim);

        int loff = l * p->seq_len * kv_dim;
        s->k = s->key_cache + loff + pos * kv_dim;
        s->v = s->value_cache + loff + pos * kv_dim;

        matmul(s->q, s->xb, w->wq + l * dim * dim, dim, dim);
        matmul(s->k, s->xb, w->wk + l * dim * kv_dim, dim, kv_dim);
        matmul(s->v, s->xb, w->wv + l * dim * kv_dim, dim, kv_dim);

        /* RoPE */
        for (int i = 0; i < dim; i += 2) {
            int head_dim = i % head_size;
            float freq = 1.0f / my_powf(10000.0f, head_dim / (float)head_size);
            float val = pos * freq;
            float fcr = my_cosf(val);
            float fci = my_sinf(val);
            int rotn = i < kv_dim ? 2 : 1;
            for (int v = 0; v < rotn; v++) {
                float *vec = v == 0 ? s->q : s->k;
                float v0 = vec[i];
                float v1 = vec[i + 1];
                vec[i]     = v0 * fcr - v1 * fci;
                vec[i + 1] = v0 * fci + v1 * fcr;
            }
        }

        /* multi-head attention */
        for (int h = 0; h < p->n_heads; h++) {
            float *q = s->q + h * head_size;
            float *att = s->att + h * p->seq_len;
            for (int t = 0; t <= pos; t++) {
                float *k = s->key_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                float score = 0.0f;
                for (int i = 0; i < head_size; i++) score += q[i] * k[i];
                score /= my_sqrtf((float)head_size);
                att[t] = score;
            }
            softmax(att, pos + 1);
            float *xb = s->xb + h * head_size;
            my_memset(xb, 0, head_size * sizeof(float));
            for (int t = 0; t <= pos; t++) {
                float *v = s->value_cache + loff + t * kv_dim + (h / kv_mul) * head_size;
                float a = att[t];
                for (int i = 0; i < head_size; i++) xb[i] += a * v[i];
            }
        }

        matmul(s->xb2, s->xb, w->wo + l * dim * dim, dim, dim);
        for (int i = 0; i < dim; i++) x[i] += s->xb2[i];

        rmsnorm(s->xb, x, w->rms_ffn_weight + l * dim, dim);
        matmul(s->hb, s->xb, w->w1 + l * dim * hidden_dim, dim, hidden_dim);
        matmul(s->hb2, s->xb, w->w3 + l * dim * hidden_dim, dim, hidden_dim);

        /* SwiGLU */
        for (int i = 0; i < hidden_dim; i++) {
            float val = s->hb[i];
            val *= (1.0f / (1.0f + my_expf(-val)));
            val *= s->hb2[i];
            s->hb[i] = val;
        }
        matmul(s->xb, s->hb, w->w2 + l * dim * hidden_dim, hidden_dim, dim);
        for (int i = 0; i < dim; i++) x[i] += s->xb[i];
    }

    rmsnorm(x, x, w->rms_final_weight, dim);
    matmul(s->logits, x, w->wcls, p->dim, p->vocab_size);
    return s->logits;
}

/* ══════════════════════════════════════════════
   Weight mapping (model is in model_data)
   ══════════════════════════════════════════════ */

static int init_model(uint32_t model_size) {
    //bump_reset();
    
    /* Place work memory after the model data, aligned to 16 bytes */
    uint32_t work_start = (model_size + 15) & ~15UL;
    if (work_start + WORK_MEM_SIZE > MODEL_DATA_MAX) {
        microkit_dbg_puts("LLM: not enough space for work mem\n");
        return -1;
    }
    work_mem = (uint8_t *)(model_data + work_start);
    work_mem_used = 0;

    float *data = (float *)model_data;
    Config *cfg = (Config *)data;

    config = *cfg;

    int shared_weights = config.vocab_size > 0 ? 1 : 0;
    if (config.vocab_size < 0) config.vocab_size = -config.vocab_size;

    /* Map weight pointers directly into model_data (read-only, no copy) */
    float *ptr = data + sizeof(Config) / sizeof(float);
    int head_size = config.dim / config.n_heads;
    unsigned long long nl = config.n_layers;

    weights.token_embedding_table = ptr; ptr += config.vocab_size * config.dim;
    weights.rms_att_weight = ptr; ptr += nl * config.dim;
    weights.wq = ptr; ptr += nl * config.dim * (config.n_heads * head_size);
    weights.wk = ptr; ptr += nl * config.dim * (config.n_kv_heads * head_size);
    weights.wv = ptr; ptr += nl * config.dim * (config.n_kv_heads * head_size);
    weights.wo = ptr; ptr += nl * (config.n_heads * head_size) * config.dim;
    weights.rms_ffn_weight = ptr; ptr += nl * config.dim;
    weights.w1 = ptr; ptr += nl * config.dim * config.hidden_dim;
    weights.w2 = ptr; ptr += nl * config.hidden_dim * config.dim;
    weights.w3 = ptr; ptr += nl * config.dim * config.hidden_dim;
    weights.rms_final_weight = ptr; ptr += config.dim;
    ptr += config.seq_len * head_size / 2; /* skip freq_cis_real */
    ptr += config.seq_len * head_size / 2; /* skip freq_cis_imag */
    weights.wcls = shared_weights ? weights.token_embedding_table : ptr;

    /* Allocate RunState from work_mem */
    int kv_dim = (config.dim * config.n_kv_heads) / config.n_heads;
    state.x = bump_calloc(config.dim, sizeof(float));
    state.xb = bump_calloc(config.dim, sizeof(float));
    state.xb2 = bump_calloc(config.dim, sizeof(float));
    state.hb = bump_calloc(config.hidden_dim, sizeof(float));
    state.hb2 = bump_calloc(config.hidden_dim, sizeof(float));
    state.q = bump_calloc(config.dim, sizeof(float));
    state.key_cache = bump_calloc(config.n_layers * config.seq_len * kv_dim, sizeof(float));
    state.value_cache = bump_calloc(config.n_layers * config.seq_len * kv_dim, sizeof(float));
    state.att = bump_calloc(config.n_heads * config.seq_len, sizeof(float));
    state.logits = bump_calloc(config.vocab_size, sizeof(float));

    if (!state.x || !state.logits) {
        microkit_dbg_puts("LLM: RunState alloc failed\n");
        return -1;
    }

    microkit_dbg_puts("LLM: model init OK, dim=");
    char buf[12]; int pos = 0; int v = config.dim;
    if (v == 0) buf[pos++] = '0';
    else { char tmp[12]; int t = 0; while (v > 0) { tmp[t++] = '0' + v % 10; v /= 10; } while (t > 0) buf[pos++] = tmp[--t]; }
    buf[pos] = '\0';
    microkit_dbg_puts(buf);
    microkit_dbg_puts(" layers=");
    pos = 0; v = config.n_layers;
    if (v == 0) buf[pos++] = '0';
    else { char tmp[12]; int t = 0; while (v > 0) { tmp[t++] = '0' + v % 10; v /= 10; } while (t > 0) buf[pos++] = tmp[--t]; }
    buf[pos] = '\0';
    microkit_dbg_puts(buf);
    microkit_dbg_puts(" vocab=");
    pos = 0; v = config.vocab_size;
    if (v == 0) buf[pos++] = '0';
    else { char tmp[12]; int t = 0; while (v > 0) { tmp[t++] = '0' + v % 10; v /= 10; } while (t > 0) buf[pos++] = tmp[--t]; }
    buf[pos] = '\0';
    microkit_dbg_puts(buf);
    microkit_dbg_puts(" seq_len=");
    pos = 0; v = config.seq_len;
    if (v == 0) buf[pos++] = '0';
    else { char tmp[12]; int t = 0; while (v > 0) { tmp[t++] = '0' + v % 10; v /= 10; } while (t > 0) buf[pos++] = tmp[--t]; }
    buf[pos] = '\0';
    microkit_dbg_puts(buf);
    microkit_dbg_puts(" work_mem=");
    pos = 0; v = work_mem_used;
    if (v == 0) buf[pos++] = '0';
    else { char tmp[12]; int t = 0; while (v > 0) { tmp[t++] = '0' + v % 10; v /= 10; } while (t > 0) buf[pos++] = tmp[--t]; }
    buf[pos] = '\0';
    microkit_dbg_puts(buf);
    microkit_dbg_puts(" bytes\n");

    return 0;
}

/* ══════════════════════════════════════════════
   Tokenizer init (from raw bytes in model_data
   after model weights, loaded by orchestrator)
   ══════════════════════════════════════════════ */

static int init_tokenizer(uint32_t tok_offset, uint32_t tok_size) {
    uint8_t *raw = (uint8_t *)(model_data + tok_offset);

    /* tokenizer.bin format:
       [4 bytes] max_token_length (int)
       then for each token (vocab_size times):
         [4 bytes] score (float)
         [4 bytes] len (int)
         [len bytes] string
    */
    tok_vocab_size = config.vocab_size;
    uint32_t off = 0;

    max_token_length = *(uint32_t *)(raw + off); off += 4;

    /* Allocate vocab and scores arrays */
    vocab = (char **)bump_alloc(tok_vocab_size * sizeof(char *));
    vocab_scores = (float *)bump_alloc(tok_vocab_size * sizeof(float));
    if (!vocab || !vocab_scores) {
        microkit_dbg_puts("LLM: tokenizer alloc failed\n");
        return -1;
    }

    /* Initialize byte_pieces for single-byte fallback tokens */
    for (int i = 0; i < 256; i++) {
        byte_pieces[i * 2] = (unsigned char)i;
        byte_pieces[i * 2 + 1] = '\0';
    }

    /* First pass: scan to calculate total string bytes needed */
    uint32_t scan_off = off;
    uint32_t total_str_bytes = 0;
    for (int i = 0; i < tok_vocab_size; i++) {
        scan_off += 4; /* skip score */
        int len;
        my_memcpy(&len, raw + scan_off, 4);
        scan_off += 4;
        total_str_bytes += len + 1; /* +1 for null terminator */
        scan_off += len;
    }

    /* Single bulk allocation for all token strings */
    char *str_pool = (char *)bump_alloc(total_str_bytes);
    if (!str_pool) {
        microkit_dbg_puts("LLM: tokenizer string pool alloc failed\n");
        return -1;
    }

    /* Second pass: copy scores and strings into allocated memory */
    uint32_t pool_off = 0;
    for (int i = 0; i < tok_vocab_size; i++) {
        float score;
        my_memcpy(&score, raw + off, 4); off += 4;
        vocab_scores[i] = score;

        int len;
        my_memcpy(&len, raw + off, 4); off += 4;

        vocab[i] = str_pool + pool_off;
        my_memcpy(vocab[i], raw + off, len); off += len;
        vocab[i][len] = '\0';
        pool_off += len + 1;
    }

    /* Build sorted vocab for BPE encoding */
    sorted_vocab = (TokenIndex *)bump_alloc(tok_vocab_size * sizeof(TokenIndex));
    if (!sorted_vocab) {
        microkit_dbg_puts("LLM: sorted_vocab alloc failed\n");
        return -1;
    }
    for (int i = 0; i < tok_vocab_size; i++) {
        sorted_vocab[i].str = vocab[i];
        sorted_vocab[i].id = i;
    }

    /* Simple insertion sort (good enough for 32k entries at init time) */
    for (int i = 1; i < tok_vocab_size; i++) {
        TokenIndex key = sorted_vocab[i];
        int j = i - 1;
        while (j >= 0 && my_strcmp(sorted_vocab[j].str, key.str) > 0) {
            sorted_vocab[j + 1] = sorted_vocab[j];
            j--;
        }
        sorted_vocab[j + 1] = key;
    }

    microkit_dbg_puts("LLM: tokenizer ready, vocab_size=");
    /* print vocab size */
    {
        char buf[12];
        int n = tok_vocab_size;
        int i = 0;
        if (n == 0) { buf[i++] = '0'; }
        else { while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; } }
        while (i > 0) { char c = buf[--i]; microkit_dbg_puts((char[]){c, '\0'}); }
    }
    microkit_dbg_puts("\n");

    return 0;
}


/* ══════════════════════════════════════════════
   Tokenizer: decode
   ══════════════════════════════════════════════ */

static char *decode_token(int prev_token, int token) {
    char *piece = vocab[token];
    if (prev_token == 1 && piece[0] == ' ') piece++;
    /* handle raw byte tokens like <0x01> */
    if (piece[0] == '<' && piece[1] == '0' && piece[2] == 'x') {
        unsigned char val = 0;
        for (int i = 3; piece[i] && piece[i] != '>'; i++) {
            val <<= 4;
            char c = piece[i];
            if (c >= '0' && c <= '9') val += c - '0';
            else if (c >= 'A' && c <= 'F') val += c - 'A' + 10;
            else if (c >= 'a' && c <= 'f') val += c - 'a' + 10;
        }
        piece = (char *)byte_pieces + val * 2;
    }
    return piece;
}

/* ══════════════════════════════════════════════
   Tokenizer: encode (simplified for prompts)
   ══════════════════════════════════════════════ */

static int str_lookup(char *str) {
    /* binary search on sorted_vocab */
    int lo = 0, hi = tok_vocab_size - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = my_strcmp(sorted_vocab[mid].str, str);
        if (cmp == 0) return sorted_vocab[mid].id;
        if (cmp < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static void encode_prompt(char *text, int *tokens, int *n_tokens) {
    *n_tokens = 0;
    tokens[(*n_tokens)++] = 1; /* BOS */

    /* add dummy prefix space */
    if (text[0] != '\0') {
        int dummy = str_lookup(" ");
        if (dummy != -1) tokens[(*n_tokens)++] = dummy;
    }

    /* encode each character/byte */
    int str_len = 0;
    char str_buf[64];
    for (int ci = 0; text[ci] != '\0'; ci++) {
        if ((text[ci] & 0xC0) != 0x80) str_len = 0;
        str_buf[str_len++] = text[ci];
        str_buf[str_len] = '\0';
        if ((text[ci + 1] & 0xC0) == 0x80 && str_len < 4) continue;

        int id = str_lookup(str_buf);
        if (id != -1) {
            tokens[(*n_tokens)++] = id;
        } else {
            for (int i = 0; i < str_len; i++)
                tokens[(*n_tokens)++] = (unsigned char)str_buf[i] + 3;
        }
        str_len = 0;
    }

    /* BPE merge loop */
    char merge_buf[128];
    while (1) {
        float best_score = -1e10f;
        int best_id = -1, best_idx = -1;
        for (int i = 0; i < *n_tokens - 1; i++) {
            /* build merged string */
            char *s1 = vocab[tokens[i]];
            char *s2 = vocab[tokens[i + 1]];
            int p = 0;
            while (*s1 && p < 120) merge_buf[p++] = *s1++;
            while (*s2 && p < 126) merge_buf[p++] = *s2++;
            merge_buf[p] = '\0';
            int id = str_lookup(merge_buf);
            if (id != -1 && vocab_scores[id] > best_score) {
                best_score = vocab_scores[id];
                best_id = id;
                best_idx = i;
            }
        }
        if (best_idx == -1) break;
        tokens[best_idx] = best_id;
        for (int i = best_idx + 1; i < *n_tokens - 1; i++)
            tokens[i] = tokens[i + 1];
        (*n_tokens)--;
    }
}

/* ══════════════════════════════════════════════
   Sampling (greedy argmax for simplicity)
   ══════════════════════════════════════════════ */
static int sample_argmax(float *probs, int n) {
    int best = 0;
    float best_p = probs[0];
    for (int i = 1; i < n; i++) {
        if (probs[i] > best_p) { best_p = probs[i]; best = i; }
    }
    return best;
}

static int sample_token(float *logits, float temperature) {
    if (temperature < 0.001f) {
        return sample_argmax(logits, config.vocab_size);
    }
    for (int i = 0; i < config.vocab_size; i++) logits[i] /= temperature;
    softmax(logits, config.vocab_size);
    float coin = random_f32(&rng_state);
    float cdf = 0.0f;
    for (int i = 0; i < config.vocab_size; i++) {
        cdf += logits[i];
        if (coin < cdf) return i;
    }
    return config.vocab_size - 1;
}

/* ══════════════════════════════════════════════
   Generate text
   ══════════════════════════════════════════════ */

/* ── Streaming generation state ─────────────────────── */
static int gen_active = 0;
static int gen_pos;
static int gen_token;
static int gen_steps;
static int gen_num_prompt_tokens;
static int *gen_prompt_tokens;
static int gen_prompt_alloc;

static void gen_step(void);

static void gen_emit_token(const char *piece) {
    /* Write token text to llm_io output area */
    char *out = (char *)(llm_io + LLM_OUTPUT);
    int i = 0;
    while (piece[i] && i < LLM_OUTPUT_MAX - 1) {
        out[i] = piece[i];
        i++;
    }
    out[i] = '\0';
    WR32(llm_io, LLM_STATUS, LLM_ST_TOKEN);
    microkit_notify(CH_ORCH);
}

static void gen_finish(void) {
    gen_active = 0;
    WR32(llm_io, LLM_STATUS, LLM_ST_DONE);
    microkit_notify(CH_ORCH);
}

static void gen_start(void) {
    char *prompt = (char *)(llm_io + LLM_PROMPT);
    gen_steps = (int)RD32(llm_io, LLM_MAX_STEPS);
    if (gen_steps <= 0 || gen_steps > (int)config.seq_len)
        gen_steps = config.seq_len;

    /* Encode prompt */
    if (!gen_prompt_tokens) {
        gen_prompt_alloc = config.seq_len;
        gen_prompt_tokens = (int *)bump_alloc(gen_prompt_alloc * sizeof(int));
        if (!gen_prompt_tokens) {
            microkit_dbg_puts("LLM: prompt alloc failed\n");
            WR32(llm_io, LLM_STATUS, LLM_ST_ERROR);
            microkit_notify(CH_ORCH);
            return;
        }
    }

    encode_prompt(prompt, gen_prompt_tokens, &gen_num_prompt_tokens);
    if (gen_num_prompt_tokens < 1) {
        gen_prompt_tokens[0] = 1; /* BOS */
        gen_num_prompt_tokens = 1;
    }

    gen_pos = 0;
    gen_token = gen_prompt_tokens[0];
    gen_active = 1;

    microkit_dbg_puts("LLM: generating (streaming)...\n");

    /* Generate first token immediately */
    gen_step();
}

static void gen_step(void) {
    if (!gen_active) return;

    /* Forward pass */
    float *logits = forward_token(gen_token, gen_pos);

    int next;
    if (gen_pos < gen_num_prompt_tokens - 1) {
        /* Still in prompt — force next prompt token */
        next = gen_prompt_tokens[gen_pos + 1];
    } else {
        /* Sample */
        next = sample_token(logits, 0.8f);
    }

    /* Check for end */
    if (next == 1 || next == 2 || gen_pos >= gen_steps - 1) {
        gen_finish();
        return;
    }

    /* Decode and emit if past prompt */
    if (gen_pos >= gen_num_prompt_tokens - 1) {
        char *piece = decode_token(gen_token, next);
        gen_emit_token(piece);
    } else {
        /* Still processing prompt, immediately do next step */
        gen_token = next;
        gen_pos++;
        gen_step();
        return;
    }

    gen_token = next;
    gen_pos++;
}

/* ══════════════════════════════════════════════
   Microkit entry points
   ══════════════════════════════════════════════ */

void init(void) {
    microkit_dbg_puts("LLM: server ready, waiting for model\n");
}

void notified(microkit_channel ch) {
    if (ch != CH_ORCH) return;

    uint32_t cmd = RD32(llm_io, LLM_CMD);

    /* Handle "next token" request during active generation */
    if (cmd == LLM_CMD_NEXT_TOK && gen_active) {
        gen_step();
        return;
    }

    switch (cmd) {
    case LLM_CMD_LOAD_DONE: {
        uint32_t msz = RD32(llm_io, LLM_MODELSZ);
        if (init_model(msz) == 0) {
            model_ready = 1;
            WR32(llm_io, LLM_STATUS, 0);
        } else {
            WR32(llm_io, LLM_STATUS, 1);
        }
        microkit_notify(CH_ORCH);
        break;
    }
    case LLM_CMD_LOAD_TOK: {
        uint32_t tok_off = RD32(llm_io, LLM_TOK_OFF);
        uint32_t tok_sz = RD32(llm_io, LLM_TOK_SZ);
        if (init_tokenizer(tok_off, tok_sz) == 0) {
            tokenizer_ready = 1;
            WR32(llm_io, LLM_STATUS, 0);
        } else {
            WR32(llm_io, LLM_STATUS, 1);
        }
        microkit_notify(CH_ORCH);
        break;
    }
    case LLM_CMD_GENERATE: {
        if (!model_ready || !tokenizer_ready) {
            microkit_dbg_puts("LLM: not ready\n");
            WR32(llm_io, LLM_STATUS, LLM_ST_ERROR);
            microkit_notify(CH_ORCH);
            break;
        }
        gen_start();
        break;
    }
    default:
        break;
    }
}
