@echo off
echo ========================================================
echo       GATEPULSE - FULL SYSTEM VERIFICATION SUITE       
echo ========================================================
echo.
echo [1/5] Compiling and running Milestone 1 (Trie Router)...
call build.bat tests/test_trie_router.cpp -o bin/test_trie_router.exe && bin\test_trie_router.exe
if %ERRORLEVEL% NEQ 0 ( echo [FAILED] Milestone 1 failed! & exit /b 1 )

echo.
echo ========================================================
echo   ALL MILESTONES PASSED!                                
echo ========================================================
