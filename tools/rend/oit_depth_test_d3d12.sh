#!/bin/sh
# The Direct3D 12 per-pixel renderer's depth test of a translucent fragment
# under SSAA, on a device (tools/rend/oit_depth_test_d3d12.c), as
# tools/rend/oit_depth_test.sh has Vulkan's: a model of small triangles that
# are not coplanar drawn into the depth, then drawn again the way the
# renderer puts its translucent polygons in its lists, once a pixel, with
# the shipped text (rend/dx12/shaders/dx12_raster.hlsl, between its
# "---- the lists' depth test under SSAA" line and the next "// ----"):
# each sample tested by hand with the plane's depth there, from the
# centre's and its differences to the next pixels (Direct3D has no
# post-depth coverage), a depth within OIT_DEPTH_NEAR of the opaque one
# counting as equal; and the fragment kept with its depth at the centroid
# of the samples it is seen at, which has to be within those samples'
# depths. Every sample is covered by the same triangle in both passes, so
# a test done right sees all of them. Each count of samples is run twice:
# as shipped, and with nothing taken as equal (what the tolerance is for).
#
#   sh tools/rend/oit_depth_test_d3d12.sh     PASS when, as shipped, nothing is missed or kept outside
#
# Built with mingw-w64 (CC_WIN, default x86_64-w64-mingw32-gcc), run under
# wine (WINE, "" on Windows) with vkd3d-proton's d3d12.dll and
# d3d12core.dll from $D3D12_DLLS where that is given, and
# D3DCOMPILER=/path/to/d3dcompiler_47.dll (needed: the shaders are made
# when it runs). Made in $WORK (default: a new directory in $TMPDIR).
set -e
cd "$(dirname "$0")/../.."
if [ -z "$WORK" ]; then
   WORK=${TMPDIR:-/tmp}/oit_depth_test_d3d12.$$
   trap 'rm -rf "$WORK"' EXIT
fi
mkdir -p "$WORK"
[ -n "$D3DCOMPILER" ] && [ -f "$D3DCOMPILER" ] || { echo "oit_depth_test_d3d12: D3DCOMPILER=/path/to/d3dcompiler_47.dll is needed" >&2; exit 2; }
cp "$D3DCOMPILER" "$WORK/d3dcompiler_47.dll"
if [ -n "$D3D12_DLLS" ]; then
   cp "$D3D12_DLLS/d3d12.dll" "$D3D12_DLLS/d3d12core.dll" "$WORK/"
   export WINEDLLOVERRIDES="d3d12,d3d12core=n"
fi
${CC_WIN:-x86_64-w64-mingw32-gcc} -std=c99 -O1 -Wall -o "$WORK/oit_depth_test_d3d12.exe" tools/rend/oit_depth_test_d3d12.c -ldxguid

SRC=core/rend/dx12/shaders/dx12_raster.hlsl
TEXT=$(sed -n "/^\/\/ ---- the lists' depth test under SSAA/,/^\/\/ ----\$/p" $SRC)
[ -n "$TEXT" ] || { echo "oit_depth_test_d3d12: the test's text is not in $SRC" >&2; exit 2; }
# @1 samples, @2 "exact": nothing taken as equal
shader() {
   echo "#define ssaaLog2 $(( $1 == 2 ? 1 : $1 == 4 ? 2 : 3 ))u"
   echo "#define ssaaMask $(( (1 << $1) - 1 ))u"
   echo "Texture2DMS<float> DepthTex : register(t0);"
   echo "RWByteAddressBuffer counts : register(u1);"
   if [ "$2" = exact ]; then echo "$TEXT" | sed 's/^#define OIT_DEPTH_NEAR .*/#define OIT_DEPTH_NEAR 1.0/'; else echo "$TEXT"; fi
   cat <<'HLSL'
// (a and b more than @n units in the last place apart)
bool ulpApart(float a, float b, int n) { return abs(int(asuint(a)) - int(asuint(b))) > n; }
float4 ps_main(float4 pos : SV_Position, uint mask : SV_Coverage) : SV_Target
{
	const float2 dz = float2(ddx_fine(pos.z), ddy_fine(pos.z));
	float2 osum;
	float on;
	const uint seen = oitSampleDepthTest(mask, int2(pos.xy), pos.z, dz, osum, on);
	const float kept = oitKeptDepth(seen, pos.z, dz, osum, on);
	const bool edge = mask != ssaaMask;
	uint old;
	counts.InterlockedAdd(0, 1u, old);
	if (edge)
		counts.InterlockedAdd(4, 1u, old);
	if (any(frac(pos.xy) != float2(0.5, 0.5)))
		counts.InterlockedAdd(48, 1u, old);
	counts.InterlockedAdd(8, countbits(mask), old);
	counts.InterlockedAdd(12, countbits(mask & ~seen), old);
	uint exact = 0u, worst = 0u, centreMissed = 0u;
	float lo = 3.0, hi = -1.0;
	for (uint s = 0u; s < (1u << ssaaLog2); s++)
	{
		if ((mask & (1u << s)) == 0u)
			continue;
		const float d = DepthTex.Load(int2(pos.xy), s);
		const float2 o = GetRenderTargetSamplePosition(s);
		const float p = pos.z + dz.x * o.x + dz.y * o.y;
		if (p == d)
			exact++;
		else
			worst = max(worst, uint(abs(int(asuint(p)) - int(asuint(d)))));
		if (pos.z < d)
			centreMissed++;
		if ((seen & (1u << s)) != 0u)
		{
			lo = min(lo, d);
			hi = max(hi, d);
		}
	}
	counts.InterlockedAdd(16, exact, old);
	counts.InterlockedMax(20, worst, old);
	counts.InterlockedAdd(28, centreMissed, old);
	if (seen != 0u && ((kept < lo && ulpApart(kept, lo, 8)) || (kept > hi && ulpApart(kept, hi, 8))))
		counts.InterlockedAdd(32, 1u, old);
	return float4(0.0, 0.0, 0.0, 0.0);
}
HLSL
}
winpath() {
   if [ -n "$WINE" ]; then echo "Z:$1"; else echo "$1"; fi
}
WINE=${WINE-wine}
# (wine's Vulkan, which vkd3d-proton draws with, wants a display: a virtual one where there is none)
XVFB=
[ -n "$WINE" ] && [ -z "$DISPLAY" ] && command -v xvfb-run > /dev/null && XVFB="xvfb-run -a"

rc=0
for n in 2 4 8; do
   for v in shipped exact; do
      shader $n $v > "$WORK/ps_${n}_$v.hlsl"
      case $v in
      shipped) echo "-- $n samples: the shipped test" ;;
      exact) echo "-- $n samples: nothing taken as equal (OIT_DEPTH_NEAR 1)" ;;
      esac
      set +e
      ( cd "$WORK" && $XVFB env -u TMPDIR WINEDEBUG=-all $WINE ./oit_depth_test_d3d12.exe $n "$(winpath "$WORK/ps_${n}_$v.hlsl")" ) 2>&1 \
         | grep -v '^X connection' | tee "$WORK/out"
      set -e
      if grep -q "has no $n samples" "$WORK/out"; then break; fi
      # (the program's lines may end in CR LF)
      if [ $v = shipped ] && ! grep -q "^oit_depth_test_d3d12: PASS" "$WORK/out"; then rc=1; fi
   done
done
if [ $rc = 0 ]; then echo "oit_depth_test_d3d12: PASS"; else echo "oit_depth_test_d3d12: FAIL"; fi
exit $rc
