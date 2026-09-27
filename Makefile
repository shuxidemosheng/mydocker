# 简单的编译脚本：gcc 编译 src 下所有 .c，输出可执行文件 mydocker
CC      = gcc
CFLAGS  = -Wall -Wextra -g -O2
SRC     = $(wildcard src/*.c)
BIN     = mydocker

$(BIN): $(SRC) src/*.h
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

test: test/memhog test/spin

# 测试程序必须 -static：busybox rootfs 里没有 glibc 动态链接器，
# 动态编译的二进制在容器内 execve 会报 ENOENT（"not found" 但文件明明在）
test/memhog: test/memhog.c
	$(CC) $(CFLAGS) -static -o $@ $<

test/spin: test/spin.c
	$(CC) $(CFLAGS) -static -o $@ $<

.PHONY: clean
clean:
	rm -f $(BIN) test/memhog test/spin
