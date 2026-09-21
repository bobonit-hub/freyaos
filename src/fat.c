/*
 * Freya - FAT16 / FAT32 implementation.
 *
 * Design notes:
 *  - Two 512 byte write-back caches: one for data/directory sectors and
 *    one for FAT sectors.  FAT updates are mirrored into every FAT copy.
 *  - Directories are walked with fat_scan_t, which hides the difference
 *    between the fixed FAT16 root area and a cluster chain.
 *  - Long file names are read, and written whenever a name does not fit
 *    the 8.3 form.  Everything is treated as ASCII.
 *  - All paths handed to this layer are absolute; the shell resolves the
 *    working directory before calling in.
 */
#include "freya.h"
#include "fat.h"

fat_fs_t g_fs;

#define NO_LBA      0xFFFFFFFFUL
#define ENT_SIZE    32
#define ENT_PER_SEC 16

/* ---------------------------------------------------------- byte order */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* --------------------------------------------------------- sector cache */
static uint8_t  s_buf[512];
static uint32_t s_buf_lba = NO_LBA;
static uint8_t  s_buf_dirty;

static uint8_t  s_fat[512];
static uint32_t s_fat_sec = NO_LBA;         /* sector index inside a FAT */
static uint8_t  s_fat_dirty;

static int cache_flush(void)
{
    if (s_buf_dirty && s_buf_lba != NO_LBA) {
        if (sd_write_block(s_buf_lba, s_buf) != 0) return FAT_ERR_IO;
        s_buf_dirty = 0;
    }
    return FAT_OK;
}

static int cache_load(uint32_t lba)
{
    if (s_buf_lba == lba) return FAT_OK;
    if (cache_flush() != FAT_OK) return FAT_ERR_IO;
    if (sd_read_block(lba, s_buf) != 0) { s_buf_lba = NO_LBA; return FAT_ERR_IO; }
    s_buf_lba = lba;
    return FAT_OK;
}

/* Claims a sector in the cache without reading it (full overwrite). */
static int cache_claim(uint32_t lba)
{
    if (s_buf_lba == lba) return FAT_OK;
    if (cache_flush() != FAT_OK) return FAT_ERR_IO;
    s_buf_lba = lba;
    return FAT_OK;
}

static int fat_sec_flush(void)
{
    if (s_fat_dirty && s_fat_sec != NO_LBA) {
        for (uint32_t i = 0; i < g_fs.num_fats; i++) {
            uint32_t lba = g_fs.fat_start + i * g_fs.fat_size + s_fat_sec;
            if (sd_write_block(lba, s_fat) != 0) return FAT_ERR_IO;
        }
        s_fat_dirty = 0;
    }
    return FAT_OK;
}

static int fat_sec_load(uint32_t sec)
{
    if (s_fat_sec == sec) return FAT_OK;
    if (fat_sec_flush() != FAT_OK) return FAT_ERR_IO;
    if (sd_read_block(g_fs.fat_start + sec, s_fat) != 0) {
        s_fat_sec = NO_LBA;
        return FAT_ERR_IO;
    }
    s_fat_sec = sec;
    return FAT_OK;
}

/*
 * FAT32 keeps a free cluster hint in the FSInfo sector.  It is only a
 * hint, but leaving it stale makes every host that mounts the card
 * rescan - or complain - so Freya keeps it up to date.
 */
#define FSI_LEAD_SIG    0x41615252UL
#define FSI_STRUC_SIG   0x61417272UL
#define FSI_TRAIL_SIG   0xAA550000UL

static void fsinfo_read(void)
{
    uint8_t sec[512];

    g_fs.free_valid = 0;
    g_fs.fsinfo_dirty = 0;
    if (!g_fs.fsinfo_lba) return;
    if (sd_read_block(g_fs.fsinfo_lba, sec) != 0) { g_fs.fsinfo_lba = 0; return; }

    if (rd32(&sec[0]) != FSI_LEAD_SIG || rd32(&sec[484]) != FSI_STRUC_SIG ||
        rd32(&sec[508]) != FSI_TRAIL_SIG) {
        g_fs.fsinfo_lba = 0;
        return;
    }

    {
        uint32_t freec = rd32(&sec[488]);
        uint32_t nextf = rd32(&sec[492]);

        if (freec != 0xFFFFFFFFUL && freec <= g_fs.clus_count) {
            g_fs.free_count = freec;
            g_fs.free_valid = 1;
        }
        if (nextf >= 2 && nextf < g_fs.clus_count + 2) g_fs.next_free = nextf;
    }
}

static int fsinfo_write(void)
{
    uint8_t sec[512];

    if (!g_fs.fsinfo_lba || !g_fs.fsinfo_dirty) return FAT_OK;
    if (sd_read_block(g_fs.fsinfo_lba, sec) != 0) return FAT_ERR_IO;

    wr32(&sec[488], g_fs.free_valid ? g_fs.free_count : 0xFFFFFFFFUL);
    wr32(&sec[492], g_fs.next_free);
    if (sd_write_block(g_fs.fsinfo_lba, sec) != 0) return FAT_ERR_IO;

    g_fs.fsinfo_dirty = 0;
    return FAT_OK;
}

int fat_sync(void)
{
    int a = cache_flush();
    int b = fat_sec_flush();
    int c = fsinfo_write();

    if (a != FAT_OK) return a;
    if (b != FAT_OK) return b;
    return c;
}

static void cache_reset(void)
{
    s_buf_lba = NO_LBA;  s_buf_dirty = 0;
    s_fat_sec = NO_LBA;  s_fat_dirty = 0;
}

/* -------------------------------------------------------- FAT accessors */
static uint32_t clus2lba(uint32_t clus)
{
    return g_fs.data_start + (clus - 2) * g_fs.sec_per_clus;
}

static int is_eoc(uint32_t v)
{
    return (g_fs.type == 32) ? (v >= 0x0FFFFFF8UL) : (v >= 0xFFF8UL);
}

static int clus_valid(uint32_t c)
{
    return c >= 2 && c < g_fs.clus_count + 2;
}

static int fat_get(uint32_t clus, uint32_t *val)
{
    uint32_t off, sec;
    int rc;

    if (!clus_valid(clus)) return FAT_ERR_INVAL;
    off = (g_fs.type == 32) ? clus * 4 : clus * 2;
    sec = off / 512;
    off %= 512;

    rc = fat_sec_load(sec);
    if (rc != FAT_OK) return rc;

    *val = (g_fs.type == 32) ? (rd32(&s_fat[off]) & 0x0FFFFFFFUL)
                             : rd16(&s_fat[off]);
    return FAT_OK;
}

static int fat_put(uint32_t clus, uint32_t val)
{
    uint32_t off, sec;
    int rc;

    if (!clus_valid(clus)) return FAT_ERR_INVAL;
    off = (g_fs.type == 32) ? clus * 4 : clus * 2;
    sec = off / 512;
    off %= 512;

    rc = fat_sec_load(sec);
    if (rc != FAT_OK) return rc;

    if (g_fs.type == 32) {
        uint32_t old = rd32(&s_fat[off]);
        wr32(&s_fat[off], (old & 0xF0000000UL) | (val & 0x0FFFFFFFUL));
    } else {
        wr16(&s_fat[off], (uint16_t)val);
    }
    s_fat_dirty = 1;
    return FAT_OK;
}

static int zero_cluster(uint32_t clus)
{
    uint32_t lba = clus2lba(clus);

    if (cache_flush() != FAT_OK) return FAT_ERR_IO;
    memset(s_buf, 0, sizeof(s_buf));
    for (uint32_t i = 0; i < g_fs.sec_per_clus; i++) {
        if (sd_write_block(lba + i, s_buf) != 0) { s_buf_lba = NO_LBA; return FAT_ERR_IO; }
    }
    s_buf_lba = lba + g_fs.sec_per_clus - 1;
    s_buf_dirty = 0;
    return FAT_OK;
}

/* Allocates one cluster and optionally links it after 'prev'. */
static int clus_alloc(uint32_t prev, uint32_t *out)
{
    uint32_t start = g_fs.next_free < 2 ? 2 : g_fs.next_free;
    uint32_t c = start;
    uint32_t scanned = 0;
    int rc;

    while (scanned <= g_fs.clus_count) {
        uint32_t v;
        if (c >= g_fs.clus_count + 2) { c = 2; scanned++; continue; }
        rc = fat_get(c, &v);
        if (rc != FAT_OK) return rc;
        if (v == 0) {
            rc = fat_put(c, (g_fs.type == 32) ? 0x0FFFFFFFUL : 0xFFFFUL);
            if (rc != FAT_OK) return rc;
            if (prev) {
                rc = fat_put(prev, c);
                if (rc != FAT_OK) return rc;
            }
            g_fs.next_free = c + 1;
            if (g_fs.free_valid && g_fs.free_count) g_fs.free_count--;
            g_fs.fsinfo_dirty = 1;
            *out = c;
            return FAT_OK;
        }
        c++;
        scanned++;
    }
    return FAT_ERR_NOSPC;
}

static int clus_free_chain(uint32_t clus)
{
    while (clus_valid(clus)) {
        uint32_t next;
        int rc = fat_get(clus, &next);
        if (rc != FAT_OK) return rc;
        rc = fat_put(clus, 0);
        if (rc != FAT_OK) return rc;
        if (g_fs.next_free > clus) g_fs.next_free = clus;
        if (g_fs.free_valid && g_fs.free_count < g_fs.clus_count) g_fs.free_count++;
        g_fs.fsinfo_dirty = 1;
        if (is_eoc(next) || next == 0) break;
        clus = next;
    }
    return FAT_OK;
}

/* --------------------------------------------------------------- mount */
static uint32_t root_clus(void)
{
    return (g_fs.type == 32) ? g_fs.root_clus : 0;
}

static int parse_bpb(const uint8_t *b, uint32_t part_lba)
{
    uint32_t fat_sz, tot_sec, data_sec, root_sectors, data_start_rel;

    if (rd16(b + 11) != 512) return FAT_ERR_NOFS;
    if (b[13] == 0 || (b[13] & (b[13] - 1)) != 0) return FAT_ERR_NOFS;
    if (b[16] == 0 || b[16] > 2) return FAT_ERR_NOFS;

    g_fs.sec_per_clus = b[13];
    g_fs.rsvd_sec     = rd16(b + 14);
    g_fs.num_fats     = b[16];
    g_fs.root_ent_cnt = rd16(b + 17);

    fat_sz  = rd16(b + 22);
    if (fat_sz == 0) fat_sz = rd32(b + 36);
    tot_sec = rd16(b + 19);
    if (tot_sec == 0) tot_sec = rd32(b + 32);
    if (fat_sz == 0 || tot_sec == 0 || g_fs.rsvd_sec == 0) return FAT_ERR_NOFS;

    root_sectors   = ((uint32_t)g_fs.root_ent_cnt * 32 + 511) / 512;
    data_start_rel = g_fs.rsvd_sec + g_fs.num_fats * fat_sz + root_sectors;
    if (tot_sec <= data_start_rel) return FAT_ERR_NOFS;
    data_sec = tot_sec - data_start_rel;

    g_fs.fat_size     = fat_sz;
    g_fs.tot_sec      = tot_sec;
    g_fs.part_lba     = part_lba;
    g_fs.fat_start    = part_lba + g_fs.rsvd_sec;
    g_fs.root_start   = g_fs.fat_start + g_fs.num_fats * fat_sz;
    g_fs.root_sectors = root_sectors;
    g_fs.data_start   = part_lba + data_start_rel;
    g_fs.clus_count   = data_sec / g_fs.sec_per_clus;
    g_fs.bytes_per_clus = (uint32_t)g_fs.sec_per_clus * 512UL;

    if (g_fs.clus_count < 4085)       return FAT_ERR_NOFS;  /* FAT12 */
    else if (g_fs.clus_count < 65525) g_fs.type = 16;
    else                              g_fs.type = 32;

    g_fs.root_clus = (g_fs.type == 32) ? rd32(b + 44) : 0;
    if (g_fs.type == 32 && !clus_valid(g_fs.root_clus)) return FAT_ERR_NOFS;

    if (g_fs.type == 32) {
        uint16_t fsi = rd16(b + 48);
        g_fs.fsinfo_lba = (fsi && fsi != 0xFFFF) ? part_lba + fsi : 0;
    } else {
        g_fs.fsinfo_lba = 0;
    }

    {
        const uint8_t *lab = (g_fs.type == 32) ? b + 71 : b + 43;
        int n = 11;
        while (n > 0 && lab[n - 1] == ' ') n--;
        for (int i = 0; i < n; i++) g_fs.label[i] = (char)lab[i];
        g_fs.label[n] = '\0';
    }

    g_fs.next_free = 2;
    return FAT_OK;
}

static int looks_like_bpb(const uint8_t *b)
{
    if (!((b[0] == 0xEB && b[2] == 0x90) || b[0] == 0xE9)) return 0;
    if (rd16(b + 11) != 512) return 0;
    if (b[13] == 0) return 0;
    return 1;
}

int fat_mount(void)
{
    uint8_t sector[512];
    int rc = FAT_ERR_NOFS;

    memset(&g_fs, 0, sizeof(g_fs));
    cache_reset();

    if (!g_sd.initialised && sd_init() != 0) return FAT_ERR_IO;
    if (sd_read_block(0, sector) != 0) return FAT_ERR_IO;

    if (looks_like_bpb(sector)) {
        rc = parse_bpb(sector, 0);
    }
    if (rc != FAT_OK && rd16(sector + 510) == 0xAA55) {
        /* MBR: take the first partition that carries a usable BPB. */
        for (int i = 0; i < 4; i++) {
            const uint8_t *p = sector + 446 + i * 16;
            uint8_t type = p[4];
            uint32_t lba = rd32(p + 8);
            uint8_t vbr[512];

            if (type == 0 || lba == 0) continue;
            if (sd_read_block(lba, vbr) != 0) continue;
            if (parse_bpb(vbr, lba) == FAT_OK) { rc = FAT_OK; break; }
        }
    }
    if (rc != FAT_OK) {
        /* Last resort: an unpartitioned volume without the usual jump. */
        rc = parse_bpb(sector, 0);
    }
    if (rc != FAT_OK) return rc;

    g_fs.mounted = 1;
    fsinfo_read();
    return FAT_OK;
}

void fat_unmount(void)
{
    fat_sync();
    g_fs.mounted = 0;
    cache_reset();
}

int fat_mounted(void) { return g_fs.mounted; }

const char *fat_type_str(void)
{
    if (!g_fs.mounted) return "none";
    return (g_fs.type == 32) ? "FAT32" : "FAT16";
}

int fat_free_clusters(uint32_t *free_clus)
{
    uint32_t n = 0;

    if (!g_fs.mounted) return FAT_ERR_NOFS;
    for (uint32_t c = 2; c < g_fs.clus_count + 2; c++) {
        uint32_t v;
        int rc = fat_get(c, &v);
        if (rc != FAT_OK) return rc;
        if (v == 0) n++;
    }
    *free_clus = n;

    /* A full scan is authoritative - refresh the hint while we have it. */
    g_fs.free_count = n;
    g_fs.free_valid = 1;
    g_fs.fsinfo_dirty = 1;
    fsinfo_write();
    return FAT_OK;
}

const char *fat_err_str(int err)
{
    switch (err) {
    case FAT_OK:           return "ok";
    case FAT_ERR_IO:       return "card I/O error";
    case FAT_ERR_NOFS:     return "no FAT filesystem";
    case FAT_ERR_NOENT:    return "no such file or directory";
    case FAT_ERR_EXIST:    return "already exists";
    case FAT_ERR_NOSPC:    return "no space left on device";
    case FAT_ERR_INVAL:    return "invalid argument";
    case FAT_ERR_NOTDIR:   return "not a directory";
    case FAT_ERR_ISDIR:    return "is a directory";
    case FAT_ERR_NOTEMPTY: return "directory not empty";
    case FAT_ERR_NOFILE:   return "not a regular file";
    case FAT_ERR_RDONLY:   return "read-only";
    default:               return "unknown error";
    }
}

/* ---------------------------------------------------- directory scanner */
static int scan_open(fat_scan_t *sc, uint32_t dir_clus)
{
    memset(sc, 0, sizeof(*sc));

    if (dir_clus == 0 && g_fs.type == 32) dir_clus = g_fs.root_clus;

    if (dir_clus == 0) {
        sc->fixed_root    = 1;
        sc->lba           = g_fs.root_start;
        sc->root_sec_left = g_fs.root_sectors;
        if (sc->root_sec_left == 0) sc->ended = 1;
    } else {
        if (!clus_valid(dir_clus)) return FAT_ERR_INVAL;
        sc->clus = dir_clus;
        sc->lba  = clus2lba(dir_clus);
    }
    return FAT_OK;
}

/* Returns a pointer to the current 32 byte entry, or NULL at the end. */
static uint8_t *scan_entry(fat_scan_t *sc)
{
    if (sc->ended) return NULL;
    if (cache_load(sc->lba) != FAT_OK) { sc->ended = 1; return NULL; }
    return &s_buf[sc->idx * ENT_SIZE];
}

/* Moves to the next entry.  With alloc != 0 the directory is extended. */
static int scan_advance(fat_scan_t *sc, int alloc)
{
    if (sc->ended) return 1;

    if (++sc->idx < ENT_PER_SEC) return 0;
    sc->idx = 0;

    if (sc->fixed_root) {
        if (--sc->root_sec_left == 0) { sc->ended = 1; return 1; }
        sc->lba++;
        return 0;
    }

    if (++sc->sec_in_clus < g_fs.sec_per_clus) {
        sc->lba++;
        return 0;
    }

    {
        uint32_t next;
        int rc = fat_get(sc->clus, &next);
        if (rc != FAT_OK) { sc->ended = 1; return rc; }

        if (is_eoc(next) || next == 0) {
            if (!alloc) { sc->ended = 1; return 1; }
            rc = clus_alloc(sc->clus, &next);
            if (rc != FAT_OK) { sc->ended = 1; return rc; }
            rc = zero_cluster(next);
            if (rc != FAT_OK) { sc->ended = 1; return rc; }
        }
        sc->clus        = next;
        sc->sec_in_clus = 0;
        sc->lba         = clus2lba(next);
    }
    return 0;
}

/* ------------------------------------------------------- name handling */
static uint8_t sfn_checksum(const uint8_t *sfn)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i]);
    return sum;
}

static void sfn_to_string(const uint8_t *e, char *out)
{
    int n = 0;

    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)e[i];
        if (i == 0 && (uint8_t)c == 0x05) c = (char)0xE5;
        if (e[12] & 0x08) c = to_lower(c);
        out[n++] = c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            char c = (char)e[i];
            if (e[12] & 0x10) c = to_lower(c);
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

static int sfn_char_ok(char c)
{
    if ((uint8_t)c < 0x20) return 0;
    return strchr("\"*+,./:;<=>?[\\]| ", c) == NULL;
}

static int lfn_char_ok(char c)
{
    if ((uint8_t)c < 0x20) return 0;
    return strchr("\"*/:<>?\\|", c) == NULL;
}

/*
 * Builds the 8.3 name for 'name'.  *needs_lfn is set when the long name
 * must also be stored; *ntres carries the Windows NT lower case flags for
 * names that only differ from 8.3 by letter case.
 */
static int make_sfn(const char *name, uint8_t sfn[11], int *needs_lfn, uint8_t *ntres)
{
    const char *dot;
    int base_len = 0, ext_len = 0, lossy = 0;
    int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0;

    memset(sfn, ' ', 11);
    *ntres = 0;
    *needs_lfn = 0;

    if (!name[0]) return FAT_ERR_INVAL;
    for (const char *p = name; *p; p++)
        if (!lfn_char_ok(*p)) return FAT_ERR_INVAL;
    if (strlen(name) >= FAT_MAX_NAME) return FAT_ERR_INVAL;

    dot = strrchr(name, '.');
    if (dot == name) dot = NULL;                /* ".foo" has no extension */

    for (const char *p = name; *p && (!dot || p < dot); p++) {
        if (*p == ' ') { lossy = 1; continue; }
        if (base_len >= 8) { lossy = 1; break; }
        if (*p >= 'a' && *p <= 'z') base_lower = 1;
        else if (*p >= 'A' && *p <= 'Z') base_upper = 1;
        sfn[base_len++] = sfn_char_ok(*p) ? (uint8_t)to_upper(*p)
                                          : (lossy = 1, (uint8_t)'_');
    }
    if (base_len == 0) return FAT_ERR_INVAL;

    if (dot) {
        for (const char *p = dot + 1; *p; p++) {
            if (ext_len >= 3) { lossy = 1; break; }
            if (*p >= 'a' && *p <= 'z') ext_lower = 1;
            else if (*p >= 'A' && *p <= 'Z') ext_upper = 1;
            sfn[8 + ext_len++] = sfn_char_ok(*p) ? (uint8_t)to_upper(*p)
                                                 : (lossy = 1, (uint8_t)'_');
        }
        /* A trailing dot or a second dot cannot be represented in 8.3. */
        if (strchr(name, '.') != dot) lossy = 1;
    }

    if (lossy || (base_lower && base_upper) || (ext_lower && ext_upper)) {
        *needs_lfn = 1;
    } else {
        if (base_lower) *ntres |= 0x08;
        if (ext_lower)  *ntres |= 0x10;
    }
    if (sfn[0] == 0xE5) sfn[0] = 0x05;
    return FAT_OK;
}

/* ------------------------------------------------ directory enumeration */
typedef struct {
    uint32_t lba;
    uint16_t off;
} entpos_t;

/*
 * Reads the next real entry (skipping free slots, volume labels and the
 * LFN slots, which are folded into e->name).  Returns 0 on success,
 * 1 at the end of the directory, negative on error.  The scanner is left
 * positioned on the 8.3 entry.
 */
static int scan_next_entry(fat_scan_t *sc, fat_dirent_t *e, entpos_t *pos,
                           char *sfn_out)
{
    char lfn[FAT_MAX_NAME];
    int have_lfn = 0, lfn_slots = 0;
    uint8_t lfn_sum = 0;

    memset(lfn, 0, sizeof(lfn));

    for (;;) {
        uint8_t *d = scan_entry(sc);
        int rc;

        if (!d) return sc->ended ? 1 : FAT_ERR_IO;

        if (d[0] == 0x00) { sc->ended = 1; return 1; }

        if (d[0] == 0xE5) {
            have_lfn = 0;
            rc = scan_advance(sc, 0);
            if (rc) return rc;
            continue;
        }

        if ((d[11] & FAT_ATTR_LFN) == FAT_ATTR_LFN) {
            uint8_t ord = d[0];
            int slot = ord & 0x1F;

            if (ord & 0x40) {
                memset(lfn, 0, sizeof(lfn));
                lfn_sum   = d[13];
                lfn_slots = slot;
                have_lfn  = (slot >= 1 && (slot - 1) * 13 < FAT_MAX_NAME - 1);
            }
            if (have_lfn && slot >= 1 && slot <= lfn_slots) {
                static const uint8_t ofs[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
                int base = (slot - 1) * 13;
                for (int i = 0; i < 13; i++) {
                    uint16_t wc = rd16(&d[ofs[i]]);
                    int idx = base + i;
                    if (idx >= FAT_MAX_NAME - 1) break;
                    if (wc == 0x0000 || wc == 0xFFFF) { lfn[idx] = '\0'; continue; }
                    lfn[idx] = (wc < 0x20 || wc > 0x7E) ? '?' : (char)wc;
                }
            } else {
                have_lfn = 0;
            }
            rc = scan_advance(sc, 0);
            if (rc) return rc;
            continue;
        }

        if (d[11] & FAT_ATTR_VOLUME) {
            have_lfn = 0;
            rc = scan_advance(sc, 0);
            if (rc) return rc;
            continue;
        }

        /* A real 8.3 entry. */
        {
            char sfn[13];
            sfn_to_string(d, sfn);
            if (sfn_out) strcpy(sfn_out, sfn);

            if (have_lfn && lfn_sum == sfn_checksum(d) && lfn[0])
                strncpy(e->name, lfn, FAT_MAX_NAME - 1);
            else
                strncpy(e->name, sfn, FAT_MAX_NAME - 1);
            e->name[FAT_MAX_NAME - 1] = '\0';

            e->attr  = d[11];
            e->size  = rd32(&d[28]);
            e->clus  = ((uint32_t)rd16(&d[20]) << 16) | rd16(&d[26]);
            e->wtime = rd16(&d[22]);
            e->wdate = rd16(&d[24]);
            if (pos) { pos->lba = sc->lba; pos->off = (uint16_t)(sc->idx * ENT_SIZE); }
        }
        return 0;
    }
}

static int find_in_dir(uint32_t dir_clus, const char *name,
                       fat_dirent_t *e, entpos_t *pos)
{
    fat_scan_t sc;
    int rc = scan_open(&sc, dir_clus);

    if (rc != FAT_OK) return rc;

    for (;;) {
        char sfn[13];
        fat_dirent_t tmp;

        rc = scan_next_entry(&sc, &tmp, pos, sfn);
        if (rc == 1) return FAT_ERR_NOENT;
        if (rc < 0) return rc;

        if (strcasecmp(tmp.name, name) == 0 || strcasecmp(sfn, name) == 0) {
            if (e) *e = tmp;
            return FAT_OK;
        }
        rc = scan_advance(&sc, 0);
        if (rc == 1) return FAT_ERR_NOENT;
        if (rc < 0) return rc;
    }
}

static int sfn_exists(uint32_t dir_clus, const uint8_t sfn[11])
{
    fat_scan_t sc;

    if (scan_open(&sc, dir_clus) != FAT_OK) return 0;

    for (;;) {
        uint8_t *d = scan_entry(&sc);
        int rc;

        if (!d) return 0;
        if (d[0] == 0x00) return 0;
        if (d[0] != 0xE5 && (d[11] & FAT_ATTR_LFN) != FAT_ATTR_LFN &&
            memcmp(d, sfn, 11) == 0)
            return 1;
        rc = scan_advance(&sc, 0);
        if (rc) return 0;
    }
}

/* ------------------------------------------------------ path resolution */
static const char *path_component(const char *p, char *out, int max)
{
    int n = 0;

    while (*p == '/') p++;
    if (!*p) return NULL;
    while (*p && *p != '/') {
        if (n < max - 1) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return p;
}

/*
 * Splits 'path' into the containing directory cluster and the final name.
 * With want_parent = 0 the whole path is resolved into *e.
 */
static int resolve(const char *path, uint32_t *parent, char *leaf,
                   fat_dirent_t *e, entpos_t *pos)
{
    char comp[FAT_MAX_NAME];
    const char *p = path;
    uint32_t dir = root_clus();
    int rc;

    if (!g_fs.mounted) return FAT_ERR_NOFS;
    if (leaf) leaf[0] = '\0';
    if (parent) *parent = dir;

    for (;;) {
        const char *next = path_component(p, comp, sizeof(comp));
        const char *peek;

        if (!next) {
            /* Path ends here: it denotes a directory we already resolved. */
            if (parent) *parent = dir;
            if (e) {
                memset(e, 0, sizeof(*e));
                e->attr = FAT_ATTR_DIR;
                e->clus = dir;
                strcpy(e->name, "/");
            }
            if (pos) { pos->lba = NO_LBA; pos->off = 0; }
            return FAT_OK;
        }

        peek = next;
        while (*peek == '/') peek++;

        if (!*peek) {
            /* Last component. */
            if (parent) *parent = dir;
            if (leaf) strncpy(leaf, comp, FAT_MAX_NAME - 1);
            if (!e) return FAT_OK;
            rc = find_in_dir(dir, comp, e, pos);
            return rc;
        }

        {
            fat_dirent_t tmp;
            rc = find_in_dir(dir, comp, &tmp, NULL);
            if (rc != FAT_OK) return rc;
            if (!(tmp.attr & FAT_ATTR_DIR)) return FAT_ERR_NOTDIR;
            dir = tmp.clus;
        }
        p = next;
    }
}

int fat_stat(const char *path, fat_dirent_t *e)
{
    return resolve(path, NULL, NULL, e, NULL);
}

/* --------------------------------------------------- creating entries */
static void fill_sfn_entry(uint8_t *d, const uint8_t sfn[11], uint8_t attr,
                           uint8_t ntres, uint32_t clus, uint32_t size)
{
    uint16_t date = rtc_fat_date();
    uint16_t time = rtc_fat_time();

    memset(d, 0, ENT_SIZE);
    memcpy(d, sfn, 11);
    d[11] = attr;
    d[12] = ntres;
    wr16(&d[14], time);            /* creation */
    wr16(&d[16], date);
    wr16(&d[18], date);            /* last access */
    wr16(&d[20], (uint16_t)(clus >> 16));
    wr16(&d[22], time);            /* last write */
    wr16(&d[24], date);
    wr16(&d[26], (uint16_t)(clus & 0xFFFF));
    wr32(&d[28], size);
}

/* Finds 'need' consecutive free slots, extending the directory if needed. */
static int dir_alloc_slots(uint32_t dir_clus, int need, fat_scan_t *out, int *hit_end)
{
    fat_scan_t sc, run;
    int run_len = 0;
    int rc = scan_open(&sc, dir_clus);

    *hit_end = 0;
    if (rc != FAT_OK) return rc;
    run = sc;

    for (;;) {
        uint8_t *d = scan_entry(&sc);

        if (!d) {
            /* End of the cluster chain: extend from the current run. */
            if (run_len == 0) run = sc;
            break;
        }
        if (d[0] == 0x00) {
            if (run_len == 0) run = sc;
            *hit_end = 1;
            *out = run;
            return FAT_OK;             /* the rest of the directory is free */
        }
        if (d[0] == 0xE5) {
            if (run_len == 0) run = sc;
            if (++run_len >= need) { *out = run; return FAT_OK; }
        } else {
            run_len = 0;
        }

        rc = scan_advance(&sc, 0);
        if (rc < 0) return rc;
        if (rc == 1) { if (run_len == 0) run = sc; break; }
    }

    /* The directory is full - grow it (the FAT16 root cannot grow). */
    if (sc.fixed_root) return FAT_ERR_NOSPC;
    {
        fat_scan_t grow = sc;
        grow.ended = 0;
        grow.idx = ENT_PER_SEC - 1;
        rc = scan_advance(&grow, 1);
        if (rc != 0) return (rc < 0) ? rc : FAT_ERR_NOSPC;
        *hit_end = 1;
        *out = grow;
    }
    return FAT_OK;
}

static int write_lfn_slot(fat_scan_t *sc, const char *name, int slot,
                          int last, uint8_t sum)
{
    static const uint8_t ofs[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
    uint8_t *d = scan_entry(sc);
    int base = (slot - 1) * 13;
    int len = (int)strlen(name);
    int past_end = 0;

    if (!d) return FAT_ERR_IO;
    memset(d, 0, ENT_SIZE);
    d[0]  = (uint8_t)(slot | (last ? 0x40 : 0));
    d[11] = FAT_ATTR_LFN;
    d[13] = sum;

    for (int i = 0; i < 13; i++) {
        int idx = base + i;
        uint16_t wc;
        if (past_end)         wc = 0xFFFF;
        else if (idx < len)   wc = (uint16_t)(uint8_t)name[idx];
        else { wc = 0x0000; past_end = 1; }
        wr16(&d[ofs[i]], wc);
    }
    s_buf_dirty = 1;
    return FAT_OK;
}

/*
 * Creates a new directory entry for 'name' in directory 'dir_clus'.
 * Returns the location of the resulting 8.3 entry in *pos.
 */
static int create_entry(uint32_t dir_clus, const char *name, uint8_t attr,
                        uint32_t clus, uint32_t size, entpos_t *pos)
{
    uint8_t sfn[11], ntres;
    int needs_lfn, n_slots, hit_end, rc;
    fat_scan_t sc;

    rc = make_sfn(name, sfn, &needs_lfn, &ntres);
    if (rc != FAT_OK) return rc;

    if (needs_lfn || sfn_exists(dir_clus, sfn)) {
        /* Derive BASE~n until the short name is unique. */
        uint8_t base[11];
        int base_len = 8, tail_ok = 0;

        memcpy(base, sfn, 11);
        while (base_len > 0 && base[base_len - 1] == ' ') base_len--;

        for (int n = 1; n <= 99 && !tail_ok; n++) {
            char suffix[4];
            int slen, keep;

            ksnprintf(suffix, sizeof(suffix), "~%d", n);
            slen = (int)strlen(suffix);
            keep = MIN(base_len, 8 - slen);

            memcpy(sfn, base, 11);
            for (int i = 0; i < slen; i++) sfn[keep + i] = (uint8_t)suffix[i];
            for (int i = keep + slen; i < 8; i++) sfn[i] = ' ';

            if (!sfn_exists(dir_clus, sfn)) tail_ok = 1;
        }
        if (!tail_ok) return FAT_ERR_EXIST;
        needs_lfn = 1;
        ntres = 0;
    }

    n_slots = needs_lfn ? (int)((strlen(name) + 12) / 13) + 1 : 1;

    rc = dir_alloc_slots(dir_clus, n_slots, &sc, &hit_end);
    if (rc != FAT_OK) return rc;

    if (needs_lfn) {
        uint8_t sum = sfn_checksum(sfn);
        int lfn_n = n_slots - 1;

        for (int slot = lfn_n; slot >= 1; slot--) {
            rc = write_lfn_slot(&sc, name, slot, slot == lfn_n, sum);
            if (rc != FAT_OK) return rc;
            rc = scan_advance(&sc, 1);
            if (rc != 0) return (rc < 0) ? rc : FAT_ERR_NOSPC;
        }
    }

    {
        uint8_t *d = scan_entry(&sc);
        if (!d) return FAT_ERR_IO;
        fill_sfn_entry(d, sfn, attr, ntres, clus, size);
        s_buf_dirty = 1;
        if (pos) { pos->lba = sc.lba; pos->off = (uint16_t)(sc.idx * ENT_SIZE); }
    }

    if (hit_end) {
        fat_scan_t tail = sc;
        if (scan_advance(&tail, 0) == 0) {
            uint8_t *d = scan_entry(&tail);
            if (d && d[0] != 0x00) { d[0] = 0x00; s_buf_dirty = 1; }
        }
    }
    return fat_sync();
}

/* Marks the 8.3 entry at 'target' and its long name slots as deleted. */
static int delete_entry(uint32_t dir_clus, const entpos_t *target)
{
    fat_scan_t sc;
    entpos_t run[21];
    int run_len = 0;
    int rc = scan_open(&sc, dir_clus);

    if (rc != FAT_OK) return rc;

    for (;;) {
        uint8_t *d = scan_entry(&sc);
        entpos_t here;

        if (!d) return FAT_ERR_NOENT;
        here.lba = sc.lba;
        here.off = (uint16_t)(sc.idx * ENT_SIZE);

        if (d[0] == 0x00) return FAT_ERR_NOENT;

        if ((d[11] & FAT_ATTR_LFN) == FAT_ATTR_LFN && d[0] != 0xE5) {
            if (run_len < (int)ARRAY_SIZE(run)) run[run_len++] = here;
        } else if (here.lba == target->lba && here.off == target->off) {
            run[run_len < (int)ARRAY_SIZE(run) ? run_len++ : run_len - 1] = here;
            for (int i = 0; i < run_len; i++) {
                if (cache_load(run[i].lba) != FAT_OK) return FAT_ERR_IO;
                s_buf[run[i].off] = 0xE5;
                s_buf_dirty = 1;
            }
            return fat_sync();
        } else {
            run_len = 0;
        }

        rc = scan_advance(&sc, 0);
        if (rc == 1) return FAT_ERR_NOENT;
        if (rc < 0) return rc;
    }
}

static int update_entry(const fat_file_t *f)
{
    uint8_t *d;

    if (f->dir_lba == NO_LBA) return FAT_OK;
    if (cache_load(f->dir_lba) != FAT_OK) return FAT_ERR_IO;

    d = &s_buf[f->dir_off];
    wr32(&d[28], f->size);
    wr16(&d[20], (uint16_t)(f->first_clus >> 16));
    wr16(&d[26], (uint16_t)(f->first_clus & 0xFFFF));
    wr16(&d[22], rtc_fat_time());
    wr16(&d[24], rtc_fat_date());
    d[11] |= FAT_ATTR_ARCHIVE;
    s_buf_dirty = 1;
    return fat_sync();
}

/* ---------------------------------------------------------------- files */
static int seek_cluster(fat_file_t *f, uint32_t clus_idx, int alloc)
{
    int rc;

    if (f->first_clus == 0) {
        if (!alloc) return FAT_ERR_NOSPC;
        rc = clus_alloc(0, &f->first_clus);
        if (rc != FAT_OK) return rc;
        f->cur_clus = f->first_clus;
        f->cur_clus_idx = 0;
    }
    if (f->cur_clus == 0 || f->cur_clus_idx > clus_idx) {
        f->cur_clus = f->first_clus;
        f->cur_clus_idx = 0;
    }
    while (f->cur_clus_idx < clus_idx) {
        uint32_t next;
        rc = fat_get(f->cur_clus, &next);
        if (rc != FAT_OK) return rc;
        if (is_eoc(next) || next == 0) {
            if (!alloc) return FAT_ERR_NOSPC;
            rc = clus_alloc(f->cur_clus, &next);
            if (rc != FAT_OK) return rc;
        }
        f->cur_clus = next;
        f->cur_clus_idx++;
    }
    return FAT_OK;
}

int fat_open(fat_file_t *f, const char *path, int flags)
{
    fat_dirent_t e;
    entpos_t pos;
    uint32_t parent;
    char leaf[FAT_MAX_NAME];
    int rc;

    if (!g_fs.mounted) return FAT_ERR_NOFS;
    memset(f, 0, sizeof(*f));

    rc = resolve(path, &parent, leaf, &e, &pos);

    if (rc == FAT_ERR_NOENT) {
        if (!(flags & FAT_CREATE)) return FAT_ERR_NOENT;
        if (!leaf[0]) return FAT_ERR_INVAL;
        rc = create_entry(parent, leaf, FAT_ATTR_ARCHIVE, 0, 0, &pos);
        if (rc != FAT_OK) return rc;
        f->first_clus = 0;
        f->size = 0;
    } else if (rc == FAT_OK) {
        if (!leaf[0]) return FAT_ERR_ISDIR;          /* path was a directory */
        if (e.attr & FAT_ATTR_DIR) return FAT_ERR_ISDIR;
        if ((flags & FAT_WRITE) && (e.attr & FAT_ATTR_RDONLY)) return FAT_ERR_RDONLY;
        f->first_clus = e.clus;
        f->size = e.size;

        if ((flags & FAT_TRUNC) && (flags & FAT_WRITE)) {
            if (f->first_clus) {
                rc = clus_free_chain(f->first_clus);
                if (rc != FAT_OK) return rc;
            }
            f->first_clus = 0;
            f->size = 0;
            f->dirty = 1;
        }
    } else {
        return rc;
    }

    f->open    = 1;
    f->flags   = (uint8_t)flags;
    f->dir_lba = pos.lba;
    f->dir_off = pos.off;
    f->pos     = (flags & FAT_APPEND) ? f->size : 0;
    f->cur_clus = 0;
    f->cur_clus_idx = 0;

    if (f->dirty) {
        rc = update_entry(f);
        if (rc != FAT_OK) return rc;
        f->dirty = 0;
    }
    return FAT_OK;
}

int fat_read(fat_file_t *f, void *buf, uint32_t len, uint32_t *got)
{
    uint8_t *out = buf;
    uint32_t done = 0;
    int rc = FAT_OK;

    if (!f->open || !(f->flags & (FAT_READ | FAT_WRITE))) return FAT_ERR_INVAL;
    if (f->pos >= f->size) { if (got) *got = 0; return FAT_OK; }
    if (len > f->size - f->pos) len = f->size - f->pos;

    while (done < len) {
        uint32_t clus_idx = f->pos / g_fs.bytes_per_clus;
        uint32_t in_clus  = f->pos % g_fs.bytes_per_clus;
        uint32_t sec      = in_clus / 512;
        uint32_t off      = in_clus % 512;
        uint32_t chunk    = MIN(512 - off, len - done);

        rc = seek_cluster(f, clus_idx, 0);
        if (rc != FAT_OK) break;
        rc = cache_load(clus2lba(f->cur_clus) + sec);
        if (rc != FAT_OK) break;

        memcpy(out + done, &s_buf[off], chunk);
        done += chunk;
        f->pos += chunk;
    }

    if (got) *got = done;
    return (done > 0) ? FAT_OK : rc;
}

int fat_write(fat_file_t *f, const void *buf, uint32_t len, uint32_t *put)
{
    const uint8_t *in = buf;
    uint32_t done = 0;
    int rc = FAT_OK;

    if (!f->open || !(f->flags & FAT_WRITE)) return FAT_ERR_INVAL;

    while (done < len) {
        uint32_t clus_idx = f->pos / g_fs.bytes_per_clus;
        uint32_t in_clus  = f->pos % g_fs.bytes_per_clus;
        uint32_t sec      = in_clus / 512;
        uint32_t off      = in_clus % 512;
        uint32_t chunk    = MIN(512 - off, len - done);
        uint32_t lba;

        rc = seek_cluster(f, clus_idx, 1);
        if (rc != FAT_OK) break;
        lba = clus2lba(f->cur_clus) + sec;

        if (off == 0 && chunk == 512) {
            rc = cache_claim(lba);          /* full sector: no read needed */
            if (rc != FAT_OK) break;
        } else {
            rc = cache_load(lba);
            if (rc != FAT_OK) break;
        }

        memcpy(&s_buf[off], in + done, chunk);
        s_buf_dirty = 1;
        done += chunk;
        f->pos += chunk;
        if (f->pos > f->size) f->size = f->pos;
    }

    f->dirty = 1;
    if (put) *put = done;
    return (done == len) ? FAT_OK : rc;
}

int fat_seek(fat_file_t *f, uint32_t pos)
{
    if (!f->open) return FAT_ERR_INVAL;
    if (pos > f->size) pos = f->size;
    f->pos = pos;
    return FAT_OK;
}

int fat_close(fat_file_t *f)
{
    int rc = FAT_OK;

    if (!f->open) return FAT_ERR_INVAL;
    if (f->dirty) rc = update_entry(f);
    if (fat_sync() != FAT_OK && rc == FAT_OK) rc = FAT_ERR_IO;
    f->open = 0;
    return rc;
}

/* ---------------------------------------------------------- directories */
int fat_opendir(fat_dir_t *d, const char *path)
{
    fat_dirent_t e;
    int rc;

    if (!g_fs.mounted) return FAT_ERR_NOFS;

    rc = resolve(path, NULL, NULL, &e, NULL);
    if (rc != FAT_OK) return rc;
    if (!(e.attr & FAT_ATTR_DIR)) return FAT_ERR_NOTDIR;

    rc = scan_open(&d->scan, e.clus);
    if (rc != FAT_OK) return rc;
    d->open = 1;
    return FAT_OK;
}

int fat_readdir(fat_dir_t *d, fat_dirent_t *e)
{
    int rc;

    if (!d->open) return FAT_ERR_INVAL;

    rc = scan_next_entry(&d->scan, e, NULL, NULL);
    if (rc != 0) return rc;

    rc = scan_advance(&d->scan, 0);
    if (rc < 0) return rc;
    return 0;
}

int fat_closedir(fat_dir_t *d)
{
    d->open = 0;
    return FAT_OK;
}

int fat_mkdir(const char *path)
{
    fat_dirent_t e;
    uint32_t parent, clus;
    char leaf[FAT_MAX_NAME];
    int rc;

    if (!g_fs.mounted) return FAT_ERR_NOFS;

    rc = resolve(path, &parent, leaf, &e, NULL);
    if (rc == FAT_OK) return FAT_ERR_EXIST;
    if (rc != FAT_ERR_NOENT) return rc;
    if (!leaf[0]) return FAT_ERR_INVAL;

    rc = clus_alloc(0, &clus);
    if (rc != FAT_OK) return rc;
    rc = zero_cluster(clus);
    if (rc != FAT_OK) return rc;

    /* "." and ".." - a parent of the root is recorded as cluster 0. */
    {
        uint8_t dot[11], dotdot[11];
        uint32_t up = (parent == root_clus()) ? 0 : parent;
        uint8_t *d;

        memset(dot, ' ', 11);    dot[0] = '.';
        memset(dotdot, ' ', 11); dotdot[0] = '.'; dotdot[1] = '.';

        rc = cache_load(clus2lba(clus));
        if (rc != FAT_OK) return rc;
        d = s_buf;
        fill_sfn_entry(&d[0], dot, FAT_ATTR_DIR, 0, clus, 0);
        fill_sfn_entry(&d[ENT_SIZE], dotdot, FAT_ATTR_DIR, 0, up, 0);
        s_buf_dirty = 1;
        rc = cache_flush();
        if (rc != FAT_OK) return rc;
    }

    rc = create_entry(parent, leaf, FAT_ATTR_DIR, clus, 0, NULL);
    if (rc != FAT_OK) {
        clus_free_chain(clus);
        fat_sync();
        return rc;
    }
    return fat_sync();
}

static int dir_is_empty(uint32_t clus)
{
    fat_scan_t sc;
    fat_dirent_t e;

    if (scan_open(&sc, clus) != FAT_OK) return 0;

    for (;;) {
        int rc = scan_next_entry(&sc, &e, NULL, NULL);
        if (rc == 1) return 1;
        if (rc < 0) return 0;
        if (strcmp(e.name, ".") != 0 && strcmp(e.name, "..") != 0) return 0;
        rc = scan_advance(&sc, 0);
        if (rc == 1) return 1;
        if (rc < 0) return 0;
    }
}

int fat_unlink(const char *path)
{
    fat_dirent_t e;
    entpos_t pos;
    uint32_t parent;
    char leaf[FAT_MAX_NAME];
    int rc;

    if (!g_fs.mounted) return FAT_ERR_NOFS;

    rc = resolve(path, &parent, leaf, &e, &pos);
    if (rc != FAT_OK) return rc;
    if (!leaf[0]) return FAT_ERR_INVAL;             /* refuse the root */
    if (e.attr & FAT_ATTR_RDONLY) return FAT_ERR_RDONLY;

    if (e.attr & FAT_ATTR_DIR) {
        if (!dir_is_empty(e.clus)) return FAT_ERR_NOTEMPTY;
    }

    if (e.clus) {
        rc = clus_free_chain(e.clus);
        if (rc != FAT_OK) return rc;
    }
    rc = delete_entry(parent, &pos);
    if (rc != FAT_OK) return rc;
    return fat_sync();
}
