#!/bin/bash

SLUG=$(cat plugin.json | jq -r ".slug")
VERSION=$(cat plugin.json | jq -r ".version")
ARCH_OS_NAME="lin-x64"

echo "Output file: $SLUG-$VERSION-$ARCH_OS_NAME.vcvplugin"

rm -r dist
mkdir -p "dist/$SLUG"
cp -r build/plugin.so res/ plugin.json LICENSE* "dist/$SLUG"
cd dist && tar -c "$SLUG" | zstd -7 -o "$SLUG"-"$VERSION"-"$ARCH_OS_NAME.vcvplugin"