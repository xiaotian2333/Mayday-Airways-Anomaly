// miniaudio 实现编译单元：解码（内置 dr_mp3/dr_flac/dr_wav）与 WASAPI 播放
// 后端只在本 TU 实例化一次；plugin.cpp 仅 include 头文件拿声明。
// 音乐播放插件专用，miniaudio 许可见 third_party/licenses/miniaudio-LICENSE.txt。
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
