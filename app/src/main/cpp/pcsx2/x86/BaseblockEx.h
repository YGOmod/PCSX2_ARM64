// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstring>
#include <map>

#include "common/Assertions.h"
#include "common/arm64/AsmHelpers.h"

// Every potential jump point in the PS2's addressable memory has a BASEBLOCK
// associated with it. So that means a BASEBLOCK for every 4 bytes of PS2
// addressable memory.  Yay!
struct BASEBLOCK
{
	uptr m_pFnptr;

	__inline uptr GetFnptr() const { return m_pFnptr; }
	void __inline SetFnptr(uptr ptr) { m_pFnptr = ptr; }
};

// extra block info (only valid for start of fn)
struct BASEBLOCKEX
{
	uptr fnptr;
	u32 startpc;
	u32 size;    // The size in dwords (equivalent to the number of instructions)
	u32 x86size; // The size in byte of the translated x86 instructions

#ifdef PCSX2_DEVBUILD
	// Could be useful to instrument the block
	//u32 visited; // number of times called
	//u64 ltime; // regs it assumes to have set already
#endif
};

class BaseBlockArray
{
	s32 _Reserved;
	s32 _Size;
	BASEBLOCKEX* blocks;

	__fi void resize(s32 size)
	{
		pxAssert(size > 0);
		BASEBLOCKEX* newMem = new BASEBLOCKEX[size];
		if (blocks)
		{
			memcpy(newMem, blocks, _Reserved * sizeof(BASEBLOCKEX));
			delete[] blocks;
		}
		blocks = newMem;
		pxAssert(blocks != NULL);
	}

	void reserve(u32 size)
	{
		resize(size);
		_Reserved = size;
	}

public:
	~BaseBlockArray()
	{
		if (blocks)
			delete[] blocks;
	}

	BaseBlockArray(s32 size)
		: _Reserved(0)
		, _Size(0)
		, blocks(NULL)
	{
		reserve(size);
	}

	BASEBLOCKEX* insert(u32 startpc, uptr fnptr)
	{
		if (_Size + 1 >= _Reserved)
		{
			reserve(_Reserved + 0x2000); // some games requires even more!
		}

		// Insert the the new BASEBLOCKEX by startpc order
		int imin = 0, imax = _Size, imid;

		while (imin < imax)
		{
			imid = (imin + imax) >> 1;

			if (blocks[imid].startpc > startpc)
				imax = imid;
			else
				imin = imid + 1;
		}

		pxAssert(imin == _Size || blocks[imin].startpc > startpc);

		if (imin < _Size)
		{
			// make a hole for a new block.
			memmove(blocks + imin + 1, blocks + imin, (_Size - imin) * sizeof(BASEBLOCKEX));
		}

		memset((blocks + imin), 0, sizeof(BASEBLOCKEX));
		blocks[imin].startpc = startpc;
		blocks[imin].fnptr = fnptr;

		_Size++;
		return &blocks[imin];
	}

	__fi BASEBLOCKEX& operator[](int idx) const
	{
		return *(blocks + idx);
	}

	void clear()
	{
		_Size = 0;
	}

	__fi u32 size() const
	{
		return _Size;
	}

	__fi void erase(s32 first, s32 last)
	{
		int range = last - first;

		if (last < _Size)
		{
			memmove(blocks + first, blocks + last, (_Size - last) * sizeof(BASEBLOCKEX));
		}

		_Size -= range;
	}
};

class BaseBlocks
{
protected:
	typedef std::multimap<u32, uptr>::iterator linkiter_t;

	// switch to a hash map later?
	std::multimap<u32, uptr> links;
	uptr recompiler;
	BaseBlockArray blocks;

public:
	BaseBlocks()
		: recompiler(0)
		, blocks(0x4000)
	{
	}

	void SetJITCompile(const void *recompiler_)
	{
		recompiler = reinterpret_cast<uptr>(recompiler_);
	}

	BASEBLOCKEX* New(u32 startpc, uptr fnptr);
	int LastIndex(u32 startpc) const;
	//BASEBLOCKEX* GetByX86(uptr ip);

	__fi int Index(u32 startpc) const
	{
		int idx = LastIndex(startpc);

		if ((idx == -1) || (startpc < blocks[idx].startpc) ||
			((blocks[idx].size) && (startpc >= blocks[idx].startpc + blocks[idx].size * 4)))
			return -1;
		else
			return idx;
	}

	__fi BASEBLOCKEX* operator[](int idx)
	{
		if (idx < 0 || idx >= (int)blocks.size())
			return 0;

		return &blocks[idx];
	}

	__fi BASEBLOCKEX* Get(u32 startpc)
	{
		return (*this)[Index(startpc)];
	}

	__fi void Remove(int first, int last)
	{
		pxAssert(first <= last);
		int idx = first;
		do
		{
			pxAssert(idx <= last);

			//u32 startpc = blocks[idx].startpc;
			std::pair<linkiter_t, linkiter_t> range = links.equal_range(blocks[idx].startpc);
			for (auto i = range.first; i != range.second; ++i) {
//                *(u32 *) i->second = recompiler - (i->second + 4);
                armEmitJmpPtr((void*)i->second, (void*)recompiler, true);
            }

			if (IsDevBuild)
			{
				// Clear the first instruction to 0xcc (breakpoint), as a way to assert if some
				// static jumps get left behind to this block.  Note: Do not clear more than the
				// first byte, since this code is called during exception handlers and event handlers
				// both of which expect to be able to return to the recompiled code.

				BASEBLOCKEX effu(blocks[idx]);
				memset((void*)effu.fnptr, 0xcc, 1);
			}
		} while (idx++ < last);

		// TODO: remove links from this block?
		blocks.erase(first, last + 1);
	}

	void Link(u32 pc, s32* jumpptr);

	__fi void Reset()
	{
		blocks.clear();
		links.clear();
	}
};

#define PC_GETBLOCK_(x, reclut) ((BASEBLOCK*)(reclut[((u32)(x)) >> 16] + (x) * (sizeof(BASEBLOCK) / 4)))

/**
 * Add a page to the recompiler lookup table
 *
 * Will associate `reclut[pagebase + pageidx]` with `mapbase[mappage << 14]`
 * Will associate `hwlut[pagebase + pageidx]` with `pageidx << 16`
 */
static inline void recLUT_SetPage(uptr reclut[0x10000], u32 hwlut[0x10000],
                                  BASEBLOCK* mapbase, uint pagebase, uint pageidx, uint mappage)
{
	// this value is in 64k pages!
	uint page = pagebase + pageidx;

	pxAssert(page < 0x10000);
	reclut[page] = (uptr)&mapbase[((s32)mappage - (s32)page) << 14];
	if (hwlut)
		hwlut[page] = 0u - (pagebase << 16);
}

static_assert(sizeof(BASEBLOCK) == 8, "BASEBLOCK is not 8 bytes");
