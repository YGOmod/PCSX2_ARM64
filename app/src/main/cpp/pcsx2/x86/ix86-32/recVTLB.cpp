// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "vtlb.h"
#include "x86/iCore.h"
#include "x86/iR5900.h"

#include "common/Perf.h"

using namespace vtlb_private;
#if !defined(__ANDROID__)
using namespace x86Emitter;
#endif

// we need enough for a 32-bit jump forwards (5 bytes)
//static constexpr u32 LOADSTORE_PADDING = 5;

//#define LOG_STORES

static u32 GetAllocatedGPRBitmask()
{
	u32 i, mask = 0;
	for (i = 0; i < iREGCNT_GPR; ++i)
	{
		if (x86regs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

static u32 GetAllocatedXMMBitmask()
{
	u32 i, mask = 0;
	for (i = 0; i < iREGCNT_XMM; ++i)
	{
		if (xmmregs[i].inuse)
			mask |= (1u << i);
	}
	return mask;
}

/*
	// Pseudo-Code For the following Dynarec Implementations -->

	u32 vmv = vmap[addr>>VTLB_PAGE_BITS].raw();
	sptr ppf=addr+vmv;
	if (!(ppf<0))
	{
		data[0]=*reinterpret_cast<DataType*>(ppf);
		if (DataSize==128)
			data[1]=*reinterpret_cast<DataType*>(ppf+8);
		return 0;
	}
	else
	{
		//has to: translate, find function, call function
		u32 hand=(u8)vmv;
		u32 paddr=(ppf-hand) << 1;
		//Console.WriteLn("Translated 0x%08X to 0x%08X",params addr,paddr);
		return reinterpret_cast<TemplateHelper<DataSize,false>::HandlerType*>(RWFT[TemplateHelper<DataSize,false>::sidx][0][hand])(paddr,data);
	}

	// And in ASM it looks something like this -->

	mov eax,ecx;
	shr eax,VTLB_PAGE_BITS;
	mov rax,[rax*wordsize+vmap];
	add rcx,rax;
	js _fullread;

	//these are wrong order, just an example ...
	mov [rax],ecx;
	mov ecx,[rdx];
	mov [rax+4],ecx;
	mov ecx,[rdx+4];
	mov [rax+4+4],ecx;
	mov ecx,[rdx+4+4];
	mov [rax+4+4+4+4],ecx;
	mov ecx,[rdx+4+4+4+4];
	///....

	jmp cont;
	_fullread:
	movzx eax,al;
	sub   ecx,eax;
	call [eax+stuff];
	cont:
	........

*/

#ifdef LOG_STORES
static std::FILE* logfile;
static bool CheckLogFile()
{
	if (!logfile)
		logfile = std::fopen("C:\\Dumps\\comp\\memlog.bad.txt", "wb");
	return (logfile != nullptr);
}

static void LogWrite(u32 addr, u64 val)
{
	if (!CheckLogFile())
		return;

	std::fprintf(logfile, "%08X @ %u: %llx\n", addr, cpuRegs.cycle, val);
	std::fflush(logfile);
}

static void __vectorcall LogWriteQuad(u32 addr, __m128i val)
{
	if (!CheckLogFile())
		return;

	std::fprintf(logfile, "%08X @ %u: %llx %llx\n", addr, cpuRegs.cycle, val.m128i_u64[0], val.m128i_u64[1]);
	std::fflush(logfile);
}
#endif

namespace vtlb_private
{
	// ------------------------------------------------------------------------
	// Prepares eax, ecx, and, ebx for Direct or Indirect operations.
	// Returns the writeback pointer for ebx (return address from indirect handling)
	//
	static void DynGen_PrepRegs(int addr_reg, int value_reg, u32 sz, bool xmm)
	{
		EE::Profiler.EmitMem();

//		_freeX86reg(arg1regd);
        _freeX86reg(ECX.GetCode());
//		xMOV(arg1regd, xRegister32(addr_reg));
        armAsm->Mov(ECX, a64::WRegister(addr_reg));

		if (value_reg >= 0)
		{
			if (sz == 128)
			{
				pxAssert(xmm);
//				_freeXMMreg(xRegisterSSE::GetArgRegister(1, 0).GetId());
                _freeXMMreg(armQRegister(1).GetCode());
//				xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg));
                armAsm->Mov(armQRegister(1), armQRegister(value_reg));
			}
			else if (xmm)
			{
				// 32bit xmms are passed in GPRs
				pxAssert(sz == 32);
//				_freeX86reg(arg2regd);
                _freeX86reg(EDX.GetCode());
//				xMOVD(arg2regd, xRegisterSSE(value_reg));
                armAsm->Fmov(EDX, a64::QRegister(value_reg).S());
			}
			else
			{
//				_freeX86reg(arg2regd);
                _freeX86reg(EDX.GetCode());
//				xMOV(arg2reg, xRegister64(value_reg));
                armAsm->Mov(RDX, a64::XRegister(value_reg));
			}
		}

//		xMOV(eax, arg1regd);
        armAsm->Mov(EAX, ECX);
//		xSHR(eax, VTLB_PAGE_BITS);
        armAsm->Lsr(EAX, EAX, VTLB_PAGE_BITS);
//		xMOV(rax, ptrNative[xComplexAddress(arg3reg, vtlbdata.vmap, rax * wordsize)]);
        armMoveAddressToReg(RXVIXLSCRATCH, vtlbdata.vmap);
        armAsm->Ldr(RAX, a64::MemOperand(RXVIXLSCRATCH, RAX, a64::LSL, 3));
//		xADD(arg1reg, rax);
        armAsm->Adds(RCX, RCX, RAX);
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectRead(u32 bits, bool sign)
	{
		pxAssert(bits == 8 || bits == 16 || bits == 32 || bits == 64 || bits == 128);

		switch (bits)
		{
            case 8:
                if (sign) {
//                    xMOVSX(rax, ptr8[arg1reg]);
                    armAsm->Ldrsb(RAX, a64::MemOperand(RCX));
                }
                else {
//                    xMOVZX(rax, ptr8[arg1reg]);
                    armAsm->Ldrb(RAX, a64::MemOperand(RCX));
                }
                break;

            case 16:
                if (sign) {
//                    xMOVSX(rax, ptr16[arg1reg]);
                    armAsm->Ldrsh(RAX, a64::MemOperand(RCX));
                }
                else {
//                    xMOVZX(rax, ptr16[arg1reg]);
                    armAsm->Ldrh(RAX, a64::MemOperand(RCX));
                }
                break;

            case 32:
                if (sign) {
//                    xMOVSX(rax, ptr32[arg1reg]);
                    armAsm->Ldrsw(RAX, a64::MemOperand(RCX));
                }
                else {
//					xMOV(eax, ptr32[arg1reg]);
                    armAsm->Ldr(EAX, a64::MemOperand(RCX));
                }
                break;

            case 64:
//				xMOV(rax, ptr64[arg1reg]);
                armAsm->Ldr(RAX, a64::MemOperand(RCX));
                break;

            case 128:
//				xMOVAPS(xmm0, ptr128[arg1reg]);
                armAsm->Ldr(xmm0.Q(), a64::MemOperand(RCX));
                break;

			jNO_DEFAULT
		}
	}

	// ------------------------------------------------------------------------
	static void DynGen_DirectWrite(u32 bits)
	{
		switch (bits)
		{
            case 8:
//				xMOV(ptr[arg1reg], xRegister8(arg2regd));
                armAsm->Strb(EDX, a64::MemOperand(RCX));
                break;

            case 16:
//				xMOV(ptr[arg1reg], xRegister16(arg2regd));
                armAsm->Strh(EDX, a64::MemOperand(RCX));
                break;

            case 32:
//				xMOV(ptr[arg1reg], arg2regd);
                armAsm->Str(EDX, a64::MemOperand(RCX));
                break;

            case 64:
//				xMOV(ptr[arg1reg], arg2reg);
                armAsm->Str(RDX, a64::MemOperand(RCX));
                break;

            case 128:
//				xMOVAPS(ptr[arg1reg], xRegisterSSE::GetArgRegister(1, 0));
                armAsm->Str(armQRegister(1).Q(), a64::MemOperand(RCX));
                break;
		}
	}
} // namespace vtlb_private

static constexpr u32 INDIRECT_DISPATCHER_SIZE = 96;
static constexpr u32 INDIRECT_DISPATCHERS_SIZE = 2 * 5 * 2 * INDIRECT_DISPATCHER_SIZE;
alignas(__pagesize) static u8 m_IndirectDispatchers[__pagesize];

// ------------------------------------------------------------------------
// mode        - 0 for read, 1 for write!
// operandsize - 0 thru 4 represents 8, 16, 32, 64, and 128 bits.
//
static u8* GetIndirectDispatcherPtr(int mode, int operandsize, int sign = 0)
{
	pxAssert(mode || operandsize >= 3 ? !sign : true);

	return &m_IndirectDispatchers[(mode * (8 * INDIRECT_DISPATCHER_SIZE)) + (sign * 5 * INDIRECT_DISPATCHER_SIZE) +
								  (operandsize * INDIRECT_DISPATCHER_SIZE)];
}

// ------------------------------------------------------------------------
// Generates a JS instruction that targets the appropriate templated instance of
// the vtlb Indirect Dispatcher.
//

template <typename GenDirectFn>
static void DynGen_HandlerTest(const GenDirectFn& gen_direct, int mode, int bits, bool sign = false)
{
	int szidx = 0;
	switch (bits)
	{
		case   8: szidx = 0; break;
		case  16: szidx = 1; break;
		case  32: szidx = 2; break;
		case  64: szidx = 3; break;
		case 128: szidx = 4; break;
		jNO_DEFAULT;
	}
//	xForwardJS8 to_handler;
    a64::Label to_handler;
    armAsm->B(&to_handler, a64::Condition::mi);
	gen_direct();
//	xForwardJump8 done;
    a64::Label done;
    armAsm->B(&done);
//	to_handler.SetTarget();
    armBind(&to_handler);

//	xFastCall(GetIndirectDispatcherPtr(mode, szidx, sign));
    armAsm->Mov(RXVIXLSCRATCH, reinterpret_cast<uintptr_t>(GetIndirectDispatcherPtr(mode, szidx, sign)));
    armAsm->Blr(RXVIXLSCRATCH);

//	done.SetTarget();
    armBind(&done);
}

// ------------------------------------------------------------------------
// Generates the various instances of the indirect dispatchers
// In: arg1reg: vtlb entry, arg2reg: data ptr (if mode >= 64), rbx: function return ptr
// Out: eax: result (if mode < 64)
static void DynGen_IndirectTlbDispatcher(int mode, int bits, bool sign)
{
	// fixup stack
#ifdef _WIN32
	xSUB(rsp, 32 + 8);
#else
//	xSUB(rsp, 8);
//    armAsm->Sub(a64::sp, a64::sp, 48);
#endif

//	xMOVZX(eax, al);
    armAsm->Uxtb(EEX, EAX);
	if (wordsize != 8) {
//        xSUB(arg1regd, 0x80000000);
        armAsm->Sub(ECX, ECX, 0x80000000);
    }
//	xSUB(arg1regd, eax);
    armAsm->Sub(ECX, ECX, EEX);

	// jump to the indirect handler, which is a C++ function.
	// [ecx is address, edx is data]
//	sptr table = (sptr)vtlbdata.RWFT[bits][mode];
//  xFastCall(ptrNative[(rax * wordsize) + table], arg1reg, arg2reg);

    armAsm->Mov(RXVIXLSCRATCH, reinterpret_cast<intptr_t>(vtlbdata.RWFT[bits][mode]));
    armAsm->Ldr(REX, a64::MemOperand(RXVIXLSCRATCH, REX, a64::LSL, 3));

    armAsm->Mov(RAX, RCX);
    armAsm->Mov(RCX, RDX);

    armAsm->Push(a64::xzr, a64::lr);
    armAsm->Blr(REX);
    armAsm->Pop(a64::lr, a64::xzr);

	if (!mode)
	{
		if (bits == 0)
		{
			if (sign) {
//                xMOVSX(rax, al);
                armAsm->Sxtb(RAX, RAX);
            }
			else {
//                xMOVZX(rax, al);
                armAsm->Uxtb(RAX, RAX);
            }
		}
		else if (bits == 1)
		{
			if (sign) {
//                xMOVSX(rax, ax);
                armAsm->Sxth(RAX, RAX);
            }
			else {
//                xMOVZX(rax, ax);
                armAsm->Uxth(RAX, RAX);
            }
		}
		else if (bits == 2)
		{
			if (sign) {
//                xCDQE();
                armAsm->Sxtw(RAX, RAX);
            }
		}
	}

#ifdef _WIN32
	xADD(rsp, 32 + 8);
#else
//	xADD(rsp, 8);
//    armAsm->Add(a64::sp, a64::sp, 48);
#endif

//	xRET();
    armAsm->Ret();
}

void vtlb_DynGenDispatchers()
{
    static bool hasBeenCalled = false;
    if (hasBeenCalled)
        return;
    hasBeenCalled = true;

    HostSys::MemProtect(m_IndirectDispatchers, __pagesize, PageAccess_ReadWrite());

    // clear the buffer to 0xcc (easier debugging).
    std::memset(m_IndirectDispatchers, 0xcc, __pagesize);

    int mode, bits, sign;
    for (mode = 0; mode < 2; ++mode)
    {
        for (bits = 0; bits < 5; ++bits)
        {
            for (sign = 0; sign < (!mode && bits < 3 ? 2 : 1); ++sign)
            {
                armSetAsmPtr(GetIndirectDispatcherPtr(mode, bits, !!sign), INDIRECT_DISPATCHERS_SIZE, nullptr);
                armStartBlock();
                ////
                DynGen_IndirectTlbDispatcher(mode, bits, !!sign);
                ////
                armEndBlock();
            }
        }
    }

    HostSys::MemProtect(m_IndirectDispatchers, __pagesize, PageAccess_ExecOnly());
    Perf::any.Register(m_IndirectDispatchers, __pagesize, "TLB Dispatcher");
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Load Implementations
// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
int vtlb_DynGenReadNonQuad(u32 bits, bool sign, bool xmm, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
    pxAssume(bits <= 64);

    int x86_dest_reg;
    if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
    {
        iFlushCall(FLUSH_FULLVTLB);

        DynGen_PrepRegs(addr_reg, -1, bits, xmm);
        DynGen_HandlerTest([bits, sign]() { DynGen_DirectRead(bits, sign); }, 0, bits, sign && bits < 64);

        if (!xmm)
        {
//			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), EAX.GetCode());
//			xMOV(xRegister64(x86_dest_reg), rax);
            armAsm->Mov(a64::XRegister(x86_dest_reg), RAX);
        }
        else
        {
            // we shouldn't be loading any FPRs which aren't 32bit..
            // we use MOVD here despite it being floating-point data, because we're going int->float reinterpret.
            pxAssert(bits == 32);
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
//			xMOVDZX(xRegisterSSE(x86_dest_reg), eax);
            armAsm->Fmov(a64::QRegister(x86_dest_reg).S(), EAX);
        }

        return x86_dest_reg;
    }

    const u8* codeStart;
//	const xAddressReg x86addr(addr_reg);
    a64::MemOperand baseAddr = a64::MemOperand(RFASTMEMBASE, a64::XRegister(addr_reg));

    if (!xmm)
    {
//		x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
        x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), EAX.GetCode());
//		codeStart = x86Ptr;
        codeStart = armGetCurrentCodePointer();
//		const xRegister64 x86reg(x86_dest_reg);

        switch (bits)
        {
            case 8:
//			    sign ? xMOVSX(x86reg, ptr8[RFASTMEMBASE + x86addr]) : xMOVZX(xRegister32(x86reg), ptr8[RFASTMEMBASE + x86addr]);
                sign ? armAsm->Ldrsb(a64::XRegister(x86_dest_reg), baseAddr) : armAsm->Ldrb(a64::WRegister(x86_dest_reg), baseAddr);
                break;
            case 16:
//			    sign ? xMOVSX(x86reg, ptr16[RFASTMEMBASE + x86addr]) : xMOVZX(xRegister32(x86reg), ptr16[RFASTMEMBASE + x86addr]);
                sign ? armAsm->Ldrsh(a64::XRegister(x86_dest_reg), baseAddr) : armAsm->Ldrh(a64::WRegister(x86_dest_reg), baseAddr);
                break;
            case 32:
//			    sign ? xMOVSX(x86reg, ptr32[RFASTMEMBASE + x86addr]) : xMOV(xRegister32(x86reg), ptr32[RFASTMEMBASE + x86addr]);
                sign ? armAsm->Ldrsw(a64::XRegister(x86_dest_reg), baseAddr) : armAsm->Ldr(a64::WRegister(x86_dest_reg), baseAddr);
                break;
            case 64:
//			    xMOV(x86reg, ptr64[RFASTMEMBASE + x86addr]);
                armAsm->Ldr(a64::XRegister(x86_dest_reg), baseAddr);
                break;

            jNO_DEFAULT
        }
    }
    else
    {
        pxAssert(bits == 32);
        x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
//		codeStart = x86Ptr;
        codeStart = armGetCurrentCodePointer();
//		const xRegisterSSE xmmreg(x86_dest_reg);
        const a64::QRegister xmmreg(x86_dest_reg);
//		xMOVSSZX(xmmreg, ptr32[RFASTMEMBASE + x86addr]);
        armAsm->Ldr(xmmreg.S(), baseAddr);
    }

//	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(x86Ptr - codeStart),
    vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(armGetCurrentCodePointer() - codeStart),
                          pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
                          static_cast<u8>(addr_reg), static_cast<u8>(x86_dest_reg),
                          static_cast<u8>(bits), sign, true, xmm);

    return x86_dest_reg;
}

// ------------------------------------------------------------------------
// Recompiled input registers:
//   ecx - source address to read from
//   Returns read value in eax.
//
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
//
int vtlb_DynGenReadNonQuad_Const(u32 bits, bool sign, bool xmm, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
    int x86_dest_reg;
    auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
    if (!vmv.isHandler(addr_const))
    {
        auto ppf = vmv.assumePtr(addr_const);
        if (!xmm)
        {
//			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), EAX.GetCode());
            switch (bits)
            {
                case 8:
//				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr8[(u8*)ppf]) : xMOVZX(xRegister32(x86_dest_reg), ptr8[(u8*)ppf]);
                    sign ? armAsm->Ldrsb(a64::XRegister(x86_dest_reg), armMemOperandPtr((u8*)ppf)) : armAsm->Ldrb(a64::WRegister(x86_dest_reg), armMemOperandPtr((u8*)ppf));
                    break;

                case 16:
//				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr16[(u16*)ppf]) : xMOVZX(xRegister32(x86_dest_reg), ptr16[(u16*)ppf]);
                    sign ? armAsm->Ldrsh(a64::XRegister(x86_dest_reg), armMemOperandPtr((u16*)ppf)) : armAsm->Ldrh(a64::WRegister(x86_dest_reg), armMemOperandPtr((u16*)ppf));
                    break;

                case 32:
//				sign ? xMOVSX(xRegister64(x86_dest_reg), ptr32[(u32*)ppf]) : xMOV(xRegister32(x86_dest_reg), ptr32[(u32*)ppf]);
                    sign ? armAsm->Ldrsw(a64::XRegister(x86_dest_reg), armMemOperandPtr((u32*)ppf)) : armAsm->Ldr(a64::WRegister(x86_dest_reg), armMemOperandPtr((u32*)ppf));
                    break;

                case 64:
//				xMOV(xRegister64(x86_dest_reg), ptr64[(u64*)ppf]);
                    armAsm->Ldr(a64::XRegister(x86_dest_reg), armMemOperandPtr((u64*)ppf));
                    break;
            }
        }
        else
        {
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
//			xMOVSSZX(xRegisterSSE(x86_dest_reg), ptr32[(float*)ppf]);
            armAsm->Ldr(a64::QRegister(x86_dest_reg).S(), armMemOperandPtr((float*)ppf));
        }
    }
    else
    {
        // has to: translate, find function, call function
        u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

        int szidx = 0;
        switch (bits)
        {
            case  8: szidx = 0; break;
            case 16: szidx = 1; break;
            case 32: szidx = 2; break;
            case 64: szidx = 3; break;
        }

        // Shortcut for the INTC_STAT register, which many games like to spin on heavily.
        if ((bits == 32) && !EmuConfig.Speedhacks.IntcStat && (paddr == INTC_STAT))
        {
//			x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
            x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), EAX.GetCode());
            if (!xmm)
            {
                if (sign) {
//                    xMOVSX(xRegister64(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
                    armAsm->Ldrsw(a64::XRegister(x86_dest_reg), armMemOperandPtr(&psHu32(INTC_STAT)));
                }
                else {
//                    xMOV(xRegister32(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
                    armAsm->Ldr(a64::WRegister(x86_dest_reg), armMemOperandPtr(&psHu32(INTC_STAT)));
                }
            }
            else
            {
//				xMOVDZX(xRegisterSSE(x86_dest_reg), ptr32[&psHu32(INTC_STAT)]);
                armAsm->Ldr(a64::QRegister(x86_dest_reg).S(), armMemOperandPtr(&psHu32(INTC_STAT)));
            }
        }
        else
        {
            iFlushCall(FLUSH_FULLVTLB);
//			xFastCall(vmv.assumeHandlerGetRaw(szidx, false), paddr);

            armAsm->Mov(EAX, paddr);
            armEmitCall(vmv.assumeHandlerGetRaw(szidx, false)); // read

            if (!xmm)
            {
//				x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(eax), eax.GetId());
                x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeX86reg(EAX), EAX.GetCode());
                switch (bits)
                {
                    // save REX prefix by using 32bit dest for zext
                    case 8:
//					sign ? xMOVSX(xRegister64(x86_dest_reg), al) : xMOVZX(xRegister32(x86_dest_reg), al);
                        sign ? armAsm->Sxtb(a64::XRegister(x86_dest_reg), EAX) : armAsm->Uxtb(a64::WRegister(x86_dest_reg), EAX);
                        break;

                    case 16:
//					sign ? xMOVSX(xRegister64(x86_dest_reg), ax) : xMOVZX(xRegister32(x86_dest_reg), ax);
                        sign ? armAsm->Sxth(a64::XRegister(x86_dest_reg), EAX) : armAsm->Uxth(a64::WRegister(x86_dest_reg), EAX);
                        break;

                    case 32:
//					sign ? xMOVSX(xRegister64(x86_dest_reg), eax) : xMOV(xRegister32(x86_dest_reg), eax);
                        sign ? armAsm->Sxtw(a64::XRegister(x86_dest_reg), EAX) : armAsm->Mov(a64::WRegister(x86_dest_reg), EAX);
                        break;

                    case 64:
//					xMOV(xRegister64(x86_dest_reg), rax);
                        armAsm->Mov(a64::XRegister(x86_dest_reg) , RAX);
                        break;
                }
            }
            else
            {
                x86_dest_reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
//				xMOVDZX(xRegisterSSE(x86_dest_reg), eax);
                armAsm->Fmov(a64::QRegister(x86_dest_reg).S(), EAX);
            }
        }
    }

    return x86_dest_reg;
}

int vtlb_DynGenReadQuad(u32 bits, int addr_reg, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	pxAssume(bits == 128);

	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

//		DynGen_PrepRegs(arg1regd.GetId(), -1, bits, true);
        DynGen_PrepRegs(ECX.GetCode(), -1, bits, true);
		DynGen_HandlerTest([bits]() {DynGen_DirectRead(bits, false); },  0, bits);

		const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0); // Handler returns in xmm0
		if (reg >= 0) {
//            xMOVAPS(xRegisterSSE(reg), xmm0);
            armAsm->Mov(a64::QRegister(reg), xmm0);
        }

		return reg;
	}

	const int reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0); // Handler returns in xmm0
	const u8* codeStart = armGetCurrentCodePointer();

//	xMOVAPS(xRegisterSSE(reg), ptr128[RFASTMEMBASE + arg1reg]);
    armAsm->Ldr(a64::QRegister(reg).Q(), a64::MemOperand(RFASTMEMBASE, RCX));

	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(armGetCurrentCodePointer() - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(RCX.GetCode()), static_cast<u8>(reg),
		static_cast<u8>(bits), false, true, true);

	return reg;
}


// ------------------------------------------------------------------------
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
int vtlb_DynGenReadQuad_Const(u32 bits, u32 addr_const, vtlb_ReadRegAllocCallback dest_reg_alloc)
{
	pxAssert(bits == 128);

	EE::Profiler.EmitConstMem(addr_const);

	int reg;
	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr_const))
	{
		void* ppf = reinterpret_cast<void*>(vmv.assumePtr(addr_const));
		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
		if (reg >= 0) {
//            xMOVAPS(xRegisterSSE(reg), ptr128[ppf]);
            armAsm->Ldr(a64::QRegister(reg).Q(), armMemOperandPtr(ppf));
        }
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		const int szidx = 4;
		iFlushCall(FLUSH_FULLVTLB);

//		xFastCall(vmv.assumeHandlerGetRaw(szidx, 0), paddr);
        armAsm->Mov(EAX, paddr);
        armEmitCall(vmv.assumeHandlerGetRaw(szidx, 0));

		reg = dest_reg_alloc ? dest_reg_alloc() : (_freeXMMreg(0), 0);
//		xMOVAPS(xRegisterSSE(reg), xmm0);
        armAsm->Mov(a64::QRegister(reg), xmm0);
	}

	return reg;
}

//////////////////////////////////////////////////////////////////////////////////////////
//                            Dynarec Store Implementations

void vtlb_DynGenWrite(u32 sz, bool xmm, int addr_reg, int value_reg)
{
#ifdef LOG_STORES
	{
		xSUB(rsp, 16 * 16);
		for (u32 i = 0; i < 16; i++)
			xMOVAPS(ptr[rsp + i * 16], xRegisterSSE::GetInstance(i));
		for (const auto& reg : {rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15, rbp})
			xPUSH(reg);

		xPUSH(xRegister64(addr_reg));
		xPUSH(xRegister64(value_reg));
		xPUSH(arg1reg);
		xPUSH(arg2reg);
		xMOV(arg1regd, xRegister32(addr_reg));
		if (xmm)
		{
			xSUB(rsp, 32 + 32);
			xMOVAPS(ptr[rsp + 32], xRegisterSSE::GetInstance(value_reg));
			xMOVAPS(ptr[rsp + 48], xRegisterSSE::GetArgRegister(1, 0));
			if (sz < 128)
				xPSHUF.D(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg), 0);
			else
				xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg));
			xFastCall((void*)LogWriteQuad);
			xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), ptr[rsp + 48]);
			xMOVAPS(xRegisterSSE::GetInstance(value_reg), ptr[rsp + 32]);
			xADD(rsp, 32 + 32);
		}
		else
		{
			xMOV(arg2reg, xRegister64(value_reg));
			if (sz == 8)
				xAND(arg2regd, 0xFF);
			else if (sz == 16)
				xAND(arg2regd, 0xFFFF);
			else if (sz == 32)
				xAND(arg2regd, -1);
			xSUB(rsp, 32);
			xFastCall((void*)LogWrite);
			xADD(rsp, 32);
		}
		xPOP(arg2reg);
		xPOP(arg1reg);
		xPOP(xRegister64(value_reg));
		xPOP(xRegister64(addr_reg));

		for (const auto& reg : {rbp, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rbx})
			xPOP(reg);

		for (u32 i = 0; i < 16; i++)
			xMOVAPS(xRegisterSSE::GetInstance(i), ptr[rsp + i * 16]);
		xADD(rsp, 16 * 16);
	}
#endif

	if (!CHECK_FASTMEM || vtlb_IsFaultingPC(pc))
	{
		iFlushCall(FLUSH_FULLVTLB);

		DynGen_PrepRegs(addr_reg, value_reg, sz, xmm);
		DynGen_HandlerTest([sz]() { DynGen_DirectWrite(sz); }, 1, sz);
		return;
	}

	const u8* codeStart = armGetCurrentCodePointer();

//	const xAddressReg vaddr_reg(addr_reg);
    a64::MemOperand mop = a64::MemOperand(RFASTMEMBASE, a64::XRegister(addr_reg));

    if (!xmm)
    {
        switch (sz)
        {
            case 8:
//			    xMOV(ptr8[RFASTMEMBASE + vaddr_reg], xRegister8(xRegister32(value_reg)));
                armAsm->Strb(a64::WRegister(value_reg), mop);
                break;
            case 16:
//			    xMOV(ptr16[RFASTMEMBASE + vaddr_reg], xRegister16(value_reg));
                armAsm->Strh(a64::WRegister(value_reg), mop);
                break;
            case 32:
//			    xMOV(ptr32[RFASTMEMBASE + vaddr_reg], xRegister32(value_reg));
                armAsm->Str(a64::WRegister(value_reg), mop);
                break;
            case 64:
//			    xMOV(ptr64[RFASTMEMBASE + vaddr_reg], xRegister64(value_reg));
                armAsm->Str(a64::XRegister(value_reg), mop);
                break;

            jNO_DEFAULT
        }
    }
    else
    {
        pxAssert(sz == 32 || sz == 128);
        switch (sz)
        {
            case 32:
//			    xMOVSS(ptr32[RFASTMEMBASE + vaddr_reg], xRegisterSSE(value_reg));
                armAsm->Str(a64::QRegister(value_reg).S(), mop);
                break;
            case 128:
//			    xMOVAPS(ptr128[RFASTMEMBASE + vaddr_reg], xRegisterSSE(value_reg));
                armAsm->Str(a64::QRegister(value_reg).Q(), mop);
                break;

            jNO_DEFAULT
        }
    }

	vtlb_AddLoadStoreInfo((uptr)codeStart, static_cast<u32>(armGetCurrentCodePointer() - codeStart),
		pc, GetAllocatedGPRBitmask(), GetAllocatedXMMBitmask(),
		static_cast<u8>(addr_reg), static_cast<u8>(value_reg),
		static_cast<u8>(sz), false, false, xmm);
}


// ------------------------------------------------------------------------
// Generates code for a store instruction, where the address is a known constant.
// TLB lookup is performed in const, with the assumption that the COP0/TLB will clear the
// recompiler if the TLB is changed.
void vtlb_DynGenWrite_Const(u32 bits, bool xmm, u32 addr_const, int value_reg)
{
	EE::Profiler.EmitConstMem(addr_const);

#ifdef LOG_STORES
	{
		xSUB(rsp, 16 * 16);
		for (u32 i = 0; i < 16; i++)
			xMOVAPS(ptr[rsp + i * 16], xRegisterSSE::GetInstance(i));
		for (const auto& reg : { rbx, rcx, rdx, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15, rbp })
			xPUSH(reg);

		xPUSH(xRegister64(value_reg));
		xPUSH(xRegister64(value_reg));
		xPUSH(arg1reg);
		xPUSH(arg2reg);
		xMOV(arg1reg, addr_const);
		if (xmm)
		{
			xSUB(rsp, 32 + 32);
			xMOVAPS(ptr[rsp + 32], xRegisterSSE::GetInstance(value_reg));
			xMOVAPS(ptr[rsp + 48], xRegisterSSE::GetArgRegister(1, 0));
			if (bits < 128)
				xPSHUF.D(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg), 0);
			else
				xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), xRegisterSSE::GetInstance(value_reg));
			xFastCall((void*)LogWriteQuad);
			xMOVAPS(xRegisterSSE::GetArgRegister(1, 0), ptr[rsp + 48]);
			xMOVAPS(xRegisterSSE::GetInstance(value_reg), ptr[rsp + 32]);
			xADD(rsp, 32 + 32);
		}
		else
		{
			xMOV(arg2reg, xRegister64(value_reg));
			if (bits == 8)
				xAND(arg2regd, 0xFF);
			else if (bits == 16)
				xAND(arg2regd, 0xFFFF);
			else if (bits == 32)
				xAND(arg2regd, -1);
			xSUB(rsp, 32);
			xFastCall((void*)LogWrite);
			xADD(rsp, 32);
		}
		xPOP(arg2reg);
		xPOP(arg1reg);
		xPOP(xRegister64(value_reg));
		xPOP(xRegister64(value_reg));

		for (const auto& reg : {rbp, r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rdx, rcx, rbx})
			xPOP(reg);

		for (u32 i = 0; i < 16; i++)
			xMOVAPS(xRegisterSSE::GetInstance(i), ptr[rsp + i * 16]);
		xADD(rsp, 16 * 16);
	}
#endif

	auto vmv = vtlbdata.vmap[addr_const >> VTLB_PAGE_BITS];
	if (!vmv.isHandler(addr_const))
	{
		auto ppf = vmv.assumePtr(addr_const);
        a64::MemOperand mop = armMemOperandPtr((void*)ppf);
		if (!xmm)
		{
			switch (bits)
			{
				case 8:
//					xMOV(ptr[(void*)ppf], xRegister8(xRegister32(value_reg)));
                    armAsm->Strb(a64::WRegister(value_reg), mop);
					break;

				case 16:
//					xMOV(ptr[(void*)ppf], xRegister16(value_reg));
                    armAsm->Strh(a64::WRegister(value_reg), mop);
					break;

				case 32:
//					xMOV(ptr[(void*)ppf], xRegister32(value_reg));
                    armAsm->Str(a64::WRegister(value_reg), mop);
					break;

				case 64:
//					xMOV(ptr64[(void*)ppf], xRegister64(value_reg));
                    armAsm->Str(a64::XRegister(value_reg), mop);
					break;

					jNO_DEFAULT
			}
		}
		else
		{
			switch (bits)
			{
				case 32:
//					xMOVSS(ptr[(void*)ppf], xRegisterSSE(value_reg));
                    armAsm->Str(a64::QRegister(value_reg).S(), mop);
					break;

				case 128:
//					xMOVAPS(ptr128[(void*)ppf], xRegisterSSE(value_reg));
                    armAsm->Str(a64::QRegister(value_reg).Q(), mop);
					break;

					jNO_DEFAULT
			}
		}
	}
	else
	{
		// has to: translate, find function, call function
		u32 paddr = vmv.assumeHandlerGetPAddr(addr_const);

		int szidx = 0;
		switch (bits)
		{
			case 8:
				szidx = 0;
				break;
			case 16:
				szidx = 1;
				break;
			case 32:
				szidx = 2;
				break;
			case 64:
				szidx = 3;
				break;
			case 128:
				szidx = 4;
				break;
		}

		iFlushCall(FLUSH_FULLVTLB);

//		_freeX86reg(arg1regd);
        _freeX86reg(ECX.GetCode());

//		xMOV(arg1regd, paddr);
        armAsm->Mov(ECX, paddr);

		if (bits == 128)
		{
			pxAssert(xmm);
//			const xRegisterSSE argreg(xRegisterSSE::GetArgRegister(1, 0));
            const a64::VRegister argreg(armQRegister(1));
//			_freeXMMreg(argreg.GetId());
            _freeXMMreg(argreg.GetCode());
//			xMOVAPS(argreg, xRegisterSSE(value_reg));
            armAsm->Mov(argreg, a64::QRegister(value_reg));
		}
		else if (xmm)
		{
			pxAssert(bits == 32);
//			_freeX86reg(arg2regd);
            _freeX86reg(EDX.GetCode());
//			xMOVD(arg2regd, xRegisterSSE(value_reg));
            armAsm->Fmov(EDX,  a64::QRegister(value_reg).S());
		}
		else
		{
//			_freeX86reg(arg2regd);
            _freeX86reg(EDX.GetCode());
//			xMOV(arg2reg, xRegister64(value_reg));
            armAsm->Mov(RDX,  a64::XRegister(value_reg));
		}

//		xFastCall(vmv.assumeHandlerGetRaw(szidx, true));
        armAsm->Mov(EAX, ECX);
        armAsm->Mov(RCX, RDX);
        armEmitCall(vmv.assumeHandlerGetRaw(szidx, true));
	}
}

//////////////////////////////////////////////////////////////////////////////////////////
//							Extra Implementations

//   ecx - virtual address
//   Returns physical address in eax.
//   Clobbers edx
void vtlb_DynV2P()
{
//	xMOV(eax, ecx);
    armAsm->Mov(EAX, ECX);
//	xAND(ecx, VTLB_PAGE_MASK); // vaddr & VTLB_PAGE_MASK
    armAsm->And(ECX, ECX, VTLB_PAGE_MASK);

//	xSHR(eax, VTLB_PAGE_BITS);
    armAsm->Lsr(EAX, EAX, VTLB_PAGE_BITS);
//	xMOV(eax, ptr[xComplexAddress(rdx, vtlbdata.ppmap, rax * 4)]);
    armMoveAddressToReg(RDX, vtlbdata.ppmap);
    armAsm->Ldr(EAX, a64::MemOperand(RDX, RAX, a64::LSL, 2));

//	xOR(eax, ecx);
    armAsm->Orr(EAX, EAX, ECX);
}

void vtlb_DynBackpatchLoadStore(uptr code_address, u32 code_size, u32 guest_pc, u32 guest_addr,
	u32 gpr_bitmask, u32 fpr_bitmask, u8 address_register, u8 data_register,
	u8 size_in_bits, bool is_signed, bool is_load, bool is_xmm)
{
	static constexpr u32 GPR_SIZE = 8;
	static constexpr u32 XMM_SIZE = 16;

	// on win32, we need to reserve an additional 32 bytes shadow space when calling out to C
#ifdef _WIN32
	static constexpr u32 SHADOW_SIZE = 32;
#else
	static constexpr u32 SHADOW_SIZE = 0;
#endif

	DevCon.WriteLn("Backpatching %s at %p[%u] (pc %08X vaddr %08X): Bitmask %08X %08X Addr %u Data %u Size %u Flags %02X %02X",
		is_load ? "load" : "store", (void*)code_address, code_size, guest_pc, guest_addr, gpr_bitmask, fpr_bitmask,
		address_register, data_register, size_in_bits, is_signed, is_load);

    std::bitset<iREGCNT_GPR> stack_gpr;
    std::bitset<iREGCNT_XMM> stack_xmm;

	u8* thunk = recBeginThunk();

	// save regs
	u32 i, stack_offset;
    u32 num_gprs = 0, num_fprs = 0;

	for (i = 0; i < iREGCNT_GPR; ++i)
	{
        if ((gpr_bitmask & (1u << i)) && armIsCallerSaved(i) && (!is_load || is_xmm || data_register != i)) {
            num_gprs++;
            stack_gpr.set(i);
        }
	}
	for (i = 0; i < iREGCNT_XMM; ++i)
	{
		if (fpr_bitmask & (1u << i) && armIsCallerSavedXmm(i) && (!is_load || !is_xmm || data_register != i)) {
            num_fprs++;
            stack_xmm.set(i);
        }
	}

	const u32 stack_size = (((num_gprs + 1) & ~1u) * GPR_SIZE) + (num_fprs * XMM_SIZE) + SHADOW_SIZE;
	if (stack_size > 0)
	{
//		xSUB(rsp, stack_size);
        armAsm->Sub(a64::sp, a64::sp, stack_size);

		stack_offset = SHADOW_SIZE;
        for (i = 0; i < iREGCNT_XMM; ++i)
        {
            if(stack_xmm[i])
            {
//				xMOVAPS(ptr128[rsp + stack_offset], xRegisterSSE(i));
                armAsm->Str(a64::DRegister(i), a64::MemOperand(a64::sp, stack_offset));
                stack_offset += XMM_SIZE;
            }
        }
        ////
        for (i = 0; i < iREGCNT_GPR; ++i)
        {
            if(stack_gpr[i])
            {
//				xMOV(ptr64[rsp + stack_offset], xRegister64(i));
                armAsm->Str(a64::XRegister(i), a64::MemOperand(a64::sp, stack_offset));
                stack_offset += GPR_SIZE;
            }
        }
	}

	if (is_load)
	{
		DynGen_PrepRegs(address_register, -1, size_in_bits, is_xmm);
		DynGen_HandlerTest([size_in_bits, is_signed]() {DynGen_DirectRead(size_in_bits, is_signed); },  0, size_in_bits, is_signed && size_in_bits <= 32);

		if (size_in_bits == 128)
		{
			if (data_register != xmm0.GetCode()) {
//                xMOVAPS(xRegisterSSE(data_register), xmm0);
                armAsm->Mov(a64::QRegister(data_register), xmm0);
            }
		}
		else
		{
			if (is_xmm)
			{
//				xMOVDZX(xRegisterSSE(data_register), rax);
                armAsm->Fmov(a64::QRegister(data_register).V1D(), RAX);
			}
			else
			{
				if (data_register != EAX.GetCode()) {
//                    xMOV(xRegister64(data_register), rax);
                    armAsm->Mov(a64::XRegister(data_register), RAX);
                }
			}
		}
	}
	else
	{
		if (address_register != RCX.GetCode()) {
//            xMOV(arg1regd, xRegister32(address_register));
            armAsm->Mov(ECX, a64::WRegister(address_register));
        }

		if (size_in_bits == 128)
		{
//			const xRegisterSSE argreg(xRegisterSSE::GetArgRegister(1, 0));
            const a64::QRegister argreg(armQRegister(1));
			if (data_register != argreg.GetCode()) {
//                xMOVAPS(argreg, xRegisterSSE(data_register));
                armAsm->Mov(argreg, a64::QRegister(data_register));
            }
		}
		else
		{
			if (is_xmm)
			{
//				xMOVD(arg2reg, xRegisterSSE(data_register));
                armAsm->Fmov(RDX, a64::QRegister(data_register).V1D());
			}
			else
			{
				if (data_register != RDX.GetCode()) {
//                    xMOV(arg2reg, xRegister64(data_register));
                    armAsm->Mov(RDX, a64::XRegister(data_register));
                }
			}
		}

		DynGen_PrepRegs(address_register, data_register, size_in_bits, is_xmm);
		DynGen_HandlerTest([size_in_bits]() { DynGen_DirectWrite(size_in_bits); }, 1, size_in_bits);
	}

	// restore regs
	if (stack_size > 0)
	{
		stack_offset = SHADOW_SIZE;
		for (i = 0; i < iREGCNT_XMM; ++i)
		{
            if(stack_xmm[i])
			{
//				xMOVAPS(xRegisterSSE(i), ptr128[rsp + stack_offset]);
                armAsm->Ldr(a64::DRegister(i), a64::MemOperand(a64::sp, stack_offset));
				stack_offset += XMM_SIZE;
			}
		}
        ////
		for (i = 0; i < iREGCNT_GPR; ++i)
		{
            if(stack_gpr[i])
			{
//				xMOV(xRegister64(i), ptr64[rsp + stack_offset]);
                armAsm->Ldr(a64::XRegister(i), a64::MemOperand(a64::sp, stack_offset));
				stack_offset += GPR_SIZE;
			}
		}

//		xADD(rsp, stack_size);
        armAsm->Add(a64::sp, a64::sp, stack_size);
	}

//	xJMP((void*)(code_address + code_size));
    armEmitJmp(reinterpret_cast<const void*>(code_address + code_size), true);

	recEndThunk();

	// backpatch to a jump to the slowmem handler
//	x86Ptr = (u8*)code_address;
//	xJMP(thunk);
    armEmitJmpPtr((void*)code_address, thunk, true);
}
