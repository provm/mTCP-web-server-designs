Two Designs:

Follow the following setups to run mTCP on your system:
    Decompress the two patch files and follow the following steps for the required design

mTCP Master-Worker [Master controls all]:
    Apply mTCP-master-epoll.patch to mTCP codebase(https://github.com/eunyoung14/mtcp)
    Setup DPDK and run the above patched mTCP source
    From apps foder, run epserver to test the shared mTCP
    
mTCP Master-Worker [Master initiates worker]:
    Apply mTCP-master-accept.patch to mTCP codebase(https://github.com/eunyoung14/mtcp)
    Setup DPDK and run the above patched mTCP source
    From apps foder, run epserver to test the shared mTCP
    
The code section has other sources regarding lock free data structure exploration.
