#include "art_page.h"
#include "art_content.h"
#include <string.h>

typedef struct { const unsigned char *s; size_t n, p; } Json;

static void ws(Json *j) {
    while (j->p < j->n && (j->s[j->p] == ' ' || j->s[j->p] == '\n' ||
           j->s[j->p] == '\r' || j->s[j->p] == '\t')) j->p++;
}

static bool take(Json *j, unsigned char c) {
    ws(j);
    if (j->p >= j->n || j->s[j->p] != c) return false;
    j->p++;
    return true;
}

static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex4(Json *j, uint32_t *value) {
    if (j->n - j->p < 4) return false;
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; i++) {
        int v = hex(j->s[j->p++]);
        if (v < 0) return false;
        n = (n << 4) | (uint32_t)v;
    }
    *value = n;
    return true;
}

/* Returns only well-formed Unicode; truncation never splits a codepoint.
 * JSON controls are replaced for display, but rejected in keys and URLs. */
static bool j_string(Json *j, char *out, size_t cap, bool display, bool *bad) {
    size_t used = 0;
    bool truncated = false;
    if (bad) *bad = false;
    if (out && cap) out[0] = '\0';
    if (!take(j, '"')) return false;
    while (j->p < j->n) {
        uint32_t cp;
        unsigned char c = j->s[j->p++];
        if (c == '"') {
            if (out && cap) out[used] = '\0';
            if (bad) *bad = truncated;
            return true;
        }
        if (c < 0x20) return false;
        if (c == '\\') {
            if (j->p >= j->n) return false;
            c = j->s[j->p++];
            switch (c) {
            case '"': case '\\': case '/': cp = c; break;
            case 'b': cp = 8; break; case 'f': cp = 12; break;
            case 'n': cp = 10; break; case 'r': cp = 13; break; case 't': cp = 9; break;
            case 'u': {
                if (!hex4(j, &cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t low;
                    if (j->n - j->p < 6 || j->s[j->p] != '\\' || j->s[j->p + 1] != 'u') return false;
                    j->p += 2;
                    if (!hex4(j, &low) || low < 0xDC00 || low > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
                break;
            }
            default: return false;
            }
        } else if (c < 0x80) cp = c;
        else {
            unsigned count;
            if (c >= 0xC2 && c <= 0xDF) { cp = c & 0x1F; count = 1; }
            else if (c >= 0xE0 && c <= 0xEF) { cp = c & 0x0F; count = 2; }
            else if (c >= 0xF0 && c <= 0xF4) { cp = c & 7; count = 3; }
            else return false;
            if (j->n - j->p < count) return false;
            for (unsigned i = 0; i < count; i++) {
                unsigned char next = j->s[j->p++];
                if ((next & 0xC0) != 0x80) return false;
                cp = (cp << 6) | (next & 0x3F);
            }
            if ((count == 2 && cp < 0x800) || (count == 3 && cp < 0x10000) ||
                cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        }
        if (cp < 0x20 || cp == 0x7F) {
            if (out && !display) truncated = true;
            cp = ' ';
        }
        unsigned char encoded[4];
        unsigned n;
        if (cp < 0x80) { encoded[0] = (unsigned char)cp; n = 1; }
        else if (cp < 0x800) {
            encoded[0] = 0xC0 | (cp >> 6); encoded[1] = 0x80 | (cp & 63); n = 2;
        } else if (cp < 0x10000) {
            encoded[0] = 0xE0 | (cp >> 12); encoded[1] = 0x80 | ((cp >> 6) & 63);
            encoded[2] = 0x80 | (cp & 63); n = 3;
        } else {
            encoded[0] = 0xF0 | (cp >> 18); encoded[1] = 0x80 | ((cp >> 12) & 63);
            encoded[2] = 0x80 | ((cp >> 6) & 63); encoded[3] = 0x80 | (cp & 63); n = 4;
        }
        if (out) {
            if (!cap || used + n >= cap) truncated = true;
            else if (!truncated) for (unsigned i = 0; i < n; i++) out[used++] = (char)encoded[i];
        }
    }
    return false;
}

static bool j_literal(Json *j, const char *word) {
    ws(j);
    size_t p = j->p;
    while (*word) {
        if (p >= j->n || j->s[p++] != (unsigned char)*word++) return false;
    }
    j->p = p;
    return true;
}

static bool j_skip(Json *j, unsigned depth);

static bool j_number(Json *j) {
    ws(j);
    if (j->p < j->n && j->s[j->p] == '-') j->p++;
    if (j->p >= j->n) return false;
    if (j->s[j->p] == '0') j->p++;
    else {
        if (j->s[j->p] < '1' || j->s[j->p] > '9') return false;
        while (j->p < j->n && j->s[j->p] >= '0' && j->s[j->p] <= '9') j->p++;
    }
    if (j->p < j->n && j->s[j->p] == '.') {
        j->p++;
        size_t start = j->p;
        while (j->p < j->n && j->s[j->p] >= '0' && j->s[j->p] <= '9') j->p++;
        if (j->p == start) return false;
    }
    if (j->p < j->n && (j->s[j->p] == 'e' || j->s[j->p] == 'E')) {
        j->p++;
        if (j->p < j->n && (j->s[j->p] == '+' || j->s[j->p] == '-')) j->p++;
        size_t start = j->p;
        while (j->p < j->n && j->s[j->p] >= '0' && j->s[j->p] <= '9') j->p++;
        if (j->p == start) return false;
    }
    return true;
}

static bool j_skip(Json *j, unsigned depth) {
    if (depth > 16) return false;
    ws(j);
    if (j->p >= j->n) return false;
    unsigned char c = j->s[j->p];
    if (c == '"') return j_string(j, NULL, 0, true, NULL);
    if (c == '{' || c == '[') {
        bool object = c == '{';
        j->p++;
        ws(j);
        if (j->p < j->n && j->s[j->p] == (object ? '}' : ']')) { j->p++; return true; }
        for (;;) {
            if (object && (!j_string(j, NULL, 0, true, NULL) || !take(j, ':'))) return false;
            if (!j_skip(j, depth + 1)) return false;
            ws(j);
            if (j->p < j->n && j->s[j->p] == (object ? '}' : ']')) { j->p++; return true; }
            if (!take(j, ',')) return false;
        }
    }
    if (c == 't') return j_literal(j, "true");
    if (c == 'f') return j_literal(j, "false");
    if (c == 'n') return j_literal(j, "null");
    return j_number(j);
}

static bool j_bool(Json *j, bool *known, bool *value, unsigned depth) {
    ws(j);
    if (j->p < j->n && j->s[j->p] == 't') { *known = true; *value = true; return j_literal(j, "true"); }
    if (j->p < j->n && j->s[j->p] == 'f') { *known = true; *value = false; return j_literal(j, "false"); }
    *known = false;
    return j_skip(j, depth);
}

static bool j_uint(Json *j, uint32_t limit, bool *known, uint32_t *value, unsigned depth) {
    ws(j);
    size_t start = j->p;
    if (!j_skip(j, depth)) return false;
    size_t end = j->p;
    uint32_t number = 0;
    *known = start < end;
    for (size_t i = start; i < end && *known; i++) {
        unsigned char c = j->s[i];
        if (c < '0' || c > '9') *known = false;
        else {
            uint32_t digit = (uint32_t)(c - '0');
            if (digit > limit || number > (limit - digit) / 10) *known = false;
            else number = number * 10 + digit;
        }
    }
    if (*known) *value = number;
    return true;
}

static bool j_text(Json *j, char *out, size_t capacity, bool display,
                   bool allow_truncation, bool *known, unsigned depth) {
    ws(j);
    if (j->p >= j->n || j->s[j->p] != '"') { *known = false; return j_skip(j, depth); }
    bool bad = false;
    if (!j_string(j, out, capacity, display, &bad)) return false;
    *known = out[0] != '\0' && (!bad || allow_truncation);
    return true;
}

static unsigned lower_ascii(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool suffix(const char *host, size_t length, const char *domain, bool subdomains) {
    size_t n = strlen(domain);
    if (length < n || (!subdomains && length != n)) return false;
    if (length > n && host[length - n - 1] != '.') return false;
    for (size_t i = 0; i < n; i++)
        if (lower_ascii((unsigned char)host[length - n + i]) != (unsigned char)domain[i]) return false;
    return true;
}

static bool allowed_url(const char *url, bool artwork_page) {
    static const char prefix[] = "https://";
    for (size_t i = 0; i < sizeof(prefix) - 1; i++) if (url[i] != prefix[i]) return false;
    const char *host = url + sizeof(prefix) - 1;
    size_t n = 0;
    while (host[n] && host[n] != '/' && host[n] != '?' && host[n] != '#') {
        unsigned char c = (unsigned char)host[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
        n++;
        if (n > 253) return false;
    }
    if (!n || host[0] == '.' || host[n - 1] == '.') return false;
    for (size_t i = 1; i < n; i++) if (host[i] == '.' && host[i - 1] == '.') return false;
    bool allowed = artwork_page ?
        (suffix(host, n, "www.deviantart.com", false) || suffix(host, n, "deviantart.com", false)) :
        (suffix(host, n, "deviantart.com", true) || suffix(host, n, "deviantart.net", true) ||
         suffix(host, n, "wixmp.com", true));
    if (!allowed) return false;
    for (const char *p = host + n; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c >= 0x7F || c == '\\' || c == '#') return false;
    }
    return true;
}

static bool valid_id(const char *id) {
    if (!id[0]) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
        unsigned c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

static bool key_is(const char *key, bool bad, const char *expected) {
    return !bad && strcmp(key, expected) == 0;
}

typedef struct { char url[1024]; uint32_t width, height; bool valid; } Candidate;

static bool candidate_sufficient(uint32_t width, uint32_t height) {
    /* Select the smallest variant that can fill the native 400x240 top screen.
     * A 320px preview was visibly enlarged and softened in the previous build. */
    return (uint64_t)width * 240u >= (uint64_t)height * 400u ? width >= 400u : height >= 240u;
}

static void candidate_consider(Candidate *best, Candidate *largest, const Candidate *next) {
    if (!next->valid || !next->width || !next->height ||
        next->width > ART_PREVIEW_MAX_DIMENSION || next->height > ART_PREVIEW_MAX_DIMENSION ||
        (uint64_t)next->width * next->height > ART_PREVIEW_MAX_PIXELS || !allowed_url(next->url, false)) return;
    uint64_t area = (uint64_t)next->width * next->height;
    uint64_t largest_area = (uint64_t)largest->width * largest->height;
    if (!largest->valid || area > largest_area) *largest = *next;
    uint64_t old_area = (uint64_t)best->width * best->height;
    bool enough = candidate_sufficient(next->width, next->height);
    bool old_enough = best->valid && candidate_sufficient(best->width, best->height);
    if (!best->valid || (enough && !old_enough) ||
        (enough == old_enough && ((enough && area < old_area) || (!enough && area > old_area))))
        *best = *next;
}

static bool j_media(Json *j, Candidate *best, Candidate *largest, unsigned depth) {
    ws(j);
    if (j->p >= j->n || j->s[j->p] != '{') return j_skip(j, depth);
    Candidate media = {{0}, 0, 0, false};
    bool has_src = false, has_w = false, has_h = false, bad = false;
    unsigned seen = 0;
    if (!take(j, '{')) return false;
    ws(j);
    if (j->p < j->n && j->s[j->p] == '}') { j->p++; return true; }
    for (;;) {
        char key[64]; bool key_bad;
        if (!j_string(j, key, sizeof(key), false, &key_bad) || !take(j, ':')) return false;
        unsigned flag = key_is(key, key_bad, "src") ? 1 : key_is(key, key_bad, "width") ? 2 :
                        key_is(key, key_bad, "height") ? 4 : 0;
        if (flag && (seen & flag)) bad = true;
        seen |= flag;
        if (flag == 1) {
            if (!j_text(j, media.url, sizeof(media.url), false, false, &has_src, depth + 1)) return false;
        } else if (flag == 2) {
            if (!j_uint(j, ART_PREVIEW_MAX_DIMENSION, &has_w, &media.width, depth + 1)) return false;
        } else if (flag == 4) {
            if (!j_uint(j, ART_PREVIEW_MAX_DIMENSION, &has_h, &media.height, depth + 1)) return false;
        } else if (!j_skip(j, depth + 1)) return false;
        ws(j);
        if (j->p < j->n && j->s[j->p] == '}') { j->p++; break; }
        if (!take(j, ',')) return false;
    }
    media.valid = !bad && has_src && has_w && has_h;
    candidate_consider(best, largest, &media);
    return true;
}

static bool j_thumbs(Json *j, Candidate *best, Candidate *largest, unsigned depth) {
    ws(j);
    if (j->p >= j->n || j->s[j->p] != '[') return j_skip(j, depth);
    if (!take(j, '[')) return false;
    ws(j);
    if (j->p < j->n && j->s[j->p] == ']') { j->p++; return true; }
    for (;;) {
        if (!j_media(j, best, largest, depth + 1)) return false;
        ws(j);
        if (j->p < j->n && j->s[j->p] == ']') { j->p++; return true; }
        if (!take(j, ',')) return false;
    }
}

static bool j_author(Json *j, char *artist, size_t capacity, bool *known, unsigned depth) {
    ws(j);
    *known = false;
    if (j->p >= j->n || j->s[j->p] != '{') return j_skip(j, depth);
    bool seen = false, duplicate = false;
    if (!take(j, '{')) return false;
    ws(j);
    if (j->p < j->n && j->s[j->p] == '}') { j->p++; return true; }
    for (;;) {
        char key[64]; bool bad;
        if (!j_string(j, key, sizeof(key), false, &bad) || !take(j, ':')) return false;
        if (key_is(key, bad, "username")) {
            if (seen) duplicate = true;
            seen = true;
            if (!j_text(j, artist, capacity, false, false, known, depth + 1)) return false;
        } else if (!j_skip(j, depth + 1)) return false;
        ws(j);
        if (j->p < j->n && j->s[j->p] == '}') { j->p++; break; }
        if (!take(j, ',')) return false;
    }
    if (duplicate) *known = false;
    return true;
}

static bool j_item(Json *j, bool filtering, ArtPage *page, unsigned depth) {
    ws(j);
    if (j->p >= j->n || j->s[j->p] != '{') return j_skip(j, depth);
    ArtPageItem item;
    Candidate best, largest;
    memset(&item, 0, sizeof(item));
    memset(&best, 0, sizeof(best));
    memset(&largest, 0, sizeof(largest));
    uint32_t flags = 0;
    unsigned seen = 0;
    bool bad = false, has_id = false, has_url = false, has_artist = false;
    bool has_title = false, blocked = false;
    if (!take(j, '{')) return false;
    ws(j);
    if (j->p < j->n && j->s[j->p] == '}') { j->p++; return true; }
    for (;;) {
        char key[64]; bool key_bad, known, value;
        if (!j_string(j, key, sizeof(key), false, &key_bad) || !take(j, ':')) return false;
        unsigned field = key_is(key, key_bad, "deviationid") ? 1u :
                         key_is(key, key_bad, "url") ? 2u :
                         key_is(key, key_bad, "title") ? 4u :
                         key_is(key, key_bad, "author") ? 8u :
                         key_is(key, key_bad, "is_deleted") ? 16u :
                         key_is(key, key_bad, "is_mature") ? 32u :
                         key_is(key, key_bad, "is_blocked") ? 64u :
                         key_is(key, key_bad, "preview") ? 128u :
                         key_is(key, key_bad, "social_preview") ? 256u :
                         key_is(key, key_bad, "thumbs") ? 512u : 0u;
        if (field && (seen & field)) bad = true;
        seen |= field;
        if (field == 1) {
            if (!j_text(j, item.id, sizeof(item.id), false, false, &has_id, depth + 1)) return false;
        } else if (field == 2) {
            if (!j_text(j, item.page_url, sizeof(item.page_url), false, false, &has_url, depth + 1)) return false;
        } else if (field == 4) {
            if (!j_text(j, item.title, sizeof(item.title), true, true, &has_title, depth + 1)) return false;
        } else if (field == 8) {
            if (!j_author(j, item.artist, sizeof(item.artist), &has_artist, depth + 1)) return false;
        } else if (field == 16 || field == 32 || field == 64) {
            if (!j_bool(j, &known, &value, depth + 1)) return false;
            if (field == 16 && known) flags |= ART_FLAG_DELETED_KNOWN | (value ? ART_FLAG_DELETED : 0);
            if (field == 32 && known) flags |= ART_FLAG_MATURE_KNOWN | (value ? ART_FLAG_MATURE : 0);
            if (field == 64) { if (!known) bad = true; else blocked = value; }
        } else if (field == 128 || field == 256) {
            if (!j_media(j, &best, &largest, depth + 1)) return false;
        } else if (field == 512) {
            if (!j_thumbs(j, &best, &largest, depth + 1)) return false;
        } else if (!j_skip(j, depth + 1)) return false;
        ws(j);
        if (j->p < j->n && j->s[j->p] == '}') { j->p++; break; }
        if (!take(j, ',')) return false;
    }
    if (bad || blocked || !has_id || !valid_id(item.id) || !has_url ||
        !allowed_url(item.page_url, true) || !has_artist || !art_content_allowed(filtering, flags))
        return true;
    if (!has_title) memcpy(item.title, "Sans titre", sizeof("Sans titre"));
    if (best.valid) {
        memcpy(item.preview_url, best.url, sizeof(item.preview_url));
        item.preview_width = best.width;
        item.preview_height = best.height;
    }
    if (page->count < ART_PAGE_MAX_ITEMS) {
        unsigned index = page->count++;
        page->items[index] = item;
        uint64_t best_area = (uint64_t)best.width * best.height;
        uint64_t largest_area = (uint64_t)largest.width * largest.height;
        /* Avoid another request for nearly identical variants; fetch a sharper
         * copy only when it has at least 50% more pixels than the quick preview. */
        if (best.valid && largest.valid && largest_area >= best_area + best_area / 2 &&
            strcmp(best.url, largest.url) != 0)
            memcpy(page->zoom_urls[index], largest.url, sizeof(page->zoom_urls[index]));
    }
    return true;
}

static bool j_results(Json *j, bool filtering, ArtPage *page, unsigned depth) {
    if (!take(j, '[')) return false;
    ws(j);
    if (j->p < j->n && j->s[j->p] == ']') { j->p++; return true; }
    for (;;) {
        if (!j_item(j, filtering, page, depth + 1)) return false;
        ws(j);
        if (j->p < j->n && j->s[j->p] == ']') { j->p++; return true; }
        if (!take(j, ',')) return false;
    }
}

ArtPageStatus art_parse_page(const char *json, size_t length, bool filter_enabled, ArtPage *out) {
    if (!out) return ART_PAGE_INVALID;
    memset(out, 0, sizeof(*out));
    if (length > ART_PAGE_MAX_BYTES) return ART_PAGE_TOO_LARGE;
    if (!json || !length) return ART_PAGE_INVALID;
    Json j = { (const unsigned char *)json, length, 0 };
    unsigned seen = 0;
    bool has_results = false, has_more_known = false;
    if (!take(&j, '{')) goto invalid;
    ws(&j);
    if (j.p < j.n && j.s[j.p] == '}') goto invalid;
    for (;;) {
        char key[64]; bool bad, known, value;
        if (!j_string(&j, key, sizeof(key), false, &bad) || !take(&j, ':')) goto invalid;
        unsigned field = key_is(key, bad, "results") ? 1u :
                         key_is(key, bad, "has_more") ? 2u :
                         key_is(key, bad, "next_offset") ? 4u :
                         key_is(key, bad, "next_cursor") ? 8u : 0u;
        if (field && (seen & field)) goto invalid;
        seen |= field;
        if (field == 1) {
            if (!j_results(&j, filter_enabled, out, 2)) goto invalid;
            has_results = true;
        } else if (field == 2) {
            if (!j_bool(&j, &known, &value, 2)) goto invalid;
            has_more_known = known;
            if (known) out->has_more = value ? 1u : 0u;
        } else if (field == 4) {
            if (!j_uint(&j, 50000, &known, &out->next_offset, 2)) goto invalid;
            out->has_next_offset = known ? 1u : 0u;
        } else if (field == 8) {
            if (!j_text(&j, out->next_cursor, sizeof(out->next_cursor), false, false, &known, 2)) goto invalid;
            if (!known) out->next_cursor[0] = '\0';
        } else if (!j_skip(&j, 2)) goto invalid;
        ws(&j);
        if (j.p < j.n && j.s[j.p] == '}') { j.p++; break; }
        if (!take(&j, ',')) goto invalid;
    }
    ws(&j);
    if (j.p != j.n || !has_results || !has_more_known) goto invalid;
    return ART_PAGE_OK;
invalid:
    memset(out, 0, sizeof(*out));
    return ART_PAGE_INVALID;
}
