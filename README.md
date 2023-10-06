# attn-server

[![Build Status](https://travis-ci.org/ripple/attn-server.svg?branch=master)](https://travis-ci.org/ripple/attn-server)
[![Build status](https://ci.appveyor.com/api/projects/status/dd42bs8pfao8k82p/branch/master?svg=true)](https://ci.appveyor.com/project/ripple/attn-server)
[![codecov](https://codecov.io/gh/ripple/attn-server/branch/master/graph/badge.svg)](https://codecov.io/gh/ripple/attn-server)

Attestation Server for XRPL Sidechains

## Table of contents

* [Dependencies](#dependencies)
  * [Conan inclusion](#conan-inclusion)
  * [Other dependencies](#other-dependencies)
* [Build and run](#build-and-run)
* [Guide](#guide)

## Dependencies

### Conan inclusion

This project depends on conan (v1.5 and higher, v2.0 not supported) to build it's dependencies. See https://conan.io/ to install conan.

### Other dependencies

* C++20
* [cmake](https://cmake.org) - at least 3.20


## Build and run

1) Create a build directory. For example: build
2) Change to that directory.
3) Configure conan (once before very 1st build)

``` bash
conan profile update settings.cppstd=20 default
conan profile update settings.compiler.libcxx=libstdc++11 default
conan profile update settings.arch=x86_64 default
```

4) Run conan. The command is:

``` bash
conan install -b missing -s build_type=Debug --output-folder . ..
```

(Note: the exact command I use is as follows, but this assumes gcc 12 is used and a gcc12 conan profile is present):
```bash
CC=$(which gcc) CXX=$(which g++) conan install -b missing --profile gcc12 -s build_type=Debug --output-folder . ..
```

5) Create a build file (replace .. with the appropriate directory):
* 5.1) Default. If you don't have installed rippled - this setup will download and build it.
``` bash
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build/generators/conan_toolchain.cmake ..
```
* 5.2) If you have rippled build from source you can use it
``` bash
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=build/generators/conan_toolchain.cmake -DRIPPLE_SRC_DIR=/home/user/repo/rippled -DRIPPLE_BIN_DIR=/home/user/repo/rippled/build-release ..
```

6) Build the project:

``` bash
make -j $(nproc)
```

7) Run

``` bash
./xbridge_witnessd --conf /home/user/repo/config.json
```

## Guide

[Attestation Server Documentation](https://github.com/XRPLF/XRPL-Standards/discussions/92)
