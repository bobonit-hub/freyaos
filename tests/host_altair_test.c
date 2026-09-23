/*
 * Freya - host test for the Altair sample.
 *
 * samples/altair/main.c is compiled unchanged for the host against a
 * service table whose console is a string going in and a buffer coming
 * out.  The 8080 is checked instruction by instruction with short
 * programs assembled by hand below - every flag rule that differs
 * between the 8080 and the chips people remember it as - and then the
 * memory map, the serial ports and the file formats around it.
 *
 * Two more runs need files that are not part of Freya:
 *
 *   ALTAIR_TESTS=dir    runs the CP/M CPU exercisers found there
 *                       (TST8080.COM, 8080PRE.COM, CPUTEST.COM,
 *                       8080EXM.COM) under a two-call BDOS
 *   ALTAIR_BASIC=file   boots that Altair BASIC image, answers its
 *                       start-up questions, runs a line, and breaks
 *                       a loop with Ctrl-C
 *
 * Either is skipped when its variable is not set.  With -i the same
 * binary is the emulator on this terminal, Ctrl-] for its menu:
 *
 *     build/tests/hostaltair -i xbasic.bin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <poll.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "freya_api.h"

static char        s_out[1 << 16];
static int         s_outn;
static int         s_live;
static const char *s_in;            /* what the terminal will type */
static int         s_in_len, s_in_pos;
static int         s_raw_calls;
static int         s_hold;          /* scripted reads that time out first */

static void h_putc(char c)
{
    if (s_live) {
        putchar(c);
        return;
    }
    /* Teletype software pads with NULs, which would end the string. */
    if (c != '\r' && c != '\0' && s_outn < (int)sizeof s_out - 1)
        s_out[s_outn++] = c;
}

static void h_puts(const char *s)
{
    while (*s)
        h_putc(*s++);
}

static int h_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    h_puts(buf);
    return n;
}

static int live_key(int wait_ms)
{
    struct pollfd p = { .fd = 0, .events = POLLIN };
    unsigned char c;

    if (poll(&p, 1, wait_ms) <= 0 || read(0, &c, 1) != 1)
        return -1;
    return c;
}

static int h_kbhit(void)
{
    struct pollfd p = { .fd = 0, .events = POLLIN };

    if (s_live)
        return poll(&p, 1, 0) > 0;
    if (s_hold > 0)
        return 0;
    return s_in_pos < s_in_len;
}

static int h_getc_timeout(uint32_t ms)
{
    if (s_live)
        return live_key((int)ms);
    if (s_hold > 0) {
        s_hold--;
        return -1;
    }
    return s_in_pos < s_in_len ? (unsigned char)s_in[s_in_pos++] : -1;
}

static int h_getc(void)
{
    return s_live ? live_key(-1) : h_getc_timeout(0);
}

static uint32_t h_ticks(void)
{
    struct timespec t;

    if (!s_live)
        return 0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void  h_delay(uint32_t ms)       { if (s_live) usleep(ms * 1000); }
static void *h_malloc(uint32_t n)       { return malloc(n); }
static void  h_free(void *p)            { free(p); }
static int   h_should_stop(void)        { return 0; }
static void  h_yield(void)              { }
static void  h_exit(int code)           { exit(code); }
static int   h_raw(int on)              { s_raw_calls++; (void)on; return 0; }

static FILE *s_files[4];

static int h_open(const char *path, int flags)
{
    const char *mode = "rb";

    if (flags & FREYA_O_WRONLY)
        mode = (flags & FREYA_O_TRUNC) ? "wb" : "ab";
    for (int i = 0; i < 4; i++) {
        if (!s_files[i]) {
            s_files[i] = fopen(path, mode);
            return s_files[i] ? i : -1;
        }
    }
    return -1;
}

static int h_read(int fd, void *buf, int len)
{
    if (fd < 0 || fd >= 4 || !s_files[fd])
        return -1;
    return (int)fread(buf, 1, (size_t)len, s_files[fd]);
}

static int h_write(int fd, const void *buf, int len)
{
    if (fd < 0 || fd >= 4 || !s_files[fd])
        return -1;
    return (int)fwrite(buf, 1, (size_t)len, s_files[fd]);
}

static int h_close(int fd)
{
    if (fd < 0 || fd >= 4 || !s_files[fd])
        return -1;
    fclose(s_files[fd]);
    s_files[fd] = NULL;
    return 0;
}

static const freya_api_t k_api = {
    .size         = sizeof(freya_api_t),
    .version      = FREYA_ABI_VERSION,
    .putc         = h_putc,
    .puts         = h_puts,
    .printf       = h_printf,
    .getc         = h_getc,
    .getc_timeout = h_getc_timeout,
    .kbhit        = h_kbhit,
    .malloc       = h_malloc,
    .free         = h_free,
    .ticks_ms     = h_ticks,
    .delay_ms     = h_delay,
    .should_stop  = h_should_stop,
    .yield        = h_yield,
    .exit         = h_exit,
    .open         = h_open,
    .close        = h_close,
    .read         = h_read,
    .write        = h_write,
    .console_raw  = h_raw,
};

#include "../samples/altair/main.c"

static int s_checks, s_fails;

static void check(const char *what, long want, long got)
{
    s_checks++;
    if (want == got) {
        printf("  ok    %s\n", what);
    } else {
        printf("  FAIL  %s: expected %lX, got %lX\n", what, want, got);
        s_fails++;
    }
}

static void type(const char *keys, int len)
{
    s_hold = 0;
    s_in = keys;
    s_in_len = len;
    s_in_pos = 0;
}

/* Puts a program at 0100h with SP at 8000h and runs it to its HLT.
 * Returns the clock states it took. */
static uint32_t exec(const uint8_t *code, int len)
{
    uint32_t used = 0;

    mem_init(MEM_BASE_KB);
    for (int i = 0; i < len; i++)
        mem_poke((uint16_t)(0x100 + i), code[i]);
    memset(&s_cpu, 0, sizeof s_cpu);
    reset(0x100);
    s_cpu.sp = 0x8000;
    while (!s_cpu.halted && used < 1000000)
        used += i8080_run(&s_cpu, 1000);
    return s_cpu.cycles;
}

#define EXEC(...) do {                                  \
        static const uint8_t prog[] = { __VA_ARGS__ };  \
        exec(prog, (int)sizeof prog);                   \
    } while (0)

#define A   s_cpu.r[R_A]
#define F   s_cpu.r[R_F]
#define HLv ((s_cpu.r[R_H] << 8) | s_cpu.r[R_L])

static void test_cpu(void)
{
    printf("\n--- arithmetic and flags ---\n");
    EXEC(0x3E, 0x8F, 0x06, 0x81, 0x80, 0x76);           /* MVI A,8F MVI B,81 ADD B */
    check("ADD 8F+81 is 10", 0x10, A);
    check("with carry and auxiliary carry", F_AC | F_CY | F_ONE, F);

    EXEC(0x3E, 0x3E, 0xD6, 0x3E, 0x76);                 /* SUI 3E from 3E */
    check("SUI of equal values is zero", 0, A);
    check("Z, P and AC set, no borrow", F_Z | F_AC | F_P | F_ONE, F);

    EXEC(0x3E, 0x00, 0xD6, 0x01, 0x76);                 /* 00 - 01 */
    check("00 - 01 is FF", 0xFF, A);
    check("S, P and a borrow, no AC", F_S | F_P | F_CY | F_ONE, F);

    EXEC(0x37, 0x3E, 0x10, 0xDE, 0x05, 0x76);           /* STC, 10 SBI 05 */
    check("SBI with borrow in: 10 - 05 - 1 is 0A", 0x0A, A);
    check("P, no AC, no borrow out", F_P | F_ONE, F);

    EXEC(0x37, 0x3E, 0x10, 0xCE, 0x05, 0x76);           /* STC, 10 ACI 05 */
    check("ACI with carry in: 10 + 05 + 1 is 16", 0x16, A);

    EXEC(0x3E, 0x05, 0xFE, 0x06, 0x76);                 /* CPI 6 with A=5 */
    check("CPI leaves A alone", 0x05, A);
    check("and sets carry when A is lower", F_CY, F & F_CY);

    EXEC(0x3E, 0x08, 0x06, 0x00, 0xA0, 0x76);           /* 08 ANA 00 */
    check("ANA is zero", 0, A);
    check("and takes AC from bit 3 of either operand", F_Z | F_AC | F_P | F_ONE, F);

    EXEC(0x37, 0x3E, 0xF0, 0xEE, 0x0F, 0x76);           /* STC, F0 XRI 0F */
    check("XRI F0 ^ 0F is FF", 0xFF, A);
    check("and clears carry and AC", F_S | F_P | F_ONE, F);

    printf("\n--- increment, decrement, DAA ---\n");
    EXEC(0x37, 0x3E, 0x0F, 0x3C, 0x76);                 /* STC, 0F INR A */
    check("INR 0F is 10", 0x10, A);
    check("with AC, and carry left as it was", F_AC | F_CY | F_ONE, F);

    EXEC(0xAF, 0x3E, 0x10, 0x3D, 0x76);                 /* XRA A, 10 DCR A */
    check("DCR 10 is 0F", 0x0F, A);
    check("and clears AC when the low nibble wraps", F_P | F_ONE, F);

    EXEC(0xAF, 0x3E, 0x11, 0x3D, 0x76);                 /* 11 DCR A */
    check("DCR 11 sets AC", F_AC, F & F_AC);

    EXEC(0x3E, 0x9B, 0x27, 0x76);                       /* 9B DAA */
    check("DAA of 9B is 01", 0x01, A);
    check("with carry and AC, as Intel's manual has it", F_AC | F_CY | F_ONE, F);

    EXEC(0x3E, 0x38, 0xC6, 0x45, 0x27, 0x76);           /* 38 + 45, DAA */
    check("BCD 38 + 45 is 83", 0x83, A);
    check("with no carry", 0, F & F_CY);

    EXEC(0x3E, 0x99, 0xC6, 0x01, 0x27, 0x76);           /* 99 + 01, DAA */
    check("BCD 99 + 01 is 00", 0x00, A);
    check("with a carry out", F_CY, F & F_CY);

    printf("\n--- rotates, DAD, CMA, CMC ---\n");
    EXEC(0x3E, 0x81, 0x07, 0x76);
    check("RLC 81 is 03", 0x03, A);
    check("and carries bit 7", F_CY, F & F_CY);
    EXEC(0x3E, 0x81, 0x0F, 0x76);
    check("RRC 81 is C0", 0xC0, A);
    EXEC(0x3E, 0x80, 0xB7, 0x17, 0x76);                 /* ORA A clears CY, RAL */
    check("RAL 80 with no carry is 00", 0x00, A);
    check("and carries bit 7 out", F_CY, F & F_CY);
    EXEC(0x37, 0x3E, 0x02, 0x1F, 0x76);                 /* STC, RAR */
    check("RAR 02 with carry is 81", 0x81, A);
    check("and carries bit 0 out", 0, F & F_CY);
    EXEC(0x21, 0xFF, 0xFF, 0x11, 0x01, 0x00, 0x19, 0x76);   /* FFFF DAD 0001 */
    check("DAD FFFF + 0001 is 0000", 0, HLv);
    check("with a carry", F_CY, F & F_CY);
    EXEC(0x3E, 0x5A, 0x2F, 0x37, 0x3F, 0x76);           /* CMA, STC, CMC */
    check("CMA 5A is A5", 0xA5, A);
    check("CMC after STC clears carry", 0, F & F_CY);

    printf("\n--- the stack ---\n");
    EXEC(0x21, 0xFF, 0xFF, 0xE5, 0xF1, 0xF5, 0xC1, 0x76);   /* PUSH H POP PSW PUSH PSW POP B */
    check("PUSH PSW stores A", 0xFF, s_cpu.r[R_B]);
    check("and a flag byte with bit 1 set, bits 3 and 5 clear",
          0xD7, s_cpu.r[R_C]);
    EXEC(0x21, 0x34, 0x12, 0xE5, 0x21, 0x78, 0x56, 0xE3, 0x76);  /* XTHL */
    check("XTHL takes the top of the stack", 0x1234, HLv);
    check("and leaves HL there", 0x5678,
          mem_rd(s_cpu.sp) | mem_rd((uint16_t)(s_cpu.sp + 1)) << 8);
    EXEC(0x11, 0x22, 0x11, 0x21, 0x44, 0x33, 0xEB, 0x76);   /* XCHG */
    check("XCHG swaps DE and HL", 0x1122, HLv);

    printf("\n--- jumps, calls, timing ---\n");
    {
        static const uint8_t prog[] = {
            0xAF,                       /* 0100 XRA A      Z set        */
            0xC4, 0x00, 0x02,           /* 0101 CNZ 0200   not taken    */
            0xCC, 0x00, 0x02,           /* 0104 CZ  0200   taken        */
            0x76,                       /* 0107 HLT                     */
        };
        uint8_t code[0x110];

        memset(code, 0, sizeof code);
        memcpy(code, prog, sizeof prog);
        code[0x100] = 0x06;             /* 0200 MVI B,42                */
        code[0x101] = 0x42;
        code[0x102] = 0xC8;             /* 0202 RZ         taken        */
        code[0x103] = 0x76;
        check("a taken conditional call and return, and one not taken, "
              "take 4+11+17+7+11+7 states", 57, exec(code, sizeof code));
        check("the call arrived", 0x42, s_cpu.r[R_B]);
        check("and the return came back to the HLT after it",
              0x108, s_cpu.pc);
        check("with the stack where it started", 0x8000, s_cpu.sp);
    }
    EXEC(0x3E, 0x01, 0x0F, 0xDA, 0x08, 0x01, 0x76, 0x76, 0x3E, 0x77, 0x76);
    check("JC is taken when RRC carried out", 0x77, A);
    EXEC(0x3E, 0x03, 0xB7, 0xE2, 0x09, 0x01, 0x3E, 0x55, 0x76, 0x76);
    check("JPO is not taken when parity is even", 0x55, A);
    {
        mem_init(MEM_BASE_KB);
        mem_poke(0x0038, 0x76);                         /* HLT at RST 7 */
        mem_poke(0x0100, 0x31);                         /* LXI SP,9000 */
        mem_poke(0x0101, 0x00);
        mem_poke(0x0102, 0x90);
        mem_poke(0x0103, 0xFF);                         /* RST 7       */
        memset(&s_cpu, 0, sizeof s_cpu);
        reset(0x100);
        while (!s_cpu.halted)
            i8080_run(&s_cpu, 1000);
    }
    check("RST 7 goes to 0038", 0x39, s_cpu.pc);
    check("and pushes the address after it", 0x0104,
          mem_rd(0x8FFE) | mem_rd(0x8FFF) << 8);

    printf("\n--- the duplicate opcodes ---\n");
    EXEC(0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x3E, 0x01, 0x76);
    check("08h..38h are NOP", 0x01, A);
    EXEC(0xCB, 0x05, 0x01, 0x76, 0x76, 0x3E, 0x02, 0x76);
    check("CBh is JMP", 0x02, A);
    {
        uint8_t code[0x110];

        memset(code, 0, sizeof code);
        code[0] = 0xDD; code[1] = 0x00; code[2] = 0x02;     /* CALL 0200 */
        code[3] = 0xED; code[4] = 0x00; code[5] = 0x02;
        code[6] = 0xFD; code[7] = 0x00; code[8] = 0x02;
        code[9] = 0x76;
        code[0x100] = 0x04;                                 /* INR B */
        code[0x101] = 0xD9;                                 /* RET   */
        exec(code, sizeof code);
        check("DDh, EDh and FDh are CALL and D9h is RET", 3, s_cpu.r[R_B]);
    }
}

static void test_memory(void)
{
    printf("\n--- the memory map ---\n");
    check("48 KiB comes from .bss", 48, mem_init(48));
    mem_wr(0xBFFF, 0x12);
    check("the top byte of RAM keeps what is written", 0x12, mem_rd(0xBFFF));
    mem_wr(0xC000, 0x34);
    check("the page above reads FFh and remembers nothing", 0xFF, mem_rd(0xC000));
    mem_wr(0xF800, 0x56);
    check("the Turnkey SRAM at F800 is RAM", 0x56, mem_rd(0xF800));
    mem_wr(0xFBFF, 0x57);
    check("up to FBFF", 0x57, mem_rd(0xFBFF));
    check("an empty PROM socket reads FFh", 0xFF, mem_rd(0xFD00));
    check("and holds nothing", 0, mem_prom_loaded(0xFD00));
    mem_wr(0xFD00, 0x00);
    check("the 8080 cannot write a PROM", 0xFF, mem_rd(0xFD00));
    mem_poke(0xFD00, 0xC3);
    check("a load can", 0xC3, mem_rd(0xFD00));
    check("and the socket then holds something", 1, mem_prom_loaded(0xFD00));
    check("which says nothing of the socket beside it", 0, mem_prom_loaded(0xFE00));
    check("nor of RAM", 0, mem_prom_loaded(0x0000));
    check("RAM past 48 KiB comes from the heap", 62, mem_init(62));
    mem_wr(0xF7FF, 0x9A);
    check("up to F7FF", 0x9A, mem_rd(0xF7FF));
    check("and a smaller machine gives it back", 16, mem_init(16));
    mem_wr(0x4000, 0x11);
    check("so 4000 is empty again", 0xFF, mem_rd(0x4000));
}

static void test_io(void)
{
    printf("\n--- the serial ports and sense switches ---\n");
    s_outn = 0;
    s_pending = -1;
    s_upper = 1;
    s_bs_del = 1;
    type("a\b\x1d", 3);
    check("2SIO status with a key waiting: RDRF and TDRE", 0x03, io_in(0x10));
    check("the key is folded to uppercase", 'A', io_in(0x11));
    check("SIO status: input ready is a 0 bit", 0x00, io_in(0x00));
    check("Backspace arrives as DEL", 0x7F, io_in(0x01));
    check("Ctrl-] is not a key", 0x02, io_in(0x10));
    check("it asks for the menu instead", 1, s_menu_req);
    check("SIO status with nothing waiting", 0x01, io_in(0x00));
    s_menu_req = 0;
    io_out(0x11, 'O');
    io_out(0x01, 'K' | 0x80);
    s_out[s_outn] = '\0';
    check("both terminal ports print, without bit 7", 1, strcmp(s_out, "OK") == 0);

    check("the 88-PIO starts on the tape reader", PIO_TAPE, s_pio);
    check("with no tape, only output ready", 0x01, io_in(0x04));
    s_pio = PIO_TERM;
    type("p", 1);
    check("88-PIO on the terminal: input ready is a 1 bit", 0x03, io_in(0x04));
    check("and the key is folded as on the serial ports", 'P', io_in(0x05));
    check("reading it clears the input flag", 0x01, io_in(0x04));
    s_outn = 0;
    io_out(0x04, 0x03);
    io_out(0x05, '!');
    s_out[s_outn] = '\0';
    check("the data port prints, a control write does not", 1,
          strcmp(s_out, "!") == 0);
    s_pio = PIO_OFF;
    check("with no 88-PIO fitted its status reads FFh", 0xFF, io_in(0x04));
    check("and its data port too", 0xFF, io_in(0x05));
    s_outn = 0;
    io_out(0x05, '?');
    check("and nothing is printed", 0, s_outn);
    check("'pio' knows its three settings", 0,
          pio_lookup("off", &s_pio) | pio_lookup("term", &s_pio) |
          pio_lookup("tape", &s_pio));
    check("and nothing else", -1, pio_lookup("printer", &s_pio));
    check("leaving the 88-PIO on the tape reader", PIO_TAPE, s_pio);

    s_switches = 0x5A;
    check("port FF reads the sense switches", 0x5A, io_in(0xFF));
    check("an absent port reads FFh", 0xFF, io_in(0x42));

    printf("\n--- files: images, Intel HEX, tape ---\n");
    {
        const char *hex = "build/tests/altair.hex";
        const char *bin = "build/tests/altair.bin";
        FILE *f = fopen(hex, "w");

        fputs(":0300300002337A1E\r\n"
              ":02FC0000C3FD42\n"
              ":00000001FF\n", f);
        fclose(f);
        mem_init(48);
        check("a HEX file loads its data bytes", 5, load_file(hex, 0));
        check("at the addresses it names", 0x7A, mem_rd(0x0032));
        check("including into a PROM socket", 0xFD, mem_rd(0xFC01));

        f = fopen(hex, "w");
        fputs(":0300300002337A1E\n:03003000023379FF\n", f);
        fclose(f);
        check("a bad checksum is reported by its line", -2 - 2, load_file(hex, 0));

        for (int i = 0; i < 300; i++)
            mem_wr((uint16_t)(0x1000 + i), (uint8_t)(i * 7));
        check("save writes the range", 300, save_bin(bin, 0x1000, 300));
        check("an image loads at the address given", 300, load_file(bin, 0x2000));
        check("byte for byte", (uint8_t)(299 * 7), mem_rd(0x2000 + 299));

        check("a tape can be attached", 0, tape_attach(bin));
        check("the 2SIO B port then has a byte", 0x03, io_in(0x12));
        check("and it is the first of the file", 0, io_in(0x13));
        check("the ACR port reads the same tape", 7, io_in(0x07));
        check("and the 88-PIO has a byte of it", 0x03, io_in(0x04));
        check("the next one", 14, io_in(0x05));
        s_outn = 0;
        io_out(0x05, 'x');
        check("a PIO on the reader prints nothing", 0, s_outn);
        for (int i = 3; i < 300; i++)
            io_in(0x13);
        check("the end of the tape is no data", 0x01, io_in(0x06));
        tape_detach();

        f = fopen(bin, "wb");
        fputs("\xc2\xc2\x11\x22", f);           /* leader, then two bytes */
        fclose(f);
        s_ahead_head = s_ahead_tail = 0;
        check("a tape in the teletype's reader", 0, tape_attach(bin));
        s_tape_tty = 1;
        type("k", 1);
        check("is read through the terminal port", 0xC2, io_in(0x11));
        io_in(0x11);
        check("byte by byte", 0x11, io_in(0x11));
        check("to its end", 0x22, io_in(0x11));
        check("and a key typed meanwhile comes after it", 'K', io_in(0x11));
        check("with the reader emptied", 0, s_tape_tty);

        f = fopen(bin, "wb");
        /* Leader 03, then AA for 0102; 02 equals L and is skipped, BB
         * for 0101; 01 equals L and is skipped, CC for 0100. */
        fputs("\x03\x03\x03\xaa\x02\xbb\x01\xcc", f);
        fclose(f);
        mem_init(48);
        tape_attach(bin);
        check("the bootstrap starts the loader at H:00", 0x0100, tape_boot(0x0103));
        check("stores the tape backwards after the leader", 0xAA, mem_rd(0x0102));
        check("skipping a byte equal to L, as the keyed-in loader does",
              0xBB, mem_rd(0x0101));
        check("down to H:00", 0xCC, mem_rd(0x0100));
        check("and leaves the rest in the teletype's reader", 1, s_tape_tty);
        tape_detach();
        mem_init(16);
        s_outn = 0;
        check("a loader past the RAM is refused", -1, boot_tape(bin, 0x7EC2));
        s_out[s_outn] = '\0';
        check("and it names the 16 KiB", 1,
              strstr(s_out, "loader at 7EC2 is past the 16 KiB of RAM") != NULL);
        tape_detach();
        remove(hex);
        remove(bin);
    }
}

/* ------------------------------------------------ CP/M exercisers */
/* Runs one exerciser.  Returns -1 if it is not there, 0 if it printed
 * the line it ends with when everything passed, 1 otherwise. */
static int cpm_run(const char *path, const char *success)
{
    uint64_t total = 0;

    mem_init(MEM_MAX_KB);
    if (load_bin(path, 0x100) <= 0)
        return -1;
    mem_poke(0x0000, 0x76);             /* warm boot: stop            */
    mem_poke(0x0005, 0x76);             /* BDOS: stop, served below   */
    mem_poke(0x0006, 0x00);             /* top of the TPA, for LXI SP */
    mem_poke(0x0007, 0xF0);
    memset(&s_cpu, 0, sizeof s_cpu);
    reset(0x100);
    s_cpu.sp = 0xF000;                  /* where the CCP would leave it */
    s_outn = 0;
    for (;;) {
        total += i8080_run(&s_cpu, 1u << 20);
        if (!s_cpu.halted)
            continue;
        if (s_cpu.pc != 0x0006)
            break;
        if (s_cpu.r[R_C] == 2) {
            h_putc((char)s_cpu.r[R_E]);
        } else if (s_cpu.r[R_C] == 9) {
            uint16_t a = (uint16_t)(s_cpu.r[R_D] << 8 | s_cpu.r[R_E]);
            while (mem_rd(a) != '$')
                h_putc((char)mem_rd(a++));
        }
        s_cpu.halted = 0;
        s_cpu.pc = pop(&s_cpu);
    }
    s_out[s_outn] = '\0';
    printf("%s\n  --    %llu clock states\n", s_out, (unsigned long long)total);
    if (strstr(s_out, "ERROR") || strstr(s_out, "FAIL"))
        return 1;
    return strstr(s_out, success) ? 0 : 1;
}

static void test_exercisers(const char *dir)
{
    static const char *const names[][2] = {
        { "TST8080.COM", "CPU IS OPERATIONAL" },
        { "8080PRE.COM", "8080 Preliminary tests complete" },
        { "CPUTEST.COM", "CPU TESTS OK" },
        { "8080EXM.COM", "Tests complete" },
    };

    printf("\n--- CPU exercisers in %s ---\n", dir);
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        char path[512];
        int rc;

        snprintf(path, sizeof path, "%s/%s", dir, names[i][0]);
        rc = cpm_run(path, names[i][1]);
        if (rc < 0) {
            printf("  --    %s not there\n", names[i][0]);
            continue;
        }
        s_checks++;
        if (rc == 0) {
            printf("  ok    %s\n", names[i][0]);
        } else {
            printf("  FAIL  %s\n", names[i][0]);
            s_fails++;
        }
    }
}

/* ------------------------------------------------------ BASIC */
/*
 * Types each line only once BASIC has gone quiet waiting for it, the
 * way a person does: Altair BASIC reads the keyboard while it prints, to
 * catch a Ctrl-C, and drops whatever else it finds there, so a line
 * typed ahead would lose its first key.
 */
static void basic_session(const char *const *lines, uint32_t max_states)
{
    uint32_t used = 0;
    int quiet = 0, last = -1;

    while (used < max_states && !s_cpu.halted) {
        used += i8080_run(&s_cpu, 200000);
        if (s_outn != last) {
            last = s_outn;
            quiet = 0;
            continue;
        }
        if (s_in_pos < s_in_len || ++quiet < 3)
            continue;
        if (!*lines) {
            /* Quiet with nothing left to type: done once BASIC is back
             * at its prompt, otherwise a program is still running. */
            if (s_outn >= 3 && memcmp(s_out + s_outn - 3, "OK\n", 3) == 0)
                break;
            continue;
        }
        type(*lines, (int)strlen(*lines));
        lines++;
    }
}

static void basic_checks(void)
{
    /* Extended BASIC asks MEMORY SIZE, LINEPRINTER and whether to keep
     * the trigonometry; 8K and 4K ask TERMINAL WIDTH and which functions
     * to keep.  C is Extended BASIC's Centronics printer and Y keeps a
     * function.  An answer nobody asked for lands at OK as an error and
     * harms nothing. */
    static const char *const keys[] = {
        "\r", "C\r", "Y\r", "Y\r", "Y\r",
        "PRINT 2+2\r",
        "FOR I=1 TO 3000:NEXT I:PRINT \"DO\";\"NE\"\r",
        NULL
    };
    static const char *const brk[] = {
        "FOR I=1 TO 1E9:NEXT\r", "\x03", NULL
    };

    type("", 0);
    basic_session(keys, 400000000u);
    s_out[s_outn] = '\0';
    printf("%s\n", s_out);
    s_checks++;
    if (strstr(s_out, "\n 4 \n") && strstr(s_out, "\nDONE\n")) {
        printf("  ok    BASIC prints 2+2 and runs a FOR loop\n");
    } else {
        printf("  FAIL  BASIC did not answer as expected\n");
        s_fails++;
    }

    s_outn = 0;
    basic_session(brk, 400000000u);
    s_out[s_outn] = '\0';
    printf("%s\n", s_out);
    s_checks++;
    if (strstr(s_out, "BREAK")) {
        printf("  ok    Ctrl-C breaks a running BASIC program\n");
    } else {
        printf("  FAIL  Ctrl-C did not break the loop\n");
        s_fails++;
    }
}

/*
 * A tape is read the way the emulator reads it, and what it left in
 * memory is then saved as the image the README suggests keeping on the
 * card, so that the next boot needs no tape.  Both are put through the
 * same session.
 */
static void test_basic(const char *path)
{
    const char *image = "build/tests/xbasic.bin";

    printf("\n--- Altair BASIC from %s ---\n", path);
    mem_init(MEM_BASE_KB);
    memset(&s_cpu, 0, sizeof s_cpu);
    s_switches = 0;
    s_outn = 0;
    type("", 0);
    if (is_tape_name(path)) {
        if (boot_tape(path, DEFAULT_BOOT)) {
            printf("  --    cannot boot it\n");
            return;
        }
        while (s_tape_tty && !s_cpu.halted)
            i8080_run(&s_cpu, 100000);
        check("the tape is read to its end", 0, s_tape_tty);
        check("and BASIC is saved as a memory image", 0x4000,
              save_bin(image, 0, 0x4000));
    } else if (load_bin(path, 0) <= 0) {
        printf("  --    cannot read it\n");
        return;
    } else {
        reset(0);
    }
    basic_checks();

    if (is_tape_name(path)) {
        printf("\n--- Altair BASIC from the image saved after the tape ---\n");
        mem_init(MEM_BASE_KB);
        memset(&s_cpu, 0, sizeof s_cpu);
        s_outn = 0;
        check("the image loads at 0000", 0x4000, load_bin(image, 0));
        reset(0);
        basic_checks();

        /* BASIC's terminal is chosen by switches A15-A12, and 5 is the
         * 88-PIO: this is BASIC's own PIO code polling both status bits. */
        printf("\n--- Altair BASIC with its terminal on the 88-PIO ---\n");
        mem_init(MEM_BASE_KB);
        memset(&s_cpu, 0, sizeof s_cpu);
        s_outn = 0;
        load_bin(image, 0);
        s_switches = 0x50;
        s_pio = PIO_TERM;
        reset(0);
        basic_checks();
        s_switches = 0;
        s_pio = PIO_TAPE;
        remove(image);
    }
}

/*
 * The Turnkey's own way in: a loader PROM in the socket at FE00h, the
 * tape on a reader port, and the sense switches telling the loader
 * which.  MBL and MBLe read the switches' low three bits for the load
 * port - 5 is the 88-PIO, 6 is 2SIO port B - and BASIC reads the high
 * four for its terminal, where 0 is 2SIO port A.
 */
static void test_mbl(const char *mbl, const char *tape, uint8_t sw,
                     const char *port)
{
    int32_t n;

    printf("\n--- %s from %s in the PROM at FE00, on the %s ---\n",
           tape, mbl, port);
    mem_init(MEM_BASE_KB);
    n = is_hex_name(mbl) ? load_hex(mbl) : load_bin(mbl, MBL_ADDR);
    check("the loader PROM is fitted", 1, n > 0 && mem_prom_loaded(MBL_ADDR));
    check("the tape is on the reader", 0, tape_attach(tape));
    s_switches = sw;
    s_pio = PIO_TAPE;
    memset(&s_cpu, 0, sizeof s_cpu);
    reset(MBL_ADDR);
    s_outn = 0;
    type("", 0);
    for (uint32_t used = 0; used < 400000000u && !s_cpu.halted; ) {
        used += i8080_run(&s_cpu, 200000);
        s_out[s_outn] = '\0';
        if (strstr(s_out, "MEMORY SIZE?"))
            break;
    }
    s_out[s_outn] = '\0';
    check("the PROM reads the tape and BASIC starts", 1,
          strstr(s_out, "MEMORY SIZE?") != NULL);
    tape_detach();
    s_switches = 0;
    basic_checks();
}

/* ---------------------------------------------------- XMODEM upload */
/* One XMODEM packet plus the EOT, padded with SUB the way a sender
 * pads.  stx selects a 1K packet. */
static int xm_frame(uint8_t *dst, int stx, uint8_t blk,
                    const uint8_t *data, int dlen, int crc_mode)
{
    int len = stx ? 1024 : 128;
    uint8_t pkt[1024];
    int n = 0;

    memset(pkt, XM_SUB, (size_t)len);
    if (dlen > len)
        dlen = len;
    if (dlen > 0)
        memcpy(pkt, data, (size_t)dlen);
    dst[n++] = (uint8_t)(stx ? XM_STX : XM_SOH);
    dst[n++] = blk;
    dst[n++] = (uint8_t)~blk;
    memcpy(dst + n, pkt, (size_t)len);
    n += len;
    if (crc_mode) {
        uint16_t c = crc16_xmodem(pkt, len);

        dst[n++] = (uint8_t)(c >> 8);
        dst[n++] = (uint8_t)c;
    } else {
        unsigned sum = 0;

        for (int i = 0; i < len; i++)
            sum += pkt[i];
        dst[n++] = (uint8_t)sum;
    }
    return n;
}

static int xm_stream(uint8_t *dst, int stx, const uint8_t *data, int dlen,
                     int crc_mode)
{
    int n = xm_frame(dst, stx, 1, data, dlen, crc_mode);

    dst[n++] = XM_EOT;
    return n;
}

static void test_upload(void)
{
    uint8_t frame[2048];
    uint8_t data[16];
    int n;
    char *argv[4];

    printf("\n--- upload over XMODEM ---\n");
    mem_init(48);

    memcpy(data, "HELLO", 5);
    mem_poke(0x2005, 0xA5);
    n = xm_stream(frame, 0, data, 5, 1);
    type((char *)frame, n);
    check("a 128-byte image lands at the address given", 5, upload_bin(0x2000, 1));
    check("byte for byte", 'H', mem_rd(0x2000));
    check("through the last real byte", 'O', mem_rd(0x2004));
    check("and the SUB padding is not stored", 0xA5, mem_rd(0x2005));

    mem_poke(0x2105, 0xA5);
    n = xm_stream(frame, 0, data, 5, 1);
    type((char *)frame, n);
    check("raw keeps the padded packet", 128, upload_bin(0x2100, 0));
    check("including its SUB bytes", XM_SUB, mem_rd(0x2105));

    {
        uint8_t blk[128];

        memset(blk, 0x11, sizeof blk);
        n = xm_frame(frame, 0, 1, blk, 128, 1);
        memset(blk, 0x22, 10);
        n += xm_frame(frame + n, 0, 2, blk, 10, 1);
        frame[n++] = XM_EOT;
        mem_poke(0x008A, 0xA5);
        type((char *)frame, n);
        check("a second packet continues where the first ended",
              138, upload_bin(0, 1));
        check("the first packet filled its 128 bytes", 0x11, mem_rd(0x007F));
        check("and the second starts after it", 0x22, mem_rd(0x0080));
        check("with its own padding stripped", 0xA5, mem_rd(0x008A));
    }

    n = xm_frame(frame, 0, 1, data, 4, 1);
    n += xm_frame(frame + n, 0, 1, data, 4, 1);
    frame[n++] = XM_EOT;
    mem_poke(0x3004, 0xA5);
    type((char *)frame, n);
    check("a retransmitted packet is stored once", 4, upload_bin(0x3000, 1));
    check("and does not grow the image", 0xA5, mem_rd(0x3004));

    memcpy(data, "ABCD", 4);
    n = xm_stream(frame, 0, data, 4, 0);
    type((char *)frame, n);
    s_hold = 21;                        /* 'C' twenty times, then NAK */
    check("checksum mode works once CRC has been given up",
          4, upload_bin(0x3100, 1));
    check("still the bytes that were sent", 'D', mem_rd(0x3103));

    n = xm_stream(frame, 1, data, 4, 1);
    type((char *)frame, n);
    mem_poke(0x3204, 0xA5);
    check("a 1K packet is accepted", 4, upload_bin(0x3200, 1));
    check("and padded out past the image, not into it", 0xA5, mem_rd(0x3204));

    n = xm_stream(frame, 0, data, 5, 1);
    type((char *)frame, n);
    check("an image is cut off at FFFFh", 2, upload_bin(0xFFFE, 1));
    check("the last two bytes are stored", 0x41, mem_rd(0xFFFE));
    check("and FFFF holds the second byte", 'B', mem_rd(0xFFFF));

    n = xm_frame(frame, 0, 2, data, 4, 1);
    frame[n++] = XM_EOT;
    mem_poke(0x3300, 0xA5);
    type((char *)frame, n);
    check("a packet out of sequence is refused", -5, upload_bin(0x3300, 1));
    check("and stores nothing", 0xA5, mem_rd(0x3300));

    frame[0] = XM_CAN;
    frame[1] = XM_CAN;
    type((char *)frame, 2);
    check("two CANs cancel the transfer", -3, upload_bin(0, 1));

    type("", 0);
    check("silence is a timeout", -2, upload_bin(0, 1));

    {
        static const char hex[] =
            ":03010000010203F6\r\n:00000001FF\r\n";

        n = xm_stream(frame, 0, (const uint8_t *)hex, (int)strlen(hex), 1);
        type((char *)frame, n);
        check("an Intel HEX upload loads its data bytes", 3, upload_hex());
        check("at the address the record names", 0x03, mem_rd(0x0102));
        check("and reports no bad line", 0, s_hex_line);
    }
    {
        static const char hex[] = ":0301000001020300\r\n";

        n = xm_stream(frame, 0, (const uint8_t *)hex, (int)strlen(hex), 1);
        type((char *)frame, n);
        check("a bad HEX record fails the upload", -6, upload_hex());
        check("and names the line", 1, s_hex_line);
    }
    {
        static const char hex[] = ":030200000A0B0CDA";

        n = xm_stream(frame, 0, (const uint8_t *)hex, (int)strlen(hex), 1);
        type((char *)frame, n);
        check("a HEX record with no newline still loads", 3, upload_hex());
        check("into the address it carried", 0x0C, mem_rd(0x0202));
    }

    s_outn = 0;
    s_raw = 0;
    argv[0] = "upload";
    argv[1] = "0";
    argv[2] = NULL;
    menu_command(2, argv);
    s_out[s_outn] = '\0';
    check("upload without a raw console is refused", 1,
          strstr(s_out, "needs a raw console") != NULL);

    s_outn = 0;
    argv[1] = "zz";
    menu_command(2, argv);
    s_out[s_outn] = '\0';
    check("a bad upload address prints the usage", 1,
          strstr(s_out, "upload [ADDR]") != NULL);

    s_raw = 1;
    mem_poke(0x0405, 0xA5);
    memcpy(data, "HELLO", 5);
    n = xm_stream(frame, 0, data, 5, 1);
    type((char *)frame, n);
    s_outn = 0;
    argv[1] = "400";
    menu_command(2, argv);
    s_out[s_outn] = '\0';
    check("the menu command receives at the address it was given",
          'O', mem_rd(0x0404));
    check("and says how many bytes arrived", 1,
          strstr(s_out, "5 bytes at 0400") != NULL);
    check("padding stayed out of memory", 0xA5, mem_rd(0x0405));

    n = xm_stream(frame, 0, data, 5, 1);
    type((char *)frame, n);
    argv[1] = "500";
    argv[2] = "raw";
    menu_command(3, argv);
    check("the menu's raw keeps the padding", XM_SUB, mem_rd(0x0505));
    s_raw = 0;
}

/* ------------------------------------------------------- live */
static int live(int argc, char **argv)
{
    struct termios saved, raw;
    int rc, tty = isatty(0);

    s_live = 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (tty) {
        tcgetattr(0, &saved);
        raw = saved;
        raw.c_iflag &= ~(unsigned)(ICRNL | IXON | ISTRIP | INLCR);
        raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_oflag &= ~(unsigned)OPOST;
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
    }
    rc = app_main(&k_api, argc, argv);
    if (tty)
        tcsetattr(0, TCSANOW, &saved);
    return rc;
}

int main(int argc, char **argv)
{
    const char *dir = getenv("ALTAIR_TESTS");
    const char *basic = getenv("ALTAIR_BASIC");
    const char *mbl = getenv("ALTAIR_MBL");

    if (argc > 1 && strcmp(argv[1], "-i") == 0)
        return live(argc - 1, argv + 1);

    g = &k_api;
    i8080_init();
    test_cpu();
    test_memory();
    test_io();
    test_upload();

    printf("\n--- the program ---\n");
    s_outn = 0;
    s_raw_calls = 0;
    type("quit\r", 5);
    {
        char *args[] = { "altair", "--sw", "zz", NULL };
        check("a bad argument is a usage error", FREYA_EXIT_USAGE,
              app_main(&k_api, 3, args));
    }
    {
        const char *bin = "build/tests/altair_halt.bin";
        FILE *f = fopen(bin, "wb");
        char *args[] = { "altair", (char *)bin, "--at", "100", NULL };

        fputs("\x3e\x2a\x76", f);               /* MVI A,2A; HLT */
        fclose(f);
        s_outn = 0;
        type("quit\r", 5);
        check("an image runs and 'quit' in the menu leaves",
              FREYA_EXIT_OK, app_main(&k_api, 4, args));
        s_out[s_outn] = '\0';
        check("the HLT is reported where it happened", 1,
              strstr(s_out, "HLT at 0102") != NULL);
        check("with the registers", 1, strstr(s_out, "A=2A") != NULL);
        check("the console was made raw and given back", 2, s_raw_calls);
        remove(bin);
    }

    if (dir)
        test_exercisers(dir);
    else
        printf("\n  --    ALTAIR_TESTS not set, skipping the CPU exercisers\n");
    if (basic)
        test_basic(basic);
    else
        printf("  --    ALTAIR_BASIC not set, skipping Altair BASIC\n");
    if (basic && mbl && is_tape_name(basic)) {
        test_mbl(mbl, basic, 0x06, "2SIO port B");
        test_mbl(mbl, basic, 0x05, "88-PIO");
    } else
        printf("  --    ALTAIR_MBL and a tape in ALTAIR_BASIC not both set, "
               "skipping the loader PROM\n");

    printf("\n%d checks, %d failures\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
