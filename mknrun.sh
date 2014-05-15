#!/bin/sh
sudo make
if [ $? -eq 0 ]; then
./vfat -s -f testfs.fat fat &
fi
