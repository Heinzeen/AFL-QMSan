# AFL-QMSan

This is a modified version of AFL++ 4.10 which supports QMSan. Please refer to the original AFL++ documentation for further details.

## Build

To build AFL-QMSan, use the same process as regular AFL++. The only difference is in some macros that determine what policies to use for QMSan. A fast way to build AFL-QMSan is:
1. clone the repository
```
git clone https://github.com/Heinzeen/AFL-QMSan.git
```
2. Compile
```
cd AFL-QMSan
CFLAGS="-DQMSAN -DQMSAN_FILTERING -DQMSAN_CALLSTACK_EDGES -DQMSAN_CALLSTACK -DQMSAN_EDGES" make clean all
```

```-QMSAN``` is used to activate QMSan, while the other three are used to activate the different policies of the ignore list. A flag can be set to have further debug information (```-DQMSAN_DEBUG```).

## Run

You can run AFL-QMSan with a command line similar to:

```
/path/to/AFL-QMSan/afl-fuzz -U -i in/ -o out/ -m none -- python3 /path/to/QMSan/qsan [application cmdline with @@]
```

It is strongly suggested to use deferred mode. To do so, find **the offset** of a suitable entrypoint (e.g. the ```main``` function of the binary) and set the env variable ```AFL_ENTRYPOINT``` to that value

## Accurate detector

QMSan needs an accurate detector to work. You can build a QEMU-based one by following the instructions in QMSan's repositories and provide its path through the ```QMSAN_PATH``` env variable. If this environment variable is not set, AFL-QMSan will try by default to use valgrind.
