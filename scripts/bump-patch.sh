#!/bin/bash
# Increment AIOS_VERSION_PATCH in include/aios/version.h
VH=include/aios/version.h
OLD=$(grep AIOS_VERSION_PATCH "$VH" | head -1 | awk '{print $3}')
NEW=$((OLD + 1))
sed -i '' "s/AIOS_VERSION_PATCH  $OLD/AIOS_VERSION_PATCH  $NEW/" "$VH"
echo "Patch: $OLD → $NEW (v$(grep AIOS_VERSION_MAJOR $VH | head -1 | awk '{print $3}').$(grep AIOS_VERSION_MINOR $VH | head -1 | awk '{print $3}').$NEW)"
