// SPDX-License-Identifier: Apache-2.0
//
// c1660w-universal-mac-driver: an IPP Everywhere Printer Application (built on PAPPL) for the
// C1660w-family HBPL v1 color LED printers.
//
// Clients (macOS, Linux, ...) see a driverless printer and send it page rasters. This
// program crops each page to the printer's imageable area, converts it to printer color,
// compresses it to HBPL v1 and sends the job to the real printer.

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pappl/pappl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/hbpl.h"
#include "color.h"

#define VERSION "0.1.0"
#define LEFT_PAD 8          /* blank columns the printer expects before the image */
#define MARGIN_HMM 410      /* 11.62 pt imageable-area margin, in hundredths of mm */

// ---------------------------------------------------------------------------
// Paper and media tables

typedef struct {
    const char *pwg;        /* PWG media size name */
    double w, h;            /* size in points */
    int code;               /* HBPL paper code */
} paper_t;

static const paper_t papers[] = {
    {"na_letter_8.5x11in", 612, 792, 0},
    {"na_legal_8.5x14in", 612, 1008, 1},
    {"iso_a4_210x297mm", 595, 842, 2},
    {"na_executive_7.25x10.5in", 522, 756, 3},
    {"na_number-10_4.125x9.5in", 297, 684, 6},
    {"na_monarch_3.875x7.5in", 279, 540, 7},
    {"iso_c5_162x229mm", 459, 649, 8},
    {"iso_dl_110x220mm", 312, 624, 9},
    {"jis_b5_182x257mm", 516, 729, 11},
    {"iso_a5_148x210mm", 420, 595, 15},
    {"na_foolscap_8.5x13in", 612, 936, 205},
};

/* PWG media-type keyword -> PJL @MSIP value. */
static const char *const media_types[][2] = {
    {"stationery", "NORMAL"},
    {"stationery-heavyweight", "HIGHQUALITY"},
    {"cardstock", "THICK"},
    {"stationery-coated", "COATEDPAPER2"},
    {"labels", "LABEL"},
    {"envelope", "ENVELOPE"},
};

static const paper_t *find_paper(double w, double h) {
    for (size_t i = 0; i < sizeof papers / sizeof *papers; i++)
        if ((fabs(w - papers[i].w) <= 2 && fabs(h - papers[i].h) <= 2) ||
            (fabs(w - papers[i].h) <= 2 && fabs(h - papers[i].w) <= 2))
            return &papers[i];
    return NULL;
}

static const char *msip_for(const char *type) {
    for (size_t i = 0; i < sizeof media_types / sizeof *media_types; i++)
        if (type && !strcmp(type, media_types[i][0])) return media_types[i][1];
    return "NORMAL";
}

// ---------------------------------------------------------------------------
// Per-job state

typedef struct {
    hbpl_page pg;
    hbpl_buf data;
} stored_page_t;

typedef struct {
    int color, deep, econo, gray_in;
    int width, height;      /* cropped image size */
    int x0, y0;             /* crop origin in the client's full-page raster */
    int stride;
    uint8_t *page;          /* stride * height * deep */
    size_t page_size;
    hbpl_page cur;
    stored_page_t *pages;
    size_t npages;
} job_t;

static void job_free(job_t *j) {
    if (!j) return;
    for (size_t i = 0; i < j->npages; i++) hbpl_buf_free(&j->pages[i].data);
    free(j->pages);
    free(j->page);
    free(j);
}

// ---------------------------------------------------------------------------
// Raster callbacks

static bool hbpl_rstartjob(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device) {
    (void)options;
    (void)device;
    job_t *j = calloc(1, sizeof *j);
    if (!j) return false;
    papplJobSetData(job, j);
    return true;
}

static bool hbpl_rstartpage(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device,
                            unsigned page) {
    (void)device;
    job_t *j = papplJobGetData(job);
    const cups_page_header2_t *h = &options->header;
    if (h->HWResolution[0] != 600 || h->HWResolution[1] != 600 || h->cupsBitsPerColor != 8) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unsupported raster: %ux%u dpi, %u bits per color",
                    h->HWResolution[0], h->HWResolution[1], h->cupsBitsPerColor);
        return false;
    }
    j->gray_in = h->cupsBitsPerPixel == 8;
    if (page == 0) {
        j->color = options->print_color_mode != PAPPL_COLOR_MODE_MONOCHROME;
        j->deep = j->color ? 4 : 1;
        j->econo = options->print_quality == IPP_QUALITY_DRAFT;
    }

    /* The client sends the whole sheet; the printer wants the imageable area, inset by the
     * margins (11.62 pt for named sizes, 11 pt for custom sizes). */
    double pw = h->PageSize[0], ph = h->PageSize[1];
    const paper_t *paper = find_paper(pw, ph);
    double margin = paper ? 11.62 : 11.0;
    int off = (int)lround(margin * 600 / 72);
    j->x0 = j->y0 = off;
    j->width = (int)lround((pw - 2 * margin) * 600 / 72);
    j->height = (int)lround((ph - 2 * margin) * 600 / 72);
    if (j->x0 + j->width > (int)h->cupsWidth) j->width = (int)h->cupsWidth - j->x0;
    if (j->y0 + j->height > (int)h->cupsHeight) j->height = (int)h->cupsHeight - j->y0;
    if (j->width < 1 || j->height < 1) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Page %u is too small to print", page + 1);
        return false;
    }
    int content = j->width + LEFT_PAD;
    j->stride = (content + 7) & ~7;

    size_t need = (size_t)j->stride * j->height * j->deep;
    if (need > j->page_size) {
        free(j->page);
        if (!(j->page = malloc(need))) {
            j->page_size = 0;
            return false;
        }
        j->page_size = need;
    }
    memset(j->page, 0, need);

    int landscape = pw > ph;
    j->cur = (hbpl_page){
        .paper_code = paper ? paper->code : 255,
        .long_edge_feed = landscape,
        .custom_width = (int)lround(pw * 600 / 72),
        .custom_height = (int)lround(ph * 600 / 72),
        .offset_x = off,
        .offset_y = off,
        .content_width = content,
        .stride = j->stride,
        .height = j->height,
        .color = j->color,
    };
    return true;
}

static bool hbpl_rwriteline(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device,
                            unsigned y, const unsigned char *line) {
    (void)options;
    (void)device;
    job_t *j = papplJobGetData(job);
    int row = (int)y - j->y0;
    if (row < 0 || row >= j->height) return true;
    int bpp = j->gray_in ? 1 : 3;
    const uint8_t *src = line + (size_t)j->x0 * bpp;
    uint8_t *dst = j->page + ((size_t)row * j->stride + LEFT_PAD) * j->deep;
    if (j->gray_in) {
        if (j->color) color_gray_to_kcmy(src, dst, j->width);
        else color_gray_to_k(src, dst, j->width);
    } else {
        if (j->color) color_rgb_to_kcmy(src, dst, j->width);
        else color_rgb_to_k(src, dst, j->width);
    }
    if (j->econo) color_draft(dst, j->width * j->deep);
    return true;
}

static bool hbpl_rendpage(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device,
                          unsigned page) {
    (void)options;
    (void)device;
    (void)page;
    job_t *j = papplJobGetData(job);
    stored_page_t *p = realloc(j->pages, (j->npages + 1) * sizeof *p);
    if (!p) return false;
    j->pages = p;
    p = &j->pages[j->npages];
    memset(p, 0, sizeof *p);
    if (hbpl_encode_page(j->page, j->stride, j->height, j->color, HBPL_PROFILE_PORTABLE, &p->data)) {
        papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to encode page %u", page + 1);
        return false;
    }
    p->pg = j->cur;
    j->npages++;
    return true;
}

static bool write_buf(pappl_device_t *device, hbpl_buf *b) {
    bool ok = papplDeviceWrite(device, b->data, b->len) == (ssize_t)b->len;
    b->len = 0;
    return ok;
}

/*
 * Send the job in the printer's expected order.
 *
 * Manual duplex: first pass = even pages in descending order, preceded by a blank page when
 * the page count is odd; second pass = odd pages in ascending order. The printer pauses
 * before the second pass (PJL IWASIDE2START) for the stack to be put back in the tray. Back
 * sides arrive already rotated for long-edge binding (we advertise "rotated" back sides).
 * Copies are made by repeating the sequence (collated), each pass of all copies together.
 */
static bool hbpl_rendjob(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device) {
    job_t *j = papplJobGetData(job);
    bool ok = true;
    int n = (int)j->npages;
    if (n == 0) {
        job_free(j);
        papplJobSetData(job, NULL);
        return true;
    }
    int duplex = options->sides == PAPPL_SIDES_TWO_SIDED_LONG_EDGE   ? 1
                 : options->sides == PAPPL_SIDES_TWO_SIDED_SHORT_EDGE ? 2
                                                                     : 0;
    /* If the client already put the copies in the raster stream, print it as received. */
    int copies = options->copies > 1 ? options->copies : 1;
    int per_set = n, sets = 1;
    if (copies > 1 && options->num_pages > 0 && n == (int)options->num_pages * copies) {
        per_set = (int)options->num_pages;
        sets = copies;
        copies = 1;
    }
    int reps = sets * copies;

    int *seq = malloc(sizeof(int) * (size_t)(per_set + 1) * (size_t)reps);   /* -1 = blank */
    if (!seq) ok = false;
    int len = 0, side2 = 0;
    for (int r = 0; ok && r < reps; r++) {
        int base = (r % sets) * per_set;
        if (duplex) {
            if (per_set % 2) seq[len++] = -1;
            for (int p = per_set - per_set % 2; p >= 2; p -= 2) seq[len++] = base + p - 1;
        } else {
            for (int p = 0; p < per_set; p++) seq[len++] = base + p;
        }
    }
    if (ok && duplex) {
        side2 = len + 1;
        for (int r = 0; r < reps; r++) {
            int base = (r % sets) * per_set;
            for (int p = 1; p <= per_set; p += 2) seq[len++] = base + p - 1;
        }
    }

    hbpl_buf blank = {0}, out = {0};
    hbpl_page blank_pg = j->pages[n - 1].pg;
    if (ok && duplex && per_set % 2) {
        size_t sz = (size_t)blank_pg.stride * blank_pg.height * (blank_pg.color ? 4 : 1);
        uint8_t *white = calloc(1, sz);
        ok = white && !hbpl_encode_page(white, blank_pg.stride, blank_pg.height, blank_pg.color,
                                        HBPL_PROFILE_PORTABLE, &blank);
        free(white);
        blank_pg.blank = 1;
    }

    char host[256] = "";
    papplSystemGetHostName(papplPrinterGetSystem(papplJobGetPrinter(job)), host, sizeof host);
    hbpl_job hj = {
        .title = papplJobGetName(job),
        .user = papplJobGetUsername(job),
        .host = host,
        .time = time(NULL),
        .copies = 1,
        .color = j->color,
        .economode = j->econo,
        .media = msip_for(options->media.type),
        .long_edge_feed = j->pages[0].pg.long_edge_feed,
        .manual_duplex = duplex,
        .side2_start = side2,
        .jam_recovery = 0,
    };
    ok = ok && !hbpl_append_job_header(&out, &hj) && write_buf(device, &out);
    for (int i = 0; ok && i < len && !papplJobIsCanceled(job); i++) {
        hbpl_page pg = seq[i] < 0 ? blank_pg : j->pages[seq[i]].pg;
        pg.page_number = i + 1;
        ok = !hbpl_append_page(&out, &pg, seq[i] < 0 ? &blank : &j->pages[seq[i]].data) &&
             write_buf(device, &out);
    }
    ok = ok && !hbpl_append_job_trailer(&out) && write_buf(device, &out);
    papplDeviceFlush(device);
    if (!ok) papplLogJob(job, PAPPL_LOGLEVEL_ERROR, "Unable to send the job to the printer");

    hbpl_buf_free(&out);
    hbpl_buf_free(&blank);
    free(seq);
    job_free(j);
    papplJobSetData(job, NULL);
    return ok;
}

/* Raw jobs already in the printer's language are copied through unchanged. */
static bool hbpl_printfile(pappl_job_t *job, pappl_pr_options_t *options, pappl_device_t *device) {
    (void)options;
    char fname[1024];
    int fd = papplJobOpenFile(job, fname, sizeof fname, NULL, NULL, "r");
    if (fd < 0) return false;
    char buf[65536];
    ssize_t n;
    bool ok = true;
    while (ok && (n = read(fd, buf, sizeof buf)) > 0) ok = papplDeviceWrite(device, buf, (size_t)n) == n;
    close(fd);
    return ok;
}

// ---------------------------------------------------------------------------
// Driver definition

static pappl_pr_driver_t drivers[] = {
    {"c1660w", "Dell C1660w Color Printer", "MFG:Dell;MDL:C1660w Color Printer;", NULL},
};

static bool hbpl_driver_cb(pappl_system_t *system, const char *driver_name, const char *device_uri,
                           const char *device_id, pappl_pr_driver_data_t *d, ipp_t **driver_attrs,
                           void *data) {
    (void)system;
    (void)device_uri;
    (void)device_id;
    (void)driver_attrs;
    (void)data;
    if (strcmp(driver_name, "c1660w")) return false;

    d->printfile_cb = hbpl_printfile;
    d->rstartjob_cb = hbpl_rstartjob;
    d->rstartpage_cb = hbpl_rstartpage;
    d->rwriteline_cb = hbpl_rwriteline;
    d->rendpage_cb = hbpl_rendpage;
    d->rendjob_cb = hbpl_rendjob;
    d->format = "application/vnd.hbpl";

    strncpy(d->make_and_model, "Dell C1660w Color Printer", sizeof d->make_and_model - 1);
    d->ppm = 15;
    d->ppm_color = 12;
    d->kind = PAPPL_KIND_DOCUMENT | PAPPL_KIND_ENVELOPE;
    d->orient_default = IPP_ORIENT_NONE;
    d->quality_default = IPP_QUALITY_NORMAL;

    d->num_resolution = 1;
    d->x_resolution[0] = d->y_resolution[0] = 600;
    d->x_default = d->y_default = 600;
    d->raster_types = PAPPL_PWG_RASTER_TYPE_SRGB_8 | PAPPL_PWG_RASTER_TYPE_SGRAY_8;
    d->color_supported = PAPPL_COLOR_MODE_AUTO | PAPPL_COLOR_MODE_COLOR | PAPPL_COLOR_MODE_MONOCHROME;
    d->color_default = PAPPL_COLOR_MODE_AUTO;

    /* Manual duplex: back sides rotated 180 degrees for long-edge binding. */
    d->duplex = PAPPL_DUPLEX_ROTATED;
    d->sides_supported = PAPPL_SIDES_ONE_SIDED | PAPPL_SIDES_TWO_SIDED_LONG_EDGE | PAPPL_SIDES_TWO_SIDED_SHORT_EDGE;
    d->sides_default = PAPPL_SIDES_ONE_SIDED;

    d->left_right = MARGIN_HMM;
    d->bottom_top = MARGIN_HMM;
    d->num_media = 0;
    for (size_t i = 0; i < sizeof papers / sizeof *papers; i++) d->media[d->num_media++] = papers[i].pwg;
    d->media[d->num_media++] = "custom_min_76.2x127mm";
    d->media[d->num_media++] = "custom_max_215.9x355.6mm";

    d->num_source = 1;
    d->source[0] = "main";
    d->num_type = (int)(sizeof media_types / sizeof *media_types);
    for (int i = 0; i < d->num_type; i++) d->type[i] = media_types[i][0];

    pwg_media_t *pwg = pwgMediaForPWG("na_letter_8.5x11in");
    pappl_media_col_t *m = &d->media_ready[0];
    strncpy(m->size_name, "na_letter_8.5x11in", sizeof m->size_name - 1);
    strncpy(m->source, "main", sizeof m->source - 1);
    strncpy(m->type, "stationery", sizeof m->type - 1);
    m->size_width = pwg ? pwg->width : 21590;
    m->size_length = pwg ? pwg->length : 27940;
    m->left_margin = m->right_margin = m->top_margin = m->bottom_margin = MARGIN_HMM;
    d->media_default = *m;
    return true;
}

/* Pick our driver for printers that identify as a C1660w. A network printer is found twice,
 * through DNS-SD and through SNMP; add only the DNS-SD one, which keeps working when the
 * printer's address changes. (Adding a printer by hand can still use any device.) */
static const char *hbpl_autoadd_cb(const char *device_info, const char *device_uri,
                                   const char *device_id, void *data) {
    (void)device_info;
    (void)data;
    if (device_uri && !strncmp(device_uri, "snmp:", 5)) return NULL;
    cups_option_t *did;
    int n = papplDeviceParseID(device_id, &did);
    const char *mdl = cupsGetOption("MODEL", n, did);
    if (!mdl) mdl = cupsGetOption("MDL", n, did);
    const char *ret = mdl && strstr(mdl, "C1660w") ? "c1660w" : NULL;
    cupsFreeOptions(n, did);
    return ret;
}

// ---------------------------------------------------------------------------
// System service mode
//
// By default PAPPL keeps its state in the user's home folder. When C1660W_STATE_DIR is set
// (the installed launchd service sets it), the state file, spool and default log go in that
// directory instead. This follows PAPPL's default system setup and accepts the same -o
// options, plus the paths above.

#define BASE_NAME "c1660w-universal-mac-driver"

static pappl_soptions_t parse_soptions(const char *value) {
    pappl_soptions_t so = PAPPL_SOPTIONS_MULTI_QUEUE | PAPPL_SOPTIONS_WEB_INTERFACE | PAPPL_SOPTIONS_WEB_TLS;
    static const struct { const char *name; pappl_soptions_t set, clear; } names[] = {
        {"none", 0, (pappl_soptions_t)~0},
        {"dnssd-host", PAPPL_SOPTIONS_DNSSD_HOST, 0},
        {"no-multi-queue", 0, PAPPL_SOPTIONS_MULTI_QUEUE},
        {"raw-socket", PAPPL_SOPTIONS_RAW_SOCKET, 0},
        {"usb-printer", PAPPL_SOPTIONS_USB_PRINTER, 0},
        {"no-web-interface", 0, PAPPL_SOPTIONS_WEB_INTERFACE},
        {"web-log", PAPPL_SOPTIONS_WEB_LOG, 0},
        {"web-network", PAPPL_SOPTIONS_WEB_NETWORK, 0},
        {"web-remote", PAPPL_SOPTIONS_WEB_REMOTE, 0},
        {"web-security", PAPPL_SOPTIONS_WEB_SECURITY, 0},
        {"no-tls", PAPPL_SOPTIONS_NO_TLS, PAPPL_SOPTIONS_WEB_TLS},
    };
    for (const char *p = value; p && *p;) {
        size_t len = strcspn(p, ",");
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            if (strlen(names[i].name) == len && !strncmp(p, names[i].name, len))
                so = (pappl_soptions_t)((so & ~names[i].clear) | names[i].set);
        p += len;
        if (*p == ',') p++;
    }
    return so;
}

static pappl_system_t *service_system_cb(int num_options, cups_option_t *options, void *data) {
    const char *dir = data;
    static char spool[1024], logfile[1024], state[1024];
    snprintf(spool, sizeof spool, "%s/spool", dir);
    snprintf(logfile, sizeof logfile, "%s/%s.log", dir, BASE_NAME);
    snprintf(state, sizeof state, "%s/%s.state", dir, BASE_NAME);

    const char *v;
    pappl_loglevel_t level = PAPPL_LOGLEVEL_WARN;
    if ((v = cupsGetOption("log-level", num_options, options))) {
        static const char *const levels[] = {"debug", "info", "warn", "error", "fatal"};
        for (int i = 0; i < 5; i++)
            if (!strcmp(v, levels[i])) level = (pappl_loglevel_t)(PAPPL_LOGLEVEL_DEBUG + i);
    }
    int port = 0;
    if ((v = cupsGetOption("server-port", num_options, options))) {
        char *end;
        long n = strtol(v, &end, 10);
        if (n < 0 || n > 65535 || *end) {
            fprintf(stderr, "%s: Bad 'server-port' value.\n", BASE_NAME);
            return NULL;
        }
        port = (int)n;
    }
    const char *spooldir = cupsGetOption("spool-directory", num_options, options);
    if (!spooldir) {
        if (mkdir(spool, 0700) && errno != EEXIST) {
            fprintf(stderr, "%s: Unable to create '%s': %s\n", BASE_NAME, spool, strerror(errno));
            return NULL;
        }
        spooldir = spool;
    }
    const char *log = cupsGetOption("log-file", num_options, options);
    if (!log) {
        /* PAPPL creates the log readable only by the service user; create it first so that
         * users can read it (it holds no document contents). */
        int fd = open(logfile, O_CREAT | O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0) close(fd);
    }

    pappl_system_t *system = papplSystemCreate(
        parse_soptions(cupsGetOption("server-options", num_options, options)), BASE_NAME, port,
        "_print,_universal", spooldir, log ? log : logfile, level,
        cupsGetOption("auth-service", num_options, options), false);
    if (!system) return NULL;
    if ((v = cupsGetOption("admin-group", num_options, options))) papplSystemSetAdminGroup(system, v);
    if ((v = cupsGetOption("server-hostname", num_options, options))) papplSystemSetHostName(system, v);
    if (!cupsGetOption("private-server", num_options, options))
        papplSystemAddListeners(system, cupsGetOption("listen-hostname", num_options, options));

    /* Command-line sub-commands look for a system server's socket in PAPPL's socket
     * directory. The macOS installer builds PAPPL with that directory set to the state
     * directory (build-deps.sh --with-papplsockdir), so listen there too. */
    static char sock[1024];
    snprintf(sock, sizeof sock, "%s/%s.sock", dir, BASE_NAME);
    papplSystemAddListeners(system, sock);

    /* PAPPL's main loop skips its own state handling once a save callback is set. */
    papplSystemSetPrinterDrivers(system, (int)(sizeof drivers / sizeof *drivers), drivers, hbpl_autoadd_cb,
                                 NULL, hbpl_driver_cb, NULL);
    papplSystemSetSaveCallback(system, (pappl_save_cb_t)papplSystemSaveState, state);
    if (!papplSystemLoadState(system, state))
        papplSystemCreatePrinters(system, PAPPL_DEVTYPE_LOCAL, NULL, NULL);
    return system;
}

// ---------------------------------------------------------------------------
// "find" sub-command: print the device URI of every C1660w found on the network, one per
// line. The installer uses it to add printers under a clean name; PAPPL's own autoadd names
// them after the discovery label, e.g. "Dellc1660W (DNS-SD Network Printer)".

static bool find_device_cb(const char *device_info, const char *device_uri, const char *device_id,
                           void *data) {
    (void)data;
    if (hbpl_autoadd_cb(device_info, device_uri, device_id, NULL)) puts(device_uri);
    return false;   /* keep listing */
}

static int find_subcmd(const char *base_name, int num_options, cups_option_t *options, int num_files,
                       char **files, void *data) {
    (void)base_name;
    (void)num_options;
    (void)options;
    (void)num_files;
    (void)files;
    (void)data;
    papplDeviceList(PAPPL_DEVTYPE_DNS_SD | PAPPL_DEVTYPE_USB, find_device_cb, NULL, NULL, NULL);
    return 0;
}

/* Web interface footer. It must not be NULL: PAPPL 1.4.12 looks the footer up as a localized
 * string and dereferences a NULL key, which kills the server on the first web page. */
#define FOOTER_HTML "C1660w Universal Mac Driver " VERSION

int main(int argc, char *argv[]) {
    char *dir = getenv("C1660W_STATE_DIR");
    return papplMainloop(argc, argv, VERSION, FOOTER_HTML, (int)(sizeof drivers / sizeof *drivers), drivers,
                         hbpl_autoadd_cb, hbpl_driver_cb, "find", find_subcmd,
                         dir && *dir ? service_system_cb : NULL, NULL, dir && *dir ? dir : NULL);
}
