@echo off
set BRA=Unknown
set REV=illegal
set GL1=Dummy

copy nul: /b +%1\comptime.c tmp.$$$ > nul
move tmp.$$$ %1\comptime.c > nul

if exist .git goto gitrev
if exist ..\.git goto gitrev
if exist .svn goto svnrev
goto filwri

:gitrev
set GIT=%2
if "%GIT%"=="" set GIT=git
for /f "tokens=* usebackq" %%s in (`%GIT% rev-parse --abbrev-ref HEAD`) do @set BRA=%%s
for /f "tokens=* usebackq" %%s in (`%GIT% rev-parse HEAD`) do @set REV=%%s
for /f "tokens=* usebackq" %%s in (`%GIT% log -1 --format^=%%s`) do @set GL1=%%s
set REV=%REV:~0,8%
goto filwri

:svnrev
set BRA=Subversion
for /f "usebackq" %%s in (`svnversion .`) do @set REV=%%s
set REV=r%REV%
goto filwri

:filwri
echo // Do not edit!  This file was autogenerated > %1\comptime.h
echo // by the %0 batch file >> %1\comptime.h
echo // >> %1\comptime.h
echo const char* compbranch = "%BRA%"; >> %1\comptime.h
echo const char* comprevision = "%REV%"; >> %1\comptime.h
echo const char* compnote = "%GL1%"; >> %1\comptime.h
