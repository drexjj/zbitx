#!/bin/bash
sudo fuser -vu /dev/snd/* /dev/snd/by-path/*
cd /home/pi/sbitx

./zbitx

bash
read -p "Press enter to continue..."
