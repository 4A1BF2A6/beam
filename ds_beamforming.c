#include <stdio.h>
#include <math.h>
#include <string.h>

#include "./speex/fft/fft_wrap.h"

/* ============================================================
 * 常量定义
 * ============================================================ */
#define N1_LEN 64           /* 半帧长度，每次对外输出的采样点数 */
#define N2_LEN 128          /* 全帧长度，每次输入的采样点数 */
#define N3_LEN 192          /* 3/4缓冲区长度，用于滑动窗平移计算（N1_LEN + N2_LEN） */
#define N4_LEN 256          /* FFT点数（N2_LEN * 2，补零到此长度做线性相关） */

#define N_CHANNEL 4         /* 麦克风通道数 */
#define REFER_CHANNEL 0     /* 参考通道索引，其他通道的时延均相对该通道计算 */
#define MARGIN_FRAMES 4     /* 时延搜索范围：在 ±MARGIN_FRAMES 个采样点内搜索互相关峰值 */

/* ============================================================
 * 全局变量
 * ============================================================ */

/* FFT句柄，由speex FFT库提供 */
void * spx_fft_handle;

/* 多通道环形缓冲区，每个通道缓存256个采样点（浮点，已归一化到[-1,1]） */
float beam_buffer[N_CHANNEL][N4_LEN];

/* 首帧标志：0=尚未收到第一帧数据，1=已初始化 */
int start = 0;

/* 汉明窗系数，长度128，在初始化时预计算 */
float hamm_val[N2_LEN];

/* 各通道FFT结果缓冲区（speex FFT输出格式，实虚部交织存储） */
float channels_fft_spx[N_CHANNEL][N4_LEN];

/* 各通道加窗后的时域数据（后半补零，共256点，用于FFT输入） */
float channels_in[N_CHANNEL][N4_LEN];

/* 当前使用的参考通道索引 */
int refChannel = REFER_CHANNEL;

/* 各通道相对参考通道的最优时延（单位：采样点数，可为负值表示超前） */
int best_delay_index[N_CHANNEL];

/* 各通道互相关峰值（用于评估时延估计的置信度） */
float best_delay_value[N_CHANNEL];

/* OLA（重叠相加）输出累积缓冲区，长度256，浮点 */
double outputf[N4_LEN];

/* ============================================================
 * 初始化
 * ============================================================ */

/**
 * 初始化波束成形模块。
 * 创建FFT句柄，预计算汉明窗系数。
 */
void ds_beamforming_init(){
	int i;
    /* 初始化256点FFT，speex库内部会分配所需资源 */
    spx_fft_handle = spx_fft_init(N4_LEN);

    /* 预计算128点汉明窗：w(n) = 0.54 - 0.46*cos(2π*n/(N-1)) */
    for(i=0; i<N2_LEN; i++){
        hamm_val[i] = 0.54 - 0.46*cos(6.283185307*i/(N2_LEN-1));
    }
}

/* ============================================================
 * TDOA 时延估计（GCC-PHAT）
 * ============================================================ */

/**
 * 基于GCC-PHAT（广义互相关-相位变换）估计各通道相对参考通道的时延。
 *
 * 流程：
 *   1. 对每个通道取beam_buffer中间128点，加汉明窗，后半补零到256点
 *   2. 做256点FFT
 *   3. 对每个非参考通道，与参考通道做复数共轭乘积，除以模值（PHAT加权），得到归一化互功率谱
 *   4. IFFT得到互相关序列
 *   5. 在 ±MARGIN_FRAMES 范围内搜索互相关峰值，对应位置即为时延估计值
 */
void tdoa_process(){
	int i;
	int channel_count;
	int chan_count;
    float result[N4_LEN];       /* IFFT输出的互相关时域序列 */
    float xcorr_value[N4_LEN+1];/* 重排后的互相关序列（正负时延居中排列） */
    float tmpData[N4_LEN];      /* 归一化互功率谱（FFT域中间结果） */
    float abs_value;            /* 当前频点的模值，用于PHAT归一化 */

    /* --- 步骤1&2：对每个通道加窗+补零+FFT --- */
    for(chan_count=0; chan_count<N_CHANNEL; chan_count++){
        /* 取beam_buffer中索引[N1_LEN, N1_LEN+N2_LEN)的128个点，乘以汉明窗 */
        for(i=0; i < N2_LEN; i++){
            channels_in[chan_count][i] = (float)(beam_buffer[chan_count][i+N1_LEN]) * hamm_val[i];
        }

        /* 后半段补零（线性卷积需要把长度扩展到2N，避免循环混叠） */
        for(i = N2_LEN; i < N4_LEN; i++){
            channels_in[chan_count][i] = 0.00;
        }

        /* 256点FFT，结果存入channels_fft_spx[chan_count]
         * speex FFT输出格式（实虚部交织）：
         *   [0]        = 直流分量（纯实数）
         *   [1],[2]    = 第1个复数频点的实部、虚部
         *   ...
         *   [2k-1],[2k]= 第k个复数频点的实部、虚部
         *   [N4_LEN-1] = 奈奎斯特频点（纯实数，存在最后一个位置）
         */
        spx_fft(spx_fft_handle,channels_in[chan_count],channels_fft_spx[chan_count]);
    }

    /* --- 步骤3&4&5：逐通道计算GCC-PHAT，估计时延 --- */
    for (channel_count = 0; channel_count < N_CHANNEL; channel_count++){
        if (channel_count != REFER_CHANNEL){
            /* 遍历所有频点（0 到 N2_LEN），计算归一化互功率谱 */
            for (i = 0; i <= N2_LEN; i++){
                /* 复数乘法：X_chan * conj(X_ref) = (a+jb)(c-jd) = (ac+bd) + j(bc-ad)
                 * speex存储格式下，三种频点需分别处理： */

                if (i == 0){
                    /* 直流分量：纯实数，存在index 0 */
                    tmpData[i] = channels_fft_spx[channel_count][i] * channels_fft_spx[refChannel][i];
                    abs_value = sqrt(tmpData[i] * tmpData[i]);
                    if (abs_value == 0){
                        abs_value = 1; /* 防止除以零 */
                    }
                    /* PHAT归一化，同时补偿speex FFT的2N缩放因子 */
                    tmpData[i] /= (abs_value * 2 * N2_LEN);
                }
                else if(i == N2_LEN){
                    /* 奈奎斯特频点（N/2）：纯实数，speex存在index [N4_LEN-1] = [2*N2_LEN-1] */
                    tmpData[i*2-1] = channels_fft_spx[channel_count][i*2-1] * channels_fft_spx[refChannel][i*2-1];
                    abs_value = sqrt(tmpData[i*2-1] * tmpData[i*2-1]);
                    if (abs_value == 0){
                        abs_value = 1;
                    }
                    tmpData[i*2-1] /= (abs_value * 2 * N2_LEN);
                }
                else{
                    /* 普通复数频点：实部存在[2i-1]，虚部存在[2i]
                     * 互相关实部：ac + bd
                     * 互相关虚部：bc - ad（注意：这里是 X_chan * conj(X_ref)，所以虚部符号为负的ad） */
                    tmpData[i*2-1] = channels_fft_spx[channel_count][i*2-1] * channels_fft_spx[refChannel][i*2-1]
                                   + channels_fft_spx[channel_count][i*2]   * channels_fft_spx[refChannel][i*2];
                    tmpData[i*2]   = channels_fft_spx[channel_count][i*2]   * channels_fft_spx[refChannel][i*2-1]
                                   - channels_fft_spx[channel_count][i*2-1] * channels_fft_spx[refChannel][i*2];
                    abs_value = sqrt(tmpData[i*2-1] * tmpData[i*2-1] + tmpData[i*2] * tmpData[i*2]);
                    if (abs_value == 0){
                        abs_value = 1;
                    }
                    /* PHAT归一化：每个频点除以自身模值，使所有频点权重相等 */
                    tmpData[i*2-1] /= (abs_value * 2 * N2_LEN);
                    tmpData[i*2]   /= (abs_value * 2 * N2_LEN);
                }
            }

            /* IFFT：将归一化互功率谱变换回时域，得到互相关序列 */
            spx_ifft(spx_fft_handle, tmpData, result);

            /* 重排互相关序列，使时延=0居中：
             *   正时延部分（0 ~ MARGIN_FRAMES-1）：来自result的头部，放到xcorr_value的后半
             *   负时延部分（对应result的尾部）：放到xcorr_value的前半
             * 最终xcorr_value布局：[负时延...][时延=0][正时延...]
             */
            /* 正时延：result[0..MARGIN_FRAMES-1] → xcorr_value[MARGIN_FRAMES+1..2*MARGIN_FRAMES] */
            for (i = 0; i < MARGIN_FRAMES; i++){
                xcorr_value[i + (MARGIN_FRAMES + 1)] = (float)(result[i]);
            }
            /* 负时延：result[2*N2_LEN-MARGIN_FRAMES-1..2*N2_LEN-1] → xcorr_value[0..MARGIN_FRAMES-1] */
            for (i = 2 * N2_LEN - (MARGIN_FRAMES + 1); i < 2 * N2_LEN; i++){
                xcorr_value[i - (2 * N2_LEN - (MARGIN_FRAMES + 1))] = (float)(result[i]);
            }

            /* 在xcorr_value[0..2*MARGIN_FRAMES]范围内搜索峰值（含两端，覆盖 ±MARGIN_FRAMES）。
             * 原实现循环上界 < 2*MARGIN_FRAMES 漏掉 +MARGIN_FRAMES-1、+MARGIN_FRAMES 两格；
             * max_value 初值改为足够负，避免负相关峰被错过。 */
            float max_value = -1e30f;
            int max_index = MARGIN_FRAMES + 1;  /* 缺省指向 lag=0 */
            for(i = 0; i <= 2*MARGIN_FRAMES; i++){
                if(xcorr_value[i] > max_value){
                    max_value = xcorr_value[i];
                    max_index = i;
                }
            }
            /* max_index 相对于中心点（MARGIN_FRAMES+1）的偏移即为时延（有符号） */
            best_delay_index[channel_count] = max_index - (MARGIN_FRAMES + 1);
            best_delay_value[channel_count] = max_value;
        }
        else{
            /* 参考通道自身时延为0，置信度设为1 */
            best_delay_index[channel_count] = 0;
            best_delay_value[channel_count] = 1.0f;
        }
    }
}

/* ============================================================
 * 延迟求和输出
 * ============================================================ */

/**
 * 根据TDOA估计结果，对各通道信号做延迟对齐后叠加，输出增强语音。
 *
 * 使用OLA（重叠相加）方式避免拼接处的不连续：
 *   - 梯形窗前半段增益从0线性升到0.25（N1_LEN点内上升）
 *   - 梯形窗后半段增益从0.25线性降到0
 *   - outputf[0..N1_LEN-1] 是上一次处理遗留的后半段，直接输出
 *   - outputf[N1_LEN..N2_LEN-1] 是本次处理的新数据积累区
 *
 * @param output  输出缓冲区，写入N1_LEN个16位PCM采样点
 */
void delay_sum_process(short *output){
	int i;
	int channel_count;

    /* 将outputf后半段（上一帧新积累的数据）移到前半段准备输出，
     * 同时清空后半段以接收本次叠加结果 */
    for(i=0; i < N1_LEN; i++) {
        outputf[i] = outputf[i+N1_LEN];  /* 前移：后半→前半 */
        outputf[i+N1_LEN] = 0;           /* 清空后半，准备本帧叠加 */
    }

    /* 逐通道叠加：每个通道按best_delay_index偏移后，用梯形窗加权累加 */
    for(channel_count=0; channel_count< N_CHANNEL; channel_count++){
        /* 梯形窗增益步长：4个通道最终叠加增益总和为1（每通道0.25），
         * N1_LEN点内从0线性升至0.25，再N1_LEN点内从0.25线性降至0 */
        float gain_step = 1.0 * 0.25/(N1_LEN);
        float gain = 0;

        for(i=0; i< N2_LEN; i++){
            /* 从beam_buffer中取当前通道、经时延补偿后的样本进行叠加
             * 偏移量 = N1_LEN（缓冲区中数据起始位置）+ best_delay_index（时延补偿） */
            outputf[i] += beam_buffer[channel_count][i+N1_LEN+best_delay_index[channel_count]] * gain;

            /* 更新梯形窗增益 */
            if(i < N1_LEN){
                gain += gain_step;  /* 前半段：增益上升 */
            }
            else if( i >= N1_LEN) {
                gain -= gain_step;  /* 后半段：增益下降 */
            }
        }
    }

    /* 将outputf前N1_LEN个浮点样本转换为16位PCM输出
     * 乘以32768将[-1,1]浮点范围映射回16位整数范围 */
    for(i = 0; i < N1_LEN; i++){
        output[i] = outputf[i] * 32768.0f;
    }
}

/* ============================================================
 * 对外主处理函数
 * ============================================================ */

int count = 0; /* 调试用帧计数器（当前未启用） */

/**
 * 波束成形主处理函数，每次调用处理128个采样点的4路输入，输出128个采样点。
 *
 * 内部采用滑动窗方式管理beam_buffer（256点循环缓冲）：
 *   - 每次输入128点，分两个64点半帧分别做TDOA估计和延迟求和
 *   - 这样可以用较小的延迟（64点）实现近似实时的处理
 *
 * @param input_channels  4路麦克风输入，每路128个采样点（16位PCM）
 * @param out             输出缓冲区，128个采样点（16位PCM）
 */
void ds_beamforming_process(short input_channels[][128], short *out){
	int i, j;

    if(start == 0){
        /* ---- 首帧：初始化缓冲区，不做处理 ---- */
        /* beam_buffer前半段（index 0..127）填0，后半段（128..255）存入当前帧数据 */
        for(i = 0; i < N_CHANNEL; i++){
            for(j = 0; j < N2_LEN; j++){
                beam_buffer[i][j]         = 0.0f;
                beam_buffer[i][j+N2_LEN]  = input_channels[i][j] / 32768.0f; /* 归一化到[-1,1] */
            }
        }
        start = 1;
        /* 首帧输出静音（全零） */
        memset(out, 0x00, sizeof(short)*N2_LEN);
    }
    else{
        /* ---- 后续帧：滑动窗处理，分两个半帧输出 ---- */

        /* === 第一半帧：压入输入数据的前64点 === */
        for(i = 0; i < N_CHANNEL; i++){
            for(j = 0; j < N4_LEN; j++){
                if(j < N3_LEN){
                    /* 将beam_buffer整体向前平移N1_LEN（64）个位置，腾出尾部空间 */
                    beam_buffer[i][j] = beam_buffer[i][j+N1_LEN];
                }
                else{
                    /* 将input_channels的前64个新样本写入beam_buffer尾部（index 192..255）
                     * j-N3_LEN 范围为 0..63，对应 input_channels 的前半帧 */
                    beam_buffer[i][j] = input_channels[i][j-N3_LEN] / 32768.0f;
                }
            }
        }
        /* 基于当前beam_buffer估计时延，并输出前64点到out[0..63] */
        tdoa_process();
        delay_sum_process(out);

        /* === 第二半帧：压入输入数据的后64点 === */
        for(i = 0; i < N_CHANNEL; i++){
            for(j = 0; j < N4_LEN; j++){
                if(j < N3_LEN){
                    /* 再次向前平移N1_LEN（64）个位置 */
                    beam_buffer[i][j] = beam_buffer[i][j+N1_LEN];
                }
                else{
                    /* 将input_channels的后64个新样本写入beam_buffer尾部（index 192..255）
                     * j-N2_LEN 范围为 0..63，但此时j从192开始，j-N2_LEN=64..127，
                     * 对应 input_channels 的后半帧 */
                    beam_buffer[i][j] = input_channels[i][j-N2_LEN] / 32768.0f;
                }
            }
        }
        /* 基于更新后的beam_buffer估计时延，并输出后64点到out[64..127] */
        tdoa_process();
        delay_sum_process(out+N1_LEN);
    }
}
