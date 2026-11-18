@echo off
rem 도와드림 레이더 모니터 실행 (포트가 다르면 아래 COM8을 바꾸세요)
chcp 65001 >nul
start "" http://localhost:8765
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0server.ps1" -Port COM8 -Http 8765
pause
