@echo off
echo ========================================================
echo       GATEPULSE - FULL SYSTEM VERIFICATION SUITE       
echo ========================================================
echo.
echo [1/5] Compiling and running Milestone 1 (Trie Router)...
call build.bat tests/test_trie_router.cpp -o bin/test_trie_router.exe && bin\test_trie_router.exe
if %ERRORLEVEL% NEQ 0 ( echo [FAILED] Milestone 1 failed! & exit /b 1 )

echo.
echo [2/5] Compiling and running Milestone 2 (Rate Limiter)...
call build.bat tests/test_rate_limiter.cpp -o bin/test_rate_limiter.exe && bin\test_rate_limiter.exe
if %ERRORLEVEL% NEQ 0 ( echo [FAILED] Milestone 2 failed! & exit /b 1 )

echo.
echo [3/5] Compiling and running Milestone 3 (Load Balancer)...
call build.bat tests/test_load_balancer.cpp -o bin/test_load_balancer.exe && bin\test_load_balancer.exe
if %ERRORLEVEL% NEQ 0 ( echo [FAILED] Milestone 3 failed! & exit /b 1 )


echo.
echo ========================================================
echo   ALL MILESTONES PASSED!                                
echo ========================================================
