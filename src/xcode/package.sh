#!/bin/sh
cd $(dirname "$0")

TESS_HOME="../.."

#ensure old package is gone
rm -f tesseract.dmg

echo creating temporary directory
#make the directory which our disk image will be made of
#use /tmp as a destination because copying ourself (xcode folder) whilst compiling causes the dog to chase its tail
TESSPKG=`mktemp -d /tmp/tesspkg.XXXXXX` || exit 1

#leave indicator of where temp directory is in case things break
ln -sf "$TESSPKG" "build/tesspkg"

echo copying package
#copy executable
ditto "$TESS_HOME/tesseract.app" "$TESSPKG/Tesseract.app"

GAMEDIR="$TESSPKG/Tesseract.app/Contents/Resources"

#copy readme and data and remove unneccesary stuff
ditto "$TESS_HOME/README.html" "$TESSPKG/"
ditto "$TESS_HOME/docs" "$TESSPKG/docs"
ditto "$TESS_HOME/data" "$GAMEDIR/data"
ditto "$TESS_HOME/packages" "$GAMEDIR/packages"
ditto "$TESS_HOME/server-init.cfg" "$GAMEDIR/"
ditto "$TESS_HOME/src" "$TESSPKG/src"
find -d "$TESSPKG" -name ".svn" -exec rm -rf {} \;
find "$TESSPKG" -name ".DS_Store" -exec rm -f {} \;
rm -rf "$TESSPKG/src/xcode/build"
ln -sf /Applications "$TESSPKG/Applications"

#finally make a disk image out of the stuff
echo creating dmg
hdiutil create -fs HFS+ -srcfolder "$TESSPKG" -volname tesseract tesseract.dmg

echo cleaning up
#cleanup
rm -rf "$TESSPKG"

echo done!

