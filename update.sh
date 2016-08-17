#!/bin/bash

LINUXDIR=/mnt/hdd2/ProjectGroup/R16-Parrot_SDK
ANDROIDDIR=/home/lab302/robot/R16-Parrot_SDK

#-------------------------------------------------
ANDROIDFILE=core
ANDROIDSUBDIR=android/system
mkdir -p .tmp/$ANDROIDSUBDIR

echo "start update $ANDROIDDIR/$ANDROIDSUBDIR/$ANDROIDFILE ..." 
sleep 1
if [ -d ".tmp/$ANDROIDSUBDIR/$ANDROIDFILE" ]; then
    rm -rf .tmp/$ANDROIDSUBDIR/$ANDROIDFILE
fi
mv -f $ANDROIDSUBDIR/$ANDROIDFILE .tmp/$ANDROIDSUBDIR
cp -r $ANDROIDDIR/$ANDROIDSUBDIR/$ANDROIDFILE $ANDROIDSUBDIR
echo "update $ANDROIDDIR/$ANDROIDSUBDIR/$ANDROIDFILE ok"

#-------------------------------------------------
LINUXFILE=arm
LINUXSUBDIR=lichee/linux-3.4/arch
mkdir -p .tmp/$LINUXSUBDIR

echo "start update $LINUXDIR/$LINUXSUBDIR/$LINUXFILE ..."
sleep 1
if [ -d ".tmp/$LINUXSUBDIR/$LINUXFILE" ]; then
    rm -rf .tmp/$LINUXSUBDIR/$LINUXFILE
fi
mv -f $LINUXSUBDIR/$LINUXFILE .tmp/$LINUXSUBDIR
cp -r $LINUXDIR/$LINUXSUBDIR/$LINUXFILE $LINUXSUBDIR 
echo "update $LINUXDIR/$LINUXSUBDIR/$LINUXFILE ok"

#-------------------------------------------------
PCTOOLFILE=pack
PCTOOLSUBDIR=lichee/tools
mkdir -p .tmp/$PCTOOLSUBDIR

echo "start update $LINUXDIR/$PCTOOLSUBDIR/$PCTOOLFILE ..."
sleep 1
if [ -d ".tmp/$PCTOOLSUBDIR/$PCTOOLFILE" ]; then
    rm -rf .tmp/$PCTOOLSUBDIR/$PCTOOLFILE
fi
mv -f $PCTOOLSUBDIR/$PCTOOLFILE .tmp/$PCTOOLSUBDIR
cp -r $LINUXDIR/$PCTOOLSUBDIR/$PCTOOLFILE $PCTOOLSUBDIR
echo "update $LINUXDIR/$PCTOOLSUBDIR/$PCTOOLFILE ok"

#-------------------------------------------------

echo
echo "---------------------check code changed-----------------"
git status
