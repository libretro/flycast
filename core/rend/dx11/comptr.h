/*
	Copyright 2021 flyinghead

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
#include <utility>

/* A COM object held: it is released when the holder goes. */
template<typename T>
class ComPtr
{
public:
	ComPtr() = default;
	ComPtr(const ComPtr& other) : ptr(other.ptr) {
		if (ptr != nullptr)
			ptr->AddRef();
	}
	ComPtr(ComPtr&& other) noexcept {
		std::swap(ptr, other.ptr);
	}
	~ComPtr() {
		if (ptr != nullptr)
			ptr->Release();
	}

	ComPtr& operator=(const ComPtr& other) {
		if (this != &other)
			*this = ComPtr(other);
		return *this;
	}
	ComPtr& operator=(ComPtr&& other) noexcept {
		std::swap(ptr, other.ptr);
		return *this;
	}

	T* operator->() const noexcept {
		return ptr;
	}
	explicit operator bool() const noexcept {
		return ptr != nullptr;
	}
	operator T*() const noexcept {
		return ptr;
	}
	/* For a function that gives one: what was held is released first. */
	T** put() noexcept {
		reset();
		return &ptr;
	}
	/* For a function that takes an array of them */
	T* const* addr() const noexcept {
		return &ptr;
	}
	T* get() const noexcept {
		return ptr;
	}
	/* Takes over a reference the caller had */
	void reset(T *p = nullptr) {
		if (p == ptr)
			return;
		std::swap(ptr, p);
		if (p != nullptr)
			p->Release();
	}

private:
	T *ptr = nullptr;
};
