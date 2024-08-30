# Stack Novation Launchpad Plugin

This is a plugin for [Stack](https://github.com/claytonpeters/stack) that
provides a cue trigger that allows buttons on the Novation Launchpad series of
devices to start/stop/pause cues. This gives you the capabiliyt of using Stack
and the Launchpad as a soundboard or even perhaps a drum machine.

For devices that provide pressure information, cues that play sound can also
be configured so that the audio level is adjusted based on the button pressure.

* Choose button
* Choose button colour
* Button action (play/pause/stop)
* * What to do if cue is already playing? This should probably be a base trigger
    option.
* Enable pressure sensitivity (set live playback volume immediately after cue start)
* Button pulsing/flashing during playback? (How do we tell the trigger that the cue has finished?)

## Building

This plugin uses CMake as its build system. You will need at least version 3.12
of CMake to build. You will need the following library depedencies (along with
their development packages) to compile this plugin:

* cmake
* gcc/g++
* pkg-config
* gtk3
* glib2
* jsoncpp
* [Stack](https://github.com/claytonpeters/stack) itself

The CMake scripts assumes you have a clone of the Stack Git repository at
`../stack`, relative to the checkout of this plugin. If you've got it elsewhere
checked out, change `CMakeLists.txt` and alter the line:

```cmake
include_directories("${PROJECT_SOURCE_DIR}/../stack/src")
```

Change this to point to the `src` directory inside your clone of the Stack
repo.

Compilation (once you have the correct depedencies installed), should be as
simple as:

```shell
cmake .
make
```

This will cmpile the plugin. To use the plugin with Stack, copy the compiled
library `libStackLaunchpadTrigger.so` to the directory containing the other Stack
plugins (which is usually the same directory as the `runstack` binary).

## Configuration

@@TODO@@

