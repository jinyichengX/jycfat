@echo off
echo 正在创建test0.txt~test251.txt共251个文本文件......
for /l %%i in (0, 1, 251) do (
echo >> test%%i.txt
)
echo 文件创建完毕