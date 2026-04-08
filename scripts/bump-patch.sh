#!/bin/bash
VH=include/aios/version.h
OLD=$(grep AIOS_VERSION_PATCH "$VH" | head -1 | awk '{print $3}')
NEW=$((OLD + 1))
sed -i.bak "s/AIOS_VERSION_PATCH  $OLD/AIOS_VERSION_PATCH  $NEW/" "$VH" && rm -f "$VH.bak"
echo "Patch: $OLD → $NEW (v$(grep AIOS_VERSION_MAJOR $VH | head -1 | awk '{print $3}').$(grep AIOS_VERSION_MINOR $VH | head -1 | awk '{print $3}').$NEW)"
