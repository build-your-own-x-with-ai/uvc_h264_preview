CC = gcc
TARGET = uvc_h264_preview
SRC = uvc_h264_preview.c

# 库路径配置
PREFIX = /usr/local
INCLUDES = \
    -I$(PREFIX)/include/libuvc \
    -I$(PREFIX)/include/libusb-1.0

LDFLAGS = -L$(PREFIX)/lib -Wl,-rpath,$(PREFIX)/lib
LIBS = -luvc -lusb-1.0 -lpthread  # 链接线程库

CFLAGS = -g -Wall -Wno-unused-variable

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(TARGET) output.h264

run: $(TARGET)
	sudo ./$(TARGET)  # 启动预览

play:  # 播放保存的文件
	ffplay output.h264 -hide_banner

help:
	@echo "命令说明:"
	@echo "  make      - 编译程序"
	@echo "  make run  - 启动实时预览（按 s 保存，按 q 退出）"
	@echo "  make play - 播放保存的 output.h264"
	@echo "  make clean- 清理生成文件"
