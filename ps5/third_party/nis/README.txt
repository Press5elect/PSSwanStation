NVIDIA Image Scaling

NIS_Config.h, NIS_Scaler.h and NIS_Main.glsl are NVIDIA's, unmodified, from
https://github.com/NVIDIAGameWorks/NVIDIAImageScaling at
35e13ba316c98eeecf16f37eae70ce88019911f6 (v1.0.3), under the MIT licence in
LICENSE.txt. ps5/shaders/look_nis.comp is NIS_Main.glsl as this title runs it
(the scaler, in 32-bit floats, writing RGBA8); ps5/tools/make-shaders.sh
compiles it with NIS_Scaler.h into ps5/src/look_spirv.inc, and
ps5/src/display.cpp runs it, with NIS_Config.h's constants and filter tables,
when "Scaling" is NIS.
