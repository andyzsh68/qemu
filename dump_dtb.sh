./build/qemu-system-aarch64 -machine virt -cpu cortex-a53 -m 2048 -machine iommu=smmuv3 -global arm-smmuv3.stage=1 -nographic -machine dumpdtb=qemu-smmuv3.dtb
dtc -I dtb -O dts -o qemu-smmuv3.dts qemu-smmuv3.dtb 
