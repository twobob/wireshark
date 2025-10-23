General Information
-------------------

Wireshark is a network traffic analyzer, or "sniffer", for Linux, macOS,
\*BSD and other Unix and Unix-like operating systems and for Windows.
It uses Qt, a graphical user interface library, and libpcap and npcap as
packet capture and filtering libraries.

The Wireshark distribution also comes with TShark, which is a
line-oriented sniffer (similar to Sun's snoop or tcpdump) that uses the
same dissection, capture-file reading and writing, and packet filtering
code as Wireshark, and with editcap, which is a program to read capture
files and write the packets from that capture file, possibly in a
different capture file format, and with some packets possibly removed
from the capture.

The official home of Wireshark is https://www.wireshark.org.

The latest distribution can be found in the subdirectory https://www.wireshark.org/download


Installation
------------

The Wireshark project builds and tests regularly on the following platforms:

  - Microsoft Windows
  - Linux (Ubuntu)

Official installation packages are available for Microsoft Windows

Console-only builds for tiny devices
------------------------------------

For resource-constrained environments where only the console analyzers are
required, enable the `ENABLE_TINY_DEVICES` CMake option. This configures the
build in "C only" mode, skips the GUI, plugin, documentation, and auxiliary
toolchain targets, and limits optional dependencies to C-based components so
the tree is ready for cross-compiling to embedded toolchains.

```
cmake -DENABLE_TINY_DEVICES=ON -DCMAKE_BUILD_TYPE=Release /path/to/wireshark
```

Only the command-line utilities such as `tshark`, `dumpcap`, and related
capture tools will be generated when this option is active, with no C++ code
compiled as part of the build graph.

It should run on other Unix-ish systems without too much trouble.

Python 3 is needed to build Wireshark. AsciiDoctor is required to build
the documentation, including the man pages. Perl and flex are required
to generate some of the source code.

You must therefore install Python 3, AsciiDoctor, and GNU "flex" (vanilla
"lex" won't work) on systems that lack them. You might need to install
Perl as well.

Full installation instructions can be found in the INSTALL file and in the
Developer's Guide at https://www.wireshark.org/docs/wsdg_html_chunked/

See also the appropriate README._OS_ files for OS-specific installation
instructions.

Usage
-----

In order to capture packets from the network, you need to make the
dumpcap program set-UID to root or you need to have access to the
appropriate entry under `/dev` if your system is so inclined (BSD-derived
systems, and systems such as Solaris and HP-UX that support DLPI,
typically fall into this category).  Although it might be tempting to
make the Wireshark and TShark executables setuid root, or to run them as
root please don't.  The capture process has been isolated in dumpcap;
this simple program is less likely to contain security holes and is thus
safer to run as root.

Please consult the man page for a description of each command-line
option and interface feature.


Multiple File Types
-------------------

Wireshark can read packets from a number of different file types.  See
the Wireshark man page or the Wireshark User's Guide for a list of
supported file formats.

Wireshark can transparently read compressed versions of any of those files if
the required compression library was available when Wireshark was compiled.
Currently supported compression formats are:

- GZIP
- LZ4
- ZSTD

GZIP and LZ4 (when using independent blocks, which is the default) support
fast random seeking, which offers much better GUI performance on large files.
Any of these compression formats can be disabled at compile time by passing
the corresponding option to cmake, i.e., `cmake -DENABLE_ZLIB=OFF`,
`cmake -DENABLE_LZ4=OFF`, or `cmake -DENABLE_ZSTD=OFF`.

Although Wireshark can read AIX iptrace files, the documentation on
AIX's iptrace packet-trace command is sparse.  The `iptrace` command
starts a daemon which you must kill in order to stop the trace. Through
experimentation it appears that sending a HUP signal to that iptrace
daemon causes a graceful shutdown and a complete packet is written
to the trace file. If a partial packet is saved at the end, Wireshark
will complain when reading that file, but you will be able to read all
other packets.  If this occurs, please let the Wireshark developers know
at wireshark-dev@wireshark.org; be sure to send us a copy of that trace
file if it's small and contains non-sensitive data.



Wireshark can also read dump trace output from the Toshiba "Compact Router"
line of ISDN routers (TR-600 and TR-650). You can telnet to the router
and start a dump session with `snoop dump`.

CoSine L2 debug output can also be read by Wireshark. To get the L2
debug output first enter the diags mode and then use
`create-pkt-log-profile` and `apply-pkt-lozg-profile` commands under
layer-2 category. For more detail how to use these commands, you
should examine the help command by `layer-2 create ?` or `layer-2 apply ?`.

To use the Lucent/Ascend, Toshiba and CoSine traces with Wireshark, you must
capture the trace output to a file on disk.  The trace is happening inside
the router and the router has no way of saving the trace to a file for you.
An easy way of doing this under Unix is to run `telnet <ascend> | tee <outfile>`.
Or, if your system has the "script" command installed, you can save
a shell session, including telnet, to a file. For example to log to a file
named tracefile.out:

~~~
$ script tracefile.out
Script started on <date/time>
$ telnet router
..... do your trace, then exit from the router's telnet session.
$ exit
Script done on <date/time>
~~~


Name Resolution
---------------

Wireshark will attempt to use reverse name resolution capabilities
when decoding IPv4 and IPv6 packets.

If you want to turn off name resolution while using Wireshark, start
Wireshark with the `-n` option to turn off all name resolution (including
resolution of MAC addresses and TCP/UDP/SMTP port numbers to names) or
with the `-N mt` option to turn off name resolution for all
network-layer addresses (IPv4, IPv6, IPX).

You can make that the default setting by opening the Preferences dialog
using the Preferences item in the Edit menu, selecting "Name resolution",
turning off the appropriate name resolution options, and clicking "OK".


SNMP
----

Wireshark can do some basic decoding of SNMP packets; it can also use
the libsmi library to do more sophisticated decoding by reading MIB
files and using the information in those files to display OIDs and
variable binding values in a friendlier fashion.  CMake  will automatically
determine whether you have the libsmi library on your system.  If you
have the libsmi library but _do not_ want Wireshark to use it, you can run
cmake with the `-DENABLE_SMI=OFF` option.



License
-------

Wireshark is distributed under the GNU GPLv2. See the file COPYING for
the full text of the license. When in doubt the full text is the legally
binding part. These notes are just to make it easier for people that are not
familiar with the GPLv2.

There are no restrictions on its use. There are restrictions on its distribution
in source or binary form.

Most parts of Wireshark are covered by a "GPL version 2 or later" license.
Some files are covered by different licenses that are compatible with
the GPLv2.

As a notable exception, some utilities distributed with the Wireshark source are
covered by other licenses that are not themselves directly compatible with the
GPLv2. This is OK, as only the tools themselves are licensed this way, the
output of the tools is not considered a derived work, and so can be safely
licensed for Wireshark's use. An incomplete selection of these tools includes:
 - the pidl utility (tools/pidl) is licensed under the GPLv3+.

Parts of Wireshark can be built and distributed as libraries. These
parts are still covered by the GPL, and NOT by the Lesser General Public
License or any other license.

If you integrate all or part of Wireshark into your own application and you
opt to publish or release it then the combined work must be released under
the terms of the GPLv2.


Disclaimer
----------

There is no warranty, expressed or implied, associated with this product.
Use at your own risk.


Gerald Combs <gerald@wireshark.org>

Gilbert Ramirez <gram@alumni.rice.edu>

Guy Harris <gharris@sonic.net>

Twobob <wireshark@ψπ.com>
