/* dxbc_compile.exe COMPILER SOURCE PROFILE OUT
 *
 * HLSL to DXBC with Microsoft's d3dcompiler_47.dll (COMPILER, its path),
 * entry point main, optimised: what tools/rend/kernels_dxbc.sh makes the
 * Direct3D 11 renderers' kernels with. Built with mingw-w64 and run under
 * wine on a machine that is not Windows. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct blob_vtbl blob_vtbl;
typedef struct blob { const blob_vtbl *v; } blob;
struct blob_vtbl
{
   HRESULT (WINAPI *QueryInterface)(blob *, const void *, void **);
   ULONG (WINAPI *AddRef)(blob *);
   ULONG (WINAPI *Release)(blob *);
   void *(WINAPI *GetBufferPointer)(blob *);
   SIZE_T (WINAPI *GetBufferSize)(blob *);
};
typedef HRESULT (WINAPI *compile_fn)(const void *src, SIZE_T size, const char *name, const void *defines, void *include,
      const char *entry, const char *target, UINT flags1, UINT flags2, blob **code, blob **errors);

#define OPTIMIZATION_LEVEL3 (1u << 15)

int main(int argc, char **argv)
{
   HMODULE dll;
   compile_fn compile;
   FILE *f;
   char *src;
   long n;
   blob *code = NULL, *errors = NULL;
   HRESULT hr;

   if (argc != 5)
   {
      fprintf(stderr, "usage: dxbc_compile COMPILER SOURCE PROFILE OUT\n");
      return 2;
   }
   dll = LoadLibraryA(argv[1]);
   compile = dll ? (compile_fn)(void *)GetProcAddress(dll, "D3DCompile") : NULL;
   if (!compile)
   {
      fprintf(stderr, "dxbc_compile: no D3DCompile in %s\n", argv[1]);
      return 2;
   }
   f = fopen(argv[2], "rb");
   if (!f)
      return 2;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   src = (char *)malloc((size_t)n + 1);
   if (!src || fread(src, 1, (size_t)n, f) != (size_t)n)
      return 2;
   fclose(f);
   hr = compile(src, (SIZE_T)n, "texdecode.h", NULL, NULL, "main", argv[3], OPTIMIZATION_LEVEL3, 0, &code, &errors);
   if (errors)
   {
      fwrite(errors->v->GetBufferPointer(errors), 1, errors->v->GetBufferSize(errors), stderr);
      errors->v->Release(errors);
   }
   if (FAILED(hr) || !code)
   {
      fprintf(stderr, "dxbc_compile: %s failed (%lx)\n", argv[3], (unsigned long)hr);
      return 1;
   }
   f = fopen(argv[4], "wb");
   if (!f || fwrite(code->v->GetBufferPointer(code), 1, code->v->GetBufferSize(code), f) != code->v->GetBufferSize(code))
      return 2;
   fclose(f);
   code->v->Release(code);
   return 0;
}
