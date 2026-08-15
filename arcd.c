#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <level_zero/ze_api.h>
#include <level_zero/zes_api.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <drm/drm.h>

#ifndef DRM_IOCTL_XE_DEVICE_QUERY
#define DRM_XE_DEVICE_QUERY             0x00
#define DRM_XE_DEVICE_QUERY_ENGINES     0
#define DRM_XE_DEVICE_QUERY_MEM_REGIONS 1
#define DRM_XE_DEVICE_QUERY_CONFIG      2
#define DRM_XE_DEVICE_QUERY_GT_TOPOLOGY 5
#define DRM_XE_MEM_REGION_CLASS_VRAM    1
#define DRM_XE_ENGINE_CLASS_RENDER        0
#define DRM_XE_ENGINE_CLASS_COPY          1
#define DRM_XE_ENGINE_CLASS_VIDEO_DECODE  2
#define DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE 3
#define DRM_XE_ENGINE_CLASS_COMPUTE       4
struct drm_xe_device_query { uint64_t extensions; uint32_t query; uint32_t size; uint64_t data; uint64_t reserved[2]; };
struct drm_xe_mem_region { uint16_t mem_class; uint16_t instance; uint32_t min_page_size; uint64_t total_size; uint64_t used; uint64_t cpu_visible_size; uint64_t cpu_visible_used; uint64_t reserved[6]; };
struct drm_xe_query_mem_regions { uint32_t num_mem_regions; uint32_t pad; struct drm_xe_mem_region mem_regions[]; };
struct drm_xe_engine_class_instance { uint16_t engine_class; uint16_t engine_instance; uint16_t gt_id; uint16_t pad; };
struct drm_xe_engine { struct drm_xe_engine_class_instance instance; uint64_t reserved[3]; };
struct drm_xe_query_engines { uint32_t num_engines; uint32_t pad; struct drm_xe_engine engines[]; };
struct drm_xe_query_config { uint32_t num_params; uint32_t pad; uint64_t info[]; };
struct drm_xe_query_topology_mask { uint16_t gt_id; uint16_t type; uint32_t num_bytes; uint8_t mask[]; };
#define DRM_IOCTL_XE_DEVICE_QUERY DRM_IOWR(DRM_COMMAND_BASE + DRM_XE_DEVICE_QUERY, struct drm_xe_device_query)
#endif

#define MAX_PMU_ENGINES 32
#define MAX_HWMON_TEMPS 8
#define MAX_CARDS       8
#define XE_PMU_ACTIVE_TICKS 0x02ULL
#define XE_PMU_TOTAL_TICKS  0x03ULL

struct pmu_engine { int fd_active, fd_total; uint64_t a0, t0; };
struct hwmon_temp { char path[1024]; int index; };
struct hwmon_fan  { char input[1024]; char pwm[1024]; char pwm_en[1024]; };

// Everything that used to be a global now lives per-card. One of these per
// physical Arc GPU, all four handles (Sysman/DRM/PMU/hwmon) tied by PCI BDF.
struct arc_card {
    // identity + matched handles
    zes_device_handle_t sysman;
    char     bdf[32];              // "0000:03:00.0"
    int      drm_node;             // e.g. 128
    char     render_name[32];      // "renderD128"
    int      pmu_type;             // perf type, or -1
    char     obj_path[64];         // "/org/freedesktop/ArcCtrl/card0"

    // Sysman handles
    zes_mem_handle_t  *mems;  uint32_t memCount;
    zes_temp_handle_t *temps; uint32_t tempCount;
    zes_fan_handle_t  *fans;  uint32_t fanCount;

    // hwmon fallbacks
    struct hwmon_temp htemps[MAX_HWMON_TEMPS]; int nhtemps;
    struct hwmon_fan  hfan;

    // PMU engine counters
    struct pmu_engine pengines[MAX_PMU_ENGINES]; int npeng;

    // telemetry (the D-Bus properties)
    char gpu_name[256];
    char architecture[64];
    uint32_t compute_cores;
    uint32_t xe_cores;
    char rebar_status[64];
    uint64_t vram_total;
    uint64_t vram_used;
    uint64_t cpu_visible_vram;
    double temperature;
    uint32_t fan_rpm;
    int num_engines;
    struct { char name[32]; double util; } engines[MAX_PMU_ENGINES];
};

static struct arc_card cards[MAX_CARDS];
static int ncards = 0;

// -- Helpers --
static const char *engine_class_name(uint16_t c) {
    switch (c) {
    case DRM_XE_ENGINE_CLASS_RENDER: return "RENDER";
    case DRM_XE_ENGINE_CLASS_COPY: return "COPY";
    case DRM_XE_ENGINE_CLASS_VIDEO_DECODE: return "VDECODE";
    case DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE: return "VENHANCE";
    case DRM_XE_ENGINE_CLASS_COMPUTE: return "COMPUTE";
    default: return "ENGINE";
    }
}

static char *drm_driver_name(int fd) {
    struct drm_version v; memset(&v, 0, sizeof(v));
    if (ioctl(fd, DRM_IOCTL_VERSION, &v) != 0) return NULL;
    char *name = calloc(v.name_len + 1, 1);
    if (!name) return NULL;
    v.name = name; v.date_len = 0; v.desc_len = 0;
    if (ioctl(fd, DRM_IOCTL_VERSION, &v) != 0) { free(name); return NULL; }
    return name;
}

// -- BDF matching (verified by card_enum_probe on single- and dual-card HW) --
static void bdf_str(const zes_pci_address_t *a, char *out, size_t n) {
    snprintf(out, n, "%04x:%02x:%02x.%x", a->domain, a->bus, a->device, a->function);
}

// renderD<N> whose PCI device symlink ends in this BDF, and whose driver is xe.
static int xe_drm_node_for_bdf(const char *bdf) {
    for (int n = 128; n <= 143; n++) {
        char link[128], tgt[512];
        snprintf(link, sizeof(link), "/sys/class/drm/renderD%d/device", n);
        ssize_t l = readlink(link, tgt, sizeof(tgt) - 1);
        if (l < 0) continue;
        tgt[l] = '\0';
        const char *base = strrchr(tgt, '/');
        base = base ? base + 1 : tgt;
        if (strcmp(base, bdf) != 0) continue;
        // confirm it's xe (not an i915 iGPU that shares the enumeration)
        char dev[64]; snprintf(dev, sizeof(dev), "/dev/dri/renderD%d", n);
        int fd = open(dev, O_RDWR);
        if (fd < 0) return -1;
        char *drv = drm_driver_name(fd);
        int is_xe = drv && strcmp(drv, "xe") == 0;
        free(drv); close(fd);
        return is_xe ? n : -1;
    }
    return -1;
}

// xe PMU perf type for this BDF ("xe_0000_03_00.0"), or -1.
static int xe_pmu_type_for_bdf(const zes_pci_address_t *a) {
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/bus/event_source/devices/xe_%04x_%02x_%02x.%x/type",
             a->domain, a->bus, a->device, a->function);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int t = -1;
    if (fscanf(f, "%d", &t) != 1) t = -1;
    fclose(f);
    return t;
}

static int open_card_node(const struct arc_card *c) {
    char path[64]; snprintf(path, sizeof(path), "/dev/dri/renderD%d", c->drm_node);
    return open(path, O_RDWR);
}

// -- Per-card hardware discovery (logic identical to the single-card daemon) --
static void probe_xe_topology(struct arc_card *c) {
    strcpy(c->rebar_status, "Unknown");
    strcpy(c->architecture, "Unknown");
    int drm_fd = open_card_node(c);
    if (drm_fd < 0) return;

    // 1. ReBAR Status & Visible VRAM
    struct drm_xe_device_query q_mem = { .query = DRM_XE_DEVICE_QUERY_MEM_REGIONS, .size = 0, .data = 0 };
    if (ioctl(drm_fd, DRM_IOCTL_XE_DEVICE_QUERY, &q_mem) == 0 && q_mem.size > 0) {
        struct drm_xe_query_mem_regions *regions = calloc(1, q_mem.size);
        q_mem.data = (uintptr_t)regions;
        if (regions && ioctl(drm_fd, DRM_IOCTL_XE_DEVICE_QUERY, &q_mem) == 0) {
            for (uint32_t i = 0; i < regions->num_mem_regions; i++) {
                if (regions->mem_regions[i].mem_class == DRM_XE_MEM_REGION_CLASS_VRAM) {
                    c->cpu_visible_vram = regions->mem_regions[i].cpu_visible_size;
                    if (regions->mem_regions[i].total_size == regions->mem_regions[i].cpu_visible_size)
                        strcpy(c->rebar_status, "ENABLED (Full VRAM)");
                    else
                        strcpy(c->rebar_status, "DISABLED (Small BAR active)");
                }
            }
        }
        free(regions);
    }

    // 2. CONFIG (Device ID & Architecture)
    int is_battlemage = 0;
    struct drm_xe_device_query q_conf = { .query = DRM_XE_DEVICE_QUERY_CONFIG, .size = 0, .data = 0 };
    if (ioctl(drm_fd, DRM_IOCTL_XE_DEVICE_QUERY, &q_conf) == 0 && q_conf.size > 0) {
        struct drm_xe_query_config *config = calloc(1, q_conf.size);
        q_conf.data = (uintptr_t)config;
        if (config && ioctl(drm_fd, DRM_IOCTL_XE_DEVICE_QUERY, &q_conf) == 0) {
            if (config->num_params > 0) {
                uint32_t dev_id = config->info[0] & 0xFFFF;
                if ((dev_id & 0xFF00) == 0xE200) {
                    is_battlemage = 1;
                    strcpy(c->architecture, "Xe2-HPG (Battlemage)");
                } else {
                    strcpy(c->architecture, "Xe-HPG (Alchemist)");
                }
            }
        }
        free(config);
    }

    // 3. GT TOPOLOGY (DSS popcount on gt0 == Xe cores)
    struct drm_xe_device_query q_top = { .query = DRM_XE_DEVICE_QUERY_GT_TOPOLOGY, .size = 0, .data = 0 };
    if (ioctl(drm_fd, DRM_IOCTL_XE_DEVICE_QUERY, &q_top) == 0 && q_top.size > 0) {
        uint8_t *buf = calloc(1, q_top.size);
        q_top.data = (uintptr_t)buf;
        if (buf && ioctl(drm_fd, DRM_IOCTL_XE_DEVICE_QUERY, &q_top) == 0) {
            int dss_count = 0;
            uint32_t off = 0;
            while (off + sizeof(struct drm_xe_query_topology_mask) <= q_top.size) {
                struct drm_xe_query_topology_mask *m = (struct drm_xe_query_topology_mask *)(buf + off);
                if (off + sizeof(*m) + m->num_bytes > q_top.size) break;
                if (m->type == 1 && m->gt_id == 0) {
                    for (uint32_t j = 0; j < m->num_bytes; j++) {
                        uint8_t b = m->mask[j];
                        while (b) { dss_count += (b & 1); b >>= 1; }
                    }
                }
                off += sizeof(*m) + m->num_bytes;
            }
            if (dss_count > 0) {
                c->xe_cores = dss_count;
                c->compute_cores = is_battlemage ? (dss_count * 8) : (dss_count * 16);
            }
        }
        free(buf);
    }
    close(drm_fd);
}

static void setup_hwmon_sensors(struct arc_card *c) {
    char hwmon_dir[256];
    snprintf(hwmon_dir, sizeof(hwmon_dir), "/sys/class/drm/%s/device/hwmon", c->render_name);
    DIR *d = opendir(hwmon_dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "hwmon", 5) != 0) continue;
        for (int i = 1; i <= 9 && c->nhtemps < MAX_HWMON_TEMPS; i++) {
            char p[1024]; int len = snprintf(p, sizeof(p), "%s/%s/temp%d_input", hwmon_dir, e->d_name, i);
            if (len < 0 || (size_t)len >= sizeof(p)) continue;
            FILE *f = fopen(p, "r");
            if (f) {
                fclose(f);
                snprintf(c->htemps[c->nhtemps].path, sizeof(c->htemps[c->nhtemps].path), "%s", p);
                c->htemps[c->nhtemps].index = i; c->nhtemps++;
            }
        }
        char pf[1024]; snprintf(pf, sizeof(pf), "%s/%s/fan1_input", hwmon_dir, e->d_name);
        FILE *ff = fopen(pf, "r");
        if (ff) {
            fclose(ff);
            snprintf(c->hfan.input, sizeof(c->hfan.input), "%s", pf);
            snprintf(c->hfan.pwm, sizeof(c->hfan.pwm), "%s/%s/pwm1", hwmon_dir, e->d_name);
            snprintf(c->hfan.pwm_en, sizeof(c->hfan.pwm_en), "%s/%s/pwm1_enable", hwmon_dir, e->d_name);
        }
    }
    closedir(d);
}

static int perf_open_xe(uint32_t type, uint64_t event, uint64_t cls, uint64_t inst, uint64_t gt) {
    uint64_t config = (event & 0xfffULL) | ((inst & 0xffULL) << 12) | ((cls & 0xffULL) << 20) | ((gt & 0xfULL) << 60);
    struct perf_event_attr pea; memset(&pea, 0, sizeof(pea));
    pea.type = type; pea.size = sizeof(pea); pea.config = config;
    return (int)syscall(SYS_perf_event_open, &pea, -1, 0, -1, 0);
}

static int read_counter(int fd, uint64_t *v) { return read(fd, v, sizeof(*v)) == (ssize_t)sizeof(*v) ? 0 : -1; }

static void setup_pmu_engines(struct arc_card *c) {
    if (c->pmu_type < 0) return;
    int drm_fd = open_card_node(c);
    if (drm_fd < 0) return;
    struct drm_xe_device_query q = { .query = DRM_XE_DEVICE_QUERY_ENGINES, .size = 0, .data = 0 };
    if (ioctl(drm_fd, DRM_IOCTL_XE_DEVICE_QUERY, &q) != 0 || q.size == 0) { close(drm_fd); return; }
    struct drm_xe_query_engines *engs = malloc(q.size);
    if (!engs) { close(drm_fd); return; }
    q.data = (uintptr_t)engs;
    if (ioctl(drm_fd, DRM_IOCTL_XE_DEVICE_QUERY, &q) == 0) {
        for (uint32_t i = 0; i < engs->num_engines && c->npeng < MAX_PMU_ENGINES; i++) {
            struct drm_xe_engine_class_instance *ci = &engs->engines[i].instance;
            if (ci->engine_class > DRM_XE_ENGINE_CLASS_COMPUTE) continue;
            int fa = perf_open_xe(c->pmu_type, XE_PMU_ACTIVE_TICKS, ci->engine_class, ci->engine_instance, ci->gt_id);
            int ft = perf_open_xe(c->pmu_type, XE_PMU_TOTAL_TICKS, ci->engine_class, ci->engine_instance, ci->gt_id);
            if (fa >= 0 && ft >= 0) {
                c->pengines[c->npeng].fd_active = fa; c->pengines[c->npeng].fd_total = ft;
                read_counter(fa, &c->pengines[c->npeng].a0); read_counter(ft, &c->pengines[c->npeng].t0);
                snprintf(c->engines[c->npeng].name, sizeof(c->engines[c->npeng].name),
                         "%s%u gt%u", engine_class_name(ci->engine_class), ci->engine_instance, ci->gt_id);
                c->npeng++;
            }
        }
    }
    c->num_engines = c->npeng;
    free(engs); close(drm_fd);
}

// -- Hardware controls (per-card, sysfs fallbacks keyed on render_name) --
static int set_power_sysfs(struct arc_card *c, double watts) {
    char hwmon_dir[256]; snprintf(hwmon_dir, sizeof(hwmon_dir), "/sys/class/drm/%s/device/hwmon", c->render_name);
    DIR *d = opendir(hwmon_dir); if (!d) return 0;
    struct dirent *e; int success = 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "hwmon", 5) == 0) {
            char p[1024]; snprintf(p, sizeof(p), "%s/%s/power1_max", hwmon_dir, e->d_name);
            FILE *f = fopen(p, "w");
            if (f) { fprintf(f, "%ld\n", (long)(watts * 1000000.0)); fclose(f); success = 1; break; }
        }
    }
    closedir(d); return success;
}

static int set_clock_sysfs(struct arc_card *c, double min_mhz, double max_mhz) {
    const char *subdirs[] = {"tile0/gt0", "gt/gt0"};
    for (int i = 0; i < 2; i++) {
        char p_min[1024], p_max[1024];
        snprintf(p_min, sizeof(p_min), "/sys/class/drm/%s/device/%s/freq_min", c->render_name, subdirs[i]);
        snprintf(p_max, sizeof(p_max), "/sys/class/drm/%s/device/%s/freq_max", c->render_name, subdirs[i]);
        FILE *f_min = fopen(p_min, "w"); FILE *f_max = fopen(p_max, "w");
        if (f_min && f_max) {
            fprintf(f_min, "%ld\n", (long)min_mhz); fprintf(f_max, "%ld\n", (long)max_mhz);
            fclose(f_min); fclose(f_max); return 1;
        }
        if (f_min) fclose(f_min);
        if (f_max) fclose(f_max);
    }
    return 0;
}

static int reset_clock_sysfs(struct arc_card *c) {
    const char *subdirs[] = {"tile0/gt0", "gt/gt0"};
    for (int i = 0; i < 2; i++) {
        char p_rp0[1024], p_rpn[1024], p_min[1024], p_max[1024];
        snprintf(p_rp0, sizeof(p_rp0), "/sys/class/drm/%s/device/%s/freq_RP0", c->render_name, subdirs[i]);
        snprintf(p_rpn, sizeof(p_rpn), "/sys/class/drm/%s/device/%s/freq_RPn", c->render_name, subdirs[i]);
        snprintf(p_min, sizeof(p_min), "/sys/class/drm/%s/device/%s/freq_min", c->render_name, subdirs[i]);
        snprintf(p_max, sizeof(p_max), "/sys/class/drm/%s/device/%s/freq_max", c->render_name, subdirs[i]);
        FILE *fr0 = fopen(p_rp0, "r"); FILE *frn = fopen(p_rpn, "r");
        if (fr0 && frn) {
            long max_val = 0, min_val = 0;
            if (fscanf(fr0, "%ld", &max_val) == 1 && fscanf(frn, "%ld", &min_val) == 1) {
                fclose(fr0); fclose(frn);
                FILE *f_min = fopen(p_min, "w"); FILE *f_max = fopen(p_max, "w");
                if (f_min && f_max) {
                    fprintf(f_min, "%ld\n", min_val); fprintf(f_max, "%ld\n", max_val);
                    fclose(f_min); fclose(f_max); return 1;
                }
                if (f_min) fclose(f_min);
                if (f_max) fclose(f_max);
            } else { fclose(fr0); fclose(frn); }
        } else {
            if (fr0) fclose(fr0);
            if (frn) fclose(frn);
        }
    }
    return 0;
}

static zes_pwr_handle_t find_power_domain(struct arc_card *c) {
    uint32_t n = 0; zesDeviceEnumPowerDomains(c->sysman, &n, NULL);
    if (n == 0) return NULL;
    zes_pwr_handle_t *pwrs = malloc(n * sizeof(zes_pwr_handle_t));
    zesDeviceEnumPowerDomains(c->sysman, &n, pwrs);
    zes_pwr_handle_t found = pwrs[0];
    free(pwrs); return found;
}

static zes_freq_handle_t find_gpu_freq_domain(struct arc_card *c) {
    uint32_t n = 0; zesDeviceEnumFrequencyDomains(c->sysman, &n, NULL);
    if (n == 0) return NULL;
    zes_freq_handle_t *freqs = malloc(n * sizeof(zes_freq_handle_t));
    zesDeviceEnumFrequencyDomains(c->sysman, &n, freqs);
    zes_freq_handle_t found = NULL;
    for (uint32_t i = 0; i < n; i++) {
        zes_freq_properties_t p = { .stype = ZES_STRUCTURE_TYPE_FREQ_PROPERTIES };
        if (zesFrequencyGetProperties(freqs[i], &p) == ZE_RESULT_SUCCESS && p.type == ZES_FREQ_DOMAIN_GPU) {
            found = freqs[i]; break;
        }
    }
    free(freqs); return found;
}

// -- Polling loop: one timer, updates every card, emits per-object --
static int timer_handler(sd_event_source *s, uint64_t usec, void *userdata) {
    (void)usec; sd_bus *bus = userdata;

    for (int k = 0; k < ncards; k++) {
        struct arc_card *c = &cards[k];

        if (c->memCount > 0) {
            zes_mem_state_t mem_state = { .stype = ZES_STRUCTURE_TYPE_MEM_STATE };
            zesMemoryGetState(c->mems[0], &mem_state);
            c->vram_used = mem_state.size - mem_state.free;
        }

        // Temperature — Sysman first, hwmon fallback
        c->temperature = 0.0;
        if (c->tempCount > 0) {
            double temp = 0.0;
            if (zesTemperatureGetState(c->temps[0], &temp) == ZE_RESULT_SUCCESS && temp > 0.0)
                c->temperature = temp;
        }
        if (c->temperature <= 0.0 && c->nhtemps > 0) {
            double max_temp = 0.0;
            for (int i = 0; i < c->nhtemps; i++) {
                FILE *f = fopen(c->htemps[i].path, "r");
                if (f) {
                    long milli = 0;
                    if (fscanf(f, "%ld", &milli) == 1) {
                        double t = milli / 1000.0;
                        if (t > max_temp) max_temp = t;
                    }
                    fclose(f);
                }
            }
            c->temperature = max_temp;
        }

        // Fan RPM — Sysman first, hwmon fallback
        c->fan_rpm = 0;
        if (c->fanCount > 0) {
            int32_t speed = 0;
            if (zesFanGetState(c->fans[0], ZES_FAN_SPEED_UNITS_RPM, &speed) == ZE_RESULT_SUCCESS)
                c->fan_rpm = speed;
        }
        if (c->fan_rpm == 0 && c->hfan.input[0] != '\0') {
            FILE *f = fopen(c->hfan.input, "r");
            if (f) { long rpm = 0; if (fscanf(f, "%ld", &rpm) == 1) c->fan_rpm = rpm; fclose(f); }
        }

        // Engines (PMU delta since last tick)
        for (int i = 0; i < c->npeng; i++) {
            uint64_t a1 = c->pengines[i].a0, t1 = c->pengines[i].t0;
            read_counter(c->pengines[i].fd_active, &a1);
            read_counter(c->pengines[i].fd_total, &t1);
            uint64_t da = a1 - c->pengines[i].a0, dt = t1 - c->pengines[i].t0;
            c->engines[i].util = dt ? 100.0 * (double)da / (double)dt : 0.0;
            c->pengines[i].a0 = a1; c->pengines[i].t0 = t1;
        }

        sd_bus_emit_properties_changed(bus, c->obj_path, "org.freedesktop.ArcCtrl",
                                       "VramUsed", "Temperature", "FanRPM", "Engines", NULL);
    }

    uint64_t now; sd_event_now(sd_event_source_get_event(s), CLOCK_MONOTONIC, &now);
    sd_event_source_set_time(s, now + 500 * 1000);
    sd_event_source_set_enabled(s, SD_EVENT_ONESHOT);
    return 0;
}

// -- DBus accessors: userdata is &card (offset added per property) --
static int property_get_str(sd_bus *b, const char *p, const char *i, const char *prop, sd_bus_message *reply, void *u, sd_bus_error *e) { (void)b;(void)p;(void)i;(void)prop;(void)e; return sd_bus_message_append(reply, "s", (char*)u); }
static int property_get_u32(sd_bus *b, const char *p, const char *i, const char *prop, sd_bus_message *reply, void *u, sd_bus_error *e) { (void)b;(void)p;(void)i;(void)prop;(void)e; return sd_bus_message_append(reply, "u", *(uint32_t*)u); }
static int property_get_u64(sd_bus *b, const char *p, const char *i, const char *prop, sd_bus_message *reply, void *u, sd_bus_error *e) { (void)b;(void)p;(void)i;(void)prop;(void)e; return sd_bus_message_append(reply, "t", *(uint64_t*)u); }
static int property_get_dbl(sd_bus *b, const char *p, const char *i, const char *prop, sd_bus_message *reply, void *u, sd_bus_error *e) { (void)b;(void)p;(void)i;(void)prop;(void)e; return sd_bus_message_append(reply, "d", *(double*)u); }
static int property_get_engines(sd_bus *b, const char *p, const char *i, const char *prop, sd_bus_message *reply, void *u, sd_bus_error *e) {
    (void)b;(void)p;(void)i;(void)prop;(void)e;
    struct arc_card *c = u;   // offset 0 for this property -> userdata is the card
    sd_bus_message_open_container(reply, 'a', "(sd)");
    for (int j = 0; j < c->num_engines; j++)
        sd_bus_message_append(reply, "(sd)", c->engines[j].name, c->engines[j].util);
    return sd_bus_message_close_container(reply);
}

// -- DBus methods: userdata is &card --
static int method_set_pl1(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e; struct arc_card *c = u; double watts; sd_bus_message_read(m, "d", &watts);
    int success = 0;
    zes_pwr_handle_t hPower = find_power_domain(c);
    if (hPower) {
        uint32_t n = 0; zesPowerGetLimitsExt(hPower, &n, NULL);
        if (n > 0) {
            zes_power_limit_ext_desc_t *limits = calloc(n, sizeof(*limits));
            for (uint32_t i = 0; i < n; i++) limits[i].stype = ZES_STRUCTURE_TYPE_POWER_LIMIT_EXT_DESC;
            zesPowerGetLimitsExt(hPower, &n, limits);
            for (uint32_t i = 0; i < n; i++) {
                if (limits[i].level == ZES_POWER_LEVEL_SUSTAINED && limits[i].limitUnit == ZES_LIMIT_UNIT_POWER && !limits[i].limitValueLocked) {
                    limits[i].limit = (int32_t)(watts * 1000.0);
                    limits[i].enabled = 1;
                    uint32_t one = 1;
                    if (zesPowerSetLimitsExt(hPower, &one, &limits[i]) == ZE_RESULT_SUCCESS) success = 1;
                    break;
                }
            }
            free(limits);
        }
    }
    if (!success) success = set_power_sysfs(c, watts);
    return sd_bus_reply_method_return(m, "b", success);
}

static int method_lock_clock(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e; struct arc_card *c = u; double mhz; sd_bus_message_read(m, "d", &mhz);
    int success = 0;
    zes_freq_handle_t hFreq = find_gpu_freq_domain(c);
    if (hFreq) {
        zes_freq_properties_t props = { .stype = ZES_STRUCTURE_TYPE_FREQ_PROPERTIES };
        zesFrequencyGetProperties(hFreq, &props);
        if (props.canControl) {
            zes_freq_range_t range = { .min = mhz, .max = mhz };
            if (zesFrequencySetRange(hFreq, &range) == ZE_RESULT_SUCCESS) success = 1;
        }
    }
    if (!success) success = set_clock_sysfs(c, mhz, mhz);
    return sd_bus_reply_method_return(m, "b", success);
}

static int method_reset_clock(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e; struct arc_card *c = u; int success = 0;
    zes_freq_handle_t hFreq = find_gpu_freq_domain(c);
    if (hFreq) {
        zes_freq_range_t range = { .min = -1.0, .max = -1.0 };
        if (zesFrequencySetRange(hFreq, &range) == ZE_RESULT_SUCCESS) success = 1;
    }
    if (!success) success = reset_clock_sysfs(c);
    return sd_bus_reply_method_return(m, "b", success);
}

static int method_set_fan_pwm(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e; struct arc_card *c = u; double pct; sd_bus_message_read(m, "d", &pct);
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    int success = 0;
    if (c->fanCount > 0) {
        zes_fan_speed_t speed = { .speed = (int32_t)pct, .units = ZES_FAN_SPEED_UNITS_PERCENT };
        if (zesFanSetFixedSpeedMode(c->fans[0], &speed) == ZE_RESULT_SUCCESS) success = 1;
    }
    if (!success && c->hfan.pwm_en[0] != '\0') {
        FILE *fe = fopen(c->hfan.pwm_en, "w");
        FILE *fp = fopen(c->hfan.pwm, "w");
        if (fe && fp) { fprintf(fe, "1\n"); fprintf(fp, "%d\n", (int)(pct * 255.0 / 100.0)); success = 1; }
        if (fe) fclose(fe);
        if (fp) fclose(fp);
    }
    return sd_bus_reply_method_return(m, "b", success);
}

static int method_auto_fan(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e; struct arc_card *c = u; int success = 0;
    if (c->fanCount > 0) {
        if (zesFanSetDefaultMode(c->fans[0]) == ZE_RESULT_SUCCESS) success = 1;
    }
    if (!success && c->hfan.pwm_en[0] != '\0') {
        FILE *fe = fopen(c->hfan.pwm_en, "w");
        if (fe) { fprintf(fe, "2\n"); fclose(fe); success = 1; }
    }
    return sd_bus_reply_method_return(m, "b", success);
}

// Offsets are into struct arc_card; each object is registered with its card as
// userdata, so a call/property on /cardN operates on cards[N].
static const sd_bus_vtable arc_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("GpuName", "s", property_get_str, offsetof(struct arc_card, gpu_name), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Architecture", "s", property_get_str, offsetof(struct arc_card, architecture), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ComputeCores", "u", property_get_u32, offsetof(struct arc_card, compute_cores), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("XeCores", "u", property_get_u32, offsetof(struct arc_card, xe_cores), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("RebarStatus", "s", property_get_str, offsetof(struct arc_card, rebar_status), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("VramTotal", "t", property_get_u64, offsetof(struct arc_card, vram_total), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CpuVisibleVram", "t", property_get_u64, offsetof(struct arc_card, cpu_visible_vram), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("VramUsed", "t", property_get_u64, offsetof(struct arc_card, vram_used), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Temperature", "d", property_get_dbl, offsetof(struct arc_card, temperature), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("FanRPM", "u", property_get_u32, offsetof(struct arc_card, fan_rpm), SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Engines", "a(sd)", property_get_engines, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("SetPowerLimit", "d", "b", method_set_pl1, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("LockClock", "d", "b", method_lock_clock, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ResetClock", "", "b", method_reset_clock, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetFanPWM", "d", "b", method_set_fan_pwm, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetFanAuto", "", "b", method_auto_fan, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

// Fill one card's telemetry + Sysman handles (identity already set).
static void init_card(struct arc_card *c) {
    probe_xe_topology(c);
    setup_hwmon_sensors(c);
    setup_pmu_engines(c);

    zes_device_properties_t props = { .stype = ZES_STRUCTURE_TYPE_DEVICE_PROPERTIES };
    zesDeviceGetProperties(c->sysman, &props);
    snprintf(c->gpu_name, sizeof(c->gpu_name), "%s", props.core.name);

    zesDeviceEnumMemoryModules(c->sysman, &c->memCount, NULL);
    if (c->memCount > 0) {
        c->mems = malloc(c->memCount * sizeof(*c->mems));
        zesDeviceEnumMemoryModules(c->sysman, &c->memCount, c->mems);
        zes_mem_state_t mem_state = { .stype = ZES_STRUCTURE_TYPE_MEM_STATE };
        zesMemoryGetState(c->mems[0], &mem_state);
        c->vram_total = mem_state.size;
        c->vram_used = mem_state.size - mem_state.free;
    }

    zesDeviceEnumTemperatureSensors(c->sysman, &c->tempCount, NULL);
    if (c->tempCount > 0) { c->temps = malloc(c->tempCount * sizeof(*c->temps)); zesDeviceEnumTemperatureSensors(c->sysman, &c->tempCount, c->temps); }

    zesDeviceEnumFans(c->sysman, &c->fanCount, NULL);
    if (c->fanCount > 0) { c->fans = malloc(c->fanCount * sizeof(*c->fans)); zesDeviceEnumFans(c->sysman, &c->fanCount, c->fans); }
}

// Enumerate every xe Arc GPU and bundle its four handles by PCI BDF.
static void enumerate_cards(void) {
    uint32_t dCount = 0; zesDriverGet(&dCount, NULL);
    if (dCount == 0) return;
    zes_driver_handle_t *drivers = malloc(dCount * sizeof(zes_driver_handle_t));
    zesDriverGet(&dCount, drivers);
    for (uint32_t i = 0; i < dCount && ncards < MAX_CARDS; i++) {
        uint32_t dc = 0; zesDeviceGet(drivers[i], &dc, NULL);
        if (dc == 0) continue;
        zes_device_handle_t *devs = malloc(dc * sizeof(zes_device_handle_t));
        zesDeviceGet(drivers[i], &dc, devs);
        for (uint32_t j = 0; j < dc && ncards < MAX_CARDS; j++) {
            zes_device_properties_t props = { .stype = ZES_STRUCTURE_TYPE_DEVICE_PROPERTIES };
            zesDeviceGetProperties(devs[j], &props);
            if (props.core.type != ZE_DEVICE_TYPE_GPU) continue;

            zes_pci_properties_t pci = { .stype = ZES_STRUCTURE_TYPE_PCI_PROPERTIES };
            if (zesDevicePciGetProperties(devs[j], &pci) != ZE_RESULT_SUCCESS) continue;

            char bdf[32]; bdf_str(&pci.address, bdf, sizeof(bdf));
            int node = xe_drm_node_for_bdf(bdf);
            if (node < 0) continue;   // not an xe card (e.g. i915 iGPU) — skip

            struct arc_card *c = &cards[ncards];
            memset(c, 0, sizeof(*c));
            c->sysman = devs[j];
            snprintf(c->bdf, sizeof(c->bdf), "%s", bdf);
            c->drm_node = node;
            snprintf(c->render_name, sizeof(c->render_name), "renderD%d", node);
            c->pmu_type = xe_pmu_type_for_bdf(&pci.address);
            snprintf(c->obj_path, sizeof(c->obj_path), "/org/freedesktop/ArcCtrl/card%d", ncards);
            ncards++;
        }
        free(devs);
    }
    free(drivers);
}

int main(void) {
    unsetenv("ZES_ENABLE_SYSMAN");
    if (zesInit(0) != ZE_RESULT_SUCCESS) { fprintf(stderr, "zesInit failed\n"); return EXIT_FAILURE; }

    enumerate_cards();
    if (ncards == 0) { fprintf(stderr, "No xe Arc GPUs found\n"); return EXIT_FAILURE; }
    for (int k = 0; k < ncards; k++) init_card(&cards[k]);

    sd_event *event = NULL; sd_event_default(&event);
    sd_bus *bus = NULL; sd_bus_open_system(&bus);
    sd_bus_attach_event(bus, event, SD_EVENT_PRIORITY_NORMAL);

    // One D-Bus object per card at /org/freedesktop/ArcCtrl/cardN.
    for (int k = 0; k < ncards; k++)
        sd_bus_add_object_vtable(bus, NULL, cards[k].obj_path, "org.freedesktop.ArcCtrl", arc_vtable, &cards[k]);

    sd_bus_request_name(bus, "org.freedesktop.ArcCtrl", 0);

    uint64_t now; sd_event_now(event, CLOCK_MONOTONIC, &now);
    sd_event_add_time(event, NULL, CLOCK_MONOTONIC, now + 500 * 1000, 0, timer_handler, bus);

    fprintf(stderr, "arcd: serving %d card(s):\n", ncards);
    for (int k = 0; k < ncards; k++)
        fprintf(stderr, "  %s -> %s (%s, %s)\n", cards[k].obj_path, cards[k].gpu_name, cards[k].bdf, cards[k].render_name);

    sd_event_loop(event);
    return 0;
}
