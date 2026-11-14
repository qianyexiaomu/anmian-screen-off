#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

#define MAX_PKG_LEN 256
#define CMD_BUFFER_SIZE 8192
#define MAX_APPS 100
#define CONFIG_LINE_MAX 512
#define CONFIG_CHECK_INTERVAL 10
#define MAX_LOG_SIZE (100 * 1024)
#define SCREEN_CHECK_INTERVAL 3
#define SCREEN_OFF_VERIFY_MS 300
#define SCREEN_OFF_VERIFY_TRIES 15
#define TOUCH_WAIT_MS 2000
#define MIN_LOOP_INTERVAL_MS 500
#define MAX_DEVICES 16
#define SLEEP_SCREEN_OFF_MS 3000
#define SLEEP_SCREEN_OFF_NO_BL_MS 15000
#define SLEEP_NO_TOUCH_SOURCE_MS 2000
#define LOG_SUPPRESS_INTERVAL 60
#define TOP_PKG_CACHE_SECONDS 5
#define DEFAULT_LIMIT_SECONDS (30 * 60)
#define MAX_TIME_VALUE 100000

#define RESET_GRACE_SECONDS 30

#define MY_NBITS(x) ((((x) - 1) / (int)(sizeof(long) * 8)) + 1)
#define BIT_TEST(nr, addr) (((1UL << ((nr) % (sizeof(long) * 8))) & (addr)[(nr) / (sizeof(long) * 8)]) != 0)

#define LOG_I(fmt, ...) do { write_log("[I] " fmt, ##__VA_ARGS__); } while (0)
#define LOG_W(fmt, ...) do { write_log("[W] " fmt, ##__VA_ARGS__); } while (0)
#define LOG_E(fmt, ...) do { write_log("[E] " fmt, ##__VA_ARGS__); } while (0)

typedef struct {
    char package[MAX_PKG_LEN];
    long limit_time;
    time_t start_time;
    time_t last_touch_time;
    time_t absent_since;
    bool screen_off_triggered;
} AppConfig;

static int epfd = -1;
static int touch_fds[MAX_DEVICES];
static int touch_fd_count = 0;
static FILE* getevent_pipe = NULL;
static int getevent_fd = -1;
static bool use_getevent = false;
static bool device_lost = false;

static bool touch_watch_ok = true;
static bool touch_warned = false;

static bool log_enabled = false;
static FILE* log_fp = NULL;
static char log_file_path[256] = {0};

static AppConfig apps[MAX_APPS];
static int app_count = 0;
static time_t last_config_mtime = 0;
static off_t last_config_size = -1;

static char cached_top_pkg[MAX_PKG_LEN] = {0};
static time_t last_top_pkg_time = 0;
static int preferred_cmd = 0;

static bool screen_state_cached = true;
static time_t last_screen_check = 0;
static char backlight_path[256] = {0};
static bool backlight_probed = false;

static bool backlight_trusted = true;

static time_t last_cmd_fail_log = 0;
static volatile sig_atomic_t stop_requested = 0;

static time_t mono_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return time(NULL);
    return (time_t)ts.tv_sec;
}

static void write_log(const char* fmt, ...);

static void handle_signal(int sig) {
    (void)sig;
    stop_requested = 1;
}

static void check_log_size(void) {
    if (!log_file_path[0]) return;
    struct stat st;
    if (stat(log_file_path, &st) != 0) return;
    if (st.st_size < MAX_LOG_SIZE) return;
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
    FILE* trunc = fopen(log_file_path, "w");
    if (trunc) fclose(trunc);
}

static void ensure_log_file(void) {
    if (log_fp || !log_file_path[0]) return;
    log_fp = fopen(log_file_path, "a");
}

static void write_log(const char* fmt, ...) {
    if (!log_enabled) return;
    check_log_size();
    ensure_log_file();
    if (!log_fp) return;

    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    } else {
        snprintf(ts, sizeof(ts), "----/--/-- --:--:--");
    }

    va_list args;
    va_start(args, fmt);
    fprintf(log_fp, "[%s] ", ts);
    vfprintf(log_fp, fmt, args);
    fputc('\n', log_fp);
    fflush(log_fp);
    va_end(args);
}

static int exec_cmd(const char* cmd, char* buf, size_t buf_size) {
    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        time_t now = mono_now();
        if (now - last_cmd_fail_log >= LOG_SUPPRESS_INTERVAL) {
            LOG_E("执行命令失败(%s): %s", strerror(errno), cmd);
            last_cmd_fail_log = now;
        }
        return -1;
    }

    size_t total = 0;
    if (buf && buf_size) buf[0] = '\0';
    if (buf && buf_size > 1) {
        while (total < buf_size - 1) {
            size_t n = fread(buf + total, 1, buf_size - 1 - total, pipe);
            if (n == 0) break;
            total += n;
        }
        buf[total] = '\0';
    }

    char drain[512];
    while (fread(drain, 1, sizeof(drain), pipe) > 0) {               }

    int status = pclose(pipe);
    if (!buf) {

        return (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
    }
    return total > 0 ? 0 : -1;
}

static bool is_pkg_char(char c) {
    return isalnum((unsigned char)c) || c == '.' || c == '_';
}

static bool extract_package(const char* s, char* out, size_t out_size) {
    if (!s || !out || out_size < 4) return false;
    while (*s) {
        if (isalpha((unsigned char)*s) || *s == '_') {
            const char* start = s;
            while (*s && is_pkg_char(*s)) s++;
            size_t len = (size_t)(s - start);
            char term = *s;
            bool ok_term = (term == '/' || term == ' ' || term == '}' || term == '\0' ||
                            term == '\n' || term == '\r' || term == '\t' || term == ',' ||
                            term == ':');
            if (len >= 3 && len < out_size && ok_term && memchr(start, '.', len) != NULL) {
                memcpy(out, start, len);
                out[len] = '\0';
                return true;
            }
            if (term == '\0') break;
        } else {
            s++;
        }
    }
    return false;
}

typedef struct {
    const char* cmd;
    const char* keyword;
} TopCmd;

static const TopCmd top_cmds[] = {

    { "/system/bin/dumpsys window 2>/dev/null | grep -m1 mCurrentFocus", "mCurrentFocus" },
    { "/system/bin/dumpsys window windows 2>/dev/null | grep -m1 mCurrentFocus", "mCurrentFocus" },
    { "/system/bin/dumpsys window displays 2>/dev/null | grep -m1 -i mFocusedApp", "mFocusedApp" },
    { "/system/bin/dumpsys activity activities 2>/dev/null | grep -m1 -i topResumedActivity", "topResumedActivity" },
    { "/system/bin/dumpsys activity activities 2>/dev/null | grep -m1 -i mResumedActivity", "mResumedActivity" },
    { "/system/bin/dumpsys activity 2>/dev/null | grep -m1 -i mResumedActivity", "mResumedActivity" },
};

#define TOP_CMD_COUNT ((int)(sizeof(top_cmds) / sizeof(top_cmds[0])))

static bool get_top_app_name(char* buf, size_t buf_size) {
    if (!buf || buf_size < MAX_PKG_LEN) return false;

    time_t now = mono_now();
    if (cached_top_pkg[0] && now - last_top_pkg_time < TOP_PKG_CACHE_SECONDS) {
        snprintf(buf, buf_size, "%s", cached_top_pkg);
        return true;
    }

    for (int k = 0; k < TOP_CMD_COUNT; k++) {
        int i = (preferred_cmd + k) % TOP_CMD_COUNT;
        char out[CMD_BUFFER_SIZE];
        if (exec_cmd(top_cmds[i].cmd, out, sizeof(out)) != 0) continue;

        const char* p = strstr(out, top_cmds[i].keyword);
        if (!p) continue;
        p += strlen(top_cmds[i].keyword);

        char pkg[MAX_PKG_LEN];
        if (!extract_package(p, pkg, sizeof(pkg))) continue;

        snprintf(cached_top_pkg, sizeof(cached_top_pkg), "%s", pkg);
        last_top_pkg_time = now;
        preferred_cmd = i;
        snprintf(buf, buf_size, "%s", pkg);
        return true;
    }
    return false;
}

static bool is_system_overlay(const char* pkg) {
    if (!pkg || !*pkg) return true;
    static const char* const patterns[] = {
        "com.android.systemui",
        "inputmethod",
        "permissioncontroller",
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
        if (strstr(pkg, patterns[i])) return true;
    }
    return strcmp(pkg, "android") == 0;
}

static bool app_is_foreground(const char* pkg) {
    char cur[MAX_PKG_LEN];
    last_top_pkg_time = 0;
    if (!get_top_app_name(cur, sizeof(cur))) return true;
    return strcmp(cur, pkg) == 0;
}

static bool ensure_epfd(void) {
    if (epfd >= 0) return true;
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) epfd = epoll_create(1);
    if (epfd < 0) LOG_E("创建 epoll 失败: %s", strerror(errno));
    return epfd >= 0;
}

static bool find_touch_devices(void) {
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        LOG_W("无法打开 /dev/input: %s", strerror(errno));
        return false;
    }

    touch_fd_count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && touch_fd_count < MAX_DEVICES) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        char path[256];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        char name[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0 || strstr(name, "uinput")) {
            close(fd);
            continue;
        }

        unsigned long abs_bits[MY_NBITS(ABS_MAX)];
        memset(abs_bits, 0, sizeof(abs_bits));
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
            close(fd);
            continue;
        }

        if (BIT_TEST(ABS_MT_POSITION_X, abs_bits) || BIT_TEST(ABS_MT_POSITION_Y, abs_bits) ||
            BIT_TEST(ABS_MT_TRACKING_ID, abs_bits) || BIT_TEST(ABS_X, abs_bits) ||
            BIT_TEST(ABS_Y, abs_bits)) {
            touch_fds[touch_fd_count++] = fd;
            continue;
        }
        close(fd);
    }
    closedir(dir);
    return touch_fd_count > 0;
}

static void cleanup_touch_devices(void) {
    for (int i = 0; i < touch_fd_count; i++) {
        if (epfd >= 0) epoll_ctl(epfd, EPOLL_CTL_DEL, touch_fds[i], NULL);
        close(touch_fds[i]);
    }
    touch_fd_count = 0;

    if (getevent_fd >= 0) {
        if (epfd >= 0) epoll_ctl(epfd, EPOLL_CTL_DEL, getevent_fd, NULL);
        pclose(getevent_pipe);
        getevent_pipe = NULL;
        getevent_fd = -1;
    }
    use_getevent = false;
    device_lost = false;
}

static bool init_touch_devices(void) {
    if (touch_fd_count > 0) return true;
    if (!ensure_epfd()) return false;

    if (!find_touch_devices()) {
        use_getevent = true;
        return false;
    }

    struct epoll_event ev;
    for (int i = 0; i < touch_fd_count; i++) {
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = touch_fds[i];
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, touch_fds[i], &ev) < 0) {
            LOG_W("注册输入设备失败: %s", strerror(errno));
            for (int j = 0; j < touch_fd_count; j++) close(touch_fds[j]);
            touch_fd_count = 0;
            use_getevent = true;
            return false;
        }
    }
    use_getevent = false;
    LOG_I("已监听 %d 个触摸设备", touch_fd_count);
    return true;
}

static bool init_getevent(void) {
    if (getevent_fd >= 0) return true;
    if (!ensure_epfd()) return false;

    if (!getevent_pipe) {
        getevent_pipe = popen("/system/bin/getevent -q", "r");
        if (!getevent_pipe) {
            LOG_W("无法启动 getevent: %s", strerror(errno));
            return false;
        }
    }
    getevent_fd = fileno(getevent_pipe);

    int flags = fcntl(getevent_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(getevent_fd, F_SETFL, flags | O_NONBLOCK);

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = getevent_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, getevent_fd, &ev) < 0) {
        LOG_W("注册 getevent 失败: %s", strerror(errno));
        pclose(getevent_pipe);
        getevent_pipe = NULL;
        getevent_fd = -1;
        return false;
    }
    LOG_I("触摸监听退回到 getevent 模式");
    return true;
}

static bool drain_touch_fd(int fd) {
    struct input_event ev;
    bool touch = false;
    bool abs_in_batch = false;
    ssize_t n;

    while ((n = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_MT_POSITION_Y ||
                ev.code == ABS_MT_TRACKING_ID || ev.code == ABS_MT_PRESSURE ||
                ev.code == ABS_X || ev.code == ABS_Y) {
                abs_in_batch = true;
                touch = true;
            }
        } else if (ev.type == EV_KEY && (ev.code == BTN_TOUCH || ev.code == BTN_TOOL_FINGER) && ev.value == 1) {
            touch = true;
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT && abs_in_batch) {
            touch = true;
        }
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        LOG_W("读取输入设备失败: %s", strerror(errno));
        device_lost = true;
    }
    return touch;
}

static bool check_touch_input(void) {
    if (touch_fd_count == 0 && getevent_fd < 0) {
        if (!init_touch_devices() && !init_getevent()) {

            touch_watch_ok = false;
            usleep(SLEEP_NO_TOUCH_SOURCE_MS * 1000);
            return false;
        }
    }
    if (!ensure_epfd()) {
        touch_watch_ok = false;
        usleep(SLEEP_NO_TOUCH_SOURCE_MS * 1000);
        return false;
    }
    touch_watch_ok = true;
    touch_warned = false;

    struct epoll_event events[MAX_DEVICES + 1];
    int nfds = epoll_wait(epfd, events, MAX_DEVICES + 1, TOUCH_WAIT_MS);
    if (nfds < 0) {
        if (errno == EINTR) return false;
        LOG_W("epoll_wait 失败(%s)，重建输入监听", strerror(errno));
        cleanup_touch_devices();
        use_getevent = true;
        touch_watch_ok = false;
        usleep(500 * 1000);
        return false;
    }

    bool touch = false;
    for (int i = 0; i < nfds; i++) {
        int fd = events[i].data.fd;
        if (fd == getevent_fd && getevent_fd >= 0) {
            char buf[512];
            ssize_t n = 0;
            bool got = false;
            while ((n = read(fd, buf, sizeof(buf))) > 0) got = true;
            if (got) touch = true;
            if (n == 0) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                pclose(getevent_pipe);
                getevent_pipe = NULL;
                getevent_fd = -1;
                use_getevent = false;
                touch_watch_ok = false;
                sleep(2);
            }
        } else {
            touch = drain_touch_fd(fd) || touch;
        }
    }

    if (device_lost) {
        cleanup_touch_devices();
        use_getevent = true;
        touch_watch_ok = false;
        usleep(200 * 1000);
    }
    return touch;
}

static bool probe_backlight_path(void) {
    if (backlight_probed) return backlight_path[0] != '\0';
    backlight_probed = true;

    DIR* dir = opendir("/sys/class/backlight");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char path[256];
            snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness", entry->d_name);
            if (access(path, R_OK) == 0) {
                snprintf(backlight_path, sizeof(backlight_path), "%s", path);
                break;
            }
        }
        closedir(dir);
    }

    if (!backlight_path[0]) {
        dir = opendir("/sys/class/leds");
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != NULL) {
                if (!strstr(entry->d_name, "lcd") && !strstr(entry->d_name, "backlight") &&
                    !strstr(entry->d_name, "panel")) {
                    continue;
                }
                char path[256];
                snprintf(path, sizeof(path), "/sys/class/leds/%s/brightness", entry->d_name);
                if (access(path, R_OK) == 0) {
                    snprintf(backlight_path, sizeof(backlight_path), "%s", path);
                    break;
                }
            }
            closedir(dir);
        }
    }

    if (backlight_path[0]) LOG_I("背光节点: %s", backlight_path);
    return backlight_path[0] != '\0';
}

static int read_backlight(long* value) {
    if (!probe_backlight_path()) return -1;
    int fd = open(backlight_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        backlight_path[0] = '\0';
        backlight_probed = false;
        return -1;
    }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        backlight_path[0] = '\0';
        backlight_probed = false;
        return -1;
    }
    buf[n] = '\0';
    *value = strtol(buf, NULL, 10);
    return 0;
}

static bool query_screen_state(void) {

    long bl = 0;
    if (backlight_trusted && read_backlight(&bl) == 0) return bl > 0;

    char out[512];
    if (exec_cmd("/system/bin/dumpsys power 2>/dev/null | grep -m1 mWakefulness=", out, sizeof(out)) == 0) {
        if (strstr(out, "Awake") || strstr(out, "Dreaming")) return true;
        if (strstr(out, "Asleep") || strstr(out, "Dozing")) return false;
    }
    if (exec_cmd("/system/bin/dumpsys display 2>/dev/null | grep -m1 mScreenState", out, sizeof(out)) == 0) {
        if (strstr(out, "=ON")) return true;
        if (strstr(out, "=OFF") || strstr(out, "=DOZE")) return false;
    }
    if (exec_cmd("/system/bin/dumpsys deviceidle get screen 2>/dev/null", out, sizeof(out)) == 0) {
        if (strstr(out, "true")) return true;
        if (strstr(out, "false")) return false;
    }
    return true;
}

static bool is_screen_on(void) {
    time_t now = mono_now();
    if (last_screen_check != 0 && now - last_screen_check < SCREEN_CHECK_INTERVAL) {
        return screen_state_cached;
    }
    screen_state_cached = query_screen_state();
    last_screen_check = now;
    return screen_state_cached;
}

static bool is_screen_on_fresh(void) {
    screen_state_cached = query_screen_state();
    last_screen_check = mono_now();
    return screen_state_cached;
}

static bool confirm_screen_off(void) {
    char out[512];
    if (exec_cmd("/system/bin/dumpsys power 2>/dev/null | grep -m1 mWakefulness=", out, sizeof(out)) == 0) {
        if (strstr(out, "Awake") || strstr(out, "Dreaming")) return false;
        if (strstr(out, "Asleep") || strstr(out, "Dozing")) return true;
    }
    return true;
}

static bool send_key(int code) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "/system/bin/input keyevent %d", code);
    if (exec_cmd(cmd, NULL, 0) == 0) return true;

    time_t now = mono_now();
    if (now - last_cmd_fail_log >= LOG_SUPPRESS_INTERVAL) {
        LOG_E("发送按键 %d 失败，请确认模块具备 root 权限", code);
        last_cmd_fail_log = now;
    }
    return false;
}

static bool wait_screen_off(void) {
    for (int i = 0; i < SCREEN_OFF_VERIFY_TRIES; i++) {
        usleep(SCREEN_OFF_VERIFY_MS * 1000);
        if (!is_screen_on_fresh()) return true;
    }
    return false;
}

static bool do_screen_off(void) {
    bool ok = send_key(223) && wait_screen_off();
    if (!ok) ok = send_key(26) && wait_screen_off();
    return ok;
}

static void trigger_screen_off(AppConfig* app) {
    if (!is_screen_on_fresh()) {
        app->screen_off_triggered = true;
        app->start_time = 0;
        app->last_touch_time = 0;
        LOG_I("应用 %s 超时，屏幕已处于关闭状态", app->package);
        return;
    }

    if (do_screen_off()) {
        app->start_time = 0;
        app->last_touch_time = 0;
        app->screen_off_triggered = true;
        LOG_I("应用 %s 无触控超时，已息屏", app->package);
        return;
    }

    LOG_E("息屏未生效（可能被系统拦截），%s 计时将重新开始", app->package);
    app->last_touch_time = mono_now();
}

static int run_test_off(void) {
    printf("=== 息屏测试 ===\n");
    printf("屏幕状态: %s\n", query_screen_state() ? "点亮" : "熄灭");

    if (!query_screen_state()) {
        printf("屏幕已经是熄灭的，先点亮再测\n");
        return 0;
    }

    bool ok = send_key(223) && wait_screen_off();
    printf("KEYCODE_SLEEP(223): %s\n", ok ? "生效" : "未生效");
    if (!ok) {
        ok = send_key(26) && wait_screen_off();
        printf("KEYCODE_POWER(26): %s\n", ok ? "生效" : "未生效");
    }

    printf("最终屏幕状态: %s\n", query_screen_state() ? "点亮" : "熄灭");
    printf("结果: %s\n", ok ? "息屏成功" : "息屏失败，可能被系统拦截了按键注入");
    return ok ? 0 : 2;
}

static char* skip_ws(char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static void strip_tail(char* s) {
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
}

static char* clean_line(char* line) {
    for (char* p = line; *p; p++) {
        if (*p == '#' && (p == line || isspace((unsigned char)p[-1]))) {
            *p = '\0';
            break;
        }
    }
    char* s = skip_ws(line);
    strip_tail(s);
    return s;
}

static long parse_time(const char* s) {
    if (!s) return DEFAULT_LIMIT_SECONDS;
    char* end = NULL;
    long value = strtol(s, &end, 10);
    if (end == s || value < 0) return DEFAULT_LIMIT_SECONDS;
    if (value > MAX_TIME_VALUE) value = MAX_TIME_VALUE;

    while (end && (*end == ' ' || *end == '\t')) end++;
    char unit = (end && *end) ? (char)tolower((unsigned char)*end) : 'm';
    switch (unit) {
        case 'h': return value * 3600;
        case 's': return value;
        case 'm':
        default: return value * 60;
    }
}

static int load_config(const char* path, bool initial) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        LOG_E("无法打开配置文件: %s (%s)", path, strerror(errno));
        return -1;
    }

    static AppConfig temp[MAX_APPS];
    int count = 0;
    AppConfig* current = NULL;
    char line[CONFIG_LINE_MAX];

    while (fgets(line, sizeof(line), fp)) {
        char* l = clean_line(line);
        if (!*l) continue;

        char* eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';

        char* key = skip_ws(l);
        strip_tail(key);
        char* value = skip_ws(eq + 1);

        if (strcmp(key, "package") == 0) {
            current = NULL;
            if (!*value) continue;
            if (count >= MAX_APPS) {
                LOG_W("配置项超过上限 %d，忽略: %s", MAX_APPS, value);
                continue;
            }

            int found = -1;
            for (int i = 0; i < count; i++) {
                if (strcmp(temp[i].package, value) == 0) {
                    found = i;
                    break;
                }
            }
            if (found >= 0) {
                current = &temp[found];
                continue;
            }

            current = &temp[count++];
            memset(current, 0, sizeof(*current));
            snprintf(current->package, sizeof(current->package), "%s", value);
            current->limit_time = DEFAULT_LIMIT_SECONDS;
        } else if (current && strcmp(key, "limit_time") == 0) {
            current->limit_time = parse_time(value);
        }
    }
    fclose(fp);

    if (count == 0) {
        if (initial) return -2;
        LOG_W("配置文件里没有有效应用，继续使用旧配置");
        return -1;
    }

    for (int i = 0; i < count; i++) {
        for (int j = 0; j < app_count; j++) {
            if (strcmp(temp[i].package, apps[j].package) == 0) {
                temp[i].start_time = apps[j].start_time;
                temp[i].last_touch_time = apps[j].last_touch_time;
                temp[i].absent_since = apps[j].absent_since;
                temp[i].screen_off_triggered = apps[j].screen_off_triggered;
                break;
            }
        }
    }

    memcpy(apps, temp, sizeof(AppConfig) * (size_t)count);
    app_count = count;
    return 0;
}

static bool check_config_updated(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (st.st_mtime != last_config_mtime || st.st_size != last_config_size) {
        last_config_mtime = st.st_mtime;
        last_config_size = st.st_size;
        return true;
    }
    return false;
}

static void diag_line(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fputc('\n', stdout);
    fflush(stdout);
}

static void diag_text(const char* text, size_t max_len) {
    char buf[256];
    size_t i = 0;
    for (; text[i] && i < max_len && i < sizeof(buf) - 1; i++) {
        buf[i] = (text[i] == '\n' || text[i] == '\r' || text[i] == '\t') ? ' ' : text[i];
    }
    buf[i] = '\0';

    char out[256];
    size_t j = 0;
    bool prev_space = false;
    for (i = 0; buf[i] && j < sizeof(out) - 1; i++) {
        bool sp = (buf[i] == ' ');
        if (sp && prev_space) continue;
        out[j++] = buf[i];
        prev_space = sp;
    }
    out[j] = '\0';
    diag_line("    %s", out);
}

static void diag_run(const char* label, const char* cmd) {
    char out[CMD_BUFFER_SIZE];
    int rc = exec_cmd(cmd, out, sizeof(out));
    if (rc != 0) {
        diag_line("%s -> 无输出/失败", label);
        return;
    }
    diag_line("%s ->", label);
    diag_text(out, 200);
}

static int run_diag(const char* config_path) {
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    if (localtime_r(&now, &tmv)) {
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    } else {
        snprintf(ts, sizeof(ts), "unknown");
    }

    bool bl_ok = false;
    bool dumpsys_screen_ok = false;
    int touch_found = 0;
    int pkg_parsed = 0;
    char first_pkg[MAX_PKG_LEN] = {0};

    diag_line("=== 安眠[息屏管理] 诊断 ===");
    diag_line("时间: %s", ts);
    diag_line("屏幕判定: %s", query_screen_state() ? "点亮" : "熄灭");

    if (probe_backlight_path()) {
        long bl = -1;
        if (read_backlight(&bl) == 0) {
            bl_ok = true;
            diag_line("背光节点: %s = %ld", backlight_path, bl);
        } else {
            diag_line("背光节点: %s 读取失败", backlight_path);
        }
    } else {
        diag_line("背光节点: 未找到（改用 dumpsys 判定）");
    }

    diag_run("dumpsys power mWakefulness", "/system/bin/dumpsys power 2>/dev/null | grep -m1 mWakefulness=");
    diag_run("dumpsys display mScreenState", "/system/bin/dumpsys display 2>/dev/null | grep -m1 mScreenState");
    diag_run("deviceidle get screen", "/system/bin/dumpsys deviceidle get screen 2>/dev/null");

    {
        char tmp[512];
        if (exec_cmd("/system/bin/dumpsys display 2>/dev/null | grep -m1 mScreenState", tmp, sizeof(tmp)) == 0 &&
            (strstr(tmp, "=ON") || strstr(tmp, "=OFF") || strstr(tmp, "=DOZE"))) {
            dumpsys_screen_ok = true;
        }
    }

    diag_line("--- 前台应用 ---");
    for (int i = 0; i < TOP_CMD_COUNT; i++) {
        char out[CMD_BUFFER_SIZE];
        if (exec_cmd(top_cmds[i].cmd, out, sizeof(out)) != 0) {
            diag_line("[%d] 失败: %s", i, top_cmds[i].cmd);
            continue;
        }
        diag_line("[%d] %s", i, top_cmds[i].cmd);
        diag_text(out, 160);
        const char* p = strstr(out, top_cmds[i].keyword);
        char pkg[MAX_PKG_LEN];
        if (p && extract_package(p + strlen(top_cmds[i].keyword), pkg, sizeof(pkg))) {
            pkg_parsed++;
            if (!first_pkg[0]) snprintf(first_pkg, sizeof(first_pkg), "%s", pkg);
            diag_line("    解析 -> %s", pkg);
        } else {
            diag_line("    解析 -> 失败");
        }
    }

    diag_line("--- 输入设备 ---");
    DIR* dir = opendir("/dev/input");
    if (!dir) {
        diag_line("无法打开 /dev/input: %s", strerror(errno));
    } else {
        int total = 0;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "event", 5) != 0) continue;
            total++;

            char path[256];
            snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                diag_line("%-10s 打不开: %s", entry->d_name, strerror(errno));
                continue;
            }

            char name[128] = {0};
            ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
            unsigned long abs_bits[MY_NBITS(ABS_MAX)];
            memset(abs_bits, 0, sizeof(abs_bits));
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);

            bool is_touch = BIT_TEST(ABS_MT_POSITION_X, abs_bits) || BIT_TEST(ABS_MT_POSITION_Y, abs_bits) ||
                            BIT_TEST(ABS_MT_TRACKING_ID, abs_bits) || BIT_TEST(ABS_X, abs_bits) ||
                            BIT_TEST(ABS_Y, abs_bits);
            if (is_touch) touch_found++;
            diag_line("%-10s %-30s 触控=%s MT_X=%d MT_TRACK=%d", entry->d_name, name[0] ? name : "(无名)",
                      is_touch ? "是" : "否", BIT_TEST(ABS_MT_POSITION_X, abs_bits) ? 1 : 0,
                      BIT_TEST(ABS_MT_TRACKING_ID, abs_bits) ? 1 : 0);
            close(fd);
        }
        closedir(dir);
        diag_line("共 %d 个 event 设备，其中触控 %d 个", total, touch_found);
    }

    int test_epfd = epoll_create1(0);
    diag_line("epoll_create1: %s", test_epfd >= 0 ? "可用" : "不可用");
    if (test_epfd >= 0) close(test_epfd);

    if (config_path) {
        diag_line("--- 配置 ---");
        if (load_config(config_path, true) == 0) {
            for (int i = 0; i < app_count; i++) {
                diag_line("%-36s 限制 %ld 秒", apps[i].package, apps[i].limit_time);
            }
        } else {
            diag_line("配置读取失败: %s", config_path);
        }
    }

    diag_line("=== 结论 ===");
    const char* verdict;
    if (touch_found > 0 && pkg_parsed > 0 && (bl_ok || dumpsys_screen_ok)) {
        verdict = "可用";
    } else if (touch_found > 0 || pkg_parsed > 0) {
        verdict = "疑似可用";
    } else {
        verdict = "需要自行测试";
    }
    diag_line("诊断结论: %s", verdict);

    if (touch_found > 0) {
        diag_line("- 触摸监听: 正常（%d 个触控设备）", touch_found);
    } else {
        diag_line("- 触摸监听: 没有识别到触摸设备，自动息屏会被暂停");
    }
    if (pkg_parsed > 0) {
        diag_line("- 前台识别: 正常（当前 %s）", first_pkg);
    } else {
        diag_line("- 前台识别: 命令全部解析失败，无法判断在用什么应用");
    }
    if (bl_ok) {
        diag_line("- 屏幕判定: 正常（背光节点可读）");
    } else if (dumpsys_screen_ok) {
        diag_line("- 屏幕判定: 背光读不到，改用 dumpsys 判定");
    } else {
        diag_line("- 屏幕判定: 读不到屏幕状态");
    }
    diag_line("- 息屏按键: 诊断无法验证，请点「测试息屏」确认");

    if (config_path && app_count == 0) {
        diag_line("- 注意: 配置里还没有应用，守护进程不会触发息屏");
    }
    if (strcmp(verdict, "可用") != 0) {
        diag_line("- 建议: 先点「测试息屏」，把这份结果和机型一起反馈");
    }

    diag_line("=== 诊断结束 ===");
    return 0;
}

static long elapsed_ms(const struct timespec* since) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long)((now.tv_sec - since->tv_sec) * 1000 + (now.tv_nsec - since->tv_nsec) / 1000000);
}

int main(int argc, char* argv[]) {
    const char* config_path = NULL;
    bool diag = false;
    bool test_off = false;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--config=", 9) == 0) {
            config_path = argv[i] + 9;
        } else if (strncmp(argv[i], "--log=", 6) == 0) {
            snprintf(log_file_path, sizeof(log_file_path), "%s", argv[i] + 6);
        } else if (strncmp(argv[i], "--debug=", 8) == 0) {
            log_enabled = strcmp(argv[i] + 8, "true") == 0;
        } else if (strcmp(argv[i], "--diag") == 0) {
            diag = true;
        } else if (strcmp(argv[i], "--test-off") == 0) {
            test_off = true;
        }
    }

    if (test_off) return run_test_off();
    if (diag) return run_diag(config_path);

    if (!config_path) {
        fprintf(stderr, "错误：必须提供 --config=<路径>\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    signal(SIGHUP, SIG_IGN);

    if (log_enabled && log_file_path[0]) {
        ensure_log_file();
        if (!log_fp) {
            fprintf(stderr, "无法打开日志文件: %s\n", log_file_path);
            log_enabled = false;
        }
    }

    struct stat st;
    if (stat(config_path, &st) == 0) {
        last_config_mtime = st.st_mtime;
        last_config_size = st.st_size;
    }

    int cfg_rc = load_config(config_path, true);
    if (cfg_rc == -2) {
        LOG_W("配置里还没有应用，空转等待；在 WebUI 添加并保存后 10 秒内生效");
    } else if (cfg_rc != 0) {
        LOG_W("配置文件暂时读不到（%s），空转等待", config_path);
    }

    LOG_I("启动时间限制守护进程，共 %d 个应用（无触控息屏模式）", app_count);
    probe_backlight_path();
    init_touch_devices();

    time_t last_config_check = 0;
    bool last_logged_screen_state = true;
    bool prev_touch_ok = true;

    while (!stop_requested) {
        struct timespec loop_begin;
        clock_gettime(CLOCK_MONOTONIC, &loop_begin);
        long spent = 0;

        time_t now = mono_now();
        bool screen_on = is_screen_on();

        if (!screen_on && last_logged_screen_state && !confirm_screen_off()) {
            screen_on = true;
            screen_state_cached = true;
            last_screen_check = now;
            if (backlight_trusted && backlight_path[0]) {
                backlight_trusted = false;
                LOG_W("背光读数与系统状态不一致，改用 dumpsys power 判定屏幕状态");
            }
        }

        if (screen_on != last_logged_screen_state) {
            LOG_I("屏幕状态: %s", screen_on ? "点亮" : "熄灭");
            last_logged_screen_state = screen_on;
        }

        if (!screen_on) {
            for (int i = 0; i < app_count; i++) {
                if (apps[i].start_time != 0 || apps[i].screen_off_triggered) {
                    apps[i].start_time = 0;
                    apps[i].last_touch_time = 0;
                    apps[i].absent_since = 0;
                    apps[i].screen_off_triggered = false;
                    LOG_I("屏幕熄灭，停止跟踪 %s", apps[i].package);
                }
            }
            cleanup_touch_devices();
            usleep((backlight_path[0] ? SLEEP_SCREEN_OFF_MS : SLEEP_SCREEN_OFF_NO_BL_MS) * 1000);
            continue;
        }

        if (now - last_config_check >= CONFIG_CHECK_INTERVAL) {
            last_config_check = now;
            if (check_config_updated(config_path) && load_config(config_path, false) == 0) {
                LOG_I("配置已重新加载，共 %d 个应用", app_count);
            }
        }

        if (app_count == 0) {

            long idle = elapsed_ms(&loop_begin);
            if (idle < 1000) usleep((1000 - idle) * 1000);
            continue;
        }

        bool has_touch = check_touch_input();

        if (touch_watch_ok && !prev_touch_ok) {
            for (int i = 0; i < app_count; i++) {
                if (apps[i].start_time != 0) apps[i].last_touch_time = now;
            }
            LOG_I("触摸监听已恢复，重新开始计时");
        }
        prev_touch_ok = touch_watch_ok;

        char current_app[MAX_PKG_LEN];
        if (get_top_app_name(current_app, sizeof(current_app))) {

            bool overlay = is_system_overlay(current_app);

            if (overlay) {
                if (has_touch) {
                    for (int i = 0; i < app_count; i++) {
                        if (apps[i].start_time != 0) apps[i].last_touch_time = now;
                    }
                }
                goto loop_end;
            }

            for (int i = 0; i < app_count; i++) {
                AppConfig* app = &apps[i];
                if (strcmp(current_app, app->package) != 0) {
                    if (app->start_time == 0) continue;

                    if (app->absent_since == 0) {
                        app->absent_since = now;
                        LOG_I("前台变成 %s，先保留 %s 的计时（宽限 %d 秒）", current_app, app->package,
                              RESET_GRACE_SECONDS);
                        continue;
                    }
                    long absent = (long)(now - app->absent_since);
                    if (absent < RESET_GRACE_SECONDS) continue;

                    app->start_time = 0;
                    app->last_touch_time = 0;
                    app->absent_since = 0;
                    app->screen_off_triggered = false;
                    LOG_I("应用 %s 已离开前台 %ld 秒，重置计时", app->package, absent);
                    continue;
                }

                app->absent_since = 0;

                if (app->start_time == 0) {
                    app->start_time = now;
                    app->last_touch_time = now;
                    app->screen_off_triggered = false;
                    LOG_I("开始跟踪 %s（限制 %ld 秒）", app->package, app->limit_time);
                    continue;
                }

                if (has_touch) app->last_touch_time = now;

                if (!touch_watch_ok) {

                    if (!touch_warned) {
                        LOG_W("无法监听触摸事件，为安全起见暂停息屏");
                        touch_warned = true;
                    }
                } else if (!app->screen_off_triggered && app->limit_time > 0 &&
                           now - app->last_touch_time >= app->limit_time &&
                           app_is_foreground(app->package)) {
                    trigger_screen_off(app);
                }
            }
        }

loop_end:
        spent = elapsed_ms(&loop_begin);
        if (spent < MIN_LOOP_INTERVAL_MS) usleep((MIN_LOOP_INTERVAL_MS - spent) * 1000);
    }

    LOG_I("收到退出信号，守护进程结束");
    cleanup_touch_devices();
    if (epfd >= 0) close(epfd);
    if (log_fp) fclose(log_fp);
    return 0;
}
