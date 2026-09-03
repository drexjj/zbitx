#!/bin/bash
sudo fuser -vu /dev/snd/* /dev/snd/by-path/*
cd /home/pi/sbitx

export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8

if [  -f  "terminal.log"  ]; then
    mv terminal.log previous.log
fi
set -o pipefail
stdbuf -oL -eL ./zbitx 2>&1  |  tee terminal.log

bash
read -p "Press enter to continue..."
