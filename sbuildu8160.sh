#!/bin/sh

echo ""
echo ""
echo "Welcome to Alberto96 Kernel builder, please wait until work is completed"
echo ""
echo ""

export ARCH=arm
export CROSS_COMPILE=/huawei/kernel/android_prebuilt_toolchains/arm-unknown-linux-gnueabi-linaro_4.7.4-2013.06/bin/arm-gnueabi-

echo "Building CM9 Kernel, ignore any compiling warnings except errors ;)"
echo ""
echo ""

DATE_START=$(date +"%s")

make -j3

make -j3 modules
rm ./modules/*
find /huawei/kernel/android_kernel_huawei_msm7x25 -name '*.ko' -exec cp -v {} ./modules \;

cd ramdisk

echo ""
echo "Packing RamDisk..."
echo ""

./mkbootfs u8160 | gzip > ramdisk.gz

cp ../arch/arm/boot/zImage .

echo ""
echo "Building boot.img kernel image"
echo ""

./mkbootimg --cmdline 'mem=211M console=ttyMSM2,115200n8 androidboot.hardware=u8160' --kernel zImage --ramdisk ramdisk.gz --base 0x00210000 --ramdiskaddr 0x1208000 -o boot.img

mv boot.img ../output

echo ""
echo "Done, you can find the kernel in output folder"
echo ""

DATE_END=$(date +"%s")
echo
DIFF=$(($DATE_END - $DATE_START))
echo "Build completed in $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds."
echo " "
