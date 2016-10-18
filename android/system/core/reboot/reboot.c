/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <cutils/properties.h>
#include <cutils/android_reboot.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    int ret;
    size_t prop_len;
    char property_val[PROPERTY_VALUE_MAX];
    const char *cmd = "reboot";
    char *optarg = "";

    opterr = 0;
    do {
        int c;

        // #include<unistd.h>
        // int getopt(int argc,char * const argv[ ],const char * optstring);
        // 分析命令行参数，详细用法见百度百科，这里代表解析'p'参数
        // extern char *optarg;指向当前选项参数（如果有）的指针
        // extern int optind, opterr, optopt;
        // optind -- 再次调用getopt()时的下一个argv指针的索引。
        // optopt -- 最后一个未知选项。
        c = getopt(argc, argv, "p");

        if (c == EOF) {
            break;
        }

        switch (c) {
        case 'p':
            cmd = "shutdown";
            break;
        case '?':
            fprintf(stderr, "usage: %s [-p] [rebootcommand]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
    } while (1);

    // 下面两个if判断是限定: iptind < argc <= iptind+1, 也就是argc=iptind+1
    // argc != max(optind)+1, argc包含了参数，optind中命令和参数是一个整体
    if(argc > optind + 1) {
        fprintf(stderr, "%s: too many arguments\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    if (argc > optind)
        optarg = argv[optind];      // 那这里是去最后一个参数表？(-p)

    // int snprintf(char *str, size_t size, const char *format, ...)
    // 这里构建了命令行放在property_val: cmd, optarg
    prop_len = snprintf(property_val, sizeof(property_val), "%s,%s", cmd, optarg);
    if (prop_len >= sizeof(property_val)) {
        fprintf(stderr, "reboot command too long: %s\n", optarg);
        exit(EXIT_FAILURE);
    }

    // 最终的目的也就是设置了这个属性值
    ret = property_set(ANDROID_RB_PROPERTY, property_val);
    if(ret < 0) {
        perror("reboot");
        exit(EXIT_FAILURE);
    }

    // Don't return early. Give the reboot command time to take effect
    // to avoid messing up scripts which do "adb shell reboot && adb wait-for-device"
    // 不要过早结束，要给出足够的时间让重启命令生效以避免脚本混乱
    // (adb shell reboot && adb wait-for-device)
    while(1) { pause(); }

    fprintf(stderr, "Done\n");
    return 0;
}
