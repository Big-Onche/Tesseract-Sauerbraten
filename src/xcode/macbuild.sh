#!/bin/sh
cd $(dirname "$0")
while [ -n "$1" ]; do
 case "$1" in
  clean)
   xcodebuild -project tesseract.xcodeproj clean -configuration Release
   ;;
  build)
   xcodebuild -project tesseract.xcodeproj -configuration Release -alltargets
   ;;
  install)
   cp -v build/Release/tesseract.app/Contents/MacOS/tesseract ../../tesseract.app/Contents/MacOS/tesseract_universal
   chmod +x ../../tesseract.app/Contents/MacOS/tesseract_universal
   ;;
  package)
   exec ./package.sh
   ;;
 esac
 shift
done
