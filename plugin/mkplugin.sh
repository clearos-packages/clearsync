#!/bin/bash
# Generate plugin skeleton from example source.

# Ask for short-name:
if [ $# -gt 0 ]; then BUFFER=$1; shift 1; fi
read -e -i "${BUFFER}" -p "Enter a short name (ex: csplugin-example): " SHORT_NAME
# Ask for long-name (descriptive name):
if [ $# -gt 0 ]; then BUFFER=$1; shift 1; fi
read -e -i "${BUFFER}" -p "Enter a long name (ex: Example, or \"Foo Bar\"): " LONG_NAME

# Sanitize names:
SHORT_NAME=$(echo "${SHORT_NAME}" | sed -e 's/[[:space:]/\*()]*//g')
LONG_NAME=$(echo "${LONG_NAME}" | sed -e 's/["\*()]*//g')
AM_SHORT_NAME=$(echo ${SHORT_NAME} | sed -e 's/-/_/g')

if [ -d "${SHORT_NAME}" ]; then
    echo "$0: ${SHORT_NAME}: exists.  Please remove manually."
    exit 1
fi

echo "Creating plugin: ${SHORT_NAME} (${LONG_NAME})..."
mkdir -vp "${SHORT_NAME}/m4" || exit 1
cp -v ../autogen.sh "${SHORT_NAME}/" || exit 1

FILES="configure.ac csplugin-example.conf csplugin-example.cpp csplugin-example.spec.in Makefile.am"

for F in $FILES; do
    OUTF=$(echo $F | sed -e "s/csplugin-example/${SHORT_NAME}/g")
    sed \
        -e "s/csplugin-example/${SHORT_NAME}/g" \
        -e "s/csplugin_example/${AM_SHORT_NAME}/g" \
        -e "s/example/${LONG_NAME}/g" \
        -e "s/Example/${LONG_NAME}/g" ${F} > "${SHORT_NAME}/${OUTF}"
done

#pushd "${SHORT_NAME}"
#./autogen.sh || exit 1
#./configure || exit 1
#popd

exit 0

# vi: expandtab shiftwidth=4 softtabstop=4 tabstop=4
