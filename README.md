### fuse-alto

A try to implement a FUSE driver for the Alto and Alto II
file system.

The code is based on L. Stewart's `aar.c` dated 1/18/93.

Currently nothing is written back to a mounted disk image,
but you can stat and read files.

Removing (rm) and renaming (mv) files is now supported,
and the internal disk image's SysDir is updated accordingly.

If time permits, I will try to implement the code required to
modify existing files and create new files.

Nothing is written back to the disk image file(s) you specified.

Note: Changed back to separate double disk filenames by comma.

#### How to build

First you need the FUSE development headers installed.
The FUSE library must be version 26 or newer (>= 2.6.0).

Depending on your system, the package name may be [code]fuse-dev[/code]
or [code]fuse-devel[/code].

Of course you need [code]cmake[/code] (3.x or newer), a C++ compiler like [code]g++[/code] and (GNU) make.

For cmake projects you should build outside of the source tree, like this:

<pre>mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cd ..
</pre>

You can then run [code]build/bin/fuse-alto[/code] or install it somewhere in your path.

#### Examples for using fuse-alto

Create a mount point for your test, like
<pre>$ mkdir /tmp/alto</pre>

First try to run in the foreground, because you don't have to unmount after use.
<pre>$ build/bin/fuse-fs /tmp/alto -f someimage.dsk</pre>

In another shell you should now be able to list, remove and rename files in [code]/tmp/alto[/code].

When done hit [code]Ctrl+C[/code] in the first shell to abort [code]fuse-alto[/code].

Running [code]fuse-alto[/code] without parameters will print some help.

Have fun!
