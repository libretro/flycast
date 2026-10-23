/* tools/rend/oit_depth_test.c's test with Direct3D 12: the per-pixel
 * renderer's depth test of a translucent fragment under SSAA
 * (rend/dx12/shaders/dx12_raster.hlsl, oitSampleDepthTest() and
 * oitKeptDepth(): Direct3D has no post-depth coverage, so each sample is
 * tested by hand with the plane's depth there), on a device. See
 * tools/rend/oit_depth_test_d3d12.sh, which makes the shaders' text and
 * runs it. Built with mingw-w64 and run under wine with vkd3d-proton, or
 * on Windows.
 *
 *   oit_depth_test_d3d12.exe N SHADER.hlsl
 *        a model of small triangles that are not coplanar (the game's
 *        vertices, z = 1/w, placed as transformVertex() places them: the
 *        depth cleared to 0, larger nearer, greater or equal)
 *        drawn into N samples a pixel (D32 float, S8, as the renderer has
 *        it), writing the depth, then the same triangles again once a pixel with
 *        SHADER.hlsl's ps_main (ps_5_1, made here with d3dcompiler_47.dll,
 *        which has to be beside the program), the depth read as the
 *        renderer reads it (a Texture2DMS), which counts into a buffer
 *        what is printed. Each sample is covered by the same triangle in
 *        both passes, so a depth test done right sees every one of them.
 *        Exit code 0 when nothing is missed or kept outside its samples'
 *        depths, 1 when something is, 3 when the device has no N samples
 *        in the standard pattern, 2 on an error. */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 320
#define H 240
/* the model's grid, in the game's 640x480 (oit_depth_test.c's) */
#define GX 53
#define GY 41
#define C_WORDS 16

typedef HRESULT (WINAPI *create_device_fn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *serialize_fn)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("oit_depth_test_d3d12: %s: %08lx\n", #x, (unsigned long)hr_); exit(2); } } while (0)

static ID3D12Device *device;
static compile_fn compile;

static ID3D12Resource *make_resource(D3D12_HEAP_TYPE type, const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear)
{
   D3D12_HEAP_PROPERTIES hp;
   ID3D12Resource *r;
   memset(&hp, 0, sizeof(hp));
   hp.Type = type;
   CHECK(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, d, state, clear, &IID_ID3D12Resource, (void **)&r));
   return r;
}

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
   D3D12_RESOURCE_DESC d;
   memset(&d, 0, sizeof(d));
   d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   d.Width = bytes;
   d.Height = 1;
   d.DepthOrArraySize = 1;
   d.MipLevels = 1;
   d.SampleDesc.Count = 1;
   d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   d.Flags = flags;
   return make_resource(type, &d, state, NULL);
}

static ID3DBlob *make_shader(const char *text, size_t len, const char *entry, const char *profile)
{
   ID3DBlob *code = NULL, *err = NULL;
   if (FAILED(compile(text, len, "oit_depth_test", NULL, NULL, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
   {
      printf("oit_depth_test_d3d12: %s: %s\n", entry, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "not compiled");
      exit(2);
   }
   return code;
}

static void barrier(ID3D12GraphicsCommandList *cl, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
   D3D12_RESOURCE_BARRIER b;
   memset(&b, 0, sizeof(b));
   b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b.Transition.pResource = r;
   b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
   b.Transition.StateBefore = from;
   b.Transition.StateAfter = to;
   ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}

/* a vertex's 1/w (oit_depth_test.c's) */
static float model_z(unsigned i, unsigned j)
{
   unsigned h = (i * 73856093u) ^ (j * 19349663u);
   float bump;
   h ^= h >> 13;
   h *= 0x5bd1e995u;
   h ^= h >> 15;
   bump = (float)(h & 1023u) / 1023.f;
   return 0.0103f + 0.0003f * (float)i + 0.00041f * (float)j + 0.0021f * bump;
}

int main(int argc, char **argv)
{
   static const char vs_text[] =
      "cbuffer c : register(b0) { float4 normalScale; float4 normalOffset; };\n"
      "precise float4 main(float3 pos : POSITION) : SV_Position\n"
      "{\n"
      "	precise float3 p = pos * normalScale.xyz + normalOffset.xyz;\n"
      "	precise float w = 1.0 / p.z;\n"
      "	precise float4 vpos = float4(p.x * w, p.y * w, normalScale.w, w);\n"
      "	return vpos;\n"
      "}\n";
   static const char a_text[] = "float4 main(float4 pos : SV_Position) : SV_Target { return float4(1.0, 1.0, 1.0, 1.0); }\n";
   HMODULE lib, dc;
   create_device_fn create;
   serialize_fn serialize;
   ID3D12CommandQueue *queue;
   ID3D12CommandAllocator *alloc;
   ID3D12GraphicsCommandList *cl;
   ID3D12Fence *fence;
   ID3D12RootSignature *root;
   ID3D12PipelineState *pa, *pb;
   ID3D12DescriptorHeap *rtv_heap, *dsv_heap, *srv_heap;
   ID3D12Resource *color, *depth, *verts, *counts, *readback, *cbuf;
   ID3DBlob *vs, *pas, *ps, *rs_blob, *rs_err;
   D3D12_COMMAND_QUEUE_DESC qd;
   D3D12_ROOT_PARAMETER params[3];
   D3D12_DESCRIPTOR_RANGE range;
   D3D12_ROOT_SIGNATURE_DESC rsd;
   D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
   D3D12_INPUT_ELEMENT_DESC ied = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
   D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ql;
   D3D12_RESOURCE_DESC rd;
   D3D12_CLEAR_VALUE cv;
   D3D12_DESCRIPTOR_HEAP_DESC hd;
   D3D12_CPU_DESCRIPTOR_HANDLE rtv, dsv, srv;
   D3D12_DEPTH_STENCIL_VIEW_DESC dvd;
   D3D12_SHADER_RESOURCE_VIEW_DESC svd;
   D3D12_VERTEX_BUFFER_VIEW vbv;
   D3D12_VIEWPORT vp = { 0.f, 0.f, (float)W, (float)H, 0.f, 1.f };
   D3D12_RECT sc = { 0, 0, W, H };
   D3D12_RANGE whole = { 0, 0 };
   float *v, *u, fz_max = 0.f;
   const unsigned *c;
   unsigned samples, nverts, i, j, k;
   char *text;
   long len;
   FILE *f;
   int bad;

   setvbuf(stdout, NULL, _IONBF, 0);
   if (argc < 3)
   {
      printf("usage: oit_depth_test_d3d12 N SHADER.hlsl\n");
      return 2;
   }
   samples = (unsigned)atoi(argv[1]);
   f = fopen(argv[2], "rb");
   if (!f || fseek(f, 0, SEEK_END) || (len = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) || !(text = (char *)malloc(len))
         || fread(text, 1, len, f) != (size_t)len)
   {
      printf("oit_depth_test_d3d12: %s not read\n", argv[2]);
      return 2;
   }
   fclose(f);

   lib = LoadLibraryA("d3d12.dll");
   dc = LoadLibraryA("d3dcompiler_47.dll");
   create = lib ? (create_device_fn)(void *)GetProcAddress(lib, "D3D12CreateDevice") : NULL;
   serialize = lib ? (serialize_fn)(void *)GetProcAddress(lib, "D3D12SerializeRootSignature") : NULL;
   compile = dc ? (compile_fn)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
   if (!create || !serialize || !compile)
   {
      printf("oit_depth_test_d3d12: no d3d12.dll or d3dcompiler_47.dll\n");
      return 2;
   }
   CHECK(create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device));

   /* N samples in the standard pattern (rend/ssaa_pattern.h's), as the renderer asks */
   memset(&ql, 0, sizeof(ql));
   ql.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
   ql.SampleCount = samples;
   if (FAILED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &ql, sizeof(ql))) || ql.NumQualityLevels == 0)
   {
      printf("oit_depth_test_d3d12: the device has no %u samples a pixel\n", samples);
      return 3;
   }

   memset(&qd, 0, sizeof(qd));
   CHECK(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
   CHECK(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
   CHECK(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
   CHECK(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));

   /* the root signature: the vertex constants, the depth's view, the counts */
   memset(params, 0, sizeof(params));
   params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
   params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
   memset(&range, 0, sizeof(range));
   range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
   range.NumDescriptors = 1;
   params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
   params[1].DescriptorTable.NumDescriptorRanges = 1;
   params[1].DescriptorTable.pDescriptorRanges = &range;
   params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
   params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
   params[2].Descriptor.ShaderRegister = 1;
   params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
   memset(&rsd, 0, sizeof(rsd));
   rsd.NumParameters = 3;
   rsd.pParameters = params;
   rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
   CHECK(serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &rs_err));
   CHECK(ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(rs_blob), ID3D10Blob_GetBufferSize(rs_blob),
         &IID_ID3D12RootSignature, (void **)&root));

   vs = make_shader(vs_text, sizeof(vs_text) - 1, "main", "vs_5_1");
   pas = make_shader(a_text, sizeof(a_text) - 1, "main", "ps_5_1");
   ps = make_shader(text, (size_t)len, "ps_main", "ps_5_1");

   /* A: the depth written (a pixel shader that writes a colour: Mesa's
    * lavapipe under vkd3d-proton crashed on a draw after one with none);
    * B: the lists' pass, once a
    * pixel, no depth test (the shader's own) and nothing written but the
    * counts. The renderer's rasterizer state: no depth clip. */
   memset(&pd, 0, sizeof(pd));
   pd.pRootSignature = root;
   pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs);
   pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
   pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(pas);
   pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(pas);
   pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
   pd.SampleMask = UINT_MAX;
   pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
   pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
   pd.RasterizerState.DepthClipEnable = FALSE;
   pd.DepthStencilState.DepthEnable = TRUE;
   pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
   pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
   pd.InputLayout.pInputElementDescs = &ied;
   pd.InputLayout.NumElements = 1;
   pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
   pd.NumRenderTargets = 1;
   pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
   pd.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
   pd.SampleDesc.Count = samples;
   pd.SampleDesc.Quality = DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN;
   CHECK(ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&pa));
   pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps);
   pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
   pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
   pd.DepthStencilState.DepthEnable = FALSE;
   pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
   pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
   CHECK(ID3D12Device_CreateGraphicsPipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&pb));

   /* the pictures: a colour and a depth of N samples */
   memset(&rd, 0, sizeof(rd));
   rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   rd.Width = W;
   rd.Height = H;
   rd.DepthOrArraySize = 1;
   rd.MipLevels = 1;
   rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   rd.SampleDesc.Count = samples;
   rd.SampleDesc.Quality = DXGI_STANDARD_MULTISAMPLE_QUALITY_PATTERN;
   rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
   memset(&cv, 0, sizeof(cv));
   cv.Format = rd.Format;
   color = make_resource(D3D12_HEAP_TYPE_DEFAULT, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
   /* (the renderer's depth formats: rend/dx12/dx12context_lr.h) */
   rd.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
   rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
   cv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
   cv.DepthStencil.Depth = 0.f;
   depth = make_resource(D3D12_HEAP_TYPE_DEFAULT, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv);

   memset(&hd, 0, sizeof(hd));
   hd.NumDescriptors = 1;
   hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
   CHECK(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
   hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
   CHECK(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap));
   hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
   hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
   CHECK(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap));
   rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv);
   dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv);
   srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(srv_heap, &srv);
   ID3D12Device_CreateRenderTargetView(device, color, NULL, rtv);
   memset(&dvd, 0, sizeof(dvd));
   dvd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
   dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
   ID3D12Device_CreateDepthStencilView(device, depth, &dvd, dsv);
   memset(&svd, 0, sizeof(svd));
   svd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
   svd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
   svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
   ID3D12Device_CreateShaderResourceView(device, depth, &svd, srv);

   /* the model: a grid of cells a little over 12 game pixels wide, two
    * triangles each, every vertex at its own depth */
   nverts = (GX - 1) * (GY - 1) * 6;
   verts = make_buffer(D3D12_HEAP_TYPE_UPLOAD, nverts * 12, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
   CHECK(ID3D12Resource_Map(verts, 0, &whole, (void **)&v));
   k = 0;
   for (j = 0; j + 1 < GY; j++)
      for (i = 0; i + 1 < GX; i++)
      {
         static const unsigned corner[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
         unsigned t;
         for (t = 0; t < 6; t++)
         {
            const unsigned ci = i + corner[t][0], cj = j + corner[t][1];
            v[k++] = 3.3f + (float)ci * 12.07f + ((ci * 7u + cj * 3u) % 5u) * 0.37f;
            v[k++] = 2.9f + (float)cj * 11.61f + ((ci * 5u + cj * 11u) % 7u) * 0.29f;
            v[k] = model_z(ci, cj);
            if (v[k] > fz_max)
               fz_max = v[k];
            k++;
         }
      }
   /* (transformVertex()'s constants: x and y to clip space, z as it is, the depth scale 1/fZ_max) */
   cbuf = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
   CHECK(ID3D12Resource_Map(cbuf, 0, &whole, (void **)&u));
   memset(u, 0, 256);
   u[0] = 2.f / 640.f; u[1] = -2.f / 480.f; u[2] = 1.f; u[3] = 1.f / fz_max;
   u[4] = -1.f; u[5] = 1.f;
   counts = make_buffer(D3D12_HEAP_TYPE_DEFAULT, C_WORDS * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   readback = make_buffer(D3D12_HEAP_TYPE_READBACK, C_WORDS * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);

   ID3D12GraphicsCommandList_ClearDepthStencilView(cl, dsv, D3D12_CLEAR_FLAG_DEPTH, 0.f, 0, 0, NULL);
   ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, &dsv);
   ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
   ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
   ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, root);
   ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &srv_heap);
   ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 0, ID3D12Resource_GetGPUVirtualAddress(cbuf));
   ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(cl, 2, ID3D12Resource_GetGPUVirtualAddress(counts));
   {
      D3D12_GPU_DESCRIPTOR_HANDLE g;
      srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(srv_heap, &g);
      ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, g);
   }
   vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(verts);
   vbv.SizeInBytes = nverts * 12;
   vbv.StrideInBytes = 12;
   ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &vbv);
   ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   ID3D12GraphicsCommandList_SetPipelineState(cl, pa);
   ID3D12GraphicsCommandList_DrawInstanced(cl, nverts, 1, 0, 0);
   barrier(cl, depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
   ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
   ID3D12GraphicsCommandList_SetPipelineState(cl, pb);
   ID3D12GraphicsCommandList_DrawInstanced(cl, nverts, 1, 0, 0);
   barrier(cl, counts, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
   ID3D12GraphicsCommandList_CopyResource(cl, readback, counts);
   CHECK(ID3D12GraphicsCommandList_Close(cl));
   ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&cl);
   CHECK(ID3D12CommandQueue_Signal(queue, fence, 1));
   while (ID3D12Fence_GetCompletedValue(fence) < 1)
      Sleep(1);

   /* the shader's counts (oit_depth_test_d3d12.sh says what each is) */
   whole.End = C_WORDS * 4;
   CHECK(ID3D12Resource_Map(readback, 0, &whole, (void **)&c));
   printf("%u samples a pixel\n", samples);
   printf("fragments %u (%u of them on an edge, %u not at the centre), samples covered %u\n", c[0], c[1], c[12], c[2]);
   printf("the plane at each sample: %u samples missed (the device's depth there is nearer), %u its depth to the bit, "
         "%u ulp at most off\n", c[3], c[4], c[5]);
   printf("the centre's depth for all of them: %u samples missed\n", c[7]);
   printf("the kept depth outside its samples' depths: %u fragments\n", c[8]);
   bad = c[0] == 0 || c[3] != 0 || c[8] != 0;
   printf("oit_depth_test_d3d12: %s\n", bad ? "FAIL" : "PASS");
   return bad;
}
