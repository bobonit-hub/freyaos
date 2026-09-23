/*
 * Linked for the Cortex-M3, not run on the host.  The test is that this
 * file's float operations resolve to src/softfp.c and the link needs no
 * other floating-point library.
 */
float softfp_link(float a, float b)
{
    float s = a + b;
    float d = a - b;
    float p = a * b;
    float q = p / s;
    int i = (int)q;
    unsigned u = (unsigned)a;
    if (a < b || a <= b || a > b || a >= b || a == b || a != b) s = d;
    return (float)i + (float)u + s;
}
