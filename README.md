## hatTAS
### Work-in-progress TAS tools for A Hat in Time

The goal for this project is to be able to run TAS replay files for A Hat in Time.

Currently, it supports replays that start immediately, as well as replays that wait for the act timer to start. There is experimental support for replays that wait for the game timer to resume.

hatTAS does not make any lasting modifications to the game. Rather, it patches it in memory. If it messes up anything, you should be able to restart the game and try again.

## Notes
This program is very early in development, and thus is not ready for general usage yet. It has many compatibility issues.

**Note**: the game only accepts inputs if it's in focus.

**Note**: this program only works if you have a (possibly virtual) XInput (Xbox 360, for example) controller connected to the game, currently. You can use x360ce to accomplish this if needed.

**Note**: this program only supports a few versions of the game, due to pointers in the executable likely changing. It's mainly the "DLC1.5" patch (`8061143192026666389`) and, as of 28/07/2019, current patch (`7770543545116491859`). Please visit the Boop Troop (AHiT speedrunning) discord server or look at `versions.ini` for more information.

## Usage
To use this program, open the command line in the current directory, and run `hatTAS.exe <movie.htas>`. For example, `hatTAS.exe sample.htas`. If using an immediate type TAS, remember to give the game focus quickly, or the inputs won't go through.

There's also an `-r` switch, which will make it repeat whenever the TAS starting condition becomes true again, also re-parsing the `.htas` file in the process. Use `control+c` to quit out of it gracefully.

If you want to break out of the program while the TAS is playing, press the `escape` key.

You can also frame-step by hitting the `right shift` key while a movie is playing. Use `left control` to quit frame-stepping and resume at normal speed.

## .htas file format
.htas is a custom extension for replay files, which are specific to this tool. `sample.htas` contains a short example. I will also detail the format here.

Comments are written with two slashes (`//`), and comment out the remainder of the line. Comments on header lines are not supported currently.

### Header
The header consists of key-value pairs. There are currently three different keys possible, to specify metadata. Each key **must** be followed by a colon and space, with the value immediately following. Keys and types are case-sensitive. The currently available keys are: `name`, `type`, `length`, `fps`, `players`

The `name` key specifies the name of the TAS replay. Default is `Generic TAS`.

The `type` key specifies the type of TAS replay. The currently available types are `IL`, `immediate`, and `fullgame` (experimental). The `IL` type waits for the act timer to start counting up from zero, where it will then start. The `immediate` type will start the inputs immediately. The `fullgame` type waits for the game timer to resume from a paused state. Default is `IL`.

The `length` key specifies the length of the TAS replay, in frames. There is no default; this key is required.

The `fps` key specifies the in-game framerate that shall be used for the TAS. Defaults to `60`.

The `players` key specifies the maximum amount of controllers to use. Defaults to `1`.

Example header:
   
```
name: My cool TAS
type: IL
length: 240
fps: 30
```

The above would specify a replay with the name `My cool TAS`, an `IL` type, and a length of `240` frames, running at `30` frames per second.

### Body
The body consists of a list of frame numbers, each with a list of *input events*. The frame numbers work like the header keys above, in that they need to be immediately followed by a colon and space. You can specify input events after the colon and space. Frame numbers have, by convention, leading `0` padding, such that they are six characters long.

All input events are written in all-caps and must have spaces around them, such that they can be delimited. The available regular input events are `A`, `B`, `X`, `Y`, `LB`, `RB`, `LT`, `RT`, `START`, `BACK`, `UP`, `RIGHT`, `DOWN`, `LEFT`.

To release a button, simply type a `~` (with no spaces in between) before the button name.

Note that `UP`, `RIGHT`, `DOWN`, `LEFT` have some peculiarities, in that they do not allow opposite buttons to be pressed at the same time. If an impossible combination of these buttons are pressed, they will take precedence in the following order: `UP`, `RIGHT`, `DOWN`, `LEFT`.

There are also some special input events - the analog sticks. They have the format `axis:value`. `~` without a value puts it back to neutral. The available values are any number between `-32768` and `32767`, `0` being neutral on the axis, `-32768` being left/down on the `X/Y` axis, and `32767` being right/up on the `X/Y` axis. The available analog axes are `LX`, `LY`, `RX`, `RY`.

There is also a special command to change the playback speed, called `SPEED`. Set it like you'd set an axis, with the input number being the speed multiplier. Useful for developing the replay. Note that the max playback speed is limited by your PC specs. `0` sets it to uncapped, meaning it will go as fast as it can.

Another command is `RNG`, which will set the cyclic return values of `rand()`, for RNG manipulation. By default, `rand()` will always return `0` while a movie is playing. `RNG` takes a comma-separated list of numbers to cycle through when `rand()` is called. `RNG:4,2,5`, for example. `~RNG` returns it to the default `0`. Try to think of it like a rigged fortune wheel, which is turned a single tick every time `rand()` is called.

Example body:
```
000010: LY:32767 X   // walk forward and attack
000045: ~LY ~X       // go neutral on LY, and let go of attack
000046: X LEFT       // attack once more, while switching hat left
000070: SPEED:2      // set playback rate to 2x speed
000100: DOWN ~SPEED  // smooch, and go back to 1x speed
```

### Resources
\[outdated] .htas editor UI, made by wooferzfg - https://github.com/wooferzfg/hat-tas-ui (still works for `ds4_deprecated` branch)

The Boop Troop discord server - https://discordapp.com/invite/HJEgHBw