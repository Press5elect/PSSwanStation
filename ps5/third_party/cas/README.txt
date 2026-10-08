FidelityFX Contrast Adaptive Sharpening

ffx_cas.h is AMD's, unmodified, from https://github.com/GPUOpen-Effects/FidelityFX-CAS
at 9fabcc9a2c45f958aff55ddfda337e74ef894b7f, under the MIT licence written at its
top. ps5/shaders/look_cas.frag includes it as GLSL (with ../fsr/ffx_a.h);
ps5/tools/make-shaders.sh compiles that into ps5/src/look_spirv.inc, which
ps5/src/display.cpp sharpens the game's picture with when "Scaling" is CAS.
