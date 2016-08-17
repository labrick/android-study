#!/bin/bash

LINUXDIR=/mnt/hdd2/ProjectGroup/R16-Parrot_SDK
ANDROIDDIR=/home/lab302/robot/R16-Parrot_SDK

echo "start update android ..."
cd android/system
if [ ! -d ".tmp" ]; then
    mkdir .tmp
fi
if [ -d ".tmp/core" ]; then
    rm -rf .tmp/core
fi
mv -f core .tmp/core
cp -r $ANDROIDDIR/android/system/core/ ./
cd ../../
echo "update android ok"

echo "start update linux ..."
sleep 1
cd lichee/linux-3.4/arch/
if [ ! -d ".tmp" ]; then
    mkdir .tmp
fi
if [ -d ".tmp/arm" ]; then
    rm -rf .tmp/arm
fi
mv -f arm .tmp/arm
cp -r $LINUXDIR/lichee/linux-3.4/arch/arm/ ./
cd ../../../
echo "update android ok"
echo
echo "---------------------check code changed-----------------"
git status
