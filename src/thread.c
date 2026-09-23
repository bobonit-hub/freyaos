/*
 * Freya - priority threads.
 *
 * The shell is a thread.  While a program runs, that same thread is the
 * program: app_main() keeps the shell stack, which is the large one, and
 * threads the program creates get a small stack of their own.  The highest
 * priority that is ready runs.  Equals take turns, ten milliseconds each.
 * Idle runs when nobody else can.
 *
 * Switching is PendSV's job, at the bottom of the priority order, so the
 * frame it sees is the thread's and not a handler's.  The same entry still
 * aborts a run: a stop is decided before a switch, and the stacked PC is
 * sent through the trampoline exactly as it was when the only thread was
 * the program.
 *
 * A thread that never yields still loses the CPU to a higher priority, and
 * the console is polled from that same PendSV, so `threads` and `stop`
 * can be typed at a program that has not blocked.  What is typed is read
 * only while the program itself is not waiting on the console.
 */
#include "freya.h"

#define SLICE_MS     10
#define STACK_MAGIC  0xDEADBEEFUL
#define IDLE_STACK   256
#define SLOT_NONE    0xFF
#define THREAD_CAP   6          /* shell, idle, and up to four workers */

#define TH_FREE      0
#define TH_READY     1
#define TH_RUNNING   2
#define TH_SLEEP     3
#define TH_DEAD      4

#define TF_SYSTEM    0x01
#define TF_IN_RUN    0x02

#define SHELL_ID     0
#define IDLE_ID      1

typedef struct {
    char     name[FREYA_THREAD_NAME_MAX + 1];
    int8_t   priority;
    uint8_t  state;
    uint8_t  flags;
    uint8_t  slot;
    uint32_t wake;
    uint32_t sp;
    uint32_t exc;
    uint32_t *stack;
} thread_t;

uint32_t thread_exc_save;
uint32_t thread_exc_restore;

static thread_t  s_thr[THREAD_CAP];
static uint8_t   s_slot_used[4];
static uint32_t  s_idle_stack[IDLE_STACK / 4] __attribute__((aligned(8)));
static thread_t *s_current;
static int       s_need;
static int       s_poll;
static uint8_t   s_slice = SLICE_MS;

/* Linker-provided pool.  The size and the count are the symbol values. */
extern uint8_t __worker_stacks[];
extern char    __worker_stack_size[];
extern char    __worker_count[];

static uint32_t worker_bytes(void)
{
    return (uint32_t)(uintptr_t)__worker_stack_size;
}

static uint32_t worker_count(void)
{
    uint32_t n = (uint32_t)(uintptr_t)__worker_count;
    if (n > 4) n = 4;
    return n;
}

static uint32_t *worker_base(uint32_t slot)
{
    return (uint32_t *)(__worker_stacks + slot * worker_bytes());
}

#ifdef FREYA_HOST
#define crit_save()       0u
#define crit_restore(pm)  ((void)(pm))
#define thread_wfi()      ((void)0)
#else
#define crit_save()       irq_save()
#define crit_restore(pm)  irq_restore(pm)
#define thread_wfi()      __wfi()
#endif

static void pend_soft(void)
{
#ifndef FREYA_HOST
    SCB->ICSR = 1UL << 28;
#endif
}

/* A fabricated exception frame: PendSV restores r4-r11 and exception-
 * returns into fn, and a return from fn lands in thread_exit. */
static uint32_t *make_frame(uint32_t *base, uint32_t bytes,
                            freya_thread_fn fn, void *arg)
{
    uint32_t *sp = base + (bytes / 4);
    int i;

    sp -= 8;
    sp[0] = (uint32_t)(uintptr_t)arg;                         /* r0  */
    sp[1] = 0;
    sp[2] = 0;
    sp[3] = 0;
    sp[4] = 0;                                                /* r12 */
    sp[5] = ((uint32_t)(uintptr_t)thread_exit) | 1u;          /* lr  */
    sp[6] = (uint32_t)(uintptr_t)fn & ~1u;                    /* pc  */
    sp[7] = 0x01000000u;                                      /* Thumb */
    sp -= 8;
    for (i = 0; i < 8; i++) sp[i] = 0;                        /* r4-r11 */
    base[0] = STACK_MAGIC;
    return sp;
}

static int runnable(const thread_t *t)
{
    if (t->state == TH_READY) return 1;
    return t == s_current && t->state == TH_RUNNING;
}

static thread_t *pick(void)
{
    thread_t *best = NULL;

    for (int i = 0; i < THREAD_CAP; i++) {
        thread_t *t = &s_thr[i];
        if (!runnable(t)) continue;
        if (!best || t->priority > best->priority)
            best = t;
        else if (t->priority == best->priority && best == s_current &&
                 t != s_current)
            best = t;                 /* same priority: the one waiting */
    }
    return best;
}

static void reap(thread_t *t)
{
    if (t->state != TH_DEAD || t->slot == SLOT_NONE) return;
    if (t->slot < 4) s_slot_used[t->slot] = 0;
    t->state = TH_FREE;
    t->name[0] = '\0';
    t->stack = NULL;
    t->slot = SLOT_NONE;
}

static int name_ok(const char *name)
{
    size_t n;

    if (!name) return 0;
    n = strlen(name);
    if (n == 0 || n > FREYA_THREAD_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)name[i] <= ' ') return 0;
    return 1;
}

static thread_t *find_name(const char *name)
{
    for (int i = 0; i < THREAD_CAP; i++) {
        if (s_thr[i].state == TH_FREE) continue;
        if (strcmp(s_thr[i].name, name) == 0) return &s_thr[i];
    }
    return NULL;
}

static const char *state_str(uint8_t state)
{
    switch (state) {
    case TH_READY:   return "ready";
    case TH_RUNNING: return "running";
    case TH_SLEEP:   return "sleep";
    default:         return "dead";
    }
}

static void idle_entry(void *arg)
{
    (void)arg;
    for (;;) {
        thread_wfi();
        thread_yield();
    }
}

#ifndef FREYA_HOST
static void pend_now(void)
{
    s_need = 1;
    SCB->ICSR = 1UL << 28;
    __dsb();
    __isb();
}
#else
static void pend_now(void)
{
    s_need = 1;
    if (s_current && thread_preempt_kind() == 2)
        (void)thread_switch(s_current->sp);
}
#endif

void thread_init(void)
{
    uint32_t *sp;

    memset(s_thr, 0, sizeof s_thr);
    memset(s_slot_used, 0, sizeof s_slot_used);

    strncpy(s_thr[SHELL_ID].name, "shell", FREYA_THREAD_NAME_MAX);
    s_thr[SHELL_ID].priority = 0;
    s_thr[SHELL_ID].state = TH_RUNNING;
    s_thr[SHELL_ID].flags = TF_SYSTEM;
    s_thr[SHELL_ID].slot = SLOT_NONE;
    s_thr[SHELL_ID].exc = 0xFFFFFFFDu;

    sp = make_frame(s_idle_stack, sizeof s_idle_stack, idle_entry, NULL);
    strncpy(s_thr[IDLE_ID].name, "idle", FREYA_THREAD_NAME_MAX);
    s_thr[IDLE_ID].priority = -1;
    s_thr[IDLE_ID].state = TH_READY;
    s_thr[IDLE_ID].flags = TF_SYSTEM;
    s_thr[IDLE_ID].slot = SLOT_NONE;
    s_thr[IDLE_ID].sp = (uint32_t)(uintptr_t)sp;
    s_thr[IDLE_ID].exc = 0xFFFFFFFDu;
    s_thr[IDLE_ID].stack = s_idle_stack;

    s_current = &s_thr[SHELL_ID];
    s_slice = SLICE_MS;
}

int thread_self(void)
{
    if (!s_current) return -1;
    return (int)(s_current - s_thr);
}

int thread_is_main(void)
{
    return s_current == &s_thr[SHELL_ID];
}

int thread_create(const char *name, int priority, freya_thread_fn fn, void *arg)
{
    thread_t *t = NULL;
    uint32_t pm, slot, bytes;
    int id;

    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (!g_app.running || !s_current) return FREYA_ERR_HANDLER;
    if (!fn || !name_ok(name)) return FREYA_ERR_ARG;
    if (priority < FREYA_PRIO_MIN || priority > FREYA_PRIO_MAX)
        return FREYA_ERR_ARG;
    bytes = worker_bytes();
    if (bytes < 128 || (bytes & 7u)) return FREYA_ERR_BUSY;

    pm = crit_save();
    if (find_name(name)) {
        crit_restore(pm);
        return FREYA_ERR_BUSY;
    }
    slot = worker_count();
    for (uint32_t i = 0; i < slot; i++) {
        if (!s_slot_used[i]) { slot = i; break; }
    }
    if (slot >= worker_count() || s_slot_used[slot]) {
        crit_restore(pm);
        return FREYA_ERR_BUSY;
    }
    s_slot_used[slot] = 1;
    id = (int)slot + 2;
    t = &s_thr[id];
    memset(t, 0, sizeof *t);
    strncpy(t->name, name, FREYA_THREAD_NAME_MAX);
    t->priority = (int8_t)priority;
    t->slot = (uint8_t)slot;
    t->stack = worker_base(slot);
    t->sp = (uint32_t)(uintptr_t)make_frame(t->stack, bytes, fn, arg);
    t->exc = 0xFFFFFFFDu;
    t->state = TH_READY;
    crit_restore(pm);

    if (s_current && priority > s_current->priority)
        pend_now();
    return id;
}

void thread_exit(void)
{
    if (!s_current || (s_current->flags & TF_SYSTEM)) {
        /* Idle and the shell are not threads a program can end.  The
         * service call turns an exit of the main thread into an exit
         * of the run before it gets here. */
        for (;;) thread_wfi();
    }
    s_current->state = TH_DEAD;
    pend_now();
    for (;;) thread_wfi();
}

void thread_yield(void)
{
    thread_t *next;

    if (!s_current || app_in_handler()) return;
    next = pick();
    if (!next || next == s_current) return;
    pend_now();
}

int thread_sleep(uint32_t ms)
{
    uint32_t pm;

    if (!s_current) return 0;
    if (app_in_handler()) return FREYA_ERR_HANDLER;
    if (ms == 0) {
        thread_yield();
        return (g_app.running && app_should_stop()) ? -1 : 0;
    }

    pm = crit_save();
    if (g_app.running && app_should_stop()) {
        crit_restore(pm);
        return -1;
    }
    s_current->wake = sys_ticks() + ms;
    s_current->state = TH_SLEEP;
    s_need = 1;
    crit_restore(pm);
    pend_now();
    return (g_app.running && app_should_stop()) ? -1 : 0;
}

void thread_tick(void)
{
    int want = 0;
    uint32_t now;

    if (!s_current) return;
    now = sys_ticks();

    for (int i = 0; i < THREAD_CAP; i++) {
        thread_t *t = &s_thr[i];
        if (t->state == TH_SLEEP && (int32_t)(now - t->wake) >= 0) {
            t->state = TH_READY;
            want = 1;
        }
    }

    if (s_current->state == TH_RUNNING) {
        for (int i = 0; i < THREAD_CAP; i++) {
            thread_t *t = &s_thr[i];
            if (t->state != TH_READY) continue;
            if (t->priority > s_current->priority) want = 1;
        }
        if (s_slice) {
            s_slice--;
        } else {
            s_slice = SLICE_MS;
            for (int i = 0; i < THREAD_CAP; i++) {
                thread_t *t = &s_thr[i];
                if (t->state == TH_READY && t->priority == s_current->priority)
                    want = 1;
            }
        }
    }

    if ((now % SLICE_MS) == 0) s_poll = 1;
    if (want) s_need = 1;
    if (s_need || s_poll) pend_soft();
}

int thread_preempt_kind(void)
{
    if (!s_current || app_switch_blocked()) return 0;
    if (g_app.running && app_should_stop()) return 1;
    if (s_poll) {
        s_poll = 0;
        shell_poll_runtime();
    }
    if (g_app.running && app_should_stop()) return 1;
    if (s_current->state == TH_DEAD) return 2;
    if (s_need && pick() != s_current) return 2;
    s_need = 0;
    return 0;
}

uint32_t thread_switch(uint32_t saved_sp)
{
    thread_t *prev = s_current;
    thread_t *next;

    prev->sp = saved_sp;
    prev->exc = thread_exc_save;
    if (prev->stack && prev->stack[0] != STACK_MAGIC) {
        prev->state = TH_DEAD;
        if (g_app.running && !(prev->flags & TF_SYSTEM))
            app_request_stop();
    } else if (prev->state == TH_RUNNING) {
        prev->state = TH_READY;
    }

    next = pick();
    if (!next) next = &s_thr[IDLE_ID];
    if (next->state == TH_FREE) next = &s_thr[IDLE_ID];
    next->state = TH_RUNNING;
    s_current = next;
    if (prev != next && prev->state == TH_DEAD)
        reap(prev);

    s_need = 0;
    s_slice = SLICE_MS;
    thread_exc_restore = next->exc ? next->exc : 0xFFFFFFFDu;
    return next->sp;
}

void thread_list(void)
{
    struct {
        char    name[FREYA_THREAD_NAME_MAX + 1];
        int8_t  priority;
        uint8_t state;
        uint8_t id;
    } row[THREAD_CAP];
    int n = 0;
    uint32_t pm = crit_save();

    for (int i = 0; i < THREAD_CAP && n < THREAD_CAP; i++) {
        if (s_thr[i].state == TH_FREE) continue;
        strncpy(row[n].name, s_thr[i].name, FREYA_THREAD_NAME_MAX);
        row[n].name[FREYA_THREAD_NAME_MAX] = '\0';
        row[n].priority = s_thr[i].priority;
        row[n].state = s_thr[i].state;
        row[n].id = (uint8_t)i;
        n++;
    }
    crit_restore(pm);

    kprintf("  id  pri  state    name\r\n");
    for (int i = 0; i < n; i++)
        kprintf("  %2u  %3d  %-7s  %s\r\n", row[i].id, row[i].priority,
                state_str(row[i].state), row[i].name);
}

int thread_stop_name(const char *name)
{
    thread_t *t;

    if (!name || !name[0]) return FREYA_ERR_ARG;
    t = find_name(name);
    if (!t) return FREYA_ERR_ARG;
    if (t == &s_thr[IDLE_ID]) return FREYA_ERR_BUSY;
    if (t == &s_thr[SHELL_ID]) {
        if ((t->flags & TF_IN_RUN) && g_app.running) {
            app_request_stop();
            return 0;
        }
        return FREYA_ERR_BUSY;
    }

    if (t != s_current) {
        t->state = TH_DEAD;
        reap(t);
        return 0;
    }
    t->state = TH_DEAD;
    pend_now();
    return 0;
}

void thread_run_begin(const char *name)
{
    thread_t *sh = &s_thr[SHELL_ID];

    if (!name || !name[0]) name = "main";
    strncpy(sh->name, name, FREYA_THREAD_NAME_MAX);
    sh->name[FREYA_THREAD_NAME_MAX] = '\0';
    /* A space would make `stop <name>` see two words. */
    for (char *p = sh->name; *p; p++)
        if ((unsigned char)*p <= ' ') { *p = '\0'; break; }
    if (!sh->name[0]) strncpy(sh->name, "main", FREYA_THREAD_NAME_MAX);
    sh->priority = FREYA_PRIO_NORMAL;
    sh->flags |= TF_IN_RUN;
}

void thread_run_end(void)
{
    uint32_t pm = crit_save();

    for (int i = 0; i < THREAD_CAP; i++) {
        thread_t *t = &s_thr[i];
        if (t->flags & TF_SYSTEM) continue;
        if (t->state == TH_FREE) continue;
        t->state = TH_DEAD;
        reap(t);
    }
    s_thr[SHELL_ID].priority = 0;
    s_thr[SHELL_ID].flags = TF_SYSTEM;
    s_thr[SHELL_ID].state = TH_RUNNING;
    strncpy(s_thr[SHELL_ID].name, "shell", FREYA_THREAD_NAME_MAX);
    s_current = &s_thr[SHELL_ID];
    s_need = 0;
    crit_restore(pm);
}

void thread_after_abort(void)
{
    s_current = &s_thr[SHELL_ID];
    s_thr[SHELL_ID].state = TH_RUNNING;
    s_need = 0;
}

void thread_reconsider(void)
{
    if (!s_current) return;
    if (s_need || s_poll || (g_app.running && app_should_stop()))
        pend_soft();
}
