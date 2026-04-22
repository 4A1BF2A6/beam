/**
 * @file ds_beamforming.h
 * @brief 延迟求和（Delay-and-Sum）波束成形模块对外接口
 *
 * 本模块对4路麦克风输入信号进行波束成形处理，
 * 通过GCC-PHAT算法估计各通道时延，再做延迟对齐叠加，
 * 输出增强后的单路语音信号。
 */

#ifndef __DS_BEAMFORMING__
#define __DS_BEAMFORMING__

/**
 * @brief 初始化波束成形模块
 *
 * 在调用 ds_beamforming_process() 之前必须先调用本函数一次。
 * 内部完成FFT句柄创建和汉明窗系数预计算。
 */
void ds_beamforming_init();

/**
 * @brief 波束成形逐帧处理
 *
 * @param input_channels  4路麦克风输入，每路128个采样点（16位有符号PCM）
 * @param out             输出缓冲区，存放增强后的单路语音，128个采样点（16位有符号PCM）
 *
 * 注意：首帧调用时仅做缓冲区初始化，输出全零静音帧；从第二帧起正常输出。
 */
void ds_beamforming_process(short input_channels[][128], short *out);

#endif
