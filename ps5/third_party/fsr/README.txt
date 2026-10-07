FidelityFX Super Resolution 1.0

ffx_a.h and ffx_fsr1.h are AMD's, unmodified, from
https://github.com/GPUOpen-Effects/FidelityFX-FSR at
a21ffb8f6c13233ba336352bdff293894c706575 (v1.0.2), under the MIT licence in
LICENSE.txt. ps5/shaders/fsr_easu.frag and fsr_rcas.frag include them as GLSL;
ps5/tools/make-fsr-shaders.sh compiles those into ps5/src/fsr_spirv.inc, which
ps5/src/display.cpp draws the game's picture with when "Scaling" is FSR 1.
