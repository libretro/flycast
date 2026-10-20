/* The core's uses of retro_atomic.h's C++11 backend, in their shape:
 * EmuBaton's unnamed enumerators and PsoState through init and the
 * stores, counters through the fetch macros.  tools/threads/old_stl.sh
 * parses this against old_stl/atomic, the <atomic> of libstdc++ 8,
 * whose free functions deduce the value parameter too; a macro that
 * hands the value through uncast fails there (issue #1215). */

#include <cstdio>
#include <cstddef>
#include <retro_atomic.h>

#if !defined(RETRO_ATOMIC_BACKEND_CXX11)
#error "this test is for the C++11 backend"
#endif

enum PsoState
{
   PSO_EMPTY = 0,
   PSO_QUEUED,
   PSO_READY,
   PSO_FAILED
};

struct Baton
{
   enum { NONE = 0, HELD, FRAME, EXIT };
   retro_atomic_int_t  state;
   retro_atomic_int_t  acked;
   retro_atomic_ptr_t  work;
   retro_atomic_size_t count;

   Baton()
   {
      retro_atomic_int_init(&state, NONE);
      retro_atomic_ptr_init(&work, NULL);
      retro_atomic_int_init(&acked, 0);
      retro_atomic_size_init(&count, FRAME);
   }
};

static int fails = 0;

static void want(const char *what, long got, long exp)
{
   if (got == exp)
      printf("ok    %s\n", what);
   else
   {
      printf("FAIL  %s: got %ld, want %ld\n", what, got, exp);
      fails++;
   }
}

int main(void)
{
   Baton b;
   retro_atomic_int_t pso;
   unsigned u = 1u << 4;
   int work   = 0;

   retro_atomic_int_init(&pso, PSO_EMPTY);
   want("baton init from unnamed enumerator",
         retro_atomic_load_acquire_int(&b.state), Baton::NONE);
   want("pso init from enumerator",
         retro_atomic_load_relaxed_int(&pso), PSO_EMPTY);
   want("size init from enumerator",
         (long)retro_atomic_load_acquire_size(&b.count), Baton::FRAME);

   retro_atomic_store_release_int(&b.state, Baton::HELD);
   want("store_release from unnamed enumerator",
         retro_atomic_load_acquire_int(&b.state), Baton::HELD);
   retro_atomic_store_relaxed_int(&pso, PSO_QUEUED);
   want("store_relaxed from enumerator",
         retro_atomic_load_relaxed_int(&pso), PSO_QUEUED);
   retro_atomic_store_release_int(&pso, u ? PSO_READY : PSO_FAILED);
   want("store_release from conditional enumerator",
         retro_atomic_load_seq_cst_int(&pso), PSO_READY);

   want("fetch_add of unsigned",
         retro_atomic_fetch_add_int(&b.acked, u), 0);
   want("fetch_add_seq_cst of enumerator",
         retro_atomic_fetch_add_seq_cst_int(&b.acked, Baton::HELD), (long)u);
   want("fetch_sub of enumerator",
         retro_atomic_fetch_sub_int(&b.acked, Baton::HELD), (long)u + 1);
   want("fetch_or of unsigned",
         retro_atomic_fetch_or_int(&b.acked, 1u), (long)u);
   want("fetch_and of unsigned",
         retro_atomic_fetch_and_int(&b.acked, ~u), (long)u + 1);
   want("acked after or and and",
         retro_atomic_load_acquire_int(&b.acked), 1);

   retro_atomic_store_release_size(&b.count, Baton::NONE);
   want("size fetch_add of enumerator",
         (long)retro_atomic_fetch_add_size(&b.count, Baton::FRAME), Baton::NONE);
   want("size fetch_sub of enumerator",
         (long)retro_atomic_fetch_sub_size(&b.count, Baton::HELD), Baton::FRAME);

   want("exchange from unnamed enumerator",
         retro_atomic_exchange_int(&b.state, Baton::EXIT), Baton::HELD);
   want("cas from unnamed enumerators",
         retro_atomic_cas_int(&b.state, Baton::EXIT, Baton::NONE), 1);
   want("state after cas",
         retro_atomic_load_acquire_int(&b.state), Baton::NONE);

   retro_atomic_store_release_ptr(&b.work, &work);
   want("ptr round trip",
         retro_atomic_load_acquire_ptr(&b.work) == &work, 1);

   printf("%s: %s\n", RETRO_ATOMIC_BACKEND_NAME, fails ? "FAIL" : "all ok");
   return fails ? 1 : 0;
}
