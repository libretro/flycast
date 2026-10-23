/* ta_gpu_test_d3d11.exe DIR: GPU-side TA processing's decode in Direct3D 11,
 * on the decodes ta_gpu_test dumped to DIR (ta_gpu_test_replay.c), as the
 * Direct3D 11 renderers run it (rend/dx11/dx11_ta_decoder.cpp): the
 * kernels as the core has them (rend/dx11/dx11_ta_dxbc.inc), typed views
 * of 32-bit words for every buffer, with the bindings the renderers give
 * them (vertex, index, indirect arguments), the frame's tables and data in
 * a dynamic buffer - the data at an offset into it (TAG_T_TA_BASE) every
 * other decode, as the renderers' ring puts them - the dispatches that go
 * by strips DispatchIndirect() of a copy of the frame's words, the sorted
 * indices given to their buffer with UpdateSubresource() and changed in
 * place by the prepass's kernel, eight views bound and the two kernels'
 * own at u1. Not NVAPI's multi-draw, nor its UAV
 * overlap: what the draws are given, the commands and the translucent
 * draw records, is what is compared. Each decode's outputs, as far as
 * ta_gpu_test_cpu.cpp looks at them, are read back (a readback for the
 * test, which the renderers never do) and written to cNNNNNN.out; how many
 * errors there were to DIR/errors. Built with mingw-w64 and run under wine,
 * or on Windows: see ta_gpu_test_d3d11.sh. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "ta_gpu.h"
#include "ta_gpu_test.h"
#include "dx11/dx11_ta_dxbc.inc"

static int errors;
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("ta_gpu_test_d3d11: %s: %08lx\n", #x, (unsigned long)hr_); exit(2); } } while (0)

enum { B_VTX, B_MOD, B_IDX, B_WORK, B_REC, B_CMD, B_FRM, B_TRD, B_TRP, B_OUTPUTS,
   B_ARGS = B_OUTPUTS, B_FRM0, B_ZERO, B_SORT, B_COUNT };
/* (ta_gpu_test_replay.c's numbers for what is written out: the outputs', then the sorted indices') */
enum { R_SRT = 9 };

static ID3D11Device *device;
static ID3D11DeviceContext *dc;
static ID3D11ComputeShader *kernels[TAG_KERNELS + 1];
static ID3D11Buffer *bufs[B_COUNT], *input, *groups_cb, *readback;
/* (u0 to u7 bound, the polygon words and the sorted indices at u1 for the
 * kernel that writes them: see dx11_ta_decoder.cpp) */
enum { UAV_SLOTS = 8 };
static ID3D11UnorderedAccessView *uavs[B_OUTPUTS], *sort_uav;
static ID3D11ShaderResourceView *table_view, *ring_view;
static UINT64 sizes[B_COUNT];
static UINT64 readback_size;
static unsigned table_bytes, ring_bytes, groups_set = 0xFFFFFFFFu;
static struct tgt_sizes sz;
static unsigned layout[13], work_words;

static ID3D11Buffer *make_buffer(UINT64 bytes, UINT bind, UINT misc, D3D11_USAGE usage, const void *data)
{
   D3D11_BUFFER_DESC d;
   D3D11_SUBRESOURCE_DATA init;
   ID3D11Buffer *b;
   memset(&d, 0, sizeof(d));
   d.ByteWidth = (UINT)(bytes < 16 ? 16 : (bytes + 15) & ~(UINT64)15);
   d.Usage = usage;
   d.BindFlags = bind;
   d.CPUAccessFlags = usage == D3D11_USAGE_DYNAMIC ? D3D11_CPU_ACCESS_WRITE : usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ : 0;
   d.MiscFlags = misc;
   memset(&init, 0, sizeof(init));
   init.pSysMem = data;
   CHECK(ID3D11Device_CreateBuffer(device, &d, data ? &init : NULL, &b));
   return b;
}

static ID3D11ShaderResourceView *make_srv(ID3D11Buffer *b, unsigned first, unsigned count)
{
   D3D11_SHADER_RESOURCE_VIEW_DESC d;
   ID3D11ShaderResourceView *v;
   memset(&d, 0, sizeof(d));
   d.Format = DXGI_FORMAT_R32_UINT;
   d.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
   d.Buffer.FirstElement = first;
   d.Buffer.NumElements = count ? count : 1;
   CHECK(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)b, &d, &v));
   return v;
}

static ID3D11UnorderedAccessView *make_uav(ID3D11Buffer *b, UINT64 bytes)
{
   D3D11_UNORDERED_ACCESS_VIEW_DESC d;
   ID3D11UnorderedAccessView *v;
   memset(&d, 0, sizeof(d));
   d.Format = DXGI_FORMAT_R32_UINT;
   d.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
   d.Buffer.NumElements = (UINT)(bytes / 4 ? bytes / 4 : 1);
   CHECK(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource *)b, &d, &v));
   return v;
}

static void copy(ID3D11Buffer *dst, unsigned dst_at, ID3D11Buffer *src, unsigned from, unsigned bytes)
{
   D3D11_BOX box;
   box.left = from;
   box.right = from + bytes;
   box.top = box.front = 0;
   box.bottom = box.back = 1;
   ID3D11DeviceContext_CopySubresourceRegion(dc, (ID3D11Resource *)dst, 0, dst_at, 0, 0, (ID3D11Resource *)src, 0, &box);
}

static void set_groups(unsigned groups)
{
   unsigned words[4] = { 0, 0, 0, 0 };
   if (groups == groups_set)
      return;
   words[0] = groups;
   ID3D11DeviceContext_UpdateSubresource(dc, (ID3D11Resource *)groups_cb, 0, NULL, words, 0, 0);
   groups_set = groups;
}

static void dispatch(unsigned k, unsigned groups)
{
   if (groups == 0)
      return;
   ID3D11DeviceContext_CSSetShader(dc, kernels[k], NULL, 0);
   ID3D11DeviceContext_Dispatch(dc, groups, 1, 1);
}

static void indirect(unsigned k, unsigned at)
{
   ID3D11DeviceContext_CSSetShader(dc, kernels[k], NULL, 0);
   ID3D11DeviceContext_DispatchIndirect(dc, bufs[B_ARGS], (at - TAG_O_ARGS64) * 4);
}

static void loop(unsigned k, unsigned count)
{
   const unsigned groups = count < 65535u ? count : 65535u;
   set_groups(groups);
   dispatch(k, groups);
}

static void init(const char *dir)
{
   char path[1024];
   FILE *f;
   D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0, got;
   static const UINT bind[B_OUTPUTS] = {
      D3D11_BIND_VERTEX_BUFFER, D3D11_BIND_VERTEX_BUFFER, D3D11_BIND_INDEX_BUFFER, 0, 0, 0, 0, 0, D3D11_BIND_SHADER_RESOURCE
   };
   unsigned i, strips, *words;
   UINT64 most;
   int b;

   snprintf(path, sizeof(path), "%s/init", dir);
   f = fopen(path, "rb");
   if (!f || fread(&sz, sizeof(sz), 1, f) != 1 || fread(layout, sizeof(layout), 1, f) != 1 || fread(&work_words, 4, 1, f) != 1)
   {
      printf("ta_gpu_test_d3d11: no %s\n", path);
      exit(2);
   }
   fclose(f);

   if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &want, 1, D3D11_SDK_VERSION, &device, &got, &dc)))
   {
      printf("ta_gpu_test_d3d11: no Direct3D 11 device of feature level 11.0\n");
      exit(2);
   }
   for (i = 1; i <= TAG_KERNELS; i++)
      CHECK(ID3D11Device_CreateComputeShader(device, dx11_ta_dxbc[i], dx11_ta_dxbc_size[i], NULL, &kernels[i]));

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
   sizes[B_SORT] = ((UINT64)sz.idx * 6 + 1 + (UINT64)sz.list[2] * TAG_PC_WORDS) * 4;
   for (b = 0; b < B_OUTPUTS; b++)
   {
      bufs[b] = make_buffer(sizes[b], bind[b] | D3D11_BIND_UNORDERED_ACCESS,
            b == B_CMD || b == B_TRD ? D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS : 0, D3D11_USAGE_DEFAULT, NULL);
      uavs[b] = make_uav(bufs[b], sizes[b]);
   }
   bufs[B_ARGS] = make_buffer(sizes[B_ARGS], D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, D3D11_USAGE_DEFAULT, NULL);
   most = sizes[B_ZERO] > sizes[B_FRM0] ? sizes[B_ZERO] : sizes[B_FRM0];
   words = (unsigned *)calloc((size_t)most / 4 + 4, 4);
   bufs[B_ZERO] = make_buffer(sizes[B_ZERO], D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_IMMUTABLE, words);
   words[TAG_O_ZMAX] = 0x3F800000u;
   bufs[B_FRM0] = make_buffer(sizes[B_FRM0], D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_IMMUTABLE, words);
   free(words);
   bufs[B_SORT] = make_buffer(sizes[B_SORT], D3D11_BIND_INDEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_USAGE_DEFAULT, NULL);
   sort_uav = make_uav(bufs[B_SORT], sizes[B_SORT]);
   {
      static const unsigned zero[4];
      groups_cb = make_buffer(16, D3D11_BIND_CONSTANT_BUFFER, 0, D3D11_USAGE_DEFAULT, zero);
      groups_set = 0;
   }
   /* the input: a piece of tables, then the ring, the frame's data at its
    * start or a megabyte and a bit on */
   table_bytes = ((sz.table_words + 63u) & ~63u) * 4;
   ring_bytes = sz.ta_words * 4 + (1u << 20) + 256;
   input = make_buffer((UINT64)table_bytes + ring_bytes, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_DYNAMIC, NULL);
   table_view = make_srv(input, 0, table_bytes / 4);
   ring_view = make_srv(input, table_bytes / 4, ring_bytes / 4);

   readback_size = 0;
   for (b = 0; b < B_OUTPUTS; b++)
      readback_size += sizes[b];
   readback_size += sizes[B_SORT];
   readback = make_buffer(readback_size, 0, 0, D3D11_USAGE_STAGING, NULL);
}

/* One decode: as DX11TaDecoder::Prepare() and Record() do it */
static void decode(unsigned call, const unsigned *head, const unsigned char *ta, unsigned *table, const unsigned *srt)
{
   const unsigned trd = head[2], srt_words = head[3], checks = head[4];
   const unsigned nv = table[TAG_T_NVERTS], nmod = table[TAG_T_NMOD], nslot = table[TAG_T_NSLOT], nshd = table[TAG_T_NSHD];
   const unsigned scan_groups = (nv + TAG_SCAN_N - 1u) / TAG_SCAN_N;
   const unsigned at = (call & 1) ? (1u << 20) + 256 : 0;
   ID3D11ShaderResourceView *inputs[2];
   ID3D11UnorderedAccessView *none[UAV_SLOTS];
   ID3D11ShaderResourceView *no_inputs[2] = { NULL, NULL };
   D3D11_MAPPED_SUBRESOURCE m;

   /* the frame's data where the ring has them, and the tables saying so */
   table[TAG_T_TA_BASE] = at / 4;
   CHECK(ID3D11DeviceContext_Map(dc, (ID3D11Resource *)input, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
   memcpy(m.pData, table, (size_t)head[1] * 4);
   memcpy((unsigned char *)m.pData + table_bytes + at, ta, head[0]);
   ID3D11DeviceContext_Unmap(dc, (ID3D11Resource *)input, 0);

   copy(bufs[B_FRM], 0, bufs[B_FRM0], 0, TAG_O_WORDS * 4);
   if (nslot != 0)
      copy(bufs[B_WORK], layout[9] * 4, bufs[B_ZERO], 0, nslot * 4);
   if (trd != 0)
      copy(bufs[B_TRD], 0, bufs[B_ZERO], 0, trd * TAG_C_WORDS * 4);
   if (checks != 0)
   {
      D3D11_BOX part;
      part.left = 0;
      part.right = srt_words * 4;
      part.top = part.front = 0;
      part.bottom = part.back = 1;
      ID3D11DeviceContext_UpdateSubresource(dc, (ID3D11Resource *)bufs[B_SORT], 0, &part, srt, 0, 0);
   }

   inputs[0] = ring_view;
   inputs[1] = table_view;
   ID3D11DeviceContext_CSSetShaderResources(dc, 0, 2, inputs);
   ID3D11DeviceContext_CSSetUnorderedAccessViews(dc, 0, UAV_SLOTS, uavs, NULL);
   set_groups(0);
   ID3D11DeviceContext_CSSetConstantBuffers(dc, 0, 1, &groups_cb);

   dispatch(TAG_K_UNPACK, (nv + nmod + 63u) / 64u);
   if (checks != 0)
   {
      ID3D11DeviceContext_CSSetUnorderedAccessViews(dc, 1, 1, &sort_uav, NULL);
      loop(TAG_K_PREPASS, checks);
   }
   dispatch(TAG_K_COUNT, scan_groups);
   dispatch(TAG_K_COUNTSUM, 1);
   copy(bufs[B_ARGS], 0, bufs[B_FRM], TAG_O_ARGS64 * 4, 9 * 4);
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
   {
      ID3D11DeviceContext_CSSetUnorderedAccessViews(dc, 1, 1, &uavs[B_TRP], NULL);
      indirect(TAG_K_TRDRAW, TAG_O_ARGS64);
   }
   if (table[TAG_T_FLAGS] & TAG_FLAG_BLEED)
      indirect(TAG_K_BLEED, TAG_O_ARGS1);
   if (table[TAG_T_FLAGS] & TAG_FLAG_PROVOKE)
      indirect(TAG_K_PROVOKE, TAG_O_ARGS1);
   memset(none, 0, sizeof(none));
   ID3D11DeviceContext_CSSetUnorderedAccessViews(dc, 0, UAV_SLOTS, none, NULL);
   ID3D11DeviceContext_CSSetShaderResources(dc, 0, 2, no_inputs);
}

/* What ta_gpu_test_cpu.cpp looks at: each piece copied to the readback
 * buffer and written out (buffer, first word, words, the words) */
struct piece { unsigned buf, first, words; };

static void write_out(const char *path, const unsigned *table, const unsigned *head)
{
   struct piece p[16];
   unsigned n = 0, i, l;
   UINT64 at = 0, offsets[16];
   unsigned frm_copy[TAG_O_WORDS];
   D3D11_MAPPED_SUBRESOURCE m;
   FILE *f;

   // the frame's words first: they say how much of the rest there is
   copy(readback, 0, bufs[B_FRM], 0, TAG_O_WORDS * 4);
   CHECK(ID3D11DeviceContext_Map(dc, (ID3D11Resource *)readback, 0, D3D11_MAP_READ, 0, &m));
   memcpy(frm_copy, m.pData, sizeof(frm_copy));
   ID3D11DeviceContext_Unmap(dc, (ID3D11Resource *)readback, 0);
   {
      const unsigned nv = table[TAG_T_NVERTS], nslot = table[TAG_T_NSLOT];
      const unsigned passes = table[TAG_T_NPASS] != 0 ? table[TAG_T_NPASS] : 1;
      const unsigned last = TAG_O_PASS + (passes - 1) * 3;
      const unsigned nidx = frm_copy[TAG_O_NIDX] < table[TAG_T_ROOM_IDX] ? frm_copy[TAG_O_NIDX] : table[TAG_T_ROOM_IDX];
      const unsigned rec_base[3] = { table[TAG_T_REC_OP], table[TAG_T_REC_PT], table[TAG_T_REC_TR] };
      const unsigned rooms[3] = { table[TAG_T_ROOM_OP], table[TAG_T_ROOM_PT], table[TAG_T_ROOM_TR] };
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
      copy(readback, (unsigned)at, p[i].buf == R_SRT ? bufs[B_SORT] : bufs[p[i].buf], p[i].first * 4, p[i].words * 4);
      at += (UINT64)p[i].words * 4;
   }
   CHECK(ID3D11DeviceContext_Map(dc, (ID3D11Resource *)readback, 0, D3D11_MAP_READ, 0, &m));
   f = fopen(path, "wb");
   if (!f)
   {
      printf("ta_gpu_test_d3d11: cannot write %s\n", path);
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
      fwrite((const unsigned char *)m.pData + offsets[i], 4, p[i].words, f);
   }
   {
      const unsigned end[3] = { 0xFFFFFFFFu, 0, 0 };
      fwrite(end, 4, 3, f);
   }
   fclose(f);
   ID3D11DeviceContext_Unmap(dc, (ID3D11Resource *)readback, 0);
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
      printf("usage: ta_gpu_test_d3d11 DIR\n");
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
      if (fread(head, 4, 5, f) != 5 || head[0] > sz.ta_words * 4 || head[1] > sz.table_words || head[1] * 4 > table_bytes
            || head[3] > sz.idx * 6 || fread(ta, 1, head[0], f) != head[0] || fread(table, 4, head[1], f) != head[1]
            || (head[3] != 0 && fread(srt, 4, head[3], f) != head[3]))
      {
         printf("ta_gpu_test_d3d11: %s is cut short\n", path);
         errors++;
         fclose(f);
         break;
      }
      fclose(f);
      decode(call, head, ta, table, srt);
      snprintf(path, sizeof(path), "%s/c%06u.out", argv[1], call);
      write_out(path, table, head);
   }
   printf("ta_gpu_test_d3d11: %u decodes\n", call);
   snprintf(path, sizeof(path), "%s/errors", argv[1]);
   f = fopen(path, "w");
   if (f)
   {
      fprintf(f, "%d\n", errors);
      fclose(f);
   }
   return errors != 0;
}
