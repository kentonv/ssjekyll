#! /bin/bash

set -euo pipefail

missingFileList () {
  cat >&2 << __EOF__
You need to create a file "package-files.list" which contains a list of
files from your system to copy into the package.  These files must include
everything needed to run jekyll and pygments.  The file
package-files-kentons-machine.list is the list Kenton came up with from his
own (Debian-Testing) system, but it may or may not work for you.
Unfortunately the process of building this list is pretty ad-hoc right now.
__EOF__
  exit 1
}

test -e package-files.list || missingFileList

cat package-files.list | (while read file; do if test "x$file" != x; then mkdir -p pkg`dirname $file`; rm -rf pkg$file; cp -rL $file pkg$file; fi; done)

rm -rf pkg/usr/lib/gems
mv pkg/var/lib/gems pkg/usr/lib/gems
rmdir pkg/var/lib

mkdir -p pkg/bin
cp /bin/busybox pkg/bin/sh
ln -sf sh pkg/bin/which  # used by jekyll
sed -i -e "s/'var'/'usr'/g" pkg/usr/lib/ruby/1.9.1/rubygems/defaults.rb

# addusersitepackages() fails when there's no /etc/passwd, but we don't want it anyway.
sed -i -e "s/^ *known_paths = addusersitepackages[(]known_paths[)] *$//g" pkg/usr/lib/python2.7/site.py
