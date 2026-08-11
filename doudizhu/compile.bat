@echo off
chcp 936 >nul
echo 正在编译斗地主 v2.0...
g++ -std=c++11 -O2 -o doudizhu.exe doudizhu.cpp -lws2_32 -static-libgcc -static-libstdc++
if %errorlevel% == 0 (
    echo 编译成功！运行 doudizhu.exe 即可开始游戏。
) else (
    echo 编译失败！请检查是否安装了 MinGW64 g++ 编译器。
)
pause
