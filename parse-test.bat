@ECHO OFF
SET install=false
SET clean=false
SET package=false
SET addon=
SET store=

:parseArgs
IF "%~1" == "" GOTO argsParsed
IF "%~1" == "install" (
  SET install=true
) ELSE IF "%~1" == "clean" (
  SET clean=true
) ELSE IF "%~1" == "package" (
  SET package=true
) ELSE IF "%~1" == "win10" (
  SET store=store
) ELSE (
  SET "addon=%addon% %~1"
)
SHIFT
GOTO parseArgs
:argsParsed

ECHO install=%install% clean=%clean% package=%package% store=%store%
SET addon
