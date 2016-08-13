### fuse-alto

A try to implement a FUSE driver for the Alto and Alto II
file system.

The code is based on L. Stewart's `aar.c` dated 1/18/93.

Currently nothing is written back to a mounted disk image,
but you can stat and read files.

If time permits, I'll be trying to finish the unlink and
rename functions and implement the code required to modify
existing files and create new files.

For the most part this needs a working (re-)allocation
of disk image blocks to be able to manipulate `SysDir`.

So even for deleting and renaming a file, and writing the
image back to the disk image file, a working *write to disk*
is what's missing.
