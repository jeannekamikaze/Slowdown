# Slowdown

Slow down Seven Years War (1997) (and possibly other old games).

## Build

You will need a 32-bit build of the tool to match the game. If using Visual
Studio, open an `x86 Native Tools Command Prompt for VS` and build with CMake:

```bash
mkdir build
cmake -S . -B build
cmake --build build --config Release
```

## Usage

```bash
slowdown.exe C:\games\SYW\syw.exe --ms 10
```

Replace the game path above accordingly.

Try playing with other settings if the game runs too slow/fast.

|Option|Comment|
|-|-|
|--ms   X|Add a constant X millisecond delay to every game frame.|
|--mspf X|Make every game frame last for X milliseconds.|
|--fps  X|Target the given frame rate.|
|--busywait|Insert busy waits instead of putting the thread to sleep. More accurate timing, higher CPU usage.|

## Rationale & Frame Anatomy

Seven Years War (and likely other old games) does not track time elapsed between
frames. It was basically written under the (implciit?) assumption that it would
run on a processor of its time. Consequently, it will easily clock 1000+ fps on
modern systems, making it unplayable. The various Windows compatibility modes
also did not help.

SYW's game loop is as follows, which should be typical of other games of the
time:

```c++
while (!done)
{
    if (PeekMessageA(...) == 0) // No messages
    {
        GameFrame();
        WaitMessage();
    }
    else
    {
        if (GetMessageA(...) == 0)
            break;
        TranslateMessage(...);
        DispatchMessageA(...);
    }
}
```

The game checks for window messages and processes any available.
Otherwise, it proceeds to produce a frame.
The unconditional `WaitMessage()` at the end of the frame does not actually
block the game unless you minimize its window. The game posts artificial
`WM_MOUSEMOVE` events to itself to keep the loop going.
So, basically, `PeekMessageA() == 0` marks the start of a new frame.

Slowdown hooks `PeekMessageA()` and inserts a delay on a return value of `0`
(the start of a new frame).

## Delay Options

The different options in the Usage section explain what kind of delay is
introduced.

My initial thought was to make every frame last a certain amount of time, like
a modern game tracking time and throttling itself to hit a target. However,
based on my (possibly flawed) perception of the game, this seems to introduce
subtle jitter. In the disassembly, we can also see short paths in the game loop
where a jump (`continue`) statement skips over to produce the next frame.

On a second thought, given that the game has no pacing to begin with, I realized
that the assumption that every frame should take the same amount of time is
probably wrong. That is, the game, as originally intended, would simply run on
the assumption that `PeekMessageA()` takes some roughly constant amount of time.
Each frame then takes more or less time depending on what path it follows.
Therefore, to mimic the original behaviour, I implemented the option to insert
a constant delay instead. This seems to work better.

## Busy Wait

The `Sleep()` API call on Windows seems to incur relatively long and random
thread scheduling delays. It works well on the order of 100s of milliseconds,
but not 10. `std::this_thread::sleep_for()` also was not better (on Windows
anyway.) Running Slowdown with a delay of 10ms often ends up delaying by 11 or
12ms instead.

As an option, Slowdown can insert a busy wait instead. This results in much more
accurate delays at the expense of higher CPU usage (basically 100% of a single
core.) It is a trade-off between accuracy and power use (and noise). You
probably do not want to use it when running on a laptop battery, and on a
desktop the fan noise can get annoying.
