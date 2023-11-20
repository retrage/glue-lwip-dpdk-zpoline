# A glue for lwIP on DPDK using zpoline

**WARNING: Some commands described in this README need the root permission (sudo). So, please conduct the following procedure only when you understand what you are doing. The authors will not bear any responsibility if the implementations, provided by the authors, cause any problems.**

## Compilation

First, please clone this repository, and enter the directory.

```
git clone https://github.com/yasukata/glue-lwip-dpdk-zpoline.git
```

```
cd glue-lwip-dpdk-zpoline
```

Then, please type the following command to compile files in this repository; this command will download lwIP and DPDK source code, and they are compiled and installed in glue-lwip-dpdk-zpoline/lwip and glue-lwip-dpdk-zpoline/dpdk, respectively.

```
make
```

Afterward, please download zpoline files in the glue-lwip-dpdk-zpoline directory.

```
git clone https://github.com/yasukata/zpoline.git
```

The following compiles the files in the zpoline directory.

```
make -C zpoline
```

## Arguments

The arguments can be specified through environment variables.

- ```NET_ADDR```: IPv4 address
- ```NET_MASK```: netmask
- ```NET_GATE```: IPv4 gateway address
- ```DPDK_ARGS```: arguments to be passed to DPDK's ```rte_eal_init```.

## Run

The following command is an example to launch a redis-server process with a tap device named tap001.

**WARNING: Your system including your Redis database file can be broken if the program in this repository has a bug. So, please never run the following command on an environment that you do not want to destroy.**

```
sudo NET_ADDR=10.100.0.20 NET_MASK=255.255.255.0 NET_GATE=10.100.0.1 DPDK_ARGS="-l 0 --vdev=net_tap,iface=tap001 --no-pci" LD_LIBRARY_PATH=./dpdk/install/lib/x86_64-linux-gnu LIBZPHOOK=./libzphook_lwip.so LD_PRELOAD=./zpoline/libzpoline.so PATH_TO/redis-server --protected-mode no
```

In another terminal/console, ...

the following assigns the IP address to tap001,

```
sudo ifconfig tap001 10.100.0.1 netmask 255.255.255.0
```

then, the following command will connect to the redis-server launced by the command above using telnet,

```
telnet 10.100.0.20 6379
```

or, the following executes a redis-benchmark process that sends requests to the redis-server program.

```
PATH_TO/redis-benchmark -e -h 10.100.0.20 -p 6379 -n 10000 -c 1 -t get --threads 1
```

## Note

This implementation assumes to be used with single-threaded networked applications such as Redis.
