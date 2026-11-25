#!/bin/sh
cd $(dirname "$0")
while [ -n "$1" ]; do
 case "$1" in
  clean)
   xcodebuild -project sauerract.xcodeproj clean -configuration Release
   ;;
  build)
   xcodebuild -project sauerract.xcodeproj -configuration Release -alltargets
   ;;
  install)
   mkdir -p ../../binaries/mac
   cp -v build/Release/sauerract.app/Contents/MacOS/sauerract ../../binaries/mac/sauerract
   chmod +x ../../binaries/mac/sauerract
   ;;
  package)
   exec ./package.sh
   ;;
 esac
 shift
done
