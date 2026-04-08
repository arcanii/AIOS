#!/bin/bash
VH=include/aios/version.h
OLD=$(grep AIOS_VERSION_MINOR "$VH" | head -1 | awk '{print $3}')
NEW=$((OLD + 1))
sed -i.bak "s/AIOS_VERSION_MINOR  $OLD/AIOS_VERSION_MINOR  $NEW/" "$VH" && rm -f "$VH.bak"
sed -i.bak "s/AIOS_VERSION_PATCH  [0-9]*/AIOS_VERSION_PATCH  0/" "$VH" && rm -f "$VH.bak"
echo "Minor: $OLD → $NEW (v$(grep AIOS_VERSION_MAJOR $VH | head -1 | awk '{print $3}').$NEW.0)"
