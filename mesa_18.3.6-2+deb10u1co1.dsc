Format: 1.0
Source: mesa
Binary: libxatracker2, libxatracker-dev, libd3dadapter9-mesa, libd3dadapter9-mesa-dev, libgbm1, libgbm-dev, libegl-mesa0, libegl1-mesa, libegl1-mesa-dev, libwayland-egl1-mesa, libgles2-mesa, libgles2-mesa-dev, libglapi-mesa, libglx-mesa0, libgl1-mesa-glx, libgl1-mesa-dri, libgl1-mesa-dri-llvm, libgl1-mesa-dev, mesa-common-dev, libosmesa6, libosmesa6-dev, mesa-va-drivers, mesa-vdpau-drivers, mesa-vulkan-drivers, mesa-opencl-icd
Architecture: any
Version: 18.3.6-2+deb10u1co1
Maintainer: Debian X Strike Force <debian-x@lists.debian.org>
Uploaders: Andreas Boll <aboll@debian.org>
Homepage: https://mesa3d.org/
Standards-Version: 4.1.4
Vcs-Browser: https://salsa.debian.org/xorg-team/lib/mesa
Vcs-Git: https://salsa.debian.org/xorg-team/lib/mesa.git
Build-Depends: debhelper (>= 11), meson (>= 0.45), quilt (>= 0.63-8.2~), pkg-config, libdrm-dev (>= 2.4.95) [!hurd-any], libx11-dev, libxxf86vm-dev, libexpat1-dev, libsensors-dev [!hurd-any], libxfixes-dev, libxdamage-dev, libxext-dev, libva-dev (>= 1.6.0) [linux-any kfreebsd-any] <!pkg.mesa.nolibva>, libvdpau-dev (>= 1.1.1) [linux-any kfreebsd-any], libvulkan-dev [amd64 arm64 armel armhf i386 mips mips64el mipsel powerpc ppc64 ppc64el s390x sparc64 x32], x11proto-dev, linux-libc-dev (>= 2.6.31) [linux-any], libx11-xcb-dev, libxcb-dri2-0-dev (>= 1.8), libxcb-glx0-dev (>= 1.8.1), libxcb-xfixes0-dev, libxcb-dri3-dev, libxcb-present-dev, libxcb-randr0-dev, libxcb-sync-dev, libxrandr-dev, libxshmfence-dev (>= 1.1), python3, python3-mako, flex, bison, llvm-7-dev (>= 1:7~) [amd64 arm64 armel armhf i386 kfreebsd-amd64 kfreebsd-i386 mips mips64el mipsel powerpc ppc64 ppc64el s390x sparc64], libwayland-dev (>= 1.15.0) [linux-any], libwayland-egl-backend-dev (>= 1.15.0) [linux-any], libclang-7-dev (>= 1:7~) [amd64 arm64 armel armhf i386 kfreebsd-amd64 kfreebsd-i386 mips mips64el mipsel powerpc ppc64 ppc64el s390x sparc64], libclc-dev (>= 0.2.0+git20180917-1~) [amd64 arm64 armel armhf i386 kfreebsd-amd64 kfreebsd-i386 mips mips64el mipsel powerpc ppc64 ppc64el s390x sparc64], wayland-protocols (>= 1.9), zlib1g-dev, libglvnd-core-dev
Build-Conflicts: libelf-dev
Package-List:
 libd3dadapter9-mesa deb libs optional arch=amd64,arm64,armel,armhf,i386,kfreebsd-i386,powerpc
 libd3dadapter9-mesa-dev deb libdevel optional arch=amd64,arm64,armel,armhf,i386,kfreebsd-i386,powerpc
 libegl-mesa0 deb libs optional arch=any
 libegl1-mesa deb oldlibs optional arch=any
 libegl1-mesa-dev deb libdevel optional arch=any
 libgbm-dev deb libdevel optional arch=linux-any,kfreebsd-any
 libgbm1 deb libs optional arch=linux-any,kfreebsd-any
 libgl1-mesa-dev deb libdevel optional arch=any
 libgl1-mesa-dri deb libs optional arch=any
 libgl1-mesa-dri-llvm deb oldlibs optional arch=any
 libgl1-mesa-glx deb oldlibs optional arch=any
 libglapi-mesa deb libs optional arch=any
 libgles2-mesa deb oldlibs optional arch=any
 libgles2-mesa-dev deb libdevel optional arch=any
 libglx-mesa0 deb libs optional arch=any
 libosmesa6 deb libs optional arch=any
 libosmesa6-dev deb libdevel optional arch=any
 libwayland-egl1-mesa deb oldlibs optional arch=linux-any
 libxatracker-dev deb libdevel optional arch=amd64,i386,x32
 libxatracker2 deb libs optional arch=amd64,i386,x32
 mesa-common-dev deb libdevel optional arch=any
 mesa-opencl-icd deb libs optional arch=amd64,arm64,armel,armhf,i386,kfreebsd-amd64,kfreebsd-i386,mips,mips64el,mipsel,powerpc,ppc64,ppc64el,s390x,sparc64
 mesa-va-drivers deb libs optional arch=linux-any,kfreebsd-any profile=!pkg.mesa.nolibva
 mesa-vdpau-drivers deb libs optional arch=linux-any,kfreebsd-any
 mesa-vulkan-drivers deb libs optional arch=amd64,arm64,armel,armhf,i386,mips,mips64el,mipsel,powerpc,ppc64,ppc64el,s390x,sparc64,x32
Checksums-Sha1:
 3064c841872150e77bd76b50065f3c825b62f59a 20348664 mesa_18.3.6.orig.tar.gz
 4722ace2ecb966e90457080d0127b4c61fd1b18c 105889 mesa_18.3.6-2+deb10u1co1.diff.gz
Checksums-Sha256:
 4619d92afadf7072f7956599a2ccd0934fc45b4ddbc2eb865bdcb50ddf963f87 20348664 mesa_18.3.6.orig.tar.gz
 c2fc9ddc4399841d874db09ac54967779791a919f255a86eead598e84cf99ef2 105889 mesa_18.3.6-2+deb10u1co1.diff.gz
Files:
 8137e4e989f0394405e5869e65678c8e 20348664 mesa_18.3.6.orig.tar.gz
 db913d1c9b6180ce64c0bdb4f2a05a56 105889 mesa_18.3.6-2+deb10u1co1.diff.gz
