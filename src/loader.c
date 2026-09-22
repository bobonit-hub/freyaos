/*
 * Freya - user program loader.
 *
 * A program is a raw image that begins with a freya_app_header_t and is
 * linked for FREYA_APP_LOAD_ADDR.  'load' copies it into the program
 * region, zeroes its .bss and hands it the service table; 'run' calls the
 * entry point on the main stack.  Only one program exists at a time.
 *
 * Three things can end a run:
 *   - app_main() returns,
 *   - the program calls api->exit(),
 *   - the program is stopped (Ctrl-C from the console ISR, or a fault),
 * and all three unwind through the same freya_longjmp() back into run(),
 * which settles the exit status and keeps it for the shell to report.
 */
#include "freya.h"
#include "fat.h"

app_state_t g_app;

static freya_jmpbuf   s_return_ctx;
volatile int          g_app_stop_reason;
static volatile int   s_stop_requested;
static int            s_exit_code;

/* Heap blocks handed to the program, reclaimed when the run ends. */
#define APP_MAX_ALLOCS  32
static void *s_app_allocs[APP_MAX_ALLOCS];

/* ------------------------------------------------------- stop handling */
void app_abort_trampoline(void) __attribute__((noreturn));
void app_fault_trampoline(void) __attribute__((noreturn));

static volatile int s_guard;            /* >0: an abort must be deferred */

#define PENDSV_SET  (1UL << 28)

void app_request_stop(void)
{
    s_stop_requested = 1;
    if (!g_app_stop_reason) g_app_stop_reason = FREYA_STOP_CTRLC;
    if (g_app.running) SCB->ICSR = PENDSV_SET;
}

/*
 * Card transfers run inside a guard: killing a program halfway through an
 * SPI block write would leave the card - and the filesystem - in an
 * undefined state, so the abort waits for the transfer to finish.
 */
void app_guard_enter(void)
{
    s_guard++;
}

void app_guard_leave(void)
{
    if (--s_guard <= 0) {
        s_guard = 0;
        if (s_stop_requested && g_app.running) SCB->ICSR = PENDSV_SET;
    }
}

/*
 * Entered from the PendSV shim with the interrupted thread's exception
 * frame.  Redirecting the stacked PC makes the thread resume in the abort
 * trampoline, which unwinds into the shell.
 */
void app_pendsv_handler(uint32_t *frame)
{
    if (!g_app.running || !s_stop_requested) return;
    if (s_guard) return;                      /* retried when the guard lifts */
    if ((frame[7] & 0x1FFUL) != 0) return;    /* not a thread mode frame     */

    /* A fault unwinds through the trampoline that dumps SRAM; a Ctrl-C
     * through the plain one.  Which it is has already been decided by
     * whoever asked for the stop. */
    frame[6] = (uint32_t)(uintptr_t)(g_app_stop_reason >= FREYA_STOP_HARDFAULT
                                     ? app_fault_trampoline
                                     : app_abort_trampoline);
    /* Bit 24 keeps the core in Thumb state, the IT bits are cleared so the
     * first instructions cannot be skipped, bit 9 (stack realignment) and
     * the rest of the frame are left alone. */
    frame[7] = (frame[7] & ~0x0600FC00UL) | (1UL << 24);
}

int app_should_stop(void)
{
    return s_stop_requested;
}

const char *app_stop_reason_str(int reason)
{
    /* Indexed by FREYA_STOP_*, in the order the ABI declares them. */
    static const char *const s[] = {
        "returned",
        "exited",
        "stopped by Ctrl-C",
        "killed by hard fault",
        "killed by memory fault",
        "killed by bus fault",
        "killed by usage fault"
    };

    if (reason < 0 || reason >= (int)ARRAY_SIZE(s)) return s[0];
    return s[reason];
}

/*
 * Entered in thread mode after the console ISR rewrote the stacked PC.
 * The interrupted program never resumes.
 */
void app_abort_trampoline(void)
{
    g_app.running = 0;                  /* close the window for a second kill */
    g_app_stop_reason = FREYA_STOP_CTRLC;
    freya_longjmp(s_return_ctx, 1);
}

void app_fault_trampoline(void)
{
    g_app.running = 0;                  /* a dump fault is a kernel panic */
    if (g_app_stop_reason == FREYA_STOP_BUSFAULT)
        ramdump_write();
    freya_longjmp(s_return_ctx, 1);
}

/* -------------------------------------------------- interrupt handlers */
/*
 * A pin or timer handler is the program's own code running in interrupt
 * context, which is the one place from which a program could take the
 * system down with it: a fault there is not a thread fault, and a loop
 * there is not something the shell can interrupt.  Both are contained.
 *
 * The handler runs inside a jump buffer.  A fault in it - or a Ctrl-C
 * that finds it looping - redirects the interrupt that called it into
 * app_handler_abort(), which unwinds back to just after the call, still
 * inside the same interrupt and on the same stack, so that interrupt
 * returns the way it would have anyway.  The run then ends in thread
 * mode through the stop that was requested along the way, which is where
 * the loader can report it.
 *
 * Handlers never nest: they all share one interrupt priority, so one
 * cannot preempt another and one jump buffer serves all of them.
 */
volatile uint32_t g_irq_events;

static freya_jmpbuf      s_handler_ctx;
static volatile int      s_in_handler;
static volatile uint32_t s_handler_ipsr;    /* the interrupt it runs in */

int app_in_handler(void)
{
    return s_in_handler;
}

static void app_handler_abort(void) __attribute__((noreturn));
static void app_handler_abort(void)
{
    freya_longjmp(s_handler_ctx, 1);
}

int app_handler_call(freya_irq_fn fn, int source, void *arg)
{
    volatile int aborted;
    uint32_t ipsr;

    if (!g_app.running || s_in_handler || s_stop_requested) return -1;

    __asm volatile ("mrs %0, ipsr" : "=r"(ipsr));
    s_handler_ipsr = ipsr;

    /* Nothing may be redirected into a jump buffer that has not been
     * filled in yet, so the flag that invites that is set after the
     * setjmp rather than before it. */
    aborted = freya_setjmp(s_handler_ctx);
    if (!aborted) {
        s_in_handler = 1;
        fn(source, arg);
    }

    s_in_handler = 0;
    return aborted ? -1 : 0;
}

/*
 * Redirect a handler that is not going to finish by itself.  'frame' is
 * the exception frame of whatever was interrupted, and it belongs to the
 * handler only when the interrupt that was interrupted is the one the
 * handler runs in - which is what the IPSR of the stacked xPSR says.
 */
int app_handler_kill(uint32_t *frame)
{
    if (!s_in_handler) return 0;
    if ((frame[7] & 0x1FFUL) != s_handler_ipsr) return 0;

    frame[6] = (uint32_t)(uintptr_t)app_handler_abort;
    frame[7] = (frame[7] & ~0x0600FC00UL) | (1UL << 24);
    return 1;
}

/* ------------------------------------------------- service table calls */
static uint32_t api_cpu_hz(void)             { return g_clocks.hclk_hz; }

static void api_delay(uint32_t ms)
{
    uint32_t start = sys_ticks();
    while ((uint32_t)(sys_ticks() - start) < ms) {
        if (s_stop_requested) return;
        __wfi();
    }
}

/* The heap is not reentrant, so a handler that preempted the thread
 * inside it is refused rather than allowed to corrupt the free list. */
static void *api_malloc(uint32_t size)
{
    void *p;

    if (s_in_handler) return NULL;
    p = kmalloc(size);

    if (p) {
        for (int i = 0; i < APP_MAX_ALLOCS; i++) {
            if (!s_app_allocs[i]) { s_app_allocs[i] = p; return p; }
        }
        /* No slot left to track it - refuse rather than leak on abort. */
        kfree(p);
        return NULL;
    }
    return NULL;
}

static void api_free(void *p)
{
    if (s_in_handler) return;
    for (int i = 0; i < APP_MAX_ALLOCS; i++) {
        if (s_app_allocs[i] == p) { s_app_allocs[i] = NULL; break; }
    }
    kfree(p);
}

static int api_should_stop(void) { return s_stop_requested; }

static void api_yield(void)
{
    if (s_stop_requested) {
        g_app_stop_reason = FREYA_STOP_CTRLC;
        freya_longjmp(s_return_ctx, 1);
    }
}

/* The code is kept whole here and truncated to a byte once, where the
 * status is settled, so exit(-1) and exit(255) end up alike. */
static void api_exit(int code)
{
    s_exit_code = code;
    g_app_stop_reason = FREYA_STOP_EXIT;
    freya_longjmp(s_return_ctx, 1);
}

/*
 * What became of the run before this one.  During a run g_app still holds
 * the previous result - app_run() writes the new one only once the program
 * is done - so a program can ask how its predecessor ended.
 */
int app_last_exit(freya_exit_t *st)
{
    if (!st || !g_app.runs) return -1;

    memset(st, 0, sizeof(*st));
    st->status = g_app.last_status;
    st->reason = g_app.last_stop_reason;
    st->run_ms = g_app.last_run_ms;
    strncpy(st->name, g_app.last_name, sizeof(st->name) - 1);
    return 0;
}

static int api_path(int (*fn)(const char *), const char *path)
{
    char abs[FAT_MAX_PATH];
    if (s_in_handler) return FAT_ERR_INVAL;     /* the card is not reentrant */
    if (fs_abspath(path, abs, sizeof(abs)) != 0) return FAT_ERR_INVAL;
    return fn(abs);
}

static int api_unlink(const char *path) { return api_path(fat_unlink, path); }
static int api_mkdir(const char *path)  { return api_path(fat_mkdir, path); }

static uint32_t api_irq_count(void) { return g_irq_events; }

/*
 * Wait in thread mode for the next pin or timer event.  This is the half
 * of the interrupt API that needs no handler at all: the program sleeps
 * here, where it may do anything, and reads the counters when it wakes.
 * Zero milliseconds waits indefinitely; a stop ends the wait like any
 * other blocking call.
 */
static int api_irq_wait(uint32_t ms)
{
    uint32_t start = sys_ticks();
    uint32_t seen = g_irq_events;

    while (g_irq_events == seen) {
        if (s_stop_requested) return -1;
        if (ms && (uint32_t)(sys_ticks() - start) >= ms) return -1;
        __wfi();
    }
    return 0;
}

static const freya_api_t s_api = {
    .size            = sizeof(freya_api_t),
    .version         = FREYA_ABI_VERSION,
    .putc            = uart_putc,
    .puts            = uart_puts,
    .printf          = kprintf,
    .getc            = uart_getc,
    .getc_timeout    = uart_getc_timeout,
    .kbhit           = uart_rx_ready,
    .malloc          = api_malloc,
    .free            = api_free,
    .ticks_ms        = sys_ticks,
    .delay_ms        = api_delay,
    .should_stop     = api_should_stop,
    .yield           = api_yield,
    .exit            = api_exit,
    .open            = fs_fd_open,
    .close           = fs_fd_close,
    .read            = fs_fd_read,
    .write           = fs_fd_write,
    .seek            = fs_fd_seek,
    .tell            = fs_fd_tell,
    .fsize           = fs_fd_size,
    .unlink          = api_unlink,
    .mkdir           = api_mkdir,
    .opendir         = fs_dd_open,
    .readdir         = fs_dd_read,
    .closedir        = fs_dd_close,
    .led             = led_set,
    .cpu_hz          = api_cpu_hz,
    .rename          = fs_rename,
    .log             = klog,
    .get_log_level   = log_get_level,
    .set_log_level   = log_set_level,
    .last_exit       = app_last_exit,
    .exit_reason_str = app_stop_reason_str,
    .pin_mode        = gpio_pin_mode,
    .pin_read        = gpio_pin_read,
    .pin_write       = gpio_pin_write,
    .pin_toggle      = gpio_pin_toggle,
    .pin_irq_attach  = gpio_irq_attach,
    .pin_irq_detach  = gpio_irq_detach,
    .pin_irq_count   = gpio_irq_count,
    .timer_open      = timer_open,
    .timer_close     = timer_close,
    .timer_start     = timer_start,
    .timer_stop      = timer_stop,
    .timer_period    = timer_period,
    .timer_count     = timer_count,
    .irq_count       = api_irq_count,
    .irq_wait        = api_irq_wait,
};

const freya_api_t *app_api(void)
{
    return &s_api;
}

/* --------------------------------------------------------------- load */
void app_unload(void)
{
    g_app.loaded = 0;
    g_app.running = 0;
    g_app.path[0] = '\0';
    g_app.name[0] = '\0';
    g_app.image_size = 0;
    g_app.bss_start = 0;
    g_app.bss_size = 0;
    g_app.entry = 0;
    g_app.flags = 0;
    g_app.data_src = 0;
    g_app.data_start = 0;
    g_app.data_end = 0;
}

_Static_assert(__builtin_offsetof(freya_app_header_t, flags) ==
               FREYA_APP_HDR_V1_SIZE,
               "the ABI 2 fields must be appended after the ABI 1 header");

/*
 * Read a header, old or new.  The ABI 1 fields come first and are a prefix
 * of the ABI 2 header, so the version is known before the appended fields
 * are read - without that, the first bytes of an ABI 1 image's .text would
 * be mistaken for its flags.
 */
static int read_header(int fd, freya_app_header_t *hdr)
{
    const int tail = (int)sizeof(*hdr) - FREYA_APP_HDR_V1_SIZE;

    memset(hdr, 0, sizeof(*hdr));
    if (fs_fd_read(fd, hdr, FREYA_APP_HDR_V1_SIZE) != FREYA_APP_HDR_V1_SIZE)
        return -1;
    if (hdr->abi_version < 2)
        return 0;
    if (fs_fd_read(fd, (uint8_t *)hdr + FREYA_APP_HDR_V1_SIZE, tail) != tail)
        return -1;
    return 0;
}

/* Common to both kinds of image: is this a Freya program this kernel can
 * speak to at all? */
static int check_magic(const freya_app_header_t *hdr, const char *who)
{
    if (hdr->magic != FREYA_APP_MAGIC) {
        kprintf("%s: not a Freya program (magic 0x%08x)\r\n", who, hdr->magic);
        return -1;
    }
    if (hdr->abi_version < FREYA_ABI_MIN_VERSION ||
        hdr->abi_version > FREYA_ABI_VERSION) {
        kprintf("%s: ABI version %u, this kernel speaks %u to %u\r\n",
                who, hdr->abi_version, (unsigned)FREYA_ABI_MIN_VERSION,
                (unsigned)FREYA_ABI_VERSION);
        return -1;
    }
    return 0;
}

/* Copy the header's name field into g_app, stopping at the first control
 * character so a malformed image cannot print rubbish. */
static void set_app_name(const freya_app_header_t *hdr)
{
    memcpy(g_app.name, hdr->name, sizeof(hdr->name));
    g_app.name[sizeof(hdr->name)] = '\0';
    for (unsigned i = 0; i < sizeof(g_app.name); i++)
        if ((uint8_t)g_app.name[i] < 0x20) { g_app.name[i] = '\0'; break; }
}

#ifdef FREYA_APP_FLASH_ADDR

#define APP_RAM_END  (FREYA_APP_LOAD_ADDR + FREYA_APP_REGION_SIZE)

/* True if [start, end) lies inside the program RAM region. */
static int in_app_ram(uint32_t start, uint32_t end)
{
    return end >= start && start >= FREYA_APP_LOAD_ADDR && end <= APP_RAM_END;
}

/*
 * A flash image is checked more closely than a RAM one, because its
 * addresses are written into flash once and then trusted on every boot.
 * 'want' is the image size the caller settled on.
 */
static int check_xip_header(const freya_app_header_t *hdr, uint32_t want,
                            const char *who)
{
    uint32_t base = FREYA_APP_FLASH_ADDR;

    if (check_magic(hdr, who) != 0) return -1;
    if (hdr->abi_version < 2 || !(hdr->flags & FREYA_APP_F_XIP)) {
        kprintf("%s: not a flash image - link it with app_flash.ld\r\n", who);
        return -1;
    }
    if (hdr->load_addr != base) {
        kprintf("%s: image is linked for 0x%08x, flash region is 0x%08x\r\n",
                who, hdr->load_addr, (unsigned)base);
        return -1;
    }
    if (want == 0 || want > FREYA_APP_FLASH_SIZE) {
        kprintf("%s: image does not fit the program flash region (", who);
        kput_size(FREYA_APP_FLASH_SIZE);
        kprintf(")\r\n");
        return -1;
    }
    if (hdr->entry < base || hdr->entry >= base + want) {
        kprintf("%s: entry 0x%08x is outside the image\r\n", who, hdr->entry);
        return -1;
    }
    if (hdr->data_end > hdr->data_start) {
        uint32_t len = hdr->data_end - hdr->data_start;
        if (!in_app_ram(hdr->data_start, hdr->data_end) ||
            hdr->data_src < base || len > want ||
            hdr->data_src > base + want - len) {
            kprintf("%s: .data (0x%08x -> 0x%08x, %u B) is out of bounds\r\n",
                    who, hdr->data_src, hdr->data_start, len);
            return -1;
        }
    }
    if (hdr->bss_end > hdr->bss_start &&
        !in_app_ram(hdr->bss_start, hdr->bss_end)) {
        kprintf("%s: .bss (0x%08x .. 0x%08x) does not fit the %u KiB "
                "program RAM region\r\n", who, hdr->bss_start, hdr->bss_end,
                (unsigned)(FREYA_APP_REGION_SIZE / 1024));
        return -1;
    }
    return 0;
}

const freya_app_header_t *app_flash_header(void)
{
    const freya_app_header_t *h =
        (const freya_app_header_t *)(uintptr_t)FREYA_APP_FLASH_ADDR;

    /* An erased region reads 0xFF, so the magic alone rules out an empty
     * one; the rest guards against an image left behind by another build. */
    if (h->magic != FREYA_APP_MAGIC) return NULL;
    if (h->abi_version < 2 || h->abi_version > FREYA_ABI_VERSION) return NULL;
    if (!(h->flags & FREYA_APP_F_XIP)) return NULL;
    if (h->load_addr != FREYA_APP_FLASH_ADDR) return NULL;
    if (h->image_size == 0 || h->image_size > FREYA_APP_FLASH_SIZE) return NULL;
    return h;
}

/* Nothing is copied: the image is already where it will execute. */
static int app_load_flash(void)
{
    const freya_app_header_t *hdr = app_flash_header();

    if (!hdr) {
        kprintf("load: no program in flash - install one, or flash it with the kernel\r\n");
        return -1;
    }
    if (check_xip_header(hdr, hdr->image_size, "load") != 0) return -1;

    g_app.loaded     = 1;
    g_app.entry      = hdr->entry;
    g_app.load_addr  = hdr->load_addr;
    g_app.image_size = hdr->image_size;
    g_app.bss_start  = hdr->bss_start;
    g_app.bss_size   = (hdr->bss_end > hdr->bss_start)
                         ? hdr->bss_end - hdr->bss_start : 0;
    g_app.flags      = hdr->flags;
    g_app.data_src   = hdr->data_src;
    g_app.data_start = hdr->data_start;
    g_app.data_end   = hdr->data_end;
    strncpy(g_app.path, APP_FLASH_PATH, sizeof(g_app.path) - 1);
    set_app_name(hdr);
    return 0;
}

#endif /* FREYA_APP_FLASH_ADDR */

int app_load(const char *path)
{
    freya_app_header_t hdr;
    uint8_t *region = (uint8_t *)FREYA_APP_LOAD_ADDR;
    char abs[FAT_MAX_PATH];
    int fd, n;
    uint32_t size, want;
    uint32_t done = 0;

    if (g_app.running) return -1;

#ifdef FREYA_APP_FLASH_ADDR
    if (strcmp(path, APP_FLASH_PATH) == 0) return app_load_flash();
#endif

    if (fs_abspath(path, abs, sizeof(abs)) != 0) {
        kprintf("load: path too long\r\n");
        return -1;
    }

    fd = fs_fd_open(abs, FREYA_O_RDONLY);
    if (fd < 0) {
        kprintf("load: %s: %s\r\n", abs, fat_err_str(fd));
        return -1;
    }
    size = (uint32_t)fs_fd_size(fd);

    if (read_header(fd, &hdr) != 0) {
        kprintf("load: cannot read program header\r\n");
        fs_fd_close(fd);
        return -1;
    }
    if (check_magic(&hdr, "load") != 0) {
        fs_fd_close(fd);
        return -1;
    }
    if (hdr.flags & FREYA_APP_F_XIP) {
#ifdef FREYA_APP_FLASH_ADDR
        kprintf("load: that is a flash image - 'install %s', then 'runflash'\r\n",
                path);
#else
        kprintf("load: that is a flash image, and this board keeps no "
                "program in flash\r\n");
#endif
        fs_fd_close(fd);
        return -1;
    }
    if (hdr.load_addr != FREYA_APP_LOAD_ADDR) {
        kprintf("load: image is linked for 0x%08x, region is 0x%08x\r\n",
                hdr.load_addr, (unsigned)FREYA_APP_LOAD_ADDR);
        fs_fd_close(fd);
        return -1;
    }
    /*
     * The header's image_size is authoritative: an XMODEM transfer pads
     * the file up to a packet boundary, so the file is often larger.
     */
    want = hdr.image_size;
    if (want == 0 || want > size) want = size;

    if (want > FREYA_APP_REGION_SIZE ||
        hdr.bss_end > FREYA_APP_LOAD_ADDR + FREYA_APP_REGION_SIZE ||
        hdr.entry < FREYA_APP_LOAD_ADDR ||
        hdr.entry >= FREYA_APP_LOAD_ADDR + FREYA_APP_REGION_SIZE) {
        kprintf("load: image does not fit the %u KiB program region\r\n",
                (unsigned)(FREYA_APP_REGION_SIZE / 1024));
        fs_fd_close(fd);
        return -1;
    }

    fs_fd_seek(fd, 0, FREYA_SEEK_SET);
    while (done < want) {
        int chunk = (int)MIN(512U, want - done);
        n = fs_fd_read(fd, region + done, chunk);
        if (n <= 0) {
            kprintf("load: read error at offset %u\r\n", done);
            fs_fd_close(fd);
            return -1;
        }
        done += (uint32_t)n;
    }
    fs_fd_close(fd);

    if (hdr.bss_end > hdr.bss_start)
        memset((void *)(uintptr_t)hdr.bss_start, 0, hdr.bss_end - hdr.bss_start);

    /* The image was written as data; make sure the core fetches it fresh. */
    __dsb();
    __isb();

    g_app.loaded     = 1;
    g_app.entry      = hdr.entry;
    g_app.load_addr  = hdr.load_addr;
    g_app.image_size = done;
    g_app.bss_start  = hdr.bss_start;
    g_app.bss_size   = (hdr.bss_end > hdr.bss_start) ? hdr.bss_end - hdr.bss_start : 0;
    g_app.flags      = 0;
    g_app.data_src   = 0;
    g_app.data_start = 0;
    g_app.data_end   = 0;
    strncpy(g_app.path, abs, sizeof(g_app.path) - 1);
    set_app_name(&hdr);

    return 0;
}

/* ------------------------------------------------------------ install */
#ifdef FREYA_APP_FLASH_ADDR

extern char __ramfunc_end[];

#define INSTALL_BUF_SIZE  512

/*
 * An install needs somewhere to stage a card block, and the program RAM
 * region is free: an install refuses to proceed while a program is
 * loaded.  flash_begin() puts its RAM resident routines at the base of
 * that region, so the buffer goes immediately after them - which is how
 * the whole feature costs nothing out of a 2 KiB heap.
 */
static uint8_t *install_buf(void)
{
    uintptr_t a = ((uintptr_t)__ramfunc_end + 3U) & ~(uintptr_t)3U;
    return (uint8_t *)a;
}

/*
 * fs_fd_read() is allowed to return a short count, and the flash writer
 * needs whole halfwords at an even address - so every chunk but the last
 * is filled before it is used, and only a genuinely odd image_size ever
 * reaches flash_program() with an odd length.
 */
static int read_full(int fd, uint8_t *buf, int want)
{
    int got = 0;

    while (got < want) {
        int n = fs_fd_read(fd, buf + got, want - got);
        if (n <= 0) break;
        got += n;
    }
    return got;
}

/* 1 if the file matches the flash region over 'want' bytes, 0 if it does
 * not, -1 on a read error. */
static int image_matches(int fd, uint32_t want, uint8_t *buf)
{
    const uint8_t *flash = (const uint8_t *)(uintptr_t)FREYA_APP_FLASH_ADDR;
    uint32_t done = 0;

    fs_fd_seek(fd, 0, FREYA_SEEK_SET);
    while (done < want) {
        int chunk = (int)MIN((uint32_t)INSTALL_BUF_SIZE, want - done);

        if (read_full(fd, buf, chunk) != chunk) return -1;
        if (memcmp(buf, flash + done, (size_t)chunk) != 0) return 0;
        done += (uint32_t)chunk;
    }
    return 1;
}

int app_install(const char *path)
{
    freya_app_header_t hdr;
    char abs[FAT_MAX_PATH];
    uint8_t *buf = install_buf();
    uint32_t page = flash_page_size();
    uint32_t size, want, pages, done = 0;
    int fd, rc, same;

    if (g_app.running) {
        kprintf("install: a program is running - stop it first\r\n");
        return -1;
    }
    if (g_app.loaded) app_unload();

    if ((uintptr_t)buf + INSTALL_BUF_SIZE > APP_RAM_END) {
        kprintf("install: no room for a card buffer in the program region\r\n");
        return -1;
    }
    if (fs_abspath(path, abs, sizeof(abs)) != 0) {
        kprintf("install: path too long\r\n");
        return -1;
    }
    fd = fs_fd_open(abs, FREYA_O_RDONLY);
    if (fd < 0) {
        kprintf("install: %s: %s\r\n", abs, fat_err_str(fd));
        return -1;
    }
    size = (uint32_t)fs_fd_size(fd);

    if (read_header(fd, &hdr) != 0) {
        kprintf("install: cannot read program header\r\n");
        fs_fd_close(fd);
        return -1;
    }
    want = hdr.image_size;
    if (want == 0 || want > size) want = size;
    if (check_xip_header(&hdr, want, "install") != 0) {
        fs_fd_close(fd);
        return -1;
    }

    /* Flash endurance is 10k cycles, so an unchanged image is left alone
     * rather than reprogrammed. */
    same = image_matches(fd, want, buf);
    if (same < 0) {
        kprintf("install: read error\r\n");
        fs_fd_close(fd);
        return -1;
    }
    if (same) {
        fs_fd_close(fd);
        kprintf("install: flash already holds this image, nothing written\r\n");
        return app_load_flash();
    }

    rc = flash_begin();
    if (rc != FLASH_OK) {
        kprintf("install: %s\r\n", flash_err_str(rc));
        fs_fd_close(fd);
        return -1;
    }

    {
        uint32_t erase0 = FREYA_APP_FLASH_ADDR & ~(page - 1);
        pages = (FREYA_APP_FLASH_ADDR + want - erase0 + page - 1) / page;
    }
    kprintf("install: console input is dropped while flash is busy\r\n");
    kprintf("  erasing %u page%s ... ", pages, pages == 1 ? "" : "s");
    uart_drain_tx();

    rc = flash_erase(FREYA_APP_FLASH_ADDR, want);
    if (rc != FLASH_OK) goto fail;

    kprintf("writing ... ");
    uart_drain_tx();

    fs_fd_seek(fd, 0, FREYA_SEEK_SET);
    while (done < want) {
        int chunk = (int)MIN((uint32_t)INSTALL_BUF_SIZE, want - done);

        if (read_full(fd, buf, chunk) != chunk) {
            kprintf("\r\ninstall: read error at offset %u\r\n", done);
            goto fail_quiet;
        }
        rc = flash_program(FREYA_APP_FLASH_ADDR + done, buf, (uint32_t)chunk);
        if (rc != FLASH_OK) goto fail;
        done += (uint32_t)chunk;
    }
    flash_end();
    uart_rx_flush();

    /*
     * Every halfword was verified as it was written.  This reads the whole
     * image back against the file once more, which is what catches a
     * mistake in an address rather than in a cell.
     */
    if (image_matches(fd, want, buf) != 1) {
        kprintf("\r\ninstall: verify failed\r\n");
        fs_fd_close(fd);
        return -1;
    }
    fs_fd_close(fd);

    kprintf("ok\r\n");
    kprintf("installed %s at 0x%08x: ", abs, (unsigned)FREYA_APP_FLASH_ADDR);
    kput_size(want);
    kprintf(" in %u page%s\r\n", pages, pages == 1 ? "" : "s");
    return app_load_flash();

fail:
    kprintf("\r\ninstall: %s at offset %u\r\n", flash_err_str(rc), done);
fail_quiet:
    flash_end();
    uart_rx_flush();
    fs_fd_close(fd);
    return -1;
}

int app_flash_erase(void)
{
    int rc;

    if (g_app.running) {
        kprintf("uninstall: a program is running - stop it first\r\n");
        return -1;
    }
    if (g_app.loaded) app_unload();

    rc = flash_begin();
    if (rc == FLASH_OK) {
        kprintf("uninstall: erasing the program flash region ... ");
        uart_drain_tx();
        rc = flash_erase(FREYA_APP_FLASH_ADDR, FREYA_APP_FLASH_SIZE);
        flash_end();
        uart_rx_flush();
    }
    if (rc != FLASH_OK) {
        kprintf("\r\nuninstall: %s\r\n", flash_err_str(rc));
        return -1;
    }
    kprintf("ok\r\n");
    return 0;
}

#define SLOT_ERASED  0xFFFFFFFFUL

static uint32_t slot_word(uint32_t off)
{
    return *(const uint32_t *)(uintptr_t)(FREYA_AUTOSTART_ADDR + off);
}

/* True when 'want' can be programmed over 'cur' without an erase (1->0 only). */
static int slot_can_program(uint32_t cur, uint32_t want)
{
    return (cur & want) == want;
}

/*
 * Rewrite the auto-start slot, keeping whichever of the three words the
 * caller did not intend to change.  flash_erase() of the 128-byte slot
 * restores anything else that shares the same erase unit (the start of
 * the program image on the Blue Pill; nothing on the Black Pill).
 */
static int slot_write(uint32_t magic, uint32_t level, uint32_t ramdump)
{
    uint32_t cur_m = slot_word(0);
    uint32_t cur_l = slot_word(FREYA_LOGLEVEL_OFF);
    uint32_t cur_d = slot_word(FREYA_RAMDUMP_OFF);
    int rc;

    if (cur_m == magic && cur_l == level && cur_d == ramdump) return FLASH_OK;
    if (g_app.running) return FLASH_ERR_BUSY;
    if (g_app.loaded) app_unload();

    rc = flash_begin();
    if (rc != FLASH_OK) return rc;

    if (!slot_can_program(cur_m, magic) || !slot_can_program(cur_l, level) ||
        !slot_can_program(cur_d, ramdump)) {
        rc = flash_erase(FREYA_AUTOSTART_ADDR, FREYA_AUTOSTART_SIZE);
        if (rc != FLASH_OK) {
            flash_end();
            return rc;
        }
        cur_m = SLOT_ERASED;
        cur_l = SLOT_ERASED;
        cur_d = SLOT_ERASED;
    }
    if (cur_m != magic) {
        rc = flash_program(FREYA_AUTOSTART_ADDR, &magic, sizeof(magic));
        if (rc != FLASH_OK) {
            flash_end();
            return rc;
        }
    }
    if (cur_l != level) {
        rc = flash_program(FREYA_AUTOSTART_ADDR + FREYA_LOGLEVEL_OFF,
                           &level, sizeof(level));
        if (rc != FLASH_OK) {
            flash_end();
            return rc;
        }
    }
    if (cur_d != ramdump) {
        rc = flash_program(FREYA_AUTOSTART_ADDR + FREYA_RAMDUMP_OFF,
                           &ramdump, sizeof(ramdump));
        if (rc != FLASH_OK) {
            flash_end();
            return rc;
        }
    }
    flash_end();
    return FLASH_OK;
}

int app_autostart_enabled(void)
{
    return slot_word(0) == FREYA_AUTOSTART_MAGIC;
}

int app_autostart_set(int enable)
{
    uint32_t magic = enable ? FREYA_AUTOSTART_MAGIC : SLOT_ERASED;

    return slot_write(magic, slot_word(FREYA_LOGLEVEL_OFF),
                      slot_word(FREYA_RAMDUMP_OFF));
}

uint32_t app_log_level_stored(void)
{
    return slot_word(FREYA_LOGLEVEL_OFF);
}

int app_log_level_store(uint32_t level)
{
    return slot_write(slot_word(0), level, slot_word(FREYA_RAMDUMP_OFF));
}

int app_ramdump_enabled(void)
{
    return slot_word(FREYA_RAMDUMP_OFF) == FREYA_RAMDUMP_MAGIC;
}

int app_ramdump_set(int enable)
{
    uint32_t flag = enable ? FREYA_RAMDUMP_MAGIC : SLOT_ERASED;

    return slot_write(slot_word(0), slot_word(FREYA_LOGLEVEL_OFF), flag);
}

#endif /* FREYA_APP_FLASH_ADDR */

/* ---------------------------------------------------------------- run */
int app_run(int argc, char **argv)
{
    typedef int (*entry_fn)(const freya_api_t *, int, char **);
    entry_fn entry;
    uint32_t t0;
    int ret, status;

    if (!g_app.loaded) {
        kprintf("run: no program loaded\r\n");
        return FREYA_EXIT_NOTFOUND;
    }

    memset(s_app_allocs, 0, sizeof(s_app_allocs));
    s_stop_requested  = 0;
    g_app_stop_reason = FREYA_STOP_NONE;
    s_exit_code       = 0;
    s_in_handler      = 0;
    g_irq_events      = 0;

#ifdef FREYA_APP_FLASH_ADDR
    /*
     * A flash image is immutable, so every run can start from its own
     * initialisers.  A RAM image cannot: its .data *is* the loaded copy,
     * and reinitialising means loading it again.
     */
    if (g_app.flags & FREYA_APP_F_XIP) {
        if (g_app.data_end > g_app.data_start)
            memcpy((void *)(uintptr_t)g_app.data_start,
                   (const void *)(uintptr_t)g_app.data_src,
                   g_app.data_end - g_app.data_start);
        if (g_app.bss_size)
            memset((void *)(uintptr_t)g_app.bss_start, 0, g_app.bss_size);
    }
#endif

    entry = (entry_fn)(uintptr_t)(g_app.entry | 1UL);   /* Thumb */
    t0 = sys_ticks();

    if (freya_setjmp(s_return_ctx) == 0) {
        g_app.running = 1;
        ret = entry(app_api(), argc, argv);
        s_exit_code = ret;
        if (!g_app_stop_reason) g_app_stop_reason = FREYA_STOP_NONE;
    } else {
        /* Unwound from exit(), Ctrl-C or a fault. */
        ret = s_exit_code;
    }

    /*
     * The status is the program's code only when the program itself ended
     * the run; a Ctrl-C or a fault reports 128 + the reason instead, so a
     * caller can tell a program that failed from one that never finished.
     */
    status = freya_exit_status(g_app_stop_reason, ret);

    g_app.running = 0;
    /* Nothing belonging to the program may still be able to run: its pin
     * and timer interrupts are dropped before the memory their handlers
     * were using is handed back. */
    gpio_irq_release();
    timer_release();
    g_app.last_run_ms = sys_ticks() - t0;
    g_app.last_status = status;
    g_app.last_stop_reason = g_app_stop_reason;
    g_app.runs++;
    strncpy(g_app.last_name, g_app.name[0] ? g_app.name : g_app.path,
            sizeof(g_app.last_name) - 1);
    g_app.last_name[sizeof(g_app.last_name) - 1] = '\0';

    klog(status == FREYA_EXIT_OK ? FREYA_LOG_INFO : FREYA_LOG_WARN,
         "%s %s, status %d, %u ms", g_app.last_name,
         app_stop_reason_str(g_app_stop_reason), status, g_app.last_run_ms);

    /* Reclaim anything the program left behind. */
    for (int i = 0; i < APP_MAX_ALLOCS; i++) {
        if (s_app_allocs[i]) { kfree(s_app_allocs[i]); s_app_allocs[i] = NULL; }
    }
    fs_close_all();
    uart_set_raw(0);
    s_stop_requested = 0;

    return status;
}
