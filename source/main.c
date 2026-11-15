#include <3ds.h>
#include <3ds/services/apt.h>
#include <stdio.h>
#include <jpeglib.h>
#include <png.h>
#include <dirent.h>
#include <setjmp.h>
#include <limits.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <3ds/services/ps.h>
#include <3ds/services/soc.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <stdatomic.h>
#include "auth_listener.h"
#include "auth_qr.h"
#include "oauth_exchange.h"
#include "oauth_token.h"
#include "oauth_transport.h"
#include "oauth_session.h"
#include "deviantart_config.h"
#include "art_page.h"
#include "art_content.h"

#define TOP_WIDTH 400
#define TOP_HEIGHT 240
#define MAX_STEREO_SHIFT 6
#define IMAGE_MAX_WIDTH 400
#define IMAGE_BUFFER_MAX_WIDTH 1440
#define IMAGE_BUFFER_MAX_HEIGHT 960
#define MAX_DECODE_DIMENSION 16384
#define MAX_ENTRIES 256
#define DIRECTORY_CACHE_LIMIT 128
#define DIRECTORY_CACHE_BUDGET (8U * 1024U * 1024U)
#define DIRECTORY_PROBE_LIMIT 512
#define NAME_SIZE 256
#define PATH_SIZE 512
#define GUIDE_FLAG_PATH "sdmc:/3ds/ShieldOnlineArt/guide_seen.dat"
#define FAVORITES_PATH "sdmc:/3ds/ShieldOnlineArt/favorites.dat"
#define FAVORITES_TEMP_PATH "sdmc:/3ds/ShieldOnlineArt/favorites.tmp"
#define AUTH_REFRESH_PATH "sdmc:/3ds/ShieldOnlineArt/refresh.dat"
#define AUTH_REFRESH_TEMP_PATH "sdmc:/3ds/ShieldOnlineArt/refresh.tmp"
#define AUTH_FORM_CAPACITY 8193
#define FAVORITES_MAX 32
#define LOCAL_VISIBLE_ROWS 5
#define LOCAL_LIST_Y 96
#define LOCAL_ROW_HEIGHT 16
#define ONLINE_VISIBLE_ROWS 6
#define ONLINE_LIST_Y 72
#define ONLINE_ROW_HEIGHT 16
#define SEARCH_CAPACITY 65

typedef enum { TAB_DISCOVER, TAB_LOCAL, TAB_FAVORITES, TAB_COUNT } AppTab;
typedef enum {
    AUTH_JOB_NONE, AUTH_JOB_TOKEN, AUTH_JOB_REFRESH, AUTH_JOB_TAG,
    AUTH_JOB_PREVIEW, AUTH_JOB_ZOOM_PREVIEW
} AuthJob;

typedef struct {
    FILE *file;
    const u8 *memory;
    size_t length, position;
} PngInput;

typedef struct {
    struct jpeg_error_mgr base;
    struct jpeg_progress_mgr progress;
    jmp_buf jump;
} JpegError;

typedef enum {
    IMAGE_FORMAT_UNSUPPORTED,
    IMAGE_FORMAT_JPEG,
    IMAGE_FORMAT_PNG,
    IMAGE_FORMAT_PNG_INTERLACED
} ImageFormat;

typedef struct {
    char name[NAME_SIZE];
    bool directory;
    long long modified;
} ImageEntry;

typedef struct {
    ImageEntry *entries;
    unsigned entry_count, selection, image_count;
    char directory[PATH_SIZE];
    char image_path[PATH_SIZE];
    char status[96];
} BrowserSnapshot;

typedef struct {
    char path[PATH_SIZE];
    bool contains_image;
} DirectoryProbe;

typedef struct {
    unsigned src_w, src_h, dst_w, dst_h;
    unsigned active_y, rows_in_band;
    u8 *output;
    u32 *row_sum;
} Resampler;

typedef struct {
    u8 *pixels;
    unsigned width, height;
} ImageLevel;

typedef struct {
    unsigned first, second, fraction;
} SampleAxis;

typedef struct {
    char magic[8];
    u32 version, count, checksum;
} FavoritesHeader;

typedef struct {
    SampleAxis sample;
    size_t left_column, right_column;
    bool left_visible, right_visible;
} RenderColumn;

static PrintConsole bottom_console;
static ImageEntry entries[MAX_ENTRIES];
static unsigned entry_count, selection, image_count;
static char current_dir[PATH_SIZE] = "sdmc:/";
static char current_path[PATH_SIZE];
static char status_text[96] = "Choisis un dossier ou une image.";
static BrowserSnapshot directory_cache[DIRECTORY_CACHE_LIMIT];
static unsigned directory_cache_count;
static BrowserSnapshot root_directory_cache;
// Reserve the root listing so it remains available even if malloc later fails.
static ImageEntry root_directory_entries[MAX_ENTRIES];
static bool root_directory_cached;
static size_t directory_cache_bytes = sizeof(directory_cache) +
    sizeof(root_directory_cache) + sizeof(root_directory_entries);
static DirectoryProbe directory_probes[DIRECTORY_PROBE_LIMIT];
static unsigned directory_probe_count;
static unsigned image_width, image_height, shown_width, shown_height;
static bool viewing_image;
static bool guide_active;
static u8 *current_image_pixels;
static unsigned current_image_w, current_image_h;
static ImageLevel image_levels[2];
static unsigned zoom_percent = 100;
static unsigned rotation_quadrants;
static int pan_offset_x, pan_offset_y;
static int pan_remainder_x, pan_remainder_y;
static int brightness_level = 3;
static int brightness_on_launch = 3;
static u8 backlight_config[2] = {0, 3};
static bool cfgu_service_ready, backlight_config_valid;
static bool lcd_ready, brightness_changed;
// Stereo uses paired framebuffers only while an image is open. The same
// filtered source pixels are sampled once and written for both eyes.
static const bool stereo_supported = true;
static bool stereo_active;
static int stereo_shift;
static bool top_frame_dirty, bottom_frame_dirty;
static bool battery_service_ready;
static int battery_percent = -1, battery_temperature_c = -1;
static u64 battery_last_update;
static AppTab active_tab = TAB_DISCOVER;
static bool local_browser_ready, quit_requested, loading_cancelled;
static bool explicit_filter_enabled = true;
static bool filter_choice_pending = true;
static unsigned guide_page;
static char search_tag[SEARCH_CAPACITY];
static char tab_status[TAB_COUNT][96] = {
    "Connexion DeviantArt a configurer.", "Choisis un dossier ou une image.",
    "Favoris sauvegardes sur la carte SD."
};
static char decode_error_text[96];
static const char *loading_label = "Preparation";
static unsigned loading_done, loading_total;
static u64 loading_last_poll, loading_last_draw;
static int last_tap_entry = -1;
static u64 last_tap_time;
static u64 filter_notice_until;
static bool auth_screen_active, auth_ps_ready, auth_soc_ready, auth_romfs_ready, auth_curl_ready;
static bool auth_thread_done, auth_thread_ok;
static volatile bool auth_cancel_requested;
static LightLock auth_result_lock;
static u32 *auth_soc_context;
static char *auth_ca_bundle;
static size_t auth_ca_bundle_length;
static OAuthSession auth_session;
static OAuthToken auth_token;
static AuthListener auth_listener = {-1, -1, 0, {0}, 0};
static Thread auth_thread;
static bool auth_thread_running;
static bool save_login_prompt_pending;
static AuthJob auth_job;
static OAuthTransfer auth_transfer;
static unsigned auth_progress_drawn_bytes;
static u64 auth_progress_last_draw;
static long auth_http_status;
static u64 auth_token_expires_at;
static char saved_refresh_token[OAUTH_REFRESH_TOKEN_CAPACITY];
static u8 auth_entropy[OAUTH_VERIFIER_ENTROPY + OAUTH_STATE_ENTROPY];
static char auth_url[OAUTH_AUTH_URL_CAPACITY];
static char auth_form[AUTH_FORM_CAPACITY];
static char auth_response[ART_PAGE_MAX_BYTES + 1];
static char auth_tag_query[SEARCH_CAPACITY];
static char auth_preview_url[1024];
static char auth_preview_mime[128];
static char auth_preview_target_id[64];
static u8 *auth_preview_bytes;
static size_t auth_preview_length;
static ArtPage online_page;
static bool online_page_valid;
static unsigned online_selection;
static unsigned online_list_start;
static unsigned auth_preview_item_index;
static unsigned viewed_online_item_index;
static ArtPageItem favorite_items[FAVORITES_MAX];
static unsigned favorite_count;
static bool favorites_loaded;
static bool auth_preview_from_favorites;
static bool auth_preview_is_zoom;
static bool preserve_image_view_on_decode;
static char zoom_attempted_item_id[64];
static char zoom_loaded_item_id[64];
static bool viewed_online_item_valid;
static ArtPageItem viewed_online_item;
static bool home_allowed_on_launch;
static u8 auth_qr_work[AUTH_QR_BUFFER_SIZE], auth_qr_result[AUTH_QR_BUFFER_SIZE];
static char auth_local_address[24];

static bool render_current_image(void);
static void clear_top(void);
static void draw_loading_screen(void);
static void restore_top_after_loading(void);
static void begin_loading(const char *label);
static bool poll_loading(unsigned done, unsigned total);
static void draw_ui(void);
static void lower_rect(int x, int y, int width, int height, u16 color);
static void draw_home_screen(void);
static void start_deviantart_login(void);
static void start_tag_search(void);
static const char *available_refresh_token(void);
static void draw_save_login_prompt(void);
static void download_online_preview(unsigned index, bool zoom_variant);
static void auth_token_worker(void *unused);
static bool decode_online_preview(const u8 *bytes, size_t length, const char *mime,
                                  const ArtPageItem *item);
static void close_auth_services(void);

static void present_frame(void) {
    // Swap a completed top frame only. An idle loop must not alternate the
    // last photo with an old guide/loading/empty framebuffer.
    if (top_frame_dirty || bottom_frame_dirty) gfxFlushBuffers();
    if (top_frame_dirty) {
        gfxScreenSwapBuffers(GFX_TOP, stereo_active);
        top_frame_dirty = false;
    }
    if (bottom_frame_dirty) {
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
        bottom_frame_dirty = false;
    }
    gspWaitForVBlank();
}

static void print_safe_name(const char *name, size_t max_chars) {
    char text[41];
    if (max_chars > sizeof(text) - 1) max_chars = sizeof(text) - 1;
    size_t shown = 0;
    const unsigned char *p = (const unsigned char *)name;
    while (*p && shown < max_chars) {
        if (*p < 0x20 || *p >= 0x80) {
            text[shown] = '?';
            if ((*p & 0xE0) == 0xC0 && p[1]) p += 2;
            else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) p += 3;
            else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) p += 4;
            else p++;
        } else {
            text[shown] = (char)*p++;
        }
        shown++;
    }
    if (*p && shown >= 3) memset(text + shown - 3, '.', 3);
    text[shown] = '\0';
    fputs(text, stdout);
}

static void jpeg_fail(j_common_ptr common) {
    JpegError *error = (JpegError *)common->err;
    longjmp(error->jump, 1);
}

static void jpeg_quiet(j_common_ptr common) { (void)common; }

static void jpeg_progress(j_common_ptr common) {
    if (!poll_loading(loading_done, loading_total)) {
        JpegError *error = (JpegError *)common->err;
        longjmp(error->jump, 1);
    }
}

static void png_fail(png_structp png, png_const_charp message) {
    (void)message;
    png_longjmp(png, 1);
}

static void png_quiet(png_structp png, png_const_charp message) {
    (void)png; (void)message;
}

static void png_file_read(png_structp png, png_bytep out, png_size_t bytes) {
    PngInput *input = (PngInput *)png_get_io_ptr(png);
    if (!poll_loading(loading_done, loading_total)) png_error(png, "Cancelled");
    if (!input) png_error(png, "Missing image source");
    if (input->memory) {
        if (input->position > input->length || bytes > input->length - input->position)
            png_error(png, "Truncated image");
        memcpy(out, input->memory + input->position, bytes);
        input->position += bytes;
    } else if (!input->file || fread(out, 1, bytes, input->file) != bytes) {
        png_error(png, "Truncated image");
    }
}

static bool has_image_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[8] = {0};
    size_t n = strlen(dot);
    if (n >= sizeof(ext)) return false;
    for (size_t i = 0; i <= n; i++) {
        char c = dot[i];
        ext[i] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
    }
    return !strcmp(ext, ".jpg") || !strcmp(ext, ".jpeg") ||
           !strcmp(ext, ".jpe") || !strcmp(ext, ".jfif") || !strcmp(ext, ".png");
}

static ImageFormat probe_image_format(FILE *file) {
    if (!file || fseek(file, 0, SEEK_SET) != 0) return IMAGE_FORMAT_UNSUPPORTED;
    u8 header[29] = {0};
    size_t n = fread(header, 1, sizeof(header), file);
    clearerr(file);
    if (fseek(file, 0, SEEK_SET) != 0) return IMAGE_FORMAT_UNSUPPORTED;

    // The file signature, not its suffix, chooses the decoder. This handles
    // common mislabeled JPEG/PNG files without feeding bytes to the wrong codec.
    if (n >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF)
        return IMAGE_FORMAT_JPEG;
    if (n < 8 || png_sig_cmp(header, 0, 8) != 0) return IMAGE_FORMAT_UNSUPPORTED;

    // PNG IHDR's interlace byte is at offset 28. Interlaced PNG needs a full
    // image of pass state; reject it explicitly to keep Old 3DS RAM bounded.
    if (n >= sizeof(header) && header[8] == 0 && header[9] == 0 &&
        header[10] == 0 && header[11] == 13 && !memcmp(header + 12, "IHDR", 4) &&
        header[28] != 0)
        return IMAGE_FORMAT_PNG_INTERLACED;
    return IMAGE_FORMAT_PNG;
}

static bool has_readable_image(const char *path, const char *name) {
    if (!has_image_extension(name)) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    ImageFormat format = probe_image_format(file);
    fclose(file);
    return format != IMAGE_FORMAT_UNSUPPORTED;
}

static void join_path(char *out, size_t capacity, const char *directory, const char *name) {
    size_t len = strlen(directory);
    size_t name_len = strlen(name);
    bool separator = len && directory[len - 1] != '/';
    size_t needed = len + (separator ? 1 : 0) + name_len + 1;
    if (needed > capacity) { if (capacity) out[0] = '\0'; return; }
    memmove(out, directory, len);
    size_t at = len;
    if (separator) out[at++] = '/';
    memmove(out + at, name, name_len);
    out[at + name_len] = '\0';
}

static void remember_directory_probe(const char *path, bool contains_image) {
    if (directory_probe_count == DIRECTORY_PROBE_LIMIT) return;
    DirectoryProbe *probe = &directory_probes[directory_probe_count++];
    strcpy(probe->path, path);
    probe->contains_image = contains_image;
}

static bool directory_contains_image(const char *path, unsigned depth, unsigned *budget,
                                     bool *complete) {
    *complete = true;
    for (unsigned i = 0; i < directory_probe_count; i++) {
        if (!strcmp(directory_probes[i].path, path))
            return directory_probes[i].contains_image;
    }
    if (!depth || !*budget) { *complete = false; return false; }
    (*budget)--;
    DIR *dir = opendir(path);
    if (!dir) { *complete = false; return false; }
    bool found = false;
    struct dirent *item;
    while (!found && (item = readdir(dir)) != NULL) {
        if (!poll_loading(loading_done + 1, 0)) { *complete = false; break; }
        if (!*budget) { *complete = false; break; }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        unsigned char type = item->d_type;
        bool image_file = has_image_extension(item->d_name);
        if (type != DT_DIR && type != DT_UNKNOWN &&
            (type != DT_REG || !image_file)) continue;
        char child[PATH_SIZE];
        join_path(child, sizeof(child), path, item->d_name);
        if (!child[0]) { *complete = false; continue; }
        if (type == DT_UNKNOWN) {
            struct stat info;
            if (stat(child, &info) != 0) { *complete = false; continue; }
            type = S_ISDIR(info.st_mode) ? DT_DIR :
                   S_ISREG(info.st_mode) ? DT_REG : DT_UNKNOWN;
        }
        if (type == DT_DIR) {
            bool child_complete;
            found = directory_contains_image(child, depth - 1, budget, &child_complete);
            if (!child_complete) *complete = false;
        }
        else if (type == DT_REG && image_file && has_readable_image(child, item->d_name))
            found = true;
    }
    closedir(dir);
    if (!found && !*budget) *complete = false;
    if (loading_cancelled || quit_requested) { *complete = false; return false; }
    if (found) *complete = true;
    if (*complete) remember_directory_probe(path, found);
    return found;
}

static int compare_entries(const void *left, const void *right) {
    const ImageEntry *a = (const ImageEntry *)left;
    const ImageEntry *b = (const ImageEntry *)right;
    if (a->directory != b->directory) return a->directory ? -1 : 1;
    if (!a->directory && a->modified != b->modified)
        return a->modified > b->modified ? -1 : 1;
    return strcasecmp(a->name, b->name);
}

static bool scan_directory(void) {
    char scan_status[sizeof(status_text)];
    memcpy(scan_status, status_text, sizeof(scan_status));
    begin_loading("Analyse des dossiers");
    DIR *directory = opendir(current_dir);
    if (!directory) {
        restore_top_after_loading();
        snprintf(status_text, sizeof(status_text), "Dossier inaccessible.");
        return false;
    }
    ImageEntry *scanned = malloc(sizeof(entries));
    if (!scanned) {
        closedir(directory);
        restore_top_after_loading();
        snprintf(status_text, sizeof(status_text), "Memoire insuffisante pour ouvrir ce dossier.");
        return false;
    }
    unsigned scanned_count = 0, scanned_images = 0;
    struct dirent *item;
    while ((item = readdir(directory)) != NULL && scanned_count < MAX_ENTRIES) {
        if (!poll_loading(loading_done + 1, 0)) break;
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        size_t name_length = strlen(item->d_name);
        if (name_length >= NAME_SIZE) continue;
        unsigned char type = item->d_type;
        bool image_file = has_image_extension(item->d_name);
        if (type != DT_DIR && type != DT_UNKNOWN &&
            (type != DT_REG || !image_file)) continue;
        char path[PATH_SIZE];
        join_path(path, sizeof(path), current_dir, item->d_name);
        if (!path[0]) continue;
        struct stat info;
        bool have_info = false;
        if (type == DT_UNKNOWN) {
            if (stat(path, &info) != 0) continue;
            have_info = true;
            type = S_ISDIR(info.st_mode) ? DT_DIR :
                   S_ISREG(info.st_mode) ? DT_REG : DT_UNKNOWN;
        }
        bool is_dir = type == DT_DIR;
        long long modified = 0;
        if (is_dir) {
            unsigned budget = 512;
            bool complete;
            if (!directory_contains_image(path, 6, &budget, &complete)) continue;
        } else {
            if (type != DT_REG || !image_file || !has_readable_image(path, item->d_name))
                continue;
            if (have_info || stat(path, &info) == 0) modified = (long long)info.st_mtime;
        }
        memcpy(scanned[scanned_count].name, item->d_name, name_length + 1);
        scanned[scanned_count].directory = is_dir;
        scanned[scanned_count].modified = modified;
        if (!is_dir) scanned_images++;
        scanned_count++;
    }
    closedir(directory);
    if (loading_cancelled || quit_requested) {
        free(scanned);
        restore_top_after_loading();
        snprintf(status_text, sizeof(status_text), "Analyse annulee. Liste precedente conservee.");
        return false;
    }
    if (scanned_count) memcpy(entries, scanned, scanned_count * sizeof(*scanned));
    free(scanned);
    entry_count = scanned_count;
    image_count = scanned_images;
    qsort(entries, entry_count, sizeof(entries[0]), compare_entries);
    if (selection >= entry_count) selection = entry_count ? entry_count - 1 : 0;
    restore_top_after_loading();
    memcpy(status_text, scan_status, sizeof(status_text));
    return true;
}

static bool is_sd_root(const char *path) {
    return !strcmp(path, "sdmc:/") || !strcmp(path, "sdmc:");
}

static BrowserSnapshot *find_browser_cache(const char *path) {
    if (is_sd_root(path)) return root_directory_cached ? &root_directory_cache : NULL;
    for (unsigned i = 0; i < directory_cache_count; i++) {
        if (!strcmp(directory_cache[i].directory, path)) return &directory_cache[i];
    }
    return NULL;
}

static void clear_browser_cache(void) {
    for (unsigned i = 0; i < directory_cache_count; i++) {
        free(directory_cache[i].entries);
        directory_cache[i].entries = NULL;
    }
    directory_cache_count = 0;
    root_directory_cached = false;
    directory_probe_count = 0;
    directory_cache_bytes = sizeof(directory_cache) + sizeof(root_directory_cache) +
                            sizeof(root_directory_entries);
}

static void save_browser_cache(void) {
    BrowserSnapshot *snapshot = find_browser_cache(current_dir);
    if (!snapshot) {
        if (is_sd_root(current_dir)) {
            snapshot = &root_directory_cache;
            snapshot->entries = root_directory_entries;
            root_directory_cached = true;
        } else {
            size_t bytes = entry_count * sizeof(entries[0]);
            if (directory_cache_count == DIRECTORY_CACHE_LIMIT ||
                bytes > DIRECTORY_CACHE_BUDGET - directory_cache_bytes) return;
            ImageEntry *saved = bytes ? malloc(bytes) : NULL;
            if (bytes && !saved) return;
            snapshot = &directory_cache[directory_cache_count++];
            snapshot->entries = saved;
            directory_cache_bytes += bytes;
        }
        if (entry_count)
            memcpy(snapshot->entries, entries, entry_count * sizeof(entries[0]));
        snapshot->entry_count = entry_count;
        snapshot->image_count = image_count;
        strcpy(snapshot->directory, current_dir);
    }
    // Cached listings never rescan during this session; retain their latest UI state.
    snapshot->selection = selection;
    strcpy(snapshot->image_path, current_path);
    memcpy(snapshot->status, status_text, sizeof(snapshot->status));
}

static bool restore_browser_cache(void) {
    BrowserSnapshot *snapshot = find_browser_cache(current_dir);
    if (!snapshot) return false;
    if (snapshot->entry_count)
        memcpy(entries, snapshot->entries, snapshot->entry_count * sizeof(entries[0]));
    entry_count = snapshot->entry_count;
    selection = snapshot->selection;
    image_count = snapshot->image_count;
    strcpy(current_path, snapshot->image_path);
    memcpy(status_text, snapshot->status, sizeof(status_text));
    return true;
}

static bool fit_to_box(unsigned width, unsigned height, unsigned max_width, unsigned max_height,
                       unsigned *fit_width, unsigned *fit_height) {
    if (!width || !height || width > MAX_DECODE_DIMENSION || height > MAX_DECODE_DIMENSION)
        return false;
    if ((u64)width * max_height > (u64)height * max_width) {
        *fit_width = max_width;
        *fit_height = (unsigned)((u64)height * max_width / width);
    } else {
        *fit_height = max_height;
        *fit_width = (unsigned)((u64)width * max_height / height);
    }
    if (!*fit_width) *fit_width = 1;
    if (!*fit_height) *fit_height = 1;
    return true;
}

static bool fit_dimensions(unsigned width, unsigned height,
                           unsigned *fit_width, unsigned *fit_height) {
    // Keep small files at their native resolution. Enlarging them here with
    // nearest-neighbour replication permanently bakes blocks into the zoom.
    if (width && height && width <= IMAGE_BUFFER_MAX_WIDTH &&
        height <= IMAGE_BUFFER_MAX_HEIGHT) {
        *fit_width = width;
        *fit_height = height;
        return true;
    }
    return fit_to_box(width, height, IMAGE_BUFFER_MAX_WIDTH, IMAGE_BUFFER_MAX_HEIGHT,
                      fit_width, fit_height);
}

static bool fit_display_dimensions(unsigned width, unsigned height,
                                   unsigned *fit_width, unsigned *fit_height) {
    return fit_to_box(width, height, IMAGE_MAX_WIDTH, TOP_HEIGHT, fit_width, fit_height);
}

static bool resampler_init(Resampler *scale, unsigned src_w, unsigned src_h,
                           unsigned dst_w, unsigned dst_h) {
    memset(scale, 0, sizeof(*scale));
    scale->src_w = src_w; scale->src_h = src_h;
    scale->dst_w = dst_w; scale->dst_h = dst_h;
    scale->output = (u8 *)malloc((size_t)dst_w * dst_h * 3);
    scale->row_sum = (u32 *)calloc((size_t)dst_w * 3, sizeof(u32));
    if (!scale->output || !scale->row_sum) {
        free(scale->output); free(scale->row_sum);
        memset(scale, 0, sizeof(*scale));
        return false;
    }
    return true;
}

static void resampler_store_row(Resampler *scale) {
    if (!scale->rows_in_band) return;
    u8 *dst = scale->output + (size_t)scale->active_y * scale->dst_w * 3;
    for (unsigned i = 0; i < scale->dst_w * 3; i++)
        dst[i] = (u8)(scale->row_sum[i] / scale->rows_in_band);
    memset(scale->row_sum, 0, (size_t)scale->dst_w * 3 * sizeof(u32));
    scale->rows_in_band = 0;
}

static void resampler_horizontal(const Resampler *scale, const u8 *src,
                                 unsigned channels, u8 *dst) {
    for (unsigned x = 0; x < scale->dst_w; x++) {
        if (scale->src_w >= scale->dst_w) {
            unsigned x0 = (unsigned)((u64)x * scale->src_w / scale->dst_w);
            unsigned x1 = (unsigned)((u64)(x + 1) * scale->src_w / scale->dst_w);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > scale->src_w) x1 = scale->src_w;
            unsigned count = x1 - x0;
            u32 sums[3] = {0, 0, 0};
            for (unsigned sx = x0; sx < x1; sx++) {
                const u8 *p = src + (size_t)sx * channels;
                unsigned alpha = channels == 4 ? p[3] : 255;
                for (unsigned c = 0; c < 3; c++) sums[c] += (p[c] * alpha) / 255;
            }
            for (unsigned c = 0; c < 3; c++) dst[x * 3 + c] = (u8)(sums[c] / count);
        } else {
            unsigned sx = (unsigned)((u64)x * scale->src_w / scale->dst_w);
            const u8 *p = src + (size_t)sx * channels;
            unsigned alpha = channels == 4 ? p[3] : 255;
            for (unsigned c = 0; c < 3; c++) dst[x * 3 + c] = (u8)((p[c] * alpha) / 255);
        }
    }
}

static void resampler_push_row(Resampler *scale, const u8 *src,
                               unsigned src_y, unsigned channels, u8 *temporary) {
    resampler_horizontal(scale, src, channels, temporary);
    if (scale->src_h >= scale->dst_h) {
        unsigned dst_y = (unsigned)((u64)src_y * scale->dst_h / scale->src_h);
        if (scale->rows_in_band && dst_y != scale->active_y) resampler_store_row(scale);
        scale->active_y = dst_y;
        for (unsigned i = 0; i < scale->dst_w * 3; i++) scale->row_sum[i] += temporary[i];
        scale->rows_in_band++;
        if (src_y + 1 == scale->src_h) resampler_store_row(scale);
    } else {
        unsigned first = (unsigned)((u64)src_y * scale->dst_h / scale->src_h);
        unsigned end = (unsigned)((u64)(src_y + 1) * scale->dst_h / scale->src_h);
        if (end <= first) end = first + 1;
        if (end > scale->dst_h) end = scale->dst_h;
        for (unsigned y = first; y < end; y++)
            memcpy(scale->output + (size_t)y * scale->dst_w * 3,
                   temporary, (size_t)scale->dst_w * 3);
    }
}

static void free_image_levels(void) {
    for (unsigned i = 0; i < 2; i++) {
        free(image_levels[i].pixels);
        memset(&image_levels[i], 0, sizeof(image_levels[i]));
    }
}

static void build_image_levels(void) {
    const u8 *source = current_image_pixels;
    unsigned width = current_image_w, height = current_image_h;
    for (unsigned level = 0; level < 2; level++) {
        if (width <= IMAGE_MAX_WIDTH && height <= TOP_HEIGHT) break;
        unsigned next_w = (width + 1) / 2, next_h = (height + 1) / 2;
        u8 *pixels = malloc((size_t)next_w * next_h * 3);
        if (!pixels) break; // Full-resolution rendering remains available.
        for (unsigned y = 0; y < next_h; y++) {
            unsigned y0 = y * 2, y1 = y0 + 1 < height ? y0 + 1 : y0;
            for (unsigned x = 0; x < next_w; x++) {
                unsigned x0 = x * 2, x1 = x0 + 1 < width ? x0 + 1 : x0;
                const u8 *a = source + ((size_t)y0 * width + x0) * 3;
                const u8 *b = source + ((size_t)y0 * width + x1) * 3;
                const u8 *c = source + ((size_t)y1 * width + x0) * 3;
                const u8 *d = source + ((size_t)y1 * width + x1) * 3;
                u8 *out = pixels + ((size_t)y * next_w + x) * 3;
                for (unsigned channel = 0; channel < 3; channel++)
                    out[channel] = (u8)((a[channel] + b[channel] +
                                        c[channel] + d[channel] + 2) / 4);
            }
        }
        image_levels[level] = (ImageLevel){ pixels, next_w, next_h };
        source = pixels;
        width = next_w;
        height = next_h;
    }
}

static bool resampler_show(Resampler *scale) {
    // Keep the previous image intact until the replacement renders correctly.
    bool preserve_view = preserve_image_view_on_decode;
    preserve_image_view_on_decode = false;
    u8 *old_pixels = current_image_pixels;
    ImageLevel old_levels[2];
    memcpy(old_levels, image_levels, sizeof(old_levels));
    unsigned old_w = current_image_w, old_h = current_image_h;
    unsigned old_zoom = zoom_percent, old_rotation = rotation_quadrants;
    int old_x = pan_offset_x, old_y = pan_offset_y;
    int old_rx = pan_remainder_x, old_ry = pan_remainder_y;
    memset(image_levels, 0, sizeof(image_levels));
    current_image_pixels = scale->output;
    scale->output = NULL;
    current_image_w = scale->dst_w;
    current_image_h = scale->dst_h;
    if (!preserve_view) {
        zoom_percent = 100;
        rotation_quadrants = 0;
        pan_offset_x = pan_offset_y = 0;
        pan_remainder_x = pan_remainder_y = 0;
    }
    build_image_levels();
    if (render_current_image()) {
        for (unsigned i = 0; i < 2; i++) free(old_levels[i].pixels);
        free(old_pixels);
        return true;
    }
    free_image_levels();
    free(current_image_pixels);
    current_image_pixels = old_pixels;
    memcpy(image_levels, old_levels, sizeof(image_levels));
    current_image_w = old_w; current_image_h = old_h;
    zoom_percent = old_zoom; rotation_quadrants = old_rotation;
    pan_offset_x = old_x; pan_offset_y = old_y;
    pan_remainder_x = old_rx; pan_remainder_y = old_ry;
    return false;
}

static void resampler_destroy(Resampler *scale) {
    free(scale->output);
    free(scale->row_sum);
    memset(scale, 0, sizeof(*scale));
}

static void clear_top(void) {
    u8 *framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (framebuffer) memset(framebuffer, 0, TOP_WIDTH * TOP_HEIGHT * 3);
    if (stereo_active) {
        framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
        if (framebuffer) memset(framebuffer, 0, TOP_WIDTH * TOP_HEIGHT * 3);
    }
    top_frame_dirty = true;
}

static size_t top_pixel_offset(unsigned x, unsigned y) {
    // The 3DS framebuffer stores columns from bottom to top. Screen x stays
    // left-to-right; reversing it as well mirrors photos and guide text.
    return ((size_t)x * TOP_HEIGHT + (TOP_HEIGHT - 1 - y)) * 3;
}

static bool update_stereo_state(void) {
    float slider = 0.0f;
    if (stereo_supported && viewing_image && !guide_active)
        slider = osGet3DSliderState();
    if (slider < 0.0f) slider = 0.0f;
    if (slider > 1.0f) slider = 1.0f;
    bool next_active = slider > (stereo_active ? 0.005f : 0.025f);
    float desired_shift = slider * MAX_STEREO_SHIFT;
    int next_shift = next_active ? (int)(desired_shift + 0.5f) : 0;
    // A little hysteresis avoids repeated redraws when the physical slider
    // rests near the threshold between two adjacent pixel depths.
    if (next_active && stereo_active &&
        desired_shift >= stereo_shift - 0.65f &&
        desired_shift <= stereo_shift + 0.65f)
        next_shift = stereo_shift;
    if (next_active == stereo_active && next_shift == stereo_shift) return false;
    if (next_active != stereo_active) gfxSet3D(next_active);
    stereo_active = next_active;
    stereo_shift = next_shift;
    return true;
}

static SampleAxis make_axis_sample(unsigned coordinate, unsigned dimension,
                                   unsigned stride) {
    unsigned limit = (dimension - 1) << 8;
    if (coordinate > limit) coordinate = limit;
    unsigned first = coordinate >> 8;
    unsigned second = first + 1 < dimension ? first + 1 : first;
    SampleAxis sample = { first * stride, second * stride, coordinate & 255 };
    return sample;
}

static inline u8 blend_channel(const u8 *a, const u8 *b, const u8 *c, const u8 *d,
                               unsigned channel, int horizontal, int vertical) {
    // Algebraically identical to the previous bilinear filter, with three
    // multiplies per channel instead of six. No intermediate rounding.
    int top = (int)a[channel] * 256 + ((int)b[channel] - a[channel]) * horizontal;
    int bottom = (int)c[channel] * 256 + ((int)d[channel] - c[channel]) * horizontal;
    return (u8)((top * 256 + (bottom - top) * vertical + 32768) >> 16);
}

static bool render_current_image(void) {
    u8 *framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    u8 *right_framebuffer = stereo_active ?
        gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL) : NULL;
    if (!framebuffer || !current_image_pixels || !current_image_w || !current_image_h)
        return false;
    if (stereo_active && !right_framebuffer) return false;
    clear_top();
    unsigned rotated_w = (rotation_quadrants & 1) ? current_image_h : current_image_w;
    unsigned rotated_h = (rotation_quadrants & 1) ? current_image_w : current_image_h;
    unsigned base_w, base_h;
    if (!fit_display_dimensions(rotated_w, rotated_h, &base_w, &base_h)) return false;
    shown_width = base_w;
    shown_height = base_h;
    unsigned draw_w = base_w * zoom_percent / 100;
    unsigned draw_h = base_h * zoom_percent / 100;
    int max_pan_x = draw_w > TOP_WIDTH ? ((int)draw_w - TOP_WIDTH) / 2 : 0;
    int max_pan_y = draw_h > TOP_HEIGHT ? ((int)draw_h - TOP_HEIGHT) / 2 : 0;
    if (pan_offset_x < -max_pan_x) pan_offset_x = -max_pan_x;
    if (pan_offset_x > max_pan_x) pan_offset_x = max_pan_x;
    if (pan_offset_y < -max_pan_y) pan_offset_y = -max_pan_y;
    if (pan_offset_y > max_pan_y) pan_offset_y = max_pan_y;
    int x0 = ((int)TOP_WIDTH - (int)draw_w) / 2 + pan_offset_x;
    int y0 = ((int)TOP_HEIGHT - (int)draw_h) / 2 + pan_offset_y;
    int depth = stereo_active ? stereo_shift : 0;
    int x_start = x0 < -depth ? -depth : x0;
    int y_start = y0 < 0 ? 0 : y0;
    int x_end = x0 + (int)draw_w > TOP_WIDTH + depth ?
        TOP_WIDTH + depth : x0 + (int)draw_w;
    int y_end = y0 + (int)draw_h > TOP_HEIGHT ? TOP_HEIGHT : y0 + (int)draw_h;
    RenderColumn columns[TOP_WIDTH + 2 * MAX_STEREO_SHIFT];
    SampleAxis rows[TOP_HEIGHT];
    unsigned row_offsets[TOP_HEIGHT];
    unsigned column_count = (unsigned)(x_end - x_start);
    unsigned row_count = (unsigned)(y_end - y_start);
    if (column_count > TOP_WIDTH + 2 * MAX_STEREO_SHIFT || row_count > TOP_HEIGHT)
        return false;
    bool swapped_axes = (rotation_quadrants & 1) != 0;
    const u8 *sample_pixels = current_image_pixels;
    unsigned sample_w = current_image_w, sample_h = current_image_h;
    // Filtered smaller levels prevent aliasing in the overview. Zoom always
    // selects a level with enough detail; the original buffer stays intact.
    for (unsigned level = 0; level < 2 && image_levels[level].pixels; level++) {
        unsigned width = swapped_axes ? image_levels[level].height : image_levels[level].width;
        unsigned height = swapped_axes ? image_levels[level].width : image_levels[level].height;
        if (width < draw_w || height < draw_h) break;
        sample_pixels = image_levels[level].pixels;
        sample_w = image_levels[level].width;
        sample_h = image_levels[level].height;
    }
    unsigned sampled_rotated_w = swapped_axes ? sample_h : sample_w;
    unsigned sampled_rotated_h = swapped_axes ? sample_w : sample_h;
    unsigned row_stride = sample_w * 3;
    unsigned column_dimension = swapped_axes ? sample_h : sample_w;
    unsigned row_dimension = swapped_axes ? sample_w : sample_h;
    unsigned column_stride = swapped_axes ? row_stride : 3;
    unsigned sample_row_stride = swapped_axes ? 3 : row_stride;

    // Compute geometry once per column/row, not once per pixel. In particular,
    // no 64-bit divides, coordinate clamps or rotation switches remain in the
    // pixel loop on the Old 3DS ARM11.
    for (unsigned i = 0; i < column_count; i++) {
        int x = x_start + (int)i;
        unsigned view = (unsigned)((u64)(x - x0) * 100 * 256 / zoom_percent);
        unsigned coordinate = (unsigned)((u64)view * sampled_rotated_w / base_w);
        unsigned limit = (column_dimension - 1) << 8;
        if (coordinate > limit) coordinate = limit;
        if (rotation_quadrants == 1 || rotation_quadrants == 2)
            coordinate = limit - coordinate;
        columns[i].sample = make_axis_sample(coordinate, column_dimension, column_stride);
        int left_x = x - depth, right_x = x + depth;
        columns[i].left_visible = left_x >= 0 && left_x < TOP_WIDTH;
        columns[i].right_visible = right_framebuffer && right_x >= 0 && right_x < TOP_WIDTH;
        columns[i].left_column = columns[i].left_visible ? (size_t)left_x * TOP_HEIGHT * 3 : 0;
        columns[i].right_column = columns[i].right_visible ? (size_t)right_x * TOP_HEIGHT * 3 : 0;
    }
    for (unsigned i = 0; i < row_count; i++) {
        int y = y_start + (int)i;
        unsigned view = (unsigned)((u64)(y - y0) * 100 * 256 / zoom_percent);
        unsigned coordinate = (unsigned)((u64)view * sampled_rotated_h / base_h);
        unsigned limit = (row_dimension - 1) << 8;
        if (coordinate > limit) coordinate = limit;
        if (rotation_quadrants == 2 || rotation_quadrants == 3)
            coordinate = limit - coordinate;
        rows[i] = make_axis_sample(coordinate, row_dimension, sample_row_stride);
        row_offsets[i] = (TOP_HEIGHT - 1 - (unsigned)y) * 3;
    }

    // Small tiles retain both decoded pixels and portrait framebuffer columns
    // in the CPU cache. The same filtered pixel is reused for both 3D eyes.
    for (unsigned tile_y = 0; tile_y < row_count; tile_y += 8) {
        unsigned tile_end_y = tile_y + 8 < row_count ? tile_y + 8 : row_count;
        for (unsigned tile_x = 0; tile_x < column_count; tile_x += 8) {
            unsigned tile_end_x = tile_x + 8 < column_count ? tile_x + 8 : column_count;
            for (unsigned x = tile_x; x < tile_end_x; x++) {
                const RenderColumn *column = &columns[x];
                u8 *left = column->left_visible ? framebuffer + column->left_column : NULL;
                u8 *right = column->right_visible ? right_framebuffer + column->right_column : NULL;
                for (unsigned y = tile_y; y < tile_end_y; y++) {
                    const SampleAxis *row = &rows[y];
                    const u8 *a = sample_pixels + row->first + column->sample.first;
                    const u8 *b = sample_pixels + row->first + column->sample.second;
                    const u8 *c = sample_pixels + row->second + column->sample.first;
                    const u8 *d = sample_pixels + row->second + column->sample.second;
                    int fx = (int)column->sample.fraction, fy = (int)row->fraction;
                    u8 red = blend_channel(a, b, c, d, 0, fx, fy);
                    u8 green = blend_channel(a, b, c, d, 1, fx, fy);
                    u8 blue = blend_channel(a, b, c, d, 2, fx, fy);
                    unsigned offset = row_offsets[y];
                    if (left) { left[offset] = blue; left[offset + 1] = green; left[offset + 2] = red; }
                    if (right) { right[offset] = blue; right[offset + 1] = green; right[offset + 2] = red; }
                }
            }
        }
    }
    snprintf(status_text, sizeof(status_text), "%u x %u affiche en %u x %u",
             image_width, image_height, shown_width, shown_height);
    return true;
}

static int circle_axis(int value) {
    if (value > 156) value = 156;
    if (value < -156) value = -156;
    return value > 30 ? value - 30 : value < -30 ? value + 30 : 0;
}

static bool move_image_view(circlePosition circle, unsigned elapsed_ms) {
    if (zoom_percent <= 100 || !current_image_pixels) {
        pan_remainder_x = pan_remainder_y = 0;
        return false;
    }
    unsigned width = (rotation_quadrants & 1) ? current_image_h : current_image_w;
    unsigned height = (rotation_quadrants & 1) ? current_image_w : current_image_h;
    unsigned base_w, base_h;
    if (!fit_display_dimensions(width, height, &base_w, &base_h)) return false;
    int draw_w = (int)(base_w * zoom_percent / 100);
    int draw_h = (int)(base_h * zoom_percent / 100);
    int max_x = draw_w > TOP_WIDTH ? (draw_w - TOP_WIDTH) / 2 : 0;
    int max_y = draw_h > TOP_HEIGHT ? (draw_h - TOP_HEIGHT) / 2 : 0;
    int old_x = pan_offset_x, old_y = pan_offset_y;
    int horizontal = circle_axis(circle.dx), vertical = circle_axis(circle.dy);
    if (!horizontal) pan_remainder_x = 0;
    if (!vertical) pan_remainder_y = 0;
    // Screen pixels per second, proportional to stick pressure. Downward
    // stick motion (negative dy) reveals the lower part of the photo.
    pan_remainder_x -= horizontal * (int)elapsed_ms * 220 / 126;
    pan_remainder_y += vertical * (int)elapsed_ms * 220 / 126;
    int step_x = pan_remainder_x / 1000, step_y = pan_remainder_y / 1000;
    pan_remainder_x -= step_x * 1000;
    pan_remainder_y -= step_y * 1000;
    pan_offset_x += step_x;
    pan_offset_y += step_y;
    if (pan_offset_x < -max_x) { pan_offset_x = -max_x; pan_remainder_x = 0; }
    if (pan_offset_x > max_x) { pan_offset_x = max_x; pan_remainder_x = 0; }
    if (pan_offset_y < -max_y) { pan_offset_y = -max_y; pan_remainder_y = 0; }
    if (pan_offset_y > max_y) { pan_offset_y = max_y; pan_remainder_y = 0; }
    return old_x != pan_offset_x || old_y != pan_offset_y;
}

static void deselect_image(void) {
    free_image_levels();
    free(current_image_pixels);
    current_image_pixels = NULL;
    current_image_w = current_image_h = 0;
    image_width = image_height = shown_width = shown_height = 0;
    current_path[0] = '\0';
    viewing_image = false;
    viewed_online_item_valid = false;
    memset(&viewed_online_item, 0, sizeof(viewed_online_item));
    zoom_percent = 100;
    rotation_quadrants = 0;
    pan_offset_x = pan_offset_y = 0;
    pan_remainder_x = pan_remainder_y = 0;
    snprintf(status_text, sizeof(status_text), active_tab == TAB_FAVORITES ?
             "Choisis un favori. A ouvre, Y retire." :
             active_tab == TAB_DISCOVER ? "Choisis une oeuvre. A ouvre, Y ajoute." :
             "Choisis un dossier ou une image.");
    clear_top();
}

static void put_pixel(u8 *framebuffer, int x, int y, u8 r, u8 g, u8 b) {
    if (x < 0 || x >= TOP_WIDTH || y < 0 || y >= TOP_HEIGHT) return;
    size_t at = top_pixel_offset((unsigned)x, (unsigned)y);
    framebuffer[at] = b;
    framebuffer[at + 1] = g;
    framebuffer[at + 2] = r;
}

static void fill_rect(u8 *framebuffer, int x, int y, int width, int height,
                      u8 r, u8 g, u8 b) {
    for (int py = y; py < y + height; py++)
        for (int px = x; px < x + width; px++) put_pixel(framebuffer, px, py, r, g, b);
}

static const u8 *font_glyph(char ch) {
    static const u8 letters[26][5] = {
        {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
        {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},
        {0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
        {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
        {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},
        {0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
        {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},
        {0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},
        {0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
        {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
        {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},
        {0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
        {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
    };
    static const u8 digits[10][5] = {
        {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
        {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
        {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
        {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E}
    };
    static const u8 colon[5] = {0,0x36,0x36,0,0};
    static const u8 plus[5] = {0x08,0x08,0x3E,0x08,0x08};
    static const u8 minus[5] = {0x08,0x08,0x08,0x08,0x08};
    static const u8 slash[5] = {0x20,0x10,0x08,0x04,0x02};
    static const u8 apostrophe[5] = {0,0x07,0x03,0,0};
    static const u8 less[5] = {0x08,0x14,0x22,0x41,0};
    static const u8 greater[5] = {0,0x41,0x22,0x14,0x08};
    static const u8 space[5] = {0,0,0,0,0};
    static const u8 period[5] = {0,0x60,0x60,0,0};
    static const u8 left_paren[5] = {0,0x1C,0x22,0x41,0};
    static const u8 right_paren[5] = {0,0x41,0x22,0x1C,0};
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if (ch >= 'A' && ch <= 'Z') return letters[(unsigned)(ch - 'A')];
    if (ch >= '0' && ch <= '9') return digits[(unsigned)(ch - '0')];
    if (ch == ':') return colon;
    if (ch == '+') return plus;
    if (ch == '-') return minus;
    if (ch == '/') return slash;
    if (ch == '\'') return apostrophe;
    if (ch == '<') return less;
    if (ch == '>') return greater;
    if (ch == '.') return period;
    if (ch == '(') return left_paren;
    if (ch == ')') return right_paren;
    return space;
}

static void draw_text(u8 *framebuffer, int x, int y, const char *text,
                      unsigned scale, u8 r, u8 g, u8 b) {
    for (; *text; text++, x += (int)(6 * scale)) {
        const u8 *glyph = font_glyph(*text);
        for (int col = 0; col < 5; col++) {
            for (int row = 0; row < 7; row++) {
                if (!(glyph[col] & (1u << row))) continue;
                fill_rect(framebuffer, x + col * (int)scale, y + row * (int)scale,
                          (int)scale, (int)scale, r, g, b);
            }
        }
    }
}

static void draw_first_run_guide(void) {
    u8 *framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!framebuffer) return;
    clear_top();
    fill_rect(framebuffer, 14, 18, 372, 204, 7, 17, 30);
    fill_rect(framebuffer, 26, 30, 34, 3, 50, 220, 255);
    static const char *titles[] = { "BIENVENUE", "VOS IMAGES", "DECOUVRIR" };
    draw_text(framebuffer, 26, 46, titles[guide_page], 2, 90, 235, 255);
    if (guide_page == 0) {
        draw_text(framebuffer, 26, 84, "Touchez un onglet en bas pour naviguer.", 1, 235, 241, 250);
        draw_text(framebuffer, 26, 106, "La croix choisit. A ouvre. B revient.", 1, 235, 241, 250);
        draw_text(framebuffer, 26, 128, "START quitte. SELECT rappelle ce guide.", 1, 235, 241, 250);
        draw_text(framebuffer, 26, 150, "Prenez le temps, vos choix sont conserves.", 1, 165, 180, 200);
    } else if (guide_page == 1) {
        draw_text(framebuffer, 26, 84, "Onglet SD : vos fichiers JPEG et PNG.", 1, 235, 241, 250);
        draw_text(framebuffer, 26, 106, "Maintiens L/R : zoom. X : tourner.", 1, 235, 241, 250);
        draw_text(framebuffer, 26, 128, "Haut / Bas : luminosite pendant la vue.", 1, 235, 241, 250);
        draw_text(framebuffer, 26, 150, "Curseur 3D : profondeur stereo de l'image.", 1, 90, 235, 255);
        draw_text(framebuffer, 26, 172, "B annule aussi un chargement en cours.", 1, 165, 180, 200);
    } else {
        draw_text(framebuffer, 26, 80, "Tu choisis les recherches et les tags saisis.", 1, 235, 241, 250);
        draw_text(framebuffer, 26, 98, "Le createur n'est pas responsable des recherches.", 1, 235, 241, 250);
        draw_text(framebuffer, 26, 116, "Les resultats sont fournis par DeviantArt.", 1, 235, 241, 250);
        draw_text(framebuffer, 26, 134, "A : masque les adultes signales par le site.", 1, 165, 180, 200);
        draw_text(framebuffer, 26, 152, "B : aucune exclusion adulte cote application.", 1, 165, 180, 200);
        draw_text(framebuffer, 26, 170, "Le site peut limiter ses propres resultats.", 1, 165, 180, 200);
    }
    char footer[48];
    snprintf(footer, sizeof(footer), "%u/3     A : %s", guide_page + 1,
             guide_page == 2 ? "J'ai compris" : "Suivant");
    draw_text(framebuffer, 26, 194, footer, 1, 90, 235, 255);
    if (guide_page) draw_text(framebuffer, 286, 194, "B : Retour", 1, 165, 180, 200);
}

static bool guide_was_seen(void) {
    FILE *file = fopen(GUIDE_FLAG_PATH, "rb");
    if (!file) return false;
    fclose(file);
    return true;
}

static void draw_loading_screen(void) {
    u8 *framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!framebuffer) return;
    clear_top();
    fill_rect(framebuffer, 18, 48, 364, 152, 7, 17, 30);
    draw_text(framebuffer, 34, 68, "CHARGEMENT", 2, 90, 235, 255);
    draw_text(framebuffer, 34, 102, loading_label, 1, 235, 241, 250);
    fill_rect(framebuffer, 34, 128, 332, 6, 25, 43, 63);
    char progress[56];
    if (loading_total) {
        unsigned percent = (unsigned)((u64)loading_done * 100 / loading_total);
        if (percent > 100) percent = 100;
        fill_rect(framebuffer, 34, 128, (int)(332 * percent / 100), 6, 50, 220, 255);
        snprintf(progress, sizeof(progress), "%u %%", percent);
    } else {
        int position = (int)((osGetTime() / 16) % 284);
        fill_rect(framebuffer, 34 + position, 128, 48, 6, 50, 220, 255);
        if (loading_done) snprintf(progress, sizeof(progress), "%u elements examines", loading_done);
        else snprintf(progress, sizeof(progress), "Veuillez patienter...");
    }
    draw_text(framebuffer, 34, 148, progress, 1, 235, 241, 250);
    draw_text(framebuffer, 34, 178, "B : Annuler     START : Quitter", 1, 165, 180, 200);
    if (stereo_active) {
        u8 *right = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, NULL, NULL);
        if (right) memcpy(right, framebuffer, TOP_WIDTH * TOP_HEIGHT * 3);
    }
    present_frame();
}

static void begin_loading(const char *label) {
    loading_label = label;
    loading_done = loading_total = 0;
    loading_cancelled = false;
    loading_last_poll = 0;
    loading_last_draw = osGetTime();
    draw_loading_screen();
}

static bool poll_loading(unsigned done, unsigned total) {
    loading_done = done;
    loading_total = total;
    if (quit_requested || loading_cancelled) return false;
    u64 now = osGetTime();
    if (loading_last_poll && now - loading_last_poll < 33) return true;
    loading_last_poll = now;
    hidScanInput();
    u32 down = hidKeysDown();
    if (down & KEY_B) loading_cancelled = true;
    if ((down & KEY_START) || !aptMainLoop()) quit_requested = true;
    if (quit_requested || loading_cancelled) return false;
    if (now - loading_last_draw >= 120) {
        loading_last_draw = now;
        draw_loading_screen();
    }
    return true;
}

static void restore_top_after_loading(void) {
    if (guide_active) draw_first_run_guide();
    else if (current_image_pixels) render_current_image();
    else draw_home_screen();
    present_frame();
}

static void remember_guide_seen(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/ShieldOnlineArt", 0777);
    FILE *file = fopen(GUIDE_FLAG_PATH, "wb");
    if (file) {
        fputc(1, file);
        fclose(file);
    }
}

static u32 favorites_checksum(const ArtPageItem *items, unsigned count) {
    const u8 *bytes = (const u8 *)items;
    size_t length = (size_t)count * sizeof(*items);
    u32 hash = 2166136261u;
    for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static bool favorite_item_valid(const ArtPageItem *item) {
    return item && memchr(item->id, 0, sizeof(item->id)) && item->id[0] &&
           memchr(item->title, 0, sizeof(item->title)) && item->title[0] &&
           memchr(item->artist, 0, sizeof(item->artist)) &&
           memchr(item->page_url, 0, sizeof(item->page_url)) &&
           memchr(item->preview_url, 0, sizeof(item->preview_url)) &&
           !strncmp(item->preview_url, "https://", 8);
}

static void load_favorites(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/ShieldOnlineArt", 0777);
    favorite_count = 0;
    FILE *file = fopen(FAVORITES_PATH, "rb");
    if (file) {
        FavoritesHeader header;
        bool valid = fread(&header, 1, sizeof(header), file) == sizeof(header) &&
            !memcmp(header.magic, "SHOAFAV1", 8) && header.version == 1 &&
            header.count <= FAVORITES_MAX;
        if (valid && header.count)
            valid = fread(favorite_items, sizeof(favorite_items[0]), header.count, file) == header.count;
        int trailing = valid ? fgetc(file) : 0;
        valid = valid && trailing == EOF && !ferror(file) &&
            header.checksum == favorites_checksum(favorite_items, header.count);
        if (valid) {
            for (unsigned i = 0; i < header.count; i++)
                if (!favorite_item_valid(&favorite_items[i])) { valid = false; break; }
        }
        fclose(file);
        if (valid) favorite_count = header.count;
        else memset(favorite_items, 0, sizeof(favorite_items));
    }
    favorites_loaded = true;
}

static bool save_favorites(void) {
    if (!favorites_loaded || favorite_count > FAVORITES_MAX) return false;
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/ShieldOnlineArt", 0777);
    FavoritesHeader header = {{0}, 1, favorite_count, 0};
    memcpy(header.magic, "SHOAFAV1", 8);
    header.checksum = favorites_checksum(favorite_items, favorite_count);
    FILE *file = fopen(FAVORITES_TEMP_PATH, "wb");
    if (!file) return false;
    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
        (!favorite_count || fwrite(favorite_items, sizeof(favorite_items[0]), favorite_count, file) == favorite_count);
    if (fclose(file) != 0) ok = false;
    if (!ok) { remove(FAVORITES_TEMP_PATH); return false; }
    remove(FAVORITES_PATH);
    if (rename(FAVORITES_TEMP_PATH, FAVORITES_PATH) != 0) {
        remove(FAVORITES_TEMP_PATH);
        return false;
    }
    return true;
}

static bool refresh_token_bytes_valid(const char *token, size_t length) {
    if (!token || !length || length >= OAUTH_REFRESH_TOKEN_CAPACITY) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)token[i];
        if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
}

static bool save_refresh_token_to_sd(const char *token) {
    if (!token) return false;
    size_t length = strnlen(token, sizeof(saved_refresh_token));
    if (!refresh_token_bytes_valid(token, length)) return false;
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/ShieldOnlineArt", 0777);
    FILE *file = fopen(AUTH_REFRESH_TEMP_PATH, "wb");
    if (!file) return false;
    bool ok = fwrite(token, 1, length, file) == length;
    if (fclose(file) != 0) ok = false;
    if (!ok) { remove(AUTH_REFRESH_TEMP_PATH); return false; }
    remove(AUTH_REFRESH_PATH);
    if (rename(AUTH_REFRESH_TEMP_PATH, AUTH_REFRESH_PATH) != 0) {
        remove(AUTH_REFRESH_TEMP_PATH);
        return false;
    }
    memcpy(saved_refresh_token, token, length);
    saved_refresh_token[length] = '\0';
    return true;
}

static void load_refresh_token_from_sd(void) {
    memset(saved_refresh_token, 0, sizeof(saved_refresh_token));
    FILE *file = fopen(AUTH_REFRESH_PATH, "rb");
    if (!file) return;
    size_t length = fread(saved_refresh_token, 1, sizeof(saved_refresh_token) - 1, file);
    int extra = fgetc(file);
    bool ok = !ferror(file) && extra == EOF &&
              refresh_token_bytes_valid(saved_refresh_token, length);
    fclose(file);
    if (!ok) {
        memset(saved_refresh_token, 0, sizeof(saved_refresh_token));
        remove(AUTH_REFRESH_PATH);
        return;
    }
    saved_refresh_token[length] = '\0';
}

static void forget_saved_login(void) {
    remove(AUTH_REFRESH_PATH);
    remove(AUTH_REFRESH_TEMP_PATH);
    memset(saved_refresh_token, 0, sizeof(saved_refresh_token));
    oauth_token_clear(&auth_token);
    auth_token_expires_at = 0;
}

static const char *available_refresh_token(void) {
    if (auth_token.refresh[0]) return auth_token.refresh;
    if (saved_refresh_token[0]) return saved_refresh_token;
    return NULL;
}

static int favorite_index(const char *id) {
    if (!id || !*id) return -1;
    for (unsigned i = 0; i < favorite_count; i++)
        if (!strcmp(favorite_items[i].id, id)) return (int)i;
    return -1;
}

static bool is_favorite(const char *id) { return favorite_index(id) >= 0; }

static void toggle_favorite(const ArtPageItem *item) {
    if (!favorite_item_valid(item)) {
        snprintf(status_text, sizeof(status_text), "Cette oeuvre ne peut pas etre gardee en favori.");
        return;
    }
    int found = favorite_index(item->id);
    if (found >= 0) {
        unsigned index = (unsigned)found;
        if (index + 1 < favorite_count)
            memmove(&favorite_items[index], &favorite_items[index + 1],
                    (favorite_count - index - 1) * sizeof(favorite_items[0]));
        favorite_count--;
        memset(&favorite_items[favorite_count], 0, sizeof(favorite_items[0]));
        if (active_tab == TAB_FAVORITES && online_selection >= favorite_count && online_selection)
            online_selection--;
        if (active_tab == TAB_FAVORITES && online_list_start > online_selection)
            online_list_start = online_selection;
        snprintf(status_text, sizeof(status_text), save_favorites() ?
                 "Favori retire de la liste." : "Favori retire, mais sauvegarde SD impossible.");
        return;
    }
    if (favorite_count >= FAVORITES_MAX) {
        snprintf(status_text, sizeof(status_text), "Liste pleine : maximum %u favoris.", FAVORITES_MAX);
        return;
    }
    favorite_items[favorite_count++] = *item;
    snprintf(status_text, sizeof(status_text), save_favorites() ?
             "Favori ajoute sur la carte SD." : "Favori ajoute en memoire, sauvegarde SD impossible.");
}

static unsigned visible_art_count(void) {
    if (active_tab == TAB_FAVORITES) return favorite_count;
    return online_page_valid ? online_page.count : 0;
}

static ArtPageItem *visible_art_item(unsigned index) {
    if (active_tab == TAB_FAVORITES)
        return index < favorite_count ? &favorite_items[index] : NULL;
    return online_page_valid && index < online_page.count ? &online_page.items[index] : NULL;
}

static void draw_home_screen(void) {
    clear_top();
    u8 *framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!framebuffer) return;
    fill_rect(framebuffer, 0, 0, TOP_WIDTH, TOP_HEIGHT, 3, 12, 22);
    fill_rect(framebuffer, 28, 48, 42, 4, 50, 220, 255);
    draw_text(framebuffer, 28, 70, "SHIELD", 3, 235, 241, 250);
    draw_text(framebuffer, 28, 102, "ONLINE ART", 2, 90, 235, 255);
    const char *line1 = "Des illustrations, a votre rythme.";
    const char *line2 = "Choisissez un onglet sur l'ecran du bas.";
    if (active_tab == TAB_LOCAL) {
        line1 = "Vos images sur la carte SD.";
        line2 = "Choisissez une image, puis appuyez sur A.";
    } else if (active_tab == TAB_FAVORITES) {
        line1 = "Retrouvez vos oeuvres preferees.";
        line2 = "Y ajoute ou retire une reference.";
    }
    draw_text(framebuffer, 28, 152, line1, 1, 235, 241, 250);
    draw_text(framebuffer, 28, 174, line2, 1, 165, 180, 200);
    draw_text(framebuffer, 28, 216, "VERSION EXPERIMENTALE - BY YANIS", 1, 90, 140, 165);
}

static void draw_auth_screen(void) {
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!fb) return;
    fill_rect(fb, 0, 0, TOP_WIDTH, TOP_HEIGHT, 255, 255, 255);
    auth_qr_draw_top(fb, TOP_WIDTH * TOP_HEIGHT * 3u, auth_qr_result);
    top_frame_dirty = true;
}

static bool launch_auth_worker(AuthJob job) {
    if (auth_thread_running || job == AUTH_JOB_NONE) return false;
    auth_job = job;
    auth_cancel_requested = false;
    auth_transfer.cancel = &auth_cancel_requested;
    auth_transfer.downloaded_bytes = 0;
    auth_transfer.total_bytes = 0;
    auth_progress_drawn_bytes = 0;
    auth_progress_last_draw = osGetTime();
    auth_response[0] = '\0';
    LightLock_Lock(&auth_result_lock);
    auth_thread_done = false;
    auth_thread_ok = false;
    auth_http_status = 0;
    LightLock_Unlock(&auth_result_lock);
    auth_thread = threadCreate(auth_token_worker, NULL, 64 * 1024, 0x30, -2, false);
    if (!auth_thread) {
        auth_job = AUTH_JOB_NONE;
        return false;
    }
    auth_thread_running = true;
    return true;
}

static void auth_token_worker(void *unused) {
    (void)unused;
    OAuthResponseBuffer response = {auth_response, sizeof(auth_response), 0, false};
    long status = 0;
    bool ok = false;
    if (auth_job == AUTH_JOB_TOKEN || auth_job == AUTH_JOB_REFRESH) {
        ok = oauth_post_token(auth_form, auth_ca_bundle, auth_ca_bundle_length,
                              &response, &status, &auth_transfer);
    } else if (auth_job == AUTH_JOB_TAG) {
        ok = oauth_get_tag(auth_tag_query, auth_token.access, auth_ca_bundle, auth_ca_bundle_length,
                           &response, &status, &auth_transfer);
    } else if ((auth_job == AUTH_JOB_PREVIEW || auth_job == AUTH_JOB_ZOOM_PREVIEW) &&
               auth_preview_bytes) {
        OAuthResponseBuffer preview = {(char *)auth_preview_bytes, ART_PREVIEW_MAX_BYTES + 1u, 0, false};
        ok = oauth_get_preview(auth_preview_url, auth_ca_bundle,
                               auth_ca_bundle_length, &preview, auth_preview_mime,
                               sizeof(auth_preview_mime), &auth_transfer);
        auth_preview_length = preview.used;
        response.overflow = preview.overflow;
    }
    LightLock_Lock(&auth_result_lock);
    auth_http_status = status;
    auth_thread_ok = ok;
    auth_thread_done = true;
    LightLock_Unlock(&auth_result_lock);
}

static bool load_auth_ca_bundle(void) {
    if (auth_ca_bundle) return true;
    FILE *file = fopen("romfs:/ca-bundle.pem", "rb");
    if (!file) return false;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return false; }
    long length = ftell(file);
    if (length <= 0 || length > 512 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); return false;
    }
    char *bundle = malloc((size_t)length + 1);
    if (!bundle) { fclose(file); return false; }
    size_t read = fread(bundle, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) { free(bundle); return false; }
    bundle[read] = 0;
    auth_ca_bundle = bundle;
    auth_ca_bundle_length = read;
    return true;
}

static void stop_auth_listener(void) {
    auth_listener_close(&auth_listener);
}

static void start_token_exchange(void) {
    if (!oauth_exchange_form(&auth_session, SHIELD_DEVIANTART_REDIRECT,
                             strlen(SHIELD_DEVIANTART_REDIRECT), auth_form, sizeof(auth_form))) {
        snprintf(status_text, sizeof(status_text), "La reponse de connexion est invalide.");
        auth_screen_active = false;
        oauth_session_clear(&auth_session);
        return;
    }
    stop_auth_listener();
    if (!launch_auth_worker(AUTH_JOB_TOKEN)) {
        oauth_pkce_clear(auth_form, sizeof(auth_form));
        snprintf(status_text, sizeof(status_text), "Memoire insuffisante pour la connexion.");
        auth_screen_active = false;
        oauth_session_clear(&auth_session);
        return;
    }
}

static void start_deviantart_login(void) {
    struct in_addr ip, mask, broadcast;
    if (auth_thread_running || auth_screen_active) return;
    if (auth_token.access[0] && osGetTime() < auth_token_expires_at) {
        start_tag_search();
        return;
    }
    if (auth_token.access[0] && !available_refresh_token()) oauth_token_clear(&auth_token);
    if (!auth_ps_ready) auth_ps_ready = R_SUCCEEDED(psInit());
    if (!auth_ps_ready) { snprintf(status_text, sizeof(status_text), "Connexion securisee indisponible."); return; }
    if (!auth_soc_ready) {
        if (!auth_soc_context) auth_soc_context = memalign(0x1000, 0x100000);
        if (!auth_soc_context || R_FAILED(socInit(auth_soc_context, 0x100000))) {
            snprintf(status_text, sizeof(status_text), "Reseau indisponible. Verifie le Wi-Fi."); return;
        }
        auth_soc_ready = true;
    }
    if (!auth_curl_ready) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            snprintf(status_text, sizeof(status_text), "Connexion securisee indisponible."); return;
        }
        auth_curl_ready = true;
    }
    if (!auth_romfs_ready) {
        if (R_FAILED(romfsInit())) {
            snprintf(status_text, sizeof(status_text), "Ressources integrees absentes du CIA."); return;
        }
        auth_romfs_ready = true;
    }
    if (!load_auth_ca_bundle()) {
        snprintf(status_text, sizeof(status_text), "Certificat HTTPS absent du CIA."); return;
    }
    if (R_FAILED(SOCU_GetIPInfo(&ip, &mask, &broadcast)) || !inet_ntop(AF_INET, &ip, auth_local_address, sizeof(auth_local_address))) {
        snprintf(status_text, sizeof(status_text), "Aucun reseau Wi-Fi actif."); return;
    }
    const char *refresh_token = available_refresh_token();
    if (refresh_token) {
        if (!oauth_refresh_form(refresh_token, auth_form, sizeof(auth_form))) {
            snprintf(status_text, sizeof(status_text), "Jeton SD invalide. Reconnecte-toi avec le QR.");
            return;
        }
        if (!launch_auth_worker(AUTH_JOB_REFRESH)) {
            oauth_pkce_clear(auth_form, sizeof(auth_form));
            snprintf(status_text, sizeof(status_text), "Impossible de reprendre la session.");
            return;
        }
        snprintf(status_text, sizeof(status_text), "Reprise de la session sauvegardee...");
        return;
    }
    memset(auth_entropy, 0, sizeof(auth_entropy));
    if (R_FAILED(PS_GenerateRandomBytes(auth_entropy, sizeof(auth_entropy))) ||
        !oauth_session_begin(&auth_session, auth_entropy, sizeof(auth_entropy),
                             SHIELD_DEVIANTART_REDIRECT, strlen(SHIELD_DEVIANTART_REDIRECT),
                             auth_url, sizeof(auth_url))) {
        oauth_pkce_clear(auth_entropy, sizeof(auth_entropy));
        snprintf(status_text, sizeof(status_text), "Impossible de preparer la connexion."); return;
    }
    oauth_pkce_clear(auth_entropy, sizeof(auth_entropy));
    if (!auth_listener_open(&auth_listener, ip.s_addr)) {
        oauth_session_clear(&auth_session);
        snprintf(status_text, sizeof(status_text), "Adresse locale indisponible. Reessaie."); return;
    }
    if (!auth_qr_encode(auth_url, strlen(auth_url), auth_qr_work, auth_qr_result)) {
        stop_auth_listener(); oauth_session_clear(&auth_session);
        snprintf(status_text, sizeof(status_text), "QR code trop long. Connexion annulee."); return;
    }
    auth_screen_active = true;
    snprintf(status_text, sizeof(status_text), "En attente du telephone. Scanne le QR.");
    draw_auth_screen();
}

static void start_tag_search(void) {
    if (auth_thread_running || auth_screen_active) return;
    if (!search_tag[0]) {
        snprintf(status_text, sizeof(status_text), "Entre un terme ou un tag avec X avant la recherche.");
        return;
    }
    if (!auth_token.access[0] || osGetTime() >= auth_token_expires_at) {
        online_page_valid = false;
        if (available_refresh_token()) {
            start_deviantart_login();
            return;
        }
        oauth_token_clear(&auth_token);
        snprintf(status_text, sizeof(status_text), "Connexion requise. Appuie sur A pour afficher le QR.");
        return;
    }
    if (!auth_soc_ready || !auth_curl_ready || !auth_ca_bundle) {
        snprintf(status_text, sizeof(status_text), "Session reseau expiree. Reconnecte-toi avec A.");
        oauth_token_clear(&auth_token);
        online_page_valid = false;
        return;
    }
    snprintf(auth_tag_query, sizeof(auth_tag_query), "%s", search_tag);
    online_page_valid = false;
    online_selection = online_list_start = 0;
    snprintf(status_text, sizeof(status_text), "Recherche du tag en cours...");
    if (!launch_auth_worker(AUTH_JOB_TAG))
        snprintf(status_text, sizeof(status_text), "Memoire insuffisante pour lancer la recherche.");
}

static void download_online_preview(unsigned index, bool zoom_variant) {
    if (auth_thread_running || auth_screen_active) return;
    const ArtPageItem *item = visible_art_item(index);
    if (!item) return;
    const char *url = item->preview_url;
    if (zoom_variant) {
        if (active_tab != TAB_DISCOVER || index >= online_page.count || !viewed_online_item_valid ||
            strcmp(viewed_online_item.id, item->id) != 0) return;
        if (zoom_loaded_item_id[0] && strcmp(zoom_loaded_item_id, item->id) == 0) return;
        if (zoom_attempted_item_id[0] && strcmp(zoom_attempted_item_id, item->id) == 0) return;
        snprintf(zoom_attempted_item_id, sizeof(zoom_attempted_item_id), "%s", item->id);
        url = online_page.zoom_urls[index];
        if (!url[0] || strcmp(url, item->preview_url) == 0) {
            snprintf(status_text, sizeof(status_text), "Pas de variante plus detaillee pour cette image.");
            return;
        }
    }
    if (!url[0]) {
        snprintf(status_text, sizeof(status_text), "Aucun apercu compatible pour cette oeuvre.");
        return;
    }
    if (!auth_soc_ready) {
        if (!auth_soc_context) auth_soc_context = memalign(0x1000, 0x100000);
        if (!auth_soc_context || R_FAILED(socInit(auth_soc_context, 0x100000))) {
            snprintf(status_text, sizeof(status_text), "Reseau indisponible. Verifie le Wi-Fi.");
            return;
        }
        auth_soc_ready = true;
    }
    if (!auth_curl_ready) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            snprintf(status_text, sizeof(status_text), "Connexion HTTPS indisponible.");
            return;
        }
        auth_curl_ready = true;
    }
    if (!auth_romfs_ready) {
        if (R_FAILED(romfsInit())) {
            snprintf(status_text, sizeof(status_text), "Ressources integrees absentes du CIA.");
            return;
        }
        auth_romfs_ready = true;
    }
    if (!load_auth_ca_bundle()) {
        snprintf(status_text, sizeof(status_text), "Certificat HTTPS absent du CIA.");
        return;
    }
    auth_preview_bytes = malloc(ART_PREVIEW_MAX_BYTES + 1u);
    if (!auth_preview_bytes) {
        snprintf(status_text, sizeof(status_text), "Memoire insuffisante pour l'apercu.");
        return;
    }
    auth_preview_length = 0;
    auth_preview_mime[0] = '\0';
    auth_preview_item_index = index;
    auth_preview_from_favorites = active_tab == TAB_FAVORITES;
    auth_preview_is_zoom = zoom_variant;
    if (!zoom_variant) {
        zoom_attempted_item_id[0] = '\0';
        zoom_loaded_item_id[0] = '\0';
    }
    snprintf(auth_preview_target_id, sizeof(auth_preview_target_id), "%s", item->id);
    snprintf(auth_preview_url, sizeof(auth_preview_url), "%s", url);
    snprintf(status_text, sizeof(status_text), zoom_variant ?
             "Chargement du detail haute resolution..." : "Chargement de l'apercu...");
    if (!launch_auth_worker(zoom_variant ? AUTH_JOB_ZOOM_PREVIEW : AUTH_JOB_PREVIEW)) {
        free(auth_preview_bytes);
        auth_preview_bytes = NULL;
        oauth_pkce_clear(auth_preview_url, sizeof(auth_preview_url));
        oauth_pkce_clear(auth_preview_target_id, sizeof(auth_preview_target_id));
        auth_preview_is_zoom = false;
        snprintf(status_text, sizeof(status_text), "Memoire insuffisante pour lancer l'apercu.");
    }
}

static bool api_error_description(const char *json, char *out, size_t capacity) {
    static const char key[] = "\"error_description\"";
    if (!json || !out || capacity < 2) return false;
    out[0] = '\0';
    const char *p = strstr(json, key);
    if (!p) return false;
    p += sizeof(key) - 1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return false;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != '"') return false;
    size_t used = 0;
    while (*p && *p != '"' && used + 1 < capacity) {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (!c) return false;
            if (c == 'u') {
                for (unsigned i = 0; i < 4; i++) if (!p[i]) return false;
                p += 4;
                c = '?';
            } else if (c == 'n' || c == 'r' || c == 't' || c == 'b' || c == 'f') {
                c = ' ';
            }
        }
        if (c < 0x20 || c >= 0x7f) c = '?';
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return used > 0;
}

static void finish_auth_thread(void) {
    if (!auth_thread_running) return;
    bool done, ok;
    LightLock_Lock(&auth_result_lock);
    done = auth_thread_done;
    ok = auth_thread_ok;
    LightLock_Unlock(&auth_result_lock);
    if (!done) return;
    threadJoin(auth_thread, U64_MAX);
    threadFree(auth_thread);
    auth_thread_running = false;
    AuthJob completed_job = auth_job;
    auth_job = AUTH_JOB_NONE;
    if (completed_job == AUTH_JOB_TOKEN) {
        oauth_pkce_clear(auth_form, sizeof(auth_form));
        if (!auth_cancel_requested && ok &&
            oauth_token_parse(auth_response, strlen(auth_response), &auth_token)) {
            auth_token_expires_at = osGetTime() + (u64)auth_token.expires_in * 1000;
            if (auth_token.refresh[0]) {
                save_login_prompt_pending = true;
                snprintf(status_text, sizeof(status_text), "Choisis si tu veux garder la connexion sur SD.");
                draw_save_login_prompt();
            } else {
                snprintf(status_text, sizeof(status_text), "Connexion validee, renouvellement non fourni.");
            }
        } else if (auth_cancel_requested) {
            oauth_token_clear(&auth_token);
            snprintf(status_text, sizeof(status_text), "Connexion annulee.");
        } else {
            oauth_token_clear(&auth_token);
            snprintf(status_text, sizeof(status_text), "Connexion refusee (%ld). Verifie la redirection.", auth_http_status);
        }
        oauth_session_clear(&auth_session);
        auth_screen_active = false;
        if (!save_login_prompt_pending) draw_home_screen();
        oauth_pkce_clear(auth_response, sizeof(auth_response));
        if (!auth_cancel_requested && !save_login_prompt_pending &&
            auth_token.access[0] && search_tag[0]) start_tag_search();
    } else if (completed_job == AUTH_JOB_REFRESH) {
        char old_refresh[OAUTH_REFRESH_TOKEN_CAPACITY] = {0};
        const char *old = available_refresh_token();
        if (old) snprintf(old_refresh, sizeof(old_refresh), "%s", old);
        bool was_saved = saved_refresh_token[0] != '\0';
        oauth_pkce_clear(auth_form, sizeof(auth_form));
        if (!auth_cancel_requested && ok &&
            oauth_token_parse(auth_response, strlen(auth_response), &auth_token)) {
            if (!auth_token.refresh[0])
                snprintf(auth_token.refresh, sizeof(auth_token.refresh), "%s", old_refresh);
            auth_token_expires_at = osGetTime() + (u64)auth_token.expires_in * 1000;
            bool persisted = true;
            if (was_saved && auth_token.refresh[0])
                persisted = save_refresh_token_to_sd(auth_token.refresh);
            snprintf(status_text, sizeof(status_text), persisted ?
                     "Session sauvegardee reprise." : "Session reprise ; mise a jour SD impossible.");
            oauth_pkce_clear(old_refresh, sizeof(old_refresh));
            oauth_pkce_clear(auth_response, sizeof(auth_response));
            if (search_tag[0]) start_tag_search();
            return;
        }
        if (auth_http_status == 400 || auth_http_status == 401) {
            forget_saved_login();
            snprintf(status_text, sizeof(status_text), "Session expiree. Reconnecte-toi avec le QR.");
        } else {
            if (!auth_token.refresh[0] && old_refresh[0])
                snprintf(auth_token.refresh, sizeof(auth_token.refresh), "%s", old_refresh);
            snprintf(status_text, sizeof(status_text), "Reprise impossible (%ld). Verifie le Wi-Fi puis reessaie.",
                     auth_http_status);
        }
        oauth_pkce_clear(old_refresh, sizeof(old_refresh));
        oauth_pkce_clear(auth_response, sizeof(auth_response));
    } else if (completed_job == AUTH_JOB_TAG) {
        if (auth_cancel_requested) {
            snprintf(status_text, sizeof(status_text), "Recherche annulee.");
        } else if (ok && art_parse_page(auth_response, strlen(auth_response),
                                        explicit_filter_enabled, &online_page) == ART_PAGE_OK) {
            online_page_valid = true;
            online_selection = online_list_start = 0;
            if (online_page.count)
                snprintf(status_text, sizeof(status_text), "%lu resultat(s) pour #%.55s",
                         (unsigned long)online_page.count, search_tag);
            else
                snprintf(status_text, sizeof(status_text), "Aucun resultat pour #%.55s", search_tag);
        } else {
            online_page_valid = false;
            if (auth_http_status == 429)
                snprintf(status_text, sizeof(status_text), "DeviantArt ralentit les recherches. Patiente puis reessaie.");
            else if (auth_http_status == 400) {
                char detail[72];
                if (api_error_description(auth_response, detail, sizeof(detail)))
                    snprintf(status_text, sizeof(status_text), "API 400 : %.75s", detail);
                else
                    snprintf(status_text, sizeof(status_text), "API 400 : requete invalide (detail absent).");
            }
            else if (auth_http_status == 401 || auth_http_status == 403)
                snprintf(status_text, sizeof(status_text), "Acces refuse (%ld). Reconnecte-toi.", auth_http_status);
            else
                snprintf(status_text, sizeof(status_text), "Recherche indisponible (%ld). Verifie le Wi-Fi.", auth_http_status);
        }
        oauth_pkce_clear(auth_response, sizeof(auth_response));
    } else if (completed_job == AUTH_JOB_PREVIEW || completed_job == AUTH_JOB_ZOOM_PREVIEW) {
        const ArtPageItem *item = auth_preview_from_favorites ?
            (auth_preview_item_index < favorite_count ? &favorite_items[auth_preview_item_index] : NULL) :
            (auth_preview_item_index < online_page.count ? &online_page.items[auth_preview_item_index] : NULL);
        bool zoom_job = completed_job == AUTH_JOB_ZOOM_PREVIEW;
        bool same_view = !zoom_job || (viewing_image && viewed_online_item_valid &&
            strcmp(viewed_online_item.id, auth_preview_target_id) == 0);
        if (auth_cancel_requested && same_view) {
            snprintf(status_text, sizeof(status_text), zoom_job ?
                     "Chargement detail annule." : "Chargement annule.");
        } else if (ok && auth_preview_bytes && item && same_view) {
            if (zoom_job) preserve_image_view_on_decode = true;
            else {
                zoom_attempted_item_id[0] = '\0';
                zoom_loaded_item_id[0] = '\0';
            }
            if (decode_online_preview(auth_preview_bytes, auth_preview_length, auth_preview_mime,
                                      item)) {
                if (zoom_job)
                    snprintf(zoom_loaded_item_id, sizeof(zoom_loaded_item_id), "%s", item->id);
                else online_selection = auth_preview_item_index;
            } else preserve_image_view_on_decode = false;
        } else if (same_view && !auth_cancel_requested) {
            snprintf(status_text, sizeof(status_text), zoom_job ?
                     "Detail indisponible (%ld). Image conservee." :
                     "Apercu indisponible (%ld). Reessaie plus tard.", auth_http_status);
        }
        free(auth_preview_bytes);
        auth_preview_bytes = NULL;
        auth_preview_length = 0;
        auth_preview_from_favorites = false;
        auth_preview_is_zoom = false;
        oauth_pkce_clear(auth_preview_url, sizeof(auth_preview_url));
        oauth_pkce_clear(auth_preview_target_id, sizeof(auth_preview_target_id));
        oauth_pkce_clear(auth_preview_mime, sizeof(auth_preview_mime));
    }
}

static void close_auth_services(void) {
    auth_cancel_requested = true;
    stop_auth_listener();
    if (auth_thread_running) {
        threadJoin(auth_thread, U64_MAX);
        threadFree(auth_thread);
        auth_thread_running = false;
    }
    oauth_pkce_clear(auth_form, sizeof(auth_form));
    oauth_pkce_clear(auth_response, sizeof(auth_response));
    oauth_pkce_clear(auth_url, sizeof(auth_url));
    oauth_pkce_clear(auth_entropy, sizeof(auth_entropy));
    oauth_pkce_clear(auth_tag_query, sizeof(auth_tag_query));
    oauth_pkce_clear(search_tag, sizeof(search_tag));
    oauth_pkce_clear(saved_refresh_token, sizeof(saved_refresh_token));
    oauth_token_clear(&auth_token);
    oauth_session_clear(&auth_session);
    oauth_pkce_clear(auth_qr_work, sizeof(auth_qr_work));
    oauth_pkce_clear(auth_qr_result, sizeof(auth_qr_result));
    if (auth_curl_ready) {
        oauth_transport_cleanup();
        curl_global_cleanup();
    }
    if (auth_romfs_ready) romfsExit();
    if (auth_soc_ready) socExit();
    if (auth_ps_ready) psExit();
    free(auth_ca_bundle);
    auth_ca_bundle = NULL;
    free(auth_preview_bytes);
    auth_preview_bytes = NULL;
    if (auth_soc_context) { free(auth_soc_context); auth_soc_context = NULL; }
}

static void draw_filter_choice(void) {
    clear_top();
    u8 *framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!framebuffer) return;
    fill_rect(framebuffer, 0, 0, TOP_WIDTH, TOP_HEIGHT, 3, 12, 22);
    draw_text(framebuffer, 26, 32, "BIENVENUE", 2, 90, 235, 255);
    draw_text(framebuffer, 26, 66, "Voulez-vous activer le filtrage ?", 1, 235, 241, 250);
    draw_text(framebuffer, 26, 92, "Il utilise le marquage de DeviantArt.", 1, 165, 180, 200);
    draw_text(framebuffer, 26, 110, "Une image mal classee peut passer.", 1, 165, 180, 200);
    draw_text(framebuffer, 26, 148, "A : Activer", 2, 90, 235, 255);
    draw_text(framebuffer, 26, 178, "B : Desactiver", 1, 235, 241, 250);
    draw_text(framebuffer, 26, 214, "Choix fixe jusqu'a la fermeture de l'app.", 1, 165, 180, 200);
}

static void draw_save_login_prompt(void) {
    u8 *framebuffer = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    if (!framebuffer) return;
    clear_top();
    fill_rect(framebuffer, 0, 0, TOP_WIDTH, TOP_HEIGHT, 3, 12, 22);
    fill_rect(framebuffer, 24, 28, 352, 4, 50, 220, 255);
    draw_text(framebuffer, 24, 48, "GARDER LA CONNEXION ?", 2, 90, 235, 255);
    draw_text(framebuffer, 24, 88, "Le jeton sera stocke sur la carte SD :", 1, 235, 241, 250);
    draw_text(framebuffer, 24, 108, "/3ds/ShieldOnlineArt/refresh.dat", 1, 90, 235, 255);
    draw_text(framebuffer, 24, 132, "Il permet l'acces a ton compte.", 1, 235, 241, 250);
    draw_text(framebuffer, 24, 150, "Ne partage pas le fichier ni la carte SD.", 1, 235, 170, 120);
    draw_text(framebuffer, 24, 168, "Tu es responsable de sa securite.", 1, 235, 170, 120);
    draw_text(framebuffer, 24, 202, "A : Enregistrer      B : Ne pas enregistrer", 1, 90, 235, 255);
    top_frame_dirty = true;
}

static void resolve_save_login_prompt(bool save) {
    save_login_prompt_pending = false;
    if (save) {
        if (save_refresh_token_to_sd(auth_token.refresh))
            snprintf(status_text, sizeof(status_text), "Session sauvegardee sur la SD. Y pour l'oublier.");
        else
            snprintf(status_text, sizeof(status_text), "Connexion active, mais sauvegarde SD impossible.");
    } else {
        snprintf(status_text, sizeof(status_text), "Connexion active pour cette session, non enregistree.");
    }
    draw_home_screen();
    if (search_tag[0]) start_tag_search();
}

static void select_session_filter(bool enabled) {
    if (!filter_choice_pending) return;
    explicit_filter_enabled = enabled;
    filter_choice_pending = false;
    filter_notice_until = osGetTime() + 3000;
    if (guide_active) draw_first_run_guide();
    else draw_home_screen();
}

static void change_brightness(int delta) {
    if (!lcd_ready) return;
    int next = brightness_level + delta;
    if (next < 1) next = 1;
    if (next > 5) next = 5;
    if (next == brightness_level) return;
    if (R_SUCCEEDED(GSPLCD_SetBrightness(GSPLCD_SCREEN_BOTH, (u32)next))) {
        brightness_level = next;
        brightness_changed = true;
    }
}

static void save_brightness_for_home(void) {
    if (!brightness_changed || !backlight_config_valid || !cfgu_service_ready) return;
    if (brightness_level == brightness_on_launch) return;
    backlight_config[1] = (u8)brightness_level;
    if (R_SUCCEEDED(CFG_SetConfigInfoBlk4(sizeof(backlight_config), 0x00050001,
                                          backlight_config)))
        (void)CFG_UpdateConfigSavegame();
}

static bool decode_jpeg(FILE *file, const u8 *memory, size_t memory_length) {
    struct jpeg_decompress_struct *decoder = (struct jpeg_decompress_struct *)calloc(1, sizeof(*decoder));
    JpegError *error = (JpegError *)calloc(1, sizeof(*error));
    Resampler *scale = (Resampler *)calloc(1, sizeof(*scale));
    u8 *volatile temporary = NULL;
    volatile bool created = false;
    volatile bool success = false;
    if (!decoder || !error || !scale) {
        snprintf(decode_error_text, sizeof(decode_error_text), "Pas assez de memoire pour ouvrir cette image.");
        goto cleanup;
    }

    decoder->err = jpeg_std_error(&error->base);
    error->base.error_exit = jpeg_fail;
    error->base.output_message = jpeg_quiet;
    if (setjmp(error->jump)) goto cleanup;

    jpeg_create_decompress(decoder);
    created = true;
    error->progress.progress_monitor = jpeg_progress;
    decoder->progress = &error->progress;
    if (memory) {
        if (!memory_length || memory_length > ULONG_MAX) goto cleanup;
        jpeg_mem_src(decoder, (unsigned char *)memory, (unsigned long)memory_length);
    } else {
        if (!file) goto cleanup;
        jpeg_stdio_src(decoder, file);
    }
    if (jpeg_read_header(decoder, TRUE) != JPEG_HEADER_OK) goto cleanup;
    u64 pixels = (u64)decoder->image_width * decoder->image_height;
    if (pixels > 32U * 1024U * 1024U ||
        (decoder->progressive_mode && pixels > 4U * 1024U * 1024U)) {
        snprintf(decode_error_text, sizeof(decode_error_text), "Cette image est trop grande. Essaie un apercu reduit.");
        goto cleanup;
    }
    if (!fit_dimensions(decoder->image_width, decoder->image_height, &shown_width, &shown_height))
        goto cleanup;
    image_width = decoder->image_width;
    image_height = decoder->image_height;

    decoder->out_color_space = JCS_RGB;
    decoder->scale_denom = 1;
    while (decoder->scale_denom < 8 &&
           (decoder->image_width / decoder->scale_denom > IMAGE_BUFFER_MAX_WIDTH * 2 ||
            decoder->image_height / decoder->scale_denom > IMAGE_BUFFER_MAX_HEIGHT * 2))
        decoder->scale_denom *= 2;
    jpeg_calc_output_dimensions(decoder);
    if (!jpeg_start_decompress(decoder) || decoder->output_components != 3 ||
        !decoder->output_width || !decoder->output_height) goto cleanup;

    unsigned width = decoder->output_width, height = decoder->output_height;
    unsigned fit_width, fit_height;
    if (!fit_dimensions(width, height, &fit_width, &fit_height)) goto cleanup;
    shown_width = fit_width;
    shown_height = fit_height;
    if (!resampler_init(scale, width, height, fit_width, fit_height)) goto cleanup;
    temporary = (u8 *)malloc((size_t)fit_width * 3);
    JSAMPARRAY row = (*decoder->mem->alloc_sarray)((j_common_ptr)decoder, JPOOL_IMAGE,
                                                   width * 3, 1);
    if (!temporary || !row) goto cleanup;

    while (decoder->output_scanline < decoder->output_height) {
        unsigned src_y = decoder->output_scanline;
        if (!poll_loading(src_y, height)) goto cleanup;
        if (jpeg_read_scanlines(decoder, row, 1) != 1) goto cleanup;
        resampler_push_row(scale, row[0], src_y, 3, (u8 *)temporary);
    }
    if (!jpeg_finish_decompress(decoder)) goto cleanup;
    if (!poll_loading(height, height)) goto cleanup;
    success = resampler_show(scale);

cleanup:
    if (created) jpeg_destroy_decompress(decoder);
    if (scale) { resampler_destroy(scale); free(scale); }
    free((void *)temporary);
    free(error);
    free(decoder);
    return (bool)success;
}

static bool decode_png(FILE *file, const u8 *memory, size_t memory_length) {
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_fail, png_quiet);
    if (!png) return false;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return false; }

    u8 *volatile row = NULL;
    u8 *volatile temporary = NULL;
    Resampler *scale = (Resampler *)calloc(1, sizeof(*scale));
    PngInput input = {file, memory, memory_length, 0};
    if (setjmp(png_jmpbuf(png))) {
        free((void *)row);
        free((void *)temporary);
        if (scale) { resampler_destroy(scale); free(scale); }
        png_destroy_read_struct(&png, &info, NULL);
        return false;
    }
    if (!scale) png_error(png, "Out of memory");

    png_set_user_limits(png, MAX_DECODE_DIMENSION, MAX_DECODE_DIMENSION);
    png_set_chunk_malloc_max(png, 256U * 1024U);
    png_set_chunk_cache_max(png, 16);
    png_set_read_fn(png, &input, png_file_read);
    png_read_info(png, info);
    png_uint_32 width = png_get_image_width(png, info);
    png_uint_32 height = png_get_image_height(png, info);
    if ((u64)width * height > 32U * 1024U * 1024U) {
        snprintf(decode_error_text, sizeof(decode_error_text), "Cette image est trop grande. Essaie un apercu reduit.");
        png_error(png, "Image too large");
    }
    if (!fit_dimensions(width, height, &shown_width, &shown_height) ||
        png_get_interlace_type(png, info) != PNG_INTERLACE_NONE)
        png_error(png, "Unsupported PNG dimensions or interlace");
    image_width = width;
    image_height = height;

    int color = png_get_color_type(png, info);
    int depth = png_get_bit_depth(png, info);
    if (depth == 16) png_set_strip_16(png);
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    if (!(color & PNG_COLOR_MASK_ALPHA) && !png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    png_size_t rowbytes = png_get_rowbytes(png, info);
    if (png_get_channels(png, info) != 4 || rowbytes < width * 4)
        png_error(png, "Unsupported PNG layout");
    unsigned fit_width, fit_height;
    if (!fit_dimensions(width, height, &fit_width, &fit_height)) png_error(png, "Bad dimensions");
    shown_width = fit_width;
    shown_height = fit_height;
    row = (u8 *)malloc(rowbytes);
    temporary = (u8 *)malloc((size_t)fit_width * 3);
    if (!row || !temporary || !resampler_init(scale, width, height, fit_width, fit_height))
        png_error(png, "Out of memory");

    for (unsigned src_y = 0; src_y < height; src_y++) {
        if (!poll_loading(src_y, height)) png_error(png, "Cancelled");
        png_read_row(png, (png_bytep)row, NULL);
        resampler_push_row(scale, (const u8 *)row, src_y, 4, (u8 *)temporary);
    }
    png_read_end(png, info);
    if (!poll_loading(height, height)) png_error(png, "Cancelled");
    bool success = resampler_show(scale);
    free((void *)row);
    free((void *)temporary);
    resampler_destroy(scale);
    free(scale);
    png_destroy_read_struct(&png, &info, NULL);
    return success;
}

static bool open_image(const char *name) {
    char image_path[PATH_SIZE];
    join_path(image_path, sizeof(image_path), current_dir, name);
    if (!image_path[0]) {
        snprintf(status_text, sizeof(status_text), "Chemin trop long.");
        return false;
    }
    begin_loading("Ouverture de l'image");
    snprintf(decode_error_text, sizeof(decode_error_text), "Cette image semble abimee ou incompatible.");
    FILE *file = fopen(image_path, "rb");
    if (!file) {
        restore_top_after_loading();
        snprintf(status_text, sizeof(status_text), "Fichier inaccessible.");
        return false;
    }
    ImageFormat format = probe_image_format(file);
    if (format == IMAGE_FORMAT_PNG_INTERLACED) {
        fclose(file);
        restore_top_after_loading();
        snprintf(status_text, sizeof(status_text), "PNG entrelace non pris en charge (RAM limitee).");
        return false;
    }
    if (format == IMAGE_FORMAT_UNSUPPORTED) {
        fclose(file);
        restore_top_after_loading();
        snprintf(status_text, sizeof(status_text), "Signature inconnue : JPEG ou PNG requis.");
        return false;
    }
    unsigned previous_width = image_width, previous_height = image_height;
    bool success = format == IMAGE_FORMAT_PNG ? decode_png(file, NULL, 0) : decode_jpeg(file, NULL, 0);
    fclose(file);
    if (!success) {
        image_width = previous_width;
        image_height = previous_height;
        restore_top_after_loading();
        snprintf(status_text, sizeof(status_text), "%s", loading_cancelled ?
                 "Ouverture annulee. L'image precedente est conservee." : decode_error_text);
        return false;
    }
    strcpy(current_path, image_path);
    viewing_image = true;
    snprintf(status_text, sizeof(status_text), "%u x %u affiche en %u x %u",
             image_width, image_height, shown_width, shown_height);
    return true;
}

static bool decode_online_preview(const u8 *bytes, size_t length, const char *mime,
                                  const ArtPageItem *item) {
    ArtPreviewInfo info;
    ArtPreviewStatus checked = art_check_preview(bytes, length, mime, mime ? strlen(mime) : 0, &info);
    if (checked != ART_PREVIEW_OK) {
        snprintf(status_text, sizeof(status_text), "Apercu refuse : format ou taille non pris en charge.");
        return false;
    }
    begin_loading("Decodage de l'apercu");
    snprintf(decode_error_text, sizeof(decode_error_text), "Apercu illisible ou incomplet.");
    unsigned previous_width = image_width, previous_height = image_height;
    bool success = info.type == ART_IMAGE_JPEG ? decode_jpeg(NULL, bytes, length) :
                   info.type == ART_IMAGE_PNG ? decode_png(NULL, bytes, length) : false;
    if (!success) {
        image_width = previous_width;
        image_height = previous_height;
        restore_top_after_loading();
        snprintf(status_text, sizeof(status_text), "%s", loading_cancelled ?
                 "Decodage annule. L'image precedente est conservee." : decode_error_text);
        return false;
    }
    snprintf(current_path, sizeof(current_path), "DeviantArt: %.100s", item->title);
    viewing_image = true;
    viewed_online_item = *item;
    viewed_online_item_valid = true;
    if (!auth_preview_is_zoom) viewed_online_item_index = auth_preview_item_index;
    snprintf(status_text, sizeof(status_text), "%ux%u affiche en %u x %u",
             image_width, image_height, shown_width, shown_height);
    return true;
}

static void go_parent(void) {
    if (is_sd_root(current_dir)) return;
    save_browser_cache();
    char previous_dir[PATH_SIZE];
    strcpy(previous_dir, current_dir);
    unsigned previous_selection = selection;
    size_t len = strlen(current_dir);
    while (len > 0 && current_dir[len - 1] == '/') current_dir[--len] = '\0';
    char *slash = strrchr(current_dir, '/');
    if (slash && slash >= current_dir + 5) slash[1] = '\0';
    else strcpy(current_dir, "sdmc:/");
    if (restore_browser_cache()) return;
    selection = 0;
    if (scan_directory()) save_browser_cache();
    else { strcpy(current_dir, previous_dir); selection = previous_selection; }
}

static bool update_battery_status(u64 now) {
    if (battery_last_update && now - battery_last_update < 20000) return false;
    battery_last_update = now;
    int old_percent = battery_percent, old_temperature = battery_temperature_c;
    u8 readings[2];
    // MCU 0x0A is temperature, 0x0B is whole battery percentage. One short
    // register read every 20 seconds, independent of SD browsing and rendering.
    if (battery_service_ready &&
        R_SUCCEEDED(MCUHWC_ReadRegister(0x0A, readings, sizeof(readings)))) {
        battery_temperature_c = readings[0] <= 100 ? readings[0] : -1;
        battery_percent = readings[1] <= 100 ? (readings[1] / 5) * 5 : -1;
    } else {
        battery_percent = battery_temperature_c = -1;
    }
    return old_percent != battery_percent || old_temperature != battery_temperature_c;
}

#define UI_RGB(r,g,b) ((u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define UI_BG UI_RGB(3,12,22)
#define UI_PANEL UI_RGB(15,30,46)
#define UI_SELECTED UI_RGB(15,65,82)
#define UI_TEXT UI_RGB(232,240,248)
#define UI_MUTED UI_RGB(155,177,193)
#define UI_ACCENT UI_RGB(70,220,240)

static void lower_rect(int x, int y, int width, int height, u16 color) {
    u16 *framebuffer = bottom_console.frameBuffer;
    if (!framebuffer) return;
    int end_x = x + width, end_y = y + height;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (end_x > 320) end_x = 320;
    if (end_y > 240) end_y = 240;
    for (int px = x; px < end_x; px++)
        for (int py = y; py < end_y; py++)
            framebuffer[px * 240 + 239 - py] = color;
}

static void lower_button(int x, int y, int width, int height, u16 color) {
    static const int inset[4] = {4, 2, 1, 1};
    for (int row = 0; row < height; row++) {
        int edge = row < height - row - 1 ? row : height - row - 1;
        int cut = edge < 4 ? inset[edge] : 0;
        lower_rect(x + cut, y + row, width - 2 * cut, 1, color);
    }
}

static void ui_text(int column, int row, const char *text, size_t limit, u16 fg, u16 bg) {
    if (column < 0 || column >= 40 || row < 0 || row >= 30) return;
    consoleSelect(&bottom_console);
    bottom_console.fg = fg;
    bottom_console.bg = bg;
    // libctru treats fg/bg as palette indexes unless these flags are set.
    bottom_console.flags = CONSOLE_FG_CUSTOM | CONSOLE_BG_CUSTOM;
    // libctru's console cursor is one-based, while our layout uses zero-based cells.
    bottom_console.cursorX = column + 1;
    bottom_console.cursorY = row + 1;
    if (limit > (size_t)(39 - column)) limit = (size_t)(39 - column);
    print_safe_name(text, limit);
}

static void ui_status(int row, const char *text) {
    for (unsigned line = 0; line < 2 && *text; line++) {
        char part[40];
        size_t remaining = strlen(text), count = remaining < 36 ? remaining : 36;
        if (!line && remaining > count) {
            size_t boundary = count;
            while (boundary > 12 && text[boundary] != ' ') boundary--;
            if (text[boundary] == ' ') count = boundary;
        }
        memcpy(part, text, count);
        part[count] = '\0';
        if (line && remaining > count && count >= 3) memset(part + count - 3, '.', 3);
        ui_text(2, row + (int)line, part, 36, UI_MUTED, UI_BG);
        text += count;
        while (*text == ' ') text++;
    }
}

static unsigned local_list_start(void) {
    return (selection / LOCAL_VISIBLE_ROWS) * LOCAL_VISIBLE_ROWS;
}

static void draw_battery_footer(void) {
    char line[48], battery[16], temperature[16];
    if (battery_percent >= 0) snprintf(battery, sizeof(battery), "%d%%", battery_percent);
    else strcpy(battery, "--%");
    if (battery_temperature_c >= 0) snprintf(temperature, sizeof(temperature), "%d C", battery_temperature_c);
    else strcpy(temperature, "-- C");
    snprintf(line, sizeof(line), "Batterie %s  |  %s", battery, temperature);
    lower_rect(0, 232, 320, 8, UI_BG);
    ui_text(2, 29, line, 36, UI_MUTED, UI_BG);
    bottom_frame_dirty = true;
}

static void draw_auth_ui(void) {
    ui_text(2, 0, "Connexion DeviantArt", 35, UI_TEXT, UI_BG);
    lower_button(8, 44, 304, 48, UI_PANEL);
    ui_text(2, 6, "1. Scanne le QR avec ton telephone", 36, UI_ACCENT, UI_PANEL);
    ui_text(2, 8, "2. Autorise SHIELD Online Art", 36, UI_TEXT, UI_PANEL);
    ui_text(2, 10, "3. Entre l'adresse ci-dessous", 36, UI_TEXT, UI_PANEL);
    lower_button(8, 104, 304, 24, UI_SELECTED);
    ui_text(2, 14, auth_local_address, 36, UI_ACCENT, UI_SELECTED);
    ui_text(2, 16, "Port 38741 automatique", 36, UI_MUTED, UI_BG);
    ui_status(17, auth_thread_running ? "Finalisation de la connexion..." : status_text);
    ui_text(2, 25, "HOME desactive ; START quitte", 36, UI_MUTED, UI_BG);
    lower_button(8, 212, 148, 20, UI_PANEL);
    ui_text(4, 27, "B : Annuler", 36, UI_MUTED, UI_PANEL);
    draw_battery_footer();
}

static void draw_ui(void) {
    bottom_frame_dirty = true;
    consoleSelect(&bottom_console);
    // Fill the RGB565 framebuffer directly: consoleClear draws every glyph.
    lower_rect(0, 0, 320, 240, UI_BG);
    char text[128];
    ui_text(2, 0, "SHIELD Online Art", 35, UI_TEXT, UI_BG);
    if (filter_choice_pending) {
        lower_button(8, 56, 304, 112, UI_PANEL);
        ui_text(3, 9, "Un choix pour cette session", 33, UI_TEXT, UI_PANEL);
        ui_text(3, 12, "A  Activer le filtrage", 33, UI_ACCENT, UI_PANEL);
        ui_text(3, 15, "B  Desactiver", 33, UI_MUTED, UI_PANEL);
        ui_text(2, 24, "Le choix revient a chaque ouverture.", 36, UI_MUTED, UI_BG);
        ui_text(2, 27, "START : quitter", 36, UI_MUTED, UI_BG);
        draw_battery_footer();
        return;
    }
    if (auth_screen_active) {
        draw_auth_ui();
        return;
    }
    if (save_login_prompt_pending) {
        lower_button(8, 32, 304, 160, UI_PANEL);
        ui_text(2, 5, "Sauvegarder le jeton sur la SD ?", 36, UI_ACCENT, UI_PANEL);
        ui_text(2, 8, "Il permet l'acces au compte.", 36, UI_TEXT, UI_PANEL);
        ui_text(2, 10, "Ne partage pas la carte ou le fichier.", 36, UI_RGB(255,190,130), UI_PANEL);
        ui_text(2, 12, "Tu es responsable de sa securite.", 36, UI_RGB(255,190,130), UI_PANEL);
        ui_text(2, 16, "A : oui     B : non", 36, UI_ACCENT, UI_PANEL);
        ui_text(2, 19, "Le fichier peut etre efface avec Y ensuite.", 36, UI_MUTED, UI_BG);
        ui_text(2, 22, "START : quitter sans enregistrer", 36, UI_MUTED, UI_BG);
        draw_battery_footer();
        return;
    }
    lower_button(8, 16, 304, 24, UI_PANEL);
    if (search_tag[0]) snprintf(text, sizeof(text), "Recherche : %s", search_tag);
    else strcpy(text, "Tag DeviantArt...   X");
    ui_text(2, 3, text, 36, UI_ACCENT, UI_PANEL);

    static const char *tabs[] = { "Decouvrir", "SD locale", "Favoris" };
    for (unsigned i = 0; i < TAB_COUNT; i++) {
        int x = 8 + (int)i * 104;
        u16 bg = active_tab == (AppTab)i ? UI_SELECTED : UI_PANEL;
        lower_button(x, 44, 96, 20, bg);
        ui_text(x / 8 + 1, 6, tabs[i], 10, active_tab == (AppTab)i ? UI_ACCENT : UI_MUTED, bg);
    }

    if (viewing_image) {
        ui_text(2, 9, "Votre image", 36, UI_ACCENT, UI_BG);
        const char *name = strrchr(current_path, '/');
        ui_text(2, 11, name ? name + 1 : current_path, 36, UI_TEXT, UI_BG);
        snprintf(text, sizeof(text), "Zoom %u%%  |  Luminosite %d/5", zoom_percent, brightness_level);
        ui_text(2, 13, text, 36, UI_MUTED, UI_BG);
        ui_text(2, 15, "L/R zooment. X tourne l'image.", 36, UI_TEXT, UI_BG);
        ui_text(2, 17, "Haut/Bas reglent la luminosite.", 36, UI_MUTED, UI_BG);
        if (viewed_online_item_valid)
            ui_text(2, 18, is_favorite(viewed_online_item.id) ? "Y : retirer des favoris" : "Y : ajouter aux favoris",
                    36, UI_ACCENT, UI_BG);
        ui_status(19, status_text);
    } else if (active_tab == TAB_LOCAL) {
        ui_text(2, 8, current_dir, 36, UI_MUTED, UI_BG);
        ui_status(9, status_text);
        if (!entry_count) {
            ui_text(2, 12, "Aucune image dans cette liste.", 36, UI_TEXT, UI_BG);
            ui_text(2, 14, "Formats : JPEG et PNG.", 36, UI_MUTED, UI_BG);
        } else {
            unsigned start = local_list_start();
            for (unsigned row = 0; row < LOCAL_VISIBLE_ROWS && start + row < entry_count; row++) {
                unsigned index = start + row;
                u16 bg = index == selection ? UI_SELECTED : UI_BG;
                lower_button(8, LOCAL_LIST_Y + (int)row * LOCAL_ROW_HEIGHT, 304, LOCAL_ROW_HEIGHT, bg);
                snprintf(text, sizeof(text), "%c %.100s", entries[index].directory ? '+' : ' ', entries[index].name);
                ui_text(2, LOCAL_LIST_Y / 8 + (int)row * 2, text, 36,
                        entries[index].directory ? UI_ACCENT : UI_TEXT, bg);
            }
        }
    } else if (active_tab == TAB_DISCOVER || active_tab == TAB_FAVORITES) {
        unsigned item_count = visible_art_count();
        if (item_count) {
            unsigned start = online_list_start;
            for (unsigned row = 0; row < ONLINE_VISIBLE_ROWS && start + row < item_count; row++) {
                unsigned index = start + row;
                const ArtPageItem *item = visible_art_item(index);
                if (!item) continue;
                u16 bg = index == online_selection ? UI_SELECTED : UI_BG;
                lower_button(8, ONLINE_LIST_Y + (int)row * ONLINE_ROW_HEIGHT, 304, ONLINE_ROW_HEIGHT, bg);
                snprintf(text, sizeof(text), "%c %u. %.23s - %.12s",
                         is_favorite(item->id) ? 'F' : ' ', index + 1, item->title,
                         item->artist[0] ? item->artist : "?");
                ui_text(2, 9 + (int)row * 2, text, 36,
                        index == online_selection ? UI_ACCENT : UI_TEXT, bg);
            }
            if (auth_thread_running &&
                (auth_job == AUTH_JOB_PREVIEW || auth_job == AUTH_JOB_ZOOM_PREVIEW)) {
                unsigned done = auth_transfer.downloaded_bytes;
                unsigned total = auth_transfer.total_bytes;
                if (total) {
                    unsigned percent = (unsigned)(((u64)done * 100) / total);
                    if (percent > 99) percent = 99;
                    snprintf(text, sizeof(text), auth_job == AUTH_JOB_ZOOM_PREVIEW ?
                             "Detail : %u%%  (%u/%u Ko)" : "Apercu : %u%%  (%u/%u Ko)", percent,
                             done / 1024, total / 1024);
                    lower_rect(16, 168, 288, 4, UI_PANEL);
                    if (done >= total) lower_rect(16, 168, 288, 4, UI_ACCENT);
                    else if (percent) lower_rect(16, 168, (int)(288 * percent / 100), 4, UI_ACCENT);
                } else {
                    snprintf(text, sizeof(text), "Apercu : %u Ko recus", done / 1024);
                    lower_rect(16, 168, 288, 4, UI_PANEL);
                }
                ui_text(2, 20, text, 36, UI_ACCENT, UI_BG);
            } else ui_status(20, status_text);
        } else if (auth_thread_running && auth_job == AUTH_JOB_TAG) {
            ui_text(2, 10, "Recherche DeviantArt en cours...", 36, UI_ACCENT, UI_BG);
            ui_text(2, 12, "Jusqu'a 50 oeuvres ; haut/bas pour defiler.", 36, UI_MUTED, UI_BG);
            ui_status(18, status_text);
        } else if (active_tab == TAB_FAVORITES) {
            ui_text(2, 11, "Aucun favori pour le moment.", 36, UI_TEXT, UI_BG);
            ui_text(2, 14, "Dans Decouvrir, selectionne une oeuvre", 36, UI_MUTED, UI_BG);
            ui_text(2, 16, "puis appuie sur Y pour la conserver.", 36, UI_MUTED, UI_BG);
        } else {
            ui_text(2, 10, "SHIELD Online Art est pret.", 36, UI_TEXT, UI_BG);
            ui_text(2, 12, auth_token.access[0] ? "Connexion DeviantArt autorisee." :
                    saved_refresh_token[0] ? "A : reprendre la session sauvegardee." :
                    "Appuie sur A pour te connecter.", 36, UI_MUTED, UI_BG);
            ui_text(2, 14, "X : rechercher un tag DeviantArt", 36, UI_TEXT, UI_BG);
            ui_text(2, 16, "Exemples : splatoon, callie", 36, UI_TEXT, UI_BG);
            if (saved_refresh_token[0]) ui_text(2, 18, "Y : effacer la connexion SD", 36, UI_MUTED, UI_BG);
            ui_status(19, status_text);
        }
    }

    u16 filter_bg = explicit_filter_enabled ? UI_SELECTED : UI_PANEL;
    lower_button(8, 176, 304, 20, filter_bg);
    snprintf(text, sizeof(text), "Filtrage anti-explicite : %s", explicit_filter_enabled ? "actif" : "inactif");
    ui_text(2, 23, text, 36, explicit_filter_enabled ? UI_ACCENT : UI_MUTED, filter_bg);
    if (filter_notice_until) {
        ui_text(2, 25, explicit_filter_enabled ? "Filtrage actif" : "Filtrage inactif", 36,
                explicit_filter_enabled ? UI_RGB(100,235,175) : UI_RGB(255,200,110), UI_BG);
    } else ui_text(2, 25, "HOME desactive pour eviter le gel", 36, UI_MUTED, UI_BG);
    lower_button(8, 212, 148, 20, UI_PANEL);
    lower_button(164, 212, 148, 20, UI_PANEL);
    ui_text(2, 27, viewing_image ? "B Retour" : active_tab == TAB_LOCAL ? "A Ouvrir" :
            (active_tab == TAB_DISCOVER || active_tab == TAB_FAVORITES) ?
            (visible_art_count() ? "A Apercu" : "X Rechercher") : "X Rechercher",
            16, UI_TEXT, UI_PANEL);
    ui_text(22, 27, viewing_image && viewed_online_item_valid ? "Y Favori" :
            !viewing_image && active_tab == TAB_LOCAL ? "B Parent" :
            !viewing_image && (active_tab == TAB_DISCOVER || active_tab == TAB_FAVORITES) ?
            (active_tab == TAB_FAVORITES ? "Y Retirer" : "Y Favori") : "SELECT Aide",
            15, UI_TEXT, UI_PANEL);
    draw_battery_footer();
}

static void open_relative_image(int direction) {
    if (!image_count || selection >= entry_count) return;
    unsigned idx = selection;
    for (unsigned checked = 0; checked < image_count; checked++) {
        do {
            if (direction > 0) idx = (idx + 1) % entry_count;
            else idx = idx == 0 ? entry_count - 1 : idx - 1;
        } while (entries[idx].directory);
        if (open_image(entries[idx].name)) {
            selection = idx;
            return;
        }
        if (loading_cancelled || quit_requested) return;
    }
}

static void switch_tab(AppTab next) {
    if (next >= TAB_COUNT) return;
    if (next == active_tab && (next != TAB_LOCAL || local_browser_ready)) return;
    if (active_tab == TAB_LOCAL) save_browser_cache();
    memcpy(tab_status[active_tab], status_text, sizeof(status_text));
    if (viewing_image) deselect_image();
    active_tab = next;
    memcpy(status_text, tab_status[active_tab], sizeof(status_text));
    last_tap_entry = -1;
    if (active_tab == TAB_FAVORITES) online_selection = online_list_start = 0;
    if (active_tab == TAB_LOCAL && !local_browser_ready) {
        local_browser_ready = scan_directory();
        if (local_browser_ready) {
            snprintf(status_text, sizeof(status_text), "Choisis une image. A ouvre, B revient.");
            save_browser_cache();
        }
    }
    draw_home_screen();
}

static void request_search(void) {
    if (auth_thread_running || auth_screen_active) return;
    char candidate[SEARCH_CAPACITY] = {0};
    SwkbdState keyboard;
    swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, SEARCH_CAPACITY - 1);
    swkbdSetHintText(&keyboard, "Tag DeviantArt, ex. : splatoon");
    swkbdSetInitialText(&keyboard, "");
    swkbdSetButton(&keyboard, SWKBD_BUTTON_LEFT, "Retour", false);
    swkbdSetButton(&keyboard, SWKBD_BUTTON_RIGHT, "Rechercher", true);
    swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    SwkbdButton button = swkbdInputText(&keyboard, candidate, sizeof(candidate));
    if (button == SWKBD_BUTTON_RIGHT) {
        const char *first = candidate;
        while (*first == ' ' || *first == '#') first++;
        size_t length = strlen(first);
        while (length && first[length - 1] == ' ') length--;
        bool valid = length > 0 && length < sizeof(search_tag);
        bool non_space = false;
        for (size_t i = 0; i < length && valid; i++) {
            unsigned char c = (unsigned char)first[i];
            valid = c >= 0x20 && c != 0x7f;
            if (c != ' ') non_space = true;
        }
        valid = valid && non_space;
        switch_tab(TAB_DISCOVER);
        if (valid) {
            memcpy(search_tag, first, length);
            search_tag[length] = '\0';
            online_page_valid = false;
            online_selection = online_list_start = 0;
            if (auth_token.access[0] && osGetTime() < auth_token_expires_at) start_tag_search();
            else snprintf(status_text, sizeof(status_text), "Tag garde. Appuie sur A pour te connecter.");
        } else {
            snprintf(status_text, sizeof(status_text), "Entre un terme ou tag (64 caracteres max).");
        }
    } else if (button == SWKBD_BUTTON_NONE) {
        snprintf(status_text, sizeof(status_text), "Le clavier n'a pas pu s'ouvrir. Reessaie.");
    }
    restore_top_after_loading();
}

static void open_selected_local(void) {
    if (active_tab != TAB_LOCAL || selection >= entry_count) return;
    if (!entries[selection].directory) { open_image(entries[selection].name); return; }
    char next_dir[PATH_SIZE], previous_dir[PATH_SIZE];
    join_path(next_dir, sizeof(next_dir), current_dir, entries[selection].name);
    if (!next_dir[0]) {
        snprintf(status_text, sizeof(status_text), "Ce chemin est trop long pour etre ouvert.");
        return;
    }
    save_browser_cache();
    strcpy(previous_dir, current_dir);
    unsigned previous_selection = selection;
    strcpy(current_dir, next_dir);
    size_t len = strlen(current_dir);
    if (len + 1 < sizeof(current_dir) && current_dir[len - 1] != '/') {
        current_dir[len++] = '/'; current_dir[len] = '\0';
    }
    selection = 0;
    last_tap_entry = -1;
    if (restore_browser_cache()) return;
    if (scan_directory()) save_browser_cache();
    else { strcpy(current_dir, previous_dir); selection = previous_selection; }
}

// Return true when a header action has already been performed. List and footer
// taps instead generate the same single A/B action as the physical buttons.
static bool handle_touch(touchPosition touch, u32 *action) {
    *action = 0;
    if (auth_screen_active) {
        if (touch.py >= 212 && touch.py < 232 && touch.px < 156) *action = KEY_B;
        return true;
    }
    if (touch.px < 8 || touch.px >= 312) return true;
    if (touch.py >= 16 && touch.py < 40) { request_search(); return true; }
    if (touch.py >= 44 && touch.py < 64) {
        unsigned tab = (touch.px - 8) / 104;
        if (tab < TAB_COUNT && (touch.px - 8) % 104 < 96) switch_tab((AppTab)tab);
        return true;
    }
    if (touch.py >= 212 && touch.py < 232) {
        if (touch.px < 156) {
            if (viewing_image) *action = KEY_B;
            else if (active_tab == TAB_LOCAL) *action = KEY_A;
            else if ((active_tab == TAB_DISCOVER || active_tab == TAB_FAVORITES) && visible_art_count())
                *action = KEY_A;
            else { request_search(); return true; }
        } else if (touch.px >= 164) {
            *action = (viewing_image && viewed_online_item_valid) ||
                      (!viewing_image && (active_tab == TAB_DISCOVER || active_tab == TAB_FAVORITES)) ?
                      KEY_Y : !viewing_image && active_tab == TAB_LOCAL ? KEY_B : KEY_SELECT;
        }
        return false;
    }
    if (!viewing_image && active_tab == TAB_DISCOVER && online_page_valid &&
        touch.py >= ONLINE_LIST_Y && touch.py < ONLINE_LIST_Y + ONLINE_VISIBLE_ROWS * ONLINE_ROW_HEIGHT) {
        unsigned row = (touch.py - ONLINE_LIST_Y) / ONLINE_ROW_HEIGHT;
        unsigned index = online_list_start + row;
        if (index < online_page.count) {
            u64 now = osGetTime();
            if (last_tap_entry == (int)index && now - last_tap_time <= 500) {
                *action = KEY_A;
                last_tap_entry = -1;
            } else {
                online_selection = index;
                if (online_selection < online_list_start) online_list_start = online_selection;
                else if (online_selection >= online_list_start + ONLINE_VISIBLE_ROWS)
                    online_list_start = online_selection - ONLINE_VISIBLE_ROWS + 1;
                last_tap_entry = (int)index;
                last_tap_time = now;
            }
        }
        return false;
    }
    if (!viewing_image && active_tab == TAB_FAVORITES && favorite_count &&
        touch.py >= ONLINE_LIST_Y && touch.py < ONLINE_LIST_Y + ONLINE_VISIBLE_ROWS * ONLINE_ROW_HEIGHT) {
        unsigned row = (touch.py - ONLINE_LIST_Y) / ONLINE_ROW_HEIGHT;
        unsigned index = online_list_start + row;
        if (index < favorite_count) {
            u64 now = osGetTime();
            if (last_tap_entry == (int)index && now - last_tap_time <= 500) {
                *action = KEY_A;
                last_tap_entry = -1;
            } else {
                online_selection = index;
                last_tap_entry = (int)index;
                last_tap_time = now;
            }
        }
        return false;
    }
    if (!viewing_image && active_tab == TAB_LOCAL &&
        touch.py >= LOCAL_LIST_Y && touch.py < LOCAL_LIST_Y + LOCAL_VISIBLE_ROWS * LOCAL_ROW_HEIGHT) {
        unsigned index = local_list_start() + (touch.py - LOCAL_LIST_Y) / LOCAL_ROW_HEIGHT;
        if (index < entry_count) {
            u64 now = osGetTime();
            if (last_tap_entry == (int)index && now - last_tap_time <= 500) {
                *action = KEY_A;
                last_tap_entry = -1;
            } else {
                selection = index;
                last_tap_entry = (int)index;
                last_tap_time = now;
            }
        }
    }
    return false;
}

int main(void) {
    gfxInitDefault();
    home_allowed_on_launch = aptIsHomeAllowed();
    aptSetHomeAllowed(false);
    gfxSet3D(false);
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, &bottom_console);
    LightLock_Init(&auth_result_lock);
    hidSetRepeatParameters(20, 6);
    cfgu_service_ready = R_SUCCEEDED(cfguInit());
    if (cfgu_service_ready &&
        R_SUCCEEDED(CFGU_GetConfigInfoBlk2(sizeof(backlight_config), 0x00050001,
                                           backlight_config)) &&
        backlight_config[1] >= 1 && backlight_config[1] <= 5) {
        brightness_level = backlight_config[1];
        brightness_on_launch = brightness_level;
        backlight_config_valid = true;
    }
    lcd_ready = R_SUCCEEDED(gspLcdInit());
    battery_service_ready = R_SUCCEEDED(mcuHwcInit());
    update_battery_status(osGetTime());
    fsInit();
    load_favorites();
    load_refresh_token_from_sd();
    guide_active = !guide_was_seen();
    memcpy(status_text, tab_status[active_tab], sizeof(status_text));
    draw_filter_choice();
    draw_ui();
    present_frame();

    u64 last_input_time = osGetTime();
    u64 zoom_repeat_deadline = 0;
    while (!quit_requested && aptMainLoop()) {
        u64 now = osGetTime();
        u64 elapsed = now - last_input_time;
        last_input_time = now;
        unsigned elapsed_ms = elapsed > 50 ? 50 : (unsigned)elapsed;
        bool image_changed = false;
        hidScanInput();
        u32 down = hidKeysDown();
        u32 navigation = (hidKeysDownRepeat() | down) &
                         (KEY_DUP | KEY_DDOWN | KEY_CPAD_UP | KEY_CPAD_DOWN);
        bool ui_changed = down != 0;
        circlePosition circle;
        hidCircleRead(&circle);
        if (down & KEY_START) break;
        if (filter_choice_pending) {
            if (down & KEY_A) select_session_filter(true);
            else if (down & KEY_B) select_session_filter(false);
        } else if (save_login_prompt_pending) {
            if (down & KEY_A) resolve_save_login_prompt(true);
            else if (down & KEY_B) resolve_save_login_prompt(false);
            ui_changed = true;
        } else if (auth_screen_active) {
            bool cancel_login = (down & KEY_B) != 0;
            if (down & KEY_TOUCH) {
                touchPosition touch;
                hidTouchRead(&touch);
                cancel_login = cancel_login || (touch.py >= 212 && touch.py < 232 && touch.px < 156);
            }
            if (cancel_login) {
                auth_cancel_requested = true;
                stop_auth_listener();
                if (!auth_thread_running) {
                    oauth_session_clear(&auth_session);
                    auth_screen_active = false;
                    snprintf(status_text, sizeof(status_text), "Connexion annulee.");
                }
                ui_changed = true;
            }
        } else if (auth_thread_running &&
                   (auth_job == AUTH_JOB_TAG || auth_job == AUTH_JOB_PREVIEW)) {
            if (down & KEY_B) {
                auth_cancel_requested = true;
                snprintf(status_text, sizeof(status_text), "Annulation en cours...");
            }
            ui_changed = ui_changed || (down & KEY_B) != 0;
        } else if (auth_thread_running && auth_job == AUTH_JOB_ZOOM_PREVIEW && (down & KEY_B)) {
            auth_cancel_requested = true;
            deselect_image();
            draw_home_screen();
            ui_changed = true;
        } else if (guide_active) {
            if ((down & KEY_B) && guide_page) {
                guide_page--;
                draw_first_run_guide();
            } else if (down & KEY_A) {
                if (guide_page < 2) {
                    guide_page++;
                    draw_first_run_guide();
                } else {
                    remember_guide_seen();
                    guide_active = false;
                    restore_top_after_loading();
                }
            }
        } else {
            bool handled = false;
            if (down & KEY_TOUCH) {
                touchPosition touch;
                hidTouchRead(&touch);
                handled = handle_touch(touch, &down);
            }
            if (!handled && (down & KEY_SELECT)) {
                guide_page = 0;
                guide_active = true;
                draw_first_run_guide();
            } else if (!handled && (down & KEY_Y)) {
                if (viewing_image && viewed_online_item_valid)
                    toggle_favorite(&viewed_online_item);
                else if (!viewing_image && active_tab == TAB_DISCOVER && online_page_valid &&
                         online_selection < online_page.count)
                    toggle_favorite(&online_page.items[online_selection]);
                else if (!viewing_image && active_tab == TAB_FAVORITES && online_selection < favorite_count)
                    toggle_favorite(&favorite_items[online_selection]);
                else if (!viewing_image && active_tab == TAB_DISCOVER && saved_refresh_token[0]) {
                    forget_saved_login();
                    snprintf(status_text, sizeof(status_text),
                             "Jeton SD efface ; autorisation DA non revoquee.");
                }
                ui_changed = true;
            } else if (!handled && !viewing_image &&
                       (active_tab == TAB_DISCOVER || active_tab == TAB_FAVORITES) && (down & KEY_A)) {
                if (visible_art_count())
                    download_online_preview(online_selection, false);
                else if (active_tab == TAB_DISCOVER && auth_token.access[0] && search_tag[0]) start_tag_search();
                else if (active_tab == TAB_DISCOVER) start_deviantart_login();
                else snprintf(status_text, sizeof(status_text), "Ajoute un favori depuis Decouvrir avec Y.");
                ui_changed = true;
            } else if (!handled && !viewing_image && (down & KEY_X)) {
                request_search();
            } else if (!handled && viewing_image) {
                if (down & KEY_B) {
                    deselect_image();
                    draw_home_screen();
                } else {
                    if (down & KEY_DUP) change_brightness(1);
                    else if (down & KEY_DDOWN) change_brightness(-1);
                    else if (down & KEY_X) {
                        rotation_quadrants = (rotation_quadrants + 1) & 3;
                        pan_offset_x = pan_offset_y = 0;
                        pan_remainder_x = pan_remainder_y = 0;
                        image_changed = true;
                    } else if ((hidKeysHeld() & (KEY_R | KEY_L)) != 0) {
                        const u32 held = hidKeysHeld();
                        const int direction = (held & KEY_R) ? 1 : -1;
                        if ((down & (KEY_R | KEY_L)) || !zoom_repeat_deadline ||
                            now >= zoom_repeat_deadline) {
                            if (direction > 0 && zoom_percent < 400) {
                                zoom_percent = zoom_percent + 25 > 400 ? 400 : zoom_percent + 25;
                                image_changed = true;
                            } else if (direction < 0 && zoom_percent > 100) {
                                zoom_percent = zoom_percent < 125 ? 100 : zoom_percent - 25;
                                image_changed = true;
                            }
                            zoom_repeat_deadline = now + 140;
                        }
                    } else {
                        zoom_repeat_deadline = 0;
                        if (down & KEY_DRIGHT) open_relative_image(1);
                        else if (down & KEY_DLEFT) open_relative_image(-1);
                    }
                    if (move_image_view(circle, elapsed_ms)) image_changed = true;
                    if (viewing_image && viewed_online_item_valid &&
                        active_tab == TAB_DISCOVER && zoom_percent >= 150 &&
                        viewed_online_item_index < online_page.count)
                        download_online_preview(viewed_online_item_index, true);
                }
            } else if (!handled) {
                if (down & (KEY_DLEFT | KEY_L))
                    switch_tab((AppTab)((active_tab + TAB_COUNT - 1) % TAB_COUNT));
                else if (down & (KEY_DRIGHT | KEY_R))
                    switch_tab((AppTab)((active_tab + 1) % TAB_COUNT));
                else if (active_tab == TAB_LOCAL) {
                    unsigned old_selection = selection;
                    if ((navigation & (KEY_DUP | KEY_CPAD_UP)) && selection > 0) selection--;
                    else if ((navigation & (KEY_DDOWN | KEY_CPAD_DOWN)) && selection + 1 < entry_count) selection++;
                    if (selection != old_selection) { ui_changed = true; last_tap_entry = -1; }
                    if (down & KEY_B) { last_tap_entry = -1; go_parent(); }
                    else if (down & KEY_A) open_selected_local();
                } else if ((active_tab == TAB_DISCOVER && online_page_valid && online_page.count) ||
                           (active_tab == TAB_FAVORITES && favorite_count)) {
                    unsigned count = visible_art_count();
                    unsigned old_selection = online_selection;
                    if ((navigation & (KEY_DUP | KEY_CPAD_UP)) && online_selection > 0) online_selection--;
                    else if ((navigation & (KEY_DDOWN | KEY_CPAD_DOWN)) && online_selection + 1 < count)
                        online_selection++;
                    if (online_selection < online_list_start) online_list_start = online_selection;
                    else if (online_selection >= online_list_start + ONLINE_VISIBLE_ROWS)
                        online_list_start = online_selection - ONLINE_VISIBLE_ROWS + 1;
                    if (old_selection != online_selection) ui_changed = true;
                }
            }
        }
        if (auth_screen_active && auth_session.phase == 1 && !auth_thread_running) {
            OAuthCallbackStatus callback = auth_listener_poll(&auth_listener, &auth_session, osGetTime());
            if (callback == OAUTH_CALLBACK_OK) start_token_exchange();
            else if (callback == OAUTH_CALLBACK_DENIED) {
                stop_auth_listener(); oauth_session_clear(&auth_session);
                auth_screen_active = false;
                snprintf(status_text, sizeof(status_text), "Autorisation refusee sur le telephone.");
                ui_changed = true;
            } else if (callback == OAUTH_CALLBACK_WRONG_STATE) {
                snprintf(status_text, sizeof(status_text), "Reponse non valide. Reessaie le QR.");
                ui_changed = true;
            }
        }
        if (auth_thread_running) {
            bool was_running = auth_thread_running;
            finish_auth_thread();
            if (was_running && !auth_thread_running) ui_changed = true;
            if (auth_thread_running &&
                (auth_job == AUTH_JOB_PREVIEW || auth_job == AUTH_JOB_ZOOM_PREVIEW)) {
                unsigned progress = auth_transfer.downloaded_bytes;
                u64 progress_now = osGetTime();
                if (progress != auth_progress_drawn_bytes &&
                    (progress - auth_progress_drawn_bytes >= 8192 || progress_now - auth_progress_last_draw >= 250)) {
                    auth_progress_drawn_bytes = progress;
                    auth_progress_last_draw = progress_now;
                    if (auth_job == AUTH_JOB_ZOOM_PREVIEW) {
                        unsigned total = auth_transfer.total_bytes;
                        if (total) {
                            unsigned percent = (unsigned)(((u64)progress * 100) / total);
                            if (percent > 99) percent = 99;
                            snprintf(status_text, sizeof(status_text), "Detail haute resolution : %u%%", percent);
                        } else snprintf(status_text, sizeof(status_text), "Detail haute resolution : %u Ko", progress / 1024);
                    }
                    ui_changed = true;
                }
            }
        }
        if (quit_requested) break;
        bool stereo_changed = update_stereo_state();
        if ((image_changed || stereo_changed) && current_image_pixels && !guide_active)
            render_current_image();
        if (filter_notice_until && osGetTime() >= filter_notice_until) {
            filter_notice_until = 0;
            ui_changed = true;
        }
        if (ui_changed || stereo_changed) draw_ui();
        if (update_battery_status(now)) draw_battery_footer();
        present_frame();
    }

    close_auth_services();
    aptSetHomeAllowed(home_allowed_on_launch);
    save_brightness_for_home();
    fsExit();
    clear_browser_cache();
    free_image_levels();
    free(current_image_pixels);
    if (battery_service_ready) mcuHwcExit();
    if (cfgu_service_ready) cfguExit();
    if (lcd_ready) gspLcdExit();
    gfxExit();
    return 0;
}
