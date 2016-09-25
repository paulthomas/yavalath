#include <math.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define TIMEOUT_USEC (60 * 1000000UL)
#define MEMORY_USAGE 0.8f
#define MCTS_C       2.0f

#ifdef __unix__
#include <sys/time.h>
#include <unistd.h>

static uint64_t
os_uepoch(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return UINT64_C(1000000) * tv.tv_sec + tv.tv_usec;
}

static size_t
os_physical_memory(void)
{
    size_t pages = sysconf(_SC_PHYS_PAGES);
    size_t page_size = sysconf(_SC_PAGE_SIZE);
    return pages * page_size;
}

static void
os_color(int color)
{
    if (color)
        printf("\x1b[%d;1m", 90 + color);
    else
        fputs("\x1b[0m", stdout);
}

#elif _WIN32
#include <windows.h>

static uint64_t
os_uepoch(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t tt = ft.dwHighDateTime;
    tt <<= 32;
    tt |= ft.dwLowDateTime;
    tt /=10;
    tt -= UINT64_C(11644473600000000);
    return tt;
}

static size_t
os_physical_memory(void)
{
    MEMORYSTATUSEX status = {.dwLength = sizeof(status)};
    GlobalMemoryStatusEx(&status);
    return status.ullTotalPhys;
}

static void
os_color(int color)
{
    WORD bits = color ? FOREGROUND_INTENSITY : 0;
    if (!color || color & 0x1)
        bits |= FOREGROUND_RED;
    if (!color || color & 0x2)
        bits |= FOREGROUND_GREEN;
    if (!color || color & 0x4)
        bits |= FOREGROUND_BLUE;
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), bits);
}
#endif

#include "tables.h"

static int
hex_to_bit(int q, int r)
{
    if (q < -4 || q > 4 || r < -4 || r > 4)
        return -1;
    else
        return store_map[q + 4][r + 4];
}

static uint64_t
rotl(const uint64_t x, int k)
{
    return (x << k) | (x >> (64 - k));
}

static uint64_t
xoroshiro128plus(uint64_t *s)
{
    const uint64_t s0 = s[0];
    uint64_t s1 = s[1];
    const uint64_t result = s0 + s1;
    s1 ^= s0;
    s[0] = rotl(s0, 55) ^ s1 ^ (s1 << 14); // a, b
    s[1] = rotl(s1, 36); // c
    return result;
}

static uint64_t
splitmix64(uint64_t *x)
{
    uint64_t z = (*x += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static void
display(uint64_t w, uint64_t b, uint64_t highlight, int color)
{
    for (int q = -4; q <= 4; q++) {
        printf("%c ", 'a' + q + 4);
        for (int s = 0; s < q + 4; s++)
            putchar(' ');
        for (int r = -4; r <= 4; r++) {
            int bit = hex_to_bit(q, r);
            if (bit == -1)
                fputs("  ", stdout);
            else {
                int h = highlight >> bit & 1;
                if (h)
                    os_color(color);
                if ((w >> bit) & 1)
                    putchar('o');
                else if ((b >> bit) & 1)
                    putchar('x');
                else
                    putchar('.');
                if (h)
                    os_color(0);
                putchar(' ');
            }
        }
        putchar('\n');
    }
}

static int
notation_to_hex(const char *s, int *q, int *r)
{
    if (s[0] < 'a' || s[1] > 'i')
        return 0;
    if (s[1] < '1' || s[1] > '9')
        return 0;
    *q = s[0] - 'a' - 4;
    *r = s[1] - "123455555"[*q + 4];
    return 1;
}

enum check_result {
    CHECK_RESULT_LOSS = -1,
    CHECK_RESULT_NOTHING = 0,
    CHECK_RESULT_WIN = 1,
};

static enum check_result
check(uint64_t c, int p, uint64_t *where)
{
    for (int i = 0; i < 12; i++) {
        uint64_t mask = pattern_win[p][i];
        if (mask && (c & mask) == mask) {
            *where = mask;
            return CHECK_RESULT_WIN;
        }
    }
    for (int i = 0; i < 9; i++) {
        uint64_t mask = pattern_lose[p][i];
        if (mask && (c & mask) == mask) {
            *where = mask;
            return CHECK_RESULT_LOSS;
        }
    }
    *where = 0;
    return CHECK_RESULT_NOTHING;
}

static uint64_t
state_hash(uint64_t a, uint64_t b)
{
    uint64_t rng[2];
    rng[0] = splitmix64(&a);
    rng[1] = splitmix64(&b);
    return xoroshiro128plus(rng);
}

#define MCTS_NULL      ((uint32_t)-1)
#define MCTS_LEAF      ((uint32_t)-2)
#define MCTS_UCB1_FLAG (1 << 0)
struct mcts {
    uint64_t rng[2];              // random number state
    uint32_t root;                // root node index
    uint32_t free;                // index of head of free list
    uint32_t nodes_avail;         // total nodes available
    uint32_t nodes_allocated;     // total number allocated
    int root_turn;                // whose turn it is at root node
    struct mcts_node {
        uint32_t head;            // head of hash list for this slot
        uint32_t chain;           // next item in hash table list
        uint64_t state[2];        // the game state at this node
        uint32_t total_playouts;  // number of playouts through this node
        uint32_t wins[61];        // win counter for each move
        uint32_t playouts[61];    // number of playouts for this play
        uint32_t next[61];        // next node when taking this play
        uint16_t refcount;        // number of nodes referencing this node
        uint8_t  flags;
    } nodes[];
};

static uint32_t
mcts_find(struct mcts *m, uint32_t list_head, const uint64_t state[2])
{
    while (list_head != MCTS_NULL) {
        struct mcts_node *n = m->nodes + list_head;
        if (n->state[0] == state[0] && n->state[1] == state[1])
            return list_head;
        list_head = n->chain;
    }
    return MCTS_NULL;
}

static uint32_t
mcts_alloc(struct mcts *m, const uint64_t state[2])
{
    uint64_t hash = state_hash(state[0], state[1]);
    uint32_t *head = &m->nodes[hash % m->nodes_avail].head;
    uint32_t nodei = mcts_find(m, *head, state);
    if (nodei != MCTS_NULL) {
        /* Node already exists, return it. */
        assert(m->nodes[nodei].refcount > 0);
        m->nodes[nodei].refcount++;
        return nodei;
    } if (m->free != MCTS_NULL) {
        /* Allocate a node. */
        nodei = m->free;
        m->free = m->nodes[m->free].chain;
        m->nodes_allocated++;
    } else {
        return MCTS_NULL;
    }

    /* Initiaize the node. */
    struct mcts_node *n = m->nodes + nodei;
    n->state[0] = state[0];
    n->state[1] = state[1];
    n->refcount = 1;
    n->total_playouts = 0;
    n->flags = 0;
    n->chain = *head;
    *head = nodei;
    for (int i = 0; i < 61; i++) {
        n->wins[i] = 0;
        n->playouts[i] = 0;
        n->next[i] = MCTS_NULL;
    }
    return nodei;
}

static void
mcts_free(struct mcts *m, uint32_t node)
{
    if (node != MCTS_NULL && node != MCTS_LEAF) {
        struct mcts_node *n = m->nodes + node;
        assert(n->refcount);
        if (--n->refcount == 0) {
            m->nodes_allocated--;
            for (int i = 0; i < 61; i++)
                mcts_free(m, n->next[i]);
            uint64_t hash = state_hash(n->state[0], n->state[1]);
            uint32_t parent = m->nodes[hash % m->nodes_avail].head;
            if (parent == node) {
                m->nodes[hash % m->nodes_avail].head = n->chain;
            } else {
                while (m->nodes[parent].chain != node)
                    parent = m->nodes[parent].chain;
                m->nodes[parent].chain = n->chain;
            }
            n->chain = m->free;
            m->free = node;
        }
    }
}

static struct mcts *
mcts_init(void *buf, size_t bufsize, uint64_t state[2], int turn)
{
    struct mcts *m = buf;
    m->nodes_avail = (bufsize  - sizeof(*m)) / sizeof(m->nodes[0]);
    m->nodes_allocated = 0;
    uint64_t seed = os_uepoch();
    m->rng[0] = splitmix64(&seed);
    m->rng[1] = splitmix64(&seed);
    m->free = 0;
    for (uint32_t i = 0; i < m->nodes_avail; i++) {
        m->nodes[i].head = MCTS_NULL;
        m->nodes[i].chain = i + 1;
    }
    m->nodes[m->nodes_avail - 1].chain = MCTS_NULL;
    m->root = mcts_alloc(m, state);
    m->root_turn = turn;
    return m->root == MCTS_NULL ? NULL : m;
}

static void
mcts_advance(struct mcts *m, int tile)
{
    uint32_t old_root = m->root;
    struct mcts_node *root = m->nodes + old_root;
    if (((root->state[0] | root->state[1]) >> tile) & 1)
        fprintf(stderr, "error: invalid move, %d\n", tile);
    uint64_t state[2] = {root->state[0], root->state[1]};
    state[m->root_turn] |= UINT64_C(1) << tile;
    m->root_turn = !m->root_turn;
    m->root = root->next[tile];
    root->next[tile] = MCTS_NULL;  // prevents free
    mcts_free(m, old_root);
    if (m->root == MCTS_NULL || m->root == MCTS_LEAF) {
        /* never explored this branch, allocate it */
        m->root = mcts_alloc(m, state);
    }
}

static int
random_play(uint64_t *rng, uint64_t taken)
{
    for (;;) {
        uint64_t random = xoroshiro128plus(rng);
        for (int i = 0; i < 10; i++) {
            int candidate = (random >> (6 * i)) & 0x3f;
            if (candidate < 61 && !((taken >> candidate) & 1))
                return candidate;
        }
    }
}

static int
mcts_playout(struct mcts *m, uint32_t node, int turn)
{
    struct mcts_node *n = m->nodes + node;
    uint64_t copy[2] = {n->state[0], n->state[1]};
    int play;
    float best = -1.0f;
    uint64_t taken = n->state[0] | n->state[1];
    if (n->flags & MCTS_UCB1_FLAG) {
        play = -1;
        float numerator = MCTS_C * logf(n->total_playouts);
        for (int i = 0; i < 61; i++) {
            if (!((taken >> i) & 1)) {
                float mean = n->wins[i] / (float)n->playouts[i];
                float x = mean + sqrtf(numerator / n->playouts[i]);
                if (x > best) {
                    best = x;
                    play = i;
                }
            }
        }
    } else {
        play = random_play(m->rng, taken);
    }
    assert(play >= 0 && play <= 61);
    uint64_t play_bit = UINT64_C(1) << play;
    copy[turn] |= play_bit;
    uint64_t dummy;
    switch (check(copy[turn], play, &dummy)) {
        case CHECK_RESULT_WIN:
            n->playouts[play]++;
            n->total_playouts++;
            n->wins[play]++;
            n->next[play] = MCTS_LEAF;
            return turn;
        case CHECK_RESULT_LOSS:
            n->playouts[play]++;
            n->total_playouts++;
            n->next[play] = MCTS_LEAF;
            return !turn;
        case CHECK_RESULT_NOTHING:
            break;
    }
    if (n->next[play] == MCTS_NULL) {
        n->next[play] = mcts_alloc(m, copy);
        if (n->next[play] == MCTS_NULL)
            return -1; // out of memory
        int full = 1;
        for (int i = 0; i < 61; i++) {
            if (!((taken >> i) & 1) && n->next[i] == MCTS_NULL) {
                full = 0;
                break;
            }
        }
        if (full)
            n->flags |= MCTS_UCB1_FLAG;
    }
    int winner = mcts_playout(m, n->next[play], !turn);
    if (winner != -1) {
        n->playouts[play]++;
        n->total_playouts++;
    }
    if (winner == turn)
        n->wins[play]++;
    return winner;
}

static int
mcts_choose(struct mcts *m, uint64_t timeout_usec)
{
    uint64_t stop = os_uepoch() + timeout_usec;
    int oom_notice = 0;
    do {
        int iterations = 64 * 1024;
        int oom_count = 0;
        for (int i = 0; i < iterations; i++) {
            int r = mcts_playout(m, m->root, m->root_turn);
            if (r < 0) {
                oom_count++;
            }
        }
        if (oom_count == iterations) {
            printf("note: no more progress made, bailing out\n");
            break;
        }
        if (oom_count && !oom_notice) {
            fprintf(stderr, "note: out of memory, examining old paths\n");
            oom_notice = 1;
        }
    } while (os_uepoch() < stop);

    int best = -1;
    float best_ratio = -1.0f;
    struct mcts_node *n = m->nodes + m->root;
    uint64_t available = n->state[0] | n->state[1];
    for (int i = 0; i < 61; i++) {
        if (!((available >> i) & 1) && n->playouts[i]) {
            float ratio = n->wins[i] / (float)n->playouts[i];
            if (ratio > best_ratio) {
                best = i;
                best_ratio = ratio;
            }
        }
    }
    assert(best != -1);
    return best;
}

int
main(void)
{
    uint64_t board[2] = {0, 0};
    unsigned turn = 0;
    enum player_type {
        PLAYER_HUMAN,
        PLAYER_AI
    } player_type[2] = {
        PLAYER_HUMAN, PLAYER_AI
    };

    size_t physical_memory = os_physical_memory();
    size_t size = physical_memory;
    void *buf;
    do {
        size *= MEMORY_USAGE;
        buf = malloc(size);
    } while (!buf);
    struct mcts *mcts = mcts_init(buf, size, board, turn);
    printf("%zu MB physical memory found, "
           "AI will use %zu MB (%" PRIu32 " nodes)\n",
           physical_memory / 1024 / 1024,
           size / 1024 / 1024,
           mcts->nodes_avail);

    uint64_t last_play = 0;
    for (;;) {
        display(board[0], board[1], last_play, 3);
        fflush(stdout);
        char line[64];
        int bit = -1;
        switch (player_type[turn]) {
            case PLAYER_HUMAN:
                for (;;) {
                    fputs("\n> ", stdout);
                    fflush(stdout);
                    if (!fgets(line, sizeof(line), stdin))
                        return -1; // EOF
                    int q, r;
                    if (notation_to_hex(line, &q, &r)) {
                        bit = hex_to_bit(q, r);
                        if (bit == -1) {
                            printf("Invalid move (out of bounds)\n");
                        } else if (((board[0] >> bit) & 1) ||
                                   ((board[1] >> bit) & 1)) {
                            printf("Invalid move (tile not free)\n");
                        } else {
                            break;
                        }
                    }
                }
                break;
            case PLAYER_AI:
                puts("AI is thinking ...");
                fflush(stdout);
                bit = mcts_choose(mcts, TIMEOUT_USEC);
                break;
        }
        last_play = UINT64_C(1) << bit;
        mcts_advance(mcts, bit);
        uint64_t where;
        board[turn] |= UINT64_C(1) << bit;
        switch (check(board[turn], bit, &where)) {
            case CHECK_RESULT_NOTHING: {
                turn = !turn;
            } break;
            case CHECK_RESULT_LOSS: {
                display(board[0], board[1], where, 1);
                printf("player %c loses!\n", "ox"[turn]);
                goto done;
            } break;
            case CHECK_RESULT_WIN: {
                display(board[0], board[1], where, 4);
                printf("player %c wins!\n", "ox"[turn]);
                goto done;
            } break;
        }
    }

done:
    free(mcts);
    #ifdef _WIN32
    getchar(); // leave window open on Windows
    #endif
    return 0;
}
