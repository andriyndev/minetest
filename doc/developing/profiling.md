# Profiling

## Comparing client mesh preparation

Use the same build type, world, route, view range, mesh worker count and FPS
limit for both builds. Prefer `RelWithDebInfo` for performance comparisons.
Visit an already-generated area to reduce variation from map generation, and
restart the client before each run. Repeat each run several times, alternating
builds; a single run can be affected by filesystem caches and server load.

Set `profiler_print_interval = 1` in the configuration and record a separate log
for each run:

```sh
./bin/luanti --info --logfile /tmp/luanti-mesh-patched-1.log
```

Record the build revision, configuration, start position and route with the log.
Move along the route, then remain stationary until loading finishes. Record when
you stopped moving so that the remaining loading time can be compared. FPS alone
is insufficient when both builds reach the configured FPS limit.

The main-thread mesh preparation profiler reports these values per logging
interval:

* `Client: Mesh input preparation (sum)`: total preparation time in milliseconds.
* `Mesh preparation: step [us]`: maximum preparation time in a client step.
* `Mesh preparation: jobs`: snapshots prepared, including replacements of ready
  jobs; this is not a count of completed meshes.
* `Mesh preparation: budget exhausted`: steps that reached the preparation time
  limit after preparing a job. One snapshot can exceed the limit, and scanning
  deferred requests can also take time.
* `Mesh queue: pending` and `pending peak`: average and maximum pending count.
* `Mesh queue: ready normal`, `ready urgent` and `running`: average job counts
  sampled after preparation. These are not worker utilization measurements.

The queue samples exclude completed results waiting for the client to apply
them. Empty input queues alone therefore do not establish that all meshes are
visible. The preparation timer excludes reporting these profiler measurements.

Older builds may lack these metrics. Compare common measurements and visible
loading time across builds, and use the new metrics to explain the patched
build's behavior. For individual frame stalls and CPU hotspots, use the tracing
or sampling tools described below rather than interval averages alone.

## Using builtin's mod profiler

Builtin has a profiler for callbacks registered by mods. It measures how much
time each callback took per server-step (on average / min / max).

To start, enable the `profiler.load` setting.
(There are more settings, see `profiler.*` and `instrument.*`.)

Use the `profiler` chatcommand to look at the results.


## Profiling Luanti on Linux with perf

We will be using a tool called "perf", which you can get by installing `perf` or `linux-perf` or `linux-tools-common`.

To get usable results you need to build Luanti with debug symbols
(`-DCMAKE_BUILD_TYPE=RelWithDebInfo` or `-DCMAKE_BUILD_TYPE=Debug`).

Run the client (or server) like this and do whatever you wanted to test:
```bash
perf record -z --call-graph dwarf -- ./bin/luanti
```

This will leave a file called "perf.data".

You can open this file with perf built-in tools but much more interesting
is the visualization using a GUI tool: **[Hotspot](https://github.com/KDAB/hotspot)**.
It will give you flamegraphs, per-thread, per-function views and much more.

### Remote Profiling

Attach perf to your running server, press *^C* to stop:
```bash
perf record -z --call-graph dwarf -F 400 -p "$(pidof luantiserver)"
```

Collect a copy of the required libraries/executables:
```bash
perf buildid-list | grep -Eo '/[^ ]+(luantiserver|\.so)[^ ]*$' | \
	tar -cvahf debug.tgz --files-from=- --ignore-failed-read
```

Give both files to the developer and also provide:
* Linux distribution and version
* commit the source was built from and/or modified source code (if applicable)

Hotspot will resolve symbols correctly when pointing the sysroot option at the collected libs.


## Profiling with Tracy

[Tracy](https://github.com/wolfpld/tracy) is
> A real time, nanosecond resolution, remote telemetry, hybrid frame and sampling
> profiler for games and other applications.

It allows one to annotate important functions and generate traces, where one can
see when each individual function call happened, and how long it took.

Tracy can also record when frames, e.g. server step, start and end, and inspect
frames that took longer than usual. Luanti already contains annotations for
its frames.

See also [Tracy's official documentation](https://github.com/wolfpld/tracy/releases/latest/download/tracy.pdf).

### Installing

Tracy consists of a client (Luanti) and a server (the gui).

Install the server, e.g. using your package manager.

### Building

Build Luanti with `-DBUILD_WITH_TRACY=1`, this will fetch Tracy for building
the Tracy client. And use `FETCH_TRACY_GIT_TAG` to get a version matching your
Tracy server, e.g. `-DFETCH_TRACY_GIT_TAG=v0.11.0` if it's `0.11.0`.

To actually use Tracy, you also have to enable it with Tracy's build options
(`-DTRACY_ENABLE=1`).

See Tracy's documentation for more build options.

TL;DR:
```
-DBUILD_WITH_TRACY=1 -DFETCH_TRACY_GIT_TAG=<your_version> -DTRACY_ENABLE=1 -DTRACY_ONLY_LOCALHOST=1
```

### Using in C++

Start the Tracy server and Luanti. You should see Luanti in the menu.

To actually get useful traces, you have to annotate functions with `ZoneScoped`
macros and recompile. Please refer to Tracy's official documentation.

### Using in Lua

Tracy also supports Lua.
If built with Tracy, Luanti loads its API in the global `tracy` table.
See Tracy's official documentation for more information.

Note: The whole Tracy Lua API is accessible to all mods. And we don't check if it
is or becomes insecure. Run untrusted mods at your own risk.

Enable `profiler.load` and `profiler.tracy` to automatically instrument mod
callback functions.
