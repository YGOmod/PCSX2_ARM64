// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "VUmicro.h"
#include "MTVU.h"

//alignas(16) VURegs vuRegs[2];

void vuMemAllocate()
{
	u8* curpos = SysMemory::GetVUMem();
	VU0.Micro	= curpos; curpos += VU0_PROGSIZE;
	VU0.Mem		= curpos; curpos += VU0_MEMSIZE;
	VU1.Micro	= curpos; curpos += VU1_PROGSIZE;
	VU1.Mem		= curpos; curpos += VU1_MEMSIZE;
}

void vuMemRelease()
{
	VU0.Micro = VU0.Mem = nullptr;
	VU1.Micro = VU1.Mem = nullptr;
}

void vuMemReset()
{
	pxAssert( VU0.Mem );
	pxAssert( VU1.Mem );

	// Below memMap is already called by "void eeMemoryReserve::Reset()"
	//memMapVUmicro();

	// === VU0 Initialization ===
	std::memset(&VU0.ACC, 0, sizeof(VU0.ACC));
	std::memset(VU0.VF, 0, sizeof(VU0.VF));
	std::memset(VU0.VI, 0, sizeof(VU0.VI));
    VU0.VF[0].f.x = 0.0f;
	VU0.VF[0].f.y = 0.0f;
	VU0.VF[0].f.z = 0.0f;
	VU0.VF[0].f.w = 1.0f;
	VU0.VI[0].UL = 0;

	// === VU1 Initialization ===
	std::memset(&VU1.ACC, 0, sizeof(VU1.ACC));
	std::memset(VU1.VF, 0, sizeof(VU1.VF));
	std::memset(VU1.VI, 0, sizeof(VU1.VI));
	VU1.VF[0].f.x = 0.0f;
	VU1.VF[0].f.y = 0.0f;
	VU1.VF[0].f.z = 0.0f;
	VU1.VF[0].f.w = 1.0f;
	VU1.VI[0].UL = 0;
}

bool SaveStateBase::vuMicroFreeze()
{
	if(IsSaving())
		vu1Thread.WaitVU();

	if (!FreezeTag("vuMicroRegs"))
		return false;

	// VU0 state information

	Freeze(VU0.ACC);
	Freeze(VU0.VF);
	Freeze(VU0.VI);
	Freeze(VU0.q);

	Freeze(VU0.cycle);
	Freeze(VU0.flags);
	Freeze(VU0.code);
	Freeze(VU0.start_pc);
	Freeze(VU0.branch);
	Freeze(VU0.branchpc);
	Freeze(VU0.delaybranchpc);
	Freeze(VU0.takedelaybranch);
	Freeze(VU0.ebit);
	Freeze(VU0.pending_q);
	Freeze(VU0.pending_p);
	Freeze(VU0.micro_macflags);
	Freeze(VU0.micro_clipflags);
	Freeze(VU0.micro_statusflags);
	Freeze(VU0.macflag);
	Freeze(VU0.statusflag);
	Freeze(VU0.clipflag);
	Freeze(VU0.nextBlockCycles);
	Freeze(VU0.VIBackupCycles);
	Freeze(VU0.VIOldValue);
	Freeze(VU0.VIRegNumber);
	Freeze(VU0.fmac);
	Freeze(VU0.fmacreadpos);
	Freeze(VU0.fmacwritepos);
	Freeze(VU0.fmaccount);
	Freeze(VU0.fdiv);
	Freeze(VU0.efu);
	Freeze(VU0.ialu);
	Freeze(VU0.ialureadpos);
	Freeze(VU0.ialuwritepos);
	Freeze(VU0.ialucount);

	// VU1 state information
	Freeze(VU1.ACC);
	Freeze(VU1.VF);
	Freeze(VU1.VI);
	Freeze(VU1.q);
	Freeze(VU1.p);

	Freeze(VU1.cycle);
	Freeze(VU1.flags);
	Freeze(VU1.code);
	Freeze(VU1.start_pc);
	Freeze(VU1.branch);
	Freeze(VU1.branchpc);
	Freeze(VU1.delaybranchpc);
	Freeze(VU1.takedelaybranch);
	Freeze(VU1.ebit);
	Freeze(VU1.pending_q);
	Freeze(VU1.pending_p);
	Freeze(VU1.micro_macflags);
	Freeze(VU1.micro_clipflags);
	Freeze(VU1.micro_statusflags);
	Freeze(VU1.macflag);
	Freeze(VU1.statusflag);
	Freeze(VU1.clipflag);
	Freeze(VU1.nextBlockCycles);
	Freeze(VU1.xgkickaddr);
	Freeze(VU1.xgkickdiff);
	Freeze(VU1.xgkicksizeremaining);
	Freeze(VU1.xgkicklastcycle);
	Freeze(VU1.xgkickcyclecount);
	Freeze(VU1.xgkickenable);
	Freeze(VU1.xgkickendpacket);
	Freeze(VU1.VIBackupCycles);
	Freeze(VU1.VIOldValue);
	Freeze(VU1.VIRegNumber);
	Freeze(VU1.fmac);
	Freeze(VU1.fmacreadpos);
	Freeze(VU1.fmacwritepos);
	Freeze(VU1.fmaccount);
	Freeze(VU1.fdiv);
	Freeze(VU1.efu);
	Freeze(VU1.ialu);
	Freeze(VU1.ialureadpos);
	Freeze(VU1.ialuwritepos);
	Freeze(VU1.ialucount);

	return IsOkay();
}
