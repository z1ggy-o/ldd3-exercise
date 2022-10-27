.PHONY: qemu
qemu:
	qemu -enable-kvm \
		 -netdev user,id=host_net,hostfwd=tcp::7023-:23 \
		 -device e1000,mac=52:54:00:12:34:50,netdev=host_net \
		 -m 512M \
     -kernel "${LDD_ROOT}"/kernels/linux-5.15.70/arch/x86_64/boot/bzImage \
     -initrd "${LDD_ROOT}"/initramfs.cpio.gz \
     -append 'console=ttyS0' \
     -nographic
