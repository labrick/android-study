#!/bin/bash

echo "start update android ..."
cd android/system
if [ ! -d ".tmp" ]; then
    mkdir .tmp
fi
if [ -d ".tmp/core" ]; then
    rm -rf .tmp/core
fi
mv -f core .tmp/core
cp -r /home/lab302/robot/R16-Parrot_SDK/android/system/core/ ./
cd ../../
echo "update android ok"

echo "start update linux ..."
sleep 1
cd lichee/linux-3.4/arch/arm
if [ ! -d ".tmp" ]; then
    mkdir .tmp
fi
if [ -d ".tmp/mach-sunxi" ]; then
    rm -rf .tmp/mach-sunxi
fi
mv -f mach-sunxi .tmp/mach-sunxi
cp -r /home/lab302/robot/R16-Parrot_SDK/lichee/linux-3.4/arch/arm/mach-sunxi/ ./
cd ../../../../
echo "update android ok"
echo
echo "---------------------check code changed-----------------"
git status
