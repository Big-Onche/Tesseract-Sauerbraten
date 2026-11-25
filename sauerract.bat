@ECHO OFF
SETLOCAL

SET "SAUERRACT_ROOT=%~dp0"
SET "SAUERRACT_BIN=%SAUERRACT_ROOT%binaries\windows\sauerract.exe"

IF NOT EXIST "%SAUERRACT_ROOT%packages\base\" (
    ECHO Sauerract requires an existing Sauerbraten installation. 1>&2
    ECHO Place this overlay in the Sauerbraten installation root ^(packages\base is missing^). 1>&2
    EXIT /B 1
)

IF NOT EXIST "%SAUERRACT_BIN%" (
    ECHO Sauerract binary not found: "%SAUERRACT_BIN%" 1>&2
    ECHO Build it from src_sauerract or install a release binary. 1>&2
    EXIT /B 1
)

PUSHD "%SAUERRACT_ROOT%" || EXIT /B 1
"%SAUERRACT_BIN%" "-u%USERPROFILE%\Documents\My Games\Sauerract" %*
SET "SAUERRACT_EXIT=%ERRORLEVEL%"
POPD
EXIT /B %SAUERRACT_EXIT%
