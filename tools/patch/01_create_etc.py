"""Create disk/etc/ configuration files."""

from .utils import set_module, log, create_file

def run():
    set_module("ETC")
    log("=== Creating disk/etc/ files ===")

    create_file("disk/etc/passwd",
        "root:4813494d137e1631bba301d5acab6e7bb7aa74ce1185d456565ef51d737677b2"
        ":0:0:System Administrator:/root:/bin/osh\n"
        "user:04f8996da763b7a969b1028ee3007569eaf3a635486ddab211d512c85b9df8fb"
        ":1000:1000:Regular User:/home/user:/bin/shell.bin\n"
    )

    create_file("disk/etc/motd",
        "\n"
        "============ Open Aries ================\n"
        "  Kernel:  seL4 14.0.0 (Microkit 2.1.0)\n"
        "  CPUs:    4 cores (SMP)\n"
        "  Sandbox: 8 process slots\n"
        "  Drivers: PL011 UART, virtio-blk, virtio-net\n"
        "============ Open Aries ================\n"
        "\n"
    )

    create_file("disk/etc/hostname", "aios\n")

    create_file("disk/etc/services.conf",
        "# AIOS Service Configuration\n"
        "# format: name:binary:flags\n"
        "# flags: daemon = long-running background process\n"
        "#        once   = run at boot then exit\n"
        "#\n"
        "# (no services configured yet)\n"
    )

    create_file("disk/hello.txt", "Hello from AIOS ext2 filesystem!\n")
    return True
