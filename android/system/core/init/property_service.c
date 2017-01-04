/*
 * Copyright (C) 2007 The Android Open Source Project
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>

#include <cutils/misc.h>
#include <cutils/sockets.h>
#include <cutils/multiuser.h>

#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/atomics.h>
#include <private/android_filesystem_config.h>

#include <selinux/selinux.h>
#include <selinux/label.h>

#include "property_service.h"
#include "init.h"
#include "util.h"
#include "log.h"

#define PERSISTENT_PROPERTY_DIR  "/data/property"

static int persistent_properties_loaded = 0;
static int property_area_inited = 0;

static int property_set_fd = -1;


#ifdef AW_BOOSTUP_ENABLE
int aw_boost_up_perf(const char *name, const char *value);
int aw_init_boostup();
#endif

/* White list of permissions for setting property services. */
struct {
    const char *prefix;
    unsigned int uid;
    unsigned int gid;
} property_perms[] = {
    { "net.rmnet0.",      AID_RADIO,    0 },
    { "net.gprs.",        AID_RADIO,    0 },
    { "net.ppp",          AID_RADIO,    0 },
    { "net.qmi",          AID_RADIO,    0 },
    { "net.lte",          AID_RADIO,    0 },
    { "net.cdma",         AID_RADIO,    0 },
    { "ril.",             AID_RADIO,    0 },
    { "gsm.",             AID_RADIO,    0 },
    { "persist.radio",    AID_RADIO,    0 },
    { "net.dns",          AID_RADIO,    0 },
    { "sys.usb.config",   AID_RADIO,    0 },
    { "net.",             AID_SYSTEM,   0 },
    { "dev.",             AID_SYSTEM,   0 },
    { "runtime.",         AID_SYSTEM,   0 },
    { "hw.",              AID_SYSTEM,   0 },
    { "sys.",             AID_SYSTEM,   0 },
    { "sys.powerctl",     AID_SHELL,    0 },
    { "service.",         AID_SYSTEM,   0 },
    { "wlan.",            AID_SYSTEM,   0 },
    { "bluetooth.",       AID_BLUETOOTH,   0 },
    { "dhcp.",            AID_SYSTEM,   0 },
    { "dhcp.",            AID_DHCP,     0 },
    { "debug.",           AID_SYSTEM,   0 },
    { "debug.",           AID_SHELL,    0 },
    { "log.",             AID_SHELL,    0 },
    { "service.adb.root", AID_SHELL,    0 },
    { "service.adb.tcp.port", AID_SHELL,    0 },
    { "persist.sys.",     AID_SYSTEM,   0 },
    { "persist.service.", AID_SYSTEM,   0 },
    { "persist.security.", AID_SYSTEM,   0 },
    { "persist.service.bdroid.", AID_BLUETOOTH,   0 },
    { "selinux."         , AID_SYSTEM,   0 },
    { "wfd.enable",        AID_MEDIA,    0 },
    { NULL, 0, 0 }
};

/*
 * White list of UID that are allowed to start/stop services.
 * Currently there are no user apps that require.
 */
struct {
    const char *service;
    unsigned int uid;
    unsigned int gid;
} control_perms[] = {
    { "dumpstate",AID_SHELL, AID_LOG },
    { "ril-daemon",AID_RADIO, AID_RADIO },
     {NULL, 0, 0 }
};

typedef struct {
    size_t size;
    int fd;
} workspace;

static int init_workspace(workspace *w, size_t size)
{
    /* dev is a tmpfs that we can use to carve a shared workspace
     * out of, so let's do that...
     */
    void *data;
    // 创建一个临时的设备
    int fd = open(PROP_FILENAME, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    
    // 下面怎么少了一部分代码？被移到其他地方去了？
    // 从kernel中映射出一块内存
/*  data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    fd = open("/dev/__properties__", O_RDONLY);
    // 删除掉临时的设备，但是映射的内存还是存在，fd被保存下来，而且只有data这个mmap有读写的权限
    // 其它人得到的fd都是只读的，而且/dev/__properties__已经被删除了，其它人已经不能在得到写权限了。
    unlink("/dev/__properties__");
    w->data = data; */

    w->size = size;
    w->fd = fd;
    return 0;
}

static workspace pa_workspace;

static int init_property_area(void)
{
    // 如果已经初始化了就直接返回
    if (property_area_inited)
        return -1;

    if(__system_property_area_init())
        return -1;

    // 初始化workspace，得到shared memory的fd, size, data
    if(init_workspace(&pa_workspace, 0))
        return -1;

    // pa指向shared memory的头，实际就是头信息，并初始化
    fcntl(pa_workspace.fd, F_SETFD, FD_CLOEXEC);

    property_area_inited = 1;
    return 0;
}

static int check_mac_perms(const char *name, char *sctx)
{
    if (is_selinux_enabled() <= 0)
        return 1;

    char *tctx = NULL;
    const char *class = "property_service";
    const char *perm = "set";
    int result = 0;

    if (!sctx)
        goto err;

    if (!sehandle_prop)
        goto err;

    if (selabel_lookup(sehandle_prop, &tctx, name, 1) != 0)
        goto err;

    if (selinux_check_access(sctx, tctx, class, perm, name) == 0)
        result = 1;

    freecon(tctx);
 err:
    return result;
}

static int check_control_mac_perms(const char *name, char *sctx)
{
    /*
     *  Create a name prefix out of ctl.<service name>
     *  The new prefix allows the use of the existing
     *  property service backend labeling while avoiding
     *  mislabels based on true property prefixes.
     */
    char ctl_name[PROP_VALUE_MAX+4];
    int ret = snprintf(ctl_name, sizeof(ctl_name), "ctl.%s", name);

    if (ret < 0 || (size_t) ret >= sizeof(ctl_name))
        return 0;

    return check_mac_perms(ctl_name, sctx);
}

/*
 * Checks permissions for starting/stoping system services.
 * AID_SYSTEM and AID_ROOT are always allowed.
 *
 * Returns 1 if uid allowed, 0 otherwise.
 */
static int check_control_perms(const char *name, unsigned int uid, unsigned int gid, char *sctx) {

    int i;
    if (uid == AID_SYSTEM || uid == AID_ROOT)
      return check_control_mac_perms(name, sctx);

    /* Search the ACL */
    for (i = 0; control_perms[i].service; i++) {
        if (strcmp(control_perms[i].service, name) == 0) {
            if ((uid && control_perms[i].uid == uid) ||
                (gid && control_perms[i].gid == gid)) {
                return check_control_mac_perms(name, sctx);
            }
        }
    }
    return 0;
}

/*
 * Checks permissions for setting system properties.
 * Returns 1 if uid allowed, 0 otherwise.
 */
static int check_perms(const char *name, unsigned int uid, unsigned int gid, char *sctx)
{
    int i;
    unsigned int app_id;

    if(!strncmp(name, "ro.", 3))
        name +=3;

    if (uid == 0)
        return check_mac_perms(name, sctx);

    app_id = multiuser_get_app_id(uid);
    if (app_id == AID_BLUETOOTH) {
        uid = app_id;
    }

    for (i = 0; property_perms[i].prefix; i++) {
        if (strncmp(property_perms[i].prefix, name,
                    strlen(property_perms[i].prefix)) == 0) {
            if ((uid && property_perms[i].uid == uid) ||
                (gid && property_perms[i].gid == gid)) {

                return check_mac_perms(name, sctx);
            }
        }
    }

    return 0;
}

// 直接调用的Bionic中的__system_property_find，然后直接将value返回
int __property_get(const char *name, char *value)
{
    return __system_property_get(name, value);
}

static void write_persistent_property(const char *name, const char *value)
{
    char tempPath[PATH_MAX];
    char path[PATH_MAX];
    int fd;

    snprintf(tempPath, sizeof(tempPath), "%s/.temp.XXXXXX", PERSISTENT_PROPERTY_DIR);
    fd = mkstemp(tempPath);
    if (fd < 0) {
        ERROR("Unable to write persistent property to temp file %s errno: %d\n", tempPath, errno);
        return;
    }
    write(fd, value, strlen(value));
    close(fd);

    snprintf(path, sizeof(path), "%s/%s", PERSISTENT_PROPERTY_DIR, name);
    if (rename(tempPath, path)) {
        unlink(tempPath);
        ERROR("Unable to rename persistent property file %s to %s\n", tempPath, path);
    }
}

static bool is_legal_property_name(const char* name, size_t namelen)
{
    size_t i;
    bool previous_was_dot = false;
    if (namelen >= PROP_NAME_MAX) return false;
    if (namelen < 1) return false;
    if (name[0] == '.') return false;
    if (name[namelen - 1] == '.') return false;

    /* Only allow alphanumeric, plus '.', '-', or '_' */
    /* Don't allow ".." to appear in a property name */
    for (i = 0; i < namelen; i++) {
        if (name[i] == '.') {
            if (previous_was_dot == true) return false;
            previous_was_dot = true;
            continue;
        }
        previous_was_dot = false;
        if (name[i] == '_' || name[i] == '-') continue;
        if (name[i] >= 'a' && name[i] <= 'z') continue;
        if (name[i] >= 'A' && name[i] <= 'Z') continue;
        if (name[i] >= '0' && name[i] <= '9') continue;
        return false;
    }

    return true;
}

int property_set(const char *name, const char *value)
{
    prop_info *pi;
    int ret;

    size_t namelen = strlen(name);
    size_t valuelen = strlen(value);

    if (!is_legal_property_name(name, namelen)) return -1;
    if (valuelen >= PROP_VALUE_MAX) return -1;

    pi = (prop_info*) __system_property_find(name);

    if(pi != 0) {
        /* ro.* properties may NEVER be modified once set */
        // ro，只读的property只能被设定一次
        if(!strncmp(name, "ro.", 3)) return -1;

        __system_property_update(pi, value, valuelen);
    } else {
        // 将key和value写入到shared memory中
        ret = __system_property_add(name, namelen, value, valuelen);
        if (ret < 0) {
            ERROR("Failed to set '%s'='%s'\n", name, value);
            return ret;
        }
    }
    /* If name starts with "net." treat as a DNS property. */
    if (strncmp("net.", name, strlen("net.")) == 0)  {
        if (strcmp("net.change", name) == 0) {
            return 0;
        }
       /*
        * The 'net.change' property is a special property used track when any
        * 'net.*' property name is updated. It is _ONLY_ updated here. Its value
        * contains the last updated 'net.*' property.
        */
        property_set("net.change", name);
    } else if (persistent_properties_loaded &&
            strncmp("persist.", name, strlen("persist.")) == 0) {
            char bootmode[PROP_VALUE_MAX];
            ret = property_get("ro.bootmode", bootmode);
            if (ret <= 0 || (strcmp(bootmode, "charger") != 0))         //do not write prop while charger mode
        /*
         * Don't write properties to disk until after we have read all default properties
         * to prevent them from being overwritten by default values.
         */
        write_persistent_property(name, value);
    } else if (strcmp("selinux.reload_policy", name) == 0 &&
               strcmp("1", value) == 0) {
        selinux_reload_policy();
    }
    property_changed(name, value);

#ifdef AW_BOOSTUP_ENABLE
    aw_boost_up_perf(name, value);
#endif
    return 0;
}

// property_service中property_set，对于动态调用时，client实际调用的是
// handle_property_set_fd()，而不是property_set
void handle_property_set_fd()
{
    prop_msg msg;
    int s;
    int r;
    int res;
    struct ucred cr;
    struct sockaddr_un addr;
    socklen_t addr_size = sizeof(addr);
    socklen_t cr_size = sizeof(cr);
    char * source_ctx = NULL;

    if ((s = accept(property_set_fd, (struct sockaddr *) &addr, &addr_size)) < 0) {
        return;
    }

    /* Check socket options here */
    // 先从套接字获取SO_PEERCRED值，以便检查传递信息的进程的访问权限
    if (getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cr_size) < 0) {
        close(s);
        ERROR("Unable to receive socket options\n");
        return;
    }

    r = TEMP_FAILURE_RETRY(recv(s, &msg, sizeof(msg), 0));
    if(r != sizeof(prop_msg)) {
        ERROR("sys_prop: mis-match msg size received: %d expected: %d errno: %d\n",
              r, sizeof(prop_msg), errno);
        close(s);
        return;
    }

    switch(msg.cmd) {
    case PROP_MSG_SETPROP:
        // 对于用户动态调用的property_set，server会收到从Bionic中发送的
        // PROP_MSG_SETPROP命令，收到后，检查权限，然后调用property_set进行设置。
        msg.name[PROP_NAME_MAX-1] = 0;
        msg.value[PROP_VALUE_MAX-1] = 0;

        if (!is_legal_property_name(msg.name, strlen(msg.name))) {
            ERROR("sys_prop: illegal property name. Got: \"%s\"\n", msg.name);
            close(s);
            return;
        }

        getpeercon(s, &source_ctx);

        // 以ctl开头的消息并非请求更改属性值，而是请求进程启动与终止消息，也就是控制命令
        if(memcmp(msg.name,"ctl.",4) == 0) {
            // Keep the old close-socket-early behavior when handling
            // ctl.* properties.
            close(s);
            // 检查访问权限，仅有system server/root以及相关进程才能使用ctl消息终止或启动进程
            if (check_control_perms(msg.value, cr.uid, cr.gid, source_ctx)) {
                handle_control_message((char*) msg.name + 4, (char*) msg.value);
            } else {
                ERROR("sys_prop: Unable to %s service ctl [%s] uid:%d gid:%d pid:%d\n",
                        msg.name + 4, msg.value, cr.uid, cr.gid, cr.pid);
            }
        } else {
            // 检查访问权限
            if (check_perms(msg.name, cr.uid, cr.gid, source_ctx)) {
                property_set((char*) msg.name, (char*) msg.value);  // 更改系统属性值
            } else {
                ERROR("sys_prop: permission denied uid:%d  name:%s\n",
                      cr.uid, msg.name);
            }

            // Note: bionic's property client code assumes that the
            // property server will not close the socket until *AFTER*
            // the property is written to memory.
            close(s);
        }
        freecon(source_ctx);
        break;

    default:
        close(s);
        break;
    }
}

void get_property_workspace(int *fd, int *sz)
{
    *fd = pa_workspace.fd;
    *sz = pa_workspace.size;
}

int get_dram_size(void)
{
    char *path = "/proc/meminfo";
    FILE *fd;
    char data[128];
    char *key, *value, *tmp;
    int total, dram_size = 1024;

    fd = fopen(path, "r");
    if (fd == NULL) {
        ERROR("cannot open %s\n", path);
        goto oops;
    }

    while (fgets(data, sizeof(data), fd)) {
        key = data;
        value = strchr(key, ':');
        if (value == 0)
            continue;
        *value++ = 0;

        if (strcmp(key, "MemTotal"))
            continue; /* should not be here */

        while (isspace(*value))
            value++;

        tmp = strchr(value, ' ');
        *tmp = 0;
        INFO("MemTotal: %sKB\n", value);
        total = atoi(value);
        dram_size = total/1024;

        break;
    }

    fclose(fd);
oops:
    return dram_size;
}

static int enable_adaptive_memory(void)
{
    char buf[PROP_VALUE_MAX] = {0};
    if(property_get("ro.memopt.disable", buf) && !strcmp(buf,"true")){
        INFO("disable adaptive memory function\n");
        return -1;
    }

    //for memory > 1024,
    if (get_dram_size() > 512) {
        property_set("dalvik.vm.heapsize", "384m");
        property_set("dalvik.vm.heapstartsize", "8m");
        property_set("dalvik.vm.heapgrowthlimit", "96m");
        property_set("dalvik.vm.heapminfree", "2m");
        property_set("dalvik.vm.heapmaxfree", "8m");
        property_set("sys.mem.opt", "false");
        property_set("ro.config.low_ram", "false");
    } else {
        property_set("dalvik.vm.heapsize", "184m");
        property_set("dalvik.vm.heapstartsize", "5m");
        property_set("dalvik.vm.heapgrowthlimit", "48m");
        property_set("dalvik.vm.heapminfree", "512K");
        property_set("dalvik.vm.heapmaxfree", "2m");
        //aw use
        if(strcmp(buf,"true")){
            property_set("sys.mem.opt", "true");
        }
        property_set("ro.config.low_ram", "true");
    }
    return 0;
}

extern bool usb_charge_flag;
static void load_properties(char *data)
{
    char *key, *value, *eol, *sol, *tmp;

    sol = data;
    while((eol = strchr(sol, '\n'))) {
        key = sol;
        *eol++ = 0;
        sol = eol;

        value = strchr(key, '=');
        if(value == 0) continue;
        *value++ = 0;

        while(isspace(*key)) key++;
        if(*key == '#') continue;
        tmp = value - 2;
        while((tmp > key) && isspace(*tmp)) *tmp-- = 0;

        while(isspace(*value)) value++;
        tmp = eol - 2;
        while((tmp > value) && isspace(*tmp)) *tmp-- = 0;

        if (usb_charge_flag && !strcmp(key, "persist.sys.usb.config")) {
            INFO("wanglei: Load properties --> key = %s, value = %s, so nothing to do\n", key, value);  
        }else {
            property_set(key, value);
        }
    }
    
    enable_adaptive_memory();
}

static void load_properties_from_file(const char *fn)
{
    char *data;
    unsigned sz;

    data = read_file(fn, &sz);

    if(data != 0) {
        load_properties(data);
        free(data);
    }
}

static void load_persistent_properties()
{
    DIR* dir = opendir(PERSISTENT_PROPERTY_DIR);
    int dir_fd;
    struct dirent*  entry;
    char value[PROP_VALUE_MAX];
    int fd, length;
    struct stat sb;

    if (dir) {
        dir_fd = dirfd(dir);
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp("persist.", entry->d_name, strlen("persist.")))
                continue;
#if HAVE_DIRENT_D_TYPE
            if (entry->d_type != DT_REG)
                continue;
#endif
            /* open the file and read the property value */
            fd = openat(dir_fd, entry->d_name, O_RDONLY | O_NOFOLLOW);
            if (fd < 0) {
                ERROR("Unable to open persistent property file \"%s\" errno: %d\n",
                      entry->d_name, errno);
                continue;
            }
            if (fstat(fd, &sb) < 0) {
                ERROR("fstat on property file \"%s\" failed errno: %d\n", entry->d_name, errno);
                close(fd);
                continue;
            }

            // File must not be accessible to others, be owned by root/root, and
            // not be a hard link to any other file.
            if (((sb.st_mode & (S_IRWXG | S_IRWXO)) != 0)
                    || (sb.st_uid != 0)
                    || (sb.st_gid != 0)
                    || (sb.st_nlink != 1)) {
                ERROR("skipping insecure property file %s (uid=%lu gid=%lu nlink=%d mode=%o)\n",
                      entry->d_name, sb.st_uid, sb.st_gid, sb.st_nlink, sb.st_mode);
                close(fd);
                continue;
            }

            memset(value, 0, PROP_VALUE_MAX);

            if (usb_charge_flag && !strcmp(entry->d_name, "persist.sys.usb.config")) {
                property_set(entry->d_name, "mass_storage");
                INFO("wanglei: set persist.sys.usb.config to mass_storage\n");
            } else{
                length = read(fd, value, sizeof(value) - 1);
                if (length >= 0) {
                    property_set(entry->d_name, value);
                } else {
                    ERROR("Unable to read persistent property file %s errno: %d\n",
                          entry->d_name, errno);
                }
            }
            close(fd);
        }
        closedir(dir);
    } else {
        ERROR("Unable to open persistent property directory %s errno: %d\n", PERSISTENT_PROPERTY_DIR, errno);
    }

    persistent_properties_loaded = 1;
}

void property_init(void)
{
    init_property_area();
}

void property_load_boot_defaults(void)
{
    load_properties_from_file(PROP_PATH_RAMDISK_DEFAULT);
}

int properties_inited(void)
{
    return property_area_inited;
}

static void load_override_properties() {
#ifdef ALLOW_LOCAL_PROP_OVERRIDE
    char debuggable[PROP_VALUE_MAX];
    int ret;

    ret = property_get("ro.debuggable", debuggable);
    if (ret && (strcmp(debuggable, "1") == 0)) {
        load_properties_from_file(PROP_PATH_LOCAL_OVERRIDE);
    }
#endif /* ALLOW_LOCAL_PROP_OVERRIDE */
}


/* When booting an encrypted system, /data is not mounted when the
 * property service is started, so any properties stored there are
 * not loaded.  Vold triggers init to load these properties once it
 * has mounted /data.
 */
void load_persist_props(void)
{
    load_override_properties();
    /* Read persistent properties after all default values have been loaded. */
    load_persistent_properties();
}

void start_property_service(void)
{
    int fd;

    // load_properties_from_file就是从文件中读取key和value，并把他们property_set(key, value);
    load_properties_from_file(PROP_PATH_SYSTEM_BUILD);      // 获取之前的几个属性值
    load_properties_from_file(PROP_PATH_SYSTEM_DEFAULT);
    load_override_properties();
    /* Read persistent properties after all default values have been loaded. */
    load_persistent_properties();                 // 读取/data/property目录路中的属性值

    // 创建套接字，以便init进程在收到子进程终止的SIGCHLD信号时调用相应的handler
    // PROP_SERVICE_NAME很熟悉的名字，在Bionic中的send_prop_msg时会连接这个属性并发送命令，
    // server端就是在这里实现的
    fd = create_socket(PROP_SERVICE_NAME, SOCK_STREAM, 0666, 0, 0);
    if(fd < 0) return;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, O_NONBLOCK);

    listen(fd, 8);
    property_set_fd = fd;
#ifdef AW_BOOSTUP_ENABLE
    aw_init_boostup();
#endif

}

int get_property_set_fd()
{
    return property_set_fd;
}
