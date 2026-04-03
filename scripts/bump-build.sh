#!/bin/bash
BH=include/aios/build_number.h
OLD=$(grep AIOS_BUILD_NUMBER "$BH" | awk '{print $3}')
NEW=$((OLD + 1))
echo "#define AIOS_BUILD_NUMBER $NEW" > "$BH"
echo "Build: $OLD → $NEW"
