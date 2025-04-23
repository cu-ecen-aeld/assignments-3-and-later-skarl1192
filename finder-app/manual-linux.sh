#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

export PATH=/home/arm-cross-compiler/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu/bin:$PATH

set -e # If any command fails, exit the script
set -u # Treat unset variables as an error

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot) # Get the sysroot path from the cross-compiler

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"

# Check if the kernel repo exists
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi

# Build the kernel image if it has not been built already
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    
    # Checkout the version of the kernel to build
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    echo "Starting kernel image build..."

    # make the `mrproper`` target
    # Deep cleansthe kernel build tree - removing the .config file with any existing configurations
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper

    # make the `defconfig`` target 
    # Generates the default configuration for the 'virt' ARM dev board we will simulate in QEMU
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig

    # make the `all` target
    # Builds the kernel image for booting with QEMU
    make -j4 ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all

    # make the `modules` target
    # Builds any kernel modules (commented out from instructions)
    #make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} modules

    # make the `dtbs` target
    # Build the device tree
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
fi

# Copy the built kernel image to outdir
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}/

echo "Creating the staging directory for the root filesystem..."
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
mkdir -p ${OUTDIR}/rootfs
cd ${OUTDIR}/rootfs
mkdir -p bin dev etc home lib lib64 proc sbin sys tmp usr var
mkdir -p usr/bin usr/lib usr/sbin
mkdir -p var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
    git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
    make distclean
    make defconfig
else
    cd busybox
fi

# TODO: Make and install busybox if it doesn't exist
if [ ! -e "${OUTDIR}/rootfs/bin/busybox" ]
then
    echo "Building and installing BusyBox..."

    # Build BusyBox
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}

    # Install BusyBox to the rootfs directory
    make CONFIG_PREFIX=${OUTDIR}/rootfs ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install
fi

echo "Checking Busy Box's library dependencies..."
cd ${OUTDIR}/rootfs
${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
echo "Adding Busy Box's library dependencies to rootfs..."

SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot) # Get the sysroot path from the cross-compiler
echo "DEBUG: SYSROOT is set to '${SYSROOT}'" # Add this line

INTERPRETER=$(${CROSS_COMPILE}readelf -a bin/busybox | grep "program interpreter" | sed -e 's/.*: \(.*\)]/\1/')
echo "Interpreter: ${INTERPRETER}"
cp "${SYSROOT}${INTERPRETER}" "${OUTDIR}/rootfs/lib/"

${CROSS_COMPILE}readelf -a bin/busybox | grep "Shared library" | sed -e 's/.*\[\(.*\)\]/\1/' | \
# Pipe the output to a while loop
while read -r libname; do
    echo "Library: ${libname}"
    
    # Find the library in sysroot and copy it to rootfs/lib64
    libpath=$(find ${SYSROOT} -name ${libname})
    cp "${libpath}" "${OUTDIR}/rootfs/lib64/"
done

# TODO: Make device nodes
echo "Creating device nodes..."
sudo mknod -m 666 dev/null c 1 3
sudo mknod -m 600 dev/console c 5 1

# TODO: Clean and build the writer utility
echo "Building writer utility..."
cd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
echo "Copying finder related scripts and exe's to /rootfs/home/ ..."
cp writer ${OUTDIR}/rootfs/home/
cp finder.sh ${OUTDIR}/rootfs/home/
mkdir -p ${OUTDIR}/rootfs/home/conf
cp conf/username.txt ${OUTDIR}/rootfs/home/conf/
cp conf/assignment.txt ${OUTDIR}/rootfs/home/conf/
cp finder-test.sh ${OUTDIR}/rootfs/home/
cp autorun-qemu.sh ${OUTDIR}/rootfs/home/

# TODO: Chown the root directory
echo "Changing ownership of rootfs to root"
sudo chown -R root:root ${OUTDIR}/rootfs

# TODO: Create initramfs.cpio.gz
echo "Creating initramfs.cpio.gz"
cd ${OUTDIR}/rootfs
find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
cd ${OUTDIR}
gzip -f initramfs.cpio