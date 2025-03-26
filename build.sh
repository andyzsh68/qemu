./configure --target-list=aarch64-softmmu --enable-kvm #--enable-debug --enable-trace-backends=simple,dtrace
 make -j$(nproc)
