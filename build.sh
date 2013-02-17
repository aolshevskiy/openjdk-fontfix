#!/bin/sh

## system-dependent local folders
## path to installed Sun JDK 6:
ALT_BOOTDIR=/opt/java6

#########################################################################################
#      DO NOT EDIT
#########################################################################################

#set -x

## exporting local env vars:
export ALT_BOOTDIR
## resolving canonical path of current dir + '/drops' suffix:
export ALT_DROPS_DIR="$( readlink -f "$( dirname "$0" )" )/openjdk7/drops"

## unchangable exports:
export LANG=C
export JAVA_HOME=""
export LD_LIBRARY_PATH=""
export BUILD_NUMBER=b00
export MILESTONE=fontfix
export ALLOW_DOWNLOADS=false
export DISABLE_HOTSPOT_OS_VERSION_CHECK=true
NCPUS=4
export HOTSPOT_BUILD_JOBS=$NCUPS
export PARALLEL_COMPILE_JOBS=$NCPUS

## starting real build process in subshell:
(
  cd $(dirname $0)/openjdk7 && make sanity && make
)
