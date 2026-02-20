#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PWMCHIP "/sys/class/pwm/pwmchip0"
#define MAX_CRSF_FRAME 64
#define RXBUF_SIZE 4096

// CRSF (TBS spec)
#define CRSF_ADDR_FLIGHT_CONTROLLER 0xC8
#define CRSF_TYPE_RC_CHANNELS_PACKED 0x16

static volatile sig_atomic_t g_stop = 0;

typedef struct {
    int port;              // UDP listen port
    int pwm0_ch;           // CRSF channel index 1..16, or 0 disabled
    int pwm1_ch;           // CRSF channel index 1..16, or 0 disabled
    int hz;                // PWM frequency
    int min_us;            // clamp min
    int max_us;            // clamp max
    int center_us;         // failsafe center
    int hold_ms;           // hold last command before centering
    int center_timeout_ms; // center after no packets
    int verbose;
    bool no_mux;
    bool mux_init_once;
    uint16_t mux_init_val;
    // SigmaStar mux values from your testing
    uint16_t mux_pwm0;     // 0x1102
    uint16_t mux_pwm1;     // 0x1121
    const char *mux_reg;   // "0x1f207994"
} cfg_t;

typedef struct {
    int ch;                 // pwm index 0 or 1
    char path[128];
    char duty_us_path[160];
    char duty_pct_path[160];
    char period_path[160];
    char enable_path[160];
    char polarity_path[160];
    int fd_duty_us;
    int fd_period;
    int fd_enable;
    int last_us;
    bool available;
    bool enabled;
} pwm_out_t;

typedef struct {
    uint8_t data[RXBUF_SIZE];
    size_t len;
} stream_buf_t;

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void on_sig(int sig) {
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  --port N              UDP port (default 9000)\n"
        "  --pwm0-ch N           Map CRSF channel N (1..16) to pwm0 (default 1)\n"
        "  --pwm1-ch N           Map CRSF channel N (1..16) to pwm1 (default 2)\n"
        "  --hz N                PWM frequency Hz (default 50)\n"
        "  --min-us N            Clamp min output us (default 1000)\n"
        "  --max-us N            Clamp max output us (default 2000)\n"
        "  --center-us N         Center/failsafe us (default 1500)\n"
        "  --hold-ms N           Hold last value after link loss (default 300)\n"
        "  --center-timeout-ms N Center outputs after no valid frame (default 500)\n"
        "  --no-mux              Do not write pin mux register (external setup)\n"
        "  --mux-reg ADDR        Mux register address (default 0x1f207994)\n"
        "  --mux-pwm0 VAL        Mux write value for pwm0 init (default 0x1102)\n"
        "  --mux-pwm1 VAL        Mux write value for pwm1 init (default 0x1121)\n"
        "  --mux-init-val VAL    One-shot mux write at startup; skips per-channel mux writes\n"
        "                        (default auto for dual-channel: 0x1122)\n"
        "  -v                    Verbose logs (packet + state)\n"
        "  -vv                   More detail (frame counters + output updates)\n"
        "  -vvv                  Very verbose (unchanged output skips)\n"
        "\n"
        "Examples:\n"
        "  %s --port 9000 --pwm0-ch 1 --pwm1-ch 2 -v\n"
        "  %s --pwm0-ch 4 --pwm1-ch 0 --center-timeout-ms 500\n"
        "  %s --no-mux --pwm0-ch 1 --pwm1-ch 2 -vv\n"
        "  %s --mux-reg 0x1f207994 --mux-pwm0 0x1102 --mux-pwm1 0x1121\n"
        "  %s --mux-init-val 0x1122 --pwm0-ch 1 --pwm1-ch 2 -vv\n",
        argv0, argv0, argv0, argv0, argv0, argv0);
}

static int parse_int(const char *s, int *out) {
    if (!s || !out) return -1;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 0);
    if (!s[0] || end == s || (end && *end) || errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static bool parse_opt_int_or_die(int argc, char **argv, int *i, int *dst, const char *opt) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "Missing value for %s\n", opt);
        return false;
    }

    const char *val = argv[++(*i)];
    if (parse_int(val, dst) != 0) {
        fprintf(stderr, "Invalid value for %s: %s\n", opt, val);
        return false;
    }
    return true;
}

static bool parse_opt_u16_or_die(int argc, char **argv, int *i, uint16_t *dst, const char *opt) {
    int tmp = 0;
    if (!parse_opt_int_or_die(argc, argv, i, &tmp, opt)) {
        return false;
    }
    if (tmp < 0 || tmp > 0xFFFF) {
        fprintf(stderr, "Out of range for %s: %d (expected 0..65535)\n", opt, tmp);
        return false;
    }
    *dst = (uint16_t)tmp;
    return true;
}

static int write_str(const char *path, const char *s) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, s, strlen(s));
    int saved = errno;
    close(fd);
    errno = saved;
    return (n == (ssize_t)strlen(s)) ? 0 : -1;
}

static int write_int_path(const char *path, int v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", v);
    return write_str(path, buf);
}

static bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int export_pwm_if_needed(int ch) {
    char p[128];
    snprintf(p, sizeof(p), PWMCHIP "/pwm%d", ch);
    if (path_exists(p)) return 0;
    return write_int_path(PWMCHIP "/export", ch);
}

static int sigma_mux_set(const cfg_t *cfg, int pwm_ch) {
    if (cfg->no_mux) {
        return 0;
    }
    char cmd[128];
    uint16_t val = (pwm_ch == 0) ? cfg->mux_pwm0 : cfg->mux_pwm1;
    // BusyBox shell-friendly one-shot
    snprintf(cmd, sizeof(cmd), "devmem %s 16 0x%04x >/dev/null 2>&1", cfg->mux_reg, val);
    return system(cmd);
}

static int sigma_mux_set_value(const cfg_t *cfg, uint16_t val) {
    if (cfg->no_mux) {
        return 0;
    }
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "devmem %s 16 0x%04x >/dev/null 2>&1", cfg->mux_reg, val);
    return system(cmd);
}

static int pwm_init_one(const cfg_t *cfg, pwm_out_t *o, int ch) {
    memset(o, 0, sizeof(*o));
    o->ch = ch;
    o->fd_duty_us = -1;
    o->fd_period = -1;
    o->fd_enable = -1;
    o->last_us = -1;

    snprintf(o->path, sizeof(o->path), PWMCHIP "/pwm%d", ch);
    snprintf(o->duty_us_path, sizeof(o->duty_us_path), "%s/duty_us", o->path);
    snprintf(o->duty_pct_path, sizeof(o->duty_pct_path), "%s/duty_cycle", o->path);
    snprintf(o->period_path, sizeof(o->period_path), "%s/period", o->path);
    snprintf(o->enable_path, sizeof(o->enable_path), "%s/enable", o->path);
    snprintf(o->polarity_path, sizeof(o->polarity_path), "%s/polarity", o->path);

    if (cfg->no_mux) {
        if (cfg->verbose) {
            fprintf(stderr, "MUX: skipping write for pwm%d (--no-mux)\n", ch);
        }
    } else if (cfg->mux_init_once) {
        if (cfg->verbose > 1) {
            fprintf(stderr, "MUX: per-channel write skipped for pwm%d (--mux-init-val active)\n", ch);
        }
    } else if (sigma_mux_set(cfg, ch) != 0 && cfg->verbose) {
        fprintf(stderr, "WARN: devmem mux set failed for pwm%d (continuing)\n", ch);
    } else if (cfg->verbose > 1) {
        uint16_t val = (ch == 0) ? cfg->mux_pwm0 : cfg->mux_pwm1;
        fprintf(stderr, "MUX: pwm%d -> %s = 0x%04x\n", ch, cfg->mux_reg, val);
    }

    if (export_pwm_if_needed(ch) != 0 && !path_exists(o->path)) {
        perror("export pwm");
        return -1;
    }

    // Must have duty_us for fine control (your patched driver)
    if (!path_exists(o->duty_us_path)) {
        fprintf(stderr, "ERROR: %s missing (driver patch not present?)\n", o->duty_us_path);
        return -1;
    }

    // Disable -> set period (Hz on this SigmaStar BSP) -> set center -> enable
    (void)write_int_path(o->enable_path, 0);
    if (write_int_path(o->period_path, cfg->hz) != 0) {
        perror("write period");
        return -1;
    }
    if (write_int_path(o->duty_us_path, cfg->center_us) != 0) {
        perror("write duty_us center");
        return -1;
    }
    if (write_int_path(o->enable_path, 1) != 0) {
        perror("enable pwm");
        return -1;
    }
    o->enabled = true;
    o->last_us = cfg->center_us;
    o->available = true;

    if (cfg->verbose) {
        fprintf(stderr, "PWM%d ready: period=%dHz center=%dus (%s)\n",
                ch, cfg->hz, cfg->center_us, o->duty_us_path);
    }
    return 0;
}

static void pwm_set_us(const cfg_t *cfg, pwm_out_t *o, int us) {
    int requested_us = us;
    if (!o->available) return;
    if (us < cfg->min_us) us = cfg->min_us;
    if (us > cfg->max_us) us = cfg->max_us;
    if (o->last_us == us) {
        if (cfg->verbose > 2) {
            fprintf(stderr, "PWM%d unchanged: duty_us=%d\n", o->ch, us);
        }
        return;
    }
    if (write_int_path(o->duty_us_path, us) == 0) {
        if (cfg->verbose > 1) {
            if (requested_us != us) {
                fprintf(stderr, "PWM%d <- %dus (clamped from %dus)\n", o->ch, us, requested_us);
            } else {
                fprintf(stderr, "PWM%d <- %dus\n", o->ch, us);
            }
        }
        o->last_us = us;
    } else if (cfg->verbose) {
        fprintf(stderr, "PWM%d write failed for %s=%d: %s\n",
                o->ch, o->duty_us_path, us, strerror(errno));
    }
}

static void pwm_center_all(const cfg_t *cfg, pwm_out_t *a, pwm_out_t *b) {
    if (cfg->verbose) {
        fprintf(stderr, "Centering PWM outputs to %dus\n", cfg->center_us);
    }
    pwm_set_us(cfg, a, cfg->center_us);
    pwm_set_us(cfg, b, cfg->center_us);
}

// CRC8 poly 0xD5 (CRSF spec)
static uint8_t crsf_crc8(const uint8_t *buf, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0xD5) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static int crsf_ticks_to_us(int ticks) {
    // TBS spec macro: TICKS_TO_US(x) ((x - 992) * 5 / 8 + 1500)
    return ((ticks - 992) * 5) / 8 + 1500;
}

static bool crsf_unpack_rc16_11bit(const uint8_t *payload, size_t len, int out_us[16]) {
    if (len < 22) return false; // 16 * 11 bits = 176 bits = 22 bytes
    for (int ch = 0; ch < 16; ch++) {
        int bitpos = ch * 11;
        int bytepos = bitpos >> 3;
        int shift = bitpos & 7;
        uint32_t w = 0;
        // Need up to 3 bytes
        w |= (uint32_t)payload[bytepos];
        if (bytepos + 1 < (int)len) w |= (uint32_t)payload[bytepos + 1] << 8;
        if (bytepos + 2 < (int)len) w |= (uint32_t)payload[bytepos + 2] << 16;
        int ticks = (int)((w >> shift) & 0x7FF);
        out_us[ch] = crsf_ticks_to_us(ticks);
    }
    return true;
}

typedef struct {
    bool got_rc;
    int ch_us[16];
    size_t frames_seen;
    size_t frames_crc_ok;
    size_t frames_bad_crc;
    size_t frames_bad_addr;
    size_t rc_frames;
} crsf_parse_result_t;

// Feed arbitrary bytes (UDP payload may contain partial/multiple frames)
static void crsf_stream_feed(stream_buf_t *sb, const uint8_t *data, size_t n) {
    if (n == 0) return;
    if (n > RXBUF_SIZE) {
        data += (n - RXBUF_SIZE);
        n = RXBUF_SIZE;
    }
    if (sb->len + n > RXBUF_SIZE) {
        size_t drop = (sb->len + n) - RXBUF_SIZE;
        memmove(sb->data, sb->data + drop, sb->len - drop);
        sb->len -= drop;
    }
    memcpy(sb->data + sb->len, data, n);
    sb->len += n;
}

static void crsf_stream_parse(stream_buf_t *sb, crsf_parse_result_t *res, int verbose) {
    size_t i = 0;
    while (sb->len - i >= 4) { // sync + len + type + crc(min)
        uint8_t sync = sb->data[i + 0];
        uint8_t flen = sb->data[i + 1];
        res->frames_seen++;

        // RC data should target flight-controller address.
        if (sync != CRSF_ADDR_FLIGHT_CONTROLLER) {
            res->frames_bad_addr++;
            i++;
            continue;
        }

        // Per spec: valid frame length field is 2..62
        if (flen < 2 || flen > 62) {
            i++;
            continue;
        }

        size_t total = (size_t)flen + 2; // includes sync+len
        if (sb->len - i < total) break;  // wait for more bytes

        const uint8_t *f = &sb->data[i];
        uint8_t type = f[2];
        const uint8_t *payload = &f[3];
        size_t payload_len = (size_t)flen - 2; // type + payload + crc => payload = flen - 2
        uint8_t crc_rx = f[total - 1];
        uint8_t crc_calc = crsf_crc8(&f[2], (size_t)flen - 1); // type + payload

        if (crc_calc != crc_rx) {
            res->frames_bad_crc++;
            // Not a valid frame at this byte offset; slide by one
            i++;
            continue;
        }
        res->frames_crc_ok++;

        if (type == CRSF_TYPE_RC_CHANNELS_PACKED) {
            if (payload_len == 22 && crsf_unpack_rc16_11bit(payload, payload_len, res->ch_us)) {
                res->got_rc = true;
                res->rc_frames++;
                if (verbose > 1) {
                    fprintf(stderr, "CRSF RC frame parsed\n");
                }
            } else if (verbose > 1) {
                fprintf(stderr, "CRSF RC frame ignored: invalid payload_len=%zu\n", payload_len);
            }
        }

        i += total;
    }

    if (i > 0) {
        memmove(sb->data, sb->data + i, sb->len - i);
        sb->len -= i;
    }
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void log_udp_rx(int verbose, ssize_t n, const struct sockaddr_in *src, const crsf_parse_result_t *res) {
    char ipbuf[INET_ADDRSTRLEN] = "?";
    if (src) {
        (void)inet_ntop(AF_INET, &src->sin_addr, ipbuf, sizeof(ipbuf));
    }
    unsigned int port = src ? (unsigned int)ntohs(src->sin_port) : 0U;

    if (verbose > 1) {
        fprintf(stderr,
                "UDP rx: %zd bytes from %s:%u | frames=%zu crc_ok=%zu rc=%zu bad_addr=%zu bad_crc=%zu\n",
                n, ipbuf, port,
                res->frames_seen, res->frames_crc_ok, res->rc_frames,
                res->frames_bad_addr, res->frames_bad_crc);
    } else {
        fprintf(stderr, "UDP rx: %zd bytes from %s:%u%s\n",
                n, ipbuf, port, res->got_rc ? " (RC update)" : " (no RC)");
    }
}

int main(int argc, char **argv) {
    cfg_t cfg = {
        .port = 9000,
        .pwm0_ch = 1,   // CRSF CH1 -> pwm0
        .pwm1_ch = 2,   // CRSF CH2 -> pwm1
        .hz = 50,
        .min_us = 1000,
        .max_us = 2000,
        .center_us = 1500,
        .hold_ms = 300,
        .center_timeout_ms = 500,
        .verbose = 0,
        .no_mux = false,
        .mux_init_once = false,
        .mux_init_val = 0,
        .mux_pwm0 = 0x1102,
        .mux_pwm1 = 0x1121,
        .mux_reg = "0x1f207994",
    };
    bool mux_strategy_explicit = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port")) {
            if (!parse_opt_int_or_die(argc, argv, &i, &cfg.port, "--port")) return 1;
        } else if (!strcmp(argv[i], "--pwm0-ch")) {
            if (!parse_opt_int_or_die(argc, argv, &i, &cfg.pwm0_ch, "--pwm0-ch")) return 1;
        } else if (!strcmp(argv[i], "--pwm1-ch")) {
            if (!parse_opt_int_or_die(argc, argv, &i, &cfg.pwm1_ch, "--pwm1-ch")) return 1;
        } else if (!strcmp(argv[i], "--hz")) {
            if (!parse_opt_int_or_die(argc, argv, &i, &cfg.hz, "--hz")) return 1;
        } else if (!strcmp(argv[i], "--min-us")) {
            if (!parse_opt_int_or_die(argc, argv, &i, &cfg.min_us, "--min-us")) return 1;
        } else if (!strcmp(argv[i], "--max-us")) {
            if (!parse_opt_int_or_die(argc, argv, &i, &cfg.max_us, "--max-us")) return 1;
        } else if (!strcmp(argv[i], "--center-us")) {
            if (!parse_opt_int_or_die(argc, argv, &i, &cfg.center_us, "--center-us")) return 1;
        } else if (!strcmp(argv[i], "--hold-ms")) {
            if (!parse_opt_int_or_die(argc, argv, &i, &cfg.hold_ms, "--hold-ms")) return 1;
        } else if (!strcmp(argv[i], "--center-timeout-ms")) {
            if (!parse_opt_int_or_die(argc, argv, &i, &cfg.center_timeout_ms, "--center-timeout-ms")) return 1;
        } else if (!strcmp(argv[i], "--no-mux")) {
            cfg.no_mux = true;
            mux_strategy_explicit = true;
        } else if (!strcmp(argv[i], "--mux-reg")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Missing value for --mux-reg\n");
                return 1;
            }
            cfg.mux_reg = argv[++i];
        } else if (!strcmp(argv[i], "--mux-pwm0")) {
            if (!parse_opt_u16_or_die(argc, argv, &i, &cfg.mux_pwm0, "--mux-pwm0")) return 1;
            cfg.mux_init_once = false;
            mux_strategy_explicit = true;
        } else if (!strcmp(argv[i], "--mux-pwm1")) {
            if (!parse_opt_u16_or_die(argc, argv, &i, &cfg.mux_pwm1, "--mux-pwm1")) return 1;
            cfg.mux_init_once = false;
            mux_strategy_explicit = true;
        } else if (!strcmp(argv[i], "--mux-init-val")) {
            if (!parse_opt_u16_or_die(argc, argv, &i, &cfg.mux_init_val, "--mux-init-val")) return 1;
            cfg.mux_init_once = true;
            mux_strategy_explicit = true;
        }
        else if (argv[i][0] == '-' && argv[i][1] == 'v') {
            const char *p = &argv[i][1];
            while (*p == 'v') {
                cfg.verbose++;
                p++;
            }
            if (*p != '\0') {
                usage(argv[0]);
                return 1;
            }
        }
        else {
            usage(argv[0]);
            return 1;
        }
    }

    if (cfg.port <= 0 || cfg.port > 65535 ||
        cfg.hz <= 0 ||
        cfg.min_us < 500 || cfg.max_us > 2500 ||
        cfg.center_us < cfg.min_us || cfg.center_us > cfg.max_us ||
        cfg.hold_ms < 0 || cfg.center_timeout_ms < cfg.hold_ms ||
        cfg.pwm0_ch < 0 || cfg.pwm0_ch > 16 ||
        cfg.pwm1_ch < 0 || cfg.pwm1_ch > 16) {
        fprintf(stderr, "Invalid arguments\n");
        return 1;
    }

    // Default for known board behavior: dual-channel works with one combined mux write.
    if (!cfg.no_mux && !mux_strategy_explicit && cfg.pwm0_ch > 0 && cfg.pwm1_ch > 0) {
        cfg.mux_init_once = true;
        cfg.mux_init_val = 0x1122;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (!cfg.no_mux && cfg.mux_init_once) {
        if (sigma_mux_set_value(&cfg, cfg.mux_init_val) != 0) {
            if (cfg.verbose) {
                fprintf(stderr, "WARN: one-shot mux write failed for %s=0x%04x (continuing)\n",
                        cfg.mux_reg, cfg.mux_init_val);
            }
        } else if (cfg.verbose) {
            fprintf(stderr, "MUX: one-shot write %s = 0x%04x\n", cfg.mux_reg, cfg.mux_init_val);
        }
    }

    pwm_out_t pwm0 = {0}, pwm1 = {0};

    if (cfg.pwm0_ch > 0 && pwm_init_one(&cfg, &pwm0, 0) != 0) return 1;
    if (cfg.pwm1_ch > 0 && pwm_init_one(&cfg, &pwm1, 1) != 0) return 1;

    // Start centered (safe startup)
    pwm_center_all(&cfg, &pwm0, &pwm1);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)cfg.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    if (cfg.verbose) {
        fprintf(stderr,
                "Listening UDP :%d | pwm0<-CH%d pwm1<-CH%d | %dHz | clamp %d..%dus | center %dus | hold %dms center@%dms\n",
                cfg.port, cfg.pwm0_ch, cfg.pwm1_ch, cfg.hz, cfg.min_us, cfg.max_us, cfg.center_us,
                cfg.hold_ms, cfg.center_timeout_ms);
        if (cfg.no_mux) {
            fprintf(stderr, "MUX mode: disabled (--no-mux)\n");
        } else if (cfg.mux_init_once) {
            fprintf(stderr, "MUX mode: one-shot via %s = 0x%04x\n",
                    cfg.mux_reg, cfg.mux_init_val);
        } else {
            fprintf(stderr, "MUX mode: per-channel writes via %s (pwm0=0x%04x pwm1=0x%04x)\n",
                    cfg.mux_reg, cfg.mux_pwm0, cfg.mux_pwm1);
        }
    }

    struct pollfd pfd = { .fd = sock, .events = POLLIN, .revents = 0 };
    stream_buf_t sb = { .len = 0 };
    uint64_t last_valid_ms = 0;
    bool link_active = false;
    bool centered_due_to_timeout = true; // already centered at startup

    while (!g_stop) {
        int pr = poll(&pfd, 1, 20); // 20ms tick
        uint64_t now = mono_ms();

        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            if (cfg.verbose) fprintf(stderr, "Socket error revents=0x%x, centering outputs\n", pfd.revents);
            pwm_center_all(&cfg, &pwm0, &pwm1);
            break;
        }

        if (pr > 0 && (pfd.revents & POLLIN)) {
            uint8_t dgram[1500];
            struct sockaddr_in src;
            socklen_t src_len = sizeof(src);
            memset(&src, 0, sizeof(src));
            ssize_t n = recvfrom(sock, dgram, sizeof(dgram), 0, (struct sockaddr *)&src, &src_len);
            if (n > 0) {
                crsf_stream_feed(&sb, dgram, (size_t)n);

                crsf_parse_result_t res;
                memset(&res, 0, sizeof(res));
                crsf_stream_parse(&sb, &res, cfg.verbose);
                if (cfg.verbose) {
                    log_udp_rx(cfg.verbose, n, &src, &res);
                }

                if (res.got_rc) {
                    if (centered_due_to_timeout && cfg.verbose) {
                        fprintf(stderr, "Link recovered: valid RC frame received\n");
                    }
                    last_valid_ms = now;
                    link_active = true;
                    centered_due_to_timeout = false;

                    if (cfg.pwm0_ch > 0 && pwm0.available) {
                        int raw_us = res.ch_us[cfg.pwm0_ch - 1];
                        int clamped_us = clampi(raw_us, cfg.min_us, cfg.max_us);
                        if (cfg.verbose > 1) {
                            fprintf(stderr, "Map: CH%d=%dus -> PWM0=%dus\n", cfg.pwm0_ch, raw_us, clamped_us);
                        }
                        pwm_set_us(&cfg, &pwm0, clamped_us);
                    }

                    if (cfg.pwm1_ch > 0 && pwm1.available) {
                        int raw_us = res.ch_us[cfg.pwm1_ch - 1];
                        int clamped_us = clampi(raw_us, cfg.min_us, cfg.max_us);
                        if (cfg.verbose > 1) {
                            fprintf(stderr, "Map: CH%d=%dus -> PWM1=%dus\n", cfg.pwm1_ch, raw_us, clamped_us);
                        }
                        pwm_set_us(&cfg, &pwm1, clamped_us);
                    }

                    if (cfg.verbose > 1) {
                        fprintf(stderr, "RC: ch%02d=%d ch%02d=%d\n",
                                cfg.pwm0_ch, (cfg.pwm0_ch ? res.ch_us[cfg.pwm0_ch - 1] : 0),
                                cfg.pwm1_ch, (cfg.pwm1_ch ? res.ch_us[cfg.pwm1_ch - 1] : 0));
                    }
                }
            } else if (n == 0) {
                if (cfg.verbose > 1) {
                    fprintf(stderr, "recvfrom returned 0 bytes\n");
                }
            } else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (cfg.verbose) perror("recv");
                // On socket receive errors, stop driving stale outputs.
                if (!centered_due_to_timeout) {
                    pwm_center_all(&cfg, &pwm0, &pwm1);
                    centered_due_to_timeout = true;
                }
                link_active = false;
                sb.len = 0;
            }
        }

        // Failsafe logic:
        // 0..hold_ms after last frame: hold last command
        // >= center_timeout_ms: center outputs
        if (link_active) {
            uint64_t age = now - last_valid_ms;
            if ((int)age >= cfg.center_timeout_ms) {
                if (!centered_due_to_timeout) {
                    if (cfg.verbose) {
                        fprintf(stderr, "FAILSAFE: no valid CRSF for %llums -> center outputs\n",
                                (unsigned long long)age);
                    }
                    pwm_center_all(&cfg, &pwm0, &pwm1);
                    centered_due_to_timeout = true;
                }
            } else if ((int)age >= cfg.hold_ms) {
                // Stage-1-like hold period has elapsed; still waiting to center at center_timeout_ms
                // We intentionally do nothing here (holding last values).
            }
        }
    }

    if (cfg.verbose) fprintf(stderr, "Stopping, centering outputs...\n");
    pwm_center_all(&cfg, &pwm0, &pwm1);
    close(sock);
    return 0;
}
