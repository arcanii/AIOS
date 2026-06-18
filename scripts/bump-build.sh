#!/bin/bash
BH=include/aios/build_number.h
OLD=$(grep AIOS_BUILD_NUMBER "$BH" | awk '{print $3}')
NEW=$((OLD + 1))
echo "#define AIOS_BUILD_NUMBER $NEW" > "$BH"
echo "Build: $OLD → $NEW"

# Stamp the wall-clock build time alongside the build number. Weekday and the
# timezone name are not available from the C __DATE__/__TIME__ macros, so capture
# the host date here (PRE_BUILD) into a generated header. Gitignored + created by
# setup-linux.py, exactly like build_number.h above.
TH=include/aios/build_time.h
echo "#define AIOS_BUILD_TIME \"$(date '+%a %b %e %H:%M:%S %Z %Y')\"" > "$TH"
echo "Build time: $(date '+%a %b %e %H:%M:%S %Z %Y')"
