/* Runtime: the UTF-8 code-point layer (stdlib/unicode.rald).
 *
 * Emerald strings remain bytes: `len` counts bytes, `s[i]` is one byte, and
 * `ord`/`chr` operate on byte values. This layer interprets a string as
 * UTF-8 and operates on code points, beside the byte semantics rather than
 * changing them — see stdlib/SPEC.md.
 *
 * Every function here is total: a byte that cannot begin a valid UTF-8
 * sequence is read as one "code point" (its own byte value), so any string
 * can be measured, indexed, sliced, and enumerated. `em_uc_valid` is the
 * strict check; `em_uc_chr` is the one place an invalid code point is an
 * error. `em_uc_ord` on an invalid first byte returns the byte's own value,
 * while `em_uc_chr` always produces well-formed UTF-8.
 */
#include "runtime_internal.h"

/* Length in bytes of the UTF-8 sequence starting at s[0], given that `n`
 * bytes remain in the string. Returns 1 for ASCII and for any byte that
 * cannot begin a valid sequence (a continuation byte, an overlong lead, a
 * surrogate encoding, a truncated sequence, or a value above U+10FFFF). */
static size_t uc_seq_len(const unsigned char *s, size_t n) {
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return n >= 2 && (s[1] & 0xC0) == 0x80 ? 2 : 1;
    if (c == 0xE0) return n >= 3 && (unsigned char)s[1] >= 0xA0 &&
                                  (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 ? 3 : 1;
    if (c >= 0xE1 && c <= 0xEC) return n >= 3 && (s[1] & 0xC0) == 0x80 &&
                                  (s[2] & 0xC0) == 0x80 ? 3 : 1;
    if (c == 0xED) return n >= 3 && (unsigned char)s[1] <= 0x9F &&
                                  (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 ? 3 : 1;
    if (c >= 0xEE && c <= 0xEF) return n >= 3 && (s[1] & 0xC0) == 0x80 &&
                                  (s[2] & 0xC0) == 0x80 ? 3 : 1;
    if (c == 0xF0) return n >= 4 && (unsigned char)s[1] >= 0x90 &&
                                  (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
                                  (s[3] & 0xC0) == 0x80 ? 4 : 1;
    if (c >= 0xF1 && c <= 0xF3) return n >= 4 && (s[1] & 0xC0) == 0x80 &&
                                  (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80 ? 4 : 1;
    if (c == 0xF4) return n >= 4 && (unsigned char)s[1] <= 0x8F &&
                                  (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
                                  (s[3] & 0xC0) == 0x80 ? 4 : 1;
    return 1; /* 0x80..0xC1 (continuation / overlong lead), 0xF5..0xFF */
}

/* The code point at s[0], using the same validity rules as uc_seq_len: a
 * valid multi-byte sequence is decoded, anything else is its raw byte. */
static int32_t uc_decode(const unsigned char *s, size_t n) {
    size_t sl = uc_seq_len(s, n);
    if (sl == 1) return (int32_t)s[0];
    unsigned char c = s[0];
    if (sl == 2) return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    if (sl == 3) return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) |
           (s[3] & 0x3F);
}

/* Number of code points in data[0..len). */
static size_t uc_count(const char *data, size_t len) {
    size_t count = 0, off = 0;
    while (off < len) {
        off += uc_seq_len((const unsigned char *)data + off, len - off);
        count++;
    }
    return count;
}

/* Byte offset of code point `i` (0-based); clamps to `len` past the end. */
static size_t uc_offset(const char *data, size_t len, size_t i) {
    size_t off = 0;
    while (i-- > 0 && off < len)
        off += uc_seq_len((const unsigned char *)data + off, len - off);
    return off;
}

Value em_uc_len(Value s) {
    if (!is_str(s)) rt_fatal("uc_len() expects a str, got %s", type_name(s));
    return em_int((int64_t)uc_count(str_data(&s), str_len(&s)));
}

Value em_uc_ord(Value s) {
    if (!is_str(s)) rt_fatal("uc_ord() expects a str, got %s", type_name(s));
    size_t len = str_len(&s);
    if (len == 0) rt_fatal("uc_ord() of an empty string");
    return em_int(uc_decode((const unsigned char *)str_data(&s), len));
}

Value em_uc_chr(Value n) {
    if (n.tag != V_INT) rt_fatal("uc_chr() expects an int, got %s", type_name(n));
    int64_t cp = n.as.i;
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        rt_fatal("uc_chr() argument out of range (0..0x10FFFF, no surrogates): %" PRId64, cp);
    char buf[4];
    size_t len;
    if (cp < 0x80) {
        buf[0] = (char)cp; len = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        len = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        len = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        len = 4;
    }
    return str_copy(buf, len);
}

Value em_uc_at(Value s, Value i) {
    if (!is_str(s)) rt_fatal("uc_at() expects a str, got %s", type_name(s));
    if (i.tag != V_INT) rt_fatal("uc_at() index must be int, not %s", type_name(i));
    const char *data = str_data(&s);
    size_t len = str_len(&s);
    size_t ncp = uc_count(data, len);
    int64_t idx = i.as.i;
    if (idx < 0) idx += (int64_t)ncp;
    if (idx < 0 || (size_t)idx >= ncp)
        rt_fatal("uc_at() index out of range (index %" PRId64 ", %zu code points)",
                 i.as.i, ncp);
    size_t off = uc_offset(data, len, (size_t)idx);
    size_t sl = uc_seq_len((const unsigned char *)data + off, len - off);
    return str_copy(data + off, sl);
}

/* Code-point slice bounds, clamped like the byte-level `slice` builtin:
 * negatives count from the end and an inverted range is empty. */
static void uc_bounds(int64_t lo, int64_t hi, size_t ncp, size_t *out_lo,
                      size_t *out_hi) {
    int64_t len = (int64_t)ncp;
    if (lo < 0) lo += len;
    if (hi < 0) hi += len;
    if (lo < 0) lo = 0;
    if (hi > len) hi = len;
    if (lo > len) lo = len;
    if (hi < lo) hi = lo;
    *out_lo = (size_t)lo;
    *out_hi = (size_t)hi;
}

Value em_uc_slice(Value s, Value lo, Value hi) {
    if (!is_str(s)) rt_fatal("uc_slice() expects a str, got %s", type_name(s));
    if (lo.tag != V_INT || hi.tag != V_INT)
        rt_fatal("uc_slice() bounds must be int, not %s/%s", type_name(lo),
                 type_name(hi));
    const char *data = str_data(&s);
    size_t len = str_len(&s);
    size_t a, b;
    uc_bounds(lo.as.i, hi.as.i, uc_count(data, len), &a, &b);
    size_t oa = uc_offset(data, len, a);
    size_t ob = uc_offset(data, len, b);
    return str_copy(data + oa, ob - oa);
}

/* Every code point as a one-character string; `for c in unicode.chars(s)`
 * is the way to iterate a string by code points. */
Value em_uc_chars(Value s) {
    if (!is_str(s)) rt_fatal("uc_chars() expects a str, got %s", type_name(s));
    const char *data = str_data(&s);
    size_t len = str_len(&s);
    size_t ncp = uc_count(data, len);
    Value out = obj_val(list_new(ncp));
    RootFrame fr;
    rt_push_frame(&fr, &out, 1);
    Obj *o = out.as.o;
    size_t off = 0;
    for (size_t i = 0; i < ncp; i++) {
        size_t sl = uc_seq_len((const unsigned char *)data + off, len - off);
        o->as.list.items[i] = str_copy(data + off, sl);
        off += sl;
    }
    o->as.list.len = ncp;
    rt_pop_frame();
    return out;
}

/* Strict validity: every byte must belong to a well-formed UTF-8 sequence
 * (no overlong encodings, no surrogates, nothing above U+10FFFF). */
Value em_uc_valid(Value s) {
    if (!is_str(s)) rt_fatal("uc_valid() expects a str, got %s", type_name(s));
    const char *data = str_data(&s);
    size_t len = str_len(&s), off = 0;
    while (off < len) {
        size_t sl = uc_seq_len((const unsigned char *)data + off, len - off);
        if (sl == 1 && (unsigned char)data[off] >= 0x80)
            return em_bool(false); /* continuation byte or failed lead */
        off += sl;
    }
    return em_bool(true);
}
