#!/bin/bash

# 注意字符串截取采用的是SDK/作为分隔符

LINUXDIR=/mnt/hdd2/ProjectGroup/R16-Parrot_SDK
ANDROIDDIR=/home/lab302/robot/R16-Parrot_SDK

update_dir_list=(
${ANDROIDDIR}/android/system/core
${ANDROIDDIR}/android/frameworks/base/core
${ANDROIDDIR}/android/frameworks/base/services
${LINUXDIR}/lichee/linux-3.4/arch/arm
${LINUXDIR}/lichee/tools/pack
)

function update_error()
{
    echo -e "\033[47;31mERROR: $*\033[0m"
}

function update_warn()
{
    echo -e "\033[47;34mWARN: $*\033[0m"
}

function update_info()
{
    echo -e "\033[47;30mINFO: $*\033[0m"
}

if [ $(basename `pwd`) != "android-study" ] ; then
    echo "Please run at the top directory of anroid-study"
    exit 1
fi

for update_dir in ${update_dir_list[@]}; do
    BASENAME=$(basename ${update_dir})
    # ${update_dir#*SDK/}   #号是运算符，*SDK/表示从左边开始删除第一个SDK/号及左边的所有字符
    DIRNAME=$(dirname ${update_dir#*SDK/})
    
    echo ${update_dir}
    echo ${DIRNAME}
    echo ${BASENAME}

    if [ ! -d ${update_dir} ]; then
        update_error "${update_dir} doesn't exit!!"
        exit 1
    fi
    
    rm -rf .tmp/${DIRNAME}
    mkdir -p .tmp/${DIRNAME} ${DIRNAME}
    
    echo "start update ${update_dir} ..." 
    sleep 1
    if [ -d ".tmp/${DIRNAME}/${BASENAME}" ]; then
        rm -rf .tmp/${DIRNAME}/${BASENAME}
    fi
    mv -f ${DIRNAME}/${BASENAME} .tmp/${DIRNAME}
    cp -r ${update_dir} ${DIRNAME}
    echo "update ${update_dir} ok"
done

echo
echo "---------------------check code changed-----------------"
git status
