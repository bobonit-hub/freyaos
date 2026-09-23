/*
 * Freya - an Altair 8800b Turnkey.
 *
 * An 8080, 48 KiB of RAM (up to 62 with the heap's help), and the
 * Turnkey Module: its 1 KiB of SRAM at F800h, its four PROM sockets at
 * FC00h and its 2SIO-compatible terminal port at 10h.  Original 8080
 * software - Altair BASIC first of all - is loaded from the card and
 * runs unchanged, talking to the Freya console as its teletype.
 *
 *   install altair.xip.bin                  once, into program flash
 *   runflash                                the files in /altair/
 *   runflash xbasic.bin                     one image at 0000h, run there
 *   runflash ext41.tap                      a BASIC tape, read and run
 *   runflash prog.hex --go 100              Intel HEX, started at 0100h
 *
 * With no image named, /altair/xbasic.bin is loaded at 0000h - or, when
 * there is no such memory image, the tape /altair/xbasic.tap is read -
 * and the PROMs /altair/turmon.bin (FD00h) and /altair/mbl.bin (FE00h)
 * go into their sockets.  The machine then starts at 0000h, or at FD00h
 * when TURMON is there, which is what the Turnkey's auto-start does.
 *
 * Ctrl-] opens the menu that stands in for the front panel the 8800b
 * Turnkey never had.  'upload' there receives an image, or an Intel
 * HEX file, over XMODEM on this same console.
 *
 * The 8080 is in i8080.c, memory in mem.c, the ports in io.c, the
 * console side in term.c and the card side in load.c.  The Makefile
 * builds a sample from main.c alone, so they are included here.
 * samples/altair16 includes this file with 16 KiB of RAM.  On the Black
 * Pill that fits the program region and so loads with 'run' as well as
 * from flash.  On the Blue Pill the same RAM is kept in program flash.
 */
#include <stddef.h>
#include "freya_api.h"

static const freya_api_t *g;

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

#include "i8080.h"
#include "mem.c"
#include "term.c"
#include "io.c"
#include "load.c"
#include "i8080.c"

#define DIR            "/altair/"
#define DEFAULT_BASIC  DIR "xbasic.bin"
#define DEFAULT_TAPE   DIR "xbasic.tap"
#define DEFAULT_BOOT   0x3FC2u       /* Extended BASIC's bootstrap      */
#define DEFAULT_TURMON DIR "turmon.bin"
#define DEFAULT_MBL    DIR "mbl.bin"
#define TURMON_ADDR    0xFD00u
#define MBL_ADDR       0xFE00u

#define SLICE          20000u        /* clock states between yields     */
#define LINE_MAX       80
#define MAX_ARGS       12

static i8080_t  s_cpu;
static uint16_t s_start;             /* where reset sends the CPU       */
static unsigned s_mhz;               /* 0: as fast as it goes           */
static uint8_t  s_quit;

/* ---------------------------------------------------------- helpers */
/* Hexadecimal, as the Altair's own documentation writes addresses; a
 * trailing 'h' is allowed. */
static int parse_hex(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int n = 0;

    for (; *s; s++, n++) {
        int d = hexval(*s);

        if (d < 0) {
            if ((*s == 'h' || *s == 'H') && s[1] == '\0' && n)
                break;
            return -1;
        }
        v = v * 16u + (uint32_t)d;
        if (v > 0xFFFFFFu)
            return -1;
    }
    if (!n)
        return -1;
    *out = v;
    return 0;
}

static int parse_dec(const char *s, uint32_t *out)
{
    uint32_t v = 0;

    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000u)
            return -1;
        v = v * 10u + (uint32_t)(*s - '0');
    }
    *out = v;
    return 0;
}

static int parse_addr(const char *s, uint16_t *out)
{
    uint32_t v;

    if (parse_hex(s, &v) || v > 0xFFFFu)
        return -1;
    *out = (uint16_t)v;
    return 0;
}

static int split(char *line, char **argv, int max)
{
    int argc = 0;

    while (*line && argc < max) {
        while (*line == ' ' || *line == '\t')
            *line++ = '\0';
        if (!*line)
            break;
        argv[argc++] = line;
        while (*line && *line != ' ' && *line != '\t')
            line++;
    }
    return argc;
}

static void show_regs(void)
{
    const i8080_t *c = &s_cpu;
    uint8_t f = c->r[R_F];

    g->printf("PC=%04X SP=%04X A=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X "
              "F=%c%c%c%c%c %s%s\r\n",
              c->pc, c->sp, c->r[R_A], c->r[R_B], c->r[R_C],
              c->r[R_D], c->r[R_E], c->r[R_H], c->r[R_L],
              (f & F_S) ? 'S' : '-', (f & F_Z) ? 'Z' : '-',
              (f & F_AC) ? 'A' : '-', (f & F_P) ? 'P' : '-',
              (f & F_CY) ? 'C' : '-',
              c->inte ? "EI" : "DI", c->halted ? " HLT" : "");
}

static void dump(uint16_t at, uint32_t len)
{
    uint32_t a = at;

    while (len) {
        uint32_t n = len > 16 ? 16 : len;

        g->printf("%04X ", (unsigned)a);
        for (uint32_t i = 0; i < n; i++)
            g->printf(" %02X", mem_rd((uint16_t)(a + i)));
        for (uint32_t i = n; i < 16; i++)
            g->puts("   ");
        g->puts("  ");
        for (uint32_t i = 0; i < n; i++) {
            uint8_t b = mem_rd((uint16_t)(a + i)) & 0x7F;
            g->putc(b >= 0x20 && b < 0x7F ? (char)b : '.');
        }
        g->puts("\r\n");
        a = (a + n) & 0xFFFFu;
        len -= n;
    }
}

/* Loads one file and says what happened.  Returns 0 when it loaded. */
static int load_report(const char *path, uint16_t at)
{
    int32_t n = load_file(path, at);

    if (n == -1) {
        g->printf("altair: cannot open %s\r\n", path);
        return -1;
    }
    if (n < -1) {
        g->printf("altair: %s: bad HEX record on line %d\r\n",
                  path, (int)(-2 - n));
        return -1;
    }
    if (is_hex_name(path))
        g->printf("altair: %s: %d bytes\r\n", path, (int)n);
    else
        g->printf("altair: %s: %d bytes at %04X\r\n", path, (int)n, at);
    return 0;
}

static void load_prom(const char *path, uint16_t at, int quiet)
{
    int fd = g->open(path, FREYA_O_RDONLY);

    if (fd < 0) {
        if (!quiet)
            g->printf("altair: cannot open %s\r\n", path);
        return;
    }
    g->close(fd);
    if (at < MEM_PROM) {
        g->printf("altair: a PROM socket is FC00, FD00, FE00 or FF00\r\n");
        return;
    }
    load_report(path, at);
}

static void reset(uint16_t pc)
{
    i8080_reset(&s_cpu, pc);
    s_cpu.cycles = 0;
    s_pending = -1;
}

/* Reads a BASIC tape the way an Altair did: the bootstrap here, then the
 * rest through the terminal port by the loader on the tape.  Returns 0
 * with the CPU pointed at that loader. */
static int boot_tape(const char *path, uint16_t hl)
{
    int32_t at;

    /* The keyed-in loader occupies HL-1 down to H:00.  Past the end of
     * main RAM those stores vanish, and the CPU would start on FFh. */
    if ((uint32_t)(uint16_t)(hl - 1) >= s_ram_kb * 1024u) {
        g->printf("altair: the loader at %04X is past the %u KiB of RAM\r\n",
                  hl, s_ram_kb);
        return -1;
    }
    if (tape_attach(path) < 0) {
        g->printf("altair: cannot open %s\r\n", path);
        return -1;
    }
    at = tape_boot(hl);
    if (at < 0) {
        g->printf("altair: %s ended inside its own loader\r\n", path);
        tape_detach();
        return -1;
    }
    g->printf("altair: %s: loader at %04X, reading the rest of the tape\r\n",
              path, (unsigned)at);
    reset((uint16_t)at);
    return 0;
}

static int is_tape_name(const char *path)
{
    return ends_with(path, ".tap");
}

/* ------------------------------------------------------------- menu */
static const char k_help[] =
    "  c                 continue (also Enter, or Ctrl-] again)\r\n"
    "  regs              show the 8080 registers\r\n"
    "  x ADDR [LEN]      examine memory\r\n"
    "  d ADDR BYTE...    deposit bytes\r\n"
    "  go ADDR           jump there and continue\r\n"
    "  reset             reset to the start address and continue\r\n"
    "  load FILE [ADDR]  load an image (default 0000) or an Intel HEX file\r\n"
    "  upload [ADDR] [raw]  receive an image over XMODEM (default 0000)\r\n"
    "  upload hex        receive an Intel HEX file over XMODEM\r\n"
    "  save FILE ADDR LEN  save memory as an image\r\n"
    "  prom FILE [ADDR]  fill a PROM socket (default FD00)\r\n"
    "  boot FILE [TYPE]  read a BASIC tape and run it; TYPE is 4k32, 4k40,\r\n"
    "                    8k, ext (the default) or disk\r\n"
    "  tape [FILE]       attach a tape to the readers, or detach it\r\n"
    "  pio [off|tape|term]  show or set what the 88-PIO at 04h/05h drives\r\n"
    "  sw [HEX]          show or set the sense switches\r\n"
    "  speed [MHZ]       show or set the clock, 0 for as fast as it goes\r\n"
    "  upper on|off      fold typed lowercase to uppercase\r\n"
    "  bs on|off         send Backspace as DEL\r\n"
    "  quit              leave the emulator\r\n"
    "  numbers are hexadecimal, except KB and MHz\r\n";

/* Reads a line with its own echo; Ctrl-C drops it, Ctrl-] returns to
 * the machine.  Returns the length, or -1 for Ctrl-]. */
static int read_line(char *buf, int max)
{
    int n = 0;

    for (;;) {
        int c = g->getc();

        if (c < 0 || c == KEY_MENU) {
            g->puts("\r\n");
            return -1;
        }
        if (c == '\r' || c == '\n') {
            g->puts("\r\n");
            buf[n] = '\0';
            return n;
        }
        if (c == 0x03) {
            g->puts("^C\r\n");
            n = 0;
            buf[0] = '\0';
            return 0;
        }
        if (c == KEY_BS || c == KEY_DEL) {
            if (n) {
                n--;
                g->puts("\b \b");
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && n < max - 1) {
            buf[n++] = (char)c;
            g->putc((char)c);
        }
    }
}

static int on_off(const char *s, uint8_t *flag)
{
    if (streq(s, "on"))
        *flag = 1;
    else if (streq(s, "off"))
        *flag = 0;
    else
        return -1;
    return 0;
}

static void upload_error(int32_t rc)
{
    const char *s;

    if (rc == -2)
        s = "timed out waiting for the sender";
    else if (rc == -3)
        s = "cancelled by the sender";
    else if (rc == -4)
        s = "too many bad packets";
    else if (rc == -5)
        s = "packet sequence error";
    else if (rc == -7)
        s = "no room for a 1K packet; send 128-byte XMODEM";
    else
        s = "transfer failed";
    g->printf("%s\r\n", s);
}

/* Receives one image.  The CPU is left where it is; 'go' starts it.
 * A binary is stripped of its XMODEM padding unless raw is set. */
static void upload_cmd(int hex, uint16_t at, int raw)
{
    int32_t n;

    g->puts("Ready for XMODEM.\r\n"
            "Start the sender now (sx file, or sx -k).  "
            "Ctrl-X twice aborts.\r\n");
    s_hex_line = 0;
    n = hex ? upload_hex() : upload_bin(at, !raw);
    if (s_hex_line)
        g->printf("bad HEX record on line %d\r\n", s_hex_line);
    else if (n < 0)
        upload_error(n);
    else if (hex)
        g->printf("%d bytes\r\n", (int)n);
    else
        g->printf("%d bytes at %04X\r\n", (int)n, at);
}

/* One menu command.  Returns 1 when the machine should run again. */
static int menu_command(int argc, char **argv)
{
    const char *cmd = argv[0];
    uint16_t a;
    uint32_t v;

    if (argc == 0 || streq(cmd, "c") || streq(cmd, "cont"))
        return 1;
    if (streq(cmd, "help") || streq(cmd, "?")) {
        g->puts(k_help);
    } else if (streq(cmd, "regs")) {
        show_regs();
    } else if (streq(cmd, "x")) {
        v = 64;
        if (argc < 2 || parse_addr(argv[1], &a) ||
            (argc > 2 && (parse_hex(argv[2], &v) || v > 0x10000u)))
            g->puts("x ADDR [LEN]\r\n");
        else
            dump(a, v);
    } else if (streq(cmd, "d")) {
        if (argc < 3 || parse_addr(argv[1], &a)) {
            g->puts("d ADDR BYTE...\r\n");
        } else {
            for (int i = 2; i < argc; i++) {
                if (parse_hex(argv[i], &v) || v > 0xFF) {
                    g->printf("not a byte: %s\r\n", argv[i]);
                    break;
                }
                mem_poke((uint16_t)(a + i - 2), (uint8_t)v);
            }
        }
    } else if (streq(cmd, "go")) {
        if (argc < 2 || parse_addr(argv[1], &a)) {
            g->puts("go ADDR\r\n");
        } else {
            reset(a);
            return 1;
        }
    } else if (streq(cmd, "reset")) {
        reset(s_start);
        return 1;
    } else if (streq(cmd, "load")) {
        a = 0;
        if (argc < 2 || (argc > 2 && parse_addr(argv[2], &a)))
            g->puts("load FILE [ADDR]\r\n");
        else
            load_report(argv[1], a);
    } else if (streq(cmd, "upload")) {
        int hex = 0, raw = 0, bad = 0;

        a = 0;
        if (argc >= 2 && streq(argv[1], "hex")) {
            hex = 1;
            bad = argc > 2;
        } else {
            int i = 1;

            if (argc >= 2 && !streq(argv[1], "raw")) {
                if (parse_addr(argv[1], &a))
                    bad = 1;
                i = 2;
            }
            if (!bad && i < argc) {
                if (i + 1 == argc && streq(argv[i], "raw"))
                    raw = 1;
                else
                    bad = 1;
            }
        }
        if (bad)
            g->puts("upload [ADDR] [raw] | upload hex\r\n");
        else if (!s_raw)
            g->puts("upload needs a raw console\r\n");
        else
            upload_cmd(hex, a, raw);
    } else if (streq(cmd, "save")) {
        if (argc < 4 || parse_addr(argv[2], &a) || parse_hex(argv[3], &v) ||
            v == 0 || v > 0x10000u) {
            g->puts("save FILE ADDR LEN\r\n");
        } else {
            int32_t n = save_bin(argv[1], a, v);

            if (n < 0)
                g->printf("cannot write %s\r\n", argv[1]);
            else
                g->printf("%s: %d bytes from %04X\r\n", argv[1], (int)n, a);
        }
    } else if (streq(cmd, "prom")) {
        a = TURMON_ADDR;
        if (argc < 2 || (argc > 2 && parse_addr(argv[2], &a)))
            g->puts("prom FILE [ADDR]\r\n");
        else
            load_prom(argv[1], a, 0);
    } else if (streq(cmd, "boot")) {
        uint16_t hl = DEFAULT_BOOT;

        if (argc < 2 || (argc > 2 && boot_lookup(argv[2], &hl)))
            g->puts("boot FILE [4k32|4k40|8k|ext|disk]\r\n");
        else if (boot_tape(argv[1], hl) == 0)
            return 1;
    } else if (streq(cmd, "tape")) {
        if (argc < 2) {
            if (s_tape >= 0)
                g->printf("tape attached, %u bytes read\r\n",
                          (unsigned)s_tape_count);
            tape_detach();
            g->puts("no tape\r\n");
        } else if (tape_attach(argv[1]) < 0) {
            g->printf("cannot open %s\r\n", argv[1]);
        } else {
            g->printf("%s on the tape reader\r\n", argv[1]);
        }
    } else if (streq(cmd, "pio")) {
        if (argc > 1 && pio_lookup(argv[1], &s_pio)) {
            g->puts("pio [off|tape|term]\r\n");
            return 0;
        }
        g->printf("88-PIO: %s\r\n", k_pio_names[s_pio]);
    } else if (streq(cmd, "sw")) {
        if (argc > 1) {
            if (parse_hex(argv[1], &v) || v > 0xFF) {
                g->puts("sw [HEX]\r\n");
                return 0;
            }
            s_switches = (uint8_t)v;
        }
        g->printf("sense switches %02X\r\n", s_switches);
    } else if (streq(cmd, "speed")) {
        if (argc > 1) {
            if (parse_dec(argv[1], &v) || v > 1000) {
                g->puts("speed [MHZ]\r\n");
                return 0;
            }
            s_mhz = v;
        }
        if (s_mhz)
            g->printf("%u MHz\r\n", s_mhz);
        else
            g->puts("as fast as it goes\r\n");
    } else if (streq(cmd, "upper")) {
        if (argc < 2 || on_off(argv[1], &s_upper))
            g->puts("upper on|off\r\n");
    } else if (streq(cmd, "bs")) {
        if (argc < 2 || on_off(argv[1], &s_bs_del))
            g->puts("bs on|off\r\n");
    } else if (streq(cmd, "quit") || streq(cmd, "bye")) {
        s_quit = 1;
        return 1;
    } else {
        g->printf("%s? 'help' lists the commands\r\n", cmd);
    }
    return 0;
}

static void menu(void)
{
    char line[LINE_MAX];
    char *argv[MAX_ARGS];

    s_menu_req = 0;
    g->puts("\r\n");
    show_regs();
    for (;;) {
        int n;

        g->puts("altair> ");
        n = read_line(line, sizeof line);
        if (n < 0)
            break;
        if (menu_command(split(line, argv, MAX_ARGS), argv))
            break;
    }
    if (s_cpu.halted && !s_quit)
        g->puts("the CPU is halted: 'go ADDR' or 'reset' to restart it\r\n");
}

/* --------------------------------------------------------- the loop */
static void run(void)
{
    uint32_t t0 = g->ticks_ms();
    uint32_t done = 0;                      /* clock states since t0 */

    while (!s_quit) {
        uint32_t used;

        if (s_cpu.halted) {
            menu();
            t0 = g->ticks_ms();
            done = 0;
            continue;
        }
        used = i8080_run(&s_cpu, SLICE);
        if (s_cpu.halted) {
            /* Nothing raises an interrupt, so a HLT is where a real
             * front panel's lights would stop. */
            g->printf("\r\naltair: HLT at %04X\r\n", (uint16_t)(s_cpu.pc - 1));
            menu();
            t0 = g->ticks_ms();
            done = 0;
            continue;
        }

        if (s_mhz) {
            uint32_t due, now;

            done += used;
            due = done / (s_mhz * 1000u);
            now = g->ticks_ms() - t0;
            if (due > now)
                g->delay_ms(due - now);
            else
                g->yield();
            if (done > 0x40000000u) {
                t0 = g->ticks_ms();
                done = 0;
            }
        } else {
            g->yield();
        }

        if (s_menu_req) {
            menu();
            t0 = g->ticks_ms();
            done = 0;
        }
    }
}

/* ------------------------------------------------------------- main */
static void usage(void)
{
    g->puts("usage: altair [IMAGE] [--at ADDR] [--go ADDR] [--ram KB] "
            "[--sw HEX] [--mhz N]\r\n"
            "              [--prom FILE[@ADDR]] [--tape FILE] "
            "[--boot 4k32|4k40|8k|ext|disk]\r\n"
            "              [--pio off|tape|term]\r\n");
}

static int file_exists(const char *path)
{
    int fd = g->open(path, FREYA_O_RDONLY);

    if (fd < 0)
        return 0;
    g->close(fd);
    return 1;
}

int app_main(const freya_api_t *api, int argc, char **argv)
{
    const char *image = NULL;
    const char *prom[4];
    uint16_t prom_at[4];
    const char *tape = NULL;
    int nprom = 0, have_go = 0, loaded_basic = 0;
    int32_t boot_pc = -1;
    uint16_t at = 0, go = 0, boot_hl = DEFAULT_BOOT;
    uint32_t ram_kb = MEM_BASE_KB, v;
    unsigned got_kb;

    g = api;
    s_quit = 0;
    s_mhz = 0;
    s_switches = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;

        if (a[0] != '-' || a[1] != '-') {
            if (image) {
                usage();
                return FREYA_EXIT_USAGE;
            }
            image = a;
            continue;
        }
        if (!val) {
            usage();
            return FREYA_EXIT_USAGE;
        }
        i++;
        if (streq(a, "--at")) {
            if (parse_addr(val, &at))
                goto bad;
        } else if (streq(a, "--go")) {
            if (parse_addr(val, &go))
                goto bad;
            have_go = 1;
        } else if (streq(a, "--ram")) {
            if (parse_dec(val, &ram_kb) || ram_kb < 1 || ram_kb > MEM_MAX_KB)
                goto bad;
        } else if (streq(a, "--sw")) {
            if (parse_hex(val, &v) || v > 0xFF)
                goto bad;
            s_switches = (uint8_t)v;
        } else if (streq(a, "--mhz")) {
            if (parse_dec(val, &v) || v > 1000)
                goto bad;
            s_mhz = v;
        } else if (streq(a, "--prom")) {
            static char names[4][64];
            int k = 0;

            if (nprom == 4)
                goto bad;
            while (val[k] && val[k] != '@' && k < 63) {
                names[nprom][k] = val[k];
                k++;
            }
            names[nprom][k] = '\0';
            prom_at[nprom] = TURMON_ADDR;
            if (val[k] == '@' && parse_addr(val + k + 1, &prom_at[nprom]))
                goto bad;
            prom[nprom] = names[nprom];
            nprom++;
        } else if (streq(a, "--tape")) {
            tape = val;
        } else if (streq(a, "--boot")) {
            if (boot_lookup(val, &boot_hl))
                goto bad;
        } else if (streq(a, "--pio")) {
            if (pio_lookup(val, &s_pio))
                goto bad;
        } else {
            goto bad;
        }
    }

    i8080_init();
    got_kb = mem_init(ram_kb);
    if (got_kb == 0) {
        mem_release();
        return FREYA_EXIT_FAIL;
    }
    g->printf("altair: 8800b Turnkey, %u KiB RAM at 0000, Turnkey SRAM at "
              "F800, PROM at FC00\r\n", got_kb);
    if (got_kb < ram_kb)
        g->printf("altair: the heap gave %u of the %u KiB asked for\r\n",
                  got_kb, (unsigned)ram_kb);

    /* The PROMs are part of the machine, so they are fitted whatever
     * image is loaded into RAM. */
    load_prom(DEFAULT_TURMON, TURMON_ADDR, 1);
    load_prom(DEFAULT_MBL, MBL_ADDR, 1);
    for (int i = 0; i < nprom; i++)
        load_prom(prom[i], prom_at[i], 0);

    /* A tape is read before the machine starts, and the CPU then starts
     * in the loader it brought; reset still goes to 0000h, where BASIC
     * will be once the tape is in. */
    if (image && is_tape_name(image)) {
        if (boot_tape(image, boot_hl)) {
            mem_release();
            return FREYA_EXIT_FAIL;
        }
        boot_pc = s_cpu.pc;
        s_start = have_go ? go : 0;
    } else if (image) {
        if (load_report(image, at)) {
            mem_release();
            return FREYA_EXIT_FAIL;
        }
        s_start = have_go ? go : at;
    } else {
        if (file_exists(DEFAULT_BASIC)) {
            loaded_basic = load_report(DEFAULT_BASIC, 0) == 0;
        } else if (file_exists(DEFAULT_TAPE) &&
                   boot_tape(DEFAULT_TAPE, boot_hl) == 0) {
            loaded_basic = 1;
            boot_pc = s_cpu.pc;
        }
        if (have_go)
            s_start = go;
        else if (mem_prom_loaded(TURMON_ADDR))
            s_start = TURMON_ADDR;
        else
            s_start = 0;
        if (!loaded_basic && !mem_prom_loaded(s_start)) {
            g->puts("altair: no " DEFAULT_BASIC ", " DEFAULT_TAPE " or "
                    DEFAULT_TURMON " - use 'load' or 'boot' in the menu\r\n");
        }
    }

    if (tape) {
        if (boot_pc >= 0)
            g->puts("altair: the tape reader already holds a tape\r\n");
        else if (tape_attach(tape) < 0)
            g->printf("altair: cannot open tape %s\r\n", tape);
    }

    if (boot_pc < 0 || have_go)
        boot_pc = s_start;
    term_open();
    g->printf("altair: %s, starting at %04X.  Ctrl-] for the menu%s\r\n",
              s_mhz ? "throttled" : "full speed", (unsigned)boot_pc,
              s_raw ? "" : ", Ctrl-C stops");
    reset((uint16_t)boot_pc);
    if (!image && !loaded_basic && !mem_prom_loaded(s_start))
        menu();
    run();

    term_close();
    tape_detach();
    mem_release();
    g->printf("altair: %u clock states since the last reset\r\n",
              (unsigned)s_cpu.cycles);
    return FREYA_EXIT_OK;

bad:
    usage();
    return FREYA_EXIT_USAGE;
}
