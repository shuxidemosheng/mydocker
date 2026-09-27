# 简单的编译脚本：gcc 编译 src 下所有 .c，输出可执行文件 mydocker
CC      = gcc
CFLAGS  = -Wall -Wextra -g -O2
SRC     = $(wildcard src/*.c)
BIN     = mydocker

$(BIN): $(SRC) src/*.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

.PHONY: clean
clean:
	rm -f $(BIN)
