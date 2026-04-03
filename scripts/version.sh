#!/bin/bash
VH=include/aios/version.h
BH=include/aios/build_number.h
MAJ=$(grep AIOS_VERSION_MAJOR "$VH" | head -1 | awk '{print $3}')
MIN=$(grep AIOS_VERSION_MINOR "$VH" | head -1 | awk '{print $3}')
PAT=$(grep AIOS_VERSION_PATCH "$VH" | head -1 | awk '{print $3}')
BLD=$(grep AIOS_BUILD_NUMBER "$BH" | awk '{print $3}')
echo "AIOS v${MAJ}.${MIN}.${PAT} (build ${BLD})"
