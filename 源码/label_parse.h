/*
 * label_parse.h —— 从 APK 里读出应用显示名
 *
 * Android 的 pm / dumpsys 都不提供应用名，名字只存在 APK 的两处二进制结构里：
 *   AndroidManifest.xml 的 <application android:label>（可能是字面量，也可能是资源 ID 引用）
 *   resources.arsc 资源表（把 @string/xxx 解析成实际文字，并按系统语言选配置）
 * 这里实现了这两块的解析，只保留「取应用名」所需的最小子集。
 *
 * 依赖：调用方负责把 APK 里的两个文件解出来（模块里用 busybox/toybox unzip -p）。
 */
#ifndef LABEL_PARSE_H
#define LABEL_PARSE_H

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LP_MAX_STR 256
#define LP_MAX_DEPTH 6

/* ---------- 小工具 ---------- */

static uint16_t lp_u16(const unsigned char* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t lp_u32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool lp_read_file(const char* path, unsigned char** out, size_t* out_size) {
    *out = NULL;
    *out_size = 0;
    FILE* fp = fopen(path, "rb");
    if (!fp) return false;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long sz = ftell(fp);
    if (sz <= 0 || sz > 64L * 1024 * 1024) {
        fclose(fp);
        return false;
    }
    rewind(fp);

    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        free(buf);
        return false;
    }
    *out = buf;
    *out_size = got;
    return true;
}

/* ASCII base64，直接打到 stdout，WebUI 侧拼成 data URI */
static void lp_b64_out(const unsigned char* data, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) | data[i + 2];
        putchar(T[(v >> 18) & 63]);
        putchar(T[(v >> 12) & 63]);
        putchar(T[(v >> 6) & 63]);
        putchar(T[v & 63]);
        i += 3;
    }
    if (i < n) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < n) v |= (uint32_t)data[i + 1] << 8;
        putchar(T[(v >> 18) & 63]);
        putchar(T[(v >> 12) & 63]);
        putchar(i + 1 < n ? T[(v >> 6) & 63] : '=');
        putchar('=');
    }
    putchar('\n');
}

/* ---------- 字符串池（ResStringPool） ---------- */

typedef struct {
    const unsigned char* offsets;
    const unsigned char* data;
    size_t data_size;
    uint32_t count;
    uint32_t flags;
} LpPool;

static bool lp_pool_init(LpPool* pool, const unsigned char* chunk, size_t avail) {
    memset(pool, 0, sizeof(*pool));
    if (avail < 28) return false;
    if (lp_u16(chunk) != 0x0001) return false;

    uint16_t header_size = lp_u16(chunk + 2);
    uint32_t size = lp_u32(chunk + 4);
    uint32_t count = lp_u32(chunk + 8);
    uint32_t flags = lp_u32(chunk + 16);
    uint32_t strings_start = lp_u32(chunk + 20);

    if (size < 28 || size > avail) return false;
    if (strings_start > size) return false;
    if ((size_t)header_size + (size_t)count * 4 > size) return false;

    pool->offsets = chunk + header_size;
    pool->data = chunk + strings_start;
    pool->data_size = size - strings_start;
    pool->count = count;
    pool->flags = flags;
    return true;
}

/* 池里的长度字段是 UTF-8 风格变长整数（1 或 2 字节） */
static size_t lp_var_len(const unsigned char* p, size_t avail, size_t* used) {
    *used = 0;
    if (!avail) return 0;
    uint32_t v = p[0];
    if (v & 0x80) {
        if (avail < 2) return 0;
        v = ((v & 0x7f) << 8) | p[1];
        *used = 2;
        return (size_t)v;
    }
    *used = 1;
    return (size_t)v;
}

static bool lp_pool_get(const LpPool* pool, uint32_t idx, char* out, size_t out_size) {
    if (!out || out_size < 2) return false;
    out[0] = '\0';
    if (idx >= pool->count) return false;

    uint32_t off = lp_u32(pool->offsets + (size_t)idx * 4);
    if (off >= pool->data_size) return false;

    const unsigned char* s = pool->data + off;
    size_t avail = pool->data_size - off;
    size_t used = 0;

    if (pool->flags & 0x100) {
        /* UTF-8 池：先 utf16 长度，再 utf8 字节数 */
        (void)lp_var_len(s, avail, &used);
        s += used;
        avail -= used;
        size_t n = lp_var_len(s, avail, &used);
        s += used;
        avail -= used;
        if (n > avail) n = avail;
        if (n >= out_size) n = out_size - 1;
        if (!n) return false;
        memcpy(out, s, n);
        out[n] = '\0';
        return true;
    }

    /* UTF-16 池：长度是 2 字节小端，之后才是 UTF-16LE 字符 */
    if (avail < 2) return false;
    size_t chars = lp_u16(s);
    s += 2;
    avail -= 2;
    if (chars > avail / 2) chars = avail / 2;

    size_t j = 0;
    for (size_t i = 0; i < chars && j + 4 < out_size; i++) {
        uint16_t c = lp_u16(s + i * 2);
        if (c < 0x80) {
            out[j++] = (char)c;
        } else if (c < 0x800) {
            out[j++] = (char)(0xC0 | (c >> 6));
            out[j++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[j++] = (char)(0xE0 | (c >> 12));
            out[j++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[j++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[j] = '\0';
    return j > 0;
}

/* ---------- AndroidManifest.xml（二进制 XML） ---------- */

typedef struct {
    bool has_literal;
    char literal[LP_MAX_STR];
    uint32_t ref;
} LpLabel;

static bool lp_axml_attr(const unsigned char* d, size_t n, const char* want_name, uint32_t want_resid,
                         LpLabel* out) {
    memset(out, 0, sizeof(*out));
    if (n < 8 || lp_u16(d) != 0x0003) return false;

    uint16_t file_header = lp_u16(d + 2);
    LpPool pool;
    bool has_pool = false;
    const unsigned char* res_ids = NULL;
    size_t res_ids_count = 0;

    size_t pos = file_header;
    while (pos + 8 <= n) {
        uint16_t type = lp_u16(d + pos);
        uint16_t header_size = lp_u16(d + pos + 2);
        uint32_t size = lp_u32(d + pos + 4);
        if (size < 8 || pos + size > n) break;

        if (type == 0x0001 && !has_pool) {
            has_pool = lp_pool_init(&pool, d + pos, size);
        } else if (type == 0x0180) {
            /* 资源映射：下标是属性名在字符串池里的索引，值是 android 属性 ID */
            res_ids = d + pos + header_size;
            res_ids_count = (size - header_size) / 4;
        } else if (type == 0x0102) {
            /* START_ELEMENT */
            if (pos + 36 <= n) {
                uint32_t name_idx = lp_u32(d + pos + 20);
                char elem[64];
                if (has_pool && lp_pool_get(&pool, name_idx, elem, sizeof(elem))) {
#ifdef LP_DEBUG
                    fprintf(stderr, "[axml] element=%s attrs=%u\n", elem, (unsigned)lp_u16(d + pos + 28));
#endif
                }
                if (has_pool && lp_pool_get(&pool, name_idx, elem, sizeof(elem)) &&
                    strcmp(elem, "application") == 0) {
                    uint16_t attr_start = lp_u16(d + pos + 24);
                    uint16_t attr_size = lp_u16(d + pos + 26);
                    uint16_t attr_count = lp_u16(d + pos + 28);
                    size_t apos = pos + 16 + attr_start;

                    for (uint16_t i = 0; i < attr_count; i++) {
                        size_t a = apos + (size_t)i * attr_size;
                        if (a + 20 > n) break;

                        uint32_t attr_name = lp_u32(d + a + 4);
                        bool is_label = false;

                        if (res_ids && attr_name < res_ids_count) {
                            is_label = (lp_u32(res_ids + (size_t)attr_name * 4) == want_resid);
                        }
                        if (!is_label && has_pool) {
                            char an[64];
                            if (lp_pool_get(&pool, attr_name, an, sizeof(an))) {
                                is_label = (strcmp(an, want_name) == 0);
                            }
                        }
                        if (!is_label) continue;

                        uint8_t data_type = d[a + 15];
                        uint32_t data = lp_u32(d + a + 16);
#ifdef LP_DEBUG
                        fprintf(stderr, "[axml] attr idx=%u name=%u resid=0x%08x dtype=%u data=0x%08x\n",
                                (unsigned)i, (unsigned)attr_name,
                                (res_ids && attr_name < res_ids_count) ? lp_u32(res_ids + (size_t)attr_name * 4) : 0,
                                (unsigned)data_type, data);
#endif
                        if (data_type == 0x03) {
                            if (has_pool && lp_pool_get(&pool, data, out->literal, sizeof(out->literal))) {
                                out->has_literal = true;
                                return true;
                            }
                            return false;
                        }
                        if (data_type == 0x01) {
                            out->ref = data;
                            return true;
                        }
                        return false;
                    }
                }
            }
        }
        pos += size;
    }
    return false;
}

static bool lp_axml_label(const unsigned char* d, size_t n, LpLabel* out) {
    return lp_axml_attr(d, n, "label", 0x01010001, out);
}

/* ---------- resources.arsc（资源表） ---------- */

typedef struct {
    const unsigned char* d;
    size_t n;
    LpPool values;
    bool has_values;
} LpTable;

static bool lp_arsc_init(LpTable* t, const unsigned char* d, size_t n) {
    memset(t, 0, sizeof(*t));
    if (n < 12 || lp_u16(d) != 0x0002) return false;
    uint16_t header_size = lp_u16(d + 2);
    t->d = d;
    t->n = n;
    if ((size_t)header_size + 8 <= n && lp_u16(d + header_size) == 0x0001) {
        t->has_values = lp_pool_init(&t->values, d + header_size, n - header_size);
    }
    return true;
}

/* 配置里的语言，用于挑中文或默认资源 */
static void lp_cfg_lang(const unsigned char* cfg, size_t avail, char lang[3], char country[3]) {
    lang[0] = lang[1] = lang[2] = '\0';
    country[0] = country[1] = country[2] = '\0';
    if (avail < 12) return;
    lang[0] = (char)cfg[8];
    lang[1] = (char)cfg[9];
    country[0] = (char)cfg[10];
    country[1] = (char)cfg[11];
}

static bool lp_arsc_resolve(const LpTable* t, uint32_t resid, char* out, size_t out_size, int depth);

/* 从某个 ResTable_type 里取出指定 entry 的值 */
/* 从某个 ResTable_type 里取出指定 entry 的原始值（不解引用） */
static bool lp_type_raw(const LpTable* t, size_t chunk, uint16_t header_size, uint16_t entry_id, uint8_t* vtype,
                        uint32_t* vdata) {
    const unsigned char* d = t->d;
    uint32_t entry_count = lp_u32(d + chunk + 12);
    uint32_t entries_start = lp_u32(d + chunk + 16);
    uint8_t flags = d[chunk + 9];
    if (entry_id >= entry_count) return false;

    size_t offsets = chunk + header_size;
    size_t off = 0;
    bool present = false;

    if (flags & 0x01) {
        /* 稀疏表：每项 4 字节（entry 索引 u16 + 偏移/4 u16） */
        for (uint32_t i = 0; i < entry_count; i++) {
            size_t e = offsets + (size_t)i * 4;
            if (e + 4 > t->n) return false;
            if (lp_u16(d + e) == entry_id) {
                off = (size_t)lp_u16(d + e + 2) * 4;
                present = true;
                break;
            }
        }
    } else if (flags & 0x02) {
        size_t e = offsets + (size_t)entry_id * 2;
        if (e + 2 > t->n) return false;
        uint16_t v = lp_u16(d + e);
        if (v != 0xFFFF) {
            off = (size_t)v * 4;
            present = true;
        }
    } else {
        size_t e = offsets + (size_t)entry_id * 4;
        if (e + 4 > t->n) return false;
        uint32_t v = lp_u32(d + e);
        if (v != 0xFFFFFFFFu) {
            off = (size_t)v;
            present = true;
        }
    }
    if (!present) return false;

    size_t ep = chunk + entries_start + off;
    if (ep + 8 > t->n) return false;

    uint16_t entry_size = lp_u16(d + ep);
    uint16_t entry_flags = lp_u16(d + ep + 2);
    if (entry_flags & 0x0001) return false; /* 复杂资源（bag/样式） */
    if (entry_size < 8 || ep + entry_size + 8 > t->n) return false;

    size_t vp = ep + entry_size;
    *vtype = d[vp + 3];
    *vdata = lp_u32(d + vp + 4);
    return true;
}

/* 取字符串值（引用会递归解开） */
static bool lp_type_entry(const LpTable* t, size_t chunk, uint16_t header_size, uint16_t entry_id, char* out,
                          size_t out_size, int depth) {
    uint8_t vtype = 0;
    uint32_t vdata = 0;
    if (!lp_type_raw(t, chunk, header_size, entry_id, &vtype, &vdata)) return false;
    if (vtype == 0x03) return t->has_values && lp_pool_get(&t->values, vdata, out, out_size);
    if (vtype == 0x01) return lp_arsc_resolve(t, vdata, out, out_size, depth + 1);
    return false;
}

static bool lp_arsc_resolve_pass(const LpTable* t, uint8_t pkg_id, uint8_t type_id, uint16_t entry_id, int want_lang,
                                 char* out, size_t out_size, int depth) {
    const unsigned char* d = t->d;
    size_t pos = lp_u16(d + 2);
    while (pos + 8 <= t->n) {
        uint16_t type = lp_u16(d + pos);
        uint16_t header_size = lp_u16(d + pos + 2);
        uint32_t size = lp_u32(d + pos + 4);
        if (size < 8 || pos + size > t->n) break;

#ifdef LP_DEBUG
        if (type == 0x0200) {
            fprintf(stderr, "[arsc] package id=0x%02x hs=%u size=%u want=0x%02x\n",
                    (unsigned)d[pos + 8], (unsigned)header_size, (unsigned)size, (unsigned)pkg_id);
        }
#endif
        if (type == 0x0200 && header_size >= 284 && d[pos + 8] == pkg_id) {
            size_t p = pos + header_size;
            size_t pend = pos + size;
            while (p + 24 <= pend) {
                uint16_t ct = lp_u16(d + p);
                uint16_t chs = lp_u16(d + p + 2);
                uint32_t csz = lp_u32(d + p + 4);
                if (csz < 8 || p + csz > pend) break;

                if (ct == 0x0201 && chs >= 24 && d[p + 8] == type_id) {
                    char lang[3], country[3];
                    lp_cfg_lang(d + p + 20, (size_t)chs - 20, lang, country);
#ifdef LP_DEBUG
                    fprintf(stderr, "[arsc] type chunk id=%u flags=0x%02x cnt=%u start=%u lang=%s-%s want=%d\n",
                            (unsigned)d[p + 8], (unsigned)d[p + 9], (unsigned)lp_u32(d + p + 12),
                            (unsigned)lp_u32(d + p + 16), lang, country, want_lang);
#endif
                    bool zh = (lang[0] == 'z' && lang[1] == 'h');
                    bool def = (lang[0] == 0);
                    bool ok = (want_lang == 2 && zh) || (want_lang == 1 && def) || (want_lang == 0 && !zh && !def);
                    if (ok && lp_type_entry(t, p, chs, entry_id, out, out_size, depth)) return true;
                }
                p += csz;
            }
            return false;
        }
        pos += size;
    }
    return false;
}

static bool lp_arsc_resolve(const LpTable* t, uint32_t resid, char* out, size_t out_size, int depth) {
    if (!t || !resid || depth > LP_MAX_DEPTH) return false;
    uint8_t pkg_id = (uint8_t)(resid >> 24);
    uint8_t type_id = (uint8_t)((resid >> 16) & 0xff);
    uint16_t entry_id = (uint16_t)(resid & 0xffff);

    /* 优先中文，其次默认配置，最后任意语言 */
    for (int want = 2; want >= 0; want--) {
        if (lp_arsc_resolve_pass(t, pkg_id, type_id, entry_id, want, out, out_size, depth)) return true;
    }
    return false;
}

static bool lp_ends_with_ci(const char* s, const char* suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    if (n < m) return false;
    for (size_t i = 0; i < m; i++) {
        if (tolower((unsigned char)s[n - m + i]) != (unsigned char)suffix[i]) return false;
    }
    return true;
}

/* 在资源表里找 icon 对应的位图路径；adaptive icon 的 xml 直接跳过 */
static bool lp_arsc_file_path(const LpTable* t, uint32_t resid, char* out, size_t out_size) {
    if (!t || !resid) return false;
    uint8_t pkg_id = (uint8_t)(resid >> 24);
    uint8_t type_id = (uint8_t)((resid >> 16) & 0xff);
    uint16_t entry_id = (uint16_t)(resid & 0xffff);

    const unsigned char* d = t->d;
    size_t pos = lp_u16(d + 2);
    int best_score = -1;

    while (pos + 8 <= t->n) {
        uint16_t type = lp_u16(d + pos);
        uint16_t header_size = lp_u16(d + pos + 2);
        uint32_t size = lp_u32(d + pos + 4);
        if (size < 8 || pos + size > t->n) break;

        if (type == 0x0200 && header_size >= 284 && d[pos + 8] == pkg_id) {
            size_t p = pos + header_size;
            size_t pend = pos + size;
            while (p + 24 <= pend) {
                uint16_t ct = lp_u16(d + p);
                uint16_t chs = lp_u16(d + p + 2);
                uint32_t csz = lp_u32(d + p + 4);
                if (csz < 8 || p + csz > pend) break;

                if (ct == 0x0201 && chs >= 24 && d[p + 8] == type_id) {
                    uint8_t vtype = 0;
                    uint32_t vdata = 0;
                    char path[LP_MAX_STR];
                    bool have = false;
                    if (lp_type_raw(t, p, chs, entry_id, &vtype, &vdata)) {
                        if (vtype == 0x03) {
                            have = t->has_values && lp_pool_get(&t->values, vdata, path, sizeof(path));
                        } else if (vtype == 0x01) {
                            have = lp_arsc_resolve(t, vdata, path, sizeof(path), 0);
                        }
                    }
                    if (have && (lp_ends_with_ci(path, ".png") || lp_ends_with_ci(path, ".jpg") ||
                                 lp_ends_with_ci(path, ".webp"))) {
                        int score = 0;
                        if (chs >= 36) score = (int)lp_u16(d + p + 34);
                        if (score > 2048) score = 2048;
                        if (score > best_score) {
                            best_score = score;
                            snprintf(out, out_size, "%s", path);
                        }
                    }
                }
                p += csz;
            }
            return best_score >= 0;
        }
        pos += size;
    }
    return false;
}

/* 一步到位：manifest + arsc → 应用名。
 * fw_arsc 可为空；系统应用常把 label 指向 framework-res 里的字符串，需要第二张表兜底。 */
static bool lp_apk_label(const unsigned char* axml, size_t axml_size, const unsigned char* arsc, size_t arsc_size,
                         const unsigned char* fw_arsc, size_t fw_size, char* out, size_t out_size) {
    out[0] = '\0';
    LpLabel label;
    if (!lp_axml_label(axml, axml_size, &label)) return false;

    if (label.has_literal) {
        snprintf(out, out_size, "%s", label.literal);
        return out[0] != '\0';
    }
    if (!label.ref) return false;

    if (arsc && arsc_size) {
        LpTable table;
        if (lp_arsc_init(&table, arsc, arsc_size) && lp_arsc_resolve(&table, label.ref, out, out_size, 0)) return true;
    }
    if (fw_arsc && fw_size) {
        LpTable fw;
        if (lp_arsc_init(&fw, fw_arsc, fw_size) && lp_arsc_resolve(&fw, label.ref, out, out_size, 0)) return true;
    }
    return false;
}

#endif /* LABEL_PARSE_H */
