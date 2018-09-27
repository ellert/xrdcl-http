# xrdcl-http - HTTP client plugin for XRootD

## Overview

`xrdcl-http` is an XRootD client plugin which allows XRootD to interact
with HTTP repositories.

## Build

This project is configured and build using CMake.

## Testing

The integration tests are found in the `test` directory. To run them, the environment variable `XROOTD_PREFIX` needs to point to a prefix where XRootD and XrdCl-Http are installed:

$ XROOTD_PREFIX=/usr/local ./test/run_test.sh

To run a single test case, provide a patter as a second argument to the script:

# XROOTD_PREFIX=/usr/local ./test/run_test.sh "001-*"