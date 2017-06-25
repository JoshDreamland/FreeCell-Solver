# Yet Another FreeCell Solver

Except, you know, this one can be built and run in order to solve a game of
FreeCell, and you don't even have to fuck with perl or cmake for four hours.
Jesus.

## Building

Try something like this:

```
g++ freecell.cc -O3 -s
```

Note that the -O3 and -s are optional; you may replace them with your own
optimization flags or discard them entirely. C++11 is required, so if your
compiler is sort of old, you may need `--std=c++11` or the like.

## Running

Put your game data in a file and call the solver on it like this:

```
freecell game.dat
```

## Game Data

As the program will tell you, it looks like this:

```
: 6C 9S 2H AC JD AS 9C 7H
: 2D AD QC KD JC JS 3D 2C
: KC TD 7D 9D QD TS 6D 6H
: 8S TH 3H KS 2S QS 8C KH
: AH JH 7C 8H 5H 8D 5D 3S
: 4S TC 4D QH 4C 3C 5C 6S
: 9H 4H 5S 7S
```

The colons are optional as long as newlines are present.
Comments in this file are not supported.

## Output

This project uses unicode.

![Example instruction screenshot](http://i.imgur.com/Mr46aZv.png)

It prints this a lot, prompting for enter after each instruction.
You can change this behavior by editing the output loop under "main"
and possibly by revising `Move::str()` to output in your desired format.
