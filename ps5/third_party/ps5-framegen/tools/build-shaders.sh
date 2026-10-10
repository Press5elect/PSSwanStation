#!/usr/bin/env bash
# build-shaders.sh: the frame generation's shaders (AMD FidelityFX SDK 2.3.0's FSR3 optical flow and frame interpolation, cut down to
# the optical flow path, MIT; sources in shaders/) from HLSL to SPIR-V, into src/framegen/fg_shaders.inc (the modules as arrays with
# each pass's descriptor bindings and workgroup size, read back from the SPIR-V by tools/genspvinc.py).
#
# Needs glslang's glslangValidator with its HLSL front end (the Vulkan SDK's has it; a source build needs ENABLE_HLSL=ON), as
# GLSLANG=<path> or on the PATH. DXC is not needed.
#
# The passes and their names in src/ps5_framegen.cpp: the optical flow's luma, its pyramid, the scene change detection's histogram and
# divergence, the search, the filter and the upscale (FSR3's "v5" optical flow); the interpolation's setup, vector field and
# interpolation (fg_fi_*: FSR3's passes cut down to the optical flow path, see shaders/README.md).
#
# Extra -D options after the script's name are passed to every pass (the test defines in ffx_frameinterpolation.h, e.g.
# -DFG_NO_REFINE), and OUT=<file> writes the result elsewhere: how tools/truth.py compares variants.
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
ffx=${FFX:-$root/shaders}
glslang=${GLSLANG:-glslangValidator}
out=${OUT:-$root/src/framegen/fg_shaders.inc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

passes=(
	of_prepare_luma:ffx_opticalflow_prepare_luma_pass
	of_luma_pyramid:ffx_opticalflow_compute_luminance_pyramid_pass
	of_scd_histogram:ffx_opticalflow_generate_scd_histogram_pass
	of_scd_divergence:ffx_opticalflow_compute_scd_divergence_pass
	of_search:ffx_opticalflow_compute_optical_flow_advanced_pass_v5
	of_filter:ffx_opticalflow_filter_optical_flow_pass_v5
	of_scale:ffx_opticalflow_scale_optical_flow_advanced_pass_v5
	fi_setup:fg_fi_setup_pass
	fi_vector_field:fg_fi_of_vector_field_pass
	fi_interpolate:fg_fi_interpolation_pass
)

# FFX_FG_OF_ONLY: the optical flow path alone (no game motion vectors or depth). FFX_FG_PORTABLE_WAVE: the search's reductions in
# shared memory instead of wave operations that assume 32 or 64 lanes. FFX_HLSL_SM=60: glslang has no SM 6.2 types.
# Bindings: t# 0+, u# 100+, b# 200+, s# 300+, structured buffers 400+ (one descriptor set).
args=(-D -V -e CS -S comp --target-env vulkan1.1 -I"$ffx/api/internal/gpu" -I"$ffx/framegeneration/fsr3/include/gpu"
	-DFFX_GPU=1 -DFFX_HLSL=1 -DFFX_FG_OF_ONLY=1 -DFFX_FG_PORTABLE_WAVE=1 -DFFX_HLSL_SM=60 -DFFX_HALF=0
	-DFFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL=0 -DFFX_FRAMEINTERPOLATION_EMBED_ROOTSIG=0 -DFFX_OPTICALFLOW_EMBED_ROOTSIG=0
	-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS=0 -DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS=0
	-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0
	--shift-texture-binding 0 --shift-image-binding 100 --shift-uav-binding 100 --shift-ssbo-binding 400
	--shift-cbuffer-binding 200 --shift-sampler-binding 300 "$@")

specs=()
for pass in "${passes[@]}"; do
	name=${pass%%:*}
	file=${pass#*:}
	"$glslang" "${args[@]}" "$ffx/framegeneration/fsr3/internal/shaders/$file.hlsl" -o "$tmp/$file.spv" > "$tmp/$file.log" 2>&1 || { cat "$tmp/$file.log"; exit 1; }
	specs+=("$name=$tmp/$file.spv")
done

python3 "$root/tools/genspvinc.py" "$out" "${specs[@]}"
