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
 * and all three unwind through the same freya_longjmp() back into run().
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
    if (!g_app_stop_reason) g_app_stop_reason = APP_STOP_CTRLC;
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

    /* Bit 24 keeps the core in Thumb state, the IT bits are cleared so the
     * first instructions cannot be skipped, bit 9 (stack realignment) and
     * the rest of the frame are left alone. */
    frame[6] = (uint32_t)(uintptr_t)app_abort_trampoline;
    frame[7] = (frame[7] & ~0x0600FC00UL) | (1UL << 24);
}

int app_should_stop(void)
{
    return s_stop_requested;
}

const char *app_stop_reason_str(int reason)
{
    switch (reason) {
    case APP_STOP_EXIT:       return "exited";
    case APP_STOP_CTRLC:      return "stopped by Ctrl-C";
    case APP_STOP_HARDFAULT:  return "killed by hard fault";
    case APP_STOP_MEMFAULT:   return "killed by memory fault";
    case APP_STOP_BUSFAULT:   return "killed by bus fault";
    case APP_STOP_USAGEFAULT: return "killed by usage fault";
    default:                  return "returned";
    }
}

/*
 * Entered in thread mode after the console ISR rewrote the stacked PC.
 * The interrupted program never resumes.
 */
void app_abort_trampoline(void)
{
    g_app.running = 0;                  /* close the window for a second kill */
    g_app_stop_reason = APP_STOP_CTRLC;
    freya_longjmp(s_return_ctx, 1);
}

void app_fault_trampoline(void)
{
    g_app.running = 0;
    freya_longjmp(s_return_ctx, 1);
}

/* ------------------------------------------------- service table calls */
static void api_putc(char c)            { uart_putc(c); }
static void api_puts(const char *s)     { uart_puts(s); }

static int api_getc(void)                   { return uart_getc(); }
static int api_getc_timeout(uint32_t ms)    { return uart_getc_timeout(ms); }
static int api_kbhit(void)                  { return uart_rx_ready(); }
static uint32_t api_ticks(void)             { return sys_ticks(); }
static uint32_t api_cpu_hz(void)            { return g_clocks.hclk_hz; }
static void api_led(int on)                 { led_set(on); }

static void api_delay(uint32_t ms)
{
    uint32_t start = sys_ticks();
    while ((uint32_t)(sys_ticks() - start) < ms) {
        if (s_stop_requested) return;
        __wfi();
    }
}

static void *api_malloc(uint32_t size)
{
    void *p = kmalloc(size);

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
    for (int i = 0; i < APP_MAX_ALLOCS; i++) {
        if (s_app_allocs[i] == p) { s_app_allocs[i] = NULL; break; }
    }
    kfree(p);
}

static int api_should_stop(void) { return s_stop_requested; }

static void api_yield(void)
{
    if (s_stop_requested) {
        g_app_stop_reason = APP_STOP_CTRLC;
        freya_longjmp(s_return_ctx, 1);
    }
}

static void api_exit(int code)
{
    s_exit_code = code;
    g_app_stop_reason = APP_STOP_EXIT;
    freya_longjmp(s_return_ctx, 1);
}

static int api_unlink(const char *path)
{
    char abs[FAT_MAX_PATH];
    if (fs_abspath(path, abs, sizeof(abs)) != 0) return FAT_ERR_INVAL;
    return fat_unlink(abs);
}

static int api_mkdir(const char *path)
{
    char abs[FAT_MAX_PATH];
    if (fs_abspath(path, abs, sizeof(abs)) != 0) return FAT_ERR_INVAL;
    return fat_mkdir(abs);
}

static const freya_api_t s_api = {
    .size          = sizeof(freya_api_t),
    .version       = FREYA_ABI_VERSION,
    .putc          = api_putc,
    .puts          = api_puts,
    .printf        = kprintf,
    .getc          = api_getc,
    .getc_timeout  = api_getc_timeout,
    .kbhit         = api_kbhit,
    .malloc        = api_malloc,
    .free          = api_free,
    .ticks_ms      = api_ticks,
    .delay_ms      = api_delay,
    .should_stop   = api_should_stop,
    .yield         = api_yield,
    .exit          = api_exit,
    .open          = fs_fd_open,
    .close         = fs_fd_close,
    .read          = fs_fd_read,
    .write         = fs_fd_write,
    .seek          = fs_fd_seek,
    .tell          = fs_fd_tell,
    .fsize         = fs_fd_size,
    .unlink        = api_unlink,
    .mkdir         = api_mkdir,
    .opendir       = fs_dd_open,
    .readdir       = fs_dd_read,
    .closedir      = fs_dd_close,
    .led           = api_led,
    .cpu_hz        = api_cpu_hz,
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
    g_app.bss_size = 0;
    g_app.entry = 0;
}

int app_load(const char *path)
{
    freya_app_header_t hdr;
    uint8_t *region = (uint8_t *)FREYA_APP_LOAD_ADDR;
    char abs[FAT_MAX_PATH];
    int fd, n;
    uint32_t size, want;
    uint32_t done = 0;

    if (g_app.running) return -1;
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

    n = fs_fd_read(fd, &hdr, (int)sizeof(hdr));
    if (n != (int)sizeof(hdr)) {
        kprintf("load: cannot read program header\r\n");
        fs_fd_close(fd);
        return -1;
    }
    if (hdr.magic != FREYA_APP_MAGIC) {
        kprintf("load: not a Freya program (magic 0x%08x)\r\n", hdr.magic);
        fs_fd_close(fd);
        return -1;
    }
    if (hdr.abi_version != FREYA_ABI_VERSION) {
        kprintf("load: ABI version %u, this kernel speaks %u\r\n",
                hdr.abi_version, (unsigned)FREYA_ABI_VERSION);
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
    g_app.bss_size   = (hdr.bss_end > hdr.bss_start) ? hdr.bss_end - hdr.bss_start : 0;
    strncpy(g_app.path, abs, sizeof(g_app.path) - 1);
    memcpy(g_app.name, hdr.name, sizeof(hdr.name));
    g_app.name[sizeof(hdr.name)] = '\0';
    for (unsigned i = 0; i < sizeof(g_app.name); i++)
        if ((uint8_t)g_app.name[i] < 0x20) { g_app.name[i] = '\0'; break; }

    return 0;
}

/* ---------------------------------------------------------------- run */
int app_run(int argc, char **argv)
{
    typedef int (*entry_fn)(const freya_api_t *, int, char **);
    entry_fn entry;
    uint32_t t0;
    int ret;

    if (!g_app.loaded) {
        kprintf("run: no program loaded\r\n");
        return -1;
    }

    memset(s_app_allocs, 0, sizeof(s_app_allocs));
    s_stop_requested  = 0;
    g_app_stop_reason = APP_STOP_NONE;
    s_exit_code       = 0;
    entry = (entry_fn)(uintptr_t)(g_app.entry | 1UL);   /* Thumb */
    t0 = sys_ticks();

    if (freya_setjmp(s_return_ctx) == 0) {
        g_app.running = 1;
        ret = entry(app_api(), argc, argv);
        s_exit_code = ret;
        if (!g_app_stop_reason) g_app_stop_reason = APP_STOP_NONE;
    } else {
        /* Unwound from exit(), Ctrl-C or a fault. */
        ret = s_exit_code;
    }

    g_app.running = 0;
    g_app.last_run_ms = sys_ticks() - t0;
    g_app.last_exit_code = ret;
    g_app.last_stop_reason = g_app_stop_reason;

    /* Reclaim anything the program left behind. */
    for (int i = 0; i < APP_MAX_ALLOCS; i++) {
        if (s_app_allocs[i]) { kfree(s_app_allocs[i]); s_app_allocs[i] = NULL; }
    }
    fs_close_all();
    uart_set_raw(0);
    s_stop_requested = 0;

    return ret;
}
