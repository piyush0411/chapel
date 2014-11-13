#!/usr/bin/env bash
#

CWD=$(cd $(dirname $0) ; pwd)
source $CWD/common-perf.bash
source $CWD/common-qthreads.bash

export CHPL_NIGHTLY_TEST_CONFIG_NAME="perf.chap04.playground"

# disabled until there are some more performance comparisons we want to do
#$CWD/nightly -cron -performance-description 'qthreads --genGraphOpts "-m default -m qthreads"' -releasePerformance -numtrials 5 -startdate 07/28/12
