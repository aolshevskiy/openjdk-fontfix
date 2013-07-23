#!/bin/bash
set -e
cd $(dirname $0)/openjdk7
unset JAVA_HOME

bash ./configure
make all



