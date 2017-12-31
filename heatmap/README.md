# heatmap

This is a heatmap visualization tool for Leela-Zero. When launched, it run
leelaz from the current directory using weights.txt, play a game against
itself and display a heatmap as it does so.

# Requirements

* Qt 5.3 or later with qmake
* C++14 capable compiler

## Example of compiling - Ubuntu

    sudo apt install qt5-default qt5-qmake curl
    qmake -qt5
    make

# Running

Copy the compiled leelaz binary and some weights into the heatmap directory, and run
heatmap.

    cp ../src/leelaz .
    cp ../weights.txt .
    ./heatmap

