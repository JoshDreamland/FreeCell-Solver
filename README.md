# Yet Another FreeCell Solver

Except, you know, this one can be built and run in order to solve a game of
FreeCell, and you don't even have to fuck with perl or cmake for four hours.
Jesus.

## Building

This project isn't hard to build. I recommend the following line:

```
g++ freecell.cc -O3 -s -DUSE_CURSES -lncurses
```

But if you don't have ncurses, you can build it the old-fashioned way:

```
g++ freecell.cc -O3 -s
```

Note that the -O3 and -s are optional; you may replace them with your own
optimization flags or discard them entirely. C++11 is required, so if your
compiler is sort of old, you may need `--std=c++11` or the like.

## Running

Put your game data in a file and call the solver on it like this:

```
freecell game.dat --interactive
```

The `--interactive` flag is optional; it will walk you through moves visually
rather than just vomiting out instructions.

If you would like the boards rendered without interactivity, you can pass
the `--print_boards` flag instead.

## Game Data

As the program will tell you, input is formatted like this:

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

Without interactivity enabled, the default output looks like this:

```
Move the Nine of Hearts onto an empty reserve
Move the Four of Spades onto an empty reserve
Move the Ace of Hearts onto the foundation
Move the Five of Spades onto an empty reserve
Move the Three of Clubs onto the Four of Hearts
 . . .
Move the King of Diamonds onto the foundation
Move the Queen of Clubs onto the foundation
Move the King of Clubs onto the foundation
```

Printing boards (or running in non-curses interactive mode), the output
looks like this:

![Board print output](http://i.imgur.com/KRTLs3C.png)

Running in curses interactive mode, the output is controlled using the mouse
wheel or arrow keys, and display can be terminated at any time using 'Q'.
The output looks like this:

![Curses interface](http://i.imgur.com/NNmZufP.png)
