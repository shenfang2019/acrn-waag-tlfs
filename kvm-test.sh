#!/bin/bash

# offline SOS CPUs except BSP before launch UOS
for i in `ls -d /sys/devices/system/cpu/cpu[1-99]`; do
        online=`cat $i/online`
        idx=`echo $i | tr -cd "[1-99]"`
        echo cpu$idx online=$online
        if [ "$online" = "1" ]; then
                echo 0 > $i/online
                echo $idx > /sys/class/vhm/acrn_vhm/offline_cpu
        fi
done

/usr/bin/acrn-dm -A -m 4096M --debugexit -E $1 -s 0:0,hostbridge -s 1:0,lpc -l com2,stdio kvm_test

