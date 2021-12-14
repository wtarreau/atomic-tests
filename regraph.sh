#!/bin/sh
SCALE=${1:-250}
DIR=$(dirname $0)

# file, #cores, [opt_title]
do_graph() {
  $DIR/graph-lat.sh "${SCALE}" "$2" "${3:-${1%.*}}" < "$1" > "${1%.*}.html"
}

do_graph ryzen-op2.raw 16 "Ryzen R/O"
do_graph ryzen-op3.raw 16 "Ryzen R/W"

do_graph epyc-op2.raw 24 "Epyc R/O"
do_graph epyc-op3.raw 24 "Epyc R/W"

do_graph xeon-op2.raw 8 "Xeon R/O"
do_graph xeon-op3.raw 8 "Xeon R/W"

do_graph i7-8650U-op2.raw 4 "Core i7-8650U R/O"
do_graph i7-8650U-op3.raw 4 "Core i7-8650U R/W"

do_graph armada-8040-op2.raw 4 "Armada 8040 R/O"
do_graph armada-8040-op3.raw 4 "Armada 8040 R/W"

do_graph lx2-op2.raw 16 "LX2160A R/O"
do_graph lx2-op3.raw 16 "LX2160A R/W"

for i in 0 $(seq 0 $(((SCALE+9)/10))); do
        echo -n "$((i*10)) "
done | ../graph-lat.sh ${SCALE} $(((SCALE+9/10)+1)) "Scale R/O" > all2.html

for i in 0 $(seq 0 $(((SCALE+9)/10))); do
        echo -n "$((i*10)) "
done | ../graph-lat.sh ${SCALE} $(((SCALE+9/10)+1)) "Scale R/W" > all3.html

names=(epyc ryzen xeon i7-8650U armada-8040 lx2)

for i in ${names[@]}; do
        cat $i-op2.html >> all2.html
        cat $i-op3.html >> all3.html
done

cat all3.html all2.html > all.html
