/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define DEBUG_UEVENTS
#define CHARGER_KLOG_LEVEL 6

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/time.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/android_alarm.h>


#include <cutils/android_reboot.h>
#include <cutils/klog.h>
#include <cutils/list.h>
#include <cutils/misc.h>
#include <cutils/uevent.h>
#include <cutils/properties.h>

#ifdef CHARGER_ENABLE_SUSPEND
#include <suspend/autosuspend.h>
#endif

#include "minui/minui.h"

#ifndef max
#define max(a,b) ((a) > (b) ? (a) : (b))
#endif

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif

#define ARRAY_SIZE(x)           (sizeof(x)/sizeof(x[0]))

#define MSEC_PER_SEC            (1000LL)
#define NSEC_PER_MSEC           (1000000LL)

#define BATTERY_UNKNOWN_TIME    (2 * MSEC_PER_SEC)
#define POWER_ON_KEY_TIME       (2 * MSEC_PER_SEC)
#define UNPLUGGED_SHUTDOWN_TIME (3 * MSEC_PER_SEC)
#define WAKEUP_TIME (3 * NSEC_PER_MSEC * MSEC_PER_SEC)

#define BATTERY_FULL_THRESH     99

#define BATTERY_LOW_TEMP_THRESH     0        //must match with sys_config.fex pmu_charge_ltf
#define BATTERY_HIGH_TEMP_THRESH     500        //must match with sys_config.fex pmu_charge_htf
#define IS_TEMP_VALID(x)        ((x) >= BATTERY_LOW_TEMP_THRESH ? ((x) <= BATTERY_HIGH_TEMP_THRESH ? true : false) : false)

#define LAST_KMSG_PATH          "/proc/last_kmsg"
#define LAST_KMSG_MAX_SZ        (32 * 1024)

#define WAKEALARM_PATH        "/sys/class/rtc/rtc0/wakealarm"

#define LOGE(x...) do { KLOG_ERROR("charger", x); } while (0)
#define LOGI(x...) do { KLOG_INFO("charger", x); } while (0)
#define LOGV(x...) do { KLOG_DEBUG("charger", x); } while (0)

struct key_state {
    bool pending;
    bool down;
    int64_t timestamp;      // key->down时的时间点
};

struct power_supply {
    struct listnode list;
    char name[256];
    char type[32];
    bool online;
    bool valid;
    char cap_path[PATH_MAX];
    char temp_path[PATH_MAX];
    char status_path[PATH_MAX];
};

struct frame {
    const char *name;       // 每帧图片名称
    int disp_time;          // 显示时间
    int min_capacity;       // 该帧对应的容量值
    bool level_only;

    gr_surface surface;
};

struct animation {
    bool run;                       // 表示动画正在运行

    struct frame *frames;           // 存放每帧的信息，数组名
    int cur_frame;                  // 当前第几帧
    int num_frames;                 // 总共多少帧

    int cur_cycle;                  // 当前周期
    int num_cycles;                 // 总共周期数

    /* current capacity being animated */
    int capacity;                   // 当前电池电量

    bool freeze;                    // 是否显示静态动画
};

struct charger {
    int64_t next_screen_transition; // 下帧动画开始时间(本次动画结束时间)
    int64_t next_key_check;         // 下次按键检查的时间，如果不检查就置为-1
    int64_t next_pwr_check;         // 下次检测DC的时间，如果不检查就置为-1

    struct key_state keys[KEY_MAX + 1];
    int uevent_fd;

    struct listnode supplies;
    int num_supplies;
    int num_supplies_online;

    struct animation *batt_anim;        // 存放动画信息
    gr_surface surf_unknown;

    struct power_supply *battery;
};

struct uevent {
    const char *action;
    const char *path;
    const char *subsystem;
    const char *ps_name;
    const char *ps_type;
    const char *ps_online;
};

static struct frame batt_anim_frames[] = {
    {
        .name = "charger/battery_0",
        .disp_time = 750,
        .min_capacity = 0,
    },
    {
        .name = "charger/battery_1",
        .disp_time = 750,
        .min_capacity = 20,
    },
    {
        .name = "charger/battery_2",
        .disp_time = 750,
        .min_capacity = 40,
    },
    {
        .name = "charger/battery_3",
        .disp_time = 750,
        .min_capacity = 60,
    },
    {
        .name = "charger/battery_4",
        .disp_time = 750,
        .min_capacity = 80,
        //.level_only = true,
    },
    {
        .name = "charger/battery_5",
        .disp_time = 750,
        .min_capacity = BATTERY_FULL_THRESH,
    },
};

static struct animation battery_animation = {
    .frames = batt_anim_frames,
    .num_frames = ARRAY_SIZE(batt_anim_frames),
    .num_cycles = 3,
};

static struct charger charger_state = {
    .batt_anim = &battery_animation,
};

static struct frame batt_overtemp_anim_frames[] = {
    {
        .name = "charger/battery_fail",
        .disp_time = 3000,
        .min_capacity = 0,
    },
    {
        .name = "charger/battery_charge",
        .disp_time = 3000,
        .min_capacity = 0,
    },
};

static struct animation battery_overtemp_animation = {
    .frames = batt_overtemp_anim_frames,
    .num_frames = ARRAY_SIZE(batt_overtemp_anim_frames),
    .num_cycles = 1,
};

static int char_width;
static int char_height;

static int alarm_fd = 0;
static pthread_t tid_alarm;

static int acquire_wake_lock_timeout(long long timeout);

static long get_wakealarm_sec(void)
{
    int fd = 0, ret = 0;
    unsigned long wakealarm_time = 0;
    char buf[32] = { 0 };

    fd = open(WAKEALARM_PATH, O_RDWR);
    if (fd < 0) {
        LOGE("open %s failed, return=%d\n", WAKEALARM_PATH, fd);
        return fd;
    }

    ret = read(fd, buf, sizeof(buf));
    if (ret > 0) {
        wakealarm_time = strtoul(buf, NULL, 0);
        LOGI("%s, %d, read wakealarm_time=%lu\n", __func__, __LINE__, wakealarm_time);

        // Clean initial wakealarm.
        // We will set wakealarm again use ANDROID_ALARM_SET ioctl.
        // Initial wakealarm's time unit is second, have conflict with
        // nanosecond alarmtimer in alarmtimer_suspend().
        snprintf(buf, sizeof(buf), "0");
        write(fd, buf, strlen(buf) + 1);
        LOGI("%s, %d, write wakealarm_time=0\n", __func__, __LINE__);

        close(fd);
        return wakealarm_time;
    }

    close(fd);
    return ret;
}

void *alarm_thread_handler(void *arg)
{
    int ret = 0;
    if (alarm_fd <= 0) {
        LOGE("%s, %d, alarm_fd=%d and exit\n", __func__, __LINE__, alarm_fd);
        return NULL;
    }

    while (true) {
        ret = ioctl(alarm_fd, ANDROID_ALARM_WAIT);
        if (ret & ANDROID_ALARM_RTC_SHUTDOWN_WAKEUP_MASK) {
            LOGI("%s, %d, alarm wakeup rebooting\n", __func__, __LINE__);
            acquire_wake_lock_timeout(UNPLUGGED_SHUTDOWN_TIME);
            android_reboot(ANDROID_RB_RESTART, 0, 0);
        } else {
            LOGI("%s, %d, alarm wait wakeup by %d\n", __func__, __LINE__, ret);
        }
    }

	return NULL;
}

/* current time in milliseconds */
static int64_t curr_time_ms(void)
{
    struct timespec tm;
    clock_gettime(CLOCK_MONOTONIC, &tm);
    return tm.tv_sec * MSEC_PER_SEC + (tm.tv_nsec / NSEC_PER_MSEC);
}

static void clear_screen(void)
{
    gr_color(0, 0, 0, 255);
    gr_fill(0, 0, gr_fb_width(), gr_fb_height());
};

#define MAX_KLOG_WRITE_BUF_SZ 256

static void dump_last_kmsg(void)
{
    char *buf;
    char *ptr;
    unsigned sz = 0;
    int len;

    LOGI("\n");
    LOGI("*************** LAST KMSG ***************\n");
    LOGI("\n");
    buf = load_file(LAST_KMSG_PATH, &sz);
    if (!buf || !sz) {
        LOGI("last_kmsg not found. Cold reset?\n");
        goto out;
    }

    len = min(sz, LAST_KMSG_MAX_SZ);
    ptr = buf + (sz - len);

    while (len > 0) {
        int cnt = min(len, MAX_KLOG_WRITE_BUF_SZ);
        char yoink;
        char *nl;

        nl = memrchr(ptr, '\n', cnt - 1);
        if (nl)
            cnt = nl - ptr + 1;

        yoink = ptr[cnt];
        ptr[cnt] = '\0';
        klog_write(6, "<6>%s", ptr);
        ptr[cnt] = yoink;

        len -= cnt;
        ptr += cnt;
    }

    free(buf);

out:
    LOGI("\n");
    LOGI("************* END LAST KMSG *************\n");
    LOGI("\n");
}

static int read_file(const char *path, char *buf, size_t sz)
{
    int fd;
    size_t cnt;

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        goto err;

    cnt = read(fd, buf, sz - 1);
    if (cnt <= 0)
        goto err;
    buf[cnt] = '\0';
    if (buf[cnt - 1] == '\n') {
        cnt--;
        buf[cnt] = '\0';
    }

    close(fd);
    return cnt;

err:
    if (fd >= 0)
        close(fd);
    return -1;
}

static int read_file_int(const char *path, int *val)
{
    char buf[32];
    int ret;
    int tmp;
    char *end;

    ret = read_file(path, buf, sizeof(buf));
    if (ret < 0)
        return -1;

    tmp = strtol(buf, &end, 0);
    if (end == buf ||
        ((end < buf+sizeof(buf)) && (*end != '\n' && *end != '\0')))
        goto err;

    *val = tmp;
    return 0;

err:
    return -1;
}

static int get_battery_capacity(struct charger *charger)
{
    int ret;
    int batt_cap = -1;

    if (!charger->battery)
        return -1;

    ret = read_file_int(charger->battery->cap_path, &batt_cap);
    if (ret < 0 || batt_cap > 100) {
        batt_cap = -1;
    }

    return batt_cap;
}

static int get_battery_temperature(struct charger *charger)
{
    int ret;
    int batt_temp = 0;

    if (!charger->battery)
        return -1;

    ret = read_file_int(charger->battery->temp_path, &batt_temp);
    if (ret < 0 || batt_temp > 800 || batt_temp < -250) {
        batt_temp = 0;
    }

    return batt_temp;
}

//this value is match to kernel
static int get_battery_status(struct charger *charger)
{
    int ret;
    int status = 0;
    char battery_status[32];

    if (!charger->battery)
        return -1;

    ret = read_file(charger->battery->status_path, battery_status,sizeof(battery_status));
    if (ret < 0)
        return status;
    if(strcmp(battery_status,"Unknown") == 0)
        status = 0;
    else if(strcmp(battery_status,"Charging") == 0)
        status = 1;
    else if(strcmp(battery_status,"Discharging") == 0)
        status = 2;
    else if(strcmp(battery_status,"Not charging") == 0)
        status = 3;
    else if(strcmp(battery_status,"Full") == 0)
        status = 4;

    return status;
}

static struct power_supply *find_supply(struct charger *charger,
                                        const char *name)
{
    struct listnode *node;
    struct power_supply *supply;

    list_for_each(node, &charger->supplies) {
        supply = node_to_item(node, struct power_supply, list);
        if (!strncmp(name, supply->name, sizeof(supply->name)))
            return supply;
    }
    return NULL;
}

static struct power_supply *add_supply(struct charger *charger,
                                       const char *name, const char *type,
                                       const char *path, bool online)
{
    struct power_supply *supply;

    supply = calloc(1, sizeof(struct power_supply));
    if (!supply)
        return NULL;

    strlcpy(supply->name, name, sizeof(supply->name));
    strlcpy(supply->type, type, sizeof(supply->type));
    snprintf(supply->cap_path, sizeof(supply->cap_path),
             "/sys/%s/capacity", path);
    snprintf(supply->temp_path, sizeof(supply->temp_path),
             "/sys/%s/temp", path);
    snprintf(supply->status_path, sizeof(supply->status_path),
             "/sys/%s/status", path);
    supply->online = online;
    list_add_tail(&charger->supplies, &supply->list);
    charger->num_supplies++;
    LOGV("... added %s %s %d\n", supply->name, supply->type, online);
    return supply;
}

static void remove_supply(struct charger *charger, struct power_supply *supply)
{
    if (!supply)
        return;
    list_remove(&supply->list);
    charger->num_supplies--;
    free(supply);
}

#ifdef CHARGER_ENABLE_SUSPEND
static int request_suspend(bool enable)
{
    if (enable)
        return autosuspend_enable();
    else
        return autosuspend_disable();
}
#else
static int request_suspend(bool enable)
{
    return 0;
}
#endif

static int acquire_wake_lock_timeout(long long timeout)
{
    int fd,ret;
    char str[64];
    fd = open("/sys/power/wake_lock", O_WRONLY, 0);
    if (fd < 0)
        return -1;

    sprintf(str,"charge %lld",timeout);
    ret = write(fd, str, strlen(str));
    close(fd);
    return ret;
}

//note the default brightness is 197
int set_backlight(int brightness)
{
    int fd,ret;
    char str[64];

    fd = open("/sys/class/disp/disp/attr/lcd_bl", O_WRONLY, 0);
    if (fd < 0)
        return -1;

    sprintf(str,"%d",brightness);
    ret = write(fd, str, strlen(str));
    close(fd);
    return ret;
}

int get_backlight(void)
{
    int ret;
    int brightness = -1;

    ret = read_file_int("/sys/class/disp/disp/attr/lcd_bl", &brightness);
    if (ret < 0 || brightness > 255) {
        brightness = 0;
    }

    return brightness;
}

static void parse_uevent(const char *msg, struct uevent *uevent)
{
    uevent->action = "";
    uevent->path = "";
    uevent->subsystem = "";
    uevent->ps_name = "";
    uevent->ps_online = "";
    uevent->ps_type = "";

    /* currently ignoring SEQNUM */
    while (*msg) {
#ifdef DEBUG_UEVENTS
        LOGV("uevent str: %s\n", msg);
#endif
        if (!strncmp(msg, "ACTION=", 7)) {
            msg += 7;
            uevent->action = msg;
        } else if (!strncmp(msg, "DEVPATH=", 8)) {
            msg += 8;
            uevent->path = msg;
        } else if (!strncmp(msg, "SUBSYSTEM=", 10)) {
            msg += 10;
            uevent->subsystem = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_NAME=", 18)) {
            msg += 18;
            uevent->ps_name = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_ONLINE=", 20)) {
            msg += 20;
            uevent->ps_online = msg;
        } else if (!strncmp(msg, "POWER_SUPPLY_TYPE=", 18)) {
            msg += 18;
            uevent->ps_type = msg;
        }

        /* advance to after the next \0 */
        while (*msg++)
            ;
    }

    LOGV("event { '%s', '%s', '%s', '%s', '%s', '%s' }\n",
         uevent->action, uevent->path, uevent->subsystem,
         uevent->ps_name, uevent->ps_type, uevent->ps_online);
}

static void process_ps_uevent(struct charger *charger, struct uevent *uevent)
{
    int online;
    char ps_type[32];
    struct power_supply *supply = NULL;
    int i;
    bool was_online = false;
    bool battery = false;

    if (uevent->ps_type[0] == '\0') {
        char *path;
        int ret;

        if (uevent->path[0] == '\0')
            return;
        ret = asprintf(&path, "/sys/%s/type", uevent->path);
        if (ret <= 0)
            return;
        ret = read_file(path, ps_type, sizeof(ps_type));
        free(path);
        if (ret < 0)
            return;
    } else {
        strlcpy(ps_type, uevent->ps_type, sizeof(ps_type));
    }

    if (!strncmp(ps_type, "Battery", 7))
        battery = true;

    online = atoi(uevent->ps_online);
    supply = find_supply(charger, uevent->ps_name);
    if (supply) {
        was_online = supply->online;
        supply->online = online;
    }

    if (!strcmp(uevent->action, "add")) {
        if (!supply) {
            supply = add_supply(charger, uevent->ps_name, ps_type, uevent->path,
                                online);
            if (!supply) {
                LOGE("cannot add supply '%s' (%s %d)\n", uevent->ps_name,
                     uevent->ps_type, online);
                return;
            }
            /* only pick up the first battery for now */
            if (battery && !charger->battery)
                charger->battery = supply;
        } else {
            LOGE("supply '%s' already exists..\n", uevent->ps_name);
        }
    } else if (!strcmp(uevent->action, "remove")) {
        if (supply) {
            if (charger->battery == supply)
                charger->battery = NULL;
            remove_supply(charger, supply);
            supply = NULL;
        }
    } else if (!strcmp(uevent->action, "change")) {
        if (!supply) {
            LOGE("power supply '%s' not found ('%s' %d)\n",
                 uevent->ps_name, ps_type, online);
            return;
        }
    } else {
        return;
    }

    /* allow battery to be managed in the supply list but make it not
     * contribute to online power supplies. */
    if (!battery) {
        if (was_online && !online)
            charger->num_supplies_online--;
        else if (supply && !was_online && online)
            charger->num_supplies_online++;
    }

    LOGI("power supply %s (%s) %s (action=%s num_online=%d num_supplies=%d)\n",
         uevent->ps_name, ps_type, battery ? "" : online ? "online" : "offline",
         uevent->action, charger->num_supplies_online, charger->num_supplies);
}

static void process_uevent(struct charger *charger, struct uevent *uevent)
{
    if (!strcmp(uevent->subsystem, "power_supply"))
        process_ps_uevent(charger, uevent);
}

#define UEVENT_MSG_LEN  1024
static int handle_uevent_fd(struct charger *charger, int fd)
{
    char msg[UEVENT_MSG_LEN+2];
    int n;

    if (fd < 0)
        return -1;

    while (true) {
        struct uevent uevent;

        n = uevent_kernel_multicast_recv(fd, msg, UEVENT_MSG_LEN);
        if (n <= 0)
            break;
        if (n >= UEVENT_MSG_LEN)   /* overflow -- discard */
            continue;

        msg[n] = '\0';
        msg[n+1] = '\0';

        parse_uevent(msg, &uevent);
        process_uevent(charger, &uevent);
    }

    return 0;
}

static int uevent_callback(int fd, short revents, void *data)
{
    struct charger *charger = data;

    if (!(revents & POLLIN))
        return -1;
    return handle_uevent_fd(charger, fd);
}

/* force the kernel to regenerate the change events for the existing
 * devices, if valid */
static void do_coldboot(struct charger *charger, DIR *d, const char *event,
                        bool follow_links, int max_depth)
{
    struct dirent *de;
    int dfd, fd;

    dfd = dirfd(d);

    fd = openat(dfd, "uevent", O_WRONLY);
    if (fd >= 0) {
        write(fd, event, strlen(event));
        close(fd);
        handle_uevent_fd(charger, charger->uevent_fd);
    }

    while ((de = readdir(d)) && max_depth > 0) {
        DIR *d2;

        LOGV("looking at '%s'\n", de->d_name);

        if ((de->d_type != DT_DIR && !(de->d_type == DT_LNK && follow_links)) ||
           de->d_name[0] == '.') {
            LOGV("skipping '%s' type %d (depth=%d follow=%d)\n",
                 de->d_name, de->d_type, max_depth, follow_links);
            continue;
        }
        LOGV("can descend into '%s'\n", de->d_name);

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            LOGE("cannot openat %d '%s' (%d: %s)\n", dfd, de->d_name,
                 errno, strerror(errno));
            continue;
        }

        d2 = fdopendir(fd);
        if (d2 == 0)
            close(fd);
        else {
            LOGV("opened '%s'\n", de->d_name);
            do_coldboot(charger, d2, event, follow_links, max_depth - 1);
            closedir(d2);
        }
    }
}

static void coldboot(struct charger *charger, const char *path,
                     const char *event)
{
    char str[256];

    LOGV("doing coldboot '%s' in '%s'\n", event, path);
    DIR *d = opendir(path);
    if (d) {
        snprintf(str, sizeof(str), "%s\n", event);
        do_coldboot(charger, d, str, true, 1);
        closedir(d);
    }
}

static int draw_text(const char *str, int x, int y)
{
    int str_len_px = gr_measure(str);

    if (x < 0)
        x = (gr_fb_width() - str_len_px) / 2;
    if (y < 0)
        y = (gr_fb_height() - char_height) / 2;
    gr_text(x, y, str, 0);

    return y + char_height;
}

static void android_green(void)
{
    gr_color(0xa4, 0xc6, 0x39, 255);
}

/* returns the last y-offset of where the surface ends */
static int draw_surface_centered(struct charger *charger, gr_surface surface)
{
    int w;
    int h;
    int x;
    int y;

    w = gr_get_width(surface);
    h = gr_get_height(surface);
    x = (gr_fb_width() - w) / 2 ;
    y = (gr_fb_height() - h) / 2 ;

    LOGV("drawing surface %dx%d+%d+%d\n", w, h, x, y);
    gr_blit(surface, 0, 0, w, h, x, y);
    return y + h;
}

static void draw_unknown(struct charger *charger)
{
    int y;
    if (charger->surf_unknown) {
        draw_surface_centered(charger, charger->surf_unknown);
    } else {
        android_green();
        y = draw_text("Charging!", -1, -1);
        draw_text("?\?/100", -1, y + 25);
    }
}

static void draw_battery(struct charger *charger)
{
    struct animation *batt_anim = charger->batt_anim;
    struct frame *frame = &batt_anim->frames[batt_anim->cur_frame];

    if (batt_anim->num_frames != 0) {
        draw_surface_centered(charger, frame->surface);
        LOGV("drawing frame #%d name=%s min_cap=%d time=%d\n",
             batt_anim->cur_frame, frame->name, frame->min_capacity,
             frame->disp_time);
    }
}

static void draw_overtemp(struct animation *batt_anim)
{
    struct frame *frame = &batt_anim->frames[batt_anim->cur_frame];
    gr_fb_blank(false);
    clear_screen();
    if (batt_anim->num_frames != 0) {
        draw_surface_centered(NULL, frame->surface);
        LOGV("drawing frame #%d name=%s\n", batt_anim->cur_frame, frame->name);
    }
    gr_flip();
}

static void redraw_screen(struct charger *charger)
{
    struct animation *batt_anim = charger->batt_anim;

    // 清屏
    clear_screen();

    /* try to display *something* */
    if (batt_anim->capacity < 0 || batt_anim->num_frames == 0)
        // 电量值未知状态或者就没有找到电量显示动画，直接显示个异类
        draw_unknown(charger);
    else
        // 显示充电动画
        draw_battery(charger);
    gr_flip();
}

static void kick_animation(struct animation *anim)
{
    anim->run = true;
}

static void reset_animation(struct animation *anim)
{
    anim->cur_cycle = 0;
    anim->cur_frame = 0;
    anim->run = false;
    anim->freeze = false;
}

static void freeze_animation(struct animation *anim)
{
        anim->cur_cycle = 0;
        anim->cur_frame = 0;
        anim->freeze = true;
}

static void thaw_animation(struct animation *anim)
{
        anim->freeze = false;
}

static void kick_overtemp_animation(struct animation *anim, int cur_frame)
{
        anim->cur_frame = cur_frame;
        anim->run = true;
        draw_overtemp(anim);
}

static void stop_overtemp_animation(struct animation *anim)
{
        anim->run = false;
}

static bool is_overtemp_animation_run(struct animation *anim)
{
        if(anim->run)
                return true;
        else
                return false;
}

// 作用：我们在状态的过程中,充电logo的电量是要增加的，比如电量
// 是20%时，要从第一格开始闪烁；如果是80%时，则要从第三格开始闪烁，
// 电量显示就是通过这个函数来计算实现的。
static void update_screen_state(struct charger *charger, int64_t now)
{
    struct animation *batt_anim = charger->batt_anim;
    int cur_frame;
    int disp_time;

    if (!batt_anim->run || now < charger->next_screen_transition)
        return;

    /* animation is over, blank screen and leave */
    // 当前周期==总的周期，说明动画结束，然后黑屏离开
    if (batt_anim->cur_cycle == batt_anim->num_cycles) {
        reset_animation(batt_anim);     // 动画初始化(周期/帧/运行状态/..)
        charger->next_screen_transition = -1;
        gr_fb_blank(true);              // 黑屏，方便下次操作？
        LOGV("[%lld] animation done\n", now);
        if (charger->num_supplies_online > 0)
            request_suspend(true);
        return;
    }

    disp_time = batt_anim->frames[batt_anim->cur_frame].disp_time;

    /* animation starting, set up the animation */
    /* 启动动画 */
    if (batt_anim->cur_frame == 0) {
        int batt_cap;
        int ret;

        LOGV("[%lld] animation starting\n", now);
        batt_cap = get_battery_capacity(charger);       // 获得电池当前的电量
        if (batt_cap >= 0 && batt_anim->num_frames != 0) {
            int i;

            /* find first frame given current capacity */
            /* 找到第一个大于当前电量值的帧图 */
            for (i = 1; i < batt_anim->num_frames; i++) {
                if (batt_cap < batt_anim->frames[i].min_capacity)
                    break;
            }
            batt_anim->cur_frame = i - 1;   // 这样就是第一个小于当前电量值的帧图

            /* show the first frame for twice as long */
            /* 第一帧显示时间是其默认时间的二倍 */
            disp_time = batt_anim->frames[batt_anim->cur_frame].disp_time * 2;
        }

        batt_anim->capacity = batt_cap;
    }

    /* unblank the screen  on first cycle */
    /* 第一周期不能黑屏了 */
    if (batt_anim->cur_cycle == 0)
        gr_fb_blank(false);

    /* draw the new frame (@ cur_frame) */
    /* 绘制新的一帧图像 */
    redraw_screen(charger);

    /* if we don't have anim frames, we only have one image, so just bump
     * the cycle counter and exit
     */
    if (batt_anim->num_frames == 0 || batt_anim->capacity < 0) {
        LOGV("[%lld] animation missing or unknown battery status\n", now);
        charger->next_screen_transition = now + BATTERY_UNKNOWN_TIME;
        batt_anim->cur_cycle++;
        return;
    }

    // 如果上面执行通过就会直接退出，运行到这里说明电量状态及动画帧数没什么问题
    // ，这里设置静态动画切换时间
    if(batt_anim->freeze == true){
        LOGV("display static battery capacity frame\n");
        charger->next_screen_transition = now + BATTERY_UNKNOWN_TIME;
        batt_anim->cur_cycle++;
        return;
    }

    // 运行到这里说明电池电量状态没问题/动画帧数没问题/设置也不是静态动画
    /* schedule next screen transition */
    /* 动画帧之间的切换时间 */
    charger->next_screen_transition = now + disp_time;

    /* advance frame cntr to the next valid frame
     * if necessary, advance cycle cntr, and reset frame cntr
     */
    /* 指向下一个有效帧 */
    batt_anim->cur_frame++;

    /* if the frame is used for level-only, that is only show it when it's
     * the current level, skip it during the animation.
     */
    /* 如果当前帧仅仅在某个特定等级使用，那就在动画中跳过这一帧。 */
    while (batt_anim->cur_frame < batt_anim->num_frames &&
           batt_anim->frames[batt_anim->cur_frame].level_only)
        batt_anim->cur_frame++;

    // 构成帧循环
    if (batt_anim->cur_frame >= batt_anim->num_frames) {
        batt_anim->cur_cycle++;
        batt_anim->cur_frame = 0;

        /* don't reset the cycle counter, since we use that as a signal
         * in a test above to check if animation is over
         */
        /* 不要清零cycle计数器，因为我们会把这个值当作动画是否结束的信号 */
    }
}

static int set_key_callback(int code, int value, void *data)
{
    struct charger *charger = data;
    int64_t now = curr_time_ms();
    int down = !!value;

    if (code > KEY_MAX)
        return -1;

    /* ignore events that don't modify our state */
    if (charger->keys[code].down == down)
        return 0;

    /* only record the down even timestamp, as the amount
     * of time the key spent not being pressed is not useful */
    if (down)
        charger->keys[code].timestamp = now;
    charger->keys[code].down = down;
    charger->keys[code].pending = true;
    if (down) {
        LOGV("[%lld] key[%d] down\n", now, code);
    } else {
        int64_t duration = now - charger->keys[code].timestamp;
        int64_t secs = duration / 1000;
        int64_t msecs = duration - secs * 1000;
        LOGV("[%lld] key[%d] up (was down for %lld.%lldsec)\n", now,
            code, secs, msecs);
    }

    return 0;
}

static void update_input_state(struct charger *charger,
                               struct input_event *ev)
{
    if (ev->type != EV_KEY)
        return;
    set_key_callback(ev->code, ev->value, charger);
}

static void set_next_key_check(struct charger *charger,
                               struct key_state *key,
                               int64_t timeout)
{
    int64_t then = key->timestamp + timeout;

    if (charger->next_key_check == -1 || then < charger->next_key_check)
        charger->next_key_check = then;
}

static void process_key(struct charger *charger, int code, int64_t now)
{
    struct key_state *key = &charger->keys[code];
    int64_t next_key_check;

    if (code == KEY_POWER) {        // 电源键
        if (key->down) {
            int64_t reboot_timeout = key->timestamp + POWER_ON_KEY_TIME;    // 2s
            if (now >= reboot_timeout) {        // 如果长按power键，就重启，也就是开机
                LOGI("[%lld] rebooting\n", now);
                android_reboot(ANDROID_RB_RESTART, 0, 0);   // 重启，也就是开机
            } else {
                /* if the key is pressed but timeout hasn't expired,
                 * make sure we wake up at the right-ish time to check
                 */
                // 设置下一次的按键检测时间为：按键按下的时间+开机长按时间(2s)
                set_next_key_check(charger, key, POWER_ON_KEY_TIME);
            }
        } else {
            /* if the power key got released, force screen state cycle */
            // 非显示电池损坏动画
            if (key->pending && !is_overtemp_animation_run(&battery_overtemp_animation)) {
                if(!charger->batt_anim->run){       // 当前没有显示充电动画，则显示
                        LOGV("[%lld] show animation!\n", now);
                        request_suspend(false);
                        reset_animation(charger->batt_anim);
                        kick_animation(charger->batt_anim);
                        set_backlight(197);
                }
                else{   // 当前正在显示充电动画，则关闭动画
                        LOGV("[%lld] close animation!\n", now);
                        reset_animation(charger->batt_anim);
                        charger->next_screen_transition = -1;
                        acquire_wake_lock_timeout(WAKEUP_TIME);
                        request_suspend(true);
                }
            }
        }
    }

    key->pending = false;
}

static void handle_input_state(struct charger *charger, int64_t now)
{
    // 调用按键处理函数
    process_key(charger, KEY_POWER, now);

    // 下次检查按键的时间控制
    if (charger->next_key_check != -1 && now > charger->next_key_check)
        // 检查完毕就置为-1，在wait_next_event中设置下次检查的时间
        charger->next_key_check = -1;
}

static void handle_power_supply_state(struct charger *charger, int64_t now)
{
    if (charger->num_supplies_online == 0) {
        // 如果没有在显示电池损坏动画
        if(!is_overtemp_animation_run(&battery_overtemp_animation)){
            request_suspend(false);
            set_backlight(197);
            freeze_animation(charger->batt_anim);
            kick_animation(charger->batt_anim);
        }

        if (charger->next_pwr_check == -1) {
            // 下次DC检测的时间为不插入关机时间(3s)
            charger->next_pwr_check = now + UNPLUGGED_SHUTDOWN_TIME;
            LOGI("[%lld] device unplugged: shutting down in %lld (@ %lld)\n",
                 now, UNPLUGGED_SHUTDOWN_TIME, charger->next_pwr_check);
        } else if (now >= charger->next_pwr_check) {    // 按键DC拔出，则关机
            LOGI("[%lld] shutting down\n", now);
            android_reboot(ANDROID_RB_POWEROFF, 0, 0);
        } else {
            /* otherwise we already have a shutdown timer scheduled */
        }
    } else {
        /* online supply present, reset shutdown timer if set */
        if (charger->next_pwr_check != -1) {
            LOGI("[%lld] device plugged in: shutdown cancelled\n", now);
            if( !is_overtemp_animation_run(&battery_overtemp_animation)){
                thaw_animation(charger->batt_anim);
                kick_animation(charger->batt_anim);
            }
        }
        charger->next_pwr_check = -1;
    }
}


static void handle_leds_state(struct charger *charger, int64_t now)
{
        static int last_num_supplies_online = -1,last_batt_cap = -1;
        static bool last_temp_valid = true;

        int batt_cap = get_battery_capacity(charger);
        bool temp_valid = IS_TEMP_VALID(get_battery_temperature(charger));

        if(last_num_supplies_online != charger->num_supplies_online ||
                last_batt_cap != batt_cap || last_temp_valid != temp_valid){
                if(charger->num_supplies_online == 0 || !temp_valid)    //temp over charge threshold
                        setLight(0x0,0,0,0,0);
                else if(batt_cap < BATTERY_FULL_THRESH)
                        setLight(0xffff0000,0,0,0,0);
                else
                        setLight(0xff00ff00,0,0,0,0);
        }
        last_num_supplies_online = charger->num_supplies_online;
        last_batt_cap = batt_cap;
        last_temp_valid = temp_valid;
}

static void handle_battery_state(struct charger *charger, int64_t now)
{
        static bool last_temp_valid = true;
        struct animation *batt_overtemp_anim = &battery_overtemp_animation;

        bool temp_valid = IS_TEMP_VALID(get_battery_temperature(charger));
        LOGV("batt_temp=%d\n",get_battery_temperature(charger));
        if(last_temp_valid != temp_valid){
                if(!temp_valid){
                        LOGV("temp_valid=%d! last_temp_valid=%d\n",temp_valid,last_temp_valid);
                        request_suspend(false);
                        reset_animation(charger->batt_anim);
                        charger->next_screen_transition = -1;
                        kick_overtemp_animation(batt_overtemp_anim,0);
                        set_backlight(197);
                }
                else{
                        LOGV("temp_valid=%d! last_temp_valid=%d\n",temp_valid,last_temp_valid);
                        request_suspend(false);
                        reset_animation(charger->batt_anim);
                        charger->next_screen_transition = now + batt_overtemp_anim->frames[batt_overtemp_anim->cur_frame].disp_time;
                        kick_overtemp_animation(batt_overtemp_anim,1);
                        set_backlight(197);
                }
        }
        else if(temp_valid && is_overtemp_animation_run(batt_overtemp_anim) && now >= charger->next_screen_transition && charger->next_screen_transition != -1){
                LOGV("Change to bat animation\n");
                stop_overtemp_animation(batt_overtemp_anim);
                reset_animation(charger->batt_anim);
                kick_animation(charger->batt_anim);
                charger->next_screen_transition = now -1;
        }
        last_temp_valid = temp_valid;
}

static void wait_next_event(struct charger *charger, int64_t now)
{
    int64_t next_event = INT64_MAX;
    int64_t timeout;
    struct input_event ev;
    int ret;

    LOGV("[%lld] next screen: %lld next key: %lld next pwr: %lld\n", now,
         charger->next_screen_transition, charger->next_key_check,
         charger->next_pwr_check);

    if (charger->next_screen_transition != -1)
        next_event = charger->next_screen_transition;
    if (charger->next_key_check != -1 && charger->next_key_check < next_event)
        next_event = charger->next_key_check;
    if (charger->next_pwr_check != -1 && charger->next_pwr_check < next_event)
        next_event = charger->next_pwr_check;

    if (next_event != -1 && next_event != INT64_MAX)
        timeout = max(0, next_event - now);
    else
        timeout = -1;
    LOGV("[%lld] blocking (%lld)\n", now, timeout);
    ret = ev_wait((int)timeout);
    if (!ret)
        ev_dispatch();
}

static int input_callback(int fd, short revents, void *data)
{
    struct charger *charger = data;
    struct input_event ev;
    int ret;

    ret = ev_get_input(fd, revents, &ev);
    if (ret)
        return -1;
    update_input_state(charger, &ev);
    return 0;
}

// 这个函数判断按键状态，DC是否插拔。
// 如果长按开机：执行android_reboot(ANDROID_RB_RESTART,0, 0);
// 如果拔出DC:执行android_reboot(ANDROID_RB_POWEROFF,0, 0);
static void event_loop(struct charger *charger)
{
    int ret;

    while (true) {
        // 这个时间来判断，有没有屏幕超时，如果超时关闭屏幕充电logo显示。
        int64_t now = curr_time_ms();       // (1)、获得当前时间

        LOGV("[%lld] event_loop()\n", now);
        handle_battery_state(charger, now);
        handle_input_state(charger, now);   // (2)、检查按键状态
        handle_power_supply_state(charger, now);    // (3)、检查DC是否拔出
        handle_leds_state(charger, now);

        /* do screen update last in case any of the above want to start
         * screen transitions (animations, etc)
         */
        // (4)、对按键时间状态标志位的判断，显示不同电量的充电logo
        update_screen_state(charger, now);

        wait_next_event(charger, now);
    }
}

static void init_shutdown_alarm(void)
{
    long alarm_secs;
    struct timeval now_tv = { 0 };
    alarm_fd = open("/dev/alarm", O_RDWR);
    if (alarm_fd < 0) {
        LOGE("open /dev/alarm failed, ret=%d\n", alarm_fd);
		return;
    }

    alarm_secs = get_wakealarm_sec();
    if (alarm_secs > 0) {
        struct timespec ts;

        gettimeofday(&now_tv, NULL);

        LOGI("gettimeofday, sec=%ld, microsec=%ld, alarm_secs=%ld\n",
          (long)now_tv.tv_sec, (long)now_tv.tv_usec, alarm_secs);

        ts.tv_sec = alarm_secs;
        ts.tv_nsec = 0;

        ioctl(alarm_fd, ANDROID_ALARM_SET(ANDROID_ALARM_RTC_SHUTDOWN_WAKEUP), &ts);

        pthread_create(&tid_alarm, NULL, alarm_thread_handler, NULL);
    }

    return;
}

int main(int argc, char **argv)
{
    int ret;
    struct charger *charger = &charger_state;
    int64_t now = curr_time_ms() - 1;
    int fd;
    int i;
    char buf_value[PROPERTY_VALUE_MAX];

    list_init(&charger->supplies);

    klog_init();
    klog_set_level(CHARGER_KLOG_LEVEL);

    dump_last_kmsg();

    LOGI("--------------- STARTING CHARGER MODE ---------------\n");

    gr_init();
    gr_font_size(&char_width, &char_height);    // (1)、初始化graphics，包括buf大小

    ev_init(input_callback, charger);           // (2)、初始化按键

    fd = uevent_open_socket(64*1024, true);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        ev_add_fd(fd, uevent_callback, charger);
    }
    charger->uevent_fd = fd;
    // (3)、创建/sys/class/power_supply结点，把socket信息通知应用层
    // uevent_open_socket这个函数是通过kobject_uevent的方式通知的应用层，就是往一个socket
    // 广播一个消息，只需要在应用层打开socket监听NETLINK_KOBJECT_UEVENT组的消息，就可以收
    // 到了,主要是创建了socket接口获得uevent的文件描述符，然后触发/sys/class/power_supply
    // 目录及其子目录下的uevent，然后接受并创建设备节点，至此设备节点才算创建。
    coldboot(charger, "/sys/class/power_supply", "add");

    ret = res_create_surface("charger/battery_fail", &charger->surf_unknown);
    if (ret < 0) {
        LOGE("Cannot load image\n");
        charger->surf_unknown = NULL;
    }

    // (4)、这里是显示charger log，res_create_surface是显示图片函数
    for (i = 0; i < charger->batt_anim->num_frames; i++) {
        struct frame *frame = &charger->batt_anim->frames[i];

        ret = res_create_surface(frame->name, &frame->surface);
        if (ret < 0) {
            LOGE("Cannot load image %s\n", frame->name);
            /* TODO: free the already allocated surfaces... */
            charger->batt_anim->num_frames = 0;
            charger->batt_anim->num_cycles = 1;
            break;
        }
    }

    // 这里是显示电池损坏动画？？？
    for (i = 0; i < battery_overtemp_animation.num_frames; i++) {
        struct frame *frame = &battery_overtemp_animation.frames[i];

        ret = res_create_surface(frame->name, &frame->surface);
        if (ret < 0) {
            LOGE("Cannot load image %s\n", frame->name);
            /* TODO: free the already allocated surfaces... */
            battery_overtemp_animation.num_frames = 0;
            battery_overtemp_animation.num_cycles = 1;
            break;
        }
    }
    // 更新按键状态
    ev_sync_key_state(set_key_callback, charger);

#ifndef CHARGER_DISABLE_INIT_BLANK
    gr_fb_blank(true);
#endif

    charger->next_screen_transition = now - 1;
    charger->next_key_check = -1;
    charger->next_pwr_check = -1;
    reset_animation(charger->batt_anim);
    kick_animation(charger->batt_anim);
    porbe_light_leds();

    init_shutdown_alarm();

    event_loop(charger);    // (5)、event_loop循环，电池状态，检测按键是否按下

    return 0;
}
