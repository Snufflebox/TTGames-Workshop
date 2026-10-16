/* TCS PC four-player patch - custom characters and the 4p_mods file overlay.
 *
 * Included by loader.c (one translation unit, no C runtime). How it works:
 * ../../CUSTOM_CHARACTERS.md. Entry points used by the hook code (custom_chars.s)
 * through the mod area: file_hook (every game file open), icon_preload (after
 * stuff\things.gsc loads) and icon_for_char (icon scene of a character); custom_watch
 * and spawn_watch are called once per frame from party_watch.
 */

/* ---------- game files: 4p_mods overlay and custom characters ----------
   Every file the game opens goes through 0x4FA3F0(name, mode, archive, wait), which looks
   in the .DAT archives first and on disk second. file_hook(name, out) runs first and
   returns 1 with out = a path relative to the game folder to open from disk instead:
   - 4p_mods\<name> when that file exists: any game file can be replaced or added;
   - custom characters: every folder 4p_mods\chars\<Folder>\ holding a character .txt
     (community format: NAME.TXT, NAME_PC.GHG, NAME_ICON_PC.GSC, animations) is added
     to the game. The loader writes merged copies of chars\chars.txt (char_start/dir/
     file), chars\collection.txt (collect, unlocked from the start or buy_in_shop) and
     the language file stuff\text\<language>.txt (the character's name under a free text
     id) to 4p_mods\_gen\ and opens those instead; the character's own .txt is copied
     there with name_id set to that id. stuff\icons\<icon> is looked up in the
     character folders too. Optional 4p_char.ini in a folder: name=..., price=...
     Base files are read through the game's own file functions. */
#define G_OPEN(n, m, a, w) ((int(__cdecl *)(const char *, int, void *, int))0x4FA3F0)(n, m, a, w)
#define G_SIZE(h) ((int(__cdecl *)(int))0x4F6B40)(h)
#define G_READ(h, b, n) ((int(__cdecl *)(int, void *, int))0x4FB550)(h, b, n)
#define G_CLOSE(h) ((void(__cdecl *)(int))0x4F99E0)(h)
#define G_ARCHIVE (*(void **)0x91EE88)
#define MAX_CUSTOM 64
#define BASE_CHAR_MAX 340             /* chars\chars.txt limit (0x4CA2EF) */

typedef struct {
    DWORD attr, t[6], size_hi, size_lo, r0, r1;
    char name[260], alt[14];
} FINDDATA;

struct custom {
    char dir[64], file[64], name[64], icon[64], gsc[64], ghg[64];
    int text_id, price, icon_gen, ghg_mode;   /* ghg_mode: 0 as named, 1 other name, 2 _gen copy */
    void *icon_scene;                         /* icon loaded once at start-up, kept for good */
};
static struct custom customs[MAX_CUSTOM];
static int n_customs = -1, ids_done, customs_added;
static volatile long fh_lock;
static volatile DWORD fh_owner, fh_opens, fh_waits, fh_ms;
static volatile DWORD other_tid, last_open_tid;
static char last_open[128];
static BYTE *fbuf;                    /* 2 MB work buffer */
#define FBUF_SIZE 0x200000


static unsigned game_dir(char *d)
{
    unsigned n = 0, last = 0;
    while (log_path[n]) {
        d[n] = log_path[n];
        if (d[n] == '\\' || d[n] == '/')
            last = n + 1;
        n++;
    }
    d[last] = 0;
    return last;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int same_i(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b))
        a++, b++;
    return !*a && !*b;
}

static int starts_i(const char *s, const char *head)   /* returns length matched or 0 */
{
    int n = 0;
    while (head[n]) {
        if (lower(s[n]) != lower(head[n]) && !((s[n] == '/' || s[n] == '\\') && head[n] == '\\'))
            return 0;
        n++;
    }
    return n;
}

static int ends_with(const char *s, const char *tail)
{
    unsigned a = 0, b = 0;
    while (s[a])
        a++;
    while (tail[b])
        b++;
    if (b > a)
        return 0;
    while (b)
        if (lower(s[--a]) != lower(tail[--b]))
            return 0;
    return 1;
}

/* ---------- 4p_mods file index ----------
   Every file the game opens (the .DAT archive's included) asks file_hook whether
   4p_mods holds a replacement, which cost one or more disk lookups per open (43 for
   an icon). The folder is listed once instead; files the loader writes itself
   (_gen) are added when written. Paths are kept lower case with '\\'. If the folder
   holds more than the index takes, lookups go to the disk as before. */
#define IDX_SLOTS 0x8000
#define IDX_POOL 0x400000
static DWORD *idx_hash;              /* IDX_SLOTS entries: offset+1 into idx_pool, 0 empty */
static char *idx_pool;
static DWORD idx_used, idx_count;
static int idx_state;                /* 0 not built, 1 ready, 2 off (too large) */

static DWORD idx_key(const char *s)
{
    DWORD h = 2166136261u;
    for (; *s; s++) {
        char c = *s == '/' ? '\\' : lower(*s);
        h = (h ^ (BYTE)c) * 16777619u;
    }
    return h;
}

static int idx_same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a == '/' ? '\\' : lower(*a), y = *b == '/' ? '\\' : lower(*b);
        if (x != y)
            return 0;
    }
    return !*a && !*b;
}

static int idx_find(const char *rel)
{
    DWORD i = idx_key(rel) & (IDX_SLOTS - 1);
    while (idx_hash[i]) {
        if (idx_same(idx_pool + idx_hash[i] - 1, rel))
            return 1;
        i = (i + 1) & (IDX_SLOTS - 1);
    }
    return 0;
}

static void idx_add(const char *rel)          /* rel: "4p_mods\\..." */
{
    DWORD i, n = 0;
    if (idx_state != 1 || idx_find(rel))
        return;
    while (rel[n])
        n++;
    if (idx_count >= IDX_SLOTS / 2 || idx_used + n + 1 > IDX_POOL) {
        idx_state = 2;
        return;
    }
    i = idx_key(rel) & (IDX_SLOTS - 1);
    while (idx_hash[i])
        i = (i + 1) & (IDX_SLOTS - 1);
    for (n = 0; rel[n]; n++)
        idx_pool[idx_used + n] = rel[n];
    idx_pool[idx_used + n] = 0;
    idx_hash[i] = idx_used + 1;
    idx_used += n + 1;
    idx_count++;
}

static void idx_scan(char *rel, unsigned n, char *abs, unsigned base, int depth)
{
    FINDDATA fd;
    HANDLE f;
    unsigned m;
    if (depth > 12 || idx_state != 1)
        return;
    m = str_cpy(abs, base, rel);
    str_cpy(abs, m, "\\*");
    f = FindFirstFileA(abs, &fd);
    if (f == INVALID_HANDLE)
        return;
    do {
        unsigned k;
        if (fd.name[0] == '.' && (!fd.name[1] || (fd.name[1] == '.' && !fd.name[2])))
            continue;
        if (n + 2 + 260 > 380)
            continue;
        rel[n] = '\\';
        k = str_cpy(rel, n + 1, fd.name);
        if (fd.attr & 0x10)
            idx_scan(rel, k, abs, base, depth + 1);
        else
            idx_add(rel);
        rel[n] = 0;
    } while (idx_state == 1 && FindNextFileA(f, &fd));
    FindClose(f);
}

static void idx_build(void)
{
    char rel[400], abs[800];
    unsigned base;
    idx_state = 2;
    idx_hash = VirtualAlloc(0, IDX_SLOTS * 4 + IDX_POOL, MEM_COMMIT_RESERVE, 4);
    if (!idx_hash)
        return;
    idx_pool = (char *)(idx_hash + IDX_SLOTS);
    idx_state = 1;
    base = game_dir(abs);
    str_cpy(rel, 0, "4p_mods");
    idx_scan(rel, 7, abs, base, 0);
    if (idx_state != 1)
        log_msg("4p_mods: too many files for the index, looking each file up on disk", 0, 0);
}

/* a file the loader wrote: add it to the index (absolute path inside the game folder) */
static void idx_add_abs(const char *path)
{
    char dir[400];
    unsigned n = game_dir(dir), k;
    for (k = 0; k < n; k++)
        if (lower(path[k]) != lower(dir[k]))
            return;
    if (starts_i(path + n, "4p_mods\\"))
        idx_add(path + n);
}

static void make_dirs(char *path)   /* creates every directory of a file path */
{
    unsigned i;
    for (i = 3; path[i]; i++)
        if (path[i] == '\\' || path[i] == '/') {
            char c = path[i];
            path[i] = 0;
            CreateDirectoryA(path, 0);
            path[i] = c;
        }
}

static int read_disk(const char *path, BYTE *buf, int max)
{
    HANDLE h = CreateFileA(path, 0x80000000, 1, 0, 3, 0x80, 0);
    DWORD got = 0;
    if (h == INVALID_HANDLE)
        return -1;
    ReadFile(h, buf, (DWORD)max, &got, 0);
    CloseHandle(h);
    return (int)got;
}

static int write_disk(char *path, const BYTE *buf, int n)
{
    DWORD w;
    HANDLE h;
    make_dirs(path);
    h = CreateFileA(path, 0x40000000, 1, 0, 2, 0x80, 0);
    if (h == INVALID_HANDLE)
        return 0;
    WriteFile(h, buf, (DWORD)n, &w, 0);
    CloseHandle(h);
    idx_add_abs(path);
    return 1;
}

static int read_game(const char *name, BYTE *buf, int max)   /* from the .DAT (or disk) */
{
    int h, size, got;
    h = G_OPEN(name, 0, G_ARCHIVE, 0);
    if (h <= 0)
        return -1;
    size = G_SIZE(h);
    got = size > 0 && size < max ? G_READ(h, buf, size) : -1;
    G_CLOSE(h);
    return got;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int parse_int(const char *p)
{
    int v = 0;
    while (*p == ' ' || *p == '\t' || *p == '=')
        p++;
    while (is_digit(*p))
        v = v * 10 + (*p++ - '0');
    return v;
}

/* value of "key=" in a text buffer (line start), copied to out; returns 1 if found */
static int ini_value(const char *buf, int len, const char *key, char *out, int max)
{
    int i = 0, k, n;
    while (i < len) {
        k = 0;
        while (key[k] && i + k < len && lower(buf[i + k]) == lower(key[k]))
            k++;
        if (!key[k]) {
            i += k;
            while (i < len && (buf[i] == ' ' || buf[i] == '=' || buf[i] == '"'))
                i++;
            n = 0;
            while (i < len && buf[i] != '\r' && buf[i] != '\n' && buf[i] != '"' && n < max - 1)
                out[n++] = buf[i++];
            while (n && out[n - 1] == ' ')
                n--;
            out[n] = 0;
            return 1;
        }
        while (i < len && buf[i] != '\n')
            i++;
        i++;
    }
    return 0;
}

static void mod_line(const char *a, const char *b, int has_val, DWORD v);
static int same_alnum(const char *a, const char *b)   /* letters and digits only */
{
    for (;;) {
        while (*a && !((lower(*a) >= 'a' && lower(*a) <= 'z') || (*a >= '0' && *a <= '9')))
            a++;
        while (*b && !((lower(*b) >= 'a' && lower(*b) <= 'z') || (*b >= '0' && *b <= '9')))
            b++;
        if (!*a || !*b)
            return !*a && !*b;
        if (lower(*a) != lower(*b))
            return 0;
        a++, b++;
    }
}

/* a <x>_PC.GHG in the folder whose <x> matches base in letters and digits (T3_M4 / T3-M4) */
static int ghg_like(const char *dir, const char *base, char *out)
{
    char pat[400];
    FINDDATA fd;
    HANDLE g;
    unsigned n = game_dir(pat);
    int found = 0;
    n = str_cpy(pat, n, "4p_mods\\chars\\");
    n = str_cpy(pat, n, dir);
    str_cpy(pat, n, "\\*_pc.ghg");
    g = FindFirstFileA(pat, &fd);
    if (g == INVALID_HANDLE)
        return 0;
    do {
        char x[64];
        int i, l = 0;
        while (fd.name[l])
            l++;
        if (l < 8 || l - 7 > 63)
            continue;
        for (i = 0; i < l - 7; i++)
            x[i] = fd.name[i];
        x[i] = 0;
        if (same_alnum(x, base) && !starts_i(x, "test")) {
            found = 1;
            if (out)
                str_cpy(out, 0, fd.name);
        }
    } while (!found && FindNextFileA(g, &fd));
    FindClose(g);
    return found;
}

/* An icon .GSC names its two pictures inside (NTBL: "<name>1" = locked silhouette,
   "<name>" = colour picture) and the game finds them by the character's icon= name.
   Community packs often copy another icon file without renaming inside (the KotOR pack
   ships every icon as anakin_jedi_icon), so the lookup fails or hits Anakin's. Such a
   file is copied to 4p_mods\_gen\icons\ with every occurrence of the inside name (node
   names and path) replaced by a unique name of the same length, and icon= set to it. */
static int is_ident(BYTE b)
{
    return (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || b == '_' || b == '-';
}

static int match_n_i(const BYTE *p, const char *s, int l)
{
    int i;
    for (i = 0; i < l; i++) {
        BYTE a = p[i], b = (BYTE)s[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b)
            return 0;
    }
    return 1;
}

static void fix_icon(struct custom *c, int idx)
{
    char path[400], inner[64], synth[64];
    int len, i, j, st = -1, en = -1, l = 0, hits = 0;
    unsigned n;
    if (!c->gsc[0] || !c->icon[0])
        return;
    n = game_dir(path);
    n = str_cpy(path, n, "4p_mods\\chars\\");
    n = str_cpy(path, n, c->dir);
    n = str_cpy(path, n, "\\");
    str_cpy(path, n, c->gsc);
    len = read_disk(path, fbuf, FBUF_SIZE - 1);
    if (len <= 0)
        return;
    /* the node names: NTBL, size, count, then "<name>1\0<name>\0" */
    for (i = 0; i + 16 < len; i++)
        if (fbuf[i] == 'N' && fbuf[i + 1] == 'T' && fbuf[i + 2] == 'B' && fbuf[i + 3] == 'L') {
            const BYTE *a = fbuf + i + 12, *b;
            int la = 0, lb = 0;
            while (i + 12 + la < len && la < 63 && a[la] >= 32) la++;
            b = a + la + 1;
            while (i + 13 + la + lb < len && lb < 63 && b[lb] >= 32) lb++;
            if (la == lb + 1 && a[lb] == '1' && match_n_i(a, (const char *)b, lb) && lb >= 3) {
                st = (int)(b - fbuf);
                l = lb;
            }
            break;
        }
    if (st < 0) {                             /* fall back to the path name */
        for (i = 0; i + 7 <= len; i++)
            if (starts_i((const char *)fbuf + i, "_pc.gsc"))
                en = i;
        if (en < 0)
            return;
        for (i = en - 1; i >= 0 && en - i < 60; i--)
            if (fbuf[i] == '\\' || fbuf[i] == '/' || fbuf[i] < 32) {
                st = i + 1;
                break;
            }
        if (st < 0 || en - st < 3)
            return;
        l = en - st;
    }
    for (i = 0; i < l; i++)
        inner[i] = (char)fbuf[st + i];
    inner[l] = 0;
    if (same_i(inner, c->icon))
        return;                               /* already consistent */
    /* new name: "m4p" + 2 letters for the folder index, padded with 'x' to length l */
    synth[0] = 'm';
    synth[1] = '4';
    synth[2] = 'p';
    synth[3] = (char)('a' + idx / 26 % 26);
    synth[4] = (char)('a' + idx % 26);
    for (j = 5; j < l; j++)
        synth[j] = 'x';
    if (l < 5)                                /* very short names: "m4" + 1 letter */
        synth[2] = (char)('a' + idx % 26);
    synth[l] = 0;
    for (i = 0; i + l <= len; i++)
        if (match_n_i(fbuf + i, inner, l) && (i == 0 || !is_ident(fbuf[i - 1])) &&
            (i + l == len || !is_ident(fbuf[i + l]) || fbuf[i + l] == '1' || fbuf[i + l] == '_')) {
            for (j = 0; j < l; j++)
                fbuf[i + j] = (BYTE)synth[j];
            hits++;
            i += l - 1;
        }
    if (hits < 2)
        return;
    n = game_dir(path);
    n = str_cpy(path, n, "4p_mods\\_gen\\icons\\");
    n = str_cpy(path, n, synth);
    str_cpy(path, n, "_pc.gsc");
    if (!write_disk(path, fbuf, len))
        return;
    str_cpy(c->icon, 0, synth);
    c->icon_gen = 1;
    mod_line("icon renamed ", c->dir, 1, (DWORD)hits);
}

/* The model: chars\<dir>\<file>_pc.ghg is what the game asks for. Packs sometimes name
   it differently (T3-M4_PC.GHG next to T3_M4.TXT), served under the asked name then. */
static void find_model(struct custom *c)
{
    char path[400];
    unsigned n = game_dir(path);
    DWORD at;
    n = str_cpy(path, n, "4p_mods\\chars\\");
    n = str_cpy(path, n, c->dir);
    n = str_cpy(path, n, "\\");
    n = str_cpy(path, n, c->file);
    str_cpy(path, n, "_PC.GHG");
    c->ghg_mode = 0;
    n = str_cpy(c->ghg, 0, c->file);
    str_cpy(c->ghg, n, "_PC.GHG");
    at = GetFileAttributesA(path);
    if (at != 0xFFFFFFFF && !(at & 0x10))
        return;
    if (ghg_like(c->dir, c->file, c->ghg)) {
        c->ghg_mode = 1;
        mod_line("model by other name ", c->ghg, 0, 0);
    }
}

/* Models exported with Maya namespaces name their nodes "MaceWindu_SE_019:head" (Jolee
   Bindo); the game looks for "head", "weaponRight|LightSabre" and so on. The namespaces
   are cut out of the node name table in a 4p_mods\_gen\models copy (each name stays at
   its offset, so nothing else in the file moves). Texture records whose size disagrees
   with their DDS are corrected in the same copy. */
static void fix_model(struct custom *c)
{
    char path[400];
    FINDDATA fd;
    HANDLE g;
    BYTE *buf;
    unsigned n;
    int size, len, i, st, en, changed = 0;
    n = game_dir(path);
    n = str_cpy(path, n, "4p_mods\\chars\\");
    n = str_cpy(path, n, c->dir);
    n = str_cpy(path, n, "\\");
    str_cpy(path, n, c->ghg);
    g = FindFirstFileA(path, &fd);
    if (g == INVALID_HANDLE)
        return;
    FindClose(g);
    size = (int)fd.size_lo;
    if (fd.size_hi || size < 64 || size > 0x8000000)
        return;
    buf = VirtualAlloc(0, (unsigned)size + 16, MEM_COMMIT_RESERVE, 4);
    if (!buf)
        return;
    len = read_disk(path, buf, size);
    for (i = 0; i + 12 < len; i++)
        if (buf[i] == 'N' && buf[i + 1] == 'T' && buf[i + 2] == 'B' && buf[i + 3] == 'L')
            break;
    if (i + 12 < len) {
        st = i + 12;
        en = st + *(int *)(buf + i + 8);
        if (en > len || en < st)
            en = st;
        for (i = st; i < en; ) {
            int e = i, o = i, j, part;
            while (e < en && buf[e])
                e++;
            for (j = i; j < e; j++)
                if (buf[j] == ':')
                    break;
            if (j < e) {                      /* rewrite "a:b|c:d" as "b|d" in place */
                part = i;
                for (j = i; j <= e; j++) {
                    if (j == e || buf[j] == '|') {
                        int k = part, last = part;
                        for (; k < j; k++)
                            if (buf[k] == ':')
                                last = k + 1;
                        for (k = last; k < j; k++)
                            buf[o++] = buf[k];
                        if (j < e)
                            buf[o++] = '|';
                        part = j + 1;
                    }
                }
                while (o < e)
                    buf[o++] = 0;
                changed++;
            }
            i = e + 1;
        }
    }
    /* Texture table at the start of the file: u16 count at +4, then per texture a
       24-byte record {width, height, levels, format, ?, size} and its DDS data. The
       game passes the record's width/height to D3DXCreateTextureFromFileInMemoryEx
       (0x6020B6), which resizes the picture when they differ from the DDS. Jolee
       Bindo's first record says 0x08000800 x 0x08000000 for a 2048x2048 DDS: D3DX
       blew it up to the device maximum and recompressed it, 30 s while the level or
       the Cantina waited. Records that disagree with their DDS header are corrected. */
    if (len >= 6) {
        int count = *(unsigned short *)(buf + 4), t, o = 6, fixed = 0;
        for (t = 0; t < count && o + 24 + 128 <= len; t++) {
            int *r = (int *)(buf + o), sz = r[5];
            BYTE *dds = buf + o + 24;
            if (r[0] <= 0 || sz < 128 || o + 24 + sz > len)
                break;                  /* width <= 0: cube map or none, other layout */
            if (dds[0] == 'D' && dds[1] == 'D' && dds[2] == 'S' && dds[3] == ' ' &&
                *(DWORD *)(dds + 4) == 124) {
                int h = *(int *)(dds + 12), w = *(int *)(dds + 16);
                if (w > 0 && h > 0 && w <= 16384 && h <= 16384 && (r[0] != w || r[1] != h)) {
                    r[0] = w;
                    r[1] = h;
                    fixed++;
                }
            }
            o += 24 + sz;
        }
        if (fixed) {
            mod_line("texture sizes fixed ", c->dir, 1, (DWORD)fixed);
            changed += fixed;
        }
    }
    if (changed) {
        n = game_dir(path);
        n = str_cpy(path, n, "4p_mods\\_gen\\models\\");
        n = str_cpy(path, n, c->dir);
        n = str_cpy(path, n, "\\");
        n = str_cpy(path, n, c->file);
        str_cpy(path, n, "_pc.ghg");
        if (write_disk(path, buf, len)) {
            c->ghg_mode = 2;
            mod_line("model copy written ", c->dir, 1, (DWORD)changed);
        }
    }
    VirtualFree(buf, 0, 0x8000);
}

static void scan_customs(void)
{
    char path[400], pat[400];
    FINDDATA fd, fd2;
    HANDLE f, g;
    unsigned n;
    n_customs = 0;
    if (!fbuf)
        fbuf = VirtualAlloc(0, FBUF_SIZE, MEM_COMMIT_RESERVE, 4);
    if (!fbuf)
        return;
    n = game_dir(pat);
    str_cpy(pat, n, "4p_mods\\chars\\*");
    f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE)
        return;
    do {
        struct custom *c;
        int i, len;
        if (!(fd.attr & 0x10) || fd.name[0] == '.' || fd.name[0] == '_')
            continue;
        if (n_customs >= MAX_CUSTOM)
            break;
        c = &customs[n_customs];
        for (i = 0; fd.name[i] && i < 63; i++)
            c->dir[i] = fd.name[i];
        c->dir[i] = 0;
        n = game_dir(pat);
        n = str_cpy(pat, n, "4p_mods\\chars\\");
        n = str_cpy(pat, n, c->dir);
        str_cpy(pat, n, "\\*.txt");
        c->file[0] = 0;
        /* the character .txt: several may be present (TEST.TXT, leftovers); prefer the
           one with a matching <name>_PC.GHG, named like the folder, not "TEST" */
        {
            int best = -1;
            g = FindFirstFileA(pat, &fd2);
            if (g != INVALID_HANDLE) {
                do {
                    char base[64], ghg[400];
                    int score = 0;
                    unsigned m;
                    if (fd2.attr & 0x10)
                        continue;
                    for (i = 0; fd2.name[i] && fd2.name[i] != '.' && i < 63; i++)
                        base[i] = fd2.name[i];
                    base[i] = 0;
                    m = game_dir(ghg);
                    m = str_cpy(ghg, m, "4p_mods\\chars\\");
                    m = str_cpy(ghg, m, c->dir);
                    m = str_cpy(ghg, m, "\\");
                    m = str_cpy(ghg, m, base);
                    str_cpy(ghg, m, "_PC.GHG");
                    {
                        DWORD at = GetFileAttributesA(ghg);
                        if (at != 0xFFFFFFFF && !(at & 0x10))
                            score += 4;
                        else if (ghg_like(c->dir, base, 0))
                            score += 4;
                    }
                    if (same_alnum(base, c->dir))
                        score += 2;
                    if (!same_i(base, "test"))
                        score += 1;
                    if (score > best) {
                        best = score;
                        str_cpy(c->file, 0, base);
                    }
                } while (FindNextFileA(g, &fd2));
                FindClose(g);
            }
        }
        /* icon: icon= in the .txt, and the folder's .GSC (prefer one with ICON in it) */
        c->icon[0] = c->gsc[0] = 0;
        if (c->file[0]) {
            int len;
            n = game_dir(path);
            n = str_cpy(path, n, "4p_mods\\chars\\");
            n = str_cpy(path, n, c->dir);
            n = str_cpy(path, n, "\\");
            n = str_cpy(path, n, c->file);
            str_cpy(path, n, ".txt");
            len = read_disk(path, fbuf, FBUF_SIZE - 1);
            if (len > 0)
                ini_value((char *)fbuf, len, "icon", c->icon, 64);
            n = game_dir(pat);
            n = str_cpy(pat, n, "4p_mods\\chars\\");
            n = str_cpy(pat, n, c->dir);
            str_cpy(pat, n, "\\*.gsc");
            g = FindFirstFileA(pat, &fd2);
            if (g != INVALID_HANDLE) {
                do {
                    int has_icon = 0;
                    for (i = 0; fd2.name[i]; i++)
                        if (starts_i(fd2.name + i, "icon"))
                            has_icon = 1;
                    if (!c->gsc[0] || has_icon) {
                        for (i = 0; fd2.name[i] && i < 63; i++)
                            c->gsc[i] = fd2.name[i];
                        c->gsc[i] = 0;
                    }
                    if (has_icon)
                        break;
                } while (FindNextFileA(g, &fd2));
                FindClose(g);
            }
        }
        if (!c->file[0])
            continue;
        c->icon_gen = 0;
        c->icon_scene = 0;
        fix_icon(c, n_customs);
        find_model(c);
        fix_model(c);
        /* display name: folder name with spaces before capitals, or 4p_char.ini name= */
        n = 0;
        for (i = 0; c->dir[i] && n < 60; i++) {
            if (i && c->dir[i] >= 'A' && c->dir[i] <= 'Z' && c->dir[i - 1] >= 'a' && c->dir[i - 1] <= 'z')
                c->name[n++] = ' ';
            c->name[n++] = c->dir[i] == '_' ? ' ' : c->dir[i];
        }
        c->name[n] = 0;
        c->price = 0;
        n = game_dir(path);
        n = str_cpy(path, n, "4p_mods\\chars\\");
        n = str_cpy(path, n, c->dir);
        str_cpy(path, n, "\\4p_char.ini");
        len = read_disk(path, fbuf, FBUF_SIZE - 1);
        if (len > 0) {
            char v[64];
            ini_value((char *)fbuf, len, "name", c->name, 64);
            if (ini_value((char *)fbuf, len, "price", v, 64))
                c->price = parse_int(v);
        }
        c->text_id = 0;
        n_customs++;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

/* text ids: the first unused ids from 352 on, judged by the language file in buf */
static void assign_ids(const BYTE *buf, int len)
{
    static BYTE used[4096];
    int i = 0, k, id = 352;
    if (ids_done)
        return;
    ids_done = 1;
    while (i < len) {
        while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == 0xEF || buf[i] == 0xBB || buf[i] == 0xBF))
            i++;
        if (i < len && is_digit((char)buf[i])) {
            int v = 0;
            while (i < len && is_digit((char)buf[i]))
                v = v * 10 + (buf[i++] - '0');
            if (v < 4096)
                used[v] = 1;
        }
        while (i < len && buf[i] != '\n')
            i++;
        i++;
    }
    for (k = 0; k < n_customs; k++) {
        while (id < 4096 && used[id])
            id++;
        customs[k].text_id = id;
        used[id] = 1;
    }
}


static int count_chars(const BYTE *b, int len)
{
    int i = 0, c = 0;
    while (i + 10 <= len) {
        if ((i == 0 || b[i - 1] == '\n') && starts_i((const char *)b + i, "char_start"))
            c++;
        while (i < len && b[i] != '\n')
            i++;
        i++;
    }
    return c;
}

/* build 4p_mods\_gen\<name> from the game's copy plus the custom characters */
static int gen_list(const char *name, int kind, char *out)
{
    char path[400], line[300];
    int len, k, base = 0, limit;
    unsigned n;
    if (!fbuf)
        return 0;
    len = read_game(name, fbuf, FBUF_SIZE - 0x10000);
    if (len <= 0)
        return 0;
    if (kind == 2)
        assign_ids(fbuf, len);
    if (kind == 0)
        base = count_chars(fbuf, len);
    limit = BASE_CHAR_MAX - base;
    for (k = 0; k < n_customs; k++) {
        struct custom *c = &customs[k];
        if (kind != 2 && k >= limit) {
            if (kind == 0)
                log_msg("custom character over the 340 limit, skipped:", 0, 0);
            continue;
        }
        if (kind == 0)
            customs_added = k + 1;
        n = 0;
        if (kind == 0) {
            n = str_cpy(line, n, "\r\nchar_start\r\n\tdir \"");
            n = str_cpy(line, n, c->dir);
            n = str_cpy(line, n, "\"\r\n\tfile \"");
            n = str_cpy(line, n, c->file);
            n = str_cpy(line, n, "\"\r\nchar_end\r\n");
        } else if (kind == 1) {
            n = str_cpy(line, n, "\r\ncollect \"");
            {
                const char *p = c->file;
                while (*p)
                    line[n++] = lower(*p++);
                line[n] = 0;
            }
            n = str_cpy(line, n, "\"");
            if (c->price > 0) {
                n = str_cpy(line, n, " buy_in_shop ");
                n = put_int(line, n, c->price);
            }
            n = str_cpy(line, n, "\r\n");
        } else {
            if (!c->text_id)
                continue;
            n = str_cpy(line, n, "\r\n");
            n = put_int(line, n, c->text_id);
            n = str_cpy(line, n, " \"");
            n = str_cpy(line, n, c->name);
            n = str_cpy(line, n, "\"\r\n");
        }
        {
            unsigned j = 0;
            while (line[j] && len < FBUF_SIZE)
                fbuf[len++] = (BYTE)line[j++];
        }
    }
    n = game_dir(path);
    n = str_cpy(path, n, "4p_mods\\_gen\\");
    str_cpy(path, n, name);
    if (!write_disk(path, fbuf, len))
        return 0;
    n = str_cpy(out, 0, "4p_mods\\_gen\\");
    str_cpy(out, n, name);
    return 1;
}

/* the custom character's own .txt with name_id set to its text id */
static int gen_char_txt(struct custom *c, const char *name, char *out)
{
    char path[400];
    int len, i, o = 0, done = 0;
    unsigned n;
    BYTE *dst;
    n = game_dir(path);
    n = str_cpy(path, n, "4p_mods\\");
    str_cpy(path, n, name);
    len = read_disk(path, fbuf, FBUF_SIZE / 2);
    if (len <= 0 || !c->text_id)
        return 0;
    dst = fbuf + FBUF_SIZE / 2;
    for (i = 0; i < len;) {
        if ((i == 0 || fbuf[i - 1] == '\n') && starts_i((const char *)fbuf + i, "name_id")) {
            char t[16];
            unsigned m = str_cpy((char *)dst + o, 0, "name_id=");
            o += m;
            m = put_int(t, 0, c->text_id);
            o += str_cpy((char *)dst + o, 0, t);
            while (i < len && fbuf[i] != '\r' && fbuf[i] != '\n')
                i++;
            done = 1;
            continue;
        }
        if (c->icon_gen && (i == 0 || fbuf[i - 1] == '\n') && starts_i((const char *)fbuf + i, "icon") &&
            (fbuf[i + 4] == '=' || fbuf[i + 4] == ' ' || fbuf[i + 4] == '\t')) {
            o += str_cpy((char *)dst + o, 0, "icon=\"");
            o += str_cpy((char *)dst + o, 0, c->icon);
            o += str_cpy((char *)dst + o, 0, "\"");
            while (i < len && fbuf[i] != '\r' && fbuf[i] != '\n')
                i++;
            continue;
        }
        if (o < FBUF_SIZE / 2 - 64)
            dst[o++] = fbuf[i];
        i++;
    }
    if (!done) {
        char t[16];
        o += str_cpy((char *)dst + o, 0, "\r\nname_id=");
        put_int(t, 0, c->text_id);
        o += str_cpy((char *)dst + o, 0, t);
        o += str_cpy((char *)dst + o, 0, "\r\n");
    }
    n = game_dir(path);
    n = str_cpy(path, n, "4p_mods\\_gen\\");
    str_cpy(path, n, name);
    if (!write_disk(path, dst, o))
        return 0;
    n = str_cpy(out, 0, "4p_mods\\_gen\\");
    str_cpy(out, n, name);
    return 1;
}

static int disk_file(const char *rel)   /* rel relative to the game folder */
{
    char path[400];
    unsigned n = game_dir(path);
    DWORD a;
    if (!idx_state)
        idx_build();
    if (idx_state == 1)
        return idx_find(rel);
    str_cpy(path, n, rel);
    a = GetFileAttributesA(path);
    return a != 0xFFFFFFFF && !(a & 0x10);
}


/* free-form line to 4p_mod.log, independent of log_msg's 60-line cap */
static int served_lines, mod_lines;
static void mod_line(const char *a, const char *b, int has_val, DWORD v)
{
    char line[300];
    unsigned n = 0;
    DWORD w;
    HANDLE h;
    if (!log_path[0] || mod_lines >= 400)
        return;
    mod_lines++;
    while (*a && n < 120)
        line[n++] = *a++;
    while (b && *b && n < 260)
        line[n++] = *b++;
    if (has_val) {
        int i;
        line[n++] = ' ';
        for (i = 7; i >= 0; i--)
            line[n++] = "0123456789ABCDEF"[(v >> (i * 4)) & 15];
    }
    line[n++] = '\r';
    line[n++] = '\n';
    h = CreateFileA(log_path, 4, 1, 0, 4, 0x80, 0);
    if (h == INVALID_HANDLE)
        return;
    WriteFile(h, line, n, &w, 0);
    CloseHandle(h);
}

static int __cdecl file_hook(const char *name, char *out)
{
    char rel[400];
    unsigned n;
    int k, r = 0;
    DWORD tid, t0;
    if (!name || !*name || !log_path[0])
        return 0;
    /* the game opens files on two threads (the main one and the loading thread that
       brings Cantina visitors in). One at a time here: the other thread waits instead
       of being sent to the .DAT (a custom file it would not find there). The same
       thread coming back in (read_game) reads the original. */
    tid = GetCurrentThreadId();
    if (fh_owner == tid)
        return 0;
    t0 = GetTickCount();
    while (__sync_lock_test_and_set(&fh_lock, 1)) {
        fh_waits++;
        Sleep(0);
    }
    fh_owner = tid;
    fh_opens++;
    if (main_tid && tid != main_tid)
        other_tid = tid;
    last_open_tid = tid;
    {
        unsigned k;
        for (k = 0; name[k] && k < sizeof last_open - 1; k++)
            last_open[k] = name[k];
        last_open[k] = 0;
    }
    while (*name == '\\' || *name == '/')
        name++;
    if (n_customs < 0) {
        scan_customs();
        for (k = 0; k < n_customs; k++)
            log_msg(customs[k].dir, 0, 0);
        if (n_customs > 0)
            log_msg("custom characters:", 1, (DWORD)n_customs);
    }
    if (n_customs > 0) {
        if (same_i(name, "chars\\chars.txt"))
            r = gen_list(name, 0, out);
        else if (same_i(name, "chars\\collection.txt"))
            r = gen_list(name, 1, out);
        else if (starts_i(name, "stuff\\text\\") && ends_with(name, ".txt") &&
                 !ends_with(name, "badwords.txt"))
            r = gen_list(name, 2, out);
        if (r)
            goto done;
        if (starts_i(name, "chars\\")) {
            for (k = 0; k < n_customs; k++) {
                struct custom *c = &customs[k];
                int m = starts_i(name + 6, c->dir);
                if (m && (name[6 + m] == '\\' || name[6 + m] == '/') &&
                    ends_with(name, ".txt") && starts_i(name + 7 + m, c->file) &&
                    same_i(name + 7 + m + starts_i(name + 7 + m, c->file), ".txt")) {
                    r = gen_char_txt(c, name, out);
                    if (r)
                        goto done;
                }
                if (m && c->ghg_mode && (name[6 + m] == '\\' || name[6 + m] == '/') &&
                    starts_i(name + 7 + m, c->file) &&
                    same_i(name + 7 + m + starts_i(name + 7 + m, c->file), "_pc.ghg")) {
                    if (c->ghg_mode == 2) {
                        n = str_cpy(rel, 0, "4p_mods\\_gen\\models\\");
                        n = str_cpy(rel, n, c->dir);
                        n = str_cpy(rel, n, "\\");
                        n = str_cpy(rel, n, c->file);
                        str_cpy(rel, n, "_pc.ghg");
                    } else {
                        n = str_cpy(rel, 0, "4p_mods\\chars\\");
                        n = str_cpy(rel, n, c->dir);
                        n = str_cpy(rel, n, "\\");
                        str_cpy(rel, n, c->ghg);
                    }
                    if (disk_file(rel)) {
                        str_cpy(out, 0, rel);
                        r = 1;
                        goto done;
                    }
                }
            }
        }
        k = starts_i(name, "stuff\\icons\\");
        if (k) {
            int j;
            for (j = 0; j < n_customs; j++) {
                n = str_cpy(rel, 0, "4p_mods\\chars\\");
                n = str_cpy(rel, n, customs[j].dir);
                n = str_cpy(rel, n, "\\");
                str_cpy(rel, n, name + k);
                if (disk_file(rel)) {
                    str_cpy(out, 0, rel);
                    r = 1;
                    goto done;
                }
            }
            /* by icon name: stuff\\icons\\<icon>_pc.gsc -> the folder's .GSC file */
            for (j = 0; j < n_customs; j++) {
                const char *ic = customs[j].icon, *p = name + k;
                int m = 0;
                if (!ic[0] || !customs[j].gsc[0])
                    continue;
                while (ic[m] && lower(ic[m]) == lower(p[m]))
                    m++;
                if (ic[m] || !(same_i(p + m, "_pc.gsc") || same_i(p + m, ".gsc")))
                    continue;
                if (customs[j].icon_gen) {
                    n = str_cpy(rel, 0, "4p_mods\\_gen\\icons\\");
                    n = str_cpy(rel, n, customs[j].icon);
                    str_cpy(rel, n, "_pc.gsc");
                } else {
                    n = str_cpy(rel, 0, "4p_mods\\chars\\");
                    n = str_cpy(rel, n, customs[j].dir);
                    n = str_cpy(rel, n, "\\");
                    str_cpy(rel, n, customs[j].gsc);
                }
                if (disk_file(rel)) {
                    str_cpy(out, 0, rel);
                    r = 1;
                    goto done;
                }
            }
        }
    }
    n = str_cpy(rel, 0, "4p_mods\\");
    str_cpy(rel, n, name);
    if (disk_file(rel)) {
        str_cpy(out, 0, rel);
        r = 1;
    }
done:
    if (r && served_lines < 150 && !ends_with(out, ".an3") && !ends_with(out, ".bsa")) {
        served_lines++;
        mod_line("served ", out, 0, 0);
    }
    fh_ms += GetTickCount() - t0;
    fh_owner = 0;
    __sync_lock_release(&fh_lock);
    return r;
}

/* ---------- Cantina spawn watch: what stops more characters from walking in ----------
   The Cantina loads one more unlocked character at a time (0x419FB0) while: the
   animation-set table has room ([0x29130E4]+0x2C used / +0x14 max), the level heap has
   a reserve free ([0x924894]-[0x924890]), and fewer than 253 scene objects exist
   ([0x9739B8]). One line whenever one of these changes, up to 80 lines. */
static int spawn_lines;
static DWORD spawn_last[4];

/* custom character check: once the character table exists, log the last entries */
static int chars_logged;
/* ---------- custom character icons, loaded once and kept ----------
   Base characters' pictures live in stuff\things.gsc, loaded once at start-up into the
   permanent heap. A custom character's picture lives in its own icon file, which the
   game only loads into the level heap for characters that are in the party or walking
   around (0x55B6E0), and the Free Play grid resolves its pictures when a level loads
   (0x57A000) from whatever happens to be loaded then: so custom pictures came and went.
   Right after things.gsc loads, every custom icon file is loaded the same way into a
   heap of our own that is never reset, and the game's "icon file of character n"
   (0x55B6C0) returns it for custom characters. */
static BYTE *icon_heap;
static DWORD icon_cur, icon_end;
static int icons_preloaded;

static void __cdecl icon_preload(void)
{
    char name[200];
    unsigned n;
    int k, ok = 0;
    if (icons_preloaded)
        return;
    icons_preloaded = 1;
    if (n_customs < 0)
        scan_customs();
    if (n_customs <= 0)
        return;
    icon_heap = VirtualAlloc(0, 0x300000 * (unsigned)n_customs, MEM_COMMIT_RESERVE, 4);
    if (!icon_heap) {
        log_msg("custom icons: no memory", 0, 0);
        return;
    }
    icon_cur = (DWORD)icon_heap;
    icon_end = icon_cur + 0x300000 * (unsigned)n_customs;
    for (k = 0; k < n_customs; k++) {
        struct custom *c = &customs[k];
        BYTE *sc;
        if (!c->icon[0] || !c->gsc[0])
            continue;
        n = str_cpy(name, 0, "stuff\\icons\\");
        n = str_cpy(name, n, c->icon);
        str_cpy(name, n, ".gsc");
        icon_cur = (icon_cur + 15) & ~15u;
        sc = ((BYTE *(__cdecl *)(DWORD *, DWORD, const char *))0x5EF7F0)(&icon_cur, icon_end, name);
        c->icon_scene = sc;
        if (sc) {
            if (*(BYTE **)(sc + 0x110))
                *(DWORD *)(*(BYTE **)(sc + 0x110) + 0x74) |= 0x10;
            ok++;
        } else
            mod_line("custom icon did not load: ", c->dir, 0, 0);
    }
    mod_line("custom icons kept loaded", 0, 1, (DWORD)ok);
    mod_line("custom icon heap used", 0, 1, icon_cur - (DWORD)icon_heap);
}

static void *__cdecl icon_for_char(int ch)
{
    int count = *(int *)0x93B278, k;
    if (customs_added <= 0 || count < customs_added)
        return 0;
    k = ch - (count - customs_added);
    if (k < 0 || k >= customs_added || k >= n_customs)
        return 0;
    return customs[k].icon_scene;
}

static void custom_watch(void)
{
    int count = *(int *)0x93B278, k, i;
    BYTE *tab = *(BYTE **)0x93B274;
    BYTE *flags = *(BYTE **)0x93B288;
    if (n_customs <= 0 || !tab || count <= 0 || !*(void **)0x951BB8)
        return;
    /* custom characters without a price are unlocked from the start: owned (1) and
       available (2), re-applied every frame because loading a save replaces the flags */
    if (flags && customs_added > 0 && count >= customs_added) {
        for (k = 0; k < customs_added; k++) {
            int idx = count - customs_added + k;
            flags[idx] |= customs[k].price > 0 ? 2 : 3;
        }
    }
    if (chars_logged)
        return;
    chars_logged = 1;
    for (k = count - n_customs - 1; k < count; k++) {
        BYTE *c = tab + k * 0x4C;
        char hex[0x4C * 2 + 1];
        int icon = *(short *)(c + 0x42), tid = *(int *)c;
        for (i = 0; i < 0x4C; i++) {
            hex[i * 2] = "0123456789ABCDEF"[c[i] >> 4];
            hex[i * 2 + 1] = "0123456789ABCDEF"[c[i] & 15];
        }
        hex[0x4C * 2] = 0;
        mod_line("char ", hex, 1, (DWORD)k);
        if (*(char **)(c + 0xC))
            mod_line("  name string ", *(char **)(c + 0xC), 0, 0);
        if (tid >= 0 && tid < 4096 && *(char ***)0x926C20 && (*(char ***)0x926C20)[tid])
            mod_line("  text ", (*(char ***)0x926C20)[tid], 0, 0);
        if (icon >= 0 && icon < *(int *)0x951BBC) {
            BYTE *e = *(BYTE **)0x951BB8 + icon * 8;
            mod_line("  icon ", *(char **)(e + 4), 1, e[0]);
        } else
            mod_line("  icon index", 0, 1, (DWORD)icon);
        if (flags)
            mod_line("  flags", 0, 1, flags[k]);
        mod_line("  owned", 0, 1, ((DWORD(__cdecl *)(int))0x594960)(k));
    }
    mod_line("icons", 0, 1, *(DWORD *)0x951BBC);
}

static void spawn_watch(void)
{
    custom_watch();
    BYTE *pool = *(BYTE **)0x29130E4;
    DWORD cur[4];
    char line[150];
    unsigned n = 0;
    int k;
    DWORD w;
    HANDLE h;
    static const char *lbl[4] = { "spawn animsets ", " max ", " heapfree ", " objects " };
    if (!pool || spawn_lines >= 80 || !log_path[0])
        return;
    cur[0] = *(DWORD *)(pool + 0x2C);
    cur[1] = *(DWORD *)(pool + 0x14);
    cur[2] = (*(DWORD *)0x924894 - *(DWORD *)0x924890) >> 16;     /* 64 KB units */
    cur[3] = *(DWORD *)0x9739B8;
    if (same((BYTE *)cur, (BYTE *)spawn_last, sizeof cur))
        return;
    for (k = 0; k < 4; k++)
        spawn_last[k] = cur[k];
    spawn_lines++;
    for (k = 0; k < 4; k++) {
        const char *p = lbl[k];
        while (*p)
            line[n++] = *p++;
        put_hex2(line, &n, cur[k], k == 2 ? 4 : 3);
    }
    {
        const char *p = " chars ";
        while (*p)
            line[n++] = *p++;
        put_hex2(line, &n, *(DWORD *)0x93B278, 3);
    }
    line[n++] = '\r';
    line[n++] = '\n';
    h = CreateFileA(log_path, 4, 1, 0, 4, 0x80, 0);
    if (h == INVALID_HANDLE)
        return;
    WriteFile(h, line, n, &w, 0);
    CloseHandle(h);
}

/* ---------- pause log: files opened through the mod ----------
   Parts of loader.c's pause log (stall_watch) and watchdog lines. */
static unsigned sample_thread(char *line, unsigned n, DWORD tid, const char *label);

static unsigned pause_files(char *line, unsigned n)
{
    n = str_cpy(line, n, ", file opens ");
    n = put_int(line, n, (int)fh_opens);
    n = str_cpy(line, n, " (");
    n = put_int(line, n, (int)fh_ms);
    n = str_cpy(line, n, " ms in the mod, waits ");
    n = put_int(line, n, (int)fh_waits);
    return str_cpy(line, n, ")");
}

static void pause_files_reset(void)
{
    fh_opens = fh_waits = fh_ms = 0;
}

/* the loading thread's position and the last file opened through the mod */
static unsigned sample_files(char *line, unsigned n)
{
    unsigned k;
    n = sample_thread(line, n, other_tid, " | loading ");
    n = str_cpy(line, n, " | opens ");
    n = put_int(line, n, (int)fh_opens);
    n = str_cpy(line, n, last_open_tid == main_tid ? ", last (main) " : ", last (loading) ");
    for (k = 0; last_open[k] && n < 390; k++)
        line[n++] = last_open[k];
    return n;
}
