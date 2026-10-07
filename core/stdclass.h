#pragma once
#include "types.h"
#include <stdlib.h>
#include <vector>
#include <string.h>

#include <rthreads/rthreads.h>
#ifndef TARGET_NO_THREADS
#include <retro_atomic.h>
#include <rthreads/retro_eventcount.h>
#endif

#ifdef _ANDROID
#include <sys/mman.h>
#else
#define PAGE_SIZE 4096
#endif

#undef PAGE_MASK
#define PAGE_MASK (PAGE_SIZE-1)

//Commonly used classes across the project
//Simple Array class for helping me out ;P
template<class T>
class Array
{
public:
	T* data;
	u32 Size;

	Array(T* Source,u32 ellements)
	{
		//initialise array
		data=Source;
		Size=ellements;
	}

	Array(u32 ellements)
	{
		//initialise array
		data=0;
		Resize(ellements,false);
		Size=ellements;
	}

	Array(u32 ellements,bool zero)
	{
		//initialise array
		data=0;
		Resize(ellements,zero);
		Size=ellements;
	}

	Array()
	{
		//initialise array
		data=0;
		Size=0;
	}

	~Array()
	{
		if  (data)
		{
			#ifdef MEM_ALLOC_TRACE
			printf("WARNING : DESTRUCTOR WITH NON FREED ARRAY [arrayid:%d]\n",id);
			#endif
			Free();
		}
	}

	void SetPtr(T* Source,u32 ellements)
	{
		//initialise array
		Free();
		data=Source;
		Size=ellements;
	}

	T* Resize(u32 size,bool bZero)
	{
		if (size==0)
		{
			if (data)
			{
				#ifdef MEM_ALLOC_TRACE
				printf("Freeing data -> resize to zero[Array:%d]\n",id);
				#endif
				Free();
			}

		}
		
		if (!data)
			data=(T*)malloc(size*sizeof(T));
		else
			data=(T*)realloc(data,size*sizeof(T));

		//TODO : Optimise this
		//if we allocated more , Zero it out
		if (bZero)
		{
			if (size>Size)
			{
				for (u32 i=Size;i<size;i++)
				{
					u8*p =(u8*)&data[i];
					for (size_t j=0;j<sizeof(T);j++)
					{
						p[j]=0;
					}
				}
			}
		}
		Size=size;

		return data;
	}

	void Zero()
	{
      memset(data,0,sizeof(T)*Size);
	}

	void Free()
	{
		if (Size != 0)
		{
			if (data)
				free(data);

			data = NULL;
		}
	}


	INLINE T& operator [](const u32 i)
	{
#ifdef MEM_BOUND_CHECK
		if (i>=Size)
		{
			printf("Error: Array %d , index out of range (%d>%d)\n",id,i,Size-1);
			MEM_DO_BREAK;
		}
#endif
		return data[i];
	}

	INLINE T& operator [](const s32 i)
	{
#ifdef MEM_BOUND_CHECK
		if (!(i>=0 && i<(s32)Size))
		{
			printf("Error: Array %d , index out of range (%d > %d)\n",id,i,Size-1);
			MEM_DO_BREAK;
		}
#endif
		return data[i];
	}
};

//Threads
#if !defined(HOST_NO_THREADS)
typedef  void* ThreadEntryFP(void* param);

typedef void* THREADHANDLE;

class cThread
{
private:
	ThreadEntryFP* Entry;
	void* param;
public :
	sthread_t *hThread;
	cThread(ThreadEntryFP* function,void* param);
	~cThread() { WaitToEnd(); }
	
	void Start();
	void WaitToEnd();
};
#endif

//Wait Events
#ifndef TARGET_NO_THREADS
/* Where a thread sleeps until another one changes something it is waiting
 * for. The waiter checks its own condition between Prepare() and Commit();
 * whoever changes the condition calls Notify() afterwards. Notify() with
 * nobody asleep is two atomic operations: no lock, no system call. */
class cEventCount
{
	retro_eventcount_t ec;

public :
	cEventCount()
	{
		if (!retro_eventcount_init(&ec))
			abort();
	}
	~cEventCount() { retro_eventcount_free(&ec); }

	void Notify() { retro_eventcount_notify(&ec); }
	int Prepare() { return retro_eventcount_prepare_wait(&ec); }
	void Cancel() { retro_eventcount_cancel_wait(&ec); }
	void Commit(int key) { retro_eventcount_commit_wait(&ec, key); }
	//Returns false if the bound expired
	bool Commit(int key, int64_t timeout_us)
	{
		return retro_eventcount_commit_wait_timeout(&ec, key, timeout_us);
	}
};
#endif

/* Auto-reset event. Set() is one store, plus a wake when a thread is
 * asleep in Wait(); nothing here takes a lock. */
class cResetEvent
{
#ifndef TARGET_NO_THREADS
	cEventCount ec;
	retro_atomic_int_t state;

	bool Consume()
	{
		return retro_atomic_load_acquire_int(&state)
			&& retro_atomic_exchange_int(&state, 0);
	}
#else
	bool state;
#endif

public :
	cResetEvent()
	{
#ifndef TARGET_NO_THREADS
		retro_atomic_int_init(&state, 0);
#else
		state = false;
#endif
	}

	//Set state to signaled
	void Set()
	{
#ifndef TARGET_NO_THREADS
		retro_atomic_store_release_int(&state, 1);
		ec.Notify();
#else
		state = true;
#endif
	}

	//Set state to non signaled
	void Reset()
	{
#ifndef TARGET_NO_THREADS
		retro_atomic_store_release_int(&state, 0);
#else
		state = false;
#endif
	}

	//Wait for signal, then reset. Returns false if timeout expired, true otherwise
	bool Wait(u32 msec)
	{
#ifndef TARGET_NO_THREADS
		/* A wake that finds the event unset is a stray one; wait again,
		 * a few times at most so the call stays bounded. */
		for (int lap = 0; ; lap++)
		{
			int key;

			if (Consume())
				return true;
			key = ec.Prepare();
			if (Consume())
			{
				ec.Cancel();
				return true;
			}
			if (!ec.Commit(key, (int64_t)msec * 1000) || lap == 3)
				return Consume();
		}
#else
		bool ret = state;
		state = false;
		return ret;
#endif
	}

	//Wait for signal, then reset
	void Wait()
	{
#ifndef TARGET_NO_THREADS
		for (;;)
		{
			int key;

			if (Consume())
				return;
			key = ec.Prepare();
			if (Consume())
			{
				ec.Cancel();
				return;
			}
			ec.Commit(key);
		}
#else
		state = false;
#endif
	}
};

//Set the path !
void set_user_config_dir(const std::string& dir);

//subpath format: /data/fsca-table.bit
std::string get_writable_data_path(const std::string& filename);
std::string get_writable_vmu_path(const char *logical_port);

bool mem_region_lock(void *start, std::size_t len);
bool mem_region_unlock(void *start, std::size_t len);
bool mem_region_noaccess(void *start, std::size_t len);
bool mem_region_set_exec(void *start, std::size_t len);
void *mem_region_reserve(void *start, std::size_t len);
bool mem_region_release(void *start, std::size_t len);
void *mem_region_map_file(void *file_handle, void *dest, std::size_t len, std::size_t offset, bool readwrite);
bool mem_region_unmap_file(void *start, std::size_t len);

class VArray2
{
public:

	u8* data;
	u32 size;
	//void Init(void* data,u32 sz);
	//void Term();
#ifdef TARGET_NO_EXCEPTIONS
	void UnLockRegion(u32 offset,u32 size) {}
#else
	void UnLockRegion(u32 offset,u32 size);
#endif

	void Zero()
	{
		UnLockRegion(0,size);
		memset(data,0,size);
	}

	INLINE u8& operator [](const u32 i)
    {
#ifdef MEM_BOUND_CHECK
        if (i>=size)
		{
			printf("Error: VArray2 , index out of range (%d>%d)\n",i,size-1);
			MEM_DO_BREAK;
		}
#endif
		return data[i];
    }
};

int ExeptionHandler(u32 dwCode, void* pExceptionPointers);
int msgboxf(const char* text,unsigned int type,...);

#define MBX_OK                       0x00000000L
#define MBX_OKCANCEL                 0x00000001L
#define MBX_ABORTRETRYIGNORE         0x00000002L
#define MBX_YESNOCANCEL              0x00000003L
#define MBX_YESNO                    0x00000004L
#define MBX_RETRYCANCEL              0x00000005L


#define MBX_ICONHAND                 0x00000010L
#define MBX_ICONQUESTION             0x00000020L
#define MBX_ICONEXCLAMATION          0x00000030L
#define MBX_ICONASTERISK             0x00000040L


#define MBX_USERICON                 0x00000080L
#define MBX_ICONWARNING              MBX_ICONEXCLAMATION
#define MBX_ICONERROR                MBX_ICONHAND


#define MBX_ICONINFORMATION          MBX_ICONASTERISK
#define MBX_ICONSTOP                 MBX_ICONHAND

#define MBX_DEFBUTTON1               0x00000000L
#define MBX_DEFBUTTON2               0x00000100L
#define MBX_DEFBUTTON3               0x00000200L

#define MBX_DEFBUTTON4               0x00000300L


#define MBX_APPLMODAL                0x00000000L
#define MBX_SYSTEMMODAL              0x00001000L
#define MBX_TASKMODAL                0x00002000L

#define MBX_HELP                     0x00004000L // Help Button


#define MBX_NOFOCUS                  0x00008000L
#define MBX_SETFOREGROUND            0x00010000L
#define MBX_DEFAULT_DESKTOP_ONLY     0x00020000L

#define MBX_TOPMOST                  0x00040000L
#define MBX_RIGHT                    0x00080000L
#define MBX_RTLREADING               0x00100000L

#define MBX_RV_OK                1
#define MBX_RV_CANCEL            2
#define MBX_RV_ABORT             3
#define MBX_RV_RETRY             4
#define MBX_RV_IGNORE            5
#define MBX_RV_YES               6
#define MBX_RV_NO                7

static inline std::string trim_trailing_ws(const std::string& str,
                 const std::string& whitespace = " ")
{
    const auto strEnd = str.find_last_not_of(whitespace);
	if (strEnd == std::string::npos)
		return "";

    return str.substr(0, strEnd + 1);
}
