#!/bin/bash

#        <threads> <qmin> <qmax> <total> <max/thd> <us wait> <chance 1/n> [non-blocking (0)] [print lost (0)] [delay sec]
default=( 5        50     200    100000  1000      100       10           0                  0                5         )

[ $# -gt 0 ] && params=(${*}) || params=(${default[*]})

/usr/bin/time -v build/logger ${params[*]} > build/out.log 2>&1 &
less -RS +F < build/out.log
