#!/bin/sh

rm ./arch/arm/configs/cm9_u8160_alberto96_defconfig
cp .config ./arch/arm/configs/
mv ./arch/arm/configs/.config ./arch/arm/configs/cm9_u8160_alberto96_defconfig
