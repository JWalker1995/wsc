#!/bin/sh

sort --unique wsc.files | xargs ls -1d 2>/dev/null > wsc.files.tmp
mv wsc.files.tmp wsc.files

printf "src/\nbuild-qtc/src/\n" > wsc.includes
