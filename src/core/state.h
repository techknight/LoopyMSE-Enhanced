#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// Savestate serialization. Every module writes its state in a fixed order
// between two System calls (a frame boundary), and reads it back in the same
// order. Sections are tagged, so a state from a different build or layout is
// refused instead of being misread.
namespace State
{

class Writer
{
public:
	std::vector<uint8_t> data;

	void bytes(const void* src, size_t size)
	{
		const uint8_t* p = (const uint8_t*)src;
		data.insert(data.end(), p, p + size);
	}

	template <typename T> void put(const T& value)
	{
		static_assert(std::is_trivially_copyable<T>::value, "put() needs a plain value");
		bytes(&value, sizeof(T));
	}

	void put_vector(const std::vector<uint8_t>& v)
	{
		put<uint32_t>((uint32_t)v.size());
		bytes(v.data(), v.size());
	}

	void put_string(const std::string& s)
	{
		put<uint32_t>((uint32_t)s.size());
		bytes(s.data(), s.size());
	}

	// Start a named section: the name and the size of what the module writes
	// next are checked on load.
	void section(const char* name)
	{
		put_string(name);
	}
};

class Reader
{
public:
	Reader(const uint8_t* data, size_t size) : p(data), end(data + size) {}

	void bytes(void* dst, size_t size)
	{
		if ((size_t)(end - p) < size) throw std::runtime_error("savestate is truncated");
		memcpy(dst, p, size);
		p += size;
	}

	template <typename T> void get(T& value)
	{
		static_assert(std::is_trivially_copyable<T>::value, "get() needs a plain value");
		bytes(&value, sizeof(T));
	}

	template <typename T> T get()
	{
		T value;
		get(value);
		return value;
	}

	void get_vector(std::vector<uint8_t>& v)
	{
		uint32_t size = get<uint32_t>();
		v.resize(size);
		bytes(v.data(), size);
	}

	std::string get_string()
	{
		uint32_t size = get<uint32_t>();
		if (size > 4096) throw std::runtime_error("savestate is damaged");
		std::string s(size, '\0');
		bytes(&s[0], size);
		return s;
	}

	void section(const char* name)
	{
		std::string found = get_string();
		if (found != name)
		{
			throw std::runtime_error("savestate does not match this build (expected " + std::string(name) + ", found " + found + ")");
		}
	}

	bool at_end() const
	{
		return p == end;
	}

private:
	const uint8_t* p;
	const uint8_t* end;
};

}  // namespace State
