/*
	Copyright 2026 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include "types.h"
#include <string.h>

namespace modem
{

class InStream
{
public:
	virtual ~InStream() {}
	virtual int read() = 0;
	virtual int available() = 0;
};

class OutStream
{
public:
	virtual ~OutStream() {}
	virtual void write(u8 v) = 0;
};

/* A queue of bytes of a fixed size, so that what it holds can go into a
 * save state as it is. N is a power of two. A byte that does not fit is
 * refused: the users of these are sized so that it does not happen. */
template<unsigned N>
class ByteFifo
{
public:
	ByteFifo() : head(0), count(0) {
		memset(buf, 0, sizeof(buf));
	}

	void clear() {
		head = 0;
		count = 0;
	}
	int size() const {
		return count;
	}
	int room() const {
		return N - count;
	}
	bool push(u8 v)
	{
		if (count == N)
			return false;
		buf[(head + count) & (N - 1)] = v;
		count++;
		return true;
	}
	int pop()
	{
		if (count == 0)
			return -1;
		u8 v = buf[head];
		head = (head + 1) & (N - 1);
		count--;
		return v;
	}
	int front() const {
		return count == 0 ? -1 : buf[head];
	}

	/* Always N + 2 bytes: the count, then the bytes in order and zeroes after
	 * them, so that two queues holding the same bytes are saved the same. */
	void serialize(void **data, unsigned int *total_size) const
	{
		u8 lin[N];
		u16 n = (u16)count;
		for (unsigned i = 0; i < N; i++)
			lin[i] = i < count ? buf[(head + i) & (N - 1)] : 0;
		LIBRETRO_S(n);
		LIBRETRO_SA(lin, N);
	}
	void unserialize(void **data, unsigned int *total_size)
	{
		u16 n = 0;
		LIBRETRO_US(n);
		LIBRETRO_USA(buf, N);
		head = 0;
		count = n <= N ? n : N;
	}

private:
	u8 buf[N];
	unsigned head;
	unsigned count;
};

}
