#pragma once

template <class T>
struct List
{
	T* daty;
	int avail;

	int size;
	bool* overrun;
   const char *list_name;

	__forceinline int used() const { return size-avail; }
	__forceinline int bytes() const { return used()* sizeof(T); }

	/* The list is full. The frame is marked, to be dropped, and the list
	 * starts again at its beginning with the @n elements asked for taken
	 * from there as by any other Append(): whatever the caller goes on to do
	 * with them - write them all, come back to the last of them through
	 * LastPtr(), give it back with PopLast() - is then inside the list, where
	 * with nothing taken LastPtr() was the element below the buffer. (@n is
	 * 1 or 4, and no list is smaller than that.) */
	NOINLINE
	T* sig_overrun(int n)
	{ 
		T* rv;

		*overrun |= true;
		Clear();
      if (list_name != NULL)
		   WARN_LOG(PVR, "List overrun for list %s", list_name);

		rv = daty;
		daty += n;
		avail -= n;
		return rv;
	}

	__forceinline 
	T* Append(int n = 1)
	{
		int ad=avail-n;

		if (ad>=0)
		{
			T* rv=daty;
			daty+=n;
			avail=ad;
			return rv;
		}
		else
			return sig_overrun(n);
	}

	__forceinline 
	T* LastPtr(int n = 1) const
	{ 
		return daty-n; 
	}

	T* PopLast()
	{
		daty--;
		avail++;

		return daty;
	}

	T* head() const { return daty-used(); }

	void InitBytes(int maxbytes,bool* ovrn, const char *name)
	{
		maxbytes-=maxbytes%sizeof(T);

		daty=(T*)malloc(maxbytes);
		
		avail=size=maxbytes/sizeof(T);

		overrun=ovrn;

		Clear();
      list_name = name;
	}

	void Init(int maxsize,bool* ovrn, const char *name)
	{
		InitBytes(maxsize*sizeof(T),ovrn, name);
	}

	void Clear()
	{
		daty=head();
		avail=size;
	}

	void Free()
	{
		Clear();
		free(daty);
	}

	T* begin() const { return head(); }
	T* end() const { return LastPtr(0); }
};
