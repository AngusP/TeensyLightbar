#!/bin/sh
# Echo the current time to the Serial port ttyACM0
# UNIX timestamp with lightbar header

serial_port=$1

thetime=$(date +%s)
channel=0x01
sizeoftime=$(echo ${#thetime} + 2 | bc)

echo "Syncing time with controller on $serial_port.\nTime is $thetime ($(date +%Y-%m-%dT%H:%M:%S))"
printf "Sending: %x %x %x %x %x %x\n" 0x01 0xFF 0x00 $sizeoftime 0xED 0xED
printf "%x %x %x %x %x %x" 0x01 0xFF 0x00 $sizeoftime 0xED 0xED > $serial_port

exit $?
