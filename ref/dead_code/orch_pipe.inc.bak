/* Pipe infrastructure
 * Auto-extracted from orchestrator.c
 * Lines 310-351
 */

/* ── Pipe infrastructure ──────────────────────────────── */
#define MAX_PIPES 8
#define PIPE_BUF_SIZE 4096
#define MAX_PIPE_FDS 8
#define PIPE_FD_BASE 100  /* pipe fds start at 100 to avoid fs fd clash */

typedef struct {
    int active;
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    int closed_write;
    int closed_read;
} pipe_t;

static pipe_t pipes[MAX_PIPES];

typedef struct {
    int active;
    int pipe_idx;
    int is_write;  /* 0=read end, 1=write end */
} pipe_fd_t;

/* Per-sandbox pipe fd table */
static pipe_fd_t pipe_fds[NUM_SANDBOXES][MAX_PIPE_FDS];

/* Per-session umask */
static uint32_t current_umask = 022;


static int      current_is_root = 0;
static char     current_username[32] = {0};
static char     login_username[32] = {0};

enum {
    RUNNING,
    EXEC_RUNNING,   /* sandbox is executing */
    GEN_WAITING,    /* LLM generating tokens */
    LLM_INIT_MODEL, /* waiting for model init reply */
    LLM_INIT_TOK,   /* waiting for tokenizer init reply */
};
static int orch_state = RUNNING;
