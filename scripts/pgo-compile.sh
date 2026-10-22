#!/bin/sh
# The compiler launcher of builds that use a profile (scripts/build.sh, IROISE_PGO=use): ccache, with -Wno-error
# after Ladybird's -Werror. Reading a profile, gcc warns of functions it has no counts for ("Missing counts for
# called function"), a warning no -Wno-... option turns off; it stays a warning.
exec ccache "$@" -Wno-error
