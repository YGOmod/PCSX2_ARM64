// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "VUops.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "MTVU.h"

#include <cmath>
u32 laststall = 0;
//Lower/Upper instructions can use that..
#define _Ft_ ((VU->code >> 16) & 0x1F)  // The rt part of the instruction register
#define _Fs_ ((VU->code >> 11) & 0x1F)  // The rd part of the instruction register
#define _Fd_ ((VU->code >>  6) & 0x1F)  // The sa part of the instruction register
#define _It_ (_Ft_ & 0xF)
#define _Is_ (_Fs_ & 0xF)
#define _Id_ (_Fd_ & 0xF)

#define _X ((VU->code >> 24) & 0x1)
#define _Y ((VU->code >> 23) & 0x1)
#define _Z ((VU->code >> 22) & 0x1)
#define _W ((VU->code >> 21) & 0x1)

#define _XYZW ((VU->code >> 21) & 0xF)

#define _Fsf_ ((VU->code >> 21) & 0x03)
#define _Ftf_ ((VU->code >> 23) & 0x03)

#define _Imm11_		(s32)(VU->code & 0x400 ? 0xfffffc00 | (VU->code & 0x3ff) : VU->code & 0x3ff)
#define _UImm11_	(s32)(VU->code & 0x7ff)

#define VI_BACKUP

alignas(16) static VECTOR RDzero;

static __ri bool _vuFMACflush(VURegs* VU)
{
	bool didflush = false;

	VUM_LOG("Flushing FMACs");

	for (int i = VU->fmacreadpos; VU->fmaccount > 0; i = (i + 1) & 3)
	{
		if ((VU->cycle - VU->fmac[i].sCycle) < VU->fmac[i].Cycle)
		{
			VUM_LOG("Not flushing FMAC pipe[%d] (macflag=%x clipflag=%x statusflag=%x) r %d w %d", i, VU->fmac[i].macflag, VU->fmac[i].clipflag, VU->fmac[i].statusflag, VU->fmacreadpos, VU->fmacwritepos);
			return didflush;
		}

		VUM_LOG("flushing FMAC pipe[%d] (macflag=%x clipflag=%x statusflag=%x) r %d w %d", i, VU->fmac[i].macflag, VU->fmac[i].clipflag, VU->fmac[i].statusflag, VU->fmacreadpos, VU->fmacwritepos);

		// Clip flags (Affected by CLIP instruction)
		if (VU->fmac[i].flagreg & (1 << REG_CLIP_FLAG))
			VU->VI[REG_CLIP_FLAG].UL = VU->fmac[i].clipflag;

		// Normal FMAC instructoins only affectx Z/S/I/O, D/I are modified only by FDIV instructions
		// Sticky flags (Affected by FSSET)
		if (VU->fmac[i].flagreg & (1 << REG_STATUS_FLAG))
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0x30) | (VU->fmac[i].statusflag & 0xFC0) | (VU->fmac[i].statusflag & 0xF);
		else
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFF0) | (VU->fmac[i].statusflag & 0xF) | ((VU->fmac[i].statusflag & 0xF) << 6);
		VU->VI[REG_MAC_FLAG].UL = VU->fmac[i].macflag;

		VU->fmacreadpos = (VU->fmacreadpos + 1) & 3;
		VU->fmaccount--;

		didflush = true;
	}

	return didflush;
}

static __ri bool _vuIALUflush(VURegs* VU)
{
	bool didflush = false;

	VUM_LOG("Flushing ALU stalls");

	for (int i = VU->ialureadpos; VU->ialucount > 0; i = (i + 1) & 3)
	{
		if ((VU->cycle - VU->ialu[i].sCycle) < VU->ialu[i].Cycle)
			return didflush;

		VU->ialureadpos = (VU->ialureadpos + 1) & 3;
		VU->ialucount--;
		didflush = true;
	}
	return didflush;
}

static __ri bool _vuFDIVflush(VURegs* VU)
{
	if (VU->fdiv.enable == 0)
		return false;

	if ((VU->cycle - VU->fdiv.sCycle) >= VU->fdiv.Cycle)
	{
		VUM_LOG("flushing FDIV pipe");

		VU->fdiv.enable = 0;
		VU->VI[REG_Q].UL = VU->fdiv.reg.UL;
		// FDIV only affects D/I
		VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFCF) | (VU->fdiv.statusflag & 0xC30);

		return true;
	}
	return false;
}

static __ri bool _vuEFUflush(VURegs* VU)
{
	if (VU->efu.enable == 0)
		return false;

	if ((VU->cycle - VU->efu.sCycle) >= VU->efu.Cycle)
	{
		VUM_LOG("flushing EFU pipe");

		VU->efu.enable = 0;
		VU->VI[REG_P].UL = VU->efu.reg.UL;

		return true;
	}

	return false;
}

// called at end of program
void _vuFlushAll(VURegs* VU)
{
	int i = 0;

	if (VU->fdiv.enable)
	{
		VU->fdiv.enable = 0;
		VU->VI[REG_Q].UL = VU->fdiv.reg.UL;
		VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFCF) | (VU->fdiv.statusflag & 0xC30);

		if ((VU->cycle - VU->fdiv.sCycle) < VU->fdiv.Cycle)
			VU->cycle = VU->fdiv.sCycle + VU->fdiv.Cycle;
	}

	if (VU->efu.enable)
	{
		VU->efu.enable = 0;
		VU->VI[REG_P].UL = VU->efu.reg.UL;

		if ((VU->cycle - VU->efu.sCycle) < VU->efu.Cycle)
			VU->cycle = VU->efu.sCycle + VU->efu.Cycle;
	}

	for (i = VU->fmacreadpos; VU->fmaccount > 0; i = (i + 1) & 3)
	{
		VUM_LOG("flushing FMAC pipe[%d] (macflag=%x)", i, VU->fmac[i].macflag);

		// Clip flags (Affected by CLIP instruction)
		if (VU->fmac[i].flagreg & (1 << REG_CLIP_FLAG))
			VU->VI[REG_CLIP_FLAG].UL = VU->fmac[i].clipflag;

		// Normal FMAC instructoins only affectx Z/S/I/O, D/I are modified only by FDIV instructions
		// Sticky flags (Affected by FSSET)
		if (VU->fmac[i].flagreg & (1 << REG_STATUS_FLAG))
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0x30) | (VU->fmac[i].statusflag & 0xFC0) | (VU->fmac[i].statusflag & 0xF);
		else
			VU->VI[REG_STATUS_FLAG].UL = (VU->VI[REG_STATUS_FLAG].UL & 0xFF0) | (VU->fmac[i].statusflag & 0xF) | ((VU->fmac[i].statusflag & 0xF) << 6);
		VU->VI[REG_MAC_FLAG].UL = VU->fmac[i].macflag;

		VU->fmacreadpos = (VU->fmacreadpos + 1) & 3;

		if ((VU->cycle - VU->fmac[i].sCycle) < VU->fmac[i].Cycle)
			VU->cycle = VU->fmac[i].sCycle + VU->fmac[i].Cycle;

		VU->fmaccount--;
	}

	for (i = VU->ialureadpos; VU->ialucount > 0; i = (i + 1) & 3)
	{
		VU->ialureadpos = (VU->ialureadpos + 1) & 3;

		if ((VU->cycle - VU->ialu[i].sCycle) < VU->ialu[i].Cycle)
			VU->cycle = VU->ialu[i].sCycle + VU->ialu[i].Cycle;

		VU->ialucount--;
	}
}

__fi void _vuTestPipes(VURegs* VU)
{
	bool flushed;

	do
	{
		flushed = false;
		flushed |= _vuFMACflush(VU);
		flushed |= _vuFDIVflush(VU);
		flushed |= _vuEFUflush(VU);
		flushed |= _vuIALUflush(VU);
	} while (flushed == true);

	if (VU == &VU1)
	{
		if (VU1.xgkickenable)
		{
			_vuXGKICKTransfer((VU1.cycle - VU1.xgkicklastcycle) - 1, false);
		}
	}
}

static void _vuFMACTestStall(VURegs* VU, u32 reg, u32 xyzw)
{
	u32 i = 0;

	for (int currentpipe = VU->fmacreadpos; i < VU->fmaccount; currentpipe = (currentpipe + 1) & 3, i++)
	{
		//Check if enough cycles have passed for this fmac position
		if ((VU->cycle - VU->fmac[currentpipe].sCycle) >= VU->fmac[currentpipe].Cycle)
			continue;

		// Check if the regs match
		if ((VU->fmac[currentpipe].regupper == reg && VU->fmac[currentpipe].xyzwupper & xyzw)
			|| (VU->fmac[currentpipe].reglower == reg && VU->fmac[currentpipe].xyzwlower & xyzw))
		{
			u32 newCycle = VU->fmac[currentpipe].Cycle + VU->fmac[currentpipe].sCycle;

			VUM_LOG("FMAC[%d] stall %d", currentpipe, newCycle - VU->cycle);
			if (newCycle > VU->cycle)
				VU->cycle = newCycle;
		}
	}
}

static __fi void _vuTestFMACStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	if (VUregsn->VFread0)
	{
		_vuFMACTestStall(VU, VUregsn->VFread0, VUregsn->VFr0xyzw);
	}
	if (VUregsn->VFread1)
	{
		_vuFMACTestStall(VU, VUregsn->VFread1, VUregsn->VFr1xyzw);
	}
}

static __fi void _vuTestFDIVStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	_vuTestFMACStalls(VU, VUregsn);

	if (VU->fdiv.enable != 0)
	{
		u32 newCycle = VU->fdiv.Cycle + VU->fdiv.sCycle;
		VUM_LOG("waiting FDIV pipe %d", newCycle - VU->cycle);
		if (newCycle > VU->cycle)
			VU->cycle = newCycle;
	}
}

static __fi void _vuTestEFUStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	_vuTestFMACStalls(VU, VUregsn);

	if (VU->efu.enable == 0)
		return;

	// With EFU commands they have a throughput/latency that doesn't match, this means if a stall occurs
	// The stall is released 1 cycle before P is updated. However there is no other command that can read
	// P on the same cycle as the stall is released, and if the stall is caused by an EFU command other
	// than WAITP, we're going to overwrite the value in the pipeline, which will break everything.
	// So the TL;DR of this is that we should be safe to release 1 cycle early and write back P
	VU->efu.Cycle -= 1;
	u32 newCycle = VU->efu.sCycle + VU->efu.Cycle;

	VUM_LOG("waiting EFU pipe %d", newCycle - VU->cycle);
	if (newCycle > VU->cycle)
		VU->cycle = newCycle;
}

static __fi void _vuTestALUStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	u32 i = 0;

	for (int currentpipe = VU->ialureadpos; i < VU->ialucount; currentpipe = (currentpipe + 1) & 3, i++)
	{
		if ((VU->cycle - VU->ialu[currentpipe].sCycle) >= VU->ialu[currentpipe].Cycle)
			continue;

		if (VU->ialu[currentpipe].reg & VUregsn->VIread) // Read and written VI regs share the same register
		{
			u32 newCycle = VU->ialu[currentpipe].Cycle + VU->ialu[currentpipe].sCycle;

			VUM_LOG("ALU[%d] stall %d", currentpipe, newCycle - VU->cycle);
			if (newCycle > VU->cycle)
				VU->cycle = newCycle;
		}
	}
}

__fi void _vuTestUpperStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	switch (VUregsn->pipe) {
	case VUPIPE_FMAC: _vuTestFMACStalls(VU, VUregsn); break;
	}
}


__fi void _vuTestLowerStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	switch (VUregsn->pipe)
	{
		case VUPIPE_FMAC: _vuTestFMACStalls(VU, VUregsn); break;
		case VUPIPE_FDIV: _vuTestFDIVStalls(VU, VUregsn); break;
		case VUPIPE_EFU:  _vuTestEFUStalls(VU, VUregsn); break;
		case VUPIPE_BRANCH: _vuTestALUStalls(VU, VUregsn); break;
	}
}

__fi void _vuClearFMAC(VURegs* VU)
{
	int i = VU->fmacwritepos;

	memset(&VU->fmac[i], 0, sizeof(fmacPipe));
	VU->fmaccount++;
}

static __ri void _vuAddFMACStalls(VURegs* VU, _VURegsNum* VUregsn, bool isUpper)
{
	int i = VU->fmacwritepos;

	VUM_LOG("adding FMAC %s pipe[%d]; reg=%x xyzw=%x flagreg=%x target=%x current %x", isUpper ? "Upper" : "Lower", i, VUregsn->VFwrite, VUregsn->VFwxyzw, VUregsn->VIwrite, VU->cycle + 4, VU->cycle);
	VU->fmac[i].sCycle = VU->cycle;
	VU->fmac[i].Cycle = 4;

	if (isUpper)
	{
		VU->fmac[i].regupper = VUregsn->VFwrite;
		VU->fmac[i].xyzwupper = VUregsn->VFwxyzw;
		VU->fmac[i].flagreg = VUregsn->VIwrite;
	}
	else
	{
		VU->fmac[i].reglower = VUregsn->VFwrite;
		VU->fmac[i].xyzwlower = VUregsn->VFwxyzw;
		VU->fmac[i].flagreg |= VUregsn->VIwrite;
	}

	VU->fmac[i].macflag = VU->macflag;
	VU->fmac[i].statusflag = VU->statusflag;
	VU->fmac[i].clipflag = VU->clipflag;
}


static __ri void _vuFDIVAdd(VURegs* VU, int cycles)
{
	VUM_LOG("adding FDIV pipe");

	VU->fdiv.enable = 1;
	VU->fdiv.sCycle = VU->cycle;
	VU->fdiv.Cycle = cycles;
	VU->fdiv.reg.F = VU->q.F;
	VU->fdiv.statusflag = VU->statusflag;
}

static __ri void _vuEFUAdd(VURegs* VU, int cycles)
{
	VUM_LOG("adding EFU pipe for %d cycles\n", cycles);

	VU->efu.enable = 1;
	VU->efu.sCycle = VU->cycle;
	VU->efu.Cycle = cycles;
	VU->efu.reg.F = VU->p.F;
}

static __ri void _vuAddIALUStalls(VURegs* VU, _VURegsNum* VUregsn)
{

	if (VUregsn->cycles == 0)
		return;

	int i = VU->ialuwritepos;

	VUM_LOG("adding IALU pipe[%d]; reg=%x target=%x current %x", i, VUregsn->VIwrite, VU->cycle + VUregsn->cycles, VU->cycle);
	VU->ialu[i].sCycle = VU->cycle;
	VU->ialu[i].Cycle = VUregsn->cycles;
	VU->ialu[i].reg = VUregsn->VIwrite;

	VU->ialuwritepos = (VU->ialuwritepos + 1) & 3;
	VU->ialucount++;
}

static __fi void _vuAddFDIVStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	if (VUregsn->VIwrite & (1 << REG_Q))
		_vuFDIVAdd(VU, VUregsn->cycles);
}

static __fi void _vuAddEFUStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	if (VUregsn->VIwrite & (1 << REG_P))
		_vuEFUAdd(VU, VUregsn->cycles);
}

__fi void _vuAddUpperStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	switch (VUregsn->pipe) {
	case VUPIPE_FMAC: _vuAddFMACStalls(VU, VUregsn, true); break;
	}
}

__fi void _vuAddLowerStalls(VURegs* VU, _VURegsNum* VUregsn)
{
	switch (VUregsn->pipe)
	{
		case VUPIPE_FMAC: _vuAddFMACStalls(VU, VUregsn, false); break;
		case VUPIPE_FDIV: _vuAddFDIVStalls(VU, VUregsn); break;
		case VUPIPE_EFU:  _vuAddEFUStalls(VU, VUregsn); break;
		case VUPIPE_IALU: _vuAddIALUStalls(VU, VUregsn); break;
	}
}

__fi void _vuBackupVI(VURegs* VU, u32 reg)
{
#ifdef VI_BACKUP
	if (VU->VIBackupCycles && reg == VU->VIRegNumber)
	{
		//On repeat writes we need to remember the value from before the chain
		VU->VIBackupCycles = 2;
		return;
	}

	VU->VIBackupCycles = 2;
	VU->VIRegNumber = reg;
	VU->VIOldValue = VU->VI[reg].US[0];
#endif
}

/******************************/
/*   VU Upper instructions    */
/******************************/
#ifndef INT_VUDOUBLEHACK
static float vuDouble(u32 f)
{
	switch (f & 0x7f800000)
	{
		case 0x0:
			f &= 0x80000000;
			return *(float*)&f;
			break;
		case 0x7f800000:
			if (CHECK_VU_OVERFLOW(0))
			{
				u32 d = (f & 0x80000000) | 0x7f7fffff;
				return *(float*)&d;
			}
			break;
	}
	return *(float*)&f;
}
#else
static __fi float vuDouble(u32 f)
{
	return *(float*)&f;
}
#endif

static __fi float vuADD_TriAceHack(u32 a, u32 b)
{
	// On VU0 TriAce Games use ADDi and expects these bit-perfect results:
	//if (a == 0xb3e2a619 && b == 0x42546666) return vuDouble(0x42546666);
	//if (a == 0x8b5b19e9 && b == 0xc7f079b3) return vuDouble(0xc7f079b3);
	//if (a == 0x4b1ed4a8 && b == 0x43a02666) return vuDouble(0x4b1ed5e7);
	//if (a == 0x7d1ca47b && b == 0x42f23333) return vuDouble(0x7d1ca47b);

	// In the 3rd case, some other rounding error is giving us incorrect
	// operands ('a' is wrong); and therefor an incorrect result.
	// We're getting:        0x4b1ed4a8 + 0x43a02666 = 0x4b1ed5e8
	// We should be getting: 0x4b1ed4a7 + 0x43a02666 = 0x4b1ed5e7
	// microVU gets the correct operands and result. The interps likely
	// don't get it due to rounding towards nearest in other calculations.

	// microVU uses something like this to get TriAce games working,
	// but VU interpreters don't seem to need it currently:

	// Update Sept 2021, now the interpreters don't suck, they do - Refraction
	s32 aExp = (a >> 23) & 0xff;
	s32 bExp = (b >> 23) & 0xff;
	if (aExp - bExp >= 25) b &= 0x80000000;
	if (aExp - bExp <=-25) a &= 0x80000000;
	float ret = vuDouble(a) + vuDouble(b);
	//DevCon.WriteLn("aExp = %d, bExp = %d", aExp, bExp);
	//DevCon.WriteLn("0x%08x + 0x%08x = 0x%08x", a, b, (u32&)ret);
	//DevCon.WriteLn("%f + %f = %f", vuDouble(a), vuDouble(b), ret);
	return ret;
}

template <u32(*Fn)(u32)>
void __fi applyUnaryFunction(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) { VU->VF[_Ft_].i.x = Fn(VU->VF[_Fs_].i.x); }
	if (_Y) { VU->VF[_Ft_].i.y = Fn(VU->VF[_Fs_].i.y); }
	if (_Z) { VU->VF[_Ft_].i.z = Fn(VU->VF[_Fs_].i.z); }
	if (_W) { VU->VF[_Ft_].i.w = Fn(VU->VF[_Fs_].i.w); }
}

u32 __fi vuOpABS(u32 fs)
{
	return fs & 0x7fffffff;
}

void __fi _vuABS(VURegs* VU)
{
	return applyUnaryFunction<vuOpABS>(VU);
}

enum class MACOpDst { Fd, Acc };

template <MACOpDst Dst>
static __fi VECTOR* _getDst(VURegs* VU)
{
	if (Dst == MACOpDst::Acc)
		return &VU->ACC;
	else if (_Fd_ == 0)
		return &RDzero;
	else
		return &VU->VF[_Fd_];
}

template <float(*Fn)(u32, u32), MACOpDst Dst>
static __fi void applyBinaryMACOp(VURegs* VU)
{
	VECTOR* dst = _getDst<Dst>(VU);
	if (_X) { dst->i.x = VU_MACx_UPDATE(VU, Fn(VU->VF[_Fs_].i.x, VU->VF[_Ft_].i.x)); } else VU_MACx_CLEAR(VU);
	if (_Y) { dst->i.y = VU_MACy_UPDATE(VU, Fn(VU->VF[_Fs_].i.y, VU->VF[_Ft_].i.y)); } else VU_MACy_CLEAR(VU);
	if (_Z) { dst->i.z = VU_MACz_UPDATE(VU, Fn(VU->VF[_Fs_].i.z, VU->VF[_Ft_].i.z)); } else VU_MACz_CLEAR(VU);
	if (_W) { dst->i.w = VU_MACw_UPDATE(VU, Fn(VU->VF[_Fs_].i.w, VU->VF[_Ft_].i.w)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

template <float(*Fn)(u32, u32), MACOpDst Dst>
static __fi void applyBinaryMACOpBroadcast(VURegs* VU, u32 bc)
{
	VECTOR* dst = _getDst<Dst>(VU);
	if (_X) { dst->i.x = VU_MACx_UPDATE(VU, Fn(VU->VF[_Fs_].i.x, bc)); } else VU_MACx_CLEAR(VU);
	if (_Y) { dst->i.y = VU_MACy_UPDATE(VU, Fn(VU->VF[_Fs_].i.y, bc)); } else VU_MACy_CLEAR(VU);
	if (_Z) { dst->i.z = VU_MACz_UPDATE(VU, Fn(VU->VF[_Fs_].i.z, bc)); } else VU_MACz_CLEAR(VU);
	if (_W) { dst->i.w = VU_MACw_UPDATE(VU, Fn(VU->VF[_Fs_].i.w, bc)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi float _vuOpADD(u32 fs, u32 ft)
{
	return vuDouble(fs) + vuDouble(ft);
}

static __fi void _vuADD(VURegs* VU)
{
	applyBinaryMACOp<_vuOpADD, MACOpDst::Fd>(VU);
}

static __fi void vuADDbc(VURegs* VU, u32 bc)
{
	applyBinaryMACOpBroadcast<_vuOpADD, MACOpDst::Fd>(VU, bc);
}

static __fi void vuADDbc_addsubhack(VURegs* VU, u32 bc)
{
	if (CHECK_VUADDSUBHACK)
		applyBinaryMACOpBroadcast<vuADD_TriAceHack, MACOpDst::Fd>(VU, bc);
	else
		applyBinaryMACOpBroadcast<_vuOpADD, MACOpDst::Fd>(VU, bc);
}

static __fi void _vuADDi(VURegs* VU)
{
	vuADDbc_addsubhack(VU, VU->VI[REG_I].UL);
}

static __fi void _vuADDq(VURegs* VU) { vuADDbc(VU, VU->VI[REG_Q].UL); }
static __fi void _vuADDx(VURegs* VU) { vuADDbc(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuADDy(VURegs* VU) { vuADDbc(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuADDz(VURegs* VU) { vuADDbc(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuADDw(VURegs* VU) { vuADDbc(VU, VU->VF[_Ft_].i.w); }

static __fi void _vuADDA(VURegs* VU)
{
	applyBinaryMACOp<_vuOpADD, MACOpDst::Acc>(VU);
}

static __fi void vuADDAbc(VURegs* VU, u32 bc)
{
	applyBinaryMACOpBroadcast<_vuOpADD, MACOpDst::Acc>(VU, bc);
}

static __fi void _vuADDAi(VURegs* VU) { vuADDAbc(VU, VU->VI[REG_I].UL); }
static __fi void _vuADDAq(VURegs* VU) { vuADDAbc(VU, VU->VI[REG_Q].UL); }
static __fi void _vuADDAx(VURegs* VU) { vuADDAbc(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuADDAy(VURegs* VU) { vuADDAbc(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuADDAz(VURegs* VU) { vuADDAbc(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuADDAw(VURegs* VU) { vuADDAbc(VU, VU->VF[_Ft_].i.w); }

static __fi float _vuOpSUB(u32 fs, u32 ft)
{
	return vuDouble(fs) - vuDouble(ft);
}

static __fi void _vuSUB(VURegs* VU)
{
	applyBinaryMACOp<_vuOpSUB, MACOpDst::Fd>(VU);
}

static __fi void vuSUBbc(VURegs* VU, u32 bc)
{
	applyBinaryMACOpBroadcast<_vuOpSUB, MACOpDst::Fd>(VU, bc);
}

static __fi void _vuSUBi(VURegs* VU) { vuSUBbc(VU, VU->VI[REG_I].UL); }
static __fi void _vuSUBq(VURegs* VU) { vuSUBbc(VU, VU->VI[REG_Q].UL); }
static __fi void _vuSUBx(VURegs* VU) { vuSUBbc(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuSUBy(VURegs* VU) { vuSUBbc(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuSUBz(VURegs* VU) { vuSUBbc(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuSUBw(VURegs* VU) { vuSUBbc(VU, VU->VF[_Ft_].i.w); }

static __fi void _vuSUBA(VURegs* VU)
{
	applyBinaryMACOp<_vuOpSUB, MACOpDst::Acc>(VU);
}

static __fi void vuSUBAbc(VURegs* VU, u32 bc)
{
	applyBinaryMACOpBroadcast<_vuOpSUB, MACOpDst::Acc>(VU, bc);
}

static __fi void _vuSUBAi(VURegs* VU) { vuSUBAbc(VU, VU->VI[REG_I].UL); }
static __fi void _vuSUBAq(VURegs* VU) { vuSUBAbc(VU, VU->VI[REG_Q].UL); }
static __fi void _vuSUBAx(VURegs* VU) { vuSUBAbc(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuSUBAy(VURegs* VU) { vuSUBAbc(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuSUBAz(VURegs* VU) { vuSUBAbc(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuSUBAw(VURegs* VU) { vuSUBAbc(VU, VU->VF[_Ft_].i.w); }

static __fi float _vuOpMUL(u32 fs, u32 ft)
{
	return vuDouble(fs) * vuDouble(ft);
}

static __fi void _vuMUL(VURegs* VU)
{
	applyBinaryMACOp<_vuOpMUL, MACOpDst::Fd>(VU);
}

static __fi void vuMULbc(VURegs* VU, u32 bc)
{
	applyBinaryMACOpBroadcast<_vuOpMUL, MACOpDst::Fd>(VU, bc);
}

static __fi void _vuMULi(VURegs* VU) { vuMULbc(VU, VU->VI[REG_I].UL); }
static __fi void _vuMULq(VURegs* VU) { vuMULbc(VU, VU->VI[REG_Q].UL); }
static __fi void _vuMULx(VURegs* VU) { vuMULbc(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuMULy(VURegs* VU) { vuMULbc(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuMULz(VURegs* VU) { vuMULbc(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuMULw(VURegs* VU) { vuMULbc(VU, VU->VF[_Ft_].i.w); }


static __fi void _vuMULA(VURegs* VU)
{
	applyBinaryMACOp<_vuOpMUL, MACOpDst::Acc>(VU);
}

static __fi void vuMULAbc(VURegs* VU, u32 bc)
{
	applyBinaryMACOpBroadcast<_vuOpMUL, MACOpDst::Acc>(VU, bc);
}

static __fi void _vuMULAi(VURegs* VU) { vuMULAbc(VU, VU->VI[REG_I].UL); }
static __fi void _vuMULAq(VURegs* VU) { vuMULAbc(VU, VU->VI[REG_Q].UL); }
static __fi void _vuMULAx(VURegs* VU) { vuMULAbc(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuMULAy(VURegs* VU) { vuMULAbc(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuMULAz(VURegs* VU) { vuMULAbc(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuMULAw(VURegs* VU) { vuMULAbc(VU, VU->VF[_Ft_].i.w); }

template <float(*Fn)(u32, u32, u32), MACOpDst Dst>
static __fi void applyTernaryMACOp(VURegs* VU)
{
	VECTOR* dst = _getDst<Dst>(VU);
	if (_X) { dst->i.x = VU_MACx_UPDATE(VU, Fn(VU->ACC.i.x, VU->VF[_Fs_].i.x, VU->VF[_Ft_].i.x)); } else VU_MACx_CLEAR(VU);
	if (_Y) { dst->i.y = VU_MACy_UPDATE(VU, Fn(VU->ACC.i.y, VU->VF[_Fs_].i.y, VU->VF[_Ft_].i.y)); } else VU_MACy_CLEAR(VU);
	if (_Z) { dst->i.z = VU_MACz_UPDATE(VU, Fn(VU->ACC.i.z, VU->VF[_Fs_].i.z, VU->VF[_Ft_].i.z)); } else VU_MACz_CLEAR(VU);
	if (_W) { dst->i.w = VU_MACw_UPDATE(VU, Fn(VU->ACC.i.w, VU->VF[_Fs_].i.w, VU->VF[_Ft_].i.w)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

template <float(*Fn)(u32, u32, u32), MACOpDst Dst>
static __fi void applyTernaryMACOpBroadcast(VURegs* VU, u32 bc)
{
	VECTOR* dst = _getDst<Dst>(VU);
	if (_X) { dst->i.x = VU_MACx_UPDATE(VU, Fn(VU->ACC.i.x, VU->VF[_Fs_].i.x, bc)); } else VU_MACx_CLEAR(VU);
	if (_Y) { dst->i.y = VU_MACy_UPDATE(VU, Fn(VU->ACC.i.y, VU->VF[_Fs_].i.y, bc)); } else VU_MACy_CLEAR(VU);
	if (_Z) { dst->i.z = VU_MACz_UPDATE(VU, Fn(VU->ACC.i.z, VU->VF[_Fs_].i.z, bc)); } else VU_MACz_CLEAR(VU);
	if (_W) { dst->i.w = VU_MACw_UPDATE(VU, Fn(VU->ACC.i.w, VU->VF[_Fs_].i.w, bc)); } else VU_MACw_CLEAR(VU);
	VU_STAT_UPDATE(VU);
}

static __fi float _vuOpMADD(u32 acc, u32 fs, u32 ft)
{
	return vuDouble(acc) + vuDouble(fs) * vuDouble(ft);
}

static __fi void _vuMADD(VURegs* VU)
{
	applyTernaryMACOp<_vuOpMADD, MACOpDst::Fd>(VU);
}

static __fi void vuMADDbc(VURegs* VU, u32 bc)
{
	applyTernaryMACOpBroadcast<_vuOpMADD, MACOpDst::Fd>(VU, bc);
}

static __fi void _vuMADDi(VURegs* VU) { vuMADDbc(VU, VU->VI[REG_I].UL); }
static __fi void _vuMADDq(VURegs* VU) { vuMADDbc(VU, VU->VI[REG_Q].UL); }
static __fi void _vuMADDx(VURegs* VU) { vuMADDbc(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuMADDy(VURegs* VU) { vuMADDbc(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuMADDz(VURegs* VU) { vuMADDbc(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuMADDw(VURegs* VU) { vuMADDbc(VU, VU->VF[_Ft_].i.w); }

static __fi void _vuMADDA(VURegs* VU)
{
	applyTernaryMACOp<_vuOpMADD, MACOpDst::Acc>(VU);
}

static __fi void vuMADDAbc(VURegs* VU, u32 bc)
{
	applyTernaryMACOpBroadcast<_vuOpMADD, MACOpDst::Acc>(VU, bc);
}

static __fi void _vuMADDAi(VURegs* VU) { vuMADDAbc(VU, VU->VI[REG_I].UL); }
static __fi void _vuMADDAq(VURegs* VU) { vuMADDAbc(VU, VU->VI[REG_Q].UL); }
static __fi void _vuMADDAx(VURegs* VU) { vuMADDAbc(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuMADDAy(VURegs* VU) { vuMADDAbc(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuMADDAz(VURegs* VU) { vuMADDAbc(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuMADDAw(VURegs* VU) { vuMADDAbc(VU, VU->VF[_Ft_].i.w); }

static __fi float _vuOpMSUB(u32 acc, u32 fs, u32 ft)
{
	return vuDouble(acc) - vuDouble(fs) * vuDouble(ft);
}

static __fi void _vuMSUB(VURegs* VU)
{
	applyTernaryMACOp<_vuOpMSUB, MACOpDst::Fd>(VU);
}

static __fi void vuMSUBbc(VURegs* VU, u32 bc)
{
	applyTernaryMACOpBroadcast<_vuOpMSUB, MACOpDst::Fd>(VU, bc);
}

static __fi void _vuMSUBi(VURegs* VU) { vuMSUBbc(VU, VU->VI[REG_I].UL); }
static __fi void _vuMSUBq(VURegs* VU) { vuMSUBbc(VU, VU->VI[REG_Q].UL); }
static __fi void _vuMSUBx(VURegs* VU) { vuMSUBbc(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuMSUBy(VURegs* VU) { vuMSUBbc(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuMSUBz(VURegs* VU) { vuMSUBbc(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuMSUBw(VURegs* VU) { vuMSUBbc(VU, VU->VF[_Ft_].i.w); }

static __fi void _vuMSUBA(VURegs* VU)
{
	applyTernaryMACOp<_vuOpMSUB, MACOpDst::Acc>(VU);
}

static __fi void vuMSUBAbc(VURegs* VU, u32 bc)
{
	applyTernaryMACOpBroadcast<_vuOpMSUB, MACOpDst::Acc>(VU, bc);
}

static __fi void _vuMSUBAi(VURegs* VU) { vuMSUBAbc(VU, VU->VI[REG_I].UL); }
static __fi void _vuMSUBAq(VURegs* VU) { vuMSUBAbc(VU, VU->VI[REG_Q].UL); }
static __fi void _vuMSUBAx(VURegs* VU) { vuMSUBAbc(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuMSUBAy(VURegs* VU) { vuMSUBAbc(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuMSUBAz(VURegs* VU) { vuMSUBAbc(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuMSUBAw(VURegs* VU) { vuMSUBAbc(VU, VU->VF[_Ft_].i.w); }

// The functions below are floating point semantics min/max on integer representations to get
// the effect of a floating point min/max without issues with denormal and special numbers.

static __fi u32 fp_max(u32 a, u32 b)
{
	return ((s32)a < 0 && (s32)b < 0) ? std::min<s32>(a, b) : std::max<s32>(a, b);
}

static __fi u32 fp_min(u32 a, u32 b)
{
	return ((s32)a < 0 && (s32)b < 0) ? std::max<s32>(a, b) : std::min<s32>(a, b);
}

template <u32(*Fn)(u32, u32)>
static __fi void applyMinMax(VURegs* VU)
{
	if (_Fd_ == 0)
		return;

	if (_X) VU->VF[_Fd_].i.x = Fn(VU->VF[_Fs_].i.x, VU->VF[_Ft_].i.x);
	if (_Y) VU->VF[_Fd_].i.y = Fn(VU->VF[_Fs_].i.y, VU->VF[_Ft_].i.y);
	if (_Z) VU->VF[_Fd_].i.z = Fn(VU->VF[_Fs_].i.z, VU->VF[_Ft_].i.z);
	if (_W) VU->VF[_Fd_].i.w = Fn(VU->VF[_Fs_].i.w, VU->VF[_Ft_].i.w);
}

template <u32(*Fn)(u32, u32)>
static __fi void applyMinMaxBroadcast(VURegs* VU, u32 bc)
{
	if (_Fd_ == 0)
		return;

	if (_X) VU->VF[_Fd_].i.x = Fn(VU->VF[_Fs_].i.x, bc);
	if (_Y) VU->VF[_Fd_].i.y = Fn(VU->VF[_Fs_].i.y, bc);
	if (_Z) VU->VF[_Fd_].i.z = Fn(VU->VF[_Fs_].i.z, bc);
	if (_W) VU->VF[_Fd_].i.w = Fn(VU->VF[_Fs_].i.w, bc);
}

static __fi void _vuMAX(VURegs* VU)
{
	applyMinMax<fp_max>(VU);
}

static __fi void _vuMAXi(VURegs* VU) { applyMinMaxBroadcast<fp_max>(VU, VU->VI[REG_I].UL); }
static __fi void _vuMAXx(VURegs* VU) { applyMinMaxBroadcast<fp_max>(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuMAXy(VURegs* VU) { applyMinMaxBroadcast<fp_max>(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuMAXz(VURegs* VU) { applyMinMaxBroadcast<fp_max>(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuMAXw(VURegs* VU) { applyMinMaxBroadcast<fp_max>(VU, VU->VF[_Ft_].i.w); }

static __fi void _vuMINI(VURegs* VU)
{
	applyMinMax<fp_min>(VU);
}

static __fi void _vuMINIi(VURegs* VU) { applyMinMaxBroadcast<fp_min>(VU, VU->VI[REG_I].UL); }
static __fi void _vuMINIx(VURegs* VU) { applyMinMaxBroadcast<fp_min>(VU, VU->VF[_Ft_].i.x); }
static __fi void _vuMINIy(VURegs* VU) { applyMinMaxBroadcast<fp_min>(VU, VU->VF[_Ft_].i.y); }
static __fi void _vuMINIz(VURegs* VU) { applyMinMaxBroadcast<fp_min>(VU, VU->VF[_Ft_].i.z); }
static __fi void _vuMINIw(VURegs* VU) { applyMinMaxBroadcast<fp_min>(VU, VU->VF[_Ft_].i.w); }

static __fi void _vuOPMULA(VURegs* VU)
{
	VU->ACC.i.x = VU_MACx_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Ft_].i.z));
	VU->ACC.i.y = VU_MACy_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Ft_].i.x));
	VU->ACC.i.z = VU_MACz_UPDATE(VU, vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Ft_].i.y));
	VU_STAT_UPDATE(VU);
}

static __fi void _vuOPMSUB(VURegs* VU)
{
	VECTOR* dst;
	float ftx, fty, ftz;
	float fsx, fsy, fsz;
	if (_Fd_ == 0)
		dst = &RDzero;
	else
		dst = &VU->VF[_Fd_];

	ftx = vuDouble(VU->VF[_Ft_].i.x);
	fty = vuDouble(VU->VF[_Ft_].i.y);
	ftz = vuDouble(VU->VF[_Ft_].i.z);
	fsx = vuDouble(VU->VF[_Fs_].i.x);
	fsy = vuDouble(VU->VF[_Fs_].i.y);
	fsz = vuDouble(VU->VF[_Fs_].i.z);

	dst->i.x = VU_MACx_UPDATE(VU, vuDouble(VU->ACC.i.x) - fsy * ftz);
	dst->i.y = VU_MACy_UPDATE(VU, vuDouble(VU->ACC.i.y) - fsz * ftx);
	dst->i.z = VU_MACz_UPDATE(VU, vuDouble(VU->ACC.i.z) - fsx * fty);
	VU_STAT_UPDATE(VU);
}

static __fi void _vuNOP(VURegs* VU)
{
}

template <u32 Offset>
static __fi u32 floatToInt(u32 uvalue)
{
	float fvalue = std::bit_cast<float>(uvalue);
	if (Offset)
		fvalue *= std::bit_cast<float>(0x3f800000 + (Offset << 23));
	uvalue = std::bit_cast<u32>(fvalue);

	if ((uvalue & 0x7f800000) >= 0x4f000000)
		return (uvalue & 0x80000000) ? 0x80000000 : 0x7fffffff;
	else
		return static_cast<u32>(static_cast<s32>(fvalue));
}

static __fi void _vuFTOI0 (VURegs* VU) { applyUnaryFunction<floatToInt< 0>>(VU); }
static __fi void _vuFTOI4 (VURegs* VU) { applyUnaryFunction<floatToInt< 4>>(VU); }
static __fi void _vuFTOI12(VURegs* VU) { applyUnaryFunction<floatToInt<12>>(VU); }
static __fi void _vuFTOI15(VURegs* VU) { applyUnaryFunction<floatToInt<15>>(VU); }

template <u32 Offset>
static __fi u32 intToFloat(u32 uvalue)
{
	float fvalue = static_cast<float>(static_cast<s32>(uvalue));
	if (Offset)
		fvalue *= std::bit_cast<float>(0x3f800000 - (Offset << 23));
	return std::bit_cast<u32>(fvalue);
}

static __fi void _vuITOF0 (VURegs* VU) { applyUnaryFunction<intToFloat< 0>>(VU); }
static __fi void _vuITOF4 (VURegs* VU) { applyUnaryFunction<intToFloat< 4>>(VU); }
static __fi void _vuITOF12(VURegs* VU) { applyUnaryFunction<intToFloat<12>>(VU); }
static __fi void _vuITOF15(VURegs* VU) { applyUnaryFunction<intToFloat<15>>(VU); }

static __fi void _vuCLIP(VURegs* VU)
{
	s32 value = VU->VF[_Ft_].i.w;
	// If denormal, set to the highest possible denormal value so only non-denormals compare higher
	value = (value & 0x7f800000) ? value & 0x7fffffff : 0x007fffff;
	const u32 pos = 0x00000000;
	const u32 neg = 0x80000000;

	VU->clipflag <<= 6;
	if (static_cast<s32>(VU->VF[_Fs_].i.x ^ pos) > value) VU->clipflag |= 0x01;
	if (static_cast<s32>(VU->VF[_Fs_].i.x ^ neg) > value) VU->clipflag |= 0x02;
	if (static_cast<s32>(VU->VF[_Fs_].i.y ^ pos) > value) VU->clipflag |= 0x04;
	if (static_cast<s32>(VU->VF[_Fs_].i.y ^ neg) > value) VU->clipflag |= 0x08;
	if (static_cast<s32>(VU->VF[_Fs_].i.z ^ pos) > value) VU->clipflag |= 0x10;
	if (static_cast<s32>(VU->VF[_Fs_].i.z ^ neg) > value) VU->clipflag |= 0x20;
	VU->clipflag = VU->clipflag & 0xFFFFFF;
}

/******************************/
/*   VU Lower instructions    */
/******************************/

static __fi void _vuDIV(VURegs* VU)
{
	float ft = vuDouble(VU->VF[_Ft_].UL[_Ftf_]);
	float fs = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	VU->statusflag &= ~0x30;

	if (ft == 0.0)
	{
		if (fs == 0.0)
			VU->statusflag |= 0x10;
		else
			VU->statusflag |= 0x20;

		if ((VU->VF[_Ft_].UL[_Ftf_] & 0x80000000) ^
			(VU->VF[_Fs_].UL[_Fsf_] & 0x80000000))
			VU->q.UL = 0xFF7FFFFF;
		else
			VU->q.UL = 0x7F7FFFFF;
	}
	else
	{
		VU->q.F = fs / ft;
		VU->q.F = vuDouble(VU->q.UL);
	}
}

static __fi void _vuSQRT(VURegs* VU)
{
	float ft = vuDouble(VU->VF[_Ft_].UL[_Ftf_]);

	VU->statusflag &= ~0x30;

	if (ft < 0.0)
		VU->statusflag |= 0x10;
	VU->q.F = sqrt(fabs(ft));
	VU->q.F = vuDouble(VU->q.UL);
}

static __fi void _vuRSQRT(VURegs* VU)
{
	float ft = vuDouble(VU->VF[_Ft_].UL[_Ftf_]);
	float fs = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);
	float temp;

	VU->statusflag &= ~0x30;

	if (ft == 0.0)
	{
		VU->statusflag |= 0x20;

		if (fs != 0)
		{
			if ((VU->VF[_Ft_].UL[_Ftf_] & 0x80000000) ^
				(VU->VF[_Fs_].UL[_Fsf_] & 0x80000000))
				VU->q.UL = 0xFF7FFFFF;
			else
				VU->q.UL = 0x7F7FFFFF;
		}
		else
		{
			if ((VU->VF[_Ft_].UL[_Ftf_] & 0x80000000) ^
				(VU->VF[_Fs_].UL[_Fsf_] & 0x80000000))
				VU->q.UL = 0x80000000;
			else
				VU->q.UL = 0;

			VU->statusflag |= 0x10;
		}
	}
	else
	{
		if (ft < 0.0)
		{
			VU->statusflag |= 0x10;
		}

		temp = sqrt(fabs(ft));
		VU->q.F = fs / temp;
		VU->q.F = vuDouble(VU->q.UL);
	}
}

static __fi void _vuIADDI(VURegs* VU)
{
	s16 imm = ((VU->code >> 6) & 0x1f);
	imm = ((imm & 0x10 ? 0xfff0 : 0) | (imm & 0xf));
	if (_It_ == 0)
		return;

	_vuBackupVI(VU, _It_);

	VU->VI[_It_].SS[0] = VU->VI[_Is_].SS[0] + imm;
}

static __fi void _vuIADDIU(VURegs* VU)
{
	if (_It_ == 0)
		return;

	_vuBackupVI(VU, _It_);

	VU->VI[_It_].SS[0] = VU->VI[_Is_].SS[0] + (((VU->code >> 10) & 0x7800) | (VU->code & 0x7ff));
}

static __fi void _vuIADD(VURegs* VU)
{
	if (_Id_ == 0)
		return;

	_vuBackupVI(VU, _Id_);

	VU->VI[_Id_].SS[0] = VU->VI[_Is_].SS[0] + VU->VI[_It_].SS[0];
}

static __fi void _vuIAND(VURegs* VU)
{
	if (_Id_ == 0)
		return;

	_vuBackupVI(VU, _Id_);

	VU->VI[_Id_].US[0] = VU->VI[_Is_].US[0] & VU->VI[_It_].US[0];
}

static __fi void _vuIOR(VURegs* VU)
{
	if (_Id_ == 0)
		return;

	_vuBackupVI(VU, _Id_);

	VU->VI[_Id_].US[0] = VU->VI[_Is_].US[0] | VU->VI[_It_].US[0];
}

static __fi void _vuISUB(VURegs* VU)
{
	if (_Id_ == 0)
		return;

	_vuBackupVI(VU, _Id_);

	VU->VI[_Id_].SS[0] = VU->VI[_Is_].SS[0] - VU->VI[_It_].SS[0];
}

static __fi void _vuISUBIU(VURegs* VU)
{
	if (_It_ == 0)
		return;

	_vuBackupVI(VU, _It_);

	VU->VI[_It_].SS[0] = VU->VI[_Is_].SS[0] - (((VU->code >> 10) & 0x7800) | (VU->code & 0x7ff));
}

static __fi void _vuMOVE(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].UL[0] = VU->VF[_Fs_].UL[0];
	if (_Y) VU->VF[_Ft_].UL[1] = VU->VF[_Fs_].UL[1];
	if (_Z) VU->VF[_Ft_].UL[2] = VU->VF[_Fs_].UL[2];
	if (_W) VU->VF[_Ft_].UL[3] = VU->VF[_Fs_].UL[3];
}

static __fi void _vuMFIR(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].SL[0] = (s32)VU->VI[_Is_].SS[0];
	if (_Y) VU->VF[_Ft_].SL[1] = (s32)VU->VI[_Is_].SS[0];
	if (_Z) VU->VF[_Ft_].SL[2] = (s32)VU->VI[_Is_].SS[0];
	if (_W) VU->VF[_Ft_].SL[3] = (s32)VU->VI[_Is_].SS[0];
}

static __fi void _vuMTIR(VURegs* VU)
{
	if (_It_ == 0)
		return;

	_vuBackupVI(VU, _It_);

	VU->VI[_It_].US[0] = *(u16*)&VU->VF[_Fs_].F[_Fsf_];
}

static __fi void _vuMR32(VURegs* VU)
{
	u32 tx;
	if (_Ft_ == 0)
		return;

	tx = VU->VF[_Fs_].i.x;
	if (_X) VU->VF[_Ft_].i.x = VU->VF[_Fs_].i.y;
	if (_Y) VU->VF[_Ft_].i.y = VU->VF[_Fs_].i.z;
	if (_Z) VU->VF[_Ft_].i.z = VU->VF[_Fs_].i.w;
	if (_W) VU->VF[_Ft_].i.w = tx;
}

// --------------------------------------------------------------------------------------
//  Load / Store Instructions (VU Interpreter)
// --------------------------------------------------------------------------------------

__fi u32* GET_VU_MEM(VURegs* VU, u32 addr) // non-static, also used by sVU for now.
{
	if (VU == &g_cpuRegistersPack.vuRegs[1])
		return (u32*)(g_cpuRegistersPack.vuRegs[1].Mem + (addr & 0x3fff));
	else if (addr & 0x4000)
		return (u32*)((u8*)g_cpuRegistersPack.vuRegs[1].VF + (addr & 0x3ff)); // get VF and VI regs (they're mapped to 0x4xx0 in VU0 mem!)
	else
		return (u32*)(g_cpuRegistersPack.vuRegs[0].Mem + (addr & 0xfff)); // for addr 0x0000 to 0x4000 just wrap around
}

static __ri void _vuLQ(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	s16 imm = (VU->code & 0x400) ? (VU->code & 0x3ff) | 0xfc00 : (VU->code & 0x3ff);
	u16 addr = ((imm + VU->VI[_Is_].SS[0]) * 16);

 	u32* ptr = (u32*)GET_VU_MEM(VU, addr);
	if (_X) VU->VF[_Ft_].UL[0] = ptr[0];
	if (_Y) VU->VF[_Ft_].UL[1] = ptr[1];
	if (_Z) VU->VF[_Ft_].UL[2] = ptr[2];
	if (_W) VU->VF[_Ft_].UL[3] = ptr[3];
}

static __ri void _vuLQD(VURegs* VU)
{
	_vuBackupVI(VU, _Is_);
	if (_Is_ != 0)
		VU->VI[_Is_].US[0]--;
	if (_Ft_ == 0)
		return;

	u32 addr = (VU->VI[_Is_].US[0] * 16);
	u32* ptr = (u32*)GET_VU_MEM(VU, addr);
	if (_X) VU->VF[_Ft_].UL[0] = ptr[0];
	if (_Y) VU->VF[_Ft_].UL[1] = ptr[1];
	if (_Z) VU->VF[_Ft_].UL[2] = ptr[2];
	if (_W) VU->VF[_Ft_].UL[3] = ptr[3];
}

static __ri void _vuLQI(VURegs* VU)
{
	_vuBackupVI(VU, _Is_);
	if (_Ft_)
	{
		u32 addr = (VU->VI[_Is_].US[0] * 16);
		u32* ptr = (u32*)GET_VU_MEM(VU, addr);
		if (_X) VU->VF[_Ft_].UL[0] = ptr[0];
		if (_Y) VU->VF[_Ft_].UL[1] = ptr[1];
		if (_Z) VU->VF[_Ft_].UL[2] = ptr[2];
		if (_W) VU->VF[_Ft_].UL[3] = ptr[3];
	}
	if (_Fs_ != 0)
		VU->VI[_Is_].US[0]++;
}

static __ri void _vuSQ(VURegs* VU)
{
	s16 imm = (VU->code & 0x400) ? (VU->code & 0x3ff) | 0xfc00 : (VU->code & 0x3ff);
	u16 addr = ((imm + VU->VI[_It_].SS[0]) * 16);
	u32* ptr = (u32*)GET_VU_MEM(VU, addr);
	if (_X) ptr[0] = VU->VF[_Fs_].UL[0];
	if (_Y) ptr[1] = VU->VF[_Fs_].UL[1];
	if (_Z) ptr[2] = VU->VF[_Fs_].UL[2];
	if (_W) ptr[3] = VU->VF[_Fs_].UL[3];
}

static __ri void _vuSQD(VURegs* VU)
{
	_vuBackupVI(VU, _It_);
	if (_Ft_ != 0)
		VU->VI[_It_].US[0]--;
	u32 addr = (VU->VI[_It_].US[0] * 16);
	u32* ptr = (u32*)GET_VU_MEM(VU, addr);
	if (_X) ptr[0] = VU->VF[_Fs_].UL[0];
	if (_Y) ptr[1] = VU->VF[_Fs_].UL[1];
	if (_Z) ptr[2] = VU->VF[_Fs_].UL[2];
	if (_W) ptr[3] = VU->VF[_Fs_].UL[3];
}

static __ri void _vuSQI(VURegs* VU)
{
	_vuBackupVI(VU, _It_);
	u32 addr = (VU->VI[_It_].US[0] * 16);
	u32* ptr = (u32*)GET_VU_MEM(VU, addr);
	if (_X) ptr[0] = VU->VF[_Fs_].UL[0];
	if (_Y) ptr[1] = VU->VF[_Fs_].UL[1];
	if (_Z) ptr[2] = VU->VF[_Fs_].UL[2];
	if (_W) ptr[3] = VU->VF[_Fs_].UL[3];
	if(_Ft_ != 0) VU->VI[_It_].US[0]++;
}

static __ri void _vuILW(VURegs* VU)
{
	if (_It_ == 0)
		return;

	s16 imm = (VU->code & 0x400) ? (VU->code & 0x3ff) | 0xfc00 : (VU->code & 0x3ff);
	u16 addr = ((imm + VU->VI[_Is_].SS[0]) * 16);
	u16* ptr = (u16*)GET_VU_MEM(VU, addr);

	if (_X) VU->VI[_It_].US[0] = ptr[0];
	if (_Y) VU->VI[_It_].US[0] = ptr[2];
	if (_Z) VU->VI[_It_].US[0] = ptr[4];
	if (_W) VU->VI[_It_].US[0] = ptr[6];
}

static __fi void _vuISW(VURegs* VU)
{
	s16 imm = (VU->code & 0x400) ? (VU->code & 0x3ff) | 0xfc00 : (VU->code & 0x3ff);
	u16 addr = ((imm + VU->VI[_Is_].SS[0]) * 16);
	u16* ptr = (u16*)GET_VU_MEM(VU, addr);
	if (_X) { ptr[0] = VU->VI[_It_].US[0]; ptr[1] = 0; }
	if (_Y) { ptr[2] = VU->VI[_It_].US[0]; ptr[3] = 0; }
	if (_Z) { ptr[4] = VU->VI[_It_].US[0]; ptr[5] = 0; }
	if (_W) { ptr[6] = VU->VI[_It_].US[0]; ptr[7] = 0; }
}

static __ri void _vuILWR(VURegs* VU)
{
	if (_It_ == 0)
		return;

	u32 addr = (VU->VI[_Is_].US[0] * 16);
	u16* ptr = (u16*)GET_VU_MEM(VU, addr);

	if (_X) VU->VI[_It_].US[0] = ptr[0];
	if (_Y) VU->VI[_It_].US[0] = ptr[2];
	if (_Z) VU->VI[_It_].US[0] = ptr[4];
	if (_W) VU->VI[_It_].US[0] = ptr[6];
}

static __ri void _vuISWR(VURegs* VU)
{
	u32 addr = (VU->VI[_Is_].US[0] * 16);
	u16* ptr = (u16*)GET_VU_MEM(VU, addr);
	if (_X) { ptr[0] = VU->VI[_It_].US[0]; ptr[1] = 0; }
	if (_Y) { ptr[2] = VU->VI[_It_].US[0]; ptr[3] = 0; }
	if (_Z) { ptr[4] = VU->VI[_It_].US[0]; ptr[5] = 0; }
	if (_W) { ptr[6] = VU->VI[_It_].US[0]; ptr[7] = 0; }
}

/* code contributed by _Riff_

The following code implements a Galois form M-series LFSR that can be configured to have a width from 0 to 32.
A Galois field can be represented as G(X) = g_m * X^m + g_(m-1) * X^(m-1) + ... + g_1 * X^1 + g0.
A Galois form M-Series LFSR represents a Galois field where g0 = g_m = 1 and the generated set contains 2^M - 1 values.
In modulo-2 arithmetic, addition is replaced by XOR and multiplication is replaced by AND.
The code is written in such a way that the polynomial lsb (g0) should be set to 0 and g_m is not represented.
As an example for setting the polynomial variable correctly, the 23-bit M-series generating polynomial X^23+X^14
  would be specified as (1 << 14).
*/

// Unused
#if 0
//The two-tap 23 stage M-series polynomials are x23+x18 and x23+x14 ((1 << 18) and (1 << 14), respectively).
//The reverse sequences can be generated by x23+x(23-18) and x23+x(23-14) ((1 << 9) and (1 << 5), respectively)
static u32 poly = 1 << 5;

static __ri void SetPoly(u32 newPoly)
{
	poly = poly & ~1;
}
#endif

static __ri void AdvanceLFSR(VURegs* VU)
{
	// code from www.project-fao.org (which is no longer there)
	int x = (VU->VI[REG_R].UL >> 4) & 1;
	int y = (VU->VI[REG_R].UL >> 22) & 1;
	VU->VI[REG_R].UL <<= 1;
	VU->VI[REG_R].UL ^= x ^ y;
	VU->VI[REG_R].UL = (VU->VI[REG_R].UL & 0x7fffff) | 0x3f800000;
}

static __ri void _vuRINIT(VURegs* VU)
{
	VU->VI[REG_R].UL = 0x3F800000 | (VU->VF[_Fs_].UL[_Fsf_] & 0x007FFFFF);
}

static __ri void _vuRGET(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].UL[0] = VU->VI[REG_R].UL;
	if (_Y) VU->VF[_Ft_].UL[1] = VU->VI[REG_R].UL;
	if (_Z) VU->VF[_Ft_].UL[2] = VU->VI[REG_R].UL;
	if (_W) VU->VF[_Ft_].UL[3] = VU->VI[REG_R].UL;
}

static __ri void _vuRNEXT(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	AdvanceLFSR(VU);
	if (_X) VU->VF[_Ft_].UL[0] = VU->VI[REG_R].UL;
	if (_Y) VU->VF[_Ft_].UL[1] = VU->VI[REG_R].UL;
	if (_Z) VU->VF[_Ft_].UL[2] = VU->VI[REG_R].UL;
	if (_W) VU->VF[_Ft_].UL[3] = VU->VI[REG_R].UL;
}

static __ri void _vuRXOR(VURegs* VU)
{
	VU->VI[REG_R].UL = 0x3F800000 | ((VU->VI[REG_R].UL ^ VU->VF[_Fs_].UL[_Fsf_]) & 0x007FFFFF);
}

static __ri void _vuWAITQ(VURegs* VU)
{
}

static __ri void _vuFSAND(VURegs* VU)
{
	u16 imm;

	imm = (((VU->code >> 21) & 0x1) << 11) | (VU->code & 0x7ff);
	if (_It_ == 0)
		return;

	VU->VI[_It_].US[0] = (VU->VI[REG_STATUS_FLAG].US[0] & 0xFFF) & imm;
}

static __ri void _vuFSEQ(VURegs* VU)
{
	u16 imm;

	imm = (((VU->code >> 21) & 0x1) << 11) | (VU->code & 0x7ff);
	if (_It_ == 0)
		return;

	if ((VU->VI[REG_STATUS_FLAG].US[0] & 0xFFF) == imm)
		VU->VI[_It_].US[0] = 1;
	else
		VU->VI[_It_].US[0] = 0;
}

static __ri void _vuFSOR(VURegs* VU)
{
	u16 imm;

	imm = (((VU->code >> 21) & 0x1) << 11) | (VU->code & 0x7ff);
	if (_It_ == 0)
		return;
	VU->VI[_It_].US[0] = (VU->VI[REG_STATUS_FLAG].US[0] & 0xFFF) | imm;
}

static __ri void _vuFSSET(VURegs* VU)
{
	u16 imm = 0;

	imm = (((VU->code >> 21) & 0x1) << 11) | (VU->code & 0x7FF);
	VU->statusflag = (imm & 0xFC0) | (VU->statusflag & 0x3F);
}

static __ri void _vuFMAND(VURegs* VU)
{
	if (_It_ == 0)
		return;

	VU->VI[_It_].US[0] = VU->VI[_Is_].US[0] & (VU->VI[REG_MAC_FLAG].UL & 0xFFFF);
}

static __fi void _vuFMEQ(VURegs* VU)
{
	if (_It_ == 0)
		return;

	if ((VU->VI[REG_MAC_FLAG].UL & 0xFFFF) == VU->VI[_Is_].US[0])
		VU->VI[_It_].US[0] = 1;
	else
		VU->VI[_It_].US[0] = 0;
}

static __fi void _vuFMOR(VURegs* VU)
{
	if (_It_ == 0)
		return;

	VU->VI[_It_].US[0] = (VU->VI[REG_MAC_FLAG].UL & 0xFFFF) | VU->VI[_Is_].US[0];
}

static __fi void _vuFCAND(VURegs* VU)
{
	if ((VU->VI[REG_CLIP_FLAG].UL & 0xFFFFFF) & (VU->code & 0xFFFFFF))
		VU->VI[1].US[0] = 1;
	else
		VU->VI[1].US[0] = 0;
}

static __fi void _vuFCEQ(VURegs* VU)
{
	if ((VU->VI[REG_CLIP_FLAG].UL & 0xFFFFFF) == (VU->code & 0xFFFFFF))
		VU->VI[1].US[0] = 1;
	else
		VU->VI[1].US[0] = 0;
}

static __fi void _vuFCOR(VURegs* VU)
{
	u32 hold = (VU->VI[REG_CLIP_FLAG].UL & 0xFFFFFF) | (VU->code & 0xFFFFFF);
	if (hold == 0xFFFFFF)
		VU->VI[1].US[0] = 1;
	else
		VU->VI[1].US[0] = 0;
}

static __fi void _vuFCSET(VURegs* VU)
{
	VU->clipflag = (u32)(VU->code & 0xFFFFFF);
}

static __fi void _vuFCGET(VURegs* VU)
{
	if (_It_ == 0)
		return;

	VU->VI[_It_].US[0] = VU->VI[REG_CLIP_FLAG].UL & 0x0FFF;
}

s32 _branchAddr(VURegs* VU)
{
	s32 bpc = VU->VI[REG_TPC].SL + (_Imm11_ * 8);
	bpc &= (VU == &VU1) ? 0x3fff : 0x0fff;
	return bpc;
}

static __fi void _setBranch(VURegs* VU, u32 bpc)
{
	if (VU->branch == 1)
	{
		//DevCon.Warning("Branch in Branch Delay slot!");
		VU->delaybranchpc = bpc;
		VU->takedelaybranch = true;
	}
	else
	{
		VU->branch = 2;
		VU->branchpc = bpc;
	}
}

static __ri void _vuIBEQ(VURegs* VU)
{
	s16 dest = VU->VI[_It_].US[0];
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _It_)
			dest = VU->VIOldValue;

		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif
	if (dest == src)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuIBGEZ(VURegs* VU)
{
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif
	if (src >= 0)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuIBGTZ(VURegs* VU)
{
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif

	if (src > 0)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuIBLEZ(VURegs* VU)
{
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif
	if (src <= 0)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuIBLTZ(VURegs* VU)
{
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif
	if (src < 0)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuIBNE(VURegs* VU)
{
	s16 dest = VU->VI[_It_].US[0];
	s16 src = VU->VI[_Is_].US[0];
#ifdef VI_BACKUP
	if (VU->VIBackupCycles > 0)
	{
		if (VU->VIRegNumber == _It_)
			dest = VU->VIOldValue;

		if (VU->VIRegNumber == _Is_)
			src = VU->VIOldValue;
	}
#endif
	if (dest != src)
	{
		s32 bpc = _branchAddr(VU);
		_setBranch(VU, bpc);
	}
}

static __ri void _vuB(VURegs* VU)
{
	s32 bpc = _branchAddr(VU);
	_setBranch(VU, bpc);
}

static __ri void _vuBAL(VURegs* VU)
{
	s32 bpc = _branchAddr(VU);


	if (_It_)
	{
		//If we are in the branch delay slot, the instruction after the first
		//instruction in the first branches target becomes the linked reg.
		if (VU->branch == 1)
			VU->VI[_It_].US[0] = (VU->branchpc + 8) / 8;
		else
			VU->VI[_It_].US[0] = (VU->VI[REG_TPC].UL + 8) / 8;
	}

	_setBranch(VU, bpc);
}

static __ri void _vuJR(VURegs* VU)
{
	u32 bpc = VU->VI[_Is_].US[0] * 8;
	_setBranch(VU, bpc);
}

//If this is in a branch delay, the jump isn't taken ( Evil Dead - Fistfull of Boomstick )
static __ri void _vuJALR(VURegs* VU)
{
	u32 bpc = VU->VI[_Is_].US[0] * 8;

	if (_It_)
	{
		//If we are in the branch delay slot, the instruction after the first
		//instruction in the first branches target becomes the linked reg.
		if (VU->branch == 1)
			VU->VI[_It_].US[0] = (VU->branchpc + 8) / 8;
		else
			VU->VI[_It_].US[0] = (VU->VI[REG_TPC].UL + 8) / 8;
	}

	_setBranch(VU, bpc);
}

static __ri void _vuMFP(VURegs* VU)
{
	if (_Ft_ == 0)
		return;

	if (_X) VU->VF[_Ft_].i.x = VU->VI[REG_P].UL;
	if (_Y) VU->VF[_Ft_].i.y = VU->VI[REG_P].UL;
	if (_Z) VU->VF[_Ft_].i.z = VU->VI[REG_P].UL;
	if (_W) VU->VF[_Ft_].i.w = VU->VI[REG_P].UL;
}

static __ri void _vuWAITP(VURegs* VU)
{
}

static __ri void _vuESADD(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Fs_].i.x) + vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Fs_].i.y) + vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Fs_].i.z);

	VU->p.F = p;
}

static __ri void _vuERSADD(VURegs* VU)
{
	float p = (vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Fs_].i.x)) + (vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Fs_].i.y)) + (vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Fs_].i.z));

	if (p != 0.0)
		p = 1.0f / p;

	VU->p.F = p;
}

static __ri void _vuELENG(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Fs_].i.x) + vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Fs_].i.y) + vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Fs_].i.z);

	if (p >= 0)
	{
		p = sqrt(p);
	}
	VU->p.F = p;
}

static __ri void _vuERLENG(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].i.x) * vuDouble(VU->VF[_Fs_].i.x) + vuDouble(VU->VF[_Fs_].i.y) * vuDouble(VU->VF[_Fs_].i.y) + vuDouble(VU->VF[_Fs_].i.z) * vuDouble(VU->VF[_Fs_].i.z);

	if (p >= 0)
	{
		p = sqrt(p);
		if (p != 0)
		{
			p = 1.0f / p;
		}
	}
	VU->p.F = p;
}


static __ri float _vuCalculateEATAN(float inputvalue) {
	float eatanconst[9] = { 0.999999344348907f, -0.333298563957214f, 0.199465364217758f, -0.13085337519646f,
							0.096420042216778f, -0.055909886956215f, 0.021861229091883f, -0.004054057877511f,
							0.785398185253143f };

	float result = (eatanconst[0] * inputvalue) + (eatanconst[1] * pow(inputvalue, 3)) + (eatanconst[2] * pow(inputvalue, 5))
					+ (eatanconst[3] * pow(inputvalue, 7)) + (eatanconst[4] * pow(inputvalue, 9)) + (eatanconst[5] * pow(inputvalue, 11))
					+ (eatanconst[6] * pow(inputvalue, 13)) + (eatanconst[7] * pow(inputvalue, 15));

	result += eatanconst[8];

	result = vuDouble(*(u32*)&result);

	return result;
}

static __ri void _vuEATAN(VURegs* VU)
{
	float p = _vuCalculateEATAN(vuDouble(VU->VF[_Fs_].UL[_Fsf_]));
	VU->p.F = p;
}

static __ri void _vuEATANxy(VURegs* VU)
{
	float p = 0;
	if (vuDouble(VU->VF[_Fs_].i.x) != 0)
	{
		p = _vuCalculateEATAN(vuDouble(VU->VF[_Fs_].i.y) / vuDouble(VU->VF[_Fs_].i.x));
	}
	VU->p.F = p;
}

static __ri void _vuEATANxz(VURegs* VU)
{
	float p = 0;
	if (vuDouble(VU->VF[_Fs_].i.x) != 0)
	{
		p = _vuCalculateEATAN(vuDouble(VU->VF[_Fs_].i.z) / vuDouble(VU->VF[_Fs_].i.x));
	}
	VU->p.F = p;
}

static __ri void _vuESUM(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].i.x) + vuDouble(VU->VF[_Fs_].i.y) + vuDouble(VU->VF[_Fs_].i.z) + vuDouble(VU->VF[_Fs_].i.w);
	VU->p.F = p;
}

static __ri void _vuERCPR(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	if (p != 0)
	{
		p = 1.0 / p;
	}

	VU->p.F = p;
}

static __ri void _vuESQRT(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	if (p >= 0)
	{
		p = sqrt(p);
	}

	VU->p.F = p;
}

static __ri void _vuERSQRT(VURegs* VU)
{
	float p = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	if (p >= 0)
	{
		p = sqrt(p);
		if (p)
		{
			p = 1.0f / p;
		}
	}

	VU->p.F = p;
}

static __ri void _vuESIN(VURegs* VU)
{
	float sinconsts[5] = {1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f, 0.000002601886990f};
	float p = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	p = (sinconsts[0] * p) + (sinconsts[1] * pow(p, 3)) + (sinconsts[2] * pow(p, 5)) + (sinconsts[3] * pow(p, 7)) + (sinconsts[4] * pow(p, 9));
	VU->p.F = vuDouble(*(u32*)&p);
}

static __ri void _vuEEXP(VURegs* VU)
{
	float consts[6] = {0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
						0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
	float p = vuDouble(VU->VF[_Fs_].UL[_Fsf_]);

	p = 1.0f + (consts[0] * p) + (consts[1] * pow(p, 2)) + (consts[2] * pow(p, 3)) + (consts[3] * pow(p, 4)) + (consts[4] * pow(p, 5)) + (consts[5] * pow(p, 6));
	p = pow(p, 4);
	p = vuDouble(*(u32*)&p);
	p = 1 / p;

	VU->p.F = p;
}

static __ri void _vuXITOP(VURegs* VU)
{
	if (_It_ == 0)
		return;

	if (VU == &VU1 && THREAD_VU1)
		VU->VI[_It_].US[0] = vu1Thread.vifRegs.itop;
	else
		VU->VI[_It_].US[0] = VU->GetVifRegs().itop;
}

void _vuXGKICKTransfer(s32 cycles, bool flush)
{
	if (!VU1.xgkickenable)
		return;

	VU1.xgkickcyclecount += cycles;
	VU1.xgkicklastcycle += cycles;

	VUM_LOG("Adding %d cycles, total XGKick cycles to run now %d flush %d enabled %d", cycles, VU1.xgkickcyclecount, flush, VU1.xgkickenable);

	while (VU1.xgkickenable && (flush || VU1.xgkickcyclecount >= 2))
	{
		u32 transfersize = 0;

		if (VU1.xgkicksizeremaining == 0)
		{
			VUM_LOG("XGKICK reading new tag from %x", VU1.xgkickaddr);
			u32 size = gifUnit.GetGSPacketSize(GIF_PATH_1, g_cpuRegistersPack.vuRegs[1].Mem, VU1.xgkickaddr, ~0u, flush);
			VU1.xgkicksizeremaining = size & 0xFFFF;
			VU1.xgkickendpacket = size >> 31;
			VU1.xgkickdiff = 0x4000 - VU1.xgkickaddr;

			if (VU1.xgkicksizeremaining == 0)
			{
				VUM_LOG("Invalid GS packet size returned, cancelling XGKick");
				VU1.xgkickenable = false;
				break;
			}
			else
				VUM_LOG("XGKICK New tag size %d bytes EOP %d", VU1.xgkicksizeremaining, VU1.xgkickendpacket);
		}

		if (!flush)
		{
			transfersize = std::min(VU1.xgkicksizeremaining / 0x10, VU1.xgkickcyclecount / 2);
			transfersize = std::min(transfersize, VU1.xgkickdiff / 0x10);
		}
		else
		{
			transfersize = VU1.xgkicksizeremaining / 0x10;
			transfersize = std::min(transfersize, VU1.xgkickdiff / 0x10);
		}

		VUM_LOG("XGKICK Transferring %x bytes from %x size %x", transfersize * 0x10, VU1.xgkickaddr, VU1.xgkicksizeremaining);

		// Would be "nicer" to do the copy until it's all up, however this really screws up PATH3 masking stuff
		// So lets just do it the other way :)
		/*if (THREAD_VU1)
		{
			if ((transfersize * 0x10) < VU1.xgkicksizeremaining)
				gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData(&VU1.Mem[VU1.xgkickaddr], transfersize * 0x10, true);
			else
				gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &g_cpuRegistersPack.vuRegs[1].Mem[VU1.xgkickaddr], transfersize * 0x10, true);
		}
		else*/
		//{
			gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, &g_cpuRegistersPack.vuRegs[1].Mem[VU1.xgkickaddr], transfersize * 0x10, true);
		//}

		if ((VU0.VI[REG_VPU_STAT].UL & 0x100) && flush)
			VU1.cycle += transfersize * 2;

		VU1.xgkickcyclecount -= transfersize * 2;

		VU1.xgkickaddr = (VU1.xgkickaddr + (transfersize * 0x10)) & 0x3FFF;
		VU1.xgkicksizeremaining -= (transfersize * 0x10);
		VU1.xgkickdiff = 0x4000 - VU1.xgkickaddr;

		if (VU1.xgkicksizeremaining || !VU1.xgkickendpacket)
			VUM_LOG("XGKICK next addr %x left size %x", VU1.xgkickaddr, VU1.xgkicksizeremaining);
		else
		{
			VUM_LOG("XGKICK transfer finished");
			VU1.xgkickenable = false;
			VU0.VI[REG_VPU_STAT].UL &= ~(1 << 12);
			// Check if VIF is waiting for the GIF to not be busy
			if (vif1Regs.stat.VGW)
			{
				vif1Regs.stat.VGW = false;
				CPU_INT(DMAC_VIF1, 8);
			}
		}
	}
	if (flush)
	{
		VUM_LOG("Disabling XGKICK");
		_vuTestPipes(&VU1);
	}
	VUM_LOG("XGKick run complete Enabled %d", VU1.xgkickenable);
}

static __ri void _vuXGKICK(VURegs* VU)
{
	if (VU->xgkickenable)
		_vuXGKICKTransfer(0, true);

	u32 addr = (VU->VI[_Is_].US[0] & 0x3ff) * 16;
	u32 diff = 0x4000 - addr;

	VU->xgkickenable = true;
	VU->xgkickaddr = addr;
	VU->xgkickdiff = diff;
	VU->xgkicksizeremaining = 0;
	VU->xgkickendpacket = false;
	VU->xgkicklastcycle = VU->cycle;
	// XGKick command counts as one cycle for the transfer.
	// Can be tested with Resident Evil: Outbreak, Kingdom Hearts, CART Fury.
	VU->xgkickcyclecount = 1;
	VU0.VI[REG_VPU_STAT].UL |= (1 << 12);
	VUM_LOG("XGKICK addr %x", addr);
}

static __ri void _vuXTOP(VURegs* VU)
{
	if (_It_ == 0)
		return;

	if (VU == &VU1 && THREAD_VU1)
		VU->VI[_It_].US[0] = (u16)vu1Thread.vifRegs.top;
	else
		VU->VI[_It_].US[0] = (u16)VU->GetVifRegs().top;
}

#define GET_VF0_FLAG(reg) (((reg) == 0) ? (1 << REG_VF0_FLAG) : 0)

#define VUREGS_FDFSI(OP, ACC) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Fd_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = (1 << REG_I)|((ACC)?(1<<REG_ACC_FLAG):0)|GET_VF0_FLAG(_Fs_); \
}

#define VUREGS_FDFSQ(OP, ACC) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Fd_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = (1 << REG_Q)|((ACC)?(1<<REG_ACC_FLAG):0)|GET_VF0_FLAG(_Fs_); \
}

#define VUREGS_FDFSFT(OP, ACC) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Fd_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = _Ft_; \
	VUregsn->VFr1xyzw= _XYZW; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = ((ACC)?(1<<REG_ACC_FLAG):0)|GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_); \
}

#define VUREGS_FDFSFTxyzw(OP, xyzw, ACC) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Fd_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = _Ft_; \
	VUregsn->VFr1xyzw= xyzw; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = ((ACC)?(1<<REG_ACC_FLAG):0)|GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_); \
}

#define VUREGS_FDFSFTx(OP, ACC) VUREGS_FDFSFTxyzw(OP, 8, ACC)
#define VUREGS_FDFSFTy(OP, ACC) VUREGS_FDFSFTxyzw(OP, 4, ACC)
#define VUREGS_FDFSFTz(OP, ACC) VUREGS_FDFSFTxyzw(OP, 2, ACC)
#define VUREGS_FDFSFTw(OP, ACC) VUREGS_FDFSFTxyzw(OP, 1, ACC)


#define VUREGS_ACCFSI(OP, readacc) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFwxyzw= _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = (1<<REG_ACC_FLAG); \
	VUregsn->VIread  = (1 << REG_I)|GET_VF0_FLAG(_Fs_)|(((readacc)||_XYZW!=15)?(1<<REG_ACC_FLAG):0); \
}

#define VUREGS_ACCFSQ(OP, readacc) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFwxyzw= _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = (1<<REG_ACC_FLAG); \
	VUregsn->VIread  = (1 << REG_Q)|GET_VF0_FLAG(_Fs_)|(((readacc)||_XYZW!=15)?(1<<REG_ACC_FLAG):0); \
}

#define VUREGS_ACCFSFT(OP, readacc) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFwxyzw= _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = _Ft_; \
	VUregsn->VFr1xyzw= _XYZW; \
	VUregsn->VIwrite = (1<<REG_ACC_FLAG); \
	VUregsn->VIread  = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_)|(((readacc)||_XYZW!=15)?(1<<REG_ACC_FLAG):0); \
}

#define VUREGS_ACCFSFTxyzw(OP, xyzw, readacc) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFwxyzw= _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = _Ft_; \
	VUregsn->VFr1xyzw= xyzw; \
	VUregsn->VIwrite = (1<<REG_ACC_FLAG); \
	VUregsn->VIread  = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_)|(((readacc)||_XYZW!=15)?(1<<REG_ACC_FLAG):0); \
}

#define VUREGS_ACCFSFTx(OP, readacc) VUREGS_ACCFSFTxyzw(OP, 8, readacc)
#define VUREGS_ACCFSFTy(OP, readacc) VUREGS_ACCFSFTxyzw(OP, 4, readacc)
#define VUREGS_ACCFSFTz(OP, readacc) VUREGS_ACCFSFTxyzw(OP, 2, readacc)
#define VUREGS_ACCFSFTw(OP, readacc) VUREGS_ACCFSFTxyzw(OP, 1, readacc)

#define VUREGS_FTFS(OP) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Ft_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
	VUregsn->VFr1xyzw = 0xff; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = (_Ft_ ? GET_VF0_FLAG(_Fs_) : 0); \
}

#define VUREGS_IDISIT(OP) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_IALU; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFread0 = 0; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = 1 << _Id_; \
	VUregsn->VIread  = (1 << _Is_) | (1 << _It_); \
	VUregsn->cycles  = 0; \
}

#define VUREGS_ITIS(OP) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_IALU; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFread0 = 0; \
	VUregsn->VFread1 = 0; \
	VUregsn->VIwrite = 1 << _It_; \
	VUregsn->VIread  = 1 << _Is_; \
	VUregsn->cycles  = 0; \
}

#define VUREGS_PFS_xyzw(OP, _cycles) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_EFU; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = 0; \
    VUregsn->VIwrite = 1 << REG_P; \
	VUregsn->VIread  = GET_VF0_FLAG(_Fs_); \
	VUregsn->cycles  = _cycles; \
}

#define VUREGS_PFS_fsf(OP, _cycles) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_EFU; \
	VUregsn->VFwrite = 0; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= 1 << (3-_Fsf_); \
	VUregsn->VFread1 = 0; \
    VUregsn->VIwrite = 1 << REG_P; \
	VUregsn->VIread  = GET_VF0_FLAG(_Fs_); \
	VUregsn->cycles  = _cycles; \
}

VUREGS_FTFS(ABS);

VUREGS_FDFSFT(ADD, 0);
VUREGS_FDFSI(ADDi, 0);
VUREGS_FDFSQ(ADDq, 0);
VUREGS_FDFSFTx(ADDx, 0);
VUREGS_FDFSFTy(ADDy, 0);
VUREGS_FDFSFTz(ADDz, 0);
VUREGS_FDFSFTw(ADDw, 0);

VUREGS_ACCFSFT(ADDA, 0);
VUREGS_ACCFSI(ADDAi, 0);
VUREGS_ACCFSQ(ADDAq, 0);
VUREGS_ACCFSFTx(ADDAx, 0);
VUREGS_ACCFSFTy(ADDAy, 0);
VUREGS_ACCFSFTz(ADDAz, 0);
VUREGS_ACCFSFTw(ADDAw, 0);

VUREGS_FDFSFT(SUB, 0);
VUREGS_FDFSI(SUBi, 0);
VUREGS_FDFSQ(SUBq, 0);
VUREGS_FDFSFTx(SUBx, 0);
VUREGS_FDFSFTy(SUBy, 0);
VUREGS_FDFSFTz(SUBz, 0);
VUREGS_FDFSFTw(SUBw, 0);

VUREGS_ACCFSFT(SUBA, 0);
VUREGS_ACCFSI(SUBAi, 0);
VUREGS_ACCFSQ(SUBAq, 0);
VUREGS_ACCFSFTx(SUBAx, 0);
VUREGS_ACCFSFTy(SUBAy, 0);
VUREGS_ACCFSFTz(SUBAz, 0);
VUREGS_ACCFSFTw(SUBAw, 0);

#define VUREGS_FDFSFTxyzw_MUL(OP, ACC, xyzw) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
		VUregsn->pipe = VUPIPE_FMAC; \
		VUregsn->VFwrite = (ACC)?0:_Fd_; \
		VUregsn->VFwxyzw = _XYZW; \
		VUregsn->VFread0 = _Fs_; \
		VUregsn->VFr0xyzw= _XYZW; \
		VUregsn->VFread1 = _Ft_; \
		VUregsn->VFr1xyzw= xyzw; \
		VUregsn->VIwrite = ((ACC)?(1<<REG_ACC_FLAG):0); \
		VUregsn->VIread  = GET_VF0_FLAG(_Fs_)|(((ACC)&&(_XYZW!=15))?(1<<REG_ACC_FLAG):0); \
}

VUREGS_FDFSFT(MUL, 0);
VUREGS_FDFSI(MULi, 0);
VUREGS_FDFSQ(MULq, 0);
VUREGS_FDFSFTxyzw_MUL(MULx, 0, 8);
VUREGS_FDFSFTxyzw_MUL(MULy, 0, 4);
VUREGS_FDFSFTxyzw_MUL(MULz, 0, 2);
VUREGS_FDFSFTxyzw_MUL(MULw, 0, 1);

VUREGS_ACCFSFT(MULA, 0);
VUREGS_ACCFSI(MULAi, 0);
VUREGS_ACCFSQ(MULAq, 0);
VUREGS_FDFSFTxyzw_MUL(MULAx, 1, 8);
VUREGS_FDFSFTxyzw_MUL(MULAy, 1, 4);
VUREGS_FDFSFTxyzw_MUL(MULAz, 1, 2);
VUREGS_FDFSFTxyzw_MUL(MULAw, 1, 1);

VUREGS_FDFSFT(MADD, 1);
VUREGS_FDFSI(MADDi, 1);
VUREGS_FDFSQ(MADDq, 1);

#define VUREGS_FDFSFT_0_xyzw(OP, xyzw) \
static __ri void _vuRegs##OP(const VURegs* VU, _VURegsNum *VUregsn) { \
	VUregsn->pipe = VUPIPE_FMAC; \
	VUregsn->VFwrite = _Fd_; \
	VUregsn->VFwxyzw = _XYZW; \
	VUregsn->VFread0 = _Fs_; \
	VUregsn->VFr0xyzw= _XYZW; \
	VUregsn->VFread1 = _Ft_; \
	VUregsn->VFr1xyzw= xyzw; \
	VUregsn->VIwrite = 0; \
	VUregsn->VIread  = (1<<REG_ACC_FLAG)|(_Ft_ ? GET_VF0_FLAG(_Fs_) : 0); \
}

VUREGS_FDFSFT_0_xyzw(MADDx, 8);
VUREGS_FDFSFT_0_xyzw(MADDy, 4);
VUREGS_FDFSFT_0_xyzw(MADDz, 2);

static __ri void _vuRegsMADDw(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
	VUregsn->VFwrite = _Fd_;
	VUregsn->VFwxyzw = _XYZW;
	VUregsn->VFread0 = _Fs_;
	VUregsn->VFr0xyzw= _XYZW;
	VUregsn->VFread1 = _Ft_;
	VUregsn->VFr1xyzw= 1;
	VUregsn->VIwrite = 0;
	VUregsn->VIread  = (1<<REG_ACC_FLAG)|GET_VF0_FLAG(_Fs_);
}

VUREGS_ACCFSFT(MADDA, 1);
VUREGS_ACCFSI(MADDAi, 1);
VUREGS_ACCFSQ(MADDAq, 1);
VUREGS_ACCFSFTx(MADDAx, 1);
VUREGS_ACCFSFTy(MADDAy, 1);
VUREGS_ACCFSFTz(MADDAz, 1);
VUREGS_ACCFSFTw(MADDAw, 1);

VUREGS_FDFSFT(MSUB, 1);
VUREGS_FDFSI(MSUBi, 1);
VUREGS_FDFSQ(MSUBq, 1);
VUREGS_FDFSFTx(MSUBx, 1);
VUREGS_FDFSFTy(MSUBy, 1);
VUREGS_FDFSFTz(MSUBz, 1);
VUREGS_FDFSFTw(MSUBw, 1);

VUREGS_ACCFSFT(MSUBA, 1);
VUREGS_ACCFSI(MSUBAi, 1);
VUREGS_ACCFSQ(MSUBAq, 1);
VUREGS_ACCFSFTx(MSUBAx, 1);
VUREGS_ACCFSFTy(MSUBAy, 1);
VUREGS_ACCFSFTz(MSUBAz, 1);
VUREGS_ACCFSFTw(MSUBAw, 1);

VUREGS_FDFSFT(MAX, 0);
VUREGS_FDFSI(MAXi, 0);
VUREGS_FDFSFTx(MAXx_, 0);
VUREGS_FDFSFTy(MAXy_, 0);
VUREGS_FDFSFTz(MAXz_, 0);
VUREGS_FDFSFTw(MAXw_, 0);

static __ri void _vuRegsMAXx(const VURegs* VU, _VURegsNum* VUregsn)
{
	_vuRegsMAXx_(VU, VUregsn);
}
static __ri void _vuRegsMAXy(const VURegs* VU, _VURegsNum* VUregsn)
{
	_vuRegsMAXy_(VU, VUregsn);
}
static __ri void _vuRegsMAXz(const VURegs* VU, _VURegsNum* VUregsn)
{
	_vuRegsMAXz_(VU, VUregsn);
}
static __ri void _vuRegsMAXw(const VURegs* VU, _VURegsNum* VUregsn)
{
	_vuRegsMAXw_(VU, VUregsn);
}

VUREGS_FDFSFT(MINI, 0);
VUREGS_FDFSI(MINIi, 0);
VUREGS_FDFSFTx(MINIx, 0);
VUREGS_FDFSFTy(MINIy, 0);
VUREGS_FDFSFTz(MINIz, 0);
VUREGS_FDFSFTw(MINIw, 0);

static __ri void _vuRegsOPMULA(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
	VUregsn->VFwrite = 0;
	VUregsn->VFwxyzw= 0xE;
	VUregsn->VFread0 = _Fs_;
	VUregsn->VFr0xyzw= 0xE;
	VUregsn->VFread1 = _Ft_;
	VUregsn->VFr1xyzw= 0xE;
	VUregsn->VIwrite = 1<<REG_ACC_FLAG;
	VUregsn->VIread  = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_)|(1<<REG_ACC_FLAG);
}

static __ri void _vuRegsOPMSUB(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
	VUregsn->VFwrite = _Fd_;
	VUregsn->VFwxyzw= 0xE;
	VUregsn->VFread0 = _Fs_;
	VUregsn->VFr0xyzw= 0xE;
	VUregsn->VFread1 = _Ft_;
	VUregsn->VFr1xyzw= 0xE;
	VUregsn->VIwrite = 0;
	VUregsn->VIread  = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_)|(1<<REG_ACC_FLAG);
}

static __ri void _vuRegsNOP(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_NONE;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 0;
}

VUREGS_FTFS(FTOI0);
VUREGS_FTFS(FTOI4);
VUREGS_FTFS(FTOI12);
VUREGS_FTFS(FTOI15);
VUREGS_FTFS(ITOF0);
VUREGS_FTFS(ITOF4);
VUREGS_FTFS(ITOF12);
VUREGS_FTFS(ITOF15);

static __ri void _vuRegsCLIP(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = _Fs_;
	VUregsn->VFr0xyzw= 0xE;
	VUregsn->VFread1 = _Ft_;
	VUregsn->VFr1xyzw= 0x1;
    VUregsn->VIwrite = 1 << REG_CLIP_FLAG;
    VUregsn->VIread  = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_)|(1 << REG_CLIP_FLAG);
}

/******************************/
/*   VU Lower instructions    */
/******************************/

static __ri void _vuRegsDIV(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FDIV;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = _Fs_;
	VUregsn->VFr0xyzw= 1 << (3-_Fsf_);
	VUregsn->VFread1 = _Ft_;
	VUregsn->VFr1xyzw= 1 << (3-_Ftf_);
    VUregsn->VIwrite = 1 << REG_Q;
    VUregsn->VIread  = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_);
	VUregsn->cycles  = 7;
}

static __ri void _vuRegsSQRT(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FDIV;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = 0;
	VUregsn->VFr0xyzw = 0;
    VUregsn->VFread1 = _Ft_;
	VUregsn->VFr1xyzw = 1 << (3-_Ftf_);
    VUregsn->VIwrite = 1 << REG_Q;
    VUregsn->VIread  = GET_VF0_FLAG(_Ft_);
	VUregsn->cycles  = 7;
}

static __ri void _vuRegsRSQRT(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FDIV;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = _Fs_;
	VUregsn->VFr0xyzw= 1 << (3-_Fsf_);
	VUregsn->VFread1 = _Ft_;
	VUregsn->VFr1xyzw= 1 << (3-_Ftf_);
    VUregsn->VIwrite = 1 << REG_Q;
    VUregsn->VIread  = GET_VF0_FLAG(_Fs_)|GET_VF0_FLAG(_Ft_);
	VUregsn->cycles  = 13;
}

VUREGS_ITIS(IADDI);
VUREGS_ITIS(IADDIU);
VUREGS_IDISIT(IADD);
VUREGS_IDISIT(IAND);
VUREGS_IDISIT(IOR);
VUREGS_IDISIT(ISUB);
VUREGS_ITIS(ISUBIU);

static __ri void _vuRegsMOVE(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = _Ft_ == 0 ? VUPIPE_NONE : VUPIPE_FMAC;
	VUregsn->VFwrite = _Ft_;
	VUregsn->VFwxyzw = _XYZW;
	VUregsn->VFread0 = _Fs_;
	VUregsn->VFr0xyzw = _XYZW;
	VUregsn->VFread1 = 0;
	VUregsn->VFr1xyzw = 0;
	VUregsn->VIwrite = 0;
	VUregsn->VIread = (_Ft_ ? GET_VF0_FLAG(_Fs_) : 0);
}
static __ri void _vuRegsMFIR(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = _Ft_;
    VUregsn->VFwxyzw = _XYZW;
	VUregsn->VFread0 = 0;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsMTIR(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = _Fs_;
    VUregsn->VFr0xyzw= 1 << (3-_Fsf_);
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = GET_VF0_FLAG(_Fs_);
}

static __ri void _vuRegsMR32(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
	VUregsn->VFwrite = _Ft_;
	VUregsn->VFwxyzw = _XYZW;
	VUregsn->VFread0 = _Fs_;
	VUregsn->VFr0xyzw= (_XYZW >> 1) | ((_XYZW << 3) & 0x8);  //rotate
	VUregsn->VFread1 = 0;
	VUregsn->VFr1xyzw = 0xff;
	VUregsn->VIwrite = 0;
	VUregsn->VIread  = (_Ft_ ? GET_VF0_FLAG(_Fs_) : 0);
}

static __ri void _vuRegsLQ(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = _Ft_;
    VUregsn->VFwxyzw = _XYZW;
	VUregsn->VFread0 = 0;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsLQD(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = _Ft_;
    VUregsn->VFwxyzw = _XYZW;
	VUregsn->VFread0 = 0;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _Is_;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsLQI(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = _Ft_;
    VUregsn->VFwxyzw = _XYZW;
	VUregsn->VFread0 = 0;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _Is_;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsSQ(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = _Fs_;
    VUregsn->VFr0xyzw= _XYZW;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << _It_;
}

static __ri void _vuRegsSQD(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = _Fs_;
    VUregsn->VFr0xyzw= _XYZW;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << _It_;
}

static __ri void _vuRegsSQI(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = _Fs_;
    VUregsn->VFr0xyzw= _XYZW;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << _It_;
}

static __ri void _vuRegsILW(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_IALU;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = 0;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << _Is_;
	VUregsn->cycles  = 4;
}

static __ri void _vuRegsISW(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_IALU;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = 0;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = (1 << _Is_) | (1 << _It_);
}

static __ri void _vuRegsILWR(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_IALU;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = 0;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = (1 << _It_);
    VUregsn->VIread  = (1 << _Is_);
	VUregsn->cycles  = 4;
}

static __ri void _vuRegsISWR(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_IALU;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = 0;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = (1 << _Is_) | (1 << _It_);
}

static __ri void _vuRegsRINIT(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = _Fs_;
    VUregsn->VFr0xyzw= 1 << (3-_Fsf_);
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << REG_R;
    VUregsn->VIread  = GET_VF0_FLAG(_Fs_);
}

static __ri void _vuRegsRGET(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = _Ft_;
    VUregsn->VFwxyzw = _XYZW;
	VUregsn->VFread0 = 0;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << REG_R;
}

static __ri void _vuRegsRNEXT(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = _Ft_;
    VUregsn->VFwxyzw = _XYZW;
	VUregsn->VFread0 = 0;
	VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << REG_R;
    VUregsn->VIread  = 1 << REG_R;
}

static __ri void _vuRegsRXOR(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
	VUregsn->VFread0 = _Fs_;
    VUregsn->VFr0xyzw= 1 << (3-_Fsf_);
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << REG_R;
    VUregsn->VIread  = (1 << REG_R)|GET_VF0_FLAG(_Fs_);
}

static __ri void _vuRegsWAITQ(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FDIV;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 0;
}

static __ri void _vuRegsFSAND(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << REG_STATUS_FLAG;
}

static __ri void _vuRegsFSEQ(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << REG_STATUS_FLAG;
}

static __ri void _vuRegsFSOR(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << REG_STATUS_FLAG;
}

static __ri void _vuRegsFSSET(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << REG_STATUS_FLAG;
	VUregsn->VIread  = 0;
}

static __ri void _vuRegsFMAND(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = (1 << REG_MAC_FLAG) | (1 << _Is_);
}

static __ri void _vuRegsFMEQ(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = (1 << REG_MAC_FLAG) | (1 << _Is_);
}

static __ri void _vuRegsFMOR(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = (1 << REG_MAC_FLAG) | (1 << _Is_);
}

static __ri void _vuRegsFCAND(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << 1;
    VUregsn->VIread  = 1 << REG_CLIP_FLAG;
}

static __ri void _vuRegsFCEQ(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << 1;
    VUregsn->VIread  = 1 << REG_CLIP_FLAG;
}

static __ri void _vuRegsFCOR(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << 1;
    VUregsn->VIread  = 1 << REG_CLIP_FLAG;
}

static __ri void _vuRegsFCSET(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << REG_CLIP_FLAG;
    VUregsn->VIread  = 0;
}

static __ri void _vuRegsFCGET(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 1 << REG_CLIP_FLAG;
}

static __ri void _vuRegsIBEQ(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = (1 << _Is_) | (1 << _It_);
}

static __ri void _vuRegsIBGEZ(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
	VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsIBGTZ(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
	VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsIBLEZ(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
	VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsIBLTZ(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
	VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsIBNE(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = (1 << _Is_) | (1 << _It_);
}

static __ri void _vuRegsB(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 0;
}

static __ri void _vuRegsBAL(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 0;
}

static __ri void _vuRegsJR(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
	VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsJALR(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_BRANCH;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 1 << _It_;
	VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsMFP(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_FMAC;
    VUregsn->VFwrite = _Ft_;
    VUregsn->VFwxyzw = _XYZW;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
	VUregsn->VIwrite = 0;
	VUregsn->VIread  = 1 << REG_P;
}

static __ri void _vuRegsWAITP(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_EFU;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 0;
	VUregsn->cycles = 0;
}

VUREGS_PFS_xyzw(ESADD, 11);
VUREGS_PFS_xyzw(ERSADD, 18);
VUREGS_PFS_xyzw(ELENG, 18);
VUREGS_PFS_xyzw(ERLENG, 24);
VUREGS_PFS_xyzw(EATANxy, 54);
VUREGS_PFS_xyzw(EATANxz, 54);
VUREGS_PFS_xyzw(ESUM, 12);
VUREGS_PFS_fsf(ERCPR, 12);
VUREGS_PFS_fsf(ESQRT, 12);
VUREGS_PFS_fsf(ERSQRT, 18);
VUREGS_PFS_fsf(ESIN, 29);
VUREGS_PFS_fsf(EATAN, 54);
VUREGS_PFS_fsf(EEXP, 44);

static __ri void _vuRegsXITOP(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_IALU;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 0;
	VUregsn->cycles  = 0;
}

static __ri void _vuRegsXGKICK(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_XGKICK;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 0;
    VUregsn->VIread  = 1 << _Is_;
}

static __ri void _vuRegsXTOP(const VURegs* VU, _VURegsNum* VUregsn)
{
	VUregsn->pipe = VUPIPE_IALU;
    VUregsn->VFwrite = 0;
    VUregsn->VFread0 = 0;
    VUregsn->VFread1 = 0;
    VUregsn->VIwrite = 1 << _It_;
    VUregsn->VIread  = 0;
	VUregsn->cycles  = 0;
}

// --------------------------------------------------------------------------------------
//  VU0
// --------------------------------------------------------------------------------------

/****************************************/
/*   VU Micromode Upper instructions    */
/****************************************/

static void VU0MI_ABS()  { _vuABS(&VU0); }
static void VU0MI_ADD()  { _vuADD(&VU0); }
static void VU0MI_ADDi() { _vuADDi(&VU0); }
static void VU0MI_ADDq() { _vuADDq(&VU0); }
static void VU0MI_ADDx() { _vuADDx(&VU0); }
static void VU0MI_ADDy() { _vuADDy(&VU0); }
static void VU0MI_ADDz() { _vuADDz(&VU0); }
static void VU0MI_ADDw() { _vuADDw(&VU0); }
static void VU0MI_ADDA() { _vuADDA(&VU0); }
static void VU0MI_ADDAi() { _vuADDAi(&VU0); }
static void VU0MI_ADDAq() { _vuADDAq(&VU0); }
static void VU0MI_ADDAx() { _vuADDAx(&VU0); }
static void VU0MI_ADDAy() { _vuADDAy(&VU0); }
static void VU0MI_ADDAz() { _vuADDAz(&VU0); }
static void VU0MI_ADDAw() { _vuADDAw(&VU0); }
static void VU0MI_SUB()  { _vuSUB(&VU0); }
static void VU0MI_SUBi() { _vuSUBi(&VU0); }
static void VU0MI_SUBq() { _vuSUBq(&VU0); }
static void VU0MI_SUBx() { _vuSUBx(&VU0); }
static void VU0MI_SUBy() { _vuSUBy(&VU0); }
static void VU0MI_SUBz() { _vuSUBz(&VU0); }
static void VU0MI_SUBw() { _vuSUBw(&VU0); }
static void VU0MI_SUBA()  { _vuSUBA(&VU0); }
static void VU0MI_SUBAi() { _vuSUBAi(&VU0); }
static void VU0MI_SUBAq() { _vuSUBAq(&VU0); }
static void VU0MI_SUBAx() { _vuSUBAx(&VU0); }
static void VU0MI_SUBAy() { _vuSUBAy(&VU0); }
static void VU0MI_SUBAz() { _vuSUBAz(&VU0); }
static void VU0MI_SUBAw() { _vuSUBAw(&VU0); }
static void VU0MI_MUL()  { _vuMUL(&VU0); }
static void VU0MI_MULi() { _vuMULi(&VU0); }
static void VU0MI_MULq() { _vuMULq(&VU0); }
static void VU0MI_MULx() { _vuMULx(&VU0); }
static void VU0MI_MULy() { _vuMULy(&VU0); }
static void VU0MI_MULz() { _vuMULz(&VU0); }
static void VU0MI_MULw() { _vuMULw(&VU0); }
static void VU0MI_MULA()  { _vuMULA(&VU0); }
static void VU0MI_MULAi() { _vuMULAi(&VU0); }
static void VU0MI_MULAq() { _vuMULAq(&VU0); }
static void VU0MI_MULAx() { _vuMULAx(&VU0); }
static void VU0MI_MULAy() { _vuMULAy(&VU0); }
static void VU0MI_MULAz() { _vuMULAz(&VU0); }
static void VU0MI_MULAw() { _vuMULAw(&VU0); }
static void VU0MI_MADD()  { _vuMADD(&VU0); }
static void VU0MI_MADDi() { _vuMADDi(&VU0); }
static void VU0MI_MADDq() { _vuMADDq(&VU0); }
static void VU0MI_MADDx() { _vuMADDx(&VU0); }
static void VU0MI_MADDy() { _vuMADDy(&VU0); }
static void VU0MI_MADDz() { _vuMADDz(&VU0); }
static void VU0MI_MADDw() { _vuMADDw(&VU0); }
static void VU0MI_MADDA()  { _vuMADDA(&VU0); }
static void VU0MI_MADDAi() { _vuMADDAi(&VU0); }
static void VU0MI_MADDAq() { _vuMADDAq(&VU0); }
static void VU0MI_MADDAx() { _vuMADDAx(&VU0); }
static void VU0MI_MADDAy() { _vuMADDAy(&VU0); }
static void VU0MI_MADDAz() { _vuMADDAz(&VU0); }
static void VU0MI_MADDAw() { _vuMADDAw(&VU0); }
static void VU0MI_MSUB()  { _vuMSUB(&VU0); }
static void VU0MI_MSUBi() { _vuMSUBi(&VU0); }
static void VU0MI_MSUBq() { _vuMSUBq(&VU0); }
static void VU0MI_MSUBx() { _vuMSUBx(&VU0); }
static void VU0MI_MSUBy() { _vuMSUBy(&VU0); }
static void VU0MI_MSUBz() { _vuMSUBz(&VU0); }
static void VU0MI_MSUBw() { _vuMSUBw(&VU0); }
static void VU0MI_MSUBA()  { _vuMSUBA(&VU0); }
static void VU0MI_MSUBAi() { _vuMSUBAi(&VU0); }
static void VU0MI_MSUBAq() { _vuMSUBAq(&VU0); }
static void VU0MI_MSUBAx() { _vuMSUBAx(&VU0); }
static void VU0MI_MSUBAy() { _vuMSUBAy(&VU0); }
static void VU0MI_MSUBAz() { _vuMSUBAz(&VU0); }
static void VU0MI_MSUBAw() { _vuMSUBAw(&VU0); }
static void VU0MI_MAX()  { _vuMAX(&VU0); }
static void VU0MI_MAXi() { _vuMAXi(&VU0); }
static void VU0MI_MAXx() { _vuMAXx(&VU0); }
static void VU0MI_MAXy() { _vuMAXy(&VU0); }
static void VU0MI_MAXz() { _vuMAXz(&VU0); }
static void VU0MI_MAXw() { _vuMAXw(&VU0); }
static void VU0MI_MINI()  { _vuMINI(&VU0); }
static void VU0MI_MINIi() { _vuMINIi(&VU0); }
static void VU0MI_MINIx() { _vuMINIx(&VU0); }
static void VU0MI_MINIy() { _vuMINIy(&VU0); }
static void VU0MI_MINIz() { _vuMINIz(&VU0); }
static void VU0MI_MINIw() { _vuMINIw(&VU0); }
static void VU0MI_OPMULA() { _vuOPMULA(&VU0); }
static void VU0MI_OPMSUB() { _vuOPMSUB(&VU0); }
static void VU0MI_NOP() { _vuNOP(&VU0); }
static void VU0MI_FTOI0()  { _vuFTOI0(&VU0); }
static void VU0MI_FTOI4()  { _vuFTOI4(&VU0); }
static void VU0MI_FTOI12() { _vuFTOI12(&VU0); }
static void VU0MI_FTOI15() { _vuFTOI15(&VU0); }
static void VU0MI_ITOF0()  { _vuITOF0(&VU0); }
static void VU0MI_ITOF4()  { _vuITOF4(&VU0); }
static void VU0MI_ITOF12() { _vuITOF12(&VU0); }
static void VU0MI_ITOF15() { _vuITOF15(&VU0); }
static void VU0MI_CLIP() { _vuCLIP(&VU0); }

/*****************************************/
/*   VU Micromode Lower instructions    */
/*****************************************/

static void VU0MI_DIV() { _vuDIV(&VU0); }
static void VU0MI_SQRT() { _vuSQRT(&VU0); }
static void VU0MI_RSQRT() { _vuRSQRT(&VU0); }
static void VU0MI_IADD() { _vuIADD(&VU0); }
static void VU0MI_IADDI() { _vuIADDI(&VU0); }
static void VU0MI_IADDIU() { _vuIADDIU(&VU0); }
static void VU0MI_IAND() { _vuIAND(&VU0); }
static void VU0MI_IOR() { _vuIOR(&VU0); }
static void VU0MI_ISUB() { _vuISUB(&VU0); }
static void VU0MI_ISUBIU() { _vuISUBIU(&VU0); }
static void VU0MI_MOVE() { _vuMOVE(&VU0); }
static void VU0MI_MFIR() { _vuMFIR(&VU0); }
static void VU0MI_MTIR() { _vuMTIR(&VU0); }
static void VU0MI_MR32() { _vuMR32(&VU0); }
static void VU0MI_LQ() { _vuLQ(&VU0); }
static void VU0MI_LQD() { _vuLQD(&VU0); }
static void VU0MI_LQI() { _vuLQI(&VU0); }
static void VU0MI_SQ() { _vuSQ(&VU0); }
static void VU0MI_SQD() { _vuSQD(&VU0); }
static void VU0MI_SQI() { _vuSQI(&VU0); }
static void VU0MI_ILW() { _vuILW(&VU0); }
static void VU0MI_ISW() { _vuISW(&VU0); }
static void VU0MI_ILWR() { _vuILWR(&VU0); }
static void VU0MI_ISWR() { _vuISWR(&VU0); }
static void VU0MI_RINIT() { _vuRINIT(&VU0); }
static void VU0MI_RGET()  { _vuRGET(&VU0); }
static void VU0MI_RNEXT() { _vuRNEXT(&VU0); }
static void VU0MI_RXOR()  { _vuRXOR(&VU0); }
static void VU0MI_WAITQ() { _vuWAITQ(&VU0); }
static void VU0MI_FSAND() { _vuFSAND(&VU0); }
static void VU0MI_FSEQ()  { _vuFSEQ(&VU0); }
static void VU0MI_FSOR()  { _vuFSOR(&VU0); }
static void VU0MI_FSSET() { _vuFSSET(&VU0); }
static void VU0MI_FMAND() { _vuFMAND(&VU0); }
static void VU0MI_FMEQ()  { _vuFMEQ(&VU0); }
static void VU0MI_FMOR()  { _vuFMOR(&VU0); }
static void VU0MI_FCAND() { _vuFCAND(&VU0); }
static void VU0MI_FCEQ()  { _vuFCEQ(&VU0); }
static void VU0MI_FCOR()  { _vuFCOR(&VU0); }
static void VU0MI_FCSET() { _vuFCSET(&VU0); }
static void VU0MI_FCGET() { _vuFCGET(&VU0); }
static void VU0MI_IBEQ() { _vuIBEQ(&VU0); }
static void VU0MI_IBGEZ() { _vuIBGEZ(&VU0); }
static void VU0MI_IBGTZ() { _vuIBGTZ(&VU0); }
static void VU0MI_IBLTZ() { _vuIBLTZ(&VU0); }
static void VU0MI_IBLEZ() { _vuIBLEZ(&VU0); }
static void VU0MI_IBNE() { _vuIBNE(&VU0); }
static void VU0MI_B()   { _vuB(&VU0); }
static void VU0MI_BAL() { _vuBAL(&VU0); }
static void VU0MI_JR()   { _vuJR(&VU0); }
static void VU0MI_JALR() { _vuJALR(&VU0); }
static void VU0MI_MFP() { _vuMFP(&VU0); }
static void VU0MI_WAITP() { _vuWAITP(&VU0); }
static void VU0MI_ESADD()   { _vuESADD(&VU0); }
static void VU0MI_ERSADD()  { _vuERSADD(&VU0); }
static void VU0MI_ELENG()   { _vuELENG(&VU0); }
static void VU0MI_ERLENG()  { _vuERLENG(&VU0); }
static void VU0MI_EATANxy() { _vuEATANxy(&VU0); }
static void VU0MI_EATANxz() { _vuEATANxz(&VU0); }
static void VU0MI_ESUM()    { _vuESUM(&VU0); }
static void VU0MI_ERCPR()   { _vuERCPR(&VU0); }
static void VU0MI_ESQRT()   { _vuESQRT(&VU0); }
static void VU0MI_ERSQRT()  { _vuERSQRT(&VU0); }
static void VU0MI_ESIN()    { _vuESIN(&VU0); }
static void VU0MI_EATAN()   { _vuEATAN(&VU0); }
static void VU0MI_EEXP()    { _vuEEXP(&VU0); }
static void VU0MI_XITOP() { _vuXITOP(&VU0); }
static void VU0MI_XGKICK() {}
static void VU0MI_XTOP() {}

/****************************************/
/*   VU Micromode Upper instructions    */
/****************************************/

static void VU0regsMI_ABS(_VURegsNum* VUregsn) { _vuRegsABS(&VU0, VUregsn); }
static void VU0regsMI_ADD(_VURegsNum* VUregsn) { _vuRegsADD(&VU0, VUregsn); }
static void VU0regsMI_ADDi(_VURegsNum* VUregsn) { _vuRegsADDi(&VU0, VUregsn); }
static void VU0regsMI_ADDq(_VURegsNum* VUregsn) { _vuRegsADDq(&VU0, VUregsn); }
static void VU0regsMI_ADDx(_VURegsNum* VUregsn) { _vuRegsADDx(&VU0, VUregsn); }
static void VU0regsMI_ADDy(_VURegsNum* VUregsn) { _vuRegsADDy(&VU0, VUregsn); }
static void VU0regsMI_ADDz(_VURegsNum* VUregsn) { _vuRegsADDz(&VU0, VUregsn); }
static void VU0regsMI_ADDw(_VURegsNum* VUregsn) { _vuRegsADDw(&VU0, VUregsn); }
static void VU0regsMI_ADDA(_VURegsNum* VUregsn) { _vuRegsADDA(&VU0, VUregsn); }
static void VU0regsMI_ADDAi(_VURegsNum* VUregsn) { _vuRegsADDAi(&VU0, VUregsn); }
static void VU0regsMI_ADDAq(_VURegsNum* VUregsn) { _vuRegsADDAq(&VU0, VUregsn); }
static void VU0regsMI_ADDAx(_VURegsNum* VUregsn) { _vuRegsADDAx(&VU0, VUregsn); }
static void VU0regsMI_ADDAy(_VURegsNum* VUregsn) { _vuRegsADDAy(&VU0, VUregsn); }
static void VU0regsMI_ADDAz(_VURegsNum* VUregsn) { _vuRegsADDAz(&VU0, VUregsn); }
static void VU0regsMI_ADDAw(_VURegsNum* VUregsn) { _vuRegsADDAw(&VU0, VUregsn); }
static void VU0regsMI_SUB(_VURegsNum* VUregsn) { _vuRegsSUB(&VU0, VUregsn); }
static void VU0regsMI_SUBi(_VURegsNum* VUregsn) { _vuRegsSUBi(&VU0, VUregsn); }
static void VU0regsMI_SUBq(_VURegsNum* VUregsn) { _vuRegsSUBq(&VU0, VUregsn); }
static void VU0regsMI_SUBx(_VURegsNum* VUregsn) { _vuRegsSUBx(&VU0, VUregsn); }
static void VU0regsMI_SUBy(_VURegsNum* VUregsn) { _vuRegsSUBy(&VU0, VUregsn); }
static void VU0regsMI_SUBz(_VURegsNum* VUregsn) { _vuRegsSUBz(&VU0, VUregsn); }
static void VU0regsMI_SUBw(_VURegsNum* VUregsn) { _vuRegsSUBw(&VU0, VUregsn); }
static void VU0regsMI_SUBA(_VURegsNum* VUregsn) { _vuRegsSUBA(&VU0, VUregsn); }
static void VU0regsMI_SUBAi(_VURegsNum* VUregsn) { _vuRegsSUBAi(&VU0, VUregsn); }
static void VU0regsMI_SUBAq(_VURegsNum* VUregsn) { _vuRegsSUBAq(&VU0, VUregsn); }
static void VU0regsMI_SUBAx(_VURegsNum* VUregsn) { _vuRegsSUBAx(&VU0, VUregsn); }
static void VU0regsMI_SUBAy(_VURegsNum* VUregsn) { _vuRegsSUBAy(&VU0, VUregsn); }
static void VU0regsMI_SUBAz(_VURegsNum* VUregsn) { _vuRegsSUBAz(&VU0, VUregsn); }
static void VU0regsMI_SUBAw(_VURegsNum* VUregsn) { _vuRegsSUBAw(&VU0, VUregsn); }
static void VU0regsMI_MUL(_VURegsNum* VUregsn) { _vuRegsMUL(&VU0, VUregsn); }
static void VU0regsMI_MULi(_VURegsNum* VUregsn) { _vuRegsMULi(&VU0, VUregsn); }
static void VU0regsMI_MULq(_VURegsNum* VUregsn) { _vuRegsMULq(&VU0, VUregsn); }
static void VU0regsMI_MULx(_VURegsNum* VUregsn) { _vuRegsMULx(&VU0, VUregsn); }
static void VU0regsMI_MULy(_VURegsNum* VUregsn) { _vuRegsMULy(&VU0, VUregsn); }
static void VU0regsMI_MULz(_VURegsNum* VUregsn) { _vuRegsMULz(&VU0, VUregsn); }
static void VU0regsMI_MULw(_VURegsNum* VUregsn) { _vuRegsMULw(&VU0, VUregsn); }
static void VU0regsMI_MULA(_VURegsNum* VUregsn) { _vuRegsMULA(&VU0, VUregsn); }
static void VU0regsMI_MULAi(_VURegsNum* VUregsn) { _vuRegsMULAi(&VU0, VUregsn); }
static void VU0regsMI_MULAq(_VURegsNum* VUregsn) { _vuRegsMULAq(&VU0, VUregsn); }
static void VU0regsMI_MULAx(_VURegsNum* VUregsn) { _vuRegsMULAx(&VU0, VUregsn); }
static void VU0regsMI_MULAy(_VURegsNum* VUregsn) { _vuRegsMULAy(&VU0, VUregsn); }
static void VU0regsMI_MULAz(_VURegsNum* VUregsn) { _vuRegsMULAz(&VU0, VUregsn); }
static void VU0regsMI_MULAw(_VURegsNum* VUregsn) { _vuRegsMULAw(&VU0, VUregsn); }
static void VU0regsMI_MADD(_VURegsNum* VUregsn) { _vuRegsMADD(&VU0, VUregsn); }
static void VU0regsMI_MADDi(_VURegsNum* VUregsn) { _vuRegsMADDi(&VU0, VUregsn); }
static void VU0regsMI_MADDq(_VURegsNum* VUregsn) { _vuRegsMADDq(&VU0, VUregsn); }
static void VU0regsMI_MADDx(_VURegsNum* VUregsn) { _vuRegsMADDx(&VU0, VUregsn); }
static void VU0regsMI_MADDy(_VURegsNum* VUregsn) { _vuRegsMADDy(&VU0, VUregsn); }
static void VU0regsMI_MADDz(_VURegsNum* VUregsn) { _vuRegsMADDz(&VU0, VUregsn); }
static void VU0regsMI_MADDw(_VURegsNum* VUregsn) { _vuRegsMADDw(&VU0, VUregsn); }
static void VU0regsMI_MADDA(_VURegsNum* VUregsn) { _vuRegsMADDA(&VU0, VUregsn); }
static void VU0regsMI_MADDAi(_VURegsNum* VUregsn) { _vuRegsMADDAi(&VU0, VUregsn); }
static void VU0regsMI_MADDAq(_VURegsNum* VUregsn) { _vuRegsMADDAq(&VU0, VUregsn); }
static void VU0regsMI_MADDAx(_VURegsNum* VUregsn) { _vuRegsMADDAx(&VU0, VUregsn); }
static void VU0regsMI_MADDAy(_VURegsNum* VUregsn) { _vuRegsMADDAy(&VU0, VUregsn); }
static void VU0regsMI_MADDAz(_VURegsNum* VUregsn) { _vuRegsMADDAz(&VU0, VUregsn); }
static void VU0regsMI_MADDAw(_VURegsNum* VUregsn) { _vuRegsMADDAw(&VU0, VUregsn); }
static void VU0regsMI_MSUB(_VURegsNum* VUregsn) { _vuRegsMSUB(&VU0, VUregsn); }
static void VU0regsMI_MSUBi(_VURegsNum* VUregsn) { _vuRegsMSUBi(&VU0, VUregsn); }
static void VU0regsMI_MSUBq(_VURegsNum* VUregsn) { _vuRegsMSUBq(&VU0, VUregsn); }
static void VU0regsMI_MSUBx(_VURegsNum* VUregsn) { _vuRegsMSUBx(&VU0, VUregsn); }
static void VU0regsMI_MSUBy(_VURegsNum* VUregsn) { _vuRegsMSUBy(&VU0, VUregsn); }
static void VU0regsMI_MSUBz(_VURegsNum* VUregsn) { _vuRegsMSUBz(&VU0, VUregsn); }
static void VU0regsMI_MSUBw(_VURegsNum* VUregsn) { _vuRegsMSUBw(&VU0, VUregsn); }
static void VU0regsMI_MSUBA(_VURegsNum* VUregsn) { _vuRegsMSUBA(&VU0, VUregsn); }
static void VU0regsMI_MSUBAi(_VURegsNum* VUregsn) { _vuRegsMSUBAi(&VU0, VUregsn); }
static void VU0regsMI_MSUBAq(_VURegsNum* VUregsn) { _vuRegsMSUBAq(&VU0, VUregsn); }
static void VU0regsMI_MSUBAx(_VURegsNum* VUregsn) { _vuRegsMSUBAx(&VU0, VUregsn); }
static void VU0regsMI_MSUBAy(_VURegsNum* VUregsn) { _vuRegsMSUBAy(&VU0, VUregsn); }
static void VU0regsMI_MSUBAz(_VURegsNum* VUregsn) { _vuRegsMSUBAz(&VU0, VUregsn); }
static void VU0regsMI_MSUBAw(_VURegsNum* VUregsn) { _vuRegsMSUBAw(&VU0, VUregsn); }
static void VU0regsMI_MAX(_VURegsNum* VUregsn) { _vuRegsMAX(&VU0, VUregsn); }
static void VU0regsMI_MAXi(_VURegsNum* VUregsn) { _vuRegsMAXi(&VU0, VUregsn); }
static void VU0regsMI_MAXx(_VURegsNum* VUregsn) { _vuRegsMAXx(&VU0, VUregsn); }
static void VU0regsMI_MAXy(_VURegsNum* VUregsn) { _vuRegsMAXy(&VU0, VUregsn); }
static void VU0regsMI_MAXz(_VURegsNum* VUregsn) { _vuRegsMAXz(&VU0, VUregsn); }
static void VU0regsMI_MAXw(_VURegsNum* VUregsn) { _vuRegsMAXw(&VU0, VUregsn); }
static void VU0regsMI_MINI(_VURegsNum* VUregsn) { _vuRegsMINI(&VU0, VUregsn); }
static void VU0regsMI_MINIi(_VURegsNum* VUregsn) { _vuRegsMINIi(&VU0, VUregsn); }
static void VU0regsMI_MINIx(_VURegsNum* VUregsn) { _vuRegsMINIx(&VU0, VUregsn); }
static void VU0regsMI_MINIy(_VURegsNum* VUregsn) { _vuRegsMINIy(&VU0, VUregsn); }
static void VU0regsMI_MINIz(_VURegsNum* VUregsn) { _vuRegsMINIz(&VU0, VUregsn); }
static void VU0regsMI_MINIw(_VURegsNum* VUregsn) { _vuRegsMINIw(&VU0, VUregsn); }
static void VU0regsMI_OPMULA(_VURegsNum* VUregsn) { _vuRegsOPMULA(&VU0, VUregsn); }
static void VU0regsMI_OPMSUB(_VURegsNum* VUregsn) { _vuRegsOPMSUB(&VU0, VUregsn); }
static void VU0regsMI_NOP(_VURegsNum* VUregsn) { _vuRegsNOP(&VU0, VUregsn); }
static void VU0regsMI_FTOI0(_VURegsNum* VUregsn) { _vuRegsFTOI0(&VU0, VUregsn); }
static void VU0regsMI_FTOI4(_VURegsNum* VUregsn) { _vuRegsFTOI4(&VU0, VUregsn); }
static void VU0regsMI_FTOI12(_VURegsNum* VUregsn) { _vuRegsFTOI12(&VU0, VUregsn); }
static void VU0regsMI_FTOI15(_VURegsNum* VUregsn) { _vuRegsFTOI15(&VU0, VUregsn); }
static void VU0regsMI_ITOF0(_VURegsNum* VUregsn) { _vuRegsITOF0(&VU0, VUregsn); }
static void VU0regsMI_ITOF4(_VURegsNum* VUregsn) { _vuRegsITOF4(&VU0, VUregsn); }
static void VU0regsMI_ITOF12(_VURegsNum* VUregsn) { _vuRegsITOF12(&VU0, VUregsn); }
static void VU0regsMI_ITOF15(_VURegsNum* VUregsn) { _vuRegsITOF15(&VU0, VUregsn); }
static void VU0regsMI_CLIP(_VURegsNum* VUregsn) { _vuRegsCLIP(&VU0, VUregsn); }

/*****************************************/
/*   VU Micromode Lower instructions    */
/*****************************************/

static void VU0regsMI_DIV(_VURegsNum* VUregsn) { _vuRegsDIV(&VU0, VUregsn); }
static void VU0regsMI_SQRT(_VURegsNum* VUregsn) { _vuRegsSQRT(&VU0, VUregsn); }
static void VU0regsMI_RSQRT(_VURegsNum* VUregsn) { _vuRegsRSQRT(&VU0, VUregsn); }
static void VU0regsMI_IADD(_VURegsNum* VUregsn) { _vuRegsIADD(&VU0, VUregsn); }
static void VU0regsMI_IADDI(_VURegsNum* VUregsn) { _vuRegsIADDI(&VU0, VUregsn); }
static void VU0regsMI_IADDIU(_VURegsNum* VUregsn) { _vuRegsIADDIU(&VU0, VUregsn); }
static void VU0regsMI_IAND(_VURegsNum* VUregsn) { _vuRegsIAND(&VU0, VUregsn); }
static void VU0regsMI_IOR(_VURegsNum* VUregsn) { _vuRegsIOR(&VU0, VUregsn); }
static void VU0regsMI_ISUB(_VURegsNum* VUregsn) { _vuRegsISUB(&VU0, VUregsn); }
static void VU0regsMI_ISUBIU(_VURegsNum* VUregsn) { _vuRegsISUBIU(&VU0, VUregsn); }
static void VU0regsMI_MOVE(_VURegsNum* VUregsn) { _vuRegsMOVE(&VU0, VUregsn); }
static void VU0regsMI_MFIR(_VURegsNum* VUregsn) { _vuRegsMFIR(&VU0, VUregsn); }
static void VU0regsMI_MTIR(_VURegsNum* VUregsn) { _vuRegsMTIR(&VU0, VUregsn); }
static void VU0regsMI_MR32(_VURegsNum* VUregsn) { _vuRegsMR32(&VU0, VUregsn); }
static void VU0regsMI_LQ(_VURegsNum* VUregsn) { _vuRegsLQ(&VU0, VUregsn); }
static void VU0regsMI_LQD(_VURegsNum* VUregsn) { _vuRegsLQD(&VU0, VUregsn); }
static void VU0regsMI_LQI(_VURegsNum* VUregsn) { _vuRegsLQI(&VU0, VUregsn); }
static void VU0regsMI_SQ(_VURegsNum* VUregsn) { _vuRegsSQ(&VU0, VUregsn); }
static void VU0regsMI_SQD(_VURegsNum* VUregsn) { _vuRegsSQD(&VU0, VUregsn); }
static void VU0regsMI_SQI(_VURegsNum* VUregsn) { _vuRegsSQI(&VU0, VUregsn); }
static void VU0regsMI_ILW(_VURegsNum* VUregsn) { _vuRegsILW(&VU0, VUregsn); }
static void VU0regsMI_ISW(_VURegsNum* VUregsn) { _vuRegsISW(&VU0, VUregsn); }
static void VU0regsMI_ILWR(_VURegsNum* VUregsn) { _vuRegsILWR(&VU0, VUregsn); }
static void VU0regsMI_ISWR(_VURegsNum* VUregsn) { _vuRegsISWR(&VU0, VUregsn); }
static void VU0regsMI_RINIT(_VURegsNum* VUregsn) { _vuRegsRINIT(&VU0, VUregsn); }
static void VU0regsMI_RGET(_VURegsNum* VUregsn) { _vuRegsRGET(&VU0, VUregsn); }
static void VU0regsMI_RNEXT(_VURegsNum* VUregsn) { _vuRegsRNEXT(&VU0, VUregsn); }
static void VU0regsMI_RXOR(_VURegsNum* VUregsn) { _vuRegsRXOR(&VU0, VUregsn); }
static void VU0regsMI_WAITQ(_VURegsNum* VUregsn) { _vuRegsWAITQ(&VU0, VUregsn); }
static void VU0regsMI_FSAND(_VURegsNum* VUregsn) { _vuRegsFSAND(&VU0, VUregsn); }
static void VU0regsMI_FSEQ(_VURegsNum* VUregsn) { _vuRegsFSEQ(&VU0, VUregsn); }
static void VU0regsMI_FSOR(_VURegsNum* VUregsn) { _vuRegsFSOR(&VU0, VUregsn); }
static void VU0regsMI_FSSET(_VURegsNum* VUregsn) { _vuRegsFSSET(&VU0, VUregsn); }
static void VU0regsMI_FMAND(_VURegsNum* VUregsn) { _vuRegsFMAND(&VU0, VUregsn); }
static void VU0regsMI_FMEQ(_VURegsNum* VUregsn) { _vuRegsFMEQ(&VU0, VUregsn); }
static void VU0regsMI_FMOR(_VURegsNum* VUregsn) { _vuRegsFMOR(&VU0, VUregsn); }
static void VU0regsMI_FCAND(_VURegsNum* VUregsn) { _vuRegsFCAND(&VU0, VUregsn); }
static void VU0regsMI_FCEQ(_VURegsNum* VUregsn) { _vuRegsFCEQ(&VU0, VUregsn); }
static void VU0regsMI_FCOR(_VURegsNum* VUregsn) { _vuRegsFCOR(&VU0, VUregsn); }
static void VU0regsMI_FCSET(_VURegsNum* VUregsn) { _vuRegsFCSET(&VU0, VUregsn); }
static void VU0regsMI_FCGET(_VURegsNum* VUregsn) { _vuRegsFCGET(&VU0, VUregsn); }
static void VU0regsMI_IBEQ(_VURegsNum* VUregsn) { _vuRegsIBEQ(&VU0, VUregsn); }
static void VU0regsMI_IBGEZ(_VURegsNum* VUregsn) { _vuRegsIBGEZ(&VU0, VUregsn); }
static void VU0regsMI_IBGTZ(_VURegsNum* VUregsn) { _vuRegsIBGTZ(&VU0, VUregsn); }
static void VU0regsMI_IBLTZ(_VURegsNum* VUregsn) { _vuRegsIBLTZ(&VU0, VUregsn); }
static void VU0regsMI_IBLEZ(_VURegsNum* VUregsn) { _vuRegsIBLEZ(&VU0, VUregsn); }
static void VU0regsMI_IBNE(_VURegsNum* VUregsn) { _vuRegsIBNE(&VU0, VUregsn); }
static void VU0regsMI_B(_VURegsNum* VUregsn) { _vuRegsB(&VU0, VUregsn); }
static void VU0regsMI_BAL(_VURegsNum* VUregsn) { _vuRegsBAL(&VU0, VUregsn); }
static void VU0regsMI_JR(_VURegsNum* VUregsn) { _vuRegsJR(&VU0, VUregsn); }
static void VU0regsMI_JALR(_VURegsNum* VUregsn) { _vuRegsJALR(&VU0, VUregsn); }
static void VU0regsMI_MFP(_VURegsNum* VUregsn) { _vuRegsMFP(&VU0, VUregsn); }
static void VU0regsMI_WAITP(_VURegsNum* VUregsn) { _vuRegsWAITP(&VU0, VUregsn); }
static void VU0regsMI_ESADD(_VURegsNum* VUregsn) { _vuRegsESADD(&VU0, VUregsn); }
static void VU0regsMI_ERSADD(_VURegsNum* VUregsn) { _vuRegsERSADD(&VU0, VUregsn); }
static void VU0regsMI_ELENG(_VURegsNum* VUregsn) { _vuRegsELENG(&VU0, VUregsn); }
static void VU0regsMI_ERLENG(_VURegsNum* VUregsn) { _vuRegsERLENG(&VU0, VUregsn); }
static void VU0regsMI_EATANxy(_VURegsNum* VUregsn) { _vuRegsEATANxy(&VU0, VUregsn); }
static void VU0regsMI_EATANxz(_VURegsNum* VUregsn) { _vuRegsEATANxz(&VU0, VUregsn); }
static void VU0regsMI_ESUM(_VURegsNum* VUregsn) { _vuRegsESUM(&VU0, VUregsn); }
static void VU0regsMI_ERCPR(_VURegsNum* VUregsn) { _vuRegsERCPR(&VU0, VUregsn); }
static void VU0regsMI_ESQRT(_VURegsNum* VUregsn) { _vuRegsESQRT(&VU0, VUregsn); }
static void VU0regsMI_ERSQRT(_VURegsNum* VUregsn) { _vuRegsERSQRT(&VU0, VUregsn); }
static void VU0regsMI_ESIN(_VURegsNum* VUregsn) { _vuRegsESIN(&VU0, VUregsn); }
static void VU0regsMI_EATAN(_VURegsNum* VUregsn) { _vuRegsEATAN(&VU0, VUregsn); }
static void VU0regsMI_EEXP(_VURegsNum* VUregsn) { _vuRegsEEXP(&VU0, VUregsn); }
static void VU0regsMI_XITOP(_VURegsNum* VUregsn) { _vuRegsXITOP(&VU0, VUregsn); }
static void VU0regsMI_XGKICK(_VURegsNum* VUregsn) { _vuRegsXGKICK(&VU0, VUregsn); }
static void VU0regsMI_XTOP(_VURegsNum* VUregsn) { _vuRegsXTOP(&VU0, VUregsn); }

void VU0unknown()
{
	pxFail("Unknown VU micromode opcode called");
	CPU_LOG("Unknown VU micromode opcode called");
}

static void VU0regsunknown(_VURegsNum* VUregsn)
{
	pxFail("Unknown VU micromode opcode called");
	CPU_LOG("Unknown VU micromode opcode called");
}

// --------------------------------------------------------------------------------------
//  VU1
// --------------------------------------------------------------------------------------

/****************************************/
/*   VU Micromode Upper instructions    */
/****************************************/

static void VU1MI_ABS()  { _vuABS(&VU1); }
static void VU1MI_ADD()  { _vuADD(&VU1); }
static void VU1MI_ADDi() { _vuADDi(&VU1); }
static void VU1MI_ADDq() { _vuADDq(&VU1); }
static void VU1MI_ADDx() { _vuADDx(&VU1); }
static void VU1MI_ADDy() { _vuADDy(&VU1); }
static void VU1MI_ADDz() { _vuADDz(&VU1); }
static void VU1MI_ADDw() { _vuADDw(&VU1); }
static void VU1MI_ADDA() { _vuADDA(&VU1); }
static void VU1MI_ADDAi() { _vuADDAi(&VU1); }
static void VU1MI_ADDAq() { _vuADDAq(&VU1); }
static void VU1MI_ADDAx() { _vuADDAx(&VU1); }
static void VU1MI_ADDAy() { _vuADDAy(&VU1); }
static void VU1MI_ADDAz() { _vuADDAz(&VU1); }
static void VU1MI_ADDAw() { _vuADDAw(&VU1); }
static void VU1MI_SUB()  { _vuSUB(&VU1); }
static void VU1MI_SUBi() { _vuSUBi(&VU1); }
static void VU1MI_SUBq() { _vuSUBq(&VU1); }
static void VU1MI_SUBx() { _vuSUBx(&VU1); }
static void VU1MI_SUBy() { _vuSUBy(&VU1); }
static void VU1MI_SUBz() { _vuSUBz(&VU1); }
static void VU1MI_SUBw() { _vuSUBw(&VU1); }
static void VU1MI_SUBA()  { _vuSUBA(&VU1); }
static void VU1MI_SUBAi() { _vuSUBAi(&VU1); }
static void VU1MI_SUBAq() { _vuSUBAq(&VU1); }
static void VU1MI_SUBAx() { _vuSUBAx(&VU1); }
static void VU1MI_SUBAy() { _vuSUBAy(&VU1); }
static void VU1MI_SUBAz() { _vuSUBAz(&VU1); }
static void VU1MI_SUBAw() { _vuSUBAw(&VU1); }
static void VU1MI_MUL()  { _vuMUL(&VU1); }
static void VU1MI_MULi() { _vuMULi(&VU1); }
static void VU1MI_MULq() { _vuMULq(&VU1); }
static void VU1MI_MULx() { _vuMULx(&VU1); }
static void VU1MI_MULy() { _vuMULy(&VU1); }
static void VU1MI_MULz() { _vuMULz(&VU1); }
static void VU1MI_MULw() { _vuMULw(&VU1); }
static void VU1MI_MULA()  { _vuMULA(&VU1); }
static void VU1MI_MULAi() { _vuMULAi(&VU1); }
static void VU1MI_MULAq() { _vuMULAq(&VU1); }
static void VU1MI_MULAx() { _vuMULAx(&VU1); }
static void VU1MI_MULAy() { _vuMULAy(&VU1); }
static void VU1MI_MULAz() { _vuMULAz(&VU1); }
static void VU1MI_MULAw() { _vuMULAw(&VU1); }
static void VU1MI_MADD()  { _vuMADD(&VU1); }
static void VU1MI_MADDi() { _vuMADDi(&VU1); }
static void VU1MI_MADDq() { _vuMADDq(&VU1); }
static void VU1MI_MADDx() { _vuMADDx(&VU1); }
static void VU1MI_MADDy() { _vuMADDy(&VU1); }
static void VU1MI_MADDz() { _vuMADDz(&VU1); }
static void VU1MI_MADDw() { _vuMADDw(&VU1); }
static void VU1MI_MADDA()  { _vuMADDA(&VU1); }
static void VU1MI_MADDAi() { _vuMADDAi(&VU1); }
static void VU1MI_MADDAq() { _vuMADDAq(&VU1); }
static void VU1MI_MADDAx() { _vuMADDAx(&VU1); }
static void VU1MI_MADDAy() { _vuMADDAy(&VU1); }
static void VU1MI_MADDAz() { _vuMADDAz(&VU1); }
static void VU1MI_MADDAw() { _vuMADDAw(&VU1); }
static void VU1MI_MSUB()  { _vuMSUB(&VU1); }
static void VU1MI_MSUBi() { _vuMSUBi(&VU1); }
static void VU1MI_MSUBq() { _vuMSUBq(&VU1); }
static void VU1MI_MSUBx() { _vuMSUBx(&VU1); }
static void VU1MI_MSUBy() { _vuMSUBy(&VU1); }
static void VU1MI_MSUBz() { _vuMSUBz(&VU1); }
static void VU1MI_MSUBw() { _vuMSUBw(&VU1); }
static void VU1MI_MSUBA()  { _vuMSUBA(&VU1); }
static void VU1MI_MSUBAi() { _vuMSUBAi(&VU1); }
static void VU1MI_MSUBAq() { _vuMSUBAq(&VU1); }
static void VU1MI_MSUBAx() { _vuMSUBAx(&VU1); }
static void VU1MI_MSUBAy() { _vuMSUBAy(&VU1); }
static void VU1MI_MSUBAz() { _vuMSUBAz(&VU1); }
static void VU1MI_MSUBAw() { _vuMSUBAw(&VU1); }
static void VU1MI_MAX()  { _vuMAX(&VU1); }
static void VU1MI_MAXi() { _vuMAXi(&VU1); }
static void VU1MI_MAXx() { _vuMAXx(&VU1); }
static void VU1MI_MAXy() { _vuMAXy(&VU1); }
static void VU1MI_MAXz() { _vuMAXz(&VU1); }
static void VU1MI_MAXw() { _vuMAXw(&VU1); }
static void VU1MI_MINI()  { _vuMINI(&VU1); }
static void VU1MI_MINIi() { _vuMINIi(&VU1); }
static void VU1MI_MINIx() { _vuMINIx(&VU1); }
static void VU1MI_MINIy() { _vuMINIy(&VU1); }
static void VU1MI_MINIz() { _vuMINIz(&VU1); }
static void VU1MI_MINIw() { _vuMINIw(&VU1); }
static void VU1MI_OPMULA() { _vuOPMULA(&VU1); }
static void VU1MI_OPMSUB() { _vuOPMSUB(&VU1); }
static void VU1MI_NOP() { _vuNOP(&VU1); }
static void VU1MI_FTOI0()  { _vuFTOI0(&VU1); }
static void VU1MI_FTOI4()  { _vuFTOI4(&VU1); }
static void VU1MI_FTOI12() { _vuFTOI12(&VU1); }
static void VU1MI_FTOI15() { _vuFTOI15(&VU1); }
static void VU1MI_ITOF0()  { _vuITOF0(&VU1); }
static void VU1MI_ITOF4()  { _vuITOF4(&VU1); }
static void VU1MI_ITOF12() { _vuITOF12(&VU1); }
static void VU1MI_ITOF15() { _vuITOF15(&VU1); }
static void VU1MI_CLIP() { _vuCLIP(&VU1); }

/*****************************************/
/*   VU Micromode Lower instructions    */
/*****************************************/

static void VU1MI_DIV() { _vuDIV(&VU1); }
static void VU1MI_SQRT() { _vuSQRT(&VU1); }
static void VU1MI_RSQRT() { _vuRSQRT(&VU1); }
static void VU1MI_IADD() { _vuIADD(&VU1); }
static void VU1MI_IADDI() { _vuIADDI(&VU1); }
static void VU1MI_IADDIU() { _vuIADDIU(&VU1); }
static void VU1MI_IAND() { _vuIAND(&VU1); }
static void VU1MI_IOR() { _vuIOR(&VU1); }
static void VU1MI_ISUB() { _vuISUB(&VU1); }
static void VU1MI_ISUBIU() { _vuISUBIU(&VU1); }
static void VU1MI_MOVE() { _vuMOVE(&VU1); }
static void VU1MI_MFIR() { _vuMFIR(&VU1); }
static void VU1MI_MTIR() { _vuMTIR(&VU1); }
static void VU1MI_MR32() { _vuMR32(&VU1); }
static void VU1MI_LQ() { _vuLQ(&VU1); }
static void VU1MI_LQD() { _vuLQD(&VU1); }
static void VU1MI_LQI() { _vuLQI(&VU1); }
static void VU1MI_SQ() { _vuSQ(&VU1); }
static void VU1MI_SQD() { _vuSQD(&VU1); }
static void VU1MI_SQI() { _vuSQI(&VU1); }
static void VU1MI_ILW() { _vuILW(&VU1); }
static void VU1MI_ISW() { _vuISW(&VU1); }
static void VU1MI_ILWR() { _vuILWR(&VU1); }
static void VU1MI_ISWR() { _vuISWR(&VU1); }
static void VU1MI_RINIT() { _vuRINIT(&VU1); }
static void VU1MI_RGET()  { _vuRGET(&VU1); }
static void VU1MI_RNEXT() { _vuRNEXT(&VU1); }
static void VU1MI_RXOR()  { _vuRXOR(&VU1); }
static void VU1MI_WAITQ() { _vuWAITQ(&VU1); }
static void VU1MI_FSAND() { _vuFSAND(&VU1); }
static void VU1MI_FSEQ()  { _vuFSEQ(&VU1); }
static void VU1MI_FSOR()  { _vuFSOR(&VU1); }
static void VU1MI_FSSET() { _vuFSSET(&VU1); }
static void VU1MI_FMAND() { _vuFMAND(&VU1); }
static void VU1MI_FMEQ()  { _vuFMEQ(&VU1); }
static void VU1MI_FMOR()  { _vuFMOR(&VU1); }
static void VU1MI_FCAND() { _vuFCAND(&VU1); }
static void VU1MI_FCEQ()  { _vuFCEQ(&VU1); }
static void VU1MI_FCOR()  { _vuFCOR(&VU1); }
static void VU1MI_FCSET() { _vuFCSET(&VU1); }
static void VU1MI_FCGET() { _vuFCGET(&VU1); }
static void VU1MI_IBEQ() { _vuIBEQ(&VU1); }
static void VU1MI_IBGEZ() { _vuIBGEZ(&VU1); }
static void VU1MI_IBGTZ() { _vuIBGTZ(&VU1); }
static void VU1MI_IBLTZ() { _vuIBLTZ(&VU1); }
static void VU1MI_IBLEZ() { _vuIBLEZ(&VU1); }
static void VU1MI_IBNE() { _vuIBNE(&VU1); }
static void VU1MI_B()   { _vuB(&VU1); }
static void VU1MI_BAL() { _vuBAL(&VU1); }
static void VU1MI_JR()   { _vuJR(&VU1); }
static void VU1MI_JALR() { _vuJALR(&VU1); }
static void VU1MI_MFP() { _vuMFP(&VU1); }
static void VU1MI_WAITP() { _vuWAITP(&VU1); }
static void VU1MI_ESADD()   { _vuESADD(&VU1); }
static void VU1MI_ERSADD()  { _vuERSADD(&VU1); }
static void VU1MI_ELENG()   { _vuELENG(&VU1); }
static void VU1MI_ERLENG()  { _vuERLENG(&VU1); }
static void VU1MI_EATANxy() { _vuEATANxy(&VU1); }
static void VU1MI_EATANxz() { _vuEATANxz(&VU1); }
static void VU1MI_ESUM()    { _vuESUM(&VU1); }
static void VU1MI_ERCPR()   { _vuERCPR(&VU1); }
static void VU1MI_ESQRT()   { _vuESQRT(&VU1); }
static void VU1MI_ERSQRT()  { _vuERSQRT(&VU1); }
static void VU1MI_ESIN()    { _vuESIN(&VU1); }
static void VU1MI_EATAN()   { _vuEATAN(&VU1); }
static void VU1MI_EEXP()    { _vuEEXP(&VU1); }
static void VU1MI_XITOP()   { _vuXITOP(&VU1); }
static void VU1MI_XGKICK()  { _vuXGKICK(&VU1); }
static void VU1MI_XTOP()    { _vuXTOP(&VU1); }



/****************************************/
/*   VU Micromode Upper instructions    */
/****************************************/

static void VU1regsMI_ABS(_VURegsNum* VUregsn) { _vuRegsABS(&VU1, VUregsn); }
static void VU1regsMI_ADD(_VURegsNum* VUregsn) { _vuRegsADD(&VU1, VUregsn); }
static void VU1regsMI_ADDi(_VURegsNum* VUregsn) { _vuRegsADDi(&VU1, VUregsn); }
static void VU1regsMI_ADDq(_VURegsNum* VUregsn) { _vuRegsADDq(&VU1, VUregsn); }
static void VU1regsMI_ADDx(_VURegsNum* VUregsn) { _vuRegsADDx(&VU1, VUregsn); }
static void VU1regsMI_ADDy(_VURegsNum* VUregsn) { _vuRegsADDy(&VU1, VUregsn); }
static void VU1regsMI_ADDz(_VURegsNum* VUregsn) { _vuRegsADDz(&VU1, VUregsn); }
static void VU1regsMI_ADDw(_VURegsNum* VUregsn) { _vuRegsADDw(&VU1, VUregsn); }
static void VU1regsMI_ADDA(_VURegsNum* VUregsn) { _vuRegsADDA(&VU1, VUregsn); }
static void VU1regsMI_ADDAi(_VURegsNum* VUregsn) { _vuRegsADDAi(&VU1, VUregsn); }
static void VU1regsMI_ADDAq(_VURegsNum* VUregsn) { _vuRegsADDAq(&VU1, VUregsn); }
static void VU1regsMI_ADDAx(_VURegsNum* VUregsn) { _vuRegsADDAx(&VU1, VUregsn); }
static void VU1regsMI_ADDAy(_VURegsNum* VUregsn) { _vuRegsADDAy(&VU1, VUregsn); }
static void VU1regsMI_ADDAz(_VURegsNum* VUregsn) { _vuRegsADDAz(&VU1, VUregsn); }
static void VU1regsMI_ADDAw(_VURegsNum* VUregsn) { _vuRegsADDAw(&VU1, VUregsn); }
static void VU1regsMI_SUB(_VURegsNum* VUregsn) { _vuRegsSUB(&VU1, VUregsn); }
static void VU1regsMI_SUBi(_VURegsNum* VUregsn) { _vuRegsSUBi(&VU1, VUregsn); }
static void VU1regsMI_SUBq(_VURegsNum* VUregsn) { _vuRegsSUBq(&VU1, VUregsn); }
static void VU1regsMI_SUBx(_VURegsNum* VUregsn) { _vuRegsSUBx(&VU1, VUregsn); }
static void VU1regsMI_SUBy(_VURegsNum* VUregsn) { _vuRegsSUBy(&VU1, VUregsn); }
static void VU1regsMI_SUBz(_VURegsNum* VUregsn) { _vuRegsSUBz(&VU1, VUregsn); }
static void VU1regsMI_SUBw(_VURegsNum* VUregsn) { _vuRegsSUBw(&VU1, VUregsn); }
static void VU1regsMI_SUBA(_VURegsNum* VUregsn) { _vuRegsSUBA(&VU1, VUregsn); }
static void VU1regsMI_SUBAi(_VURegsNum* VUregsn) { _vuRegsSUBAi(&VU1, VUregsn); }
static void VU1regsMI_SUBAq(_VURegsNum* VUregsn) { _vuRegsSUBAq(&VU1, VUregsn); }
static void VU1regsMI_SUBAx(_VURegsNum* VUregsn) { _vuRegsSUBAx(&VU1, VUregsn); }
static void VU1regsMI_SUBAy(_VURegsNum* VUregsn) { _vuRegsSUBAy(&VU1, VUregsn); }
static void VU1regsMI_SUBAz(_VURegsNum* VUregsn) { _vuRegsSUBAz(&VU1, VUregsn); }
static void VU1regsMI_SUBAw(_VURegsNum* VUregsn) { _vuRegsSUBAw(&VU1, VUregsn); }
static void VU1regsMI_MUL(_VURegsNum* VUregsn) { _vuRegsMUL(&VU1, VUregsn); }
static void VU1regsMI_MULi(_VURegsNum* VUregsn) { _vuRegsMULi(&VU1, VUregsn); }
static void VU1regsMI_MULq(_VURegsNum* VUregsn) { _vuRegsMULq(&VU1, VUregsn); }
static void VU1regsMI_MULx(_VURegsNum* VUregsn) { _vuRegsMULx(&VU1, VUregsn); }
static void VU1regsMI_MULy(_VURegsNum* VUregsn) { _vuRegsMULy(&VU1, VUregsn); }
static void VU1regsMI_MULz(_VURegsNum* VUregsn) { _vuRegsMULz(&VU1, VUregsn); }
static void VU1regsMI_MULw(_VURegsNum* VUregsn) { _vuRegsMULw(&VU1, VUregsn); }
static void VU1regsMI_MULA(_VURegsNum* VUregsn) { _vuRegsMULA(&VU1, VUregsn); }
static void VU1regsMI_MULAi(_VURegsNum* VUregsn) { _vuRegsMULAi(&VU1, VUregsn); }
static void VU1regsMI_MULAq(_VURegsNum* VUregsn) { _vuRegsMULAq(&VU1, VUregsn); }
static void VU1regsMI_MULAx(_VURegsNum* VUregsn) { _vuRegsMULAx(&VU1, VUregsn); }
static void VU1regsMI_MULAy(_VURegsNum* VUregsn) { _vuRegsMULAy(&VU1, VUregsn); }
static void VU1regsMI_MULAz(_VURegsNum* VUregsn) { _vuRegsMULAz(&VU1, VUregsn); }
static void VU1regsMI_MULAw(_VURegsNum* VUregsn) { _vuRegsMULAw(&VU1, VUregsn); }
static void VU1regsMI_MADD(_VURegsNum* VUregsn) { _vuRegsMADD(&VU1, VUregsn); }
static void VU1regsMI_MADDi(_VURegsNum* VUregsn) { _vuRegsMADDi(&VU1, VUregsn); }
static void VU1regsMI_MADDq(_VURegsNum* VUregsn) { _vuRegsMADDq(&VU1, VUregsn); }
static void VU1regsMI_MADDx(_VURegsNum* VUregsn) { _vuRegsMADDx(&VU1, VUregsn); }
static void VU1regsMI_MADDy(_VURegsNum* VUregsn) { _vuRegsMADDy(&VU1, VUregsn); }
static void VU1regsMI_MADDz(_VURegsNum* VUregsn) { _vuRegsMADDz(&VU1, VUregsn); }
static void VU1regsMI_MADDw(_VURegsNum* VUregsn) { _vuRegsMADDw(&VU1, VUregsn); }
static void VU1regsMI_MADDA(_VURegsNum* VUregsn) { _vuRegsMADDA(&VU1, VUregsn); }
static void VU1regsMI_MADDAi(_VURegsNum* VUregsn) { _vuRegsMADDAi(&VU1, VUregsn); }
static void VU1regsMI_MADDAq(_VURegsNum* VUregsn) { _vuRegsMADDAq(&VU1, VUregsn); }
static void VU1regsMI_MADDAx(_VURegsNum* VUregsn) { _vuRegsMADDAx(&VU1, VUregsn); }
static void VU1regsMI_MADDAy(_VURegsNum* VUregsn) { _vuRegsMADDAy(&VU1, VUregsn); }
static void VU1regsMI_MADDAz(_VURegsNum* VUregsn) { _vuRegsMADDAz(&VU1, VUregsn); }
static void VU1regsMI_MADDAw(_VURegsNum* VUregsn) { _vuRegsMADDAw(&VU1, VUregsn); }
static void VU1regsMI_MSUB(_VURegsNum* VUregsn) { _vuRegsMSUB(&VU1, VUregsn); }
static void VU1regsMI_MSUBi(_VURegsNum* VUregsn) { _vuRegsMSUBi(&VU1, VUregsn); }
static void VU1regsMI_MSUBq(_VURegsNum* VUregsn) { _vuRegsMSUBq(&VU1, VUregsn); }
static void VU1regsMI_MSUBx(_VURegsNum* VUregsn) { _vuRegsMSUBx(&VU1, VUregsn); }
static void VU1regsMI_MSUBy(_VURegsNum* VUregsn) { _vuRegsMSUBy(&VU1, VUregsn); }
static void VU1regsMI_MSUBz(_VURegsNum* VUregsn) { _vuRegsMSUBz(&VU1, VUregsn); }
static void VU1regsMI_MSUBw(_VURegsNum* VUregsn) { _vuRegsMSUBw(&VU1, VUregsn); }
static void VU1regsMI_MSUBA(_VURegsNum* VUregsn) { _vuRegsMSUBA(&VU1, VUregsn); }
static void VU1regsMI_MSUBAi(_VURegsNum* VUregsn) { _vuRegsMSUBAi(&VU1, VUregsn); }
static void VU1regsMI_MSUBAq(_VURegsNum* VUregsn) { _vuRegsMSUBAq(&VU1, VUregsn); }
static void VU1regsMI_MSUBAx(_VURegsNum* VUregsn) { _vuRegsMSUBAx(&VU1, VUregsn); }
static void VU1regsMI_MSUBAy(_VURegsNum* VUregsn) { _vuRegsMSUBAy(&VU1, VUregsn); }
static void VU1regsMI_MSUBAz(_VURegsNum* VUregsn) { _vuRegsMSUBAz(&VU1, VUregsn); }
static void VU1regsMI_MSUBAw(_VURegsNum* VUregsn) { _vuRegsMSUBAw(&VU1, VUregsn); }
static void VU1regsMI_MAX(_VURegsNum* VUregsn) { _vuRegsMAX(&VU1, VUregsn); }
static void VU1regsMI_MAXi(_VURegsNum* VUregsn) { _vuRegsMAXi(&VU1, VUregsn); }
static void VU1regsMI_MAXx(_VURegsNum* VUregsn) { _vuRegsMAXx(&VU1, VUregsn); }
static void VU1regsMI_MAXy(_VURegsNum* VUregsn) { _vuRegsMAXy(&VU1, VUregsn); }
static void VU1regsMI_MAXz(_VURegsNum* VUregsn) { _vuRegsMAXz(&VU1, VUregsn); }
static void VU1regsMI_MAXw(_VURegsNum* VUregsn) { _vuRegsMAXw(&VU1, VUregsn); }
static void VU1regsMI_MINI(_VURegsNum* VUregsn) { _vuRegsMINI(&VU1, VUregsn); }
static void VU1regsMI_MINIi(_VURegsNum* VUregsn) { _vuRegsMINIi(&VU1, VUregsn); }
static void VU1regsMI_MINIx(_VURegsNum* VUregsn) { _vuRegsMINIx(&VU1, VUregsn); }
static void VU1regsMI_MINIy(_VURegsNum* VUregsn) { _vuRegsMINIy(&VU1, VUregsn); }
static void VU1regsMI_MINIz(_VURegsNum* VUregsn) { _vuRegsMINIz(&VU1, VUregsn); }
static void VU1regsMI_MINIw(_VURegsNum* VUregsn) { _vuRegsMINIw(&VU1, VUregsn); }
static void VU1regsMI_OPMULA(_VURegsNum* VUregsn) { _vuRegsOPMULA(&VU1, VUregsn); }
static void VU1regsMI_OPMSUB(_VURegsNum* VUregsn) { _vuRegsOPMSUB(&VU1, VUregsn); }
static void VU1regsMI_NOP(_VURegsNum* VUregsn) { _vuRegsNOP(&VU1, VUregsn); }
static void VU1regsMI_FTOI0(_VURegsNum* VUregsn) { _vuRegsFTOI0(&VU1, VUregsn); }
static void VU1regsMI_FTOI4(_VURegsNum* VUregsn) { _vuRegsFTOI4(&VU1, VUregsn); }
static void VU1regsMI_FTOI12(_VURegsNum* VUregsn) { _vuRegsFTOI12(&VU1, VUregsn); }
static void VU1regsMI_FTOI15(_VURegsNum* VUregsn) { _vuRegsFTOI15(&VU1, VUregsn); }
static void VU1regsMI_ITOF0(_VURegsNum* VUregsn) { _vuRegsITOF0(&VU1, VUregsn); }
static void VU1regsMI_ITOF4(_VURegsNum* VUregsn) { _vuRegsITOF4(&VU1, VUregsn); }
static void VU1regsMI_ITOF12(_VURegsNum* VUregsn) { _vuRegsITOF12(&VU1, VUregsn); }
static void VU1regsMI_ITOF15(_VURegsNum* VUregsn) { _vuRegsITOF15(&VU1, VUregsn); }
static void VU1regsMI_CLIP(_VURegsNum* VUregsn) { _vuRegsCLIP(&VU1, VUregsn); }

/*****************************************/
/*   VU Micromode Lower instructions    */
/*****************************************/

static void VU1regsMI_DIV(_VURegsNum* VUregsn) { _vuRegsDIV(&VU1, VUregsn); }
static void VU1regsMI_SQRT(_VURegsNum* VUregsn) { _vuRegsSQRT(&VU1, VUregsn); }
static void VU1regsMI_RSQRT(_VURegsNum* VUregsn) { _vuRegsRSQRT(&VU1, VUregsn); }
static void VU1regsMI_IADD(_VURegsNum* VUregsn) { _vuRegsIADD(&VU1, VUregsn); }
static void VU1regsMI_IADDI(_VURegsNum* VUregsn) { _vuRegsIADDI(&VU1, VUregsn); }
static void VU1regsMI_IADDIU(_VURegsNum* VUregsn) { _vuRegsIADDIU(&VU1, VUregsn); }
static void VU1regsMI_IAND(_VURegsNum* VUregsn) { _vuRegsIAND(&VU1, VUregsn); }
static void VU1regsMI_IOR(_VURegsNum* VUregsn) { _vuRegsIOR(&VU1, VUregsn); }
static void VU1regsMI_ISUB(_VURegsNum* VUregsn) { _vuRegsISUB(&VU1, VUregsn); }
static void VU1regsMI_ISUBIU(_VURegsNum* VUregsn) { _vuRegsISUBIU(&VU1, VUregsn); }
static void VU1regsMI_MOVE(_VURegsNum* VUregsn) { _vuRegsMOVE(&VU1, VUregsn); }
static void VU1regsMI_MFIR(_VURegsNum* VUregsn) { _vuRegsMFIR(&VU1, VUregsn); }
static void VU1regsMI_MTIR(_VURegsNum* VUregsn) { _vuRegsMTIR(&VU1, VUregsn); }
static void VU1regsMI_MR32(_VURegsNum* VUregsn) { _vuRegsMR32(&VU1, VUregsn); }
static void VU1regsMI_LQ(_VURegsNum* VUregsn) { _vuRegsLQ(&VU1, VUregsn); }
static void VU1regsMI_LQD(_VURegsNum* VUregsn) { _vuRegsLQD(&VU1, VUregsn); }
static void VU1regsMI_LQI(_VURegsNum* VUregsn) { _vuRegsLQI(&VU1, VUregsn); }
static void VU1regsMI_SQ(_VURegsNum* VUregsn) { _vuRegsSQ(&VU1, VUregsn); }
static void VU1regsMI_SQD(_VURegsNum* VUregsn) { _vuRegsSQD(&VU1, VUregsn); }
static void VU1regsMI_SQI(_VURegsNum* VUregsn) { _vuRegsSQI(&VU1, VUregsn); }
static void VU1regsMI_ILW(_VURegsNum* VUregsn) { _vuRegsILW(&VU1, VUregsn); }
static void VU1regsMI_ISW(_VURegsNum* VUregsn) { _vuRegsISW(&VU1, VUregsn); }
static void VU1regsMI_ILWR(_VURegsNum* VUregsn) { _vuRegsILWR(&VU1, VUregsn); }
static void VU1regsMI_ISWR(_VURegsNum* VUregsn) { _vuRegsISWR(&VU1, VUregsn); }
static void VU1regsMI_RINIT(_VURegsNum* VUregsn) { _vuRegsRINIT(&VU1, VUregsn); }
static void VU1regsMI_RGET(_VURegsNum* VUregsn) { _vuRegsRGET(&VU1, VUregsn); }
static void VU1regsMI_RNEXT(_VURegsNum* VUregsn) { _vuRegsRNEXT(&VU1, VUregsn); }
static void VU1regsMI_RXOR(_VURegsNum* VUregsn) { _vuRegsRXOR(&VU1, VUregsn); }
static void VU1regsMI_WAITQ(_VURegsNum* VUregsn) { _vuRegsWAITQ(&VU1, VUregsn); }
static void VU1regsMI_FSAND(_VURegsNum* VUregsn) { _vuRegsFSAND(&VU1, VUregsn); }
static void VU1regsMI_FSEQ(_VURegsNum* VUregsn) { _vuRegsFSEQ(&VU1, VUregsn); }
static void VU1regsMI_FSOR(_VURegsNum* VUregsn) { _vuRegsFSOR(&VU1, VUregsn); }
static void VU1regsMI_FSSET(_VURegsNum* VUregsn) { _vuRegsFSSET(&VU1, VUregsn); }
static void VU1regsMI_FMAND(_VURegsNum* VUregsn) { _vuRegsFMAND(&VU1, VUregsn); }
static void VU1regsMI_FMEQ(_VURegsNum* VUregsn) { _vuRegsFMEQ(&VU1, VUregsn); }
static void VU1regsMI_FMOR(_VURegsNum* VUregsn) { _vuRegsFMOR(&VU1, VUregsn); }
static void VU1regsMI_FCAND(_VURegsNum* VUregsn) { _vuRegsFCAND(&VU1, VUregsn); }
static void VU1regsMI_FCEQ(_VURegsNum* VUregsn) { _vuRegsFCEQ(&VU1, VUregsn); }
static void VU1regsMI_FCOR(_VURegsNum* VUregsn) { _vuRegsFCOR(&VU1, VUregsn); }
static void VU1regsMI_FCSET(_VURegsNum* VUregsn) { _vuRegsFCSET(&VU1, VUregsn); }
static void VU1regsMI_FCGET(_VURegsNum* VUregsn) { _vuRegsFCGET(&VU1, VUregsn); }
static void VU1regsMI_IBEQ(_VURegsNum* VUregsn) { _vuRegsIBEQ(&VU1, VUregsn); }
static void VU1regsMI_IBGEZ(_VURegsNum* VUregsn) { _vuRegsIBGEZ(&VU1, VUregsn); }
static void VU1regsMI_IBGTZ(_VURegsNum* VUregsn) { _vuRegsIBGTZ(&VU1, VUregsn); }
static void VU1regsMI_IBLTZ(_VURegsNum* VUregsn) { _vuRegsIBLTZ(&VU1, VUregsn); }
static void VU1regsMI_IBLEZ(_VURegsNum* VUregsn) { _vuRegsIBLEZ(&VU1, VUregsn); }
static void VU1regsMI_IBNE(_VURegsNum* VUregsn) { _vuRegsIBNE(&VU1, VUregsn); }
static void VU1regsMI_B(_VURegsNum* VUregsn) { _vuRegsB(&VU1, VUregsn); }
static void VU1regsMI_BAL(_VURegsNum* VUregsn) { _vuRegsBAL(&VU1, VUregsn); }
static void VU1regsMI_JR(_VURegsNum* VUregsn) { _vuRegsJR(&VU1, VUregsn); }
static void VU1regsMI_JALR(_VURegsNum* VUregsn) { _vuRegsJALR(&VU1, VUregsn); }
static void VU1regsMI_MFP(_VURegsNum* VUregsn) { _vuRegsMFP(&VU1, VUregsn); }
static void VU1regsMI_WAITP(_VURegsNum* VUregsn) { _vuRegsWAITP(&VU1, VUregsn); }
static void VU1regsMI_ESADD(_VURegsNum* VUregsn) { _vuRegsESADD(&VU1, VUregsn); }
static void VU1regsMI_ERSADD(_VURegsNum* VUregsn) { _vuRegsERSADD(&VU1, VUregsn); }
static void VU1regsMI_ELENG(_VURegsNum* VUregsn) { _vuRegsELENG(&VU1, VUregsn); }
static void VU1regsMI_ERLENG(_VURegsNum* VUregsn) { _vuRegsERLENG(&VU1, VUregsn); }
static void VU1regsMI_EATANxy(_VURegsNum* VUregsn) { _vuRegsEATANxy(&VU1, VUregsn); }
static void VU1regsMI_EATANxz(_VURegsNum* VUregsn) { _vuRegsEATANxz(&VU1, VUregsn); }
static void VU1regsMI_ESUM(_VURegsNum* VUregsn) { _vuRegsESUM(&VU1, VUregsn); }
static void VU1regsMI_ERCPR(_VURegsNum* VUregsn) { _vuRegsERCPR(&VU1, VUregsn); }
static void VU1regsMI_ESQRT(_VURegsNum* VUregsn) { _vuRegsESQRT(&VU1, VUregsn); }
static void VU1regsMI_ERSQRT(_VURegsNum* VUregsn) { _vuRegsERSQRT(&VU1, VUregsn); }
static void VU1regsMI_ESIN(_VURegsNum* VUregsn) { _vuRegsESIN(&VU1, VUregsn); }
static void VU1regsMI_EATAN(_VURegsNum* VUregsn) { _vuRegsEATAN(&VU1, VUregsn); }
static void VU1regsMI_EEXP(_VURegsNum* VUregsn) { _vuRegsEEXP(&VU1, VUregsn); }
static void VU1regsMI_XITOP(_VURegsNum* VUregsn) { _vuRegsXITOP(&VU1, VUregsn); }
static void VU1regsMI_XGKICK(_VURegsNum* VUregsn) { _vuRegsXGKICK(&VU1, VUregsn); }
static void VU1regsMI_XTOP(_VURegsNum* VUregsn) { _vuRegsXTOP(&VU1, VUregsn); }

static void VU1unknown()
{
	pxFail("Unknown VU micromode opcode called");
	CPU_LOG("Unknown VU micromode opcode called");
}

static void VU1regsunknown(_VURegsNum* VUregsn)
{
	pxFail("Unknown VU micromode opcode called");
	CPU_LOG("Unknown VU micromode opcode called");
}



// --------------------------------------------------------------------------------------
//  VU Micromode Tables/Opcodes defs macros
// --------------------------------------------------------------------------------------

#define _vuTablesMess(PREFIX, FNTYPE) \
alignas(16) static const FNTYPE PREFIX##LowerOP_T3_00_OPCODE[32] = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_MOVE  , PREFIX##MI_LQI   , PREFIX##MI_DIV  , PREFIX##MI_MTIR,  \
	PREFIX##MI_RNEXT , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_MFP   , PREFIX##MI_XTOP , PREFIX##MI_XGKICK,  \
	PREFIX##MI_ESADD , PREFIX##MI_EATANxy, PREFIX##MI_ESQRT, PREFIX##MI_ESIN,  \
}; \
 \
alignas(16) static const FNTYPE PREFIX##LowerOP_T3_01_OPCODE[32] = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_MR32  , PREFIX##MI_SQI   , PREFIX##MI_SQRT , PREFIX##MI_MFIR,  \
	PREFIX##MI_RGET  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##MI_XITOP, PREFIX##unknown,  \
	PREFIX##MI_ERSADD, PREFIX##MI_EATANxz, PREFIX##MI_ERSQRT, PREFIX##MI_EATAN, \
}; \
 \
alignas(16) static const FNTYPE PREFIX##LowerOP_T3_10_OPCODE[32] = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_LQD   , PREFIX##MI_RSQRT, PREFIX##MI_ILWR,  \
	PREFIX##MI_RINIT , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ELENG , PREFIX##MI_ESUM  , PREFIX##MI_ERCPR, PREFIX##MI_EEXP,  \
}; \
 \
alignas(16) static const FNTYPE PREFIX##LowerOP_T3_11_OPCODE[32] = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_SQD   , PREFIX##MI_WAITQ, PREFIX##MI_ISWR,  \
	PREFIX##MI_RXOR  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ERLENG, PREFIX##unknown  , PREFIX##MI_WAITP, PREFIX##unknown,  \
}; \
 \
alignas(16) static const FNTYPE PREFIX##LowerOP_OPCODE[64] = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x20 */  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_IADD  , PREFIX##MI_ISUB  , PREFIX##MI_IADDI, PREFIX##unknown, /* 0x30 */ \
	PREFIX##MI_IAND  , PREFIX##MI_IOR   , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##LowerOP_T3_00, PREFIX##LowerOP_T3_01, PREFIX##LowerOP_T3_10, PREFIX##LowerOP_T3_11,  \
}; \
 \
alignas(16) const FNTYPE PREFIX##_LOWER_OPCODE[128] = { \
	PREFIX##MI_LQ    , PREFIX##MI_SQ    , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ILW   , PREFIX##MI_ISW   , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_IADDIU, PREFIX##MI_ISUBIU, PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_FCEQ  , PREFIX##MI_FCSET , PREFIX##MI_FCAND, PREFIX##MI_FCOR, /* 0x10 */ \
	PREFIX##MI_FSEQ  , PREFIX##MI_FSSET , PREFIX##MI_FSAND, PREFIX##MI_FSOR, \
	PREFIX##MI_FMEQ  , PREFIX##unknown  , PREFIX##MI_FMAND, PREFIX##MI_FMOR, \
	PREFIX##MI_FCGET , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_B     , PREFIX##MI_BAL   , PREFIX##unknown , PREFIX##unknown, /* 0x20 */  \
	PREFIX##MI_JR    , PREFIX##MI_JALR  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_IBEQ  , PREFIX##MI_IBNE  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_IBLTZ , PREFIX##MI_IBGTZ , PREFIX##MI_IBLEZ, PREFIX##MI_IBGEZ, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x30 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##LowerOP  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x40*/  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x50 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x60 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x70 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
}; \
 \
alignas(16) static const FNTYPE PREFIX##_UPPER_FD_00_TABLE[32] = { \
	PREFIX##MI_ADDAx, PREFIX##MI_SUBAx , PREFIX##MI_MADDAx, PREFIX##MI_MSUBAx, \
	PREFIX##MI_ITOF0, PREFIX##MI_FTOI0, PREFIX##MI_MULAx , PREFIX##MI_MULAq , \
	PREFIX##MI_ADDAq, PREFIX##MI_SUBAq, PREFIX##MI_ADDA  , PREFIX##MI_SUBA  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
alignas(16) static const FNTYPE PREFIX##_UPPER_FD_01_TABLE[32] = { \
	PREFIX##MI_ADDAy , PREFIX##MI_SUBAy  , PREFIX##MI_MADDAy, PREFIX##MI_MSUBAy, \
	PREFIX##MI_ITOF4 , PREFIX##MI_FTOI4 , PREFIX##MI_MULAy , PREFIX##MI_ABS   , \
	PREFIX##MI_MADDAq, PREFIX##MI_MSUBAq, PREFIX##MI_MADDA , PREFIX##MI_MSUBA , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
alignas(16) static const FNTYPE PREFIX##_UPPER_FD_10_TABLE[32] = { \
	PREFIX##MI_ADDAz , PREFIX##MI_SUBAz  , PREFIX##MI_MADDAz, PREFIX##MI_MSUBAz, \
	PREFIX##MI_ITOF12, PREFIX##MI_FTOI12, PREFIX##MI_MULAz , PREFIX##MI_MULAi , \
	PREFIX##MI_ADDAi, PREFIX##MI_SUBAi , PREFIX##MI_MULA  , PREFIX##MI_OPMULA, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
alignas(16) static const FNTYPE PREFIX##_UPPER_FD_11_TABLE[32] = { \
	PREFIX##MI_ADDAw , PREFIX##MI_SUBAw  , PREFIX##MI_MADDAw, PREFIX##MI_MSUBAw, \
	PREFIX##MI_ITOF15, PREFIX##MI_FTOI15, PREFIX##MI_MULAw , PREFIX##MI_CLIP  , \
	PREFIX##MI_MADDAi, PREFIX##MI_MSUBAi, PREFIX##unknown  , PREFIX##MI_NOP   , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
alignas(16) const FNTYPE PREFIX##_UPPER_OPCODE[64] = { \
	PREFIX##MI_ADDx  , PREFIX##MI_ADDy  , PREFIX##MI_ADDz  , PREFIX##MI_ADDw, \
	PREFIX##MI_SUBx  , PREFIX##MI_SUBy  , PREFIX##MI_SUBz  , PREFIX##MI_SUBw, \
	PREFIX##MI_MADDx , PREFIX##MI_MADDy , PREFIX##MI_MADDz , PREFIX##MI_MADDw, \
	PREFIX##MI_MSUBx , PREFIX##MI_MSUBy , PREFIX##MI_MSUBz , PREFIX##MI_MSUBw, \
	PREFIX##MI_MAXx  , PREFIX##MI_MAXy  , PREFIX##MI_MAXz  , PREFIX##MI_MAXw,  /* 0x10 */  \
	PREFIX##MI_MINIx , PREFIX##MI_MINIy , PREFIX##MI_MINIz , PREFIX##MI_MINIw, \
	PREFIX##MI_MULx  , PREFIX##MI_MULy  , PREFIX##MI_MULz  , PREFIX##MI_MULw, \
	PREFIX##MI_MULq  , PREFIX##MI_MAXi  , PREFIX##MI_MULi  , PREFIX##MI_MINIi, \
	PREFIX##MI_ADDq  , PREFIX##MI_MADDq , PREFIX##MI_ADDi  , PREFIX##MI_MADDi, /* 0x20 */ \
	PREFIX##MI_SUBq  , PREFIX##MI_MSUBq , PREFIX##MI_SUBi  , PREFIX##MI_MSUBi, \
	PREFIX##MI_ADD   , PREFIX##MI_MADD  , PREFIX##MI_MUL   , PREFIX##MI_MAX, \
	PREFIX##MI_SUB   , PREFIX##MI_MSUB  , PREFIX##MI_OPMSUB, PREFIX##MI_MINI, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown,  /* 0x30 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown, \
	PREFIX##_UPPER_FD_00, PREFIX##_UPPER_FD_01, PREFIX##_UPPER_FD_10, PREFIX##_UPPER_FD_11,  \
};


#define _vuTablesPre(VU, PREFIX) \
 \
 static void PREFIX##_UPPER_FD_00(); \
 static void PREFIX##_UPPER_FD_01(); \
 static void PREFIX##_UPPER_FD_10(); \
 static void PREFIX##_UPPER_FD_11(); \
 static void PREFIX##LowerOP(); \
 static void PREFIX##LowerOP_T3_00(); \
 static void PREFIX##LowerOP_T3_01(); \
 static void PREFIX##LowerOP_T3_10(); \
 static void PREFIX##LowerOP_T3_11(); \

#define _vuTablesPost(VU, PREFIX) \
 \
 static void PREFIX##_UPPER_FD_00() { \
 PREFIX##_UPPER_FD_00_TABLE[(VU.code >> 6) & 0x1f ](); \
} \
 \
 static void PREFIX##_UPPER_FD_01() { \
 PREFIX##_UPPER_FD_01_TABLE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##_UPPER_FD_10() { \
 PREFIX##_UPPER_FD_10_TABLE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##_UPPER_FD_11() { \
 PREFIX##_UPPER_FD_11_TABLE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##LowerOP() { \
 PREFIX##LowerOP_OPCODE[VU.code & 0x3f](); \
} \
 \
 static void PREFIX##LowerOP_T3_00() { \
 PREFIX##LowerOP_T3_00_OPCODE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##LowerOP_T3_01() { \
 PREFIX##LowerOP_T3_01_OPCODE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##LowerOP_T3_10() { \
 PREFIX##LowerOP_T3_10_OPCODE[(VU.code >> 6) & 0x1f](); \
} \
 \
 static void PREFIX##LowerOP_T3_11() { \
 PREFIX##LowerOP_T3_11_OPCODE[(VU.code >> 6) & 0x1f](); \
} \


// --------------------------------------------------------------------------------------
//  VuRegsN Tables
// --------------------------------------------------------------------------------------

#define _vuRegsTables(VU, PREFIX, FNTYPE) \
 static void PREFIX##_UPPER_FD_00(_VURegsNum *VUregsn); \
 static void PREFIX##_UPPER_FD_01(_VURegsNum *VUregsn); \
 static void PREFIX##_UPPER_FD_10(_VURegsNum *VUregsn); \
 static void PREFIX##_UPPER_FD_11(_VURegsNum *VUregsn); \
 static void PREFIX##LowerOP(_VURegsNum *VUregsn); \
 static void PREFIX##LowerOP_T3_00(_VURegsNum *VUregsn); \
 static void PREFIX##LowerOP_T3_01(_VURegsNum *VUregsn); \
 static void PREFIX##LowerOP_T3_10(_VURegsNum *VUregsn); \
 static void PREFIX##LowerOP_T3_11(_VURegsNum *VUregsn); \
 \
 _vuTablesMess(PREFIX, FNTYPE) \
 \
 static void PREFIX##_UPPER_FD_00(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_00_TABLE[(VU.code >> 6) & 0x1f ](VUregsn); \
} \
 \
 static void PREFIX##_UPPER_FD_01(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_01_TABLE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##_UPPER_FD_10(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_10_TABLE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##_UPPER_FD_11(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_11_TABLE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##LowerOP(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_OPCODE[VU.code & 0x3f](VUregsn); \
} \
 \
 static void PREFIX##LowerOP_T3_00(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_00_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##LowerOP_T3_01(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_01_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##LowerOP_T3_10(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_10_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 static void PREFIX##LowerOP_T3_11(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_11_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
} \

_vuTablesPre(VU0, VU0)
_vuTablesMess(VU0, FnPtr_VuVoid)
_vuTablesPost(VU0, VU0)

_vuTablesPre(VU1, VU1)
_vuTablesMess(VU1, FnPtr_VuVoid)
_vuTablesPost(VU1, VU1)

_vuRegsTables(VU0, VU0regs, FnPtr_VuRegsN)
_vuRegsTables(VU1, VU1regs, FnPtr_VuRegsN)


// --------------------------------------------------------------------------------------
//  VU0macro (COP2)
// --------------------------------------------------------------------------------------

static __fi void SYNCMSFLAGS()
{
	VU0.VI[REG_STATUS_FLAG].UL = (VU0.VI[REG_STATUS_FLAG].UL & 0xFC0) | (VU0.statusflag & 0xF) | ((VU0.statusflag & 0xF) << 6);
	VU0.VI[REG_MAC_FLAG].UL = VU0.macflag;
}

static __fi void SYNCCLIPFLAG()
{
	VU0.VI[REG_CLIP_FLAG].UL = VU0.clipflag;
}

static __fi void SYNCSTATUSFLAG()
{
	VU0.VI[REG_STATUS_FLAG].UL = (VU0.VI[REG_STATUS_FLAG].UL & 0xFC0) | (VU0.statusflag & 0xF) | ((VU0.statusflag & 0xF) << 6);
}

static __fi void SYNCFDIV()
{
	VU0.VI[REG_Q].UL = VU0.q.UL;
	VU0.VI[REG_STATUS_FLAG].UL = (VU0.VI[REG_STATUS_FLAG].UL & 0x3CF) | (VU0.statusflag & 0x30) | ((VU0.statusflag & 0x30) << 6);
}

void VABS()  { VU0.code = cpuRegs.code; _vuABS(&VU0); }
void VADD()  { VU0.code = cpuRegs.code; _vuADD(&VU0); SYNCMSFLAGS(); }
void VADDi() { VU0.code = cpuRegs.code; _vuADDi(&VU0); SYNCMSFLAGS(); }
void VADDq() { VU0.code = cpuRegs.code; _vuADDq(&VU0); SYNCMSFLAGS(); }
void VADDx() { VU0.code = cpuRegs.code; _vuADDx(&VU0); SYNCMSFLAGS(); }
void VADDy() { VU0.code = cpuRegs.code; _vuADDy(&VU0); SYNCMSFLAGS(); }
void VADDz() { VU0.code = cpuRegs.code; _vuADDz(&VU0); SYNCMSFLAGS(); }
void VADDw() { VU0.code = cpuRegs.code; _vuADDw(&VU0); SYNCMSFLAGS(); }
void VADDA() { VU0.code = cpuRegs.code; _vuADDA(&VU0); SYNCMSFLAGS(); }
void VADDAi() { VU0.code = cpuRegs.code; _vuADDAi(&VU0); SYNCMSFLAGS(); }
void VADDAq() { VU0.code = cpuRegs.code; _vuADDAq(&VU0); SYNCMSFLAGS(); }
void VADDAx() { VU0.code = cpuRegs.code; _vuADDAx(&VU0); SYNCMSFLAGS(); }
void VADDAy() { VU0.code = cpuRegs.code; _vuADDAy(&VU0); SYNCMSFLAGS(); }
void VADDAz() { VU0.code = cpuRegs.code; _vuADDAz(&VU0); SYNCMSFLAGS(); }
void VADDAw() { VU0.code = cpuRegs.code; _vuADDAw(&VU0); SYNCMSFLAGS(); }
void VSUB()  { VU0.code = cpuRegs.code; _vuSUB(&VU0); SYNCMSFLAGS(); }
void VSUBi() { VU0.code = cpuRegs.code; _vuSUBi(&VU0); SYNCMSFLAGS(); }
void VSUBq() { VU0.code = cpuRegs.code; _vuSUBq(&VU0); SYNCMSFLAGS(); }
void VSUBx() { VU0.code = cpuRegs.code; _vuSUBx(&VU0); SYNCMSFLAGS(); }
void VSUBy() { VU0.code = cpuRegs.code; _vuSUBy(&VU0); SYNCMSFLAGS(); }
void VSUBz() { VU0.code = cpuRegs.code; _vuSUBz(&VU0); SYNCMSFLAGS(); }
void VSUBw() { VU0.code = cpuRegs.code; _vuSUBw(&VU0); SYNCMSFLAGS(); }
void VSUBA()  { VU0.code = cpuRegs.code; _vuSUBA(&VU0); SYNCMSFLAGS(); }
void VSUBAi() { VU0.code = cpuRegs.code; _vuSUBAi(&VU0); SYNCMSFLAGS(); }
void VSUBAq() { VU0.code = cpuRegs.code; _vuSUBAq(&VU0); SYNCMSFLAGS(); }
void VSUBAx() { VU0.code = cpuRegs.code; _vuSUBAx(&VU0); SYNCMSFLAGS(); }
void VSUBAy() { VU0.code = cpuRegs.code; _vuSUBAy(&VU0); SYNCMSFLAGS(); }
void VSUBAz() { VU0.code = cpuRegs.code; _vuSUBAz(&VU0); SYNCMSFLAGS(); }
void VSUBAw() { VU0.code = cpuRegs.code; _vuSUBAw(&VU0); SYNCMSFLAGS(); }
void VMUL()  { VU0.code = cpuRegs.code; _vuMUL(&VU0); SYNCMSFLAGS(); }
void VMULi() { VU0.code = cpuRegs.code; _vuMULi(&VU0); SYNCMSFLAGS(); }
void VMULq() { VU0.code = cpuRegs.code; _vuMULq(&VU0); SYNCMSFLAGS(); }
void VMULx() { VU0.code = cpuRegs.code; _vuMULx(&VU0); SYNCMSFLAGS(); }
void VMULy() { VU0.code = cpuRegs.code; _vuMULy(&VU0); SYNCMSFLAGS(); }
void VMULz() { VU0.code = cpuRegs.code; _vuMULz(&VU0); SYNCMSFLAGS(); }
void VMULw() { VU0.code = cpuRegs.code; _vuMULw(&VU0); SYNCMSFLAGS(); }
void VMULA()  { VU0.code = cpuRegs.code; _vuMULA(&VU0); SYNCMSFLAGS(); }
void VMULAi() { VU0.code = cpuRegs.code; _vuMULAi(&VU0); SYNCMSFLAGS(); }
void VMULAq() { VU0.code = cpuRegs.code; _vuMULAq(&VU0); SYNCMSFLAGS(); }
void VMULAx() { VU0.code = cpuRegs.code; _vuMULAx(&VU0); SYNCMSFLAGS(); }
void VMULAy() { VU0.code = cpuRegs.code; _vuMULAy(&VU0); SYNCMSFLAGS(); }
void VMULAz() { VU0.code = cpuRegs.code; _vuMULAz(&VU0); SYNCMSFLAGS(); }
void VMULAw() { VU0.code = cpuRegs.code; _vuMULAw(&VU0); SYNCMSFLAGS(); }
void VMADD()  { VU0.code = cpuRegs.code; _vuMADD(&VU0); SYNCMSFLAGS(); }
void VMADDi() { VU0.code = cpuRegs.code; _vuMADDi(&VU0); SYNCMSFLAGS(); }
void VMADDq() { VU0.code = cpuRegs.code; _vuMADDq(&VU0); SYNCMSFLAGS(); }
void VMADDx() { VU0.code = cpuRegs.code; _vuMADDx(&VU0); SYNCMSFLAGS(); }
void VMADDy() { VU0.code = cpuRegs.code; _vuMADDy(&VU0); SYNCMSFLAGS(); }
void VMADDz() { VU0.code = cpuRegs.code; _vuMADDz(&VU0); SYNCMSFLAGS(); }
void VMADDw() { VU0.code = cpuRegs.code; _vuMADDw(&VU0); SYNCMSFLAGS(); }
void VMADDA()  { VU0.code = cpuRegs.code; _vuMADDA(&VU0); SYNCMSFLAGS(); }
void VMADDAi() { VU0.code = cpuRegs.code; _vuMADDAi(&VU0); SYNCMSFLAGS(); }
void VMADDAq() { VU0.code = cpuRegs.code; _vuMADDAq(&VU0); SYNCMSFLAGS(); }
void VMADDAx() { VU0.code = cpuRegs.code; _vuMADDAx(&VU0); SYNCMSFLAGS(); }
void VMADDAy() { VU0.code = cpuRegs.code; _vuMADDAy(&VU0); SYNCMSFLAGS(); }
void VMADDAz() { VU0.code = cpuRegs.code; _vuMADDAz(&VU0); SYNCMSFLAGS(); }
void VMADDAw() { VU0.code = cpuRegs.code; _vuMADDAw(&VU0); SYNCMSFLAGS(); }
void VMSUB()  { VU0.code = cpuRegs.code; _vuMSUB(&VU0); SYNCMSFLAGS(); }
void VMSUBi() { VU0.code = cpuRegs.code; _vuMSUBi(&VU0); SYNCMSFLAGS(); }
void VMSUBq() { VU0.code = cpuRegs.code; _vuMSUBq(&VU0); SYNCMSFLAGS(); }
void VMSUBx() { VU0.code = cpuRegs.code; _vuMSUBx(&VU0); SYNCMSFLAGS(); }
void VMSUBy() { VU0.code = cpuRegs.code; _vuMSUBy(&VU0); SYNCMSFLAGS(); }
void VMSUBz() { VU0.code = cpuRegs.code; _vuMSUBz(&VU0); SYNCMSFLAGS(); }
void VMSUBw() { VU0.code = cpuRegs.code; _vuMSUBw(&VU0); SYNCMSFLAGS(); }
void VMSUBA()  { VU0.code = cpuRegs.code; _vuMSUBA(&VU0); SYNCMSFLAGS(); }
void VMSUBAi() { VU0.code = cpuRegs.code; _vuMSUBAi(&VU0); SYNCMSFLAGS(); }
void VMSUBAq() { VU0.code = cpuRegs.code; _vuMSUBAq(&VU0); SYNCMSFLAGS(); }
void VMSUBAx() { VU0.code = cpuRegs.code; _vuMSUBAx(&VU0); SYNCMSFLAGS(); }
void VMSUBAy() { VU0.code = cpuRegs.code; _vuMSUBAy(&VU0); SYNCMSFLAGS(); }
void VMSUBAz() { VU0.code = cpuRegs.code; _vuMSUBAz(&VU0); SYNCMSFLAGS(); }
void VMSUBAw() { VU0.code = cpuRegs.code; _vuMSUBAw(&VU0); SYNCMSFLAGS(); }
void VMAX()  { VU0.code = cpuRegs.code; _vuMAX(&VU0); }
void VMAXi() { VU0.code = cpuRegs.code; _vuMAXi(&VU0); }
void VMAXx() { VU0.code = cpuRegs.code; _vuMAXx(&VU0); }
void VMAXy() { VU0.code = cpuRegs.code; _vuMAXy(&VU0); }
void VMAXz() { VU0.code = cpuRegs.code; _vuMAXz(&VU0); }
void VMAXw() { VU0.code = cpuRegs.code; _vuMAXw(&VU0); }
void VMINI()  { VU0.code = cpuRegs.code; _vuMINI(&VU0); }
void VMINIi() { VU0.code = cpuRegs.code; _vuMINIi(&VU0); }
void VMINIx() { VU0.code = cpuRegs.code; _vuMINIx(&VU0); }
void VMINIy() { VU0.code = cpuRegs.code; _vuMINIy(&VU0); }
void VMINIz() { VU0.code = cpuRegs.code; _vuMINIz(&VU0); }
void VMINIw() { VU0.code = cpuRegs.code; _vuMINIw(&VU0); }
void VOPMULA() { VU0.code = cpuRegs.code; _vuOPMULA(&VU0); SYNCMSFLAGS(); }
void VOPMSUB() { VU0.code = cpuRegs.code; _vuOPMSUB(&VU0); SYNCMSFLAGS(); }
void VNOP()    { VU0.code = cpuRegs.code; _vuNOP(&VU0); }
void VFTOI0()  { VU0.code = cpuRegs.code; _vuFTOI0(&VU0); }
void VFTOI4()  { VU0.code = cpuRegs.code; _vuFTOI4(&VU0); }
void VFTOI12() { VU0.code = cpuRegs.code; _vuFTOI12(&VU0); }
void VFTOI15() { VU0.code = cpuRegs.code; _vuFTOI15(&VU0); }
void VITOF0()  { VU0.code = cpuRegs.code; _vuITOF0(&VU0); }
void VITOF4()  { VU0.code = cpuRegs.code; _vuITOF4(&VU0); }
void VITOF12() { VU0.code = cpuRegs.code; _vuITOF12(&VU0); }
void VITOF15() { VU0.code = cpuRegs.code; _vuITOF15(&VU0); }
void VCLIPw()  { VU0.code = cpuRegs.code; _vuCLIP(&VU0); SYNCCLIPFLAG(); }

void VDIV()    { VU0.code = cpuRegs.code; _vuDIV(&VU0); SYNCFDIV(); }
void VSQRT()   { VU0.code = cpuRegs.code; _vuSQRT(&VU0); SYNCFDIV(); }
void VRSQRT()  { VU0.code = cpuRegs.code; _vuRSQRT(&VU0); SYNCFDIV(); }
void VIADD()   { VU0.code = cpuRegs.code; _vuIADD(&VU0); }
void VIADDI()  { VU0.code = cpuRegs.code; _vuIADDI(&VU0); }
void VIADDIU() { VU0.code = cpuRegs.code; _vuIADDIU(&VU0); }
void VIAND()   { VU0.code = cpuRegs.code; _vuIAND(&VU0); }
void VIOR()    { VU0.code = cpuRegs.code; _vuIOR(&VU0); }
void VISUB()   { VU0.code = cpuRegs.code; _vuISUB(&VU0); }
void VISUBIU() { VU0.code = cpuRegs.code; _vuISUBIU(&VU0); }
void VMOVE()   { VU0.code = cpuRegs.code; _vuMOVE(&VU0); }
void VMFIR()   { VU0.code = cpuRegs.code; _vuMFIR(&VU0); }
void VMTIR()   { VU0.code = cpuRegs.code; _vuMTIR(&VU0); }
void VMR32()   { VU0.code = cpuRegs.code; _vuMR32(&VU0); }
void VLQ()     { VU0.code = cpuRegs.code; _vuLQ(&VU0); }
void VLQD()    { VU0.code = cpuRegs.code; _vuLQD(&VU0); }
void VLQI()    { VU0.code = cpuRegs.code; _vuLQI(&VU0); }
void VSQ()     { VU0.code = cpuRegs.code; _vuSQ(&VU0); }
void VSQD()    { VU0.code = cpuRegs.code; _vuSQD(&VU0); }
void VSQI()    { VU0.code = cpuRegs.code; _vuSQI(&VU0); }
void VILW()    { VU0.code = cpuRegs.code; _vuILW(&VU0); }
void VISW()    { VU0.code = cpuRegs.code; _vuISW(&VU0); }
void VILWR()   { VU0.code = cpuRegs.code; _vuILWR(&VU0); }
void VISWR()   { VU0.code = cpuRegs.code; _vuISWR(&VU0); }
void VRINIT()  { VU0.code = cpuRegs.code; _vuRINIT(&VU0); }
void VRGET()   { VU0.code = cpuRegs.code; _vuRGET(&VU0); }
void VRNEXT()  { VU0.code = cpuRegs.code; _vuRNEXT(&VU0); }
void VRXOR()   { VU0.code = cpuRegs.code; _vuRXOR(&VU0); }
void VWAITQ()  { VU0.code = cpuRegs.code; _vuWAITQ(&VU0); }
void VFSAND()  { VU0.code = cpuRegs.code; _vuFSAND(&VU0); }
void VFSEQ()   { VU0.code = cpuRegs.code; _vuFSEQ(&VU0); }
void VFSOR()   { VU0.code = cpuRegs.code; _vuFSOR(&VU0); }
void VFSSET()  { VU0.code = cpuRegs.code; _vuFSSET(&VU0); SYNCSTATUSFLAG(); }
void VFMAND()  { VU0.code = cpuRegs.code; _vuFMAND(&VU0); }
void VFMEQ()   { VU0.code = cpuRegs.code; _vuFMEQ(&VU0); }
void VFMOR()   { VU0.code = cpuRegs.code; _vuFMOR(&VU0); }
void VFCAND()  { VU0.code = cpuRegs.code; _vuFCAND(&VU0); }
void VFCEQ()   { VU0.code = cpuRegs.code; _vuFCEQ(&VU0); }
void VFCOR()   { VU0.code = cpuRegs.code; _vuFCOR(&VU0); }
void VFCSET()  { VU0.code = cpuRegs.code; _vuFCSET(&VU0); SYNCCLIPFLAG(); }
void VFCGET()  { VU0.code = cpuRegs.code; _vuFCGET(&VU0); }
void VXITOP()  { VU0.code = cpuRegs.code; _vuXITOP(&VU0); }

