#!/bin/bash

printf "<html><body>\n"
if [ -n "$3" ]; then
        printf "<h1>$3</h1>\n"
fi

printf "<table cellspacing=1 cellpadding=10 bgcolor=\"#603020\">\n"

awk -v scale="${1:-100}" -v cpus="${2:0}" '
function map_to_rgb(x,    r, g, b)   # r,g,b are local
{
        x = int(x)

        if (x < 48)         r = 128 - x*2     # 128..32
        else if (x < 160)   r = (x - 32) * 2  # 32..255
        else                r = 255

        if (x < 32)         g = 224
        else if (x < 144)   g = 224 - (x-32)*2
        else if (x < 171)   g = 0
        else                g = (x - 171) * 3

        if (x < 64)         b = 224 + int(x/2)
        else if (x < 192)   b = 255 - (x - 64) * 2
        else if (x < 256)   b = 0
        else                b = (x - 256) * 8

        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (b < 0) b = 0; else if (b > 255) b = 255;

        return (r * 65536) + (g * 256) + b
}

{
    if (cpus && cpus <= NF)
        NF=cpus+1
    if (cpus && cpus < NR)
        next;
    print "<tr>"
    for (i=2; i<=NF; i++) {
        if ($i == "-")
           $i = 0
        val=$i/scale
        if (val < 0.0)
           val = 0.0
        if (val > 1.0)
           val = 1.0
        val=val ^ 0.75  # to smooth things a bit and emphasize small values
        #printf("<td bgcolor=\"#%02x%02x%02x\">&nbsp;</td>", 255*val, 32+32*val, 64+64*(1-val))
        #printf("<td bgcolor=\"#%02x%02x%02x\">%d</td>", 255*val, 32+32*val, 64+64*(1-val),$i)
        printf("<td bgcolor=\"#%06x\">%d</td>", map_to_rgb(val*255),$i)
    }
    print "</tr>"
}'

printf "</table></body></html>\n"
