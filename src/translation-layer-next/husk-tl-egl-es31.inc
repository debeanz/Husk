/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GLES 3.1 on a 3.0 driver (included by husk-tl-egl.c, which supplies a_glGetIntegerv / a_eglGetProcAddress).
 *
 * ANGLE over Metal stops at OpenGL ES 3.0, and Minecraft's Android materials hold only "#version 310 es" shaders, chosen by
 * reporting 3.1. For a game that asks for it (tl_egl_es31_shim) the driver is described as 3.1 and each such shader is written
 * down to 300 es: the version line changes, `layout(binding = N)` -- which 3.0 does not have -- is dropped and applied after
 * the link as glUniform1i / glUniformBlockBinding, a varying's `layout(location = N)` becomes a name both stages share
 * (es31_varying), and a buffer texture becomes an array texture filled from the buffer (es31_tb). What 3.1 really adds (compute,
 * images, storage buffers written by shaders) is not there.
 */
static bool g_es31_shim;
void tl_egl_es31_shim(bool on) { g_es31_shim = on; }

/* Buffer textures as array textures (es31_tb, below; the shader side in es31_downgrade). */
#define TB_TARGET 0x8C2A                /* GL_TEXTURE_BUFFER, as a texture target and as a buffer target */
#define TB_ARRAY 0x8C1A                 /* GL_TEXTURE_2D_ARRAY */
#define TB_W 4096                       /* texels to a row */
#define TB_MAX 512

typedef struct { char name[72]; int binding; bool block; } es31_bind;
#define ES31_SHADERS 1024
#define ES31_BINDS 12
#define ES31_ATTACHED 2048
static struct { unsigned shader; int n; es31_bind b[ES31_BINDS]; } g_es31_sh[ES31_SHADERS];
static struct { unsigned prog, shader; } g_es31_att[ES31_ATTACHED];
static int g_es31_natt;
static pthread_mutex_t g_es31_mu = PTHREAD_MUTEX_INITIALIZER;

/* "OpenGL ES 3.0 ..." -> "OpenGL ES 3.1 ...", "OpenGL ES GLSL ES 3.00 ..." -> "... 3.10 ..." (a copy: the driver's string is not ours to edit) */
static const char *es31_fix(const char *s, bool glsl)
{
    static char ring[8][300]; static atomic_uint k;
    char *out = ring[atomic_fetch_add(&k, 1) % 8];
    snprintf(out, 300, "%s", s);
    if (glsl) { char *p = strstr(out, "GLSL ES 3.00"); if (p) { p[10] = '1'; p[11] = '0'; } }
    else { char *p = strstr(out, "OpenGL ES 3.0"); if (p) p[12] = '1'; }
    return out;
}

static bool es31_ident(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* Whether `word` stands on its own in `s` (so "in" is not found in "inout" or "int"). */
static bool es31_has_word(const char *s, const char *word)
{
    size_t wl = strlen(word);
    for (const char *p = strstr(s, word); p; p = strstr(p + 1, word))
        if ((p == s || !es31_ident(p[-1])) && !es31_ident(p[wl])) return true;
    return false;
}

/*
 * A varying -- a vertex shader's output, a fragment shader's input -- with `layout(location = N)`. 3.1 matches the two stages' varyings
 * by location, so their names may differ (Unreal's do: out_TEXCOORD0 and in_TEXCOORD0); 3.0 has no location on them and matches by
 * name. So the declaration loses its layout and takes a name made of its location, the same in both stages, and a #define after it
 * turns the shader's own name into that one. Writes it at out + *o and says so; a declaration it cannot read is left alone.
 */
static bool es31_varying(char *out, size_t *o, const char *decl, size_t dl, const char *q)
{
    const char *eq = strchr(q, '=');
    int loc = eq ? atoi(eq + 1) : -1;
    if (loc < 0 || strchr(q, ',')) return false;           /* location and nothing else */
    size_t e = dl;
    while (e && (decl[e - 1] == ' ' || decl[e - 1] == '\n' || decl[e - 1] == '\t' || decl[e - 1] == '\r')) e--;
    size_t arr = e;                                         /* an array's [N] stays after the new name */
    while (e && decl[e - 1] == ']') { while (e && decl[e - 1] != '[') e--; if (e) e--; }
    while (e && (decl[e - 1] == ' ' || decl[e - 1] == '\t')) e--;
    size_t st = e; while (st && es31_ident(decl[st - 1])) st--;
    if (e <= st || e - st > 120) return false;
    char name[128]; memcpy(name, decl + st, e - st); name[e - st] = 0;
    *o += (size_t)sprintf(out + *o, "%.*shusk_v%d%.*s;\n#define %s husk_v%d\n", (int)st, decl, loc, (int)(arr - e), decl + e, name, loc);
    return true;
}

/*
 * The 300 es text of a 310 es shader (malloc'd), with the bindings it dropped in rec[0..*nrec). A 300 es shader is given the same
 * treatment but keeps its version line: an engine that took the driver for 3.1 writes 3.1's qualifiers into the 300 es shaders it
 * makes too. Anything else is returned as it came.
 */
static char *es31_downgrade(const char *src, size_t n, es31_bind *rec, int *nrec)
{
    char *out = malloc(2 * n + 8192); size_t o = 0;
    *nrec = 0;
    const char *ver = strstr(src, "#version 310 es");
    if (!ver && !strstr(src, "#version 300 es")) { memcpy(out, src, n); out[n] = 0; return out; }
    const bool vertex = strstr(src, "gl_Position") != NULL;     /* only a vertex shader writes it */
    /* Buffer textures (es31_tb): read from an array texture, TB_W texels to a row. */
    const bool buffers = strstr(src, "samplerBuffer") != NULL;
    static const char tb_macro[] = "#define husk_tb(i) ivec3(int(i) & 4095, int(i) >> 12, 0)";
    _Static_assert(TB_W == 4096, "husk_tb's mask and shift are TB_W's");
    for (size_t i = 0; i < n;) {
        if (ver && src + i == ver) {
            memcpy(out + o, "#version 300 es", 15); o += 15; i += 15;
            while (i < n && src[i] != '\n') out[o++] = src[i++];
            if (buffers) { out[o++] = '\n'; memcpy(out + o, tb_macro, sizeof(tb_macro) - 1); o += sizeof(tb_macro) - 1; }
            continue;
        }
        /* A preprocessor line is the preprocessor's (a #define of a layout is not a declaration): as it is -- except that the buffer
         * textures' extension is not asked for (they are not buffer textures here), and the 300 es version line is followed by the macro
         * they are read through. */
        if ((i == 0 || src[i - 1] == '\n') && src[i] == '#') {
            size_t e = i; while (e < n && src[e] != '\n') e++;
            bool tb_ext = buffers && e - i > 10 && !strncmp(src + i, "#extension", 10) && memmem(src + i, e - i, "texture_buffer", 14);
            bool version = !strncmp(src + i, "#version", 8);
            if (!tb_ext) { memcpy(out + o, src + i, e - i); o += e - i; }
            i = e;
            if (version && buffers) { out[o++] = '\n'; memcpy(out + o, tb_macro, sizeof(tb_macro) - 1); o += sizeof(tb_macro) - 1; }
            continue;
        }
        if (buffers && (i == 0 || !es31_ident(src[i - 1]))) {
            /* [iu]samplerBuffer -> [iu]sampler2DArray */
            size_t pre = (src[i] == 'i' || src[i] == 'u') ? 1 : 0;
            if (i + pre + 13 <= n && !strncmp(src + i + pre, "samplerBuffer", 13) && (i + pre + 13 == n || !es31_ident(src[i + pre + 13]))) {
                if (pre) out[o++] = src[i];
                memcpy(out + o, "sampler2DArray", 14); o += 14; i += pre + 13; continue;
            }
            /* texelFetch(s, i) -- two arguments only ever name a buffer -- -> texelFetch(s, husk_tb(i), 0) */
            if (i + 11 <= n && !strncmp(src + i, "texelFetch(", 11)) {
                size_t j = i + 11, comma = 0; int depth = 1, commas = 0;
                while (j < n && depth) {
                    if (src[j] == '(' || src[j] == '[') depth++;
                    else if (src[j] == ')' || src[j] == ']') depth--;
                    else if (src[j] == ',' && depth == 1) { if (!commas) comma = j; commas++; }
                    if (depth) j++;
                }
                if (j < n && commas == 1) {
                    o += (size_t)sprintf(out + o, "texelFetch(%.*s, husk_tb(%.*s), 0)", (int)(comma - (i + 11)), src + i + 11, (int)(j - comma - 1), src + comma + 1);
                    i = j + 1; continue;
                }
            }
        }
        /* Unreal's INTERFACE_LOCATION(N): its shaders' #define makes it layout(location = N) when the driver has separate shader
         * objects, which 3.1 does. On a varying it gets what a written-out layout(location = N) gets; anywhere else it stays. */
        if (i + 19 <= n && !strncmp(src + i, "INTERFACE_LOCATION(", 19) && (i == 0 || !es31_ident(src[i - 1]))) {
            size_t j = i + 19; while (j < n && src[j] != ')') j++;
            if (j < n) {
                char q[48]; size_t ql = j - (i + 19); if (ql > 30) ql = 30;
                int ql_n = snprintf(q, sizeof(q), "location=%.*s", (int)ql, src + i + 19); (void)ql_n;
                size_t d = j + 1; while (d < n && src[d] != ';' && src[d] != '{') d++;
                char decl[200]; size_t dl = d - (j + 1); if (dl >= sizeof(decl)) dl = sizeof(decl) - 1;
                memcpy(decl, src + j + 1, dl); decl[dl] = 0;
                if (d < n && src[d] == ';' && es31_has_word(decl, vertex ? "out" : "in") && es31_varying(out, &o, decl, dl, q)) { i = d + 1; continue; }
            }
        }
        size_t lp = i + 6;                                          /* "layout" (and any spaces) then "(" */
        while (lp < n && src[lp] == ' ') lp++;
        if (i + 7 <= n && !strncmp(src + i, "layout", 6) && lp < n && src[lp] == '(' && (i == 0 || !es31_ident(src[i - 1]))) {
            lp++;                                                   /* just after the "(" */
            size_t j = lp; int depth = 1;
            while (j < n && depth) { if (src[j] == '(') depth++; else if (src[j] == ')') depth--; j++; }
            char q[160]; size_t ql = j - 1 - lp; if (ql >= sizeof(q)) ql = sizeof(q) - 1;
            memcpy(q, src + lp, ql); q[ql] = 0;
            char *b = strstr(q, "binding");
            size_t d = j; while (d < n && src[d] != ';' && src[d] != '{') d++;
            char decl[200]; size_t dl = d - j; if (dl >= sizeof(decl)) dl = sizeof(decl) - 1;
            memcpy(decl, src + j, dl); decl[dl] = 0;
            bool opaque = strstr(decl, "image") || strstr(decl, "atomic_uint");
            bool ssbo = !opaque && strstr(decl, "buffer") && d < n && src[d] == '{' && !strstr(decl, "writeonly") && !strstr(decl, "coherent");
            if (ssbo && b) {
                /* A read-only storage block of structs becomes a uniform block of 1024 of them (std140 and std430 lay these structs out alike
                 * when their members are 4-byte scalars); the game's glBindBufferBase on the storage target is redirected to the uniform one. */
                size_t close = d; int depth2 = 0;
                while (close < n) { if (src[close] == '{') depth2++; else if (src[close] == '}' && --depth2 == 0) break; close++; }
                char *eq = strchr(b, '='); int val = eq ? atoi(eq + 1) : -1;
                size_t e = dl;
                while (e && (decl[e - 1] == ' ' || decl[e - 1] == '\n' || decl[e - 1] == '\t' || decl[e - 1] == '\r')) e--;
                size_t st = e; while (st && es31_ident(decl[st - 1])) st--;
                if (close < n && val >= 0 && e > st && e - st < sizeof(rec[0].name) && *nrec < ES31_BINDS) {
                    memcpy(rec[*nrec].name, decl + st, e - st); rec[*nrec].name[e - st] = 0; rec[*nrec].binding = val; rec[*nrec].block = true; (*nrec)++;
                    o += (size_t)sprintf(out + o, "layout(std140) uniform %.*s\n{", (int)(e - st), decl + st);
                    for (size_t k = d + 1; k < close; k++) {
                        if (src[k] == '[' && k + 1 < close && src[k + 1] == ']') { o += (size_t)sprintf(out + o, "[1024]"); k++; }
                        else out[o++] = src[k];
                    }
                    out[o++] = '}';
                    i = close + 1; continue;
                }
            }
            if (b && !opaque && !ssbo) {
                char *eq = strchr(b, '='); int val = eq ? atoi(eq + 1) : -1;
                bool block = d < n && src[d] == '{';
                if (*nrec < ES31_BINDS && val >= 0) {
                    size_t e = dl;
                    while (e && (decl[e - 1] == ' ' || decl[e - 1] == '\n' || decl[e - 1] == '\t' || decl[e - 1] == '\r')) e--;
                    while (e && decl[e - 1] == ']') { while (e && decl[e - 1] != '[') e--; if (e) e--; }
                    size_t st = e; while (st && es31_ident(decl[st - 1])) st--;
                    if (e > st && e - st < sizeof(rec[0].name)) {
                        memcpy(rec[*nrec].name, decl + st, e - st); rec[*nrec].name[e - st] = 0;
                        rec[*nrec].binding = val; rec[*nrec].block = block; (*nrec)++;
                    }
                }
                /* the layout again, without the binding qualifier */
                char keep[160]; size_t kl = 0; const char *tok = q;
                while (tok) {
                    const char *comma = strchr(tok, ',');
                    size_t tl = comma ? (size_t)(comma - tok) : strlen(tok);
                    const char *t = tok; size_t tn = tl; while (tn && *t == ' ') { t++; tn--; }
                    if (!(tn >= 7 && !strncmp(t, "binding", 7)) && tn) { if (kl) keep[kl++] = ','; memcpy(keep + kl, t, tn); kl += tn; }
                    tok = comma ? comma + 1 : NULL;
                }
                if (kl) { memcpy(out + o, "layout(", 7); o += 7; memcpy(out + o, keep, kl); o += kl; out[o++] = ')'; }
                i = j; continue;
            }
            /* layout(location) on a varying (a vertex shader's `out`, a fragment shader's `in`); a vertex input's and a fragment output's
             * location is 3.0 too, and stays */
            if (!b && strstr(q, "location") && d < n && src[d] == ';'
                && es31_has_word(decl, vertex ? "out" : "in") && es31_varying(out, &o, decl, dl, q)) { i = d + 1; continue; }
            memcpy(out + o, src + i, j - i); o += j - i; i = j; continue;
        }
        out[o++] = src[i++];
    }
    out[o] = 0;
    return out;
}

static void (*r_glShaderSource)(unsigned, int, const char *const *, const int *);
static void w_glShaderSource(unsigned sh, int count, const char *const *str, const int *len)
{
    if (!g_es31_shim || count <= 0 || !str) { r_glShaderSource(sh, count, str, len); return; }
    size_t total = 0;
    for (int i = 0; i < count; i++) total += str[i] ? (len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i])) : 0;
    char *cat = malloc(total + 1); size_t k = 0;
    for (int i = 0; i < count; i++) if (str[i]) { size_t l = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(cat + k, str[i], l); k += l; }
    cat[k] = 0;
    es31_bind rec[ES31_BINDS]; int nrec = 0;
    char *down = es31_downgrade(cat, k, rec, &nrec);
    pthread_mutex_lock(&g_es31_mu);
    int slot = (int)(sh % ES31_SHADERS);
    g_es31_sh[slot].shader = sh; g_es31_sh[slot].n = nrec; memcpy(g_es31_sh[slot].b, rec, sizeof(es31_bind) * (size_t)nrec);
    pthread_mutex_unlock(&g_es31_mu);
    const char *one = down; int one_len = (int)strlen(down);
    r_glShaderSource(sh, 1, &one, &one_len);
    free(cat); free(down);
}

static void (*r_glAttachShader)(unsigned, unsigned);
static void w_glAttachShader(unsigned prog, unsigned sh)
{
    r_glAttachShader(prog, sh);
    if (!g_es31_shim) return;
    pthread_mutex_lock(&g_es31_mu);
    g_es31_att[g_es31_natt % ES31_ATTACHED].prog = prog; g_es31_att[g_es31_natt % ES31_ATTACHED].shader = sh; g_es31_natt++;
    pthread_mutex_unlock(&g_es31_mu);
}

static void (*r_glLinkProgram)(unsigned);
static void w_glLinkProgram(unsigned prog)
{
    r_glLinkProgram(prog);
    if (!g_es31_shim || !a_eglGetProcAddress) return;
    static int (*getloc)(unsigned, const char *); static unsigned (*getblk)(unsigned, const char *);
    static void (*blkbind)(unsigned, unsigned, unsigned), (*uni1i)(int, int), (*useprog)(unsigned), (*getprogiv)(unsigned, unsigned, int *);
    if (!getloc) {
        getloc = a_eglGetProcAddress("glGetUniformLocation"); getblk = a_eglGetProcAddress("glGetUniformBlockIndex");
        blkbind = a_eglGetProcAddress("glUniformBlockBinding"); uni1i = a_eglGetProcAddress("glUniform1i");
        useprog = a_eglGetProcAddress("glUseProgram"); getprogiv = a_eglGetProcAddress("glGetProgramiv");
    }
    if (!getloc || !getblk || !blkbind || !uni1i || !useprog || !getprogiv) return;
    int ok = 0; getprogiv(prog, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (!ok) return;
    int cur = 0; a_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &cur);
    bool used = false;
    pthread_mutex_lock(&g_es31_mu);
    int total = g_es31_natt < ES31_ATTACHED ? g_es31_natt : ES31_ATTACHED;
    for (int a = 0; a < total; a++) {
        if (g_es31_att[a].prog != prog) continue;
        unsigned sh = g_es31_att[a].shader;
        int slot = (int)(sh % ES31_SHADERS);
        if (g_es31_sh[slot].shader != sh) continue;
        for (int b = 0; b < g_es31_sh[slot].n; b++) {
            const es31_bind *e = &g_es31_sh[slot].b[b];
            if (e->block) { unsigned idx = getblk(prog, e->name); if (idx != 0xFFFFFFFFu) blkbind(prog, idx, (unsigned)e->binding); }
            else { int loc = getloc(prog, e->name); if (loc >= 0) { if (!used) { useprog(prog); used = true; } uni1i(loc, e->binding); } }
        }
    }
    pthread_mutex_unlock(&g_es31_mu);
    if (used) useprog((unsigned)cur);
}

static void (*r_glGetIntegerv)(unsigned, int *);
static void w_glGetIntegerv(unsigned pname, int *data)
{
    r_glGetIntegerv(pname, data);
    if (g_es31_shim && pname == 0x821C /* GL_MINOR_VERSION */ && data && *data == 0) *data = 1;
}

/* The storage-buffer target (a block of the game's data) is a uniform buffer here; a buffer the game made for it is grown to what the
 * 1024-element block above needs, so the binding is never smaller than the block. */
static void es31_note_storage(const char *call, unsigned target)
{
    static atomic_int said;
    if (g_es31_shim && target == 0x90D2 && atomic_fetch_add(&said, 1) < 12) tl_log_line("es31: %s on the storage-buffer target", call);
}
#define ES31_UBO_MIN (1024 * 32)
static void (*r_glBindBufferBase)(unsigned, unsigned, unsigned);
static void w_glBindBufferBase(unsigned target, unsigned index, unsigned buffer)
{
    es31_note_storage("glBindBufferBase", target);
    if (g_es31_shim && target == 0x90D2 /* GL_SHADER_STORAGE_BUFFER */) {
        static void (*getparam)(unsigned, unsigned, int *), (*bindbuf)(unsigned, unsigned), (*bufdata)(unsigned, long, const void *, unsigned),
                    (*copysub)(unsigned, unsigned, long, long, long), (*getsub)(unsigned, long, long, void *), (*subdata)(unsigned, long, long, const void *);
        if (!getparam && a_eglGetProcAddress) {
            getparam = a_eglGetProcAddress("glGetBufferParameteriv"); bindbuf = a_eglGetProcAddress("glBindBuffer");
            bufdata = a_eglGetProcAddress("glBufferData"); getsub = a_eglGetProcAddress("glGetBufferSubData"); subdata = a_eglGetProcAddress("glBufferSubData");
            copysub = a_eglGetProcAddress("glCopyBufferSubData");
        }
        if (getparam && bindbuf && bufdata && subdata && copysub && buffer) {
            int size = 0, usage = 0x88E4; /* GL_STATIC_DRAW */
            bindbuf(0x8F36 /* GL_COPY_WRITE_BUFFER */, buffer);
            getparam(0x8F36, 0x8764 /* GL_BUFFER_SIZE */, &size);
            getparam(0x8F36, 0x8765 /* GL_BUFFER_USAGE */, &usage);
            if (size > 0 && size < ES31_UBO_MIN) {
                unsigned tmp = 0; void (*gen)(int, unsigned *) = a_eglGetProcAddress("glGenBuffers"); void (*del)(int, const unsigned *) = a_eglGetProcAddress("glDeleteBuffers");
                if (gen && del) {
                    gen(1, &tmp);
                    bindbuf(0x8F37 /* GL_COPY_READ_BUFFER */, tmp);
                    bufdata(0x8F37, size, NULL, 0x88E4);
                    copysub(0x8F36, 0x8F37, 0, 0, size);
                    bufdata(0x8F36, ES31_UBO_MIN, NULL, (unsigned)usage);
                    copysub(0x8F37, 0x8F36, 0, 0, size);
                    del(1, &tmp);
                }
            }
            bindbuf(0x8F36, 0);
        }
        target = 0x8A11; /* GL_UNIFORM_BUFFER */
    }
    r_glBindBufferBase(target, index, buffer);
}

/*
 * Buffer textures (EXT_texture_buffer: a shader reads a buffer through a samplerBuffer, by texel index), which 3.0 does not have and
 * Unreal's mobile renderer uses (bones, particles...). Each is a 2D array texture here, one layer of rows TB_W texels wide: the shader's
 * samplerBuffer becomes a sampler2DArray and texelFetch(s, i) reads (i % TB_W, i / TB_W) (es31_downgrade). glTexBuffer copies the buffer
 * into the texture on the GPU, through the pixel-unpack binding; the copy is made again, before the next draw, after the game writes
 * that buffer. An array texture rather than a 2D one so that it never takes the place of the game's own 2D texture on a unit.
 */
typedef struct { unsigned tex, buf, ifmt; long off, range; long uploaded; bool dirty; } es31_tb;
static es31_tb g_tb[TB_MAX];
static int g_ntb;
static atomic_bool g_tb_dirty;
static pthread_mutex_t g_tb_mu = PTHREAD_MUTEX_INITIALIZER;

/* A buffer texture's internal format: its pixel format and type for the upload, and its size per texel. */
static bool tb_format(unsigned ifmt, unsigned *fmt, unsigned *type, int *bpt)
{
    static const struct { unsigned ifmt, fmt, type; int bpt; } k[] = {
        { 0x8229, 0x1903, 0x1401, 1 },  { 0x822B, 0x8227, 0x1401, 2 },  { 0x8058, 0x1908, 0x1401, 4 },       /* R8 RG8 RGBA8 */
        { 0x822D, 0x1903, 0x140B, 2 },  { 0x822F, 0x8227, 0x140B, 4 },  { 0x881A, 0x1908, 0x140B, 8 },       /* R16F RG16F RGBA16F */
        { 0x822E, 0x1903, 0x1406, 4 },  { 0x8230, 0x8227, 0x1406, 8 },  { 0x8815, 0x1907, 0x1406, 12 },      /* R32F RG32F RGB32F */
        { 0x8814, 0x1908, 0x1406, 16 },                                                                      /* RGBA32F */
        { 0x8232, 0x8D94, 0x1401, 1 },  { 0x8238, 0x8228, 0x1401, 2 },  { 0x8D7C, 0x8D99, 0x1401, 4 },       /* R8UI RG8UI RGBA8UI */
        { 0x8234, 0x8D94, 0x1403, 2 },  { 0x823A, 0x8228, 0x1403, 4 },  { 0x8D76, 0x8D99, 0x1403, 8 },       /* R16UI RG16UI RGBA16UI */
        { 0x8236, 0x8D94, 0x1405, 4 },  { 0x823C, 0x8228, 0x1405, 8 },  { 0x8D71, 0x8D98, 0x1405, 12 },      /* R32UI RG32UI RGB32UI */
        { 0x8D70, 0x8D99, 0x1405, 16 },                                                                      /* RGBA32UI */
        { 0x8235, 0x8D94, 0x1404, 4 },  { 0x823B, 0x8228, 0x1404, 8 },  { 0x8D83, 0x8D98, 0x1404, 12 },      /* R32I RG32I RGB32I */
        { 0x8D82, 0x8D99, 0x1404, 16 },                                                                      /* RGBA32I */
        { 0x8233, 0x8D94, 0x1402, 2 },  { 0x8239, 0x8228, 0x1402, 4 },  { 0x8D88, 0x8D99, 0x1402, 8 },       /* R16I RG16I RGBA16I */
        { 0x8231, 0x8D94, 0x1400, 1 },  { 0x8237, 0x8228, 0x1400, 2 },  { 0x8D8E, 0x8D99, 0x1400, 4 },       /* R8I RG8I RGBA8I */
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (k[i].ifmt == ifmt) { *fmt = k[i].fmt; *type = k[i].type; *bpt = k[i].bpt; return true; }
    return false;
}

static struct {
    void (*bind_tex)(unsigned, unsigned), (*bind_buf)(unsigned, unsigned), (*geti)(unsigned, int *), (*pixel)(unsigned, int);
    void (*param)(unsigned, unsigned, int), (*bufparam)(unsigned, unsigned, int *);
    void (*image3d)(unsigned, int, int, int, int, int, int, unsigned, unsigned, const void *);
    void (*sub3d)(unsigned, int, int, int, int, int, int, int, unsigned, unsigned, const void *);
} TB;
static bool tb_ready(void)
{
    if (TB.sub3d) return true;
    if (!a_eglGetProcAddress) return false;
    TB.bind_tex = a_eglGetProcAddress("glBindTexture"); TB.bind_buf = a_eglGetProcAddress("glBindBuffer");
    TB.geti = a_eglGetProcAddress("glGetIntegerv"); TB.pixel = a_eglGetProcAddress("glPixelStorei");
    TB.param = a_eglGetProcAddress("glTexParameteri"); TB.bufparam = a_eglGetProcAddress("glGetBufferParameteriv");
    TB.image3d = a_eglGetProcAddress("glTexImage3D");
    TB.sub3d = a_eglGetProcAddress("glTexSubImage3D");
    return TB.bind_tex && TB.bind_buf && TB.geti && TB.pixel && TB.param && TB.bufparam && TB.image3d && TB.sub3d;
}

/* Copy entry e's buffer into its texture, on the GPU (g_tb_mu held). The bindings it uses are put back as they were. */
static void tb_upload(es31_tb *e)
{
    unsigned fmt, type; int bpt;
    e->dirty = false;
    if (!tb_ready() || !tb_format(e->ifmt, &fmt, &type, &bpt)) return;
    int old_tex = 0, old_unpack = 0, old_row = 0, old_align = 4, old_skip_rows = 0, old_skip_px = 0, old_height = 0, old_images = 0, old_copy = 0;
    TB.geti(0x8C1D /* GL_TEXTURE_BINDING_2D_ARRAY */, &old_tex); TB.geti(0x88EF /* GL_PIXEL_UNPACK_BUFFER_BINDING */, &old_unpack);
    TB.geti(0x0CF2 /* GL_UNPACK_ROW_LENGTH */, &old_row); TB.geti(0x0CF5 /* GL_UNPACK_ALIGNMENT */, &old_align);
    TB.geti(0x0CF3 /* GL_UNPACK_SKIP_ROWS */, &old_skip_rows); TB.geti(0x0CF4 /* GL_UNPACK_SKIP_PIXELS */, &old_skip_px);
    TB.geti(0x806E /* GL_UNPACK_IMAGE_HEIGHT */, &old_height); TB.geti(0x806D /* GL_UNPACK_SKIP_IMAGES */, &old_images);
    TB.geti(0x8F37 /* GL_COPY_WRITE_BUFFER_BINDING */, &old_copy);
    long size = e->range;
    if (size <= 0) { int s = 0; TB.bind_buf(0x8F37, e->buf); TB.bufparam(0x8F37, 0x8764 /* GL_BUFFER_SIZE */, &s); size = s - e->off; }
    long texels = size > 0 ? size / bpt : 0;
    int rows = (int)(texels / TB_W), rem = (int)(texels % TB_W), h = rows + (rem ? 1 : 0);
    TB.bind_tex(TB_ARRAY, e->tex);
    if (e->uploaded != (long)h * TB_W || e->uploaded == 0) {
        TB.bind_buf(0x88EC /* GL_PIXEL_UNPACK_BUFFER */, 0);
        TB.image3d(TB_ARRAY, 0, (int)e->ifmt, TB_W, h > 0 ? h : 1, 1, 0, fmt, type, NULL);
        TB.param(TB_ARRAY, 0x2801 /* GL_TEXTURE_MIN_FILTER */, 0x2600 /* GL_NEAREST */);
        TB.param(TB_ARRAY, 0x2800 /* GL_TEXTURE_MAG_FILTER */, 0x2600);
        TB.param(TB_ARRAY, 0x813D /* GL_TEXTURE_MAX_LEVEL */, 0);
        e->uploaded = (long)(h > 0 ? h : 1) * TB_W;
    }
    if (texels > 0) {
        TB.bind_buf(0x88EC, e->buf);
        TB.pixel(0x0CF2, 0); TB.pixel(0x0CF5, 1); TB.pixel(0x0CF3, 0); TB.pixel(0x0CF4, 0); TB.pixel(0x806E, 0); TB.pixel(0x806D, 0);
        if (rows) TB.sub3d(TB_ARRAY, 0, 0, 0, 0, TB_W, rows, 1, fmt, type, (const void *)(uintptr_t)e->off);
        if (rem) TB.sub3d(TB_ARRAY, 0, 0, rows, 0, rem, 1, 1, fmt, type, (const void *)(uintptr_t)(e->off + (long)rows * TB_W * bpt));
    }
    TB.bind_buf(0x88EC, (unsigned)old_unpack); TB.bind_buf(0x8F37, (unsigned)old_copy);
    TB.pixel(0x0CF2, old_row); TB.pixel(0x0CF5, old_align); TB.pixel(0x0CF3, old_skip_rows); TB.pixel(0x0CF4, old_skip_px);
    TB.pixel(0x806E, old_height); TB.pixel(0x806D, old_images);
    TB.bind_tex(TB_ARRAY, (unsigned)old_tex);
}

/* glTexBuffer(Range): the texture bound to GL_TEXTURE_BUFFER on the active unit -- bound here as an array texture -- is made the window
 * onto the buffer, and filled. */
static void tb_attach(unsigned target, unsigned ifmt, unsigned buf, long off, long range)
{
    if (target != TB_TARGET || !tb_ready()) return;
    int tex = 0; TB.geti(0x8C1D /* GL_TEXTURE_BINDING_2D_ARRAY */, &tex);
    if (!tex) return;
    static atomic_int said;
    if (atomic_fetch_add(&said, 1) < 3) tl_log_line("es31: texture %d reads buffer %u as a buffer texture (format %#x), as an array texture", tex, buf, ifmt);
    pthread_mutex_lock(&g_tb_mu);
    es31_tb *e = NULL;
    for (int i = 0; i < g_ntb && !e; i++) if (g_tb[i].tex == (unsigned)tex) e = &g_tb[i];
    if (!e && g_ntb < TB_MAX) { e = &g_tb[g_ntb++]; memset(e, 0, sizeof(*e)); e->tex = (unsigned)tex; }
    if (e) {
        if (e->ifmt != ifmt) e->uploaded = 0;
        e->buf = buf; e->ifmt = ifmt; e->off = off; e->range = range;
        if (buf) tb_upload(e);
    }
    pthread_mutex_unlock(&g_tb_mu);
}
static void w_glTexBuffer(unsigned target, unsigned ifmt, unsigned buf) { tb_attach(target, ifmt, buf, 0, 0); }
static void w_glTexBufferRange(unsigned target, unsigned ifmt, unsigned buf, long off, long size) { tb_attach(target, ifmt, buf, off, size); }

/* The game wrote the buffer bound to `target`: a buffer texture over it is filled again before the next draw. */
static void tb_touch(unsigned target)
{
    if (!g_ntb || !tb_ready()) return;
    unsigned pname;
    switch (target) {
    case 0x8892: pname = 0x8894; break;     /* GL_ARRAY_BUFFER */
    case 0x8893: pname = 0x8895; break;     /* GL_ELEMENT_ARRAY_BUFFER */
    case 0x88EB: pname = 0x88ED; break;     /* GL_PIXEL_PACK_BUFFER */
    case 0x88EC: pname = 0x88EF; break;     /* GL_PIXEL_UNPACK_BUFFER */
    case 0x8A11: pname = 0x8A28; break;     /* GL_UNIFORM_BUFFER */
    case 0x8C8E: pname = 0x8C8F; break;     /* GL_TRANSFORM_FEEDBACK_BUFFER */
    case 0x8F36: case 0x8F37: pname = target; break;
    default: return;
    }
    int buf = 0; TB.geti(pname, &buf);
    if (!buf) return;
    pthread_mutex_lock(&g_tb_mu);
    for (int i = 0; i < g_ntb; i++) if (g_tb[i].buf == (unsigned)buf) { g_tb[i].dirty = true; atomic_store(&g_tb_dirty, true); }
    pthread_mutex_unlock(&g_tb_mu);
}
static void tb_flush(void)
{
    if (!atomic_exchange(&g_tb_dirty, false)) return;
    pthread_mutex_lock(&g_tb_mu);
    for (int i = 0; i < g_ntb; i++) if (g_tb[i].dirty) tb_upload(&g_tb[i]);
    pthread_mutex_unlock(&g_tb_mu);
}

/* The other calls that name the storage target: it is the uniform one here. The buffer-texture target is the copy-write one, a
 * binding of its own that the game's copies set before they use it. */
#define ES31_STORAGE(t) ((t) == 0x90D2 ? 0x8A11u : (t) == TB_TARGET ? 0x8F37u : (t))
static void (*r_glBindBuffer)(unsigned, unsigned);
static void w_glBindBuffer(unsigned t, unsigned b) { es31_note_storage("glBindBuffer", t); r_glBindBuffer(g_es31_shim ? ES31_STORAGE(t) : t, b); }
static void (*r_glBufferData)(unsigned, long, const void *, unsigned);
static void w_glBufferData(unsigned t, long n, const void *d, unsigned u)
{
    es31_note_storage("glBufferData", t);
    if (g_es31_shim) t = ES31_STORAGE(t);
    r_glBufferData(t, n, d, u);
    if (g_es31_shim) tb_touch(t);
}
static void (*r_glBufferSubData)(unsigned, long, long, const void *);
static void w_glBufferSubData(unsigned t, long o, long n, const void *d)
{
    es31_note_storage("glBufferSubData", t);
    if (g_es31_shim) t = ES31_STORAGE(t);
    r_glBufferSubData(t, o, n, d);
    if (g_es31_shim) tb_touch(t);
}
static unsigned char (*r_glUnmapBuffer)(unsigned);
static unsigned char w_glUnmapBuffer(unsigned t)
{
    if (g_es31_shim) t = ES31_STORAGE(t);
    unsigned char r = r_glUnmapBuffer(t);
    if (g_es31_shim) tb_touch(t);
    return r;
}
static void *(*r_glMapBufferRange)(unsigned, long, long, unsigned);
static void *w_glMapBufferRange(unsigned t, long o, long n, unsigned access) { return r_glMapBufferRange(g_es31_shim ? ES31_STORAGE(t) : t, o, n, access); }
static void (*r_glCopyBufferSubData)(unsigned, unsigned, long, long, long);
static void w_glCopyBufferSubData(unsigned rt, unsigned wt, long ro, long wo, long n)
{
    if (g_es31_shim) { rt = ES31_STORAGE(rt); wt = ES31_STORAGE(wt); }
    r_glCopyBufferSubData(rt, wt, ro, wo, n);
    if (g_es31_shim) tb_touch(wt);
}
/* A buffer texture is bound as the array texture it is here. */
static void (*r_glBindTexture)(unsigned, unsigned);
static void w_glBindTexture(unsigned t, unsigned tex) { r_glBindTexture(g_es31_shim && t == TB_TARGET ? TB_ARRAY : t, tex); }
static void (*r_glDeleteTextures)(int, const unsigned *);
static void w_glDeleteTextures(int n, const unsigned *t)
{
    if (g_es31_shim && g_ntb && t) {
        pthread_mutex_lock(&g_tb_mu);
        for (int k = 0; k < n; k++)
            for (int i = 0; i < g_ntb; i++) if (g_tb[i].tex == t[k]) { g_tb[i] = g_tb[--g_ntb]; break; }
        pthread_mutex_unlock(&g_tb_mu);
    }
    r_glDeleteTextures(n, t);
}
/* Draws: what the buffer textures show is brought up to date first. */
static void (*r_glDrawArrays)(unsigned, int, int);
static void w_glDrawArrays(unsigned m, int f, int c) { if (g_es31_shim) tb_flush(); r_glDrawArrays(m, f, c); }
static void (*r_glDrawElements)(unsigned, int, unsigned, const void *);
static void w_glDrawElements(unsigned m, int c, unsigned t, const void *i) { if (g_es31_shim) tb_flush(); r_glDrawElements(m, c, t, i); }
static void (*r_glDrawArraysInstanced)(unsigned, int, int, int);
static void w_glDrawArraysInstanced(unsigned m, int f, int c, int n) { if (g_es31_shim) tb_flush(); r_glDrawArraysInstanced(m, f, c, n); }
static void (*r_glDrawElementsInstanced)(unsigned, int, unsigned, const void *, int);
static void w_glDrawElementsInstanced(unsigned m, int c, unsigned t, const void *i, int n) { if (g_es31_shim) tb_flush(); r_glDrawElementsInstanced(m, c, t, i, n); }
static void (*r_glDrawRangeElements)(unsigned, unsigned, unsigned, int, unsigned, const void *);
static void w_glDrawRangeElements(unsigned m, unsigned s, unsigned e, int c, unsigned t, const void *i) { if (g_es31_shim) tb_flush(); r_glDrawRangeElements(m, s, e, c, t, i); }
static void (*r_glBindBufferRange)(unsigned, unsigned, unsigned, long, long);
static void w_glBindBufferRange(unsigned t, unsigned i, unsigned b, long o, long n)
{
    es31_note_storage("glBindBufferRange", t);
    if (g_es31_shim && t == 0x90D2) { w_glBindBufferBase(t, i, b); return; }   /* grows the buffer if need be; the whole buffer is bound */
    r_glBindBufferRange(t, i, b, o, n);
}
