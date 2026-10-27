vvramfs
======

Unused RAM is wasted RAM, so why not put some of that VRAM in your graphics card
to work?

vvramfs is a utility that uses the [FUSE library](http://fuse.sourceforge.net/)
to create a file system in VRAM. The idea is pretty much the same as a ramdisk,
except that it uses the video RAM of a discrete graphics card to store
files. It is not intented for serious use, but it does actually work fairly
well, especially since consumer GPUs with 4GB or more VRAM are now available.

vvramfs ports https://github.com/Overv/vramfs from OpenCL to Vulkan. Writing this  port was easier than getting AMD's OpenCL drivers to work.

#### Requirements

- Linux, preferably 7.0+
- libfuse3-dev
- A graphics card with support for Vulkan 1.0

#### Building

First, ensure the Vulkan driver for your graphics card is installed and verify that it's
recognized as a Vulkan device by running `vulkaninfo`. Then install the `libfuse3-dev`
package or build it from source. You will also need `pkg-config` and Vulkan 
development files, (`libvulkan-dev` package or equivalent).

Just run `make` to build `vramfs`.

#### Mounting

Mount a disk by running `bin/vramfs <mountdir> <size>`. The `mountdir` can be
any empty directory. The `size` is the disk size in bytes. For more information,
run `bin/vramfs` without arguments.

The size must be a multiple of 128 MiB. The current focus is towards virtual disks
to use for swap or to create finer-grained filesystems on. Creating many small files
without using a nested filesystem is discouraged.

The recommended maximum size of a vramdisk is 50% of your VRAM. If you go over
that, your driver or system may become unstable because it has to start
swapping. For example, webpages in Chrome will stop rendering properly.

If the disk has been inactive for a while, the graphics card will likely lower
its memory clock, which means it'll take a second to get up to speed again.

Implementation
--------------

The FUSE library is used to implement vramfs as a user space file system. This
eases development and makes working with GPU APIs straightforward.

#### Basic architecture

When the program is started, it checks for an Vulkan capable GPU and attempts to
allocate the specified amount of memory. Once the memory has been allocated, the
root entry object is created and a global reference to it is stored.

FUSE then forwards calls like `stat`, `readdir` and `write` to the file system
functions. These will then locate the entry through the root entry using the
specified path. The required operations will then be performed on the entry
object. If the entry is a file object, the operation may access the data in VRAM.

When a file is created or opened, a `file_session` object is created to store
the reference to the file object and any other data that is persistent between
an `fopen` and `fclose` call.

#### VRAM block allocation

Vulkan is used to allocate blocks of memory on the graphics card by creating
buffer objects. When a new disk is mounted, a pool of `disk size / block size`
buffers is created and initialised with zeros. That is not just a good practice,
but it's also required with some Vulkan drivers to check if the VRAM required
for the block is actually available.

To avoid dealing with creating queues and synchronizing copies, we just uncached-mmap GPU VRAM onto the host then `memcpy` as if it was regular RAM. GPU hotplug/loss shall be undefined behavior.

Block objects are managed using a `shared_ptr` so that they can automatically
reinsert themselves into the pool on deconstruction.

#### File system

The file system is a tree of `entry_t` objects with members for attributes like
the parent directory, mode and access time. Each type of entry has its own
subclass that derives from it: `file_t`, `dir_t` and `symlink_t`. The main file
that implements all of the FUSE callbacks has a permanent reference to the root
directory entry.

The `file_t` class contains extra `write`, `read` and `size` methods and manages
the blocks to store the file data.

The `dir_t` class has an extra `unordered_map` that maps names to `entry_t`
references for quick child lookup using its member function `find`.

Finally, the `symlink_t` class has an extra `target` string member that stores
the pointer of the symlink.

All of the entry objects are also managed using `shared_ptr` so that an object
and its data (e.g. file blocks) are automatically deallocated when they're
unlinked and no process holds a file handle to them anymore. This can also be
used to easily implement hard links later on.

The classes use getter/setter functions to automatically update the access,
modification and change times at the appropriate moment. For example, calling
the `children` member function of `dir_t` changes the access time and change
time of the directory.

#### Thread safety

Unfortunately most of the operations are not thread safe, so all of the FUSE
callbacks share a mutex to ensure that only one thread is mutating the file
system at a time. The exceptions are `read` and `write`, which will temporarily
release the lock while waiting for a read or write to complete.

Benchmarks
----------

The system used for testing has the following specifications:

* **OS:** Ubuntu 26.04
* **CPU:** AMD Ryzen 9 5900X
* **RAM:** 32 GB DDR4-3600
* **GPU:** AMD Radeon RX 6650 XT

Performance has been measured:

* `dd if=/dev/zero of=/tmp/vram/test bs=1M count=1024 oflag=dsync`: 3.7 GB/s
* `dd if=/tmp/test of=/tmp/vram/test bs=1M count=1024 oflag=dsync`: 1.9 GB/s
* `dd if=/dev/urandom of=/tmp/vram/test bs=1M count=1024 oflag=dsync`: 429 MB/s
* `dd if=/tmp/vram/test of=/dev/null bs=1M count=1024 oflag=dsync`: 30 MB/s (TODO FIXME)

Regular ramdisk comparison:

* `dd if=/tmp/test of=/tmp/test2 bs=1M count=1024 oflag=dsync`: 1.7 GB/s
* `dd if=/dev/urandom of=/tmp/test bs=1M count=1024 oflag=dsync`: 420 MB/s
* `dd if=/tmp/test of=/dev/null bs=1M count=1024 oflag=dsync`: 3.7 GB/s

License
-------

SPDX-License-Identifier: MIT

SPDX-FileCopyrightText: 2014 Alexander Overvoorde

SPDX-FileCopyrightText: 2026 Daniel Tang
