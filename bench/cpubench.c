/*
 * Freya - CPU performance tests: Dhrystone 2.1 and Whetstone.
 *
 *     cpubench [-i|-f] [-t seconds]
 *
 * -i runs the integer test alone, -f the float test alone; with neither
 * both run.  Each runs for about the given seconds, 2 by default, 1 to
 * 20.  Ctrl-C stops it between two slices of a test.
 *
 * The integer test is Reinhold Weicker's Dhrystone 2.1 (1988), the C
 * version, with its procedures, its records and its strings as he wrote
 * them.  The score is in Dhrystones a second and in DMIPS, which divides
 * that by 1757, the VAX 11/780's score.  The float test is the
 * Whetstone of Curnow and Wichmann (1976), the eight sections of Roy
 * Longbottom's C version, in single precision: the float the FPUs of the
 * Cortex-M4F, M33F and M7F boards compute.  One pass is a million
 * Whetstone instructions and the score is in MWIPS.  With the CPU's
 * clock known, both are also given per MHz.
 *
 * The ground rule of both is that the procedures are called, not
 * expanded where they are called, so each is marked BENCH_CALL.  The
 * strings are copied and compared by strcpy() and strcmp() of their
 * own, the same code on every target; sinf(), cosf(), atanf(), expf(),
 * logf() and sqrtf() are the C library's.
 *
 * Dhrystone checks its globals against the values Weicker gives once
 * the runs are done, and a wrong one fails the test.  Whetstone prints
 * the value each section leaves after one pass, run again untimed: the
 * same on every target to the last digit or two, which the C library's
 * functions may round differently.
 */
#include "cpubench.h"

#include <math.h>

#if defined(__GNUC__) && !defined(__clang__)
#define BENCH_CALL  __attribute__((noinline, noclone))
#else
#define BENCH_CALL  __attribute__((noinline))
#endif

#define TIME_DEFAULT_S  2U
#define TIME_MAX_S      20U
/* A slice of a test grows until it takes this long, so the clock's
 * millisecond is small beside it and Ctrl-C is still seen promptly. */
#define SLICE_MS        20U

static const cpubench_io_t *s_io;

/* ------------------------------------------------------------ Dhrystone */
/*
 * Dhrystone 2.1.  Names, types and statements are Weicker's; the two
 * records are static instead of malloc()'d, Enum_Loc in Proc_2() and
 * Ch_Loc in Func_2(), which the loop always sets but GCC at -Os cannot
 * tell, start with a value, and the main loop is dhry_run(), entered
 * once per slice.
 */
typedef enum { Ident_1, Ident_2, Ident_3, Ident_4, Ident_5 } Enumeration;

typedef int     One_Thirty;
typedef int     One_Fifty;
typedef char    Capital_Letter;
typedef int     Boolean;
typedef char    Str_30[31];
typedef int     Arr_1_Dim[50];
typedef int     Arr_2_Dim[50][50];

typedef struct record {
    struct record *Ptr_Comp;
    Enumeration    Discr;
    union {
        struct {
            Enumeration Enum_Comp;
            int         Int_Comp;
            char        Str_Comp[31];
        } var_1;
        struct {
            Enumeration E_Comp_2;
            char        Str_2_Comp[31];
        } var_2;
        struct {
            char        Ch_1_Comp;
            char        Ch_2_Comp;
        } var_3;
    } variant;
} Rec_Type, *Rec_Pointer;

#define true    1
#define false   0

static Rec_Type     Glob_Rec, Next_Glob_Rec;
static Rec_Pointer  Ptr_Glob, Next_Ptr_Glob;
static int          Int_Glob;
static Boolean      Bool_Glob;
static char         Ch_1_Glob, Ch_2_Glob;
static Arr_1_Dim    Arr_1_Glob;
static Arr_2_Dim    Arr_2_Glob;

/* What the last run left in main()'s locals, for the check. */
static One_Fifty    Last_Int_1, Last_Int_2, Last_Int_3;
static Enumeration  Last_Enum;
static Str_30       Last_Str_1, Last_Str_2;

static void Proc_1(Rec_Pointer Ptr_Val_Par);
static void Proc_2(One_Fifty *Int_Par_Ref);
static void Proc_3(Rec_Pointer *Ptr_Ref_Par);
static void Proc_4(void);
static void Proc_5(void);
static void Proc_6(Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par);
static void Proc_7(One_Fifty Int_1_Par_Val, One_Fifty Int_2_Par_Val,
                   One_Fifty *Int_Par_Ref);
static void Proc_8(Arr_1_Dim Arr_1_Par_Ref, Arr_2_Dim Arr_2_Par_Ref,
                   int Int_1_Par_Val, int Int_2_Par_Val);
static Enumeration Func_1(Capital_Letter Ch_1_Par_Val,
                          Capital_Letter Ch_2_Par_Val);
static Boolean Func_2(Str_30 Str_1_Par_Ref, Str_30 Str_2_Par_Ref);
static Boolean Func_3(Enumeration Enum_Par_Val);

BENCH_CALL static char *dhry_strcpy(char *d, const char *s)
{
    char *r = d;

    while ((*d++ = *s++) != '\0')
        ;
    return r;
}

BENCH_CALL static int dhry_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void dhry_init(void)
{
    Next_Ptr_Glob = &Next_Glob_Rec;
    Ptr_Glob = &Glob_Rec;

    Ptr_Glob->Ptr_Comp                = Next_Ptr_Glob;
    Ptr_Glob->Discr                   = Ident_1;
    Ptr_Glob->variant.var_1.Enum_Comp = Ident_3;
    Ptr_Glob->variant.var_1.Int_Comp  = 40;
    dhry_strcpy(Ptr_Glob->variant.var_1.Str_Comp,
                "DHRYSTONE PROGRAM, SOME STRING");
    Arr_2_Glob[8][7] = 10;
}

BENCH_CALL static void dhry_run(uint32_t Number_Of_Runs)
{
    One_Fifty   Int_1_Loc;
    One_Fifty   Int_2_Loc;
    One_Fifty   Int_3_Loc;
    char        Ch_Index;
    Enumeration Enum_Loc;
    Str_30      Str_1_Loc;
    Str_30      Str_2_Loc;
    uint32_t    Run_Index;

    dhry_strcpy(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");

    for (Run_Index = 1; Run_Index <= Number_Of_Runs; ++Run_Index) {
        Proc_5();
        Proc_4();
        /* Ch_1_Glob == 'A', Ch_2_Glob == 'B', Bool_Glob == true */
        Int_1_Loc = 2;
        Int_2_Loc = 3;
        dhry_strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
        Enum_Loc = Ident_2;
        Bool_Glob = !Func_2(Str_1_Loc, Str_2_Loc);
        /* Bool_Glob == 1 */
        while (Int_1_Loc < Int_2_Loc) {     /* loop body executed once */
            Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
            /* Int_3_Loc == 7 */
            Proc_7(Int_1_Loc, Int_2_Loc, &Int_3_Loc);
            /* Int_3_Loc == 7 */
            Int_1_Loc += 1;
        }
        /* Int_1_Loc == 3, Int_2_Loc == 3, Int_3_Loc == 7 */
        Proc_8(Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);
        /* Int_Glob == 5 */
        Proc_1(Ptr_Glob);
        for (Ch_Index = 'A'; Ch_Index <= Ch_2_Glob; ++Ch_Index) {
            /* loop body executed twice */
            if (Enum_Loc == Func_1(Ch_Index, 'C')) {
                /* then, not executed */
                Proc_6(Ident_1, &Enum_Loc);
                dhry_strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 3'RD STRING");
                Int_2_Loc = (One_Fifty)Run_Index;
                Int_Glob = (int)Run_Index;
            }
        }
        /* Int_1_Loc == 3, Int_2_Loc == 3, Int_3_Loc == 7 */
        Int_2_Loc = Int_2_Loc * Int_1_Loc;
        Int_1_Loc = Int_2_Loc / Int_3_Loc;
        Int_2_Loc = 7 * (Int_2_Loc - Int_3_Loc) - Int_1_Loc;
        /* Int_1_Loc == 1, Int_2_Loc == 13, Int_3_Loc == 7 */
        Proc_2(&Int_1_Loc);
        /* Int_1_Loc == 5 */
    }

    if (Number_Of_Runs) {
        Last_Int_1 = Int_1_Loc;
        Last_Int_2 = Int_2_Loc;
        Last_Int_3 = Int_3_Loc;
        Last_Enum  = Enum_Loc;
        dhry_strcpy(Last_Str_1, Str_1_Loc);
        dhry_strcpy(Last_Str_2, Str_2_Loc);
    }
}

BENCH_CALL static void Proc_1(Rec_Pointer Ptr_Val_Par)
{
    Rec_Pointer Next_Record = Ptr_Val_Par->Ptr_Comp;
    /* == Ptr_Glob_Next */

    *Ptr_Val_Par->Ptr_Comp = *Ptr_Glob;
    Ptr_Val_Par->variant.var_1.Int_Comp = 5;
    Next_Record->variant.var_1.Int_Comp = Ptr_Val_Par->variant.var_1.Int_Comp;
    Next_Record->Ptr_Comp = Ptr_Val_Par->Ptr_Comp;
    Proc_3(&Next_Record->Ptr_Comp);
    /* Ptr_Val_Par->Ptr_Comp->Ptr_Comp == Ptr_Glob->Ptr_Comp */
    if (Next_Record->Discr == Ident_1) {        /* then, executed */
        Next_Record->variant.var_1.Int_Comp = 6;
        Proc_6(Ptr_Val_Par->variant.var_1.Enum_Comp,
               &Next_Record->variant.var_1.Enum_Comp);
        Next_Record->Ptr_Comp = Ptr_Glob->Ptr_Comp;
        Proc_7(Next_Record->variant.var_1.Int_Comp, 10,
               &Next_Record->variant.var_1.Int_Comp);
    } else {                                    /* not executed */
        *Ptr_Val_Par = *Ptr_Val_Par->Ptr_Comp;
    }
}

BENCH_CALL static void Proc_2(One_Fifty *Int_Par_Ref)
{
    One_Fifty   Int_Loc;
    Enumeration Enum_Loc = Ident_2;

    Int_Loc = *Int_Par_Ref + 10;
    do                                          /* executed once */
        if (Ch_1_Glob == 'A') {                 /* then, executed */
            Int_Loc -= 1;
            *Int_Par_Ref = Int_Loc - Int_Glob;
            Enum_Loc = Ident_1;
        }
    while (Enum_Loc != Ident_1);                /* true */
}

BENCH_CALL static void Proc_3(Rec_Pointer *Ptr_Ref_Par)
{
    if (Ptr_Glob != 0)                          /* then, executed */
        *Ptr_Ref_Par = Ptr_Glob->Ptr_Comp;
    Proc_7(10, Int_Glob, &Ptr_Glob->variant.var_1.Int_Comp);
}

BENCH_CALL static void Proc_4(void)
{
    Boolean Bool_Loc;

    Bool_Loc = Ch_1_Glob == 'A';
    Bool_Glob = Bool_Loc | Bool_Glob;
    Ch_2_Glob = 'B';
}

BENCH_CALL static void Proc_5(void)
{
    Ch_1_Glob = 'A';
    Bool_Glob = false;
}

BENCH_CALL static void Proc_6(Enumeration Enum_Val_Par,
                              Enumeration *Enum_Ref_Par)
{
    *Enum_Ref_Par = Enum_Val_Par;
    if (!Func_3(Enum_Val_Par))                  /* then, not executed */
        *Enum_Ref_Par = Ident_4;
    switch (Enum_Val_Par) {
    case Ident_1:
        *Enum_Ref_Par = Ident_1;
        break;
    case Ident_2:
        if (Int_Glob > 100)                     /* then */
            *Enum_Ref_Par = Ident_1;
        else
            *Enum_Ref_Par = Ident_4;
        break;
    case Ident_3:                               /* executed */
        *Enum_Ref_Par = Ident_2;
        break;
    case Ident_4:
        break;
    case Ident_5:
        *Enum_Ref_Par = Ident_3;
        break;
    }
}

BENCH_CALL static void Proc_7(One_Fifty Int_1_Par_Val,
                              One_Fifty Int_2_Par_Val,
                              One_Fifty *Int_Par_Ref)
{
    One_Fifty Int_Loc;

    Int_Loc = Int_1_Par_Val + 2;
    *Int_Par_Ref = Int_2_Par_Val + Int_Loc;
}

BENCH_CALL static void Proc_8(Arr_1_Dim Arr_1_Par_Ref,
                              Arr_2_Dim Arr_2_Par_Ref,
                              int Int_1_Par_Val, int Int_2_Par_Val)
{
    One_Fifty Int_Index;
    One_Fifty Int_Loc;

    Int_Loc = Int_1_Par_Val + 5;
    Arr_1_Par_Ref[Int_Loc] = Int_2_Par_Val;
    Arr_1_Par_Ref[Int_Loc + 1] = Arr_1_Par_Ref[Int_Loc];
    Arr_1_Par_Ref[Int_Loc + 30] = Int_Loc;
    for (Int_Index = Int_Loc; Int_Index <= Int_Loc + 1; ++Int_Index)
        Arr_2_Par_Ref[Int_Loc][Int_Index] = Int_Loc;
    Arr_2_Par_Ref[Int_Loc][Int_Loc - 1] += 1;
    Arr_2_Par_Ref[Int_Loc + 20][Int_Loc] = Arr_1_Par_Ref[Int_Loc];
    Int_Glob = 5;
}

BENCH_CALL static Enumeration Func_1(Capital_Letter Ch_1_Par_Val,
                                     Capital_Letter Ch_2_Par_Val)
{
    Capital_Letter Ch_1_Loc;
    Capital_Letter Ch_2_Loc;

    Ch_1_Loc = Ch_1_Par_Val;
    Ch_2_Loc = Ch_1_Loc;
    if (Ch_2_Loc != Ch_2_Par_Val)               /* then, executed */
        return Ident_1;
    else {                                      /* not executed */
        Ch_1_Glob = Ch_1_Loc;
        return Ident_2;
    }
}

BENCH_CALL static Boolean Func_2(Str_30 Str_1_Par_Ref, Str_30 Str_2_Par_Ref)
{
    One_Thirty     Int_Loc;
    Capital_Letter Ch_Loc = 'A';

    Int_Loc = 2;
    while (Int_Loc <= 2)                        /* loop body executed once */
        if (Func_1(Str_1_Par_Ref[Int_Loc],
                   Str_2_Par_Ref[Int_Loc + 1]) == Ident_1) {
            /* then, executed */
            Ch_Loc = 'A';
            Int_Loc += 1;
        }
    if (Ch_Loc >= 'W' && Ch_Loc < 'Z')          /* then, not executed */
        Int_Loc = 7;
    if (Ch_Loc == 'R')                          /* then, not executed */
        return true;
    else {                                      /* executed */
        if (dhry_strcmp(Str_1_Par_Ref, Str_2_Par_Ref) > 0) {
            /* then, not executed */
            Int_Loc += 7;
            Int_Glob = Int_Loc;
            return true;
        } else                                  /* executed */
            return false;
    }
}

BENCH_CALL static Boolean Func_3(Enumeration Enum_Par_Val)
{
    Enumeration Enum_Loc;

    Enum_Loc = Enum_Par_Val;
    if (Enum_Loc == Ident_3)                    /* then, executed */
        return true;
    else                                        /* not executed */
        return false;
}

/* The values Weicker lists as "should be" at the end of the program.
 * The number of failures, each one printed. */
static int dhry_check(uint32_t runs)
{
    const cpubench_io_t *io = s_io;
    int bad = 0;

#define DHRY_EXPECT(what, cond) \
    do { if (!(cond)) { io->printf("  wrong         : %s\r\n", what); bad++; } } while (0)

    DHRY_EXPECT("Int_Glob", Int_Glob == 5);
    DHRY_EXPECT("Bool_Glob", Bool_Glob == 1);
    DHRY_EXPECT("Ch_1_Glob", Ch_1_Glob == 'A');
    DHRY_EXPECT("Ch_2_Glob", Ch_2_Glob == 'B');
    DHRY_EXPECT("Arr_1_Glob[8]", Arr_1_Glob[8] == 7);
    DHRY_EXPECT("Arr_2_Glob[8][7]",
                (uint32_t)Arr_2_Glob[8][7] == runs + 10U);
    DHRY_EXPECT("Ptr_Glob->Ptr_Comp", Ptr_Glob->Ptr_Comp == Next_Ptr_Glob);
    DHRY_EXPECT("Ptr_Glob->Discr", Ptr_Glob->Discr == Ident_1);
    DHRY_EXPECT("Ptr_Glob->Enum_Comp",
                Ptr_Glob->variant.var_1.Enum_Comp == Ident_3);
    DHRY_EXPECT("Ptr_Glob->Int_Comp", Ptr_Glob->variant.var_1.Int_Comp == 17);
    DHRY_EXPECT("Ptr_Glob->Str_Comp",
                dhry_strcmp(Ptr_Glob->variant.var_1.Str_Comp,
                            "DHRYSTONE PROGRAM, SOME STRING") == 0);
    DHRY_EXPECT("Next_Ptr_Glob->Ptr_Comp",
                Next_Ptr_Glob->Ptr_Comp == Next_Ptr_Glob);
    DHRY_EXPECT("Next_Ptr_Glob->Discr", Next_Ptr_Glob->Discr == Ident_1);
    DHRY_EXPECT("Next_Ptr_Glob->Enum_Comp",
                Next_Ptr_Glob->variant.var_1.Enum_Comp == Ident_2);
    DHRY_EXPECT("Next_Ptr_Glob->Int_Comp",
                Next_Ptr_Glob->variant.var_1.Int_Comp == 18);
    DHRY_EXPECT("Next_Ptr_Glob->Str_Comp",
                dhry_strcmp(Next_Ptr_Glob->variant.var_1.Str_Comp,
                            "DHRYSTONE PROGRAM, SOME STRING") == 0);
    DHRY_EXPECT("Int_1_Loc", Last_Int_1 == 5);
    DHRY_EXPECT("Int_2_Loc", Last_Int_2 == 13);
    DHRY_EXPECT("Int_3_Loc", Last_Int_3 == 7);
    DHRY_EXPECT("Enum_Loc", Last_Enum == Ident_2);
    DHRY_EXPECT("Str_1_Loc",
                dhry_strcmp(Last_Str_1, "DHRYSTONE PROGRAM, 1'ST STRING") == 0);
    DHRY_EXPECT("Str_2_Loc",
                dhry_strcmp(Last_Str_2, "DHRYSTONE PROGRAM, 2'ND STRING") == 0);
#undef DHRY_EXPECT
    return bad;
}

/* ------------------------------------------------------------ Whetstone */
/*
 * Longbottom's whets.c in single precision, every constant a float so
 * nothing is widened to double.  WHET_X100 = 10 sizes the sections the
 * way Curnow's loop count of 10 does, so one pass is one million
 * Whetstone instructions.  Section 1 runs its n1 loops once, not ten
 * times over to be timed on its own: the whole pass is what is timed.
 */
#define WHET_X100   10L

/* What each section left in the last whet_run(). */
static float s_whet_res[8];

BENCH_CALL static void whet_pa(float e[4], float t, float t2)
{
    long j;

    for (j = 0; j < 6; j++) {
        e[0] = (e[0] + e[1] + e[2] - e[3]) * t;
        e[1] = (e[0] + e[1] - e[2] + e[3]) * t;
        e[2] = (e[0] - e[1] + e[2] + e[3]) * t;
        e[3] = (-e[0] + e[1] + e[2] + e[3]) / t2;
    }
}

BENCH_CALL static void whet_po(float e1[4], long j, long k, long l)
{
    e1[j] = e1[k];
    e1[k] = e1[l];
    e1[l] = e1[j];
}

BENCH_CALL static void whet_p3(float *x, float *y, float *z,
                               float t, float t1, float t2)
{
    *x = *y;
    *y = *z;
    *x = t * (*x + *y);
    *y = t1 * (*x + *y);
    *z = (*x + *y) / t2;
}

BENCH_CALL static void whet_run(uint32_t xtra)
{
    long n1, n2, n3, n4, n5, n6, n7, i;
    uint32_t ix;
    float x, y, z;
    long j, k, l;
    float e1[4];
    float t  = 0.49999975f;
    float t0 = t;
    float t1 = 0.50000025f;
    float t2 = 2.0f;

    n1 = 12 * WHET_X100;
    n2 = 14 * WHET_X100;
    n3 = 345 * WHET_X100;
    n4 = 210 * WHET_X100;
    n5 = 32 * WHET_X100;
    n6 = 899 * WHET_X100;
    n7 = 616 * WHET_X100;

    /* Section 1, array elements */
    e1[0] = 1.0f;
    e1[1] = -1.0f;
    e1[2] = -1.0f;
    e1[3] = -1.0f;
    for (ix = 0; ix < xtra; ix++) {
        for (i = 0; i < n1; i++) {
            e1[0] = (e1[0] + e1[1] + e1[2] - e1[3]) * t;
            e1[1] = (e1[0] + e1[1] - e1[2] + e1[3]) * t;
            e1[2] = (e1[0] - e1[1] + e1[2] + e1[3]) * t;
            e1[3] = (-e1[0] + e1[1] + e1[2] + e1[3]) * t;
        }
        t = 1.0f - t;
    }
    t = t0;
    s_whet_res[0] = e1[3];

    /* Section 2, array as parameter */
    for (ix = 0; ix < xtra; ix++) {
        for (i = 0; i < n2; i++)
            whet_pa(e1, t, t2);
        t = 1.0f - t;
    }
    t = t0;
    s_whet_res[1] = e1[3];

    /* Section 3, conditional jumps */
    j = 1;
    for (ix = 0; ix < xtra; ix++) {
        for (i = 0; i < n3; i++) {
            if (j == 1) j = 2;
            else        j = 3;
            if (j > 2)  j = 0;
            else        j = 1;
            if (j < 1)  j = 1;
            else        j = 0;
        }
    }
    s_whet_res[2] = (float)j;

    /* Section 4, integer arithmetic */
    j = 1;
    k = 2;
    l = 3;
    for (ix = 0; ix < xtra; ix++) {
        for (i = 0; i < n4; i++) {
            j = j * (k - j) * (l - k);
            k = l * k - (l - j) * k;
            l = (l - k) * (k + j);
            e1[l - 2] = (float)(j + k + l);
            e1[k - 2] = (float)(j * k * l);
        }
    }
    s_whet_res[3] = e1[0] + e1[1];

    /* Section 5, trigonometric functions */
    x = 0.5f;
    y = 0.5f;
    for (ix = 0; ix < xtra; ix++) {
        for (i = 1; i < n5; i++) {
            x = t * atanf(t2 * sinf(x) * cosf(x) /
                          (cosf(x + y) + cosf(x - y) - 1.0f));
            y = t * atanf(t2 * sinf(y) * cosf(y) /
                          (cosf(x + y) + cosf(x - y) - 1.0f));
        }
        t = 1.0f - t;
    }
    t = t0;
    s_whet_res[4] = y;

    /* Section 6, procedure calls */
    x = 1.0f;
    y = 1.0f;
    z = 1.0f;
    for (ix = 0; ix < xtra; ix++) {
        for (i = 0; i < n6; i++)
            whet_p3(&x, &y, &z, t, t1, t2);
    }
    s_whet_res[5] = z;

    /* Section 7, array references */
    j = 0;
    k = 1;
    l = 2;
    e1[0] = 1.0f;
    e1[1] = 2.0f;
    e1[2] = 3.0f;
    for (ix = 0; ix < xtra; ix++) {
        for (i = 0; i < n6; i++)
            whet_po(e1, j, k, l);
    }
    s_whet_res[6] = e1[2];

    /* Section 8, standard functions */
    x = 0.75f;
    for (ix = 0; ix < xtra; ix++) {
        for (i = 0; i < n7; i++)
            x = sqrtf(expf(logf(x) / t1));
    }
    s_whet_res[7] = x;
}

/* ---------------------------------------------------------------- timing */
/*
 * Run fn in slices until secs have gone by.  A slice starts at one run
 * and doubles while it takes less than SLICE_MS.  The runs done, with
 * the milliseconds they took in *ms; 0 when Ctrl-C stopped it.
 */
static uint64_t bench_time(void (*fn)(uint32_t), uint32_t secs, uint32_t *ms)
{
    const cpubench_io_t *io = s_io;
    uint32_t target = secs * 1000U;
    uint32_t slice = 1, t0, t1, t;
    uint64_t done = 0;

    t0 = io->ticks_ms();
    for (;;) {
        t = io->ticks_ms();
        fn(slice);
        t1 = io->ticks_ms();
        done += slice;
        if (t1 - t0 >= target) break;
        if (io->should_stop()) return 0;
        if (t1 - t < SLICE_MS && slice < 0x40000000U) slice *= 2;
    }
    *ms = t1 - t0;
    return done;
}

/* n / ms * 1000 / div, in hundredths. */
static uint32_t rate_x100(uint64_t n, uint32_t ms, uint32_t div)
{
    return (uint32_t)((n * 100000U + (uint64_t)ms * div / 2U) /
                      ((uint64_t)ms * div));
}

static void print_u64(uint64_t v)
{
    const cpubench_io_t *io = s_io;

    if (v >= 1000000000U)
        io->printf("%u%09u", (uint32_t)(v / 1000000000U),
                   (uint32_t)(v % 1000000000U));
    else
        io->printf("%u", (uint32_t)v);
}

static void print_x100(const char *label, uint32_t v)
{
    s_io->printf("  %-14s: %u.%02u\r\n", label, v / 100U, v % 100U);
}

static void print_per_mhz(const char *label, uint32_t v_x100)
{
    uint32_t mhz_x100, per;

    if (!s_io->cpu_hz) return;
    /* the clock in hundredths of a MHz, and v per MHz in hundredths */
    mhz_x100 = s_io->cpu_hz / 10000U;
    if (!mhz_x100) return;
    per = (uint32_t)(((uint64_t)v_x100 * 100U + mhz_x100 / 2U) / mhz_x100);
    s_io->printf("  %-14s: %u.%02u at %u MHz\r\n", label, per / 100U,
                 per % 100U, (s_io->cpu_hz + 500000U) / 1000000U);
}

/* Print a float with 6 decimals, from its integer and fractional parts. */
static void print_result(int n, float v)
{
    const cpubench_io_t *io = s_io;
    uint32_t ip, fp;
    float a = v < 0.0f ? -v : v;

    if (a >= 4.0e9f) {
        io->printf("  N%d result     : big\r\n", n);
        return;
    }
    ip = (uint32_t)a;
    fp = (uint32_t)((a - (float)ip) * 1000000.0f + 0.5f);
    if (fp >= 1000000U) {
        ip++;
        fp -= 1000000U;
    }
    io->printf("  N%d result     : %s%u.%06u\r\n", n, v < 0.0f ? "-" : "",
               ip, fp);
}

/* ------------------------------------------------------------- the tests */
static int run_dhrystone(uint32_t secs)
{
    const cpubench_io_t *io = s_io;
    uint32_t ms = 0, dps, dmips;
    uint64_t runs;

    io->printf("Dhrystone 2.1, %u s\r\n", secs);
    dhry_init();
    runs = bench_time(dhry_run, secs, &ms);
    if (!runs) return -1;

    dps = (uint32_t)((runs * 1000U + ms / 2U) / ms);
    dmips = rate_x100(runs, ms, 1757U);
    io->printf("  runs          : ");
    print_u64(runs);
    io->printf(" in %u ms\r\n", ms);
    io->printf("  Dhrystones/s  : %u\r\n", dps);
    print_x100("DMIPS", dmips);
    print_per_mhz("DMIPS/MHz", dmips);
    if (dhry_check((uint32_t)runs) != 0) {
        io->printf("  check         : FAILED\r\n");
        return 1;
    }
    io->printf("  check         : ok\r\n");
    return 0;
}

static int run_whetstone(uint32_t secs)
{
    const cpubench_io_t *io = s_io;
    uint32_t ms = 0, mwips;
    uint64_t passes;

    io->printf("Whetstone, single precision, %u s\r\n", secs);
    passes = bench_time(whet_run, secs, &ms);
    if (!passes) return -1;

    mwips = rate_x100(passes, ms, 1U);
    io->printf("  passes        : ");
    print_u64(passes);
    io->printf(" in %u ms\r\n", ms);
    print_x100("MWIPS", mwips);
    print_per_mhz("MWIPS/MHz", mwips);
    /* N2 and N8 depend on the passes, so the values are one pass's. */
    whet_run(1);
    for (int i = 0; i < 8; i++)
        print_result(i + 1, s_whet_res[i]);
    return 0;
}

static int parse_secs(const char *s, uint32_t *out)
{
    uint32_t v = 0;

    if (!*s) return -1;
    while (*s >= '0' && *s <= '9') {
        v = v * 10U + (uint32_t)(*s++ - '0');
        if (v > TIME_MAX_S) return -1;
    }
    if (*s || v == 0) return -1;
    *out = v;
    return 0;
}

int cpubench(const cpubench_io_t *io, int argc, char **argv)
{
    uint32_t secs = TIME_DEFAULT_S;
    int do_int = 1, do_float = 1, rc = 0, r;

    s_io = io;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (a[0] == '-' && a[1] == 'i' && !a[2]) {
            do_int = 1;
            do_float = 0;
        } else if (a[0] == '-' && a[1] == 'f' && !a[2]) {
            do_int = 0;
            do_float = 1;
        } else if (a[0] == '-' && a[1] == 't' && !a[2] && i + 1 < argc &&
                   parse_secs(argv[i + 1], &secs) == 0) {
            i++;
        } else {
            io->printf("usage: %s\r\n", CPUBENCH_USAGE);
            io->printf("  -i integer (Dhrystone), -f float (Whetstone), "
                       "-t 1..%u seconds each\r\n", TIME_MAX_S);
            return 2;
        }
    }

    if (do_int) {
        r = run_dhrystone(secs);
        if (r < 0) return 1;
        if (r) rc = 1;
    }
    if (do_float) {
        r = run_whetstone(secs);
        if (r < 0) return 1;
        if (r) rc = 1;
    }
    return rc;
}
