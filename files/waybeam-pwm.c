#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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
        "  -v                    Verbose logs\n"
        "\n"
        "Examples:\n"
        "  %s --port 9000 --pwm0-ch 1 --pwm1-ch 2 -v\n"
        "  %s --pwm0-ch 4 --pwm1-ch 0 --center-timeout-ms 500\n",
        argv0, argv0, argv0);
}

static int parse_int(const char *s, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (!s[0] || (end && *end)) return -1;
    *out = (int)v;
    return 0;
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
    char cmd[128];
    uint16_t val = (pwm_ch == 0) ? cfg->mux_pwm0 : cfg->mux_pwm1;
    // BusyBox shell-friendly one-shot
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

    if (sigma_mux_set(cfg, ch) != 0 && cfg->verbose) {
        fprintf(stderr, "WARN: devmem mux set failed for pwm%d (continuing)\n", ch);
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
    if (!o->available) return;
    if (us < cfg->min_us) us = cfg->min_us;
    if (us > cfg->max_us) us = cfg->max_us;
    if (o->last_us == us) return;
    if (write_int_path(o->duty_us_path, us) == 0) {
        o->last_us = us;
    }
}

static void pwm_center_all(const cfg_t *cfg, pwm_out_t *a, pwm_out_t *b) {
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
            // Not a valid frame at this byte offset; slide by one
            i++;
            continue;
        }

        // Valid frame
        (void)sync; // we accept any valid sync/address byte if CRC+length match

        if (type == CRSF_TYPE_RC_CHANNELS_PACKED) {
            if (crsf_unpack_rc16_11bit(payload, payload_len, res->ch_us)) {
                res->got_rc = true;
                if (verbose > 1) {
                    fprintf(stderr, "CRSF RC frame parsed\n");
                }
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
        .mux_pwm0 = 0x1102,
        .mux_pwm1 = 0x1121,
        .mux_reg = "0x1f207994",
    };

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) parse_int(argv[++i], &cfg.port);
        else if (!strcmp(argv[i], "--pwm0-ch") && i + 1 < argc) parse_int(argv[++i], &cfg.pwm0_ch);
        else if (!strcmp(argv[i], "--pwm1-ch") && i + 1 < argc) parse_int(argv[++i], &cfg.pwm1_ch);
        else if (!strcmp(argv[i], "--hz") && i + 1 < argc) parse_int(argv[++i], &cfg.hz);
        else if (!strcmp(argv[i], "--min-us") && i + 1 < argc) parse_int(argv[++i], &cfg.min_us);
        else if (!strcmp(argv[i], "--max-us") && i + 1 < argc) parse_int(argv[++i], &cfg.max_us);
        else if (!strcmp(argv[i], "--center-us") && i + 1 < argc) parse_int(argv[++i], &cfg.center_us);
        else if (!strcmp(argv[i], "--hold-ms") && i + 1 < argc) parse_int(argv[++i], &cfg.hold_ms);
        else if (!strcmp(argv[i], "--center-timeout-ms") && i + 1 < argc) parse_int(argv[++i], &cfg.center_timeout_ms);
        else if (!strcmp(argv[i], "-v")) cfg.verbose++;
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

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

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

        if (pr > 0 && (pfd.revents & POLLIN)) {
            uint8_t dgram[1500];
            ssize_t n = recv(sock, dgram, sizeof(dgram), 0);
            if (n > 0) {
                crsf_stream_feed(&sb, dgram, (size_t)n);

                crsf_parse_result_t res;
                memset(&res, 0, sizeof(res));
                crsf_stream_parse(&sb, &res, cfg.verbose);

                if (res.got_rc) {
                    last_valid_ms = now;
                    link_active = true;
                    centered_due_to_timeout = false;

                    if (cfg.pwm0_ch > 0 && pwm0.available) {
                        int us = res.ch_us[cfg.pwm0_ch - 1];
                        us = clampi(us, cfg.min_us, cfg.max_us);
                        pwm_set_us(&cfg, &pwm0, us);
                    }

                    if (cfg.pwm1_ch > 0 && pwm1.available) {
                        int us = res.ch_us[cfg.pwm1_ch - 1];
                        us = clampi(us, cfg.min_us, cfg.max_us);
                        pwm_set_us(&cfg, &pwm1, us);
                    }

                    if (cfg.verbose > 1) {
                        fprintf(stderr, "RC: ch%02d=%d ch%02d=%d\n",
                                cfg.pwm0_ch, (cfg.pwm0_ch ? res.ch_us[cfg.pwm0_ch - 1] : 0),
                                cfg.pwm1_ch, (cfg.pwm1_ch ? res.ch_us[cfg.pwm1_ch - 1] : 0));
                    }
                }
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
