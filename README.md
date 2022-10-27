This repo contains experiment environment that described by https://github.com/d0u9/Linux-Device-Driver.
Above ropo tells how to setup a experiment environment on qemu with new kernel version (i.e., 5.10) while
the original LDD3 examples are based on version 2.6.

The version that I use is kernel 5.15 since the toolchain that I have (i.e., Ubuntu 22.04) cannot build
5.10 version and I am too lazy to install older toolchains. I assume this is fine because we only work
on some general purpose device drivers.

If you are also using Ubuntu 22.04, you can use this repo directly, just compile the kernel and go.
If not, maybe you need to download other versions of kernels. Still, you can try compile the kernel
given by this repo first, if you can successfully compile it, then it's fine.

All the setup process can be find in the above original repo, this repo just include all the source
code that we need so you do not need to download them by yourself. Also, I created a Makefile with
common used targets to ease our life.

## How to use

### Setup qemu

First, set `LDD_ROOT` as the path of this repo. Then add `LDD_ROOT/bin` into your `PATH`.
If you do not want add these environment variables, put them into the Makefile is also okay.

Compile qemu by ourself instead of download it from the package managements because we need to add
some patch to it. Check [here](https://github.com/d0u9/Linux-Device-Driver/blob/master/02_getting_start_with_driver_development/01_development_with_qemu.md) for more details.

The patch file is under the repo root, the link in above doc is wrong.

Use following options to build qemu.
```shell
mkdir build
cd build
../configure \
    --sysconfdir=$CONFIG_DIR \
    --target-list=$TARGET_LIST \
    --enable-virtfs \
    --enable-debug \
    --extra-cflags="-g3" \
    --extra-ldflags="-g3" \
    --disable-strip \
    ;
make -j

# instead of install, I prefor to use the executable obj file directly
cd $LDD_ROOT/bin
ln -s ../qemu/build/qemu-system-x86_64 qemu
```

### Run kernel on qemu

Kernel source code is under the `kernels` directory. It is configured. You just need run `make bzImage` and `make modules` to compile it.

I created a target `qemu` in the Makefile, use `make qemu` to run the kernel with qemu. You may need to modify the Makefile if you are using
different kernels.

### Use NFS

For copy code to the kernel, we use nfs. We setup nfs server in the host side, and mount the shared directory in guest running on qemu.

Host side:
```shell
# install nfs server
sudo apt-get install nfs-kernel-server

# share the ldd directory
sudo bash -c "echo \
    '$LDD_ROOT        127.0.0.1(insecure,rw,sync,no_root_squash)' \
    >> /etc/exports"

sudo exportfs -a
```

Guest side (no need if using this repo):
```shell
# /path/to/working/directory is the absolute path of $LDD_ROOT
mount -t nfs -o nolock host_machine:/path/to/working/directory /mnt
```

To avoid manually mount this directory every time, I added this line to the `init` script.
So it is in the initramfs and will be run automatically.

### Setup telnet (no need to do by yourself)

Some tests need multiple processes to be connected to the qemu guest, for example, when testing FIFO.

We use telnet to access the guest and busybox has already have it.
Check [here](https://github.com/d0u9/Linux-Device-Driver/blob/master/02_getting_start_with_driver_development/05_telnet_server.md) for setup details.

This repo has already add it to the initramfs. The port is set to `7023` and we use `telnet localhost 7023` to connect to the guest.
Modify qemu command option you want to use other ports.

