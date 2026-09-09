ESP Package Manager (EPM)
=========================

What is EPM?
------------

EPM is a simple cross-platform tool that generates software and patch
distributions in various formats from a list of files.  Supported formats
include:

- AIX software packages ("installp")
- AT&T software packages ("pkgadd"), used by Solaris and others
- BSD packages ("pkg_create")
- Compaq Tru64 UNIX ("setld")
- Debian Package Manager ("dpkg")
- HP-UX software packages ("swinstall")
- IRIX software manager ("inst", "swmgr", or "tardist")
- macOS software packages ("name.pkg")
- Portable (installation and removal scripts with tar files)
- Red Hat Package Manager ("rpm")
- Slackware software packages ("name.tgz")

EPM also includes graphical "setup" and "uninstall" programs that can be
provided with your distributions to make installation and removal of more than
one package a snap.  The installers can be customized with product logos,
"readme" files, and click-wrap licenses as desired.

EPM is provided as free software under version 2 of the Apache License (ALv2).

How is this releated to the official EPM?
-----------------------------------------

Starting with version 4.5.2, this is, in fact, the official EPM distribution.


How Do I Compile EPM?
---------------------

See the file `INSTALL.md` for more info on that.


How Do I Use EPM?
-----------------

Please look at the EPM manual.  A preformatted copy is included with the source
archive in the file `doc/epm-book.html`.

An example EPM software list file is provided with this distribution in the
file `epm.list`.


Do I Have to Pay to Distribute Software Using EPM?
--------------------------------------------------

No!  EPM is free software and any installations you create are unencumbered by
licensing of any kind, not even the GPL or the ALv2.


What's New in EPM?
------------------

See the file `CHANGES.md` for change information.


Resources
---------

The official home page for EPM is <https://jimjag.github.io/epm/>.

Report all problems and submit all patches/pull requests using the Github issue
tracking pages at <https://github.com/jimjag/epm/issues>.


Legal Stuff
-----------

EPM is Copyright © 1999-2020 by Michael R Sweet. All rights reserved.
EPM is Copyright © 2020-2026 by Jim Jagielski. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
