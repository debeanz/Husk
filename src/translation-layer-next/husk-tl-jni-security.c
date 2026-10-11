/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The parts of java.security and android.util.Base64 that native code reaches through JNI to check a signed licence:
 * decode a Base64 key, wrap it as an X.509 public key, and verify an RSA signature. The verification is Apple's
 * (Security.framework); nothing here signs.
 */
#define _DARWIN_C_SOURCE
#include "husk-tl-jni.h"

#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <Security/Security.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-bionic.h"

static jvalue vl(void *p) { jvalue v; v.j = 0; v.l = p; return v; }
static jvalue vz(int z) { jvalue v; v.j = 0; v.z = z != 0; return v; }
#define STR(s) tl_jni_new_string(s)
static const char *S(const jobj *o) { const char *s = tl_jni_string(o); return s ? s : ""; }
static void Noop(tl_jcall *c) { (void)c; }

/* ------------------------------------------------------------------ Base64 */

enum { B64_NO_PADDING = 1, B64_NO_WRAP = 2, B64_CRLF = 4, B64_URL_SAFE = 8 };

static int b64_value(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Decodes leniently, as Android does: whitespace is skipped, padding is optional. NULL on a bad character. */
static uint8_t *b64_decode(const uint8_t *in, size_t n, size_t *out_n)
{
    uint8_t *out = malloc(n / 4 * 3 + 3);
    size_t k = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int c = in[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        int v = b64_value(c);
        if (v < 0) { free(out); return NULL; }
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out[k++] = (uint8_t)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    *out_n = k;
    return out;
}

static jobj *bytes_array(const uint8_t *p, size_t n)
{
    jobj *a = tl_jni_new_prim_array('B', (uint32_t)n);
    if (n) memcpy(a->arr.data, p, n);
    return a;
}

static void Base64_decodeString(tl_jcall *c)
{
    const char *s = S(c->args[0].l);
    size_t n; uint8_t *d = b64_decode((const uint8_t *)s, strlen(s), &n);
    if (!d) { tl_jni_throw("java/lang/IllegalArgumentException", "bad base-64"); c->ret = vl(NULL); return; }
    c->ret = vl(bytes_array(d, n)); free(d);
}
static void Base64_decodeBytes(tl_jcall *c)
{
    jobj *a = c->args[0].l;
    size_t n; uint8_t *d = a ? b64_decode(a->arr.data, a->arr.len, &n) : NULL;
    if (!d) { tl_jni_throw("java/lang/IllegalArgumentException", "bad base-64"); c->ret = vl(NULL); return; }
    c->ret = vl(bytes_array(d, n)); free(d);
}
static char *b64_encode(const uint8_t *in, size_t n, int flags)
{
    const char *tab = (flags & B64_URL_SAFE) ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_" : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc(n / 3 * 4 + 8 + n / 57 * 2);
    size_t k = 0; int col = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[k++] = tab[v >> 18]; out[k++] = tab[(v >> 12) & 63];
        if (i + 1 < n) out[k++] = tab[(v >> 6) & 63]; else if (!(flags & B64_NO_PADDING)) out[k++] = '=';
        if (i + 2 < n) out[k++] = tab[v & 63]; else if (!(flags & B64_NO_PADDING)) out[k++] = '=';
        col += 4;
        if (!(flags & B64_NO_WRAP) && col >= 76 && i + 3 < n) { if (flags & B64_CRLF) out[k++] = '\r'; out[k++] = '\n'; col = 0; }
    }
    if (!(flags & B64_NO_WRAP)) { if (flags & B64_CRLF) out[k++] = '\r'; out[k++] = '\n'; }
    out[k] = 0;
    return out;
}
static void Base64_encodeToString(tl_jcall *c)
{
    jobj *a = c->args[0].l;
    char *e = b64_encode(a ? a->arr.data : (uint8_t *)"", a ? a->arr.len : 0, c->args[1].i);
    c->ret = vl(STR(e)); free(e);
}
static void Base64_encode(tl_jcall *c)
{
    jobj *a = c->args[0].l;
    char *e = b64_encode(a ? a->arr.data : (uint8_t *)"", a ? a->arr.len : 0, c->args[1].i);
    c->ret = vl(bytes_array((const uint8_t *)e, strlen(e))); free(e);
}

/* ----------------------------------------------------- keys and signatures */

static void KeySpec_init(tl_jcall *c)
{
    jobj *a = c->args[0].l;
    tl_jni_set_field(c->self, "encoded", "[B", vl(a ? tl_jni_ref(a) : NULL));
}
static void KeyFactory_getInstance(tl_jcall *c)
{
    jobj *f = tl_jni_new_object(tl_jni_class("java/security/KeyFactory"));
    tl_jni_set_field(f, "algorithm", "Ljava/lang/String;", vl(STR(S(c->args[0].l))));
    c->ret = vl(f);
}
static void KeyFactory_generatePublic(tl_jcall *c)
{
    jobj *spec = c->args[0].l;
    jobj *k = tl_jni_new_object(tl_jni_class("java/security/PublicKey"));
    tl_jni_set_field(k, "encoded", "[B", spec ? tl_jni_get_field(spec, "encoded", "[B") : vl(NULL));
    tl_jni_set_field(k, "algorithm", "Ljava/lang/String;", vl(STR("RSA")));
    c->ret = vl(k);
}
static void Key_getEncoded(tl_jcall *c) { jvalue v = tl_jni_get_field(c->self, "encoded", "[B"); c->ret = vl(v.l ? tl_jni_ref(v.l) : NULL); }
static void Key_getAlgorithm(tl_jcall *c) { c->ret = vl(STR("RSA")); }

/* Signature: the message is gathered by update(), verified by verify() */
typedef struct { uint8_t *msg; size_t n, cap; jobj *key; char algo[40]; } sigstate;
static sigstate *sig_of(jobj *o) { if (!o->native) o->native = calloc(1, sizeof(sigstate)); return o->native; }
static void Signature_getInstance(tl_jcall *c)
{
    jobj *s = tl_jni_new_object(tl_jni_class("java/security/Signature"));
    snprintf(sig_of(s)->algo, sizeof(sig_of(s)->algo), "%s", S(c->args[0].l));
    c->ret = vl(s);
}
static void Signature_initVerify(tl_jcall *c) { sigstate *st = sig_of(c->self); st->key = c->args[0].l; st->n = 0; }
static void sig_add(sigstate *st, const uint8_t *p, size_t n)
{
    if (st->n + n > st->cap) { st->cap = (st->n + n) * 2; st->msg = realloc(st->msg, st->cap); }
    memcpy(st->msg + st->n, p, n); st->n += n;
}
static void Signature_update(tl_jcall *c) { jobj *a = c->args[0].l; if (a) sig_add(sig_of(c->self), a->arr.data, a->arr.len); }
static void Signature_updateRange(tl_jcall *c)
{
    jobj *a = c->args[0].l; int off = c->args[1].i, len = c->args[2].i;
    if (a && off >= 0 && len >= 0 && (uint32_t)(off + len) <= a->arr.len) sig_add(sig_of(c->self), (const uint8_t *)a->arr.data + off, (size_t)len);
}

/* The DER of a SubjectPublicKeyInfo holds a BIT STRING whose bytes are the PKCS#1 RSAPublicKey that Security wants. */
static bool der_next(const uint8_t *p, size_t n, size_t *hdr, size_t *len)
{
    if (n < 2) return false;
    size_t l = p[1], h = 2;
    if (l & 0x80) { size_t k = l & 0x7F; if (k == 0 || k > 4 || n < 2 + k) return false; l = 0; for (size_t i = 0; i < k; i++) l = (l << 8) | p[2 + i]; h = 2 + k; }
    if (h + l > n) return false;
    *hdr = h; *len = l;
    return true;
}
static bool spki_to_pkcs1(const uint8_t *der, size_t n, const uint8_t **out, size_t *out_n)
{
    size_t h, l;
    if (!der_next(der, n, &h, &l) || der[0] != 0x30) return false;
    const uint8_t *p = der + h; size_t rem = l;
    size_t h2, l2;
    if (!der_next(p, rem, &h2, &l2) || p[0] != 0x30) return false;          /* the algorithm identifier */
    p += h2 + l2; rem -= h2 + l2;
    if (!der_next(p, rem, &h2, &l2) || p[0] != 0x03 || l2 < 1) return false;   /* the key's bit string */
    *out = p + h2 + 1; *out_n = l2 - 1;
    return true;
}

static void Signature_verify(tl_jcall *c)
{
    sigstate *st = sig_of(c->self);
    jobj *sigb = c->args[0].l;
    bool ok = false;
    jvalue enc = st->key ? tl_jni_get_field(st->key, "encoded", "[B") : (jvalue){ .j = 0 };
    jobj *e = enc.l;
    const uint8_t *rsa; size_t rsa_n;
    if (e && sigb && spki_to_pkcs1(e->arr.data, e->arr.len, &rsa, &rsa_n)) {
        CFDataRef kd = CFDataCreate(NULL, rsa, (CFIndex)rsa_n);
        const void *ks[] = { kSecAttrKeyType, kSecAttrKeyClass }, *vs[] = { kSecAttrKeyTypeRSA, kSecAttrKeyClassPublic };
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, ks, vs, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        SecKeyRef key = SecKeyCreateWithData(kd, attrs, NULL);
        SecKeyAlgorithm alg = !strcasecmp(st->algo, "SHA256withRSA") ? kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256
                            : !strcasecmp(st->algo, "SHA512withRSA") ? kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA512
                            : !strcasecmp(st->algo, "SHA384withRSA") ? kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA384
                            : kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA1;
        if (key) {
            CFDataRef msg = CFDataCreate(NULL, st->msg ? st->msg : (const uint8_t *)"", (CFIndex)st->n);
            CFDataRef sg = CFDataCreate(NULL, sigb->arr.data, (CFIndex)sigb->arr.len);
            ok = SecKeyVerifySignature(key, alg, msg, sg, NULL);
            CFRelease(msg); CFRelease(sg); CFRelease(key);
        }
        CFRelease(attrs); CFRelease(kd);
    }
    st->n = 0;
    tl_log_line("security: %s verify -> %s", st->algo, ok ? "valid" : "INVALID");
    c->ret = vz(ok);
}

/* java.util.Arrays.copyOfRange / copyOf, for primitive arrays */
static void Arrays_copyOfRange(tl_jcall *c)
{
    jobj *a = c->args[0].l; int from = c->args[1].i, to = c->args[2].i;
    if (!a || a->kind != TL_K_PRIM_ARRAY) { tl_jni_throw("java/lang/NullPointerException", "array"); c->ret = vl(NULL); return; }
    if (from < 0 || from > (int)a->arr.len || from > to) { tl_jni_throw("java/lang/ArrayIndexOutOfBoundsException", "copyOfRange"); c->ret = vl(NULL); return; }
    jobj *r = tl_jni_new_prim_array(a->arr.etype, (uint32_t)(to - from));
    size_t avail = (size_t)((int)a->arr.len - from), want = (size_t)(to - from), n = avail < want ? avail : want;
    memcpy(r->arr.data, (const uint8_t *)a->arr.data + (size_t)from * a->arr.esz, n * a->arr.esz);
    c->ret = vl(r);
}
static void Arrays_copyOf(tl_jcall *c)
{
    jobj *a = c->args[0].l; int len = c->args[1].i;
    if (!a || a->kind != TL_K_PRIM_ARRAY || len < 0) { tl_jni_throw("java/lang/NullPointerException", "array"); c->ret = vl(NULL); return; }
    jobj *r = tl_jni_new_prim_array(a->arr.etype, (uint32_t)len);
    size_t n = (uint32_t)len < a->arr.len ? (size_t)len : a->arr.len;
    memcpy(r->arr.data, a->arr.data, n * a->arr.esz);
    c->ret = vl(r);
}


/* ------------------------------------------------------------------ java.util.UUID */

typedef struct { uint64_t msb, lsb; } uuid_state;
static jobj *uuid_new(uint64_t msb, uint64_t lsb)
{
    jobj *o = tl_jni_new_object(tl_jni_class("java/util/UUID"));
    uuid_state *u = calloc(1, sizeof(*u)); u->msb = msb; u->lsb = lsb; o->native = u;
    return o;
}
static uuid_state *uuid_of(jobj *o) { if (!o->native) o->native = calloc(1, sizeof(uuid_state)); return o->native; }
static void UUID_randomUUID(tl_jcall *c)
{
    uint64_t r[2]; arc4random_buf(r, sizeof(r));
    r[0] = (r[0] & ~0xF000ull) | 0x4000ull;                      /* version 4 */
    r[1] = (r[1] & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull; /* IETF variant */
    c->ret = vl(uuid_new(r[0], r[1]));
}
static void UUID_init(tl_jcall *c) { uuid_state *u = uuid_of(c->self); u->msb = (uint64_t)c->args[0].j; u->lsb = (uint64_t)c->args[1].j; }
static void UUID_toString(tl_jcall *c)
{
    uuid_state *u = uuid_of(c->self); char b[40];
    snprintf(b, sizeof(b), "%08x-%04x-%04x-%04x-%012llx", (unsigned)(u->msb >> 32), (unsigned)((u->msb >> 16) & 0xFFFF), (unsigned)(u->msb & 0xFFFF),
             (unsigned)(u->lsb >> 48), (unsigned long long)(u->lsb & 0xFFFFFFFFFFFFull));
    c->ret = vl(STR(b));
}
static void UUID_fromString(tl_jcall *c)
{
    const char *s = S(c->args[0].l); uint64_t v[2] = { 0, 0 }; int digits = 0;
    for (; *s; s++) {
        if (*s == '-') continue;
        int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (d < 0 || digits >= 32) { tl_jni_throw("java/lang/IllegalArgumentException", "Invalid UUID"); c->ret = vl(NULL); return; }
        v[digits / 16] = (v[digits / 16] << 4) | (uint64_t)d; digits++;
    }
    if (digits != 32) { tl_jni_throw("java/lang/IllegalArgumentException", "Invalid UUID"); c->ret = vl(NULL); return; }
    c->ret = vl(uuid_new(v[0], v[1]));
}
static void UUID_msb(tl_jcall *c) { c->ret.j = (int64_t)uuid_of(c->self)->msb; }
static void UUID_lsb(tl_jcall *c) { c->ret.j = (int64_t)uuid_of(c->self)->lsb; }

/* ---------------------------------------------- MessageDigest, Mac, SecureRandom */

/*
 * What .NET's crypto library on Android (System.Security.Cryptography.Native.Android) hashes with: it has no digests of its
 * own and asks Java's MessageDigest and Mac, and aborts the process if digest() answers null. Apple's CommonCrypto does the
 * work here.
 */
enum { MD_MD5 = 1, MD_SHA1, MD_SHA256, MD_SHA384, MD_SHA512 };
static int md_kind(const char *n)
{
    char a[32]; size_t k = 0;
    for (; *n && k + 1 < sizeof(a); n++) if (*n != '-' && *n != '_') a[k++] = (char)((*n >= 'a' && *n <= 'z') ? *n - 32 : *n);
    a[k] = 0;
    if (!strncmp(a, "HMAC", 4)) memmove(a, a + 4, strlen(a + 4) + 1);
    if (!strcmp(a, "MD5")) return MD_MD5;
    if (!strcmp(a, "SHA1") || !strcmp(a, "SHA")) return MD_SHA1;
    if (!strcmp(a, "SHA256")) return MD_SHA256;
    if (!strcmp(a, "SHA384")) return MD_SHA384;
    if (!strcmp(a, "SHA512")) return MD_SHA512;
    return 0;
}
static size_t md_len(int k) { return k == MD_MD5 ? 16 : k == MD_SHA1 ? 20 : k == MD_SHA256 ? 32 : k == MD_SHA384 ? 48 : 64; }

typedef struct {
    int kind;
    union { CC_MD5_CTX md5; CC_SHA1_CTX sha1; CC_SHA256_CTX sha256; CC_SHA512_CTX sha512; } u;
} mdstate;
static void md_init(mdstate *m)
{
    switch (m->kind) {
    case MD_MD5: CC_MD5_Init(&m->u.md5); break;
    case MD_SHA1: CC_SHA1_Init(&m->u.sha1); break;
    case MD_SHA256: CC_SHA256_Init(&m->u.sha256); break;
    case MD_SHA384: CC_SHA384_Init(&m->u.sha512); break;
    default: CC_SHA512_Init(&m->u.sha512); break;
    }
}
static void md_update(mdstate *m, const void *p, size_t n)
{
    switch (m->kind) {
    case MD_MD5: CC_MD5_Update(&m->u.md5, p, (CC_LONG)n); break;
    case MD_SHA1: CC_SHA1_Update(&m->u.sha1, p, (CC_LONG)n); break;
    case MD_SHA256: CC_SHA256_Update(&m->u.sha256, p, (CC_LONG)n); break;
    case MD_SHA384: CC_SHA384_Update(&m->u.sha512, p, (CC_LONG)n); break;
    default: CC_SHA512_Update(&m->u.sha512, p, (CC_LONG)n); break;
    }
}
static void md_final(mdstate *m, uint8_t *out)
{
    switch (m->kind) {
    case MD_MD5: CC_MD5_Final(out, &m->u.md5); break;
    case MD_SHA1: CC_SHA1_Final(out, &m->u.sha1); break;
    case MD_SHA256: CC_SHA256_Final(out, &m->u.sha256); break;
    case MD_SHA384: CC_SHA384_Final(out, &m->u.sha512); break;
    default: CC_SHA512_Final(out, &m->u.sha512); break;
    }
    md_init(m);                                                     /* digest() leaves the object reset, as Java's does */
}
static mdstate *md_of(jobj *o) { if (!o->native) o->native = calloc(1, sizeof(mdstate)); return o->native; }
static void MD_getInstance(tl_jcall *c)
{
    int k = md_kind(S(c->args[0].l));
    if (!k) { tl_log_line("security: MessageDigest.getInstance(%s): not an algorithm here", S(c->args[0].l)); tl_jni_throw("java/security/NoSuchAlgorithmException", S(c->args[0].l)); return; }
    jobj *o = tl_jni_new_object(tl_jni_class("java/security/MessageDigest"));
    mdstate *m = md_of(o); m->kind = k; md_init(m);
    c->ret = vl(o);
}
static void MD_update(tl_jcall *c) { jobj *a = c->args[0].l; if (a) md_update(md_of(c->self), a->arr.data, a->arr.len); }
static void MD_updateRange(tl_jcall *c)
{
    jobj *a = c->args[0].l; int off = c->args[1].i, len = c->args[2].i;
    if (a && off >= 0 && len >= 0 && (uint32_t)off + (uint32_t)len <= a->arr.len) md_update(md_of(c->self), (uint8_t *)a->arr.data + off, (size_t)len);
}
static void MD_updateByte(tl_jcall *c) { uint8_t b = (uint8_t)c->args[0].b; md_update(md_of(c->self), &b, 1); }
static void MD_digest(tl_jcall *c) { mdstate *m = md_of(c->self); uint8_t out[64]; md_final(m, out); c->ret = vl(bytes_array(out, md_len(m->kind))); }
static void MD_digestWith(tl_jcall *c) { MD_update(c); MD_digest(c); }
static void MD_reset(tl_jcall *c) { md_init(md_of(c->self)); }
static void MD_length(tl_jcall *c) { c->ret.i = (int)md_len(md_of(c->self)->kind); }
static void MD_clone(tl_jcall *c)
{
    jobj *o = tl_jni_new_object(c->self->cls);
    *md_of(o) = *md_of(c->self);
    c->ret = vl(o);
}

/* SecretKeySpec(key, algorithm): a Key whose getEncoded() is the bytes. */
static void SKS_init(tl_jcall *c)
{
    jobj *a = c->args[0].l;
    jvalue v = vl(a ? bytes_array(a->arr.data, a->arr.len) : NULL);
    tl_jni_set_field(c->self, "encoded", "[B", v);
}

typedef struct { int kind; CCHmacContext ctx; uint8_t key[256]; size_t keylen; bool ready; } macstate;
static macstate *mac_of(jobj *o) { if (!o->native) o->native = calloc(1, sizeof(macstate)); return o->native; }
static CCHmacAlgorithm mac_alg(int k) { return k == MD_MD5 ? kCCHmacAlgMD5 : k == MD_SHA1 ? kCCHmacAlgSHA1 : k == MD_SHA256 ? kCCHmacAlgSHA256 : k == MD_SHA384 ? kCCHmacAlgSHA384 : kCCHmacAlgSHA512; }
static void mac_start(macstate *m) { CCHmacInit(&m->ctx, mac_alg(m->kind), m->key, m->keylen); m->ready = true; }
static void Mac_getInstance(tl_jcall *c)
{
    int k = md_kind(S(c->args[0].l));
    if (!k) { tl_log_line("security: Mac.getInstance(%s): not an algorithm here", S(c->args[0].l)); tl_jni_throw("java/security/NoSuchAlgorithmException", S(c->args[0].l)); return; }
    jobj *o = tl_jni_new_object(tl_jni_class("javax/crypto/Mac"));
    mac_of(o)->kind = k;
    c->ret = vl(o);
}
static void Mac_init(tl_jcall *c)
{
    macstate *m = mac_of(c->self);
    jobj *key = c->args[0].l;
    jvalue v = key ? tl_jni_get_field(key, "encoded", "[B") : vl(NULL);
    jobj *a = v.l;
    m->keylen = a ? (a->arr.len < sizeof(m->key) ? a->arr.len : sizeof(m->key)) : 0;
    if (m->keylen) memcpy(m->key, a->arr.data, m->keylen);
    mac_start(m);
}
static void Mac_update(tl_jcall *c) { macstate *m = mac_of(c->self); jobj *a = c->args[0].l; if (!m->ready) mac_start(m); if (a) CCHmacUpdate(&m->ctx, a->arr.data, a->arr.len); }
static void Mac_doFinal(tl_jcall *c)
{
    macstate *m = mac_of(c->self);
    if (!m->ready) mac_start(m);
    uint8_t out[64];
    CCHmacFinal(&m->ctx, out);
    mac_start(m);
    c->ret = vl(bytes_array(out, md_len(m->kind)));
}
static void Mac_reset(tl_jcall *c) { mac_start(mac_of(c->self)); }
static void Mac_clone(tl_jcall *c) { jobj *o = tl_jni_new_object(c->self->cls); *mac_of(o) = *mac_of(c->self); c->ret = vl(o); }

static void SecureRandom_nextBytes(tl_jcall *c) { jobj *a = c->args[0].l; if (a && a->arr.len) arc4random_buf(a->arr.data, a->arr.len); }

static const struct { const char *name, *super; } k_classes[] = {
    { "java/util/UUID", "java/lang/Object" },
    { "java/util/Arrays", "java/lang/Object" }, { "java/lang/ArrayIndexOutOfBoundsException", "java/lang/RuntimeException" },
    { "android/util/Base64", "java/lang/Object" }, { "java/security/KeyFactory", "java/lang/Object" },
    { "java/security/spec/KeySpec", "java/lang/Object" }, { "java/security/spec/X509EncodedKeySpec", "java/security/spec/KeySpec" },
    { "java/security/Key", "java/lang/Object" }, { "java/security/PublicKey", "java/security/Key" },
    { "java/security/Signature", "java/lang/Object" }, { "java/lang/IllegalArgumentException", "java/lang/RuntimeException" },
    { "java/security/MessageDigest", "java/lang/Object" }, { "javax/crypto/Mac", "java/lang/Object" },
    { "javax/crypto/SecretKey", "java/security/Key" }, { "javax/crypto/spec/SecretKeySpec", "javax/crypto/SecretKey" },
    { "java/security/SecureRandom", "java/lang/Object" },
    { "java/security/GeneralSecurityException", "java/lang/Exception" },
    { "java/security/NoSuchAlgorithmException", "java/security/GeneralSecurityException" },
};

#define M_(c, n, s, f) { c, n, s, f }
static const tl_jhle k_hle[] = {
    M_("java/util/Arrays", "copyOfRange", "([BII)[B", Arrays_copyOfRange), M_("java/util/Arrays", "copyOfRange", "([III)[I", Arrays_copyOfRange),
    M_("java/util/Arrays", "copyOf", "([BI)[B", Arrays_copyOf), M_("java/util/Arrays", "copyOf", "([II)[I", Arrays_copyOf),
    M_("java/util/UUID", "randomUUID", "()Ljava/util/UUID;", UUID_randomUUID), M_("java/util/UUID", "<init>", "(JJ)V", UUID_init),
    M_("java/util/UUID", "toString", "()Ljava/lang/String;", UUID_toString), M_("java/util/UUID", "fromString", "(Ljava/lang/String;)Ljava/util/UUID;", UUID_fromString),
    M_("java/util/UUID", "getMostSignificantBits", "()J", UUID_msb), M_("java/util/UUID", "getLeastSignificantBits", "()J", UUID_lsb),
    M_("android/util/Base64", "decode", "(Ljava/lang/String;I)[B", Base64_decodeString),
    M_("android/util/Base64", "decode", "([BI)[B", Base64_decodeBytes),
    M_("android/util/Base64", "encodeToString", "([BI)Ljava/lang/String;", Base64_encodeToString),
    M_("android/util/Base64", "encode", "([BI)[B", Base64_encode),
    M_("java/security/spec/X509EncodedKeySpec", "<init>", "([B)V", KeySpec_init),
    M_("java/security/KeyFactory", "getInstance", "(Ljava/lang/String;)Ljava/security/KeyFactory;", KeyFactory_getInstance),
    M_("java/security/KeyFactory", "generatePublic", "(Ljava/security/spec/KeySpec;)Ljava/security/PublicKey;", KeyFactory_generatePublic),
    M_("java/security/Key", "getEncoded", "()[B", Key_getEncoded), M_("java/security/Key", "getAlgorithm", "()Ljava/lang/String;", Key_getAlgorithm),
    M_("java/security/Signature", "getInstance", "(Ljava/lang/String;)Ljava/security/Signature;", Signature_getInstance),
    M_("java/security/Signature", "initVerify", "(Ljava/security/PublicKey;)V", Signature_initVerify),
    M_("java/security/Signature", "update", "([B)V", Signature_update), M_("java/security/Signature", "update", "([BII)V", Signature_updateRange),
    M_("java/security/Signature", "verify", "([B)Z", Signature_verify),
    M_("java/security/MessageDigest", "getInstance", "(Ljava/lang/String;)Ljava/security/MessageDigest;", MD_getInstance),
    M_("java/security/MessageDigest", "update", "([B)V", MD_update), M_("java/security/MessageDigest", "update", "([BII)V", MD_updateRange),
    M_("java/security/MessageDigest", "update", "(B)V", MD_updateByte), M_("java/security/MessageDigest", "digest", "()[B", MD_digest),
    M_("java/security/MessageDigest", "digest", "([B)[B", MD_digestWith), M_("java/security/MessageDigest", "reset", "()V", MD_reset),
    M_("java/security/MessageDigest", "getDigestLength", "()I", MD_length), M_("java/security/MessageDigest", "clone", "()Ljava/lang/Object;", MD_clone),
    M_("javax/crypto/spec/SecretKeySpec", "<init>", "([BLjava/lang/String;)V", SKS_init),
    M_("javax/crypto/Mac", "getInstance", "(Ljava/lang/String;)Ljavax/crypto/Mac;", Mac_getInstance),
    M_("javax/crypto/Mac", "init", "(Ljava/security/Key;)V", Mac_init), M_("javax/crypto/Mac", "update", "([B)V", Mac_update),
    M_("javax/crypto/Mac", "doFinal", "()[B", Mac_doFinal), M_("javax/crypto/Mac", "reset", "()V", Mac_reset),
    M_("javax/crypto/Mac", "clone", "()Ljava/lang/Object;", Mac_clone),
    M_("java/security/SecureRandom", "<init>", "()V", Noop), M_("java/security/SecureRandom", "nextBytes", "([B)V", SecureRandom_nextBytes),
    { NULL, NULL, NULL, NULL }
};

void tl_security_install(void)
{
    for (size_t i = 0; i < sizeof(k_classes) / sizeof(k_classes[0]); i++) tl_jni_declare(k_classes[i].name, k_classes[i].super);
    tl_jni_register_hle(k_hle);
    (void)Noop;
}
