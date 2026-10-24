/* tile_test.c with Direct3D 12 for the device: the compute renderer's
 * kernels as HLSL (tools/rend/kernels_dx12.sh compute-test: test mode,
 * nothing fused, DXIL) against the reference on the CPU, on the frames
 * tile_dump wrote, recorded as the Direct3D 12 renderers record them
 * (rend/dx12/dx12_compute.cpp): the same root signature - the buffers as
 * root unordered access views, the walk's textures a bindless table and
 * a sampler table, the picture a view of its own - the frame's words in
 * write-combined memory of a custom heap the kernels read in place, the
 * dispatches whose sizes the GPU made from the bins' counts copied to an
 * argument buffer first. Built with mingw-w64 and run under wine with
 * vkd3d-proton, or on Windows: tools/rend/tile_test.sh with
 * TILE_DEVICE=d3d12.
 *
 *   tile_test_d3d12.exe DXIL_DIR FRAME...
 *
 * DXIL_DIR has the kernels kernels_dx12.sh compute-test made: kK.dxil, the
 * walk k5_SAMPLES_KEEP.dxil for each count and K the test draws with (its
 * sorted variant; k5u_SAMPLES.dxil its unsorted, dispatched before it). The
 * environment is tile_test.c's (TILE_SAMPLES, TILE_KEEP, TILE_GPU,
 * TILE_PPM, TILE_TIMES, TILE_PROD); TILE_VALIDATE=1 the debug layer, where
 * there is one. A workgroup has the 32 KiB of shared memory Direct3D 12
 * gives every one: the walk's own K is pvrc_fragments_kept() of that. */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>

#include "tile_test_main.h"

typedef HRESULT (WINAPI *create_device_fn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *serialize_fn)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
typedef HRESULT (WINAPI *debug_fn)(REFIID, void **);

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("tile_test_d3d12: %s: %08lx\n", #x, (unsigned long)hr_); exit(2); } } while (0)

static ID3D12Device *device;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *cl;
static ID3D12Fence *fence;
static UINT64 fence_value;
static ID3D12RootSignature *root;
static ID3D12CommandSignature *dispatch_sig;
static ID3D12DescriptorHeap *views, *samplers;
static ID3D12QueryHeap *queries;
static UINT64 tick_hz;
static const char *dxil_dir;
static int validating;

/* The pipelines made for a count of samples and a K, kept */
struct kernels { unsigned samples, keep; ID3D12PipelineState *k[PVRC_KERNELS + 2]; };	/* (the walk's unsorted variant last) */
static struct kernels made[16];
static unsigned made_count;

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
   D3D12_HEAP_PROPERTIES hp;
   D3D12_RESOURCE_DESC d;
   ID3D12Resource *r;
   memset(&hp, 0, sizeof(hp));
   hp.Type = type;
   if (type == D3D12_HEAP_TYPE_CUSTOM)
   {
      /* (what the renderer has the frame's words in) */
      hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
      hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
   }
   memset(&d, 0, sizeof(d));
   d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
   d.Width = bytes < 256 ? 256 : bytes;
   d.Height = 1;
   d.DepthOrArraySize = 1;
   d.MipLevels = 1;
   d.SampleDesc.Count = 1;
   d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
   d.Flags = flags;
   CHECK(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&r));
   return r;
}

static ID3D12Resource *make_image(unsigned w, unsigned h, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
   D3D12_HEAP_PROPERTIES hp;
   D3D12_RESOURCE_DESC d;
   ID3D12Resource *r;
   memset(&hp, 0, sizeof(hp));
   hp.Type = D3D12_HEAP_TYPE_DEFAULT;
   memset(&d, 0, sizeof(d));
   d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
   d.Width = w;
   d.Height = h;
   d.DepthOrArraySize = 1;
   d.MipLevels = 1;
   d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   d.SampleDesc.Count = 1;
   d.Flags = flags;
   CHECK(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&r));
   return r;
}

static void *map(ID3D12Resource *r)
{
   void *p;
   CHECK(ID3D12Resource_Map(r, 0, NULL, &p));
   return p;
}

static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
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

static void uav_barrier(void)
{
   D3D12_RESOURCE_BARRIER b;
   memset(&b, 0, sizeof(b));
   b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
   ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}

/* The list executed, waited for, and opened again */
static void run(void)
{
   ID3D12CommandList *lists[1];
   CHECK(ID3D12GraphicsCommandList_Close(cl));
   lists[0] = (ID3D12CommandList *)cl;
   ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
   CHECK(ID3D12CommandQueue_Signal(queue, fence, ++fence_value));
   while (ID3D12Fence_GetCompletedValue(fence) < fence_value)
      Sleep(0);
   CHECK(ID3D12CommandAllocator_Reset(alloc));
   CHECK(ID3D12GraphicsCommandList_Reset(cl, alloc, NULL));
}

static void validation_messages(void)
{
   ID3D12InfoQueue *iq;
   UINT64 i, n;
   if (!validating || FAILED(ID3D12Device_QueryInterface(device, &IID_ID3D12InfoQueue, (void **)&iq)))
      return;
   n = ID3D12InfoQueue_GetNumStoredMessages(iq);
   for (i = 0; i < n; i++)
   {
      SIZE_T len = 0;
      D3D12_MESSAGE *m;
      ID3D12InfoQueue_GetMessage(iq, i, NULL, &len);
      m = (D3D12_MESSAGE *)malloc(len);
      ID3D12InfoQueue_GetMessage(iq, i, m, &len);
      if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR && validation_errors++ < 10)
         printf("validation: %s\n", m->pDescription);
      free(m);
   }
   ID3D12InfoQueue_ClearStoredMessages(iq);
   ID3D12InfoQueue_Release(iq);
}

static void init(const char *dir, int validate)
{
   HMODULE lib;
   create_device_fn create;
   serialize_fn serialize;
   D3D12_COMMAND_QUEUE_DESC qd;
   D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1;
   D3D12_FEATURE_DATA_SHADER_MODEL sm;
   D3D12_ROOT_PARAMETER params[4];
   D3D12_DESCRIPTOR_RANGE ranges[4];
   D3D12_STATIC_SAMPLER_DESC linear;
   D3D12_ROOT_SIGNATURE_DESC rd;
   D3D12_INDIRECT_ARGUMENT_DESC arg;
   D3D12_COMMAND_SIGNATURE_DESC sig;
   D3D12_DESCRIPTOR_HEAP_DESC hd;
   D3D12_SAMPLER_DESC sd;
   D3D12_QUERY_HEAP_DESC qh;
   ID3DBlob *blob, *err = NULL;
   unsigned i;

   dxil_dir = dir;
   lib = LoadLibraryA("d3d12.dll");
   create = lib ? (create_device_fn)(void *)GetProcAddress(lib, "D3D12CreateDevice") : NULL;
   serialize = lib ? (serialize_fn)(void *)GetProcAddress(lib, "D3D12SerializeRootSignature") : NULL;
   if (validate && lib)
   {
      debug_fn get = (debug_fn)(void *)GetProcAddress(lib, "D3D12GetDebugInterface");
      ID3D12Debug *dbg;
      if (get && SUCCEEDED(get(&IID_ID3D12Debug, (void **)&dbg)))
      {
         ID3D12Debug_EnableDebugLayer(dbg);
         validating = 1;
      }
      else
         printf("tile_test_d3d12: no debug layer\n");
   }
   if (!create || !serialize || FAILED(create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device)))
   {
      printf("tile_test_d3d12: no Direct3D 12 device\n");
      exit(2);
   }
   memset(&o1, 0, sizeof(o1));
   sm.HighestShaderModel = D3D_SHADER_MODEL_6_0;
   if (FAILED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1))) || !o1.Int64ShaderOps
         || FAILED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) || sm.HighestShaderModel < D3D_SHADER_MODEL_6_0)
   {
      printf("tile_test_d3d12: no shader model 6.0 with 64-bit integers\n");
      exit(2);
   }
   shared_bytes = 32768;
   printf("device: Direct3D 12, shader model %x, %u bytes of shared memory%s\n", (unsigned)sm.HighestShaderModel, shared_bytes,
         validating ? ", debug layer on" : "");
   memset(&qd, 0, sizeof(qd));
   CHECK(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
   CHECK(ID3D12CommandQueue_GetTimestampFrequency(queue, &tick_hz));
   CHECK(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
   CHECK(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
   CHECK(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));

   /* the renderer's root signature (dx12_compute.cpp): the pictures'
    * places (b0); a table of the buffers (u0 to u9: views, so that what
    * is read past a buffer's end is 0, and what is written there goes
    * nowhere, as in Vulkan's robust buffers) and the picture drawn (u0,
    * space 2); the bindless table (t0 on, space 1); the samplers (s0 on,
    * space 1); the linear sampler (s0) */
   memset(params, 0, sizeof(params));
   params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
   params[0].Constants.Num32BitValues = 2;
   memset(ranges, 0, sizeof(ranges));
   ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
   ranges[0].NumDescriptors = 10;
   ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
   ranges[1].NumDescriptors = 1;
   ranges[1].RegisterSpace = 2;
   ranges[1].OffsetInDescriptorsFromTableStart = 10;
   ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
   ranges[2].NumDescriptors = UINT_MAX;
   ranges[2].RegisterSpace = 1;
   ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
   ranges[3].NumDescriptors = UINT_MAX;
   ranges[3].RegisterSpace = 1;
   for (i = 0; i < 3; i++)
   {
      params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[1 + i].DescriptorTable.NumDescriptorRanges = i == 0 ? 2 : 1;
      params[1 + i].DescriptorTable.pDescriptorRanges = &ranges[i == 0 ? 0 : i + 1];
   }
   memset(&linear, 0, sizeof(linear));
   linear.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
   linear.AddressU = linear.AddressV = linear.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
   linear.MaxLOD = D3D12_FLOAT32_MAX;
   memset(&rd, 0, sizeof(rd));
   rd.NumParameters = 4;
   rd.pParameters = params;
   rd.NumStaticSamplers = 1;
   rd.pStaticSamplers = &linear;
   if (FAILED(serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
   {
      printf("tile_test_d3d12: root signature: %s\n", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
      exit(2);
   }
   CHECK(ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
         &IID_ID3D12RootSignature, (void **)&root));
   ID3D10Blob_Release(blob);
   memset(&arg, 0, sizeof(arg));
   arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
   memset(&sig, 0, sizeof(sig));
   sig.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
   sig.NumArgumentDescs = 1;
   sig.pArgumentDescs = &arg;
   CHECK(ID3D12Device_CreateCommandSignature(device, &sig, NULL, &IID_ID3D12CommandSignature, (void **)&dispatch_sig));
   /* the views: the buffers' from 0, the picture drawn's at 10, the
    * textures' table from 11 (its first the texture every slot, the picture
    * to start from and the palette are: one texel) */
   memset(&hd, 0, sizeof(hd));
   hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
   hd.NumDescriptors = 12;
   hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
   CHECK(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&views));
   hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
   hd.NumDescriptors = 1;
   CHECK(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&samplers));
   memset(&sd, 0, sizeof(sd));
   sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
   sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
   sd.MaxLOD = D3D12_FLOAT32_MAX;
   {
      D3D12_CPU_DESCRIPTOR_HANDLE h;
      samplers->lpVtbl->GetCPUDescriptorHandleForHeapStart(samplers, &h);
      ID3D12Device_CreateSampler(device, &sd, h);
   }
   memset(&qh, 0, sizeof(qh));
   qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
   qh.Count = 8;
   CHECK(ID3D12Device_CreateQueryHeap(device, &qh, &IID_ID3D12QueryHeap, (void **)&queries));
}

static ID3D12PipelineState *make_pipeline(const char *name)
{
   char path[1024];
   size_t size;
   unsigned char *code;
   D3D12_COMPUTE_PIPELINE_STATE_DESC pd;
   ID3D12PipelineState *pso;
   snprintf(path, sizeof(path), "%s/%s", dxil_dir, name);
   code = read_file(path, &size);
   memset(&pd, 0, sizeof(pd));
   pd.pRootSignature = root;
   pd.CS.pShaderBytecode = code;
   pd.CS.BytecodeLength = size;
   CHECK(ID3D12Device_CreateComputePipelineState(device, &pd, &IID_ID3D12PipelineState, (void **)&pso));
   free(code);
   return pso;
}

static ID3D12PipelineState **pipelines_for(unsigned samples, unsigned keep)
{
   unsigned i, k;
   char name[64];
   for (i = 0; i < made_count; i++)
      if (made[i].samples == samples && made[i].keep == keep)
         return made[i].k;
   if (made_count == sizeof(made) / sizeof(made[0]))
      exit(2);
   made[made_count].samples = samples;
   made[made_count].keep = keep;
   for (k = 1; k <= PVRC_KERNELS; k++)
   {
      if (k == PVRC_K_WALK)
         snprintf(name, sizeof(name), "k%u_%u_%u.dxil", k, samples, keep);
      else
         snprintf(name, sizeof(name), "k%u.dxil", k);
      made[made_count].k[k] = make_pipeline(name);
   }
   snprintf(name, sizeof(name), "k%uu_%u.dxil", PVRC_K_WALK, samples);
   made[made_count].k[PVRC_KERNELS + 1] = make_pipeline(name);
   return made[made_count++].k;
}

/* tile_test.c's: the same frame, the same kernels, the same order */
static double gpu_draw(const unsigned *frame0, unsigned words, unsigned samples, unsigned keep, unsigned *out, unsigned *stats, int gpu)
{
   const unsigned w = frame0[PVRC_F_WIDTH], h = frame0[PVRC_F_HEIGHT], runs = frame0[PVRC_F_RUNS];
   const unsigned tiles = frame0[PVRC_F_TILES_X] * frame0[PVRC_F_TILES_Y];
   const unsigned binWords = pvrc_bin_words(runs, frame0[PVRC_F_POOL]);
   const unsigned nsrc = frame0[PVRC_F_SOURCES] + 1;
   const D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
   const D3D12_RESOURCE_FLAGS UAVF = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
   const UINT64 cbBytes = (UINT64)nsrc * PVRC_TAG_C_WORDS * 4, tbBytes = (UINT64)nsrc * 2 * PVRC_TAG_C_WORDS * 4, dbBytes = (PVRC_TAG_O_ZSCALE + 1) * 4;
   ID3D12Resource *cb = make_buffer(D3D12_HEAP_TYPE_CUSTOM, cbBytes, UAVF, UAV);
   ID3D12Resource *tb = make_buffer(D3D12_HEAP_TYPE_CUSTOM, tbBytes, UAVF, UAV);
   ID3D12Resource *db = make_buffer(D3D12_HEAP_TYPE_CUSTOM, dbBytes, UAVF, UAV);
   unsigned *gf = gpu ? (unsigned *)malloc((size_t)words * 4) : NULL;
   unsigned *cbm = (unsigned *)map(cb), *tbm = (unsigned *)map(tb), *dbm = (unsigned *)map(db);
   const unsigned prims = gpu ? gpu_frame(frame0, words, gf, cbm, tbm, dbm) : frame0[PVRC_F_PRIMS];
   const unsigned *frame = gpu ? gf : frame0;
   /* (the frame's words where the renderer has them; the slots' words all 0:
    * the first texture, with the first sampler) */
   const UINT64 fbBytes = (UINT64)words * 4, rbBytes = (UINT64)(prims ? prims : 1) * PVRC_REC_WORDS * 4, bbBytes = (UINT64)binWords * 4;
   ID3D12Resource *fb = make_buffer(D3D12_HEAP_TYPE_CUSTOM, fbBytes, UAVF, UAV);
   ID3D12Resource *sb = make_buffer(D3D12_HEAP_TYPE_CUSTOM, PVRC_TEX_SLOTS * 4, UAVF, UAV);
   ID3D12Resource *rb = make_buffer(D3D12_HEAP_TYPE_DEFAULT, rbBytes, UAVF, UAV);
   ID3D12Resource *bb = make_buffer(D3D12_HEAP_TYPE_DEFAULT, bbBytes, UAVF, UAV);
   ID3D12Resource *ab = make_buffer(D3D12_HEAP_TYPE_DEFAULT, 16 * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
   const UINT64 pitch = ((UINT64)w * 4 + 255) & ~(UINT64)255;
   ID3D12Resource *ob = make_buffer(D3D12_HEAP_TYPE_READBACK, pitch * h, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12Resource *bins_back = make_buffer(D3D12_HEAP_TYPE_READBACK, (UINT64)binWords * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12Resource *frame_back = make_buffer(D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12Resource *time_back = make_buffer(D3D12_HEAP_TYPE_READBACK, 8 * 8, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12Resource *outImage = make_image(w, h, UAVF, UAV);
   ID3D12Resource *dummy = make_image(1, 1, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12Resource *dummy_up = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
   ID3D12PipelineState **k = pipelines_for(samples, keep);
   const unsigned fw = PVRC_FINE_W(samples), fh = PVRC_FINE_H(samples);
   ID3D12DescriptorHeap *heaps[2];
   D3D12_CPU_DESCRIPTOR_HANDLE vc;
   D3D12_GPU_DESCRIPTOR_HANDLE vg, sg;
   UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
   D3D12_TEXTURE_COPY_LOCATION dst, src;
   unsigned *m;
   double t0, t1;
   unsigned stamp = 0;
   unsigned i;

   memcpy(map(fb), frame, (size_t)words * 4);
   memset(map(sb), 0, PVRC_TEX_SLOTS * 4);
   /* the one texel: 0, or green and transparent for TILE_PROD, as tile_test.c has it */
   m = (unsigned *)map(dummy_up);
   m[0] = getenv("TILE_PROD") ? 0x0000ff00u : 0u;
   views->lpVtbl->GetCPUDescriptorHandleForHeapStart(views, &vc);
   views->lpVtbl->GetGPUDescriptorHandleForHeapStart(views, &vg);
   samplers->lpVtbl->GetGPUDescriptorHandleForHeapStart(samplers, &sg);
   {
      D3D12_UNORDERED_ACCESS_VIEW_DESC ud;
      D3D12_SHADER_RESOURCE_VIEW_DESC sd;
      ID3D12Resource *b[10];
      UINT64 bytes[10];
      b[0] = fb; b[1] = rb; b[2] = bb;
      /* (the vertices, indices and volume triangles are the frame's own words, either way) */
      b[3] = b[4] = b[5] = fb;
      b[6] = cb; b[7] = tb; b[8] = db; b[9] = sb;
      bytes[0] = bytes[3] = bytes[4] = bytes[5] = fbBytes;
      bytes[1] = rbBytes; bytes[2] = bbBytes; bytes[6] = cbBytes; bytes[7] = tbBytes; bytes[8] = dbBytes; bytes[9] = PVRC_TEX_SLOTS * 4;
      for (i = 0; i < 10; i++)
      {
         memset(&ud, 0, sizeof(ud));
         ud.Format = DXGI_FORMAT_UNKNOWN;
         ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
         ud.Buffer.NumElements = (UINT)(bytes[i] / 4);
         ud.Buffer.StructureByteStride = 4;
         ID3D12Device_CreateUnorderedAccessView(device, b[i], NULL, &ud, vc);
         vc.ptr += inc;
      }
      memset(&ud, 0, sizeof(ud));
      ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
      ID3D12Device_CreateUnorderedAccessView(device, outImage, NULL, &ud, vc);
      vc.ptr += inc;
      memset(&sd, 0, sizeof(sd));
      sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      sd.Texture2D.MipLevels = 1;
      ID3D12Device_CreateShaderResourceView(device, dummy, &sd, vc);
   }
   memset(&dst, 0, sizeof(dst));
   dst.pResource = dummy;
   dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   memset(&src, 0, sizeof(src));
   src.pResource = dummy_up;
   src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
   src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   src.PlacedFootprint.Footprint.Width = 1;
   src.PlacedFootprint.Footprint.Height = 1;
   src.PlacedFootprint.Footprint.Depth = 1;
   src.PlacedFootprint.Footprint.RowPitch = 256;
   ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
   transition(dummy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

   heaps[0] = views;
   heaps[1] = samplers;
   ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, heaps);
   ID3D12GraphicsCommandList_SetComputeRootSignature(cl, root);
   {
      const unsigned places[2] = { 0, 0 };
      D3D12_GPU_DESCRIPTOR_HANDLE table = vg;
      ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 0, 2, places, 0);
      ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 1, vg);
      table.ptr += 11 * inc;
      ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 2, table);
      ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 3, sg);
   }
   ID3D12GraphicsCommandList_EndQuery(cl, queries, D3D12_QUERY_TYPE_TIMESTAMP, stamp++);
   /* the bins' counts to the dispatches' arguments */
#define ARGS() do { transition(bb, UAV, D3D12_RESOURCE_STATE_COPY_SOURCE); \
      transition(ab, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST); \
      ID3D12GraphicsCommandList_CopyBufferRegion(cl, ab, 0, bb, (UINT64)PVRC_B_STATS(runs) * 4, 16 * 4); \
      transition(ab, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT); \
      transition(bb, D3D12_RESOURCE_STATE_COPY_SOURCE, UAV); } while (0)
#define STEP(kernel, x, y) do { ID3D12GraphicsCommandList_SetPipelineState(cl, k[kernel]); \
      if ((x) && (y)) { ID3D12GraphicsCommandList_Dispatch(cl, (x), (y), 1); } uav_barrier(); \
      ID3D12GraphicsCommandList_EndQuery(cl, queries, D3D12_QUERY_TYPE_TIMESTAMP, stamp++); } while (0)
#define INDIRECT(kernel, at) do { ID3D12GraphicsCommandList_SetPipelineState(cl, k[kernel]); \
      ID3D12GraphicsCommandList_ExecuteIndirect(cl, dispatch_sig, 1, ab, (UINT64)(at) * 4, NULL, 0); uav_barrier(); \
      ID3D12GraphicsCommandList_EndQuery(cl, queries, D3D12_QUERY_TYPE_TIMESTAMP, stamp++); } while (0)
   if (gpu)
   {
      ID3D12GraphicsCommandList_SetPipelineState(cl, k[PVRC_K_GPUSRC]);
      ID3D12GraphicsCommandList_Dispatch(cl, 1, 1, 1);
      uav_barrier();
      ARGS();
      INDIRECT(PVRC_K_SETUP, PVRC_ST_SETUP_X);
      STEP(PVRC_K_SCAN, 1, 1);
      INDIRECT(PVRC_K_SCATTER, PVRC_ST_SETUP_X);
      INDIRECT(PVRC_K_SORT, PVRC_ST_SORT_X);
   }
   else
   {
      STEP(PVRC_K_SETUP, (prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP, 1);
      STEP(PVRC_K_SCAN, 1, 1);
      STEP(PVRC_K_SCATTER, (prims + PVRC_WG_SETUP - 1) / PVRC_WG_SETUP, 1);
      STEP(PVRC_K_SORT, tiles, frame[PVRC_F_CHUNKS]);
   }
   /* (the walk's two variants over the same tiles, as the renderer
    * dispatches them) */
   ID3D12GraphicsCommandList_SetPipelineState(cl, k[PVRC_KERNELS + 1]);
   ID3D12GraphicsCommandList_Dispatch(cl, (w + fw - 1) / fw, (h + fh - 1) / fh, 1);
   STEP(PVRC_K_WALK, (w + fw - 1) / fw, (h + fh - 1) / fh);
   ARGS();
   ID3D12GraphicsCommandList_SetPipelineState(cl, k[PVRC_K_CLEAR]);
   ID3D12GraphicsCommandList_ExecuteIndirect(cl, dispatch_sig, 1, ab, (UINT64)PVRC_ST_CLEAR_X * 4, NULL, 0);
   uav_barrier();
   ID3D12GraphicsCommandList_ResolveQueryData(cl, queries, D3D12_QUERY_TYPE_TIMESTAMP, 0, stamp, time_back, 0);
   /* the picture, the bins and the frame's count of triangles, read back */
   transition(outImage, UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
   memset(&dst, 0, sizeof(dst));
   dst.pResource = ob;
   dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
   dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
   dst.PlacedFootprint.Footprint.Width = w;
   dst.PlacedFootprint.Footprint.Height = h;
   dst.PlacedFootprint.Footprint.Depth = 1;
   dst.PlacedFootprint.Footprint.RowPitch = (UINT)pitch;
   memset(&src, 0, sizeof(src));
   src.pResource = outImage;
   src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
   ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
   transition(bb, UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
   ID3D12GraphicsCommandList_CopyBufferRegion(cl, bins_back, 0, bb, 0, (UINT64)binWords * 4);
   transition(fb, UAV, D3D12_RESOURCE_STATE_COPY_SOURCE);
   ID3D12GraphicsCommandList_CopyBufferRegion(cl, frame_back, 0, fb, 0, 256);
   t0 = now_ms();
   run();
   t1 = now_ms();
   validation_messages();
   {
      const unsigned char *o = (const unsigned char *)map(ob);
      for (i = 0; i < h; i++)
         memcpy(out + (size_t)i * w, o + (size_t)i * pitch, (size_t)w * 4);
   }
   if (getenv("TILE_TIMES") && stamp >= 6)
   {
      const UINT64 *ts = (const UINT64 *)map(time_back);
      const double f = 1e3 / (double)tick_hz;
      /* (as the decode's: the numbering's dispatch has no stamp of its own) */
      printf("tile_test: kernels: setup %.2f, scan %.2f, scatter %.2f, sort %.2f, walk %.2f ms\n",
            (ts[1] - ts[0]) * f, (ts[2] - ts[1]) * f, (ts[3] - ts[2]) * f, (ts[4] - ts[3]) * f, (ts[5] - ts[4]) * f);
   }
   {
      const unsigned *bins = (const unsigned *)map(bins_back);
      const unsigned *pool = bins + PVRC_B_POOL(runs);
      unsigned dirty = 0;
      memcpy(stats, bins + PVRC_B_STATS(runs), PVRC_STATS * 4);
      if (gpu && ((const unsigned *)map(frame_back))[PVRC_F_PRIMS] != prims)
      {
         printf("tile_test: numbered on the device to %u triangles, not %u\n", ((const unsigned *)map(frame_back))[PVRC_F_PRIMS], prims);
         validation_errors++;
      }
      /* the pool is left as the clear leaves it, all 0 */
      for (i = 0; i < (frame[PVRC_F_POOL] + 1) / 2; i++)
         dirty += pool[i] != 0;
      if (dirty)
      {
         printf("tile_test: the bin pool has %u words left after the clear\n", dirty);
         validation_errors++;
      }
   }
   {
      ID3D12Resource *all[] = { cb, tb, db, fb, sb, rb, bb, ab, ob, bins_back, frame_back, time_back, outImage, dummy, dummy_up };
      for (i = 0; i < sizeof(all) / sizeof(all[0]); i++)
         ID3D12Resource_Release(all[i]);
   }
   free(gf);
   return t1 - t0;
}
