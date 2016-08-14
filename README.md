## fuse-alto

A try to implement a FUSE driver for the Alto and Alto II
file system.

The code is based on L. Stewart's `aar.c` dated 1/18/93.

Currently nothing is written back to a mounted disk image,
but you can stat and read files.

Removing (rm) and renaming (mv) files is now supported,
and the internal disk image's SysDir is updated accordingly.

If time permits, I will try to implement the code required to
modify existing files and create new files.

Remember: nothing is written back to the disk image file(s) you specified.
Each time you mount a disk image it is in its original state.

<strong>Note</strong>: Now using commas again to separate double disk filenames.

#### How to build

First you need the FUSE development headers installed.
The FUSE library must be version 26 or newer (>= 2.6.0).
Depending on your system, the package name may be <tt>fuse-dev</tt>
or <tt>fuse-devel</tt>.

Of course you need <tt>cmake</tt> (3.x or newer), a C++ compiler like <tt>g++</tt> and (GNU) make.

For cmake projects you should build outside of the source tree, like this:

<pre>mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cd ..
</pre>
Without <tt>-DCMABLE_BUILD_TYPE=Debug</tt> the default is to build a Release version.

You can now run <tt>build/bin/fuse-alto</tt> or install it somewhere in your path.

#### Examples for using fuse-alto

Running <tt>fuse-alto</tt> without parameters will print some help.

Create a mount point for your test, like
<pre>$ mkdir /tmp/alto</pre>

First try to run in the foreground, because then you don't have to <tt>umount /tmp/alto</tt> after use.
<pre>$ build/bin/fuse-alto /tmp/alto -f someimage.dsk</pre>

In another shell you should now be able to list, remove and rename files in <tt>/tmp/alto</tt>.

When done hit <tt>Ctrl+C</tt> in the first shell to abort <tt>fuse-alto</tt> and auto-unmount <tt>/tmp/alto</tt>.

If you don't want to run in foreground, run without <tt>-f</tt>.

Have fun!
