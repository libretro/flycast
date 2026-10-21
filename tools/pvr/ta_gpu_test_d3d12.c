/* ta_gpu_test_d3d12.exe DIR: GPU-side TA processing's decode in Direct3D 12,
 * on the decodes ta_gpu_test dumped to DIR (ta_gpu_test_replay.c), as the
 * Direct3D 12 renderers record it (rend/dx12/dx12_ta_decoder.cpp): the
 * kernels as the core has them (rend/dx12/dx12_ta_dxbc.inc), root
 * descriptors, the frame's data in host-cached memory of a custom heap,
 * the dispatches' arguments copied out of the frame's words, the sorted
 * indices in write-combined memory of a custom heap that the prepass's
 * kernel writes in place. Each decode's outputs, as far as
 * ta_gpu_test_cpu.cpp looks at them, are read back (a readback for the
 * test, which the renderers never do) and written to cNNNNNN.out; how many
 * errors there were to DIR/errors. Built with mingw-w64 and run under wine
 * with vkd3d-proton, or on Windows: see ta_gpu_test_d3d12.sh. */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "ta_gpu.h"
#include "ta_gpu_test.h"
#include "dx12/dx12_ta_dxbc.inc"

typedef HRESULT (WINAPI *create_device_fn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
typedef HRESULT (WINAPI *serialize_fn)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);

static int errors;
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("ta_gpu_test_d3d12: %s: %08lx\n", #x, (unsigned long)hr_); exit(2); } } while (0)

enum { B_VTX, B_MOD, B_IDX, B_WORK, B_REC, B_CMD, B_FRM, B_TRD, B_TRP, B_OUTPUTS,
   B_ARGS = B_OUTPUTS, B_FRM0, B_ZERO, B_COUNT };
/* (ta_gpu_test_replay.c's numbers for what is written out: the outputs', then the sorted indices') */
enum { R_SRT = 9 };

static ID3D12Device *device;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *cl;
static ID3D12Fence *fence;
static UINT64 fence_value;
static ID3D12RootSignature *root;
static ID3D12PipelineState *kernels[TAG_KERNELS + 1];
static ID3D12CommandSignature *dispatch_sig;
static ID3D12Resource *bufs[B_COUNT], *pool, *tables, *sorted, *readback, *upload;
static D3D12_RESOURCE_STATES states[B_COUNT];
static UINT64 sizes[B_COUNT];
static unsigned char *pool_map, *tables_map, *sorted_map, *readback_map, *upload_map;
static UINT64 readback_size;
static D3D12_RESOURCE_STATES sorted_state = D3D12_RESOURCE_STATE_COMMON;
static struct tgt_sizes sz;
static unsigned layout[13], work_words;

static ID3D12Resource *make_buffer(D3D12_HEAP_TYPE type, D3D12_CPU_PAGE_PROPERTY page, UINT64 bytes, D3D12_RESOURCE_FLAGS flags,
      D3D12_RESOURCE_STATES state)
{
   D3D12_HEAP_PROPERTIES hp;
   D3D12_RESOURCE_DESC d;
   ID3D12Resource *r;
   memset(&hp, 0, sizeof(hp));
   hp.Type = type;
   if (type == D3D12_HEAP_TYPE_CUSTOM)
   {
      hp.CPUPageProperty = page;
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

static void *map(ID3D12Resource *r)
{
   void *p;
   CHECK(ID3D12Resource_Map(r, 0, NULL, &p));
   return p;
}

static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES *from, D3D12_RESOURCE_STATES to)
{
   D3D12_RESOURCE_BARRIER b;
   if (*from == to)
      return;
   memset(&b, 0, sizeof(b));
   b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
   b.Transition.pResource = r;
   b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
   b.Transition.StateBefore = *from;
   b.Transition.StateAfter = to;
   ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
   *from = to;
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

static void dispatch(unsigned k, unsigned groups)
{
   ID3D12GraphicsCommandList_SetPipelineState(cl, kernels[k]);
   if (groups != 0)
      ID3D12GraphicsCommandList_Dispatch(cl, groups, 1, 1);
   uav_barrier();
}

static void indirect(unsigned k, unsigned at)
{
   ID3D12GraphicsCommandList_SetPipelineState(cl, kernels[k]);
   ID3D12GraphicsCommandList_ExecuteIndirect(cl, dispatch_sig, 1, bufs[B_ARGS], (UINT64)(at - TAG_O_ARGS64) * 4, NULL, 0);
   uav_barrier();
}

static void loop(unsigned k, unsigned count)
{
   const unsigned groups = count < 65535u ? count : 65535u;
   ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(cl, 0, groups, 0);
   dispatch(k, groups);
}

static void init(const char *dir)
{
   char path[1024];
   FILE *f;
   HMODULE lib;
   create_device_fn create;
   serialize_fn serialize;
   D3D12_COMMAND_QUEUE_DESC qd;
   D3D12_ROOT_PARAMETER params[13];
   D3D12_ROOT_SIGNATURE_DESC rd;
   D3D12_INDIRECT_ARGUMENT_DESC arg;
   D3D12_COMMAND_SIGNATURE_DESC sig;
   ID3DBlob *blob, *err = NULL;
   unsigned i, strips;
   int b;

   snprintf(path, sizeof(path), "%s/init", dir);
   f = fopen(path, "rb");
   if (!f || fread(&sz, sizeof(sz), 1, f) != 1 || fread(layout, sizeof(layout), 1, f) != 1 || fread(&work_words, 4, 1, f) != 1)
   {
      printf("ta_gpu_test_d3d12: no %s\n", path);
      exit(2);
   }
   fclose(f);

   lib = LoadLibraryA("d3d12.dll");
   create = lib ? (create_device_fn)(void *)GetProcAddress(lib, "D3D12CreateDevice") : NULL;
   serialize = lib ? (serialize_fn)(void *)GetProcAddress(lib, "D3D12SerializeRootSignature") : NULL;
   if (!create || !serialize || FAILED(create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device)))
   {
      printf("ta_gpu_test_d3d12: no Direct3D 12 device\n");
      exit(2);
   }
   memset(&qd, 0, sizeof(qd));
   CHECK(ID3D12Device_CreateCommandQueue(device, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
   CHECK(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
   CHECK(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
   CHECK(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));

   // the renderer's root signature (dx12_ta_decoder.cpp)
   memset(params, 0, sizeof(params));
   params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
   params[0].Constants.Num32BitValues = 1;
   for (i = 0; i < 2; i++)
   {
      params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
      params[1 + i].Descriptor.ShaderRegister = i;
   }
   for (i = 0; i < 10; i++)
   {
      params[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      params[3 + i].Descriptor.ShaderRegister = i;
   }
   memset(&rd, 0, sizeof(rd));
   rd.NumParameters = 13;
   rd.pParameters = params;
   if (FAILED(serialize(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
   {
      printf("ta_gpu_test_d3d12: root signature: %s\n", err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "");
      exit(2);
   }
   CHECK(ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
         &IID_ID3D12RootSignature, (void **)&root));
   for (i = 1; i <= TAG_KERNELS; i++)
   {
      D3D12_COMPUTE_PIPELINE_STATE_DESC cs;
      memset(&cs, 0, sizeof(cs));
      cs.pRootSignature = root;
      cs.CS.pShaderBytecode = dx12_ta_dxbc[i];
      cs.CS.BytecodeLength = dx12_ta_dxbc_size[i];
      CHECK(ID3D12Device_CreateComputePipelineState(device, &cs, &IID_ID3D12PipelineState, (void **)&kernels[i]));
   }
   memset(&arg, 0, sizeof(arg));
   arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
   memset(&sig, 0, sizeof(sig));
   sig.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
   sig.NumArgumentDescs = 1;
   sig.pArgumentDescs = &arg;
   CHECK(ID3D12Device_CreateCommandSignature(device, &sig, NULL, &IID_ID3D12CommandSignature, (void **)&dispatch_sig));

   strips = sz.list[0] + sz.list[1] + sz.list[2];
   sizes[B_VTX] = (UINT64)sz.verts * TAG_V_WORDS * 4;
   sizes[B_MOD] = (UINT64)sz.modtrig * 9 * 4 + 4;
   sizes[B_IDX] = (UINT64)sz.idx * 4;
   sizes[B_WORK] = (UINT64)work_words * 4;
   sizes[B_REC] = (UINT64)strips * TAG_C_WORDS * 4;
   sizes[B_CMD] = (UINT64)sz.headers * 2 * TAG_C_WORDS * 4;
   sizes[B_FRM] = TAG_O_WORDS * 4;
   sizes[B_TRD] = (UINT64)(sz.verts + sz.headers) * TAG_C_WORDS * 4;
   sizes[B_TRP] = (UINT64)sz.list[2] * TAG_P_WORDS * 4;
   sizes[B_ARGS] = 12 * 4;
   sizes[B_FRM0] = TAG_O_WORDS * 4;
   sizes[B_ZERO] = sizes[B_TRD] > (UINT64)sz.headers * 4 ? sizes[B_TRD] : (UINT64)sz.headers * 4;
   for (b = 0; b < B_COUNT; b++)
   {
      states[b] = b < B_OUTPUTS ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST;
      bufs[b] = make_buffer(D3D12_HEAP_TYPE_DEFAULT, 0, sizes[b], b < B_OUTPUTS ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE,
            states[b]);
   }
   pool = make_buffer(D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, (UINT64)sz.ta_words * 4, D3D12_RESOURCE_FLAG_NONE,
         D3D12_RESOURCE_STATE_COMMON);
   pool_map = (unsigned char *)map(pool);
   tables = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 0, (UINT64)sz.table_words * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
   tables_map = (unsigned char *)map(tables);
   sorted = make_buffer(D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE, (UINT64)sz.idx * 6 * 4,
         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
   sorted_map = (unsigned char *)map(sorted);
   readback_size = 0;
   for (b = 0; b < B_OUTPUTS; b++)
      readback_size += sizes[b];
   readback_size += (UINT64)sz.idx * 6 * 4;
   {
      D3D12_HEAP_PROPERTIES hp;
      D3D12_RESOURCE_DESC d;
      memset(&hp, 0, sizeof(hp));
      hp.Type = D3D12_HEAP_TYPE_READBACK;
      memset(&d, 0, sizeof(d));
      d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      d.Width = readback_size;
      d.Height = 1;
      d.DepthOrArraySize = 1;
      d.MipLevels = 1;
      d.SampleDesc.Count = 1;
      d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      CHECK(ID3D12Device_CreateCommittedResource(device, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&readback));
   }
   readback_map = (unsigned char *)map(readback);
   upload = make_buffer(D3D12_HEAP_TYPE_UPLOAD, 0, TAG_O_WORDS * 4, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
   upload_map = (unsigned char *)map(upload);
   memset(upload_map, 0, TAG_O_WORDS * 4);
   ((unsigned *)upload_map)[TAG_O_ZMAX] = 0x3F800000u;
   ID3D12GraphicsCommandList_CopyBufferRegion(cl, bufs[B_FRM0], 0, upload, 0, TAG_O_WORDS * 4);
   transition(bufs[B_FRM0], &states[B_FRM0], D3D12_RESOURCE_STATE_COPY_SOURCE);
   transition(bufs[B_ARGS], &states[B_ARGS], D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
   transition(bufs[B_ZERO], &states[B_ZERO], D3D12_RESOURCE_STATE_COPY_SOURCE);
   run();
}

/* One decode: as DX12TaDecoder::Record() does it */
static void decode(const unsigned *head, const unsigned char *ta, const unsigned *table, const unsigned *srt)
{
   const unsigned trd = head[2], srt_words = head[3], checks = head[4];
   const unsigned nv = table[TAG_T_NVERTS], nmod = table[TAG_T_NMOD], nslot = table[TAG_T_NSLOT], nshd = table[TAG_T_NSHD];
   const unsigned scan_groups = (nv + TAG_SCAN_N - 1u) / TAG_SCAN_N;
   int b;

   memcpy(pool_map, ta, head[0]);
   memcpy(tables_map, table, (size_t)head[1] * 4);
   for (b = 0; b < B_OUTPUTS; b++)
      transition(bufs[b], &states[b], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   transition(sorted, &sorted_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   transition(bufs[B_FRM], &states[B_FRM], D3D12_RESOURCE_STATE_COPY_DEST);
   if (nslot != 0)
      transition(bufs[B_WORK], &states[B_WORK], D3D12_RESOURCE_STATE_COPY_DEST);
   if (trd != 0)
      transition(bufs[B_TRD], &states[B_TRD], D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12GraphicsCommandList_CopyBufferRegion(cl, bufs[B_FRM], 0, bufs[B_FRM0], 0, TAG_O_WORDS * 4);
   if (nslot != 0)
      ID3D12GraphicsCommandList_CopyBufferRegion(cl, bufs[B_WORK], (UINT64)layout[9] * 4, bufs[B_ZERO], 0, (UINT64)nslot * 4);
   if (trd != 0)
      ID3D12GraphicsCommandList_CopyBufferRegion(cl, bufs[B_TRD], 0, bufs[B_ZERO], 0, (UINT64)trd * TAG_C_WORDS * 4);
   transition(bufs[B_FRM], &states[B_FRM], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   transition(bufs[B_WORK], &states[B_WORK], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   transition(bufs[B_TRD], &states[B_TRD], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

   ID3D12GraphicsCommandList_SetComputeRootSignature(cl, root);
   ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(cl, 0, 0, 0);
   ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(pool));
   ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cl, 2, ID3D12Resource_GetGPUVirtualAddress(tables));
   for (b = 0; b < B_OUTPUTS; b++)
      ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 3 + b, ID3D12Resource_GetGPUVirtualAddress(bufs[b]));
   if (checks != 0)
   {
      memcpy(sorted_map, srt, (size_t)srt_words * 4);
      ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 12, ID3D12Resource_GetGPUVirtualAddress(sorted));
   }

   dispatch(TAG_K_UNPACK, (nv + nmod + 63u) / 64u);
   if (checks != 0)
      loop(TAG_K_PREPASS, checks);
   dispatch(TAG_K_COUNT, scan_groups);
   dispatch(TAG_K_COUNTSUM, 1);
   transition(bufs[B_FRM], &states[B_FRM], D3D12_RESOURCE_STATE_COPY_SOURCE);
   transition(bufs[B_ARGS], &states[B_ARGS], D3D12_RESOURCE_STATE_COPY_DEST);
   ID3D12GraphicsCommandList_CopyBufferRegion(cl, bufs[B_ARGS], 0, bufs[B_FRM], TAG_O_ARGS64 * 4, 9 * 4);
   transition(bufs[B_FRM], &states[B_FRM], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
   transition(bufs[B_ARGS], &states[B_ARGS], D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
   dispatch(TAG_K_ENDS, scan_groups);
   indirect(TAG_K_SUMMARY, TAG_O_ARGS64);
   indirect(TAG_K_GROUPS, TAG_O_ARGSCH);
   dispatch(TAG_K_GROUPSUM, 1);
   indirect(TAG_K_STATES, TAG_O_ARGSCH);
   indirect(TAG_K_EMIT, TAG_O_ARGS64);
   dispatch(TAG_K_FINISH, (nslot + 63u) / 64u);
   if (nshd != 0)
      loop(TAG_K_SHADOW, nshd);
   if (trd != 0)
      indirect(TAG_K_TRDRAW, TAG_O_ARGS64);
   if (table[TAG_T_FLAGS] & TAG_FLAG_BLEED)
      indirect(TAG_K_BLEED, TAG_O_ARGS1);
   if (table[TAG_T_FLAGS] & TAG_FLAG_PROVOKE)
      indirect(TAG_K_PROVOKE, TAG_O_ARGS1);
   run();
}

/* What ta_gpu_test_cpu.cpp looks at: each piece copied to the readback
 * buffer and written out (buffer, first word, words, the words) */
struct piece { unsigned buf, first, words; };

static void write_out(const char *path, const unsigned *table, const unsigned *head)
{
   struct piece p[16];
   unsigned n = 0, i, l;
   UINT64 at = 0, offsets[16];
   const unsigned *frm;
   FILE *f;
   int b;

   for (b = 0; b < B_OUTPUTS; b++)
      transition(bufs[b], &states[b], D3D12_RESOURCE_STATE_COPY_SOURCE);
   transition(sorted, &sorted_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
   // the frame's words first: they say how much of the rest there is
   ID3D12GraphicsCommandList_CopyBufferRegion(cl, readback, 0, bufs[B_FRM], 0, TAG_O_WORDS * 4);
   run();
   frm = (const unsigned *)readback_map;
   {
      const unsigned nv = table[TAG_T_NVERTS], nslot = table[TAG_T_NSLOT];
      const unsigned passes = table[TAG_T_NPASS] != 0 ? table[TAG_T_NPASS] : 1;
      const unsigned last = TAG_O_PASS + (passes - 1) * 3;
      const unsigned nidx = frm[TAG_O_NIDX] < table[TAG_T_ROOM_IDX] ? frm[TAG_O_NIDX] : table[TAG_T_ROOM_IDX];
      const unsigned rec_base[3] = { table[TAG_T_REC_OP], table[TAG_T_REC_PT], table[TAG_T_REC_TR] };
      const unsigned rooms[3] = { table[TAG_T_ROOM_OP], table[TAG_T_ROOM_PT], table[TAG_T_ROOM_TR] };
      unsigned frm_copy[TAG_O_WORDS];
      memcpy(frm_copy, frm, sizeof(frm_copy));
      p[n].buf = B_FRM; p[n].first = 0; p[n].words = TAG_O_WORDS; n++;
      p[n].buf = B_VTX; p[n].first = 0; p[n].words = nv * TAG_V_WORDS; n++;
      p[n].buf = B_MOD; p[n].first = 0; p[n].words = table[TAG_T_NMOD] * 9; n++;
      p[n].buf = B_IDX; p[n].first = 0; p[n].words = nidx; n++;
      for (l = 0; l < 3; l++)
      {
         unsigned c = frm_copy[last + l] < rooms[l] ? frm_copy[last + l] : rooms[l];
         p[n].buf = B_REC; p[n].first = rec_base[l]; p[n].words = c * TAG_C_WORDS; n++;
      }
      p[n].buf = B_CMD; p[n].first = 0; p[n].words = nslot * TAG_C_WORDS; n++;
      p[n].buf = B_CMD; p[n].first = table[TAG_T_SHD_CMD] * TAG_C_WORDS; p[n].words = table[TAG_T_NSHD] != 0 ? nslot * TAG_C_WORDS : 0; n++;
      p[n].buf = B_WORK; p[n].first = table[TAG_T_W_SSTATE]; p[n].words = frm_copy[TAG_O_NSTRIPS] * TAG_ST_WORDS; n++;
      p[n].buf = B_TRD; p[n].first = 0; p[n].words = head[2] * TAG_C_WORDS; n++;
      {
         unsigned c = frm_copy[last + 2] < rooms[2] ? frm_copy[last + 2] : rooms[2];
         p[n].buf = B_TRP; p[n].first = 0; p[n].words = c * TAG_P_WORDS; n++;
      }
      p[n].buf = R_SRT; p[n].first = 0; p[n].words = head[4] != 0 ? head[3] : 0; n++;
   }
   for (i = 0; i < n; i++)
   {
      offsets[i] = at;
      if (p[i].words == 0)
         continue;
      ID3D12GraphicsCommandList_CopyBufferRegion(cl, readback, at, p[i].buf == R_SRT ? sorted : bufs[p[i].buf],
            (UINT64)p[i].first * 4, (UINT64)p[i].words * 4);
      at += (UINT64)p[i].words * 4;
   }
   run();
   f = fopen(path, "wb");
   if (!f)
   {
      printf("ta_gpu_test_d3d12: cannot write %s\n", path);
      exit(2);
   }
   fwrite(head, 4, 5, f);
   for (i = 0; i < n; i++)
   {
      unsigned rec[3];
      if (p[i].words == 0)
         continue;
      rec[0] = p[i].buf;
      rec[1] = p[i].first;
      rec[2] = p[i].words;
      fwrite(rec, 4, 3, f);
      fwrite(readback_map + offsets[i], 4, p[i].words, f);
   }
   {
      const unsigned end[3] = { 0xFFFFFFFFu, 0, 0 };
      fwrite(end, 4, 3, f);
   }
   fclose(f);
   for (b = 0; b < B_OUTPUTS; b++)
      transition(bufs[b], &states[b], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

int main(int argc, char **argv)
{
   char path[1024];
   unsigned call;
   unsigned char *ta = NULL;
   unsigned *table = NULL, *srt = NULL;
   FILE *f;

   setvbuf(stdout, NULL, _IONBF, 0);
   if (argc != 2)
   {
      printf("usage: ta_gpu_test_d3d12 DIR\n");
      return 2;
   }
   init(argv[1]);
   ta = (unsigned char *)malloc((size_t)sz.ta_words * 4);
   table = (unsigned *)malloc((size_t)sz.table_words * 4);
   srt = (unsigned *)malloc((size_t)sz.idx * 6 * 4);
   for (call = 0;; call++)
   {
      unsigned head[5];
      snprintf(path, sizeof(path), "%s/c%06u.in", argv[1], call);
      f = fopen(path, "rb");
      if (!f)
         break;
      if (fread(head, 4, 5, f) != 5 || head[0] > sz.ta_words * 4 || head[1] > sz.table_words || head[3] > sz.idx * 6
            || fread(ta, 1, head[0], f) != head[0] || fread(table, 4, head[1], f) != head[1]
            || (head[3] != 0 && fread(srt, 4, head[3], f) != head[3]))
      {
         printf("ta_gpu_test_d3d12: %s is cut short\n", path);
         errors++;
         fclose(f);
         break;
      }
      fclose(f);
      decode(head, ta, table, srt);
      snprintf(path, sizeof(path), "%s/c%06u.out", argv[1], call);
      write_out(path, table, head);
   }
   printf("ta_gpu_test_d3d12: %u decodes\n", call);
   snprintf(path, sizeof(path), "%s/errors", argv[1]);
   f = fopen(path, "w");
   if (f)
   {
      fprintf(f, "%d\n", errors);
      fclose(f);
   }
   return errors != 0;
}
