#!/bin/sh

libtoolize --force --copy
#gettextize --force --copy --intl
autoheader
aclocal -I m4
autoconf
automake -a -c

rm autom4te.cache/*
rmdir autom4te.cache

