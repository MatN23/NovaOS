# Building Nova Desktop OS

Nova Desktop is a native FreeBSD solution. Because it relies on the FreeBSD kernel, build system (`make`), and subsystems (DRM, OSS, devd), **it must be compiled within a FreeBSD environment**.

You cannot compile Nova directly on macOS, Windows, or Linux userlands. Instead, you must run a FreeBSD Virtual Machine (VM) on your host OS to perform the build.

This guide explains how to set up the build environment on your platform.

---

## 1. macOS (Apple Silicon & Intel)

### Apple Silicon (M1/M2/M3) - Recommended: **UTM**
UTM uses Apple's native virtualization framework for near-native performance.

1.  **Download UTM**: Get it from [mac.getutm.app](https://mac.getutm.app).
2.  **Download FreeBSD ARM64 ISO**: Fetch the latest `aarch64` disc ISO from [freebsd.org](https://download.freebsd.org/releases/ISO-IMAGES/).
3.  **Create VM**:
    *   File > New Virtual Machine > Virtualize > FreeBSD 14.x ARM64.
    *   **RAM**: Allocate 8GB+ (12GB+ recommended for `-j10` builds).
    *   **Storage**: 30GB+.
4.  **Filesharing**:
    *   In VM Settings > Sharing, select your local `freebsd-src` directory.
    *   Set mode to **VirtioFS**.
    *   Inside the VM, mount it: `mkdir /usr/src_host && mount_vt9p sharename /usr/src_host`.
5.  **Build**: Copy source from `/usr/src_host` to `/usr/src` manually or use `rsync`, then run `make buildworld`.

### Intel Macs - Recommended: **VMware Fusion Player**
VMware Fusion Player is free for personal use and very stable.

1.  **Download**: Get VMware Fusion from Broadcom/VMware site.
2.  **Download FreeBSD AMD64 ISO**: `amd64` architecture.
3.  **Setup**: Standard VM creation procedure. Enable "Shared Folders" in Settings.

---

## 2. Windows 10/11

### Recommended: **Hyper-V** (Pro/Enterprise/Edu only)
Hyper-V provides Type-1 hypervisor performance.

1.  **Enable Hyper-V**: Turn on "Hyper-V" in "Turn Windows features on or off".
2.  **Manager**: Open "Hyper-V Manager".
3.  **Create VM**:
    *   "Generation 2".
    *   Disable "Secure Boot" (FreeBSD supports it, but simpler without for dev).
    *   **Network**: Default Switch.
4.  **Transfer Source**:
    *   Hyper-V does not have easy folder sharing with FreeBSD guests.
    *   Use **SMB/CIFS**: Share your Windows `freebsd-src` folder, then mount in FreeBSD:
        ```bash
        mount_smbfs -I <host-ip> //user@host/share /usr/src
        ```
    *   Or use **WinSCP** to copy files.

### Alternative: **VMware Workstation Player** (Home Edition)
Better guest integration tools (`open-vm-tools`) than Hyper-V for file sharing.

1.  **Install**: FreeBSD 14.x AMD64.
2.  **Shared Folders**: Install `open-vm-tools` package in FreeBSD (`pkg install open-vm-tools`) to mount host directories via HGFS (fuse).

---

## 3. Linux (Ubuntu, Fedora, Arch)

### Recommended: **KVM/QEMU with Virt-Manager**
This offers native virtualization performance (KVM).

1.  **Install Tools**: `sudo apt install qemu-kvm libvirt-daemon-system virt-manager`.
2.  **Create VM**:
    *   Open **Virtual Machine Manager**.
    *   Install from FreeBSD AMD64 ISO.
    *   **CPU**: Pass "Host" CPU configuration for best performance.
3.  **Filesharing (VirtioFS)**:
    *   In Virt-Manager hardware settings: Add Hardware > Filesystem.
    *   Driver: `virtiofs`.
    *   Source path: `/path/to/freebsd-src`.
    *   Target path: `src_share`.
    *   In FreeBSD: `mount_vt9p src_share /mnt`.

---

## The Build Command (All Platforms)

Once you are inside your FreeBSD VM and have your source code at `/usr/src`:

1.  **Build World** (Compiles the OS userland):
    ```bash
    # Adjust -jN to your VM's core count
    cd /usr/src
    sudo make -j$(sysctl -n hw.ncpu) buildworld
    ```

2.  **Build Kernel** (Compiles the Nova-enabled kernel):
    ```bash
    sudo make -j$(sysctl -n hw.ncpu) buildkernel KERNCONF=GENERIC
    ```
    *(Note: We will create a custom NOVA kernel config later)*

3.  **Build Release** (Generates the ISO):
    ```bash
    cd release
    sudo make -j$(sysctl -n hw.ncpu) release
    ```
    The resulting ISO will be in `/usr/obj/usr/src/amd64.amd64/release/disc1.iso` (or `arm64.aarch64` for Mac M-series).
