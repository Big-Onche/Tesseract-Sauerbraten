Name "Sauerract"

OutFile "sauerract_20YY_MM_DD_foo_edition_windows.exe"

InstallDir $PROGRAMFILES\Sauerbraten

InstallDirRegKey HKLM "Software\Sauerract" "Install_Dir"

SetCompressor /SOLID lzma
XPStyle on

Page components
Page directory
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles

Section "Sauerract (required)"

  SectionIn RO

  SetOutPath $INSTDIR

  File /r "..\..\binaries"
  File /r "..\..\sauerract"
  File /r "..\..\src_sauerract"
  File "..\..\sauerract.bat"
  File "..\..\sauerract.sh"
  File "..\..\sauerract.command"
  File "..\..\README.md"
  File "..\..\LICENSE"

  WriteRegStr HKLM SOFTWARE\Sauerract "Install_Dir" "$INSTDIR"

  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Sauerract" "DisplayName" "Sauerract"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Sauerract" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Sauerract" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Sauerract" "NoRepair" 1
  WriteUninstaller "uninstall.exe"

  IfFileExists "$DOCUMENTS\My Games\Sauerract\sauerract\config\config.cfg" ConfigFound NoConfig
  ConfigFound:
     Delete "$DOCUMENTS\My Games\Sauerract\sauerract\config\old-config.cfg"
     Rename "$DOCUMENTS\My Games\Sauerract\sauerract\config\config.cfg" "$DOCUMENTS\My Games\Sauerract\sauerract\config\old-config.cfg"
  NoConfig:

SectionEnd

Section "Start Menu Shortcuts"

  CreateDirectory "$SMPROGRAMS\Sauerract"

  CreateDirectory "$DOCUMENTS\My Games\Sauerract"

  SetOutPath "$INSTDIR"

  CreateShortCut "$INSTDIR\Sauerract.lnk"                "$INSTDIR\sauerract.bat" "" "$INSTDIR\binaries\windows\sauerract.exe" 0 SW_SHOWMINIMIZED
  CreateShortCut "$SMPROGRAMS\Sauerract\Sauerract.lnk"   "$INSTDIR\sauerract.bat" "" "$INSTDIR\binaries\windows\sauerract.exe" 0 SW_SHOWMINIMIZED
;  CreateShortCut "$SMPROGRAMS\Sauerract\README.lnk"      "$INSTDIR\README.html"   "" "$INSTDIR\README.html" 0

  CreateShortCut "$INSTDIR\User Data.lnk"                "$DOCUMENTS\My Games\Sauerract"
  CreateShortCut "$SMPROGRAMS\Sauerract\User Data.lnk"   "$DOCUMENTS\My Games\Sauerract"

  CreateShortCut "$SMPROGRAMS\Sauerract\Uninstall.lnk"   "$INSTDIR\uninstall.exe" "" "$INSTDIR\uninstall.exe" 0
SectionEnd

Section "Uninstall"

  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Sauerract"
  DeleteRegKey HKLM SOFTWARE\Sauerract

  RMDir /r "$SMPROGRAMS\Sauerract"
  RMDir /r "$INSTDIR\binaries"
  RMDir /r "$INSTDIR\sauerract"
  RMDir /r "$INSTDIR\src_sauerract"
  Delete "$INSTDIR\sauerract.bat"
  Delete "$INSTDIR\sauerract.sh"
  Delete "$INSTDIR\sauerract.command"
  Delete "$INSTDIR\Sauerract.lnk"

SectionEnd
