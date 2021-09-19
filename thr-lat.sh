#!/bin/bash

CPUS=("$@")

for op in 2 3; do
        printf "#%-5s" "Op=$op"
        for t1 in "${CPUS[@]}"; do
                printf " %3d" $t1
        done
        echo

        for t0 in "${CPUS[@]}"; do
                printf "%6d" $t0
                for t1 in "${CPUS[@]}"; do
                        if [ "$t0" = "$t1" ]; then
                                printf "   -"
                        else
                                lat=$(./cas-bench -o "$op" -r 1 -t 2 0:"$t0" 1:"$t1" | awk '/^threads/{print $NF}')
                                printf " %3d" $lat
                        fi
                done
                echo
        done
        echo
done
