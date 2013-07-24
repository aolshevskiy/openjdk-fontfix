#!/bin/bash
set -e
cd $(dirname $0)/openjdk7
unset JAVA_HOME

FREETYPE2_LIBS='-lfreetype -lfontconfig' bash ./configure
make all



