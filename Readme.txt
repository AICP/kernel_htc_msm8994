defconfig file: msm8994_defconfig

Download:
========
Download the corresponding official android toolchain:

linux-x86:
git clone https://android.googlesource.com/platform/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9

darwin-x86:
git clone https://android.googlesource.com/platform/prebuilts/gcc/darwin-x86/aarch64/aarch64-linux-android-4.9

Build the kernel:
=================
set the following environment variables:

$ mkdir -p out
$ make ARCH=arm64 CROSS_COMPILE=<path-to-toolchain>/bin/aarch64-linux-android- O=out msm8994_defconfig
$ make ARCH=arm64 CROSS_COMPILE=<path-to-toolchain>/bin/aarch64-linux-android- O=out -j16

Output binary files:
====================
After the build process finishes, the kernel image will be located at out/arch/arm64/boot/Image.gz-dtb


For additional information:
===========================
http://htcdev.com

