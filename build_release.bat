@echo off
call "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cmake --build build-verify --target strata-poly 2>&1
