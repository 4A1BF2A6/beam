#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <windows.h>
#include <portaudio.h>

/* ds_beamforming.h 是纯C接口，用 extern "C" 包裹避免C++名称修饰问题 */
extern "C" {
#include "ds_beamforming.h"
}

#define SAMPLE_RATE      48000   /* 设备原生采样率：48kHz */
#define FRAMES_PER_BUF   384     /* 每次回调帧数：384帧@48kHz = 128帧@16kHz，与波束成形帧长一致 */
#define NUM_CHANNELS     7       /* 设备总通道数：ch0-3原始麦克风，ch4级联，ch5回采，ch6算法处理 */
#define MIC_CHANNELS     4       /* 用于波束成形的原始麦克风通道数 */
#define DECIMATE         3       /* 降采样比：48kHz / 16kHz = 3 */

/* 传递给音频回调的用户数据 */
struct CallbackData {
    FILE *fp;            /* 输出PCM文件句柄（处理后） */
    FILE *fp_raw;        /* 输出PCM文件句柄（处理前，ch0原始） */
    int   frames_written;
};

static int audio_callback(
    const void *input, void * /*output*/,
    unsigned long frame_count,
    const PaStreamCallbackTimeInfo * /*time*/,
    PaStreamCallbackFlags /*flags*/,
    void *user_data)
{
    CallbackData       *d  = static_cast<CallbackData *>(user_data);
    const short        *in = static_cast<const short *>(input);

    /* 解交织7通道，同时3:1抽取降采样（48kHz→16kHz），只取前4路原始麦克风 */
    short mic[MIC_CHANNELS][128];
    int out_frame = 0;
    for (int s = 0; s < (int)frame_count; s += DECIMATE) {
        for (int c = 0; c < MIC_CHANNELS; c++)
            mic[c][out_frame] = in[s * NUM_CHANNELS + c];
        out_frame++;
    }

    fwrite(mic[0], sizeof(short), 128, d->fp_raw);

    short out[128];
    ds_beamforming_process(mic, out);

    fwrite(out, sizeof(short), 128, d->fp);
    d->frames_written++;
    return paContinue;
}

int main() {
    /* 初始化 PortAudio 库 */
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        fprintf(stderr, "PortAudio init error: %s\n", Pa_GetErrorText(err));
        return 1;
    }

    /* 列出所有输入设备，方便确认麦克风阵列是否被正确识别 */
    int n = Pa_GetDeviceCount();
    printf("=== Input devices ===\n");
    for (int i = 0; i < n; i++) {
        const PaDeviceInfo *info = Pa_GetDeviceInfo(i);
        if (info->maxInputChannels > 0) {
            /* 尝试将设备名称从 UTF-8 转换为 GBK */
            wchar_t wname[256];
            MultiByteToWideChar(CP_UTF8, 0, info->name, -1, wname, 256);
            char gbkname[256];
            WideCharToMultiByte(CP_ACP, 0, wname, -1, gbkname, 256, NULL, NULL);
            printf("  [%2d] %-40s  ch=%d\n", i, gbkname, info->maxInputChannels);
        }
    }
    printf("\n");

    /* 让用户选择设备 */
    int dev = -1;
    while (dev < 0 || dev >= n) {
        printf("Select device index: ");
        if (scanf("%d", &dev) != 1) {
            fprintf(stderr, "Invalid input\n");
            Pa_Terminate();
            return 1;
        }
        /* 清除输入缓冲区 */
        int c;
        while ((c = getchar()) != '\n' && c != EOF);
        
        if (dev >= 0 && dev < n) {
            const PaDeviceInfo *info = Pa_GetDeviceInfo(dev);
            if (info->maxInputChannels < NUM_CHANNELS) {
                printf("Device only has %d channels, need %d. Try again.\n", 
                       info->maxInputChannels, NUM_CHANNELS);
                dev = -1;
            }
        }
    }
    
    /* 显示选择的设备名称（处理乱码） */
    const PaDeviceInfo *sel_info = Pa_GetDeviceInfo(dev);
    wchar_t wname[256];
    MultiByteToWideChar(CP_UTF8, 0, sel_info->name, -1, wname, 256);
    char gbkname[256];
    WideCharToMultiByte(CP_ACP, 0, wname, -1, gbkname, 256, NULL, NULL);
    printf("Using device [%d]: %s\n\n", dev, gbkname);

    /* 打开输出文件，用于保存波束成形后的原始PCM数据 */
    FILE *fp = fopen("output.pcm", "wb");
    if (!fp) { fprintf(stderr, "Cannot open output.pcm\n"); Pa_Terminate(); return 1; }
    FILE *fp_raw = fopen("input_ch0.pcm", "wb");
    if (!fp_raw) { fprintf(stderr, "Cannot open input_ch0.pcm\n"); fclose(fp); Pa_Terminate(); return 1; }

    /* 初始化波束成形模块（必须在 process 之前调用） */
    ds_beamforming_init();

    CallbackData data = { fp, fp_raw, 0 };

    /* 配置输入流参数 */
    PaStreamParameters params{};
    params.device                    = dev;
    params.channelCount              = NUM_CHANNELS;         /* 7通道输入 */
    params.sampleFormat              = paInt16;              /* 16位有符号整数 */
    params.suggestedLatency          = Pa_GetDeviceInfo(dev)->defaultLowInputLatency; /* 使用设备推荐的低延迟配置 */
    params.hostApiSpecificStreamInfo = nullptr;

    /* 打开音频流：只开输入，不开输出（output参数为nullptr） */
    PaStream *stream;
    err = Pa_OpenStream(&stream, &params, nullptr,
                        SAMPLE_RATE, FRAMES_PER_BUF, 0, audio_callback, &data);
    if (err != paNoError) {
        fprintf(stderr, "Pa_OpenStream error: %s\n", Pa_GetErrorText(err));
        fclose(fp); Pa_Terminate(); return 1;
    }

    /* 启动录音，按 Enter 停止 */
    Pa_StartStream(stream);
    printf("Recording... press Enter to stop.\n");
    getchar();

    /* 自动将PCM转换为WAV格式 */
    char cmd[256];

    /* 转换波束成形后的输出 */
    sprintf(cmd, "ffmpeg -y -f s16le -ar 16000 -ac 1 -i output.pcm output.wav");
    if (system(cmd) == 0) {
        printf("Converted output.pcm -> output.wav\n");
    } else {
        printf("Warning: ffmpeg not found, keeping PCM file\n");
    }

    /* 转换原始第一路麦克风 */
    sprintf(cmd, "ffmpeg -y -f s16le -ar 16000 -ac 1 -i input_ch0.pcm input_ch0.wav");
    if (system(cmd) == 0) {
        printf("Converted input_ch0.pcm -> input_ch0.wav\n");
    }

    return 0;
}
