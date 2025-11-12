#include "libuvc/libuvc.h"
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <termios.h>
#include <fcntl.h>

// 全局控制变量
static int running = 1;          // 程序运行标志
static int save_to_file = 0;     // 是否保存到文件（0=不保存，1=保存）
static FILE *fp_h264 = NULL;     // 保存文件的指针
static FILE *pipe_ffplay = NULL; // 用于 ffplay 预览的管道

// 信号处理：捕获 Ctrl+C 退出
void sigint_handler(int sig) {
    running = 0;
    printf("\n准备停止预览...\n");
}

// 线程：监听键盘输入（按 s 切换保存状态，按 q 退出）
void *keyboard_listener(void *arg) {
    struct termios old_tio, new_tio;
    char c;

    // 设置终端为非阻塞模式（无需按回车即可读取按键）
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= (~ICANON & ~ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    while (running) {
        if (read(STDIN_FILENO, &c, 1) == 1) {
            switch (c) {
                case 's':
                case 'S':
                    save_to_file = !save_to_file;
                    if (save_to_file) {
                        fp_h264 = fopen("output.h264", "wb");
                        if (fp_h264) {
                            printf("\n开始保存 H.264 到 output.h264\n");
                        } else {
                            perror("无法打开 output.h264");
                            save_to_file = 0;
                        }
                    } else {
                        if (fp_h264) {
                            fclose(fp_h264);
                            fp_h264 = NULL;
                            printf("\n已停止保存，文件：output.h264\n");
                        }
                    }
                    break;
                case 'q':
                case 'Q':
                    running = 0;
                    printf("\n用户请求退出...\n");
                    break;
            }
        }
        usleep(100000); // 降低 CPU 占用
    }

    // 恢复终端设置
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
    return NULL;
}

// 帧回调函数：处理视频帧，同时用于预览和保存
void cb(uvc_frame_t *frame, void *ptr) {
    if (!running) return;

    // 仅处理 H.264 帧
    if (frame->frame_format == UVC_FRAME_FORMAT_H264) {
        // 1. 发送数据到 ffplay 预览（通过管道）
        if (pipe_ffplay) {
            fwrite(frame->data, 1, frame->data_bytes, pipe_ffplay);
            fflush(pipe_ffplay); // 实时刷新，降低延迟
        }

        // 2. 若启用保存，则写入文件
        if (save_to_file && fp_h264) {
            fwrite(frame->data, 1, frame->data_bytes, fp_h264);
            fflush(fp_h264);
        }

        // 打印帧信息（每 30 帧一次）
        static int count = 0;
        if (++count % 30 == 0) {
            printf("已预览 %d 帧 | 分辨率：%dx%d | 按 s 保存，按 q 退出\n",
                   count, frame->width, frame->height);
        }
    }
}

// 初始化 ffplay 管道
int init_ffplay_pipe() {
    // 创建管道并启动 ffplay（指定 H.264 格式）
    pipe_ffplay = popen("ffplay -f h264 -i - -hide_banner -window_title 'UVC H.264 预览'", "w");
    if (!pipe_ffplay) {
        perror("无法启动 ffplay");
        return -1;
    }
    printf("ffplay 预览窗口已启动\n");
    return 0;
}

int main(int argc, char **argv) {
    uvc_context_t *ctx = NULL;
    uvc_device_t *dev = NULL;
    uvc_device_handle_t *devh = NULL;
    uvc_stream_ctrl_t ctrl;
    uvc_error_t res;
    pthread_t key_thread;

    // 注册信号处理
    signal(SIGINT, sigint_handler);

    // 初始化 UVC
    res = uvc_init(&ctx, NULL);
    if (res < 0) {
        uvc_perror(res, "uvc_init 失败");
        return res;
    }
    printf("UVC 初始化成功\n");

    // 查找 UVC 设备
    res = uvc_find_device(ctx, &dev, 0, 0, NULL);
    if (res < 0) {
        uvc_perror(res, "未找到 UVC 设备");
        uvc_exit(ctx);
        return res;
    }
    printf("找到 UVC 设备\n");

    // 打开设备
    res = uvc_open(dev, &devh);
    if (res < 0) {
        uvc_perror(res, "无法打开设备");
        uvc_unref_device(dev);
        uvc_exit(ctx);
        return res;
    }
    printf("设备打开成功\n");

    // 打印设备信息
    uvc_print_diag(devh, stderr);

    // 获取设备支持的格式
    const uvc_format_desc_t *format_desc = uvc_get_format_descs(devh);
    const uvc_frame_desc_t *frame_desc = format_desc->frame_descs;
    enum uvc_frame_format frame_format;
    int width = 640, height = 480, fps = 30;

    // 自动识别格式
    switch (format_desc->bDescriptorSubtype) {
        case UVC_VS_FORMAT_FRAME_BASED:
            frame_format = UVC_FRAME_FORMAT_H264;
            printf("设备支持 H.264 格式\n");
            break;
        case UVC_VS_FORMAT_MJPEG:
            frame_format = UVC_COLOR_FORMAT_MJPEG;
            printf("设备支持 MJPEG 格式（预览可能不兼容）\n");
            return -1; // 本程序仅支持 H.264
        default:
            frame_format = UVC_COLOR_FORMAT_YUYV;
            printf("设备不支持 H.264（仅支持 YUYV）\n");
            return -1;
    }

    // 获取分辨率和帧率
    if (frame_desc) {
        width = frame_desc->wWidth;
        height = frame_desc->wHeight;
        fps = 10000000 / frame_desc->dwDefaultFrameInterval;
    }
    printf("使用参数：%dx%d@%dfps\n", width, height, fps);

    // 配置流参数
    res = uvc_get_stream_ctrl_format_size(devh, &ctrl, frame_format, width, height, fps);
    uvc_print_stream_ctrl(&ctrl, stderr);
    if (res < 0) {
        uvc_perror(res, "流参数配置失败");
        uvc_close(devh);
        uvc_unref_device(dev);
        uvc_exit(ctx);
        return res;
    }

    // 初始化 ffplay 预览管道
    if (init_ffplay_pipe() != 0) {
        uvc_close(devh);
        uvc_unref_device(dev);
        uvc_exit(ctx);
        return -1;
    }

    // 启动键盘监听线程
    pthread_create(&key_thread, NULL, keyboard_listener, NULL);
    printf("操作提示：按 s 开始/停止保存，按 q 退出\n");

    // 启动流传输
    res = uvc_start_streaming(devh, &ctrl, cb, NULL, 0);
    if (res < 0) {
        uvc_perror(res, "启动流传输失败");
        pclose(pipe_ffplay);
        uvc_close(devh);
        uvc_unref_device(dev);
        uvc_exit(ctx);
        return res;
    }
    printf("开始预览...\n");

    // 等待退出信号
    while (running) {
        sleep(1);
    }

    // 清理资源
    uvc_stop_streaming(devh);
    printf("流传输已停止\n");

    // 关闭文件和管道
    if (fp_h264) {
        fclose(fp_h264);
        printf("保存文件已关闭：output.h264\n");
    }
    if (pipe_ffplay) {
        pclose(pipe_ffplay);
        printf("ffplay 已关闭\n");
    }

    // 等待键盘线程结束
    pthread_join(key_thread, NULL);

    // 释放设备资源
    uvc_close(devh);
    uvc_unref_device(dev);
    uvc_exit(ctx);
    printf("程序已退出\n");

    return 0;
}
