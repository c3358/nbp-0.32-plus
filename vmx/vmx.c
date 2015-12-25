﻿/* 
 * Copyright holder: Invisible Things Lab
 */

#include "vmx.h"
#include "cpuid.h"
#include "vmxtraps.h"

HVM_DEPENDENT Vmx = {
  ARCH_VMX,
  VmxIsImplemented,
  VmxInitialize,
  VmxVirtualize,
  VmxShutdown,
  VmxIsNestedEvent,
  VmxDispatchNestedEvent,
  VmxDispatchEvent,
  VmxAdjustRip,
  VmxRegisterTraps,
  VmxIsTrapVaild
};

ULONG64 g_HostStackBaseAddress;

extern ULONG g_uSubvertedCPUs;

NTSTATUS NTAPI VmxDisable (
)
{
  VmxTurnOff ();

  clear_in_cr4 (X86_CR4_VMXE);

  return STATUS_SUCCESS;
}

static BOOLEAN NTAPI VmxIsUnconditionalEvent (
  ULONG64 uVmExitNumber
)
{
  if (uVmExitNumber == EXIT_REASON_TRIPLE_FAULT
      || uVmExitNumber == EXIT_REASON_INIT
      || uVmExitNumber == EXIT_REASON_SIPI
      || uVmExitNumber == EXIT_REASON_IO_SMI
      || uVmExitNumber == EXIT_REASON_OTHER_SMI
      || uVmExitNumber == EXIT_REASON_TASK_SWITCH
      || uVmExitNumber == EXIT_REASON_CPUID
      || uVmExitNumber == EXIT_REASON_INVD || uVmExitNumber == EXIT_REASON_RSM
      || uVmExitNumber == EXIT_REASON_VMCALL
      || uVmExitNumber == EXIT_REASON_VMCLEAR
      || uVmExitNumber == EXIT_REASON_VMLAUNCH
      || uVmExitNumber == EXIT_REASON_VMPTRLD
      || uVmExitNumber == EXIT_REASON_VMPTRST
      || uVmExitNumber == EXIT_REASON_VMREAD
      || uVmExitNumber == EXIT_REASON_VMRESUME
      || uVmExitNumber == EXIT_REASON_VMWRITE
      || uVmExitNumber == EXIT_REASON_VMXOFF
      || uVmExitNumber == EXIT_REASON_VMXON
      || uVmExitNumber == EXIT_REASON_INVALID_GUEST_STATE
      || uVmExitNumber == EXIT_REASON_MSR_LOADING || uVmExitNumber == EXIT_REASON_MACHINE_CHECK)
    return TRUE;
  else
    return FALSE;

}

/********************************************************************
  检测当前的处理器是否支持Vt
********************************************************************/
BOOLEAN NTAPI VmxIsImplemented ()
{
  ULONG32 eax, ebx, ecx, edx;
  GetCpuIdInfo (0, &eax, &ebx, &ecx, &edx);
  if (eax < 1) {
    KdPrint (("VmxIsImplemented(): Extended CPUID functions not implemented\n"));
    return FALSE;
  }
  if (!(ebx == 0x756e6547 && ecx == 0x6c65746e && edx == 0x49656e69)) {
    KdPrint (("VmxIsImplemented(): Not an INTEL processor\n"));
    return FALSE;
  }
  //intel cpu use fun_0x1 to test VMX.  
  GetCpuIdInfo (0x1, &eax, &ebx, &ecx, &edx);
  return (BOOLEAN) (CmIsBitSet (ecx, 5));
}

VOID VmxHandleInterception (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  BOOLEAN WillBeAlsoHandledByGuestHv
)
{
  NTSTATUS Status;
  ULONG64 Exitcode;
  PNBP_TRAP Trap;

  if (!Cpu || !GuestRegs)
    return;

  Exitcode = VmxRead (VM_EXIT_REASON);

  // search for a registered trap for this interception
  Status = TrFindRegisteredTrap (Cpu, GuestRegs, Exitcode, &Trap);
  if (!NT_SUCCESS (Status))
  {
    KdPrint (("VmxHandleInterception(): TrFindRegisteredTrap() failed for exitcode 0x%llX\n", Exitcode));
    Hvm->ArchShutdown (Cpu, GuestRegs);
    return;
  }

  Status = TrExecuteGeneralTrapHandler (Cpu, GuestRegs, Trap, WillBeAlsoHandledByGuestHv);
  if (!NT_SUCCESS (Status))
    KdPrint (("VmxHandleInterception(): HvmExecuteGeneralTrapHandler() failed with status 0x%08hX\n", Status));
}

VOID NTAPI VmxDispatchEvent (
  PCPU Cpu,
  PGUEST_REGS GuestRegs
)
{
  VmxHandleInterception (Cpu, GuestRegs, FALSE);
                         /* this intercept will not be handled by guest hv */
}

static VOID NTAPI VmxDispatchNestedEvent (
  PCPU Cpu,
  PGUEST_REGS GuestRegs
)
{
  NTSTATUS Status;
  PNBP_TRAP Trap;
  BOOLEAN bInterceptedByGuest;
  ULONG64 Exitcode;

  if (!Cpu || !GuestRegs)
    return;

  _KdPrint (("VmxDispatchNestedEvent(): DUMMY!!! This build doesn't support nested virtualization!\n"));

}

static BOOLEAN NTAPI VmxIsNestedEvent (
  PCPU Cpu,
  PGUEST_REGS GuestRegs
)
{
  return FALSE;                 // DUMMY!!! This build doesn't support nested virtualization!!!
}

static VOID NTAPI VmxAdjustRip (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  ULONG64 Delta
)
{
  __vmx_vmwrite (GUEST_RIP, VmxRead (GUEST_RIP) + Delta);
  return;
}

static ULONG32 NTAPI VmxAdjustControls (
  ULONG32 Ctl,
  ULONG32 Msr
)
{
  LARGE_INTEGER MsrValue;

  MsrValue.QuadPart = __readmsr (Msr);
  Ctl &= MsrValue.HighPart;     /* bit == 0 in high word ==> must be zero */
  Ctl |= MsrValue.LowPart;      /* bit == 1 in low word  ==> must be one  */
  return Ctl;
}

NTSTATUS NTAPI VmxFillGuestSelectorData (
  PVOID GdtBase,
  ULONG Segreg,
  USHORT Selector
)
{
  SEGMENT_SELECTOR SegmentSelector = { 0 };
  ULONG uAccessRights;

  CmInitializeSegmentSelector (&SegmentSelector, Selector, GdtBase);
  uAccessRights = ((PUCHAR) & SegmentSelector.attributes)[0] + (((PUCHAR) & SegmentSelector.attributes)[1] << 12);

  if (!Selector)
    uAccessRights |= 0x10000;

  __vmx_vmwrite (GUEST_ES_SELECTOR + Segreg * 2, Selector);
  __vmx_vmwrite (GUEST_ES_LIMIT    + Segreg * 2, SegmentSelector.limit);
  __vmx_vmwrite (GUEST_ES_AR_BYTES + Segreg * 2, uAccessRights);

  if ((Segreg == LDTR) || (Segreg == TR))
    // don't setup for FS/GS - their bases are stored in MSR values
    __vmx_vmwrite (GUEST_ES_BASE + Segreg * 2, SegmentSelector.base);

  return STATUS_SUCCESS;
}

static NTSTATUS VmxSetupVMCS (
  PCPU Cpu,
  PVOID GuestRip,
  PVOID GuestRsp
)
{
  SEGMENT_SELECTOR SegmentSelector;
  PHYSICAL_ADDRESS VmcsToContinuePA = Cpu->Vmx.VmcsToContinuePA;
  PVOID GdtBase;
  ULONG32 Interceptions = 0;

  if (!Cpu->Vmx.OriginalVmcs)
    return STATUS_INVALID_PARAMETER;

  __vmx_vmclear (&VmcsToContinuePA);
  __vmx_vmptrld (&VmcsToContinuePA);

  /*16BIT Host-Statel Fields. */
#ifdef _X86_
  __vmx_vmwrite (HOST_CS_SELECTOR, RegGetCs () & 0xf8);
  __vmx_vmwrite (HOST_DS_SELECTOR, RegGetDs () & 0xf8);
  __vmx_vmwrite (HOST_ES_SELECTOR, RegGetEs () & 0xf8);
  __vmx_vmwrite (HOST_SS_SELECTOR, RegGetSs () & 0xf8);
#else
  __vmx_vmwrite (HOST_CS_SELECTOR, BP_GDT64_CODE);
  __vmx_vmwrite (HOST_DS_SELECTOR, BP_GDT64_DATA);
  __vmx_vmwrite (HOST_ES_SELECTOR, BP_GDT64_DATA);
  __vmx_vmwrite (HOST_SS_SELECTOR, BP_GDT64_DATA);
#endif
  __vmx_vmwrite (HOST_FS_SELECTOR, RegGetFs () & 0xf8);
  __vmx_vmwrite (HOST_GS_SELECTOR, RegGetGs () & 0xf8);
  __vmx_vmwrite (HOST_TR_SELECTOR, GetTrSelector () & 0xf8);

  /*64BIT Control Fields. */

//   __vmx_vmwrite (IO_BITMAP_A,      Cpu->Vmx.IOBitmapAPA.LowPart);
// #ifdef VMX_ENABLE_PS2_KBD_SNIFFER
//   *(((unsigned char *) (Cpu->Vmx.IOBitmapA)) + (0x60 / 8)) = 0x11;      //0x60 0x64 PS keyboard mouse
// #endif
//   __vmx_vmwrite (IO_BITMAP_A_HIGH, Cpu->Vmx.IOBitmapBPA.HighPart);
//   KdPrint (("IOBitmapA %x\n", Cpu->Vmx.IOBitmapAPA.QuadPart));
//   //
//   __vmx_vmwrite (IO_BITMAP_B,      Cpu->Vmx.IOBitmapBPA.LowPart);
//   //*(((unsigned char*)(Cpu->Vmx.IOBitmapB))+((0xc880-0x8000)/8))=0xff;  //0xc880-0xc887  // FIXME???
//   __vmx_vmwrite (IO_BITMAP_B_HIGH, Cpu->Vmx.IOBitmapBPA.HighPart);
//   KdPrint (("IOBitmapB %x\n", Cpu->Vmx.IOBitmapBPA.QuadPart));
// 
//   __vmx_vmwrite (MSR_BITMAP,      Cpu->Vmx.MSRBitmapPA.LowPart);
//   __vmx_vmwrite (MSR_BITMAP_HIGH, Cpu->Vmx.MSRBitmapPA.HighPart);
//   KdPrint (("MSRBitmap %x\n", Cpu->Vmx.MSRBitmapPA.QuadPart));

  //VM_EXIT_MSR_STORE_ADDR          = 0x00002006,  //no init
  //VM_EXIT_MSR_STORE_ADDR_HIGH     = 0x00002007,  //no init
  //VM_EXIT_MSR_LOAD_ADDR           = 0x00002008,  //no init
  //VM_EXIT_MSR_LOAD_ADDR_HIGH      = 0x00002009,  //no init
  //VM_ENTRY_MSR_LOAD_ADDR          = 0x0000200a,  //no init
  //VM_ENTRY_MSR_LOAD_ADDR_HIGH     = 0x0000200b,  //no init
//   __vmx_vmwrite (TSC_OFFSET,      0);
//   __vmx_vmwrite (TSC_OFFSET_HIGH, 0);
  //VIRTUAL_APIC_PAGE_ADDR          = 0x00002012,   //no init
  //VIRTUAL_APIC_PAGE_ADDR_HIGH     = 0x00002013,   //no init

  /*64BIT Guest-Statel Fields. */
  __vmx_vmwrite (VMCS_LINK_POINTER,      0xffffffff);
  __vmx_vmwrite (VMCS_LINK_POINTER_HIGH, 0xffffffff);

//   __vmx_vmwrite (GUEST_IA32_DEBUGCTL,      __readmsr (MSR_IA32_DEBUGCTL) & 0xffffffff);
//   __vmx_vmwrite (GUEST_IA32_DEBUGCTL_HIGH, __readmsr (MSR_IA32_DEBUGCTL) >> 32);

  /*32BIT Control Fields. */  //disable Vmexit by Extern-interrupt,NMI and Virtual NMI
  __vmx_vmwrite (PIN_BASED_VM_EXEC_CONTROL, VmxAdjustControls (0, MSR_IA32_VMX_PINBASED_CTLS));

#ifdef VMX_ENABLE_MSR_BITMAP
  Interceptions |= CPU_BASED_ACTIVATE_MSR_BITMAP;  // MSR_BITMAP
#endif

#ifdef VMX_ENABLE_PS2_KBD_SNIFFER
  Interceptions |= CPU_BASED_ACTIVATE_IO_BITMAP;   // IO_BITMAP_A IO_BITMAP_B
#endif

#ifdef INTERCEPT_RDTSCs
  Interceptions |= CPU_BASED_RDTSC_EXITING;
#endif
  //KdPrint(("Proc_Base_exec_control : %08x\n", Interceptions));
  __vmx_vmwrite (CPU_BASED_VM_EXEC_CONTROL, VmxAdjustControls (Interceptions, MSR_IA32_VMX_PROCBASED_CTLS));

#ifdef INTERCEPT_RDTSCs
  __vmx_vmwrite (EXCEPTION_BITMAP, 1 << 1);  // intercept #DB
#endif

#ifdef _X86_
  __vmx_vmwrite (VM_EXIT_CONTROLS,  VmxAdjustControls (VM_EXIT_ACK_INTR_ON_EXIT, MSR_IA32_VMX_EXIT_CTLS));
  __vmx_vmwrite (VM_ENTRY_CONTROLS, VmxAdjustControls (0, MSR_IA32_VMX_ENTRY_CTLS));
#else
  __vmx_vmwrite (VM_EXIT_CONTROLS,
            VmxAdjustControls (VM_EXIT_IA32E_MODE | VM_EXIT_ACK_INTR_ON_EXIT, MSR_IA32_VMX_EXIT_CTLS));
  __vmx_vmwrite (VM_ENTRY_CONTROLS, VmxAdjustControls (VM_ENTRY_IA32E_MODE, MSR_IA32_VMX_ENTRY_CTLS));
#endif

  // 处理#PF异常时用
//   __vmx_vmwrite (PAGE_FAULT_ERROR_CODE_MASK,  0);
//   __vmx_vmwrite (PAGE_FAULT_ERROR_CODE_MATCH, 0);

  // primary processor_based Control : CR3-load exiting
  // __vmx_vmwrite (CR3_TARGET_COUNT,        0);

  __vmx_vmwrite (VM_EXIT_MSR_STORE_COUNT, 0);
  __vmx_vmwrite (VM_EXIT_MSR_LOAD_COUNT,  0);
  __vmx_vmwrite (VM_ENTRY_MSR_LOAD_COUNT, 0);
  __vmx_vmwrite (VM_ENTRY_INTR_INFO_FIELD,0);

  //==========================================================================

  /*32BIT Read-only Fields:need no setup */
  //VM_ENTRY_EXCEPTION_ERROR_CODE   = 0x00004018,  //no init
  //VM_ENTRY_INSTRUCTION_LEN        = 0x0000401a,  //no init
  //TPR_THRESHOLD                   = 0x0000401c,  //no init

  /*32BIT Guest-Statel Fields. */

  __vmx_vmwrite (GUEST_GDTR_LIMIT, GetGdtLimit ());
  __vmx_vmwrite (GUEST_IDTR_LIMIT, GetIdtLimit ());

  __vmx_vmwrite (GUEST_INTERRUPTIBILITY_STATE, 0);   // 指示当前有STI阻塞状态
  __vmx_vmwrite (GUEST_ACTIVITY_STATE,         0);   // 处于正常执行指令状态         
  //GUEST_SM_BASE          = 0x98000,   //no init

  __vmx_vmwrite (GUEST_SYSENTER_CS,     __readmsr (MSR_IA32_SYSENTER_CS));

  /*32BIT Host-Statel Fields. */

  __vmx_vmwrite (HOST_IA32_SYSENTER_CS, __readmsr (MSR_IA32_SYSENTER_CS));     //no use

  /* NATURAL Control State Fields:need not setup. */
  //__vmx_vmwrite (CR0_GUEST_HOST_MASK, X86_CR0_PG);
  __vmx_vmwrite (CR4_GUEST_HOST_MASK, X86_CR4_VMXE); //disable vmexit 0f mov to cr4 expect for X86_CR4_VMXE
  //
  //__vmx_vmwrite (CR0_READ_SHADOW, X86_CR0_PG);
  __vmx_vmwrite (CR4_READ_SHADOW, 0);

  // primary processor_based Control : CR3-load exiting
//   __vmx_vmwrite (CR3_TARGET_VALUE0, 0);      //no use
//   __vmx_vmwrite (CR3_TARGET_VALUE1, 0);      //no use                        
//   __vmx_vmwrite (CR3_TARGET_VALUE2, 0);      //no use
//   __vmx_vmwrite (CR3_TARGET_VALUE3, 0);      //no use

  /* NATURAL Read-only State Fields:need not setup. */

  /* NATURAL GUEST State Fields. */

  __vmx_vmwrite (GUEST_CR0, __readcr0 ());
  __vmx_vmwrite (GUEST_CR3, __readcr3 ());
  __vmx_vmwrite (GUEST_CR4, __readcr4 ());

  GdtBase = (PVOID) GetGdtBase ();

  //
  // Setup guest selectors
  //
  VmxFillGuestSelectorData (GdtBase, ES, RegGetEs ());
  VmxFillGuestSelectorData (GdtBase, CS, RegGetCs ());
  VmxFillGuestSelectorData (GdtBase, SS, RegGetSs ());
  VmxFillGuestSelectorData (GdtBase, DS, RegGetDs ());
  VmxFillGuestSelectorData (GdtBase, FS, RegGetFs ());
  VmxFillGuestSelectorData (GdtBase, GS, RegGetGs ());
  VmxFillGuestSelectorData (GdtBase, LDTR, GetLdtr ());
  VmxFillGuestSelectorData (GdtBase, TR, GetTrSelector ());

#ifdef _X86_
  CmInitializeSegmentSelector (&SegmentSelector, RegGetEs (), GdtBase);
  __vmx_vmwrite (GUEST_ES_BASE, SegmentSelector.base);

  CmInitializeSegmentSelector (&SegmentSelector, RegGetCs (), GdtBase);
  __vmx_vmwrite (GUEST_CS_BASE, SegmentSelector.base);

  CmInitializeSegmentSelector (&SegmentSelector, RegGetSs (), GdtBase);
  __vmx_vmwrite (GUEST_SS_BASE, SegmentSelector.base);

  CmInitializeSegmentSelector (&SegmentSelector, RegGetDs (), GdtBase);
  __vmx_vmwrite (GUEST_DS_BASE, SegmentSelector.base);

  CmInitializeSegmentSelector (&SegmentSelector, RegGetFs (), GdtBase);
  __vmx_vmwrite (GUEST_FS_BASE, SegmentSelector.base);

  CmInitializeSegmentSelector (&SegmentSelector, RegGetGs (), GdtBase);
  __vmx_vmwrite (GUEST_GS_BASE, SegmentSelector.base);
#else
  __vmx_vmwrite (GUEST_ES_BASE, 0);
  __vmx_vmwrite (GUEST_CS_BASE, 0);
  __vmx_vmwrite (GUEST_SS_BASE, 0);
  __vmx_vmwrite (GUEST_DS_BASE, 0);
  __vmx_vmwrite (GUEST_FS_BASE, __readmsr (MSR_FS_BASE));
  __vmx_vmwrite (GUEST_GS_BASE, __readmsr (MSR_GS_BASE));
#endif

  // LDTR/TR bases have been set in VmxFillGuestSelectorData()
  __vmx_vmwrite (GUEST_GDTR_BASE, (ULONG64) GdtBase);
  __vmx_vmwrite (GUEST_IDTR_BASE, GetIdtBase ());

  __vmx_vmwrite (GUEST_DR7, 0x400);
  __vmx_vmwrite (GUEST_RSP, (ULONG64) GuestRsp);     //setup guest sp
  __vmx_vmwrite (GUEST_RIP, (ULONG64) GuestRip);     //setup guest ip:CmSlipIntoMatrix
  __vmx_vmwrite (GUEST_RFLAGS, RegGetRflags ());
  //VmxWrite(GUEST_PENDING_DBG_EXCEPTIONS, 0);//no init
  __vmx_vmwrite (GUEST_SYSENTER_ESP, __readmsr (MSR_IA32_SYSENTER_ESP));
  __vmx_vmwrite (GUEST_SYSENTER_EIP, __readmsr (MSR_IA32_SYSENTER_EIP));

  /* HOST State Fields. */
  __vmx_vmwrite (HOST_CR0, __readcr0 ());
  __vmx_vmwrite (HOST_CR3, __readcr3 ());
  __vmx_vmwrite (HOST_CR4, __readcr4 ());

  __vmx_vmwrite (HOST_FS_BASE, __readmsr (MSR_FS_BASE));
  __vmx_vmwrite (HOST_GS_BASE, __readmsr (MSR_GS_BASE));

  CmInitializeSegmentSelector (&SegmentSelector, GetTrSelector (), GdtBase);
  __vmx_vmwrite (HOST_TR_BASE, SegmentSelector.base);

  __vmx_vmwrite (HOST_GDTR_BASE, (ULONG64) Cpu->GdtArea);
  __vmx_vmwrite (HOST_IDTR_BASE, (ULONG64) Cpu->IdtArea);

  __vmx_vmwrite (HOST_IA32_SYSENTER_ESP, __readmsr (MSR_IA32_SYSENTER_ESP));
  __vmx_vmwrite (HOST_IA32_SYSENTER_EIP, __readmsr (MSR_IA32_SYSENTER_EIP));

  // HOST_RSP与HOST_RIP决定进入VMM的地址
#ifdef _X86_
  __vmx_vmwrite (HOST_RSP, g_HostStackBaseAddress + 0x0C00); //setup host sp at vmxLaunch(...)
#else
  __vmx_vmwrite (HOST_RSP, (ULONG64) Cpu);   //setup host sp at vmxLaunch(...)
#endif

  __vmx_vmwrite (HOST_RIP, (ULONG64) VmxVmexitHandler);

  _KdPrint (("VmxSetupVMCS(): Exit\n"));

  return STATUS_SUCCESS;
}

NTSTATUS NTAPI VmxInitialize (
  PCPU Cpu,
  PVOID GuestRip,
  PVOID GuestRsp
)
{
    NTSTATUS Status;
  PHYSICAL_ADDRESS AlignedVmcsPA;
  ULONG64 VaDelta;
  PHYSICAL_ADDRESS t;

  // 检查IA32_FEATURE_CONTROL寄存器的Lock位
  if (!(__readmsr(MSR_IA32_FEATURE_CONTROL) & FEATURE_CONTROL_LOCKED))
  {
      KdPrint(("VmxInitialize() IA32_FEATURE_CONTROL bit[0] = 0!\n"));
      return STATUS_UNSUCCESSFUL;
  }

  // 检查IA32_FEATURE_CONTROL寄存器的Enable VMX outside SMX位
  if (!(__readmsr(MSR_IA32_FEATURE_CONTROL) & FEATURE_CONTROL_VMXON_ENABLED))
  {
      KdPrint(("VmxInitialize() IA32_FEATURE_CONTROL bit[2] = 0!\n"));
      return STATUS_UNSUCCESSFUL;
  }

#ifdef _X86_
  g_HostStackBaseAddress = (ULONG64)MmAllocateContiguousPages (1, NULL);
#endif

  //
  // 为VMXON结构分配空间 (Allocate VMXON region)
  //
  Cpu->Vmx.OriginaVmxonR = MmAllocateContiguousPages (VMX_VMXONR_SIZE_IN_PAGES, &Cpu->Vmx.OriginalVmxonRPA);
  if (!Cpu->Vmx.OriginaVmxonR)
  {
    _KdPrint (("VmxInitialize(): Failed to allocate memory for original VMCS\n"));
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  _KdPrint (("VmxInitialize(): OriginaVmxonR VA: 0x%p\n",   Cpu->Vmx.OriginaVmxonR));
  _KdPrint (("VmxInitialize(): OriginaVmxonR PA: 0x%llx\n", Cpu->Vmx.OriginalVmxonRPA.QuadPart));

  //
  // 为VMCS结构分配空间 (Allocate VMCS)
  //
  Cpu->Vmx.OriginalVmcs = MmAllocateContiguousPages (VMX_VMCS_SIZE_IN_PAGES, &Cpu->Vmx.OriginalVmcsPA);
  if (!Cpu->Vmx.OriginalVmcs)
  {
    _KdPrint (("VmxInitialize(): Failed to allocate memory for original VMCS\n"));
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  _KdPrint (("VmxInitialize(): Vmcs VA: 0x%p\n",   Cpu->Vmx.OriginalVmcs));
  _KdPrint (("VmxInitialize(): Vmcs PA: 0x%llx\n", Cpu->Vmx.OriginalVmcsPA.QuadPart));

  //
  // TODISCOVER:
  // these two PAs are equal if there're no nested VMs
  //
  Cpu->Vmx.VmcsToContinuePA = Cpu->Vmx.OriginalVmcsPA;

  // init IOBitmap and MsrBitmap

  //
  // 为IO位图A分配空间, IOA控制0000H-7FFFH
  //
  Cpu->Vmx.IOBitmapA = MmAllocateContiguousPages (VMX_IOBitmap_SIZE_IN_PAGES, &Cpu->Vmx.IOBitmapAPA);
  if (!Cpu->Vmx.IOBitmapA)
  {
    _KdPrint (("VmxInitialize(): Failed to allocate memory for IOBitmapA\n"));
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  RtlZeroMemory (Cpu->Vmx.IOBitmapA, PAGE_SIZE);   // IO位图A初始化为0

  _KdPrint (("VmxInitialize(): IOBitmapA VA: 0x%p\n", Cpu->Vmx.IOBitmapA));
  _KdPrint (("VmxInitialize(): IOBitmapA PA: 0x%llx\n", Cpu->Vmx.IOBitmapAPA.QuadPart));

  //
  // 为IO位图B分配空间, IOB控制8000H-FFFFH
  //
  Cpu->Vmx.IOBitmapB = MmAllocateContiguousPages (VMX_IOBitmap_SIZE_IN_PAGES, &Cpu->Vmx.IOBitmapBPA);
  if (!Cpu->Vmx.IOBitmapB) {
    _KdPrint (("VmxInitialize(): Failed to allocate memory for IOBitmapB\n"));
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  RtlZeroMemory (Cpu->Vmx.IOBitmapB, PAGE_SIZE);   // IO位图B初始化为0

  _KdPrint (("VmxInitialize(): IOBitmapB VA: 0x%p\n", Cpu->Vmx.IOBitmapB));
  _KdPrint (("VmxInitialize(): IOBitmapB PA: 0x%llx\n", Cpu->Vmx.IOBitmapBPA.QuadPart));

  //
  // 为MSR位图分配空间
  //
  Cpu->Vmx.MSRBitmap = MmAllocateContiguousPages (VMX_MSRBitmap_SIZE_IN_PAGES, &Cpu->Vmx.MSRBitmapPA);
  if (!Cpu->Vmx.MSRBitmap) {
    _KdPrint (("VmxInitialize(): Failed to allocate memory for  MSRBitmap\n"));
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  RtlZeroMemory (Cpu->Vmx.MSRBitmap, PAGE_SIZE);   // MSR位图初始化为0

  _KdPrint (("VmxInitialize(): MSRBitmap VA: 0x%p\n", Cpu->Vmx.MSRBitmap));
  _KdPrint (("VmxInitialize(): MSRBitmap PA: 0x%llx\n", Cpu->Vmx.MSRBitmapPA.QuadPart));

  set_in_cr4 (X86_CR4_VMXE);
  *(ULONG64 *) Cpu->Vmx.OriginaVmxonR = (__readmsr(MSR_IA32_VMX_BASIC) & 0xffffffff); //set up vmcs_revision_id
  *(ULONG64 *) Cpu->Vmx.OriginalVmcs  = (__readmsr(MSR_IA32_VMX_BASIC) & 0xffffffff); 
  t = MmGetPhysicalAddress (Cpu->Vmx.OriginaVmxonR);
  if (__vmx_on (&t))
  {
    _KdPrint (("VmxOn Failed!\n"));
    return STATUS_UNSUCCESSFUL;
  }

  //============================= 配置VMCS ================================

  if (!NT_SUCCESS (Status = VmxSetupVMCS (Cpu, GuestRip, GuestRsp)))
  {
    KdPrint (("Vmx(): VmxSetupVMCS() failed with status 0x%08hX\n", Status));
    VmxDisable ();
    return Status;
  }

  //
  // 读取MSR_EFE/CR0/CR3/CR4等寄存器的内容并记录到CPU结构
  //
  Cpu->Vmx.GuestEFER = __readmsr (MSR_EFER);
  KdPrint (("Guest MSR_EFER Read 0x%llx \n", Cpu->Vmx.GuestEFER));
  Cpu->Vmx.GuestCR0 = __readcr0 ();
  Cpu->Vmx.GuestCR3 = __readcr3 ();
  Cpu->Vmx.GuestCR4 = __readcr4 ();

#ifdef INTERCEPT_RDTSCs
  Cpu->Tracing = 0;
#endif

  CmCli ();
  return STATUS_SUCCESS;
}

static VOID VmxGenerateTrampolineToGuest (
  PCPU Cpu,
  PGUEST_REGS GuestRegs,
  PUCHAR Trampoline
)
{
  ULONG uTrampolineSize = 0;
  ULONG64 NewRsp;

  if (!Cpu || !GuestRegs)
    return;

  // assume Trampoline buffer is big enough
  __vmx_vmwrite (GUEST_RFLAGS, VmxRead (GUEST_RFLAGS) & ~0x100);     // disable TF

  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RCX, GuestRegs->rcx);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RDX, GuestRegs->rdx);

  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RBX, GuestRegs->rbx);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RBP, GuestRegs->rbp);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RSI, GuestRegs->rsi);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RDI, GuestRegs->rdi);

#ifndef _X86_
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_R8, GuestRegs->r8);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_R9, GuestRegs->r9);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_R10, GuestRegs->r10);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_R11, GuestRegs->r11);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_R12, GuestRegs->r12);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_R13, GuestRegs->r13);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_R14, GuestRegs->r14);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_R15, GuestRegs->r15);
#endif

  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_CR0, VmxRead (GUEST_CR0));
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_CR3, VmxRead (GUEST_CR3));
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_CR4, VmxRead (GUEST_CR4));

  NewRsp = VmxRead (GUEST_RSP);

  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RSP, NewRsp);

  // construct stack frame for IRETQ:
  // [TOS]        rip
  // [TOS+0x08]   cs
  // [TOS+0x10]   rflags
  // [TOS+0x18]   rsp
  // [TOS+0x20]   ss

  // construct stack frame for IRETD:
  // [TOS]        rip
  // [TOS+0x4]    cs
  // [TOS+0x8]    rflags

#ifndef _X86_
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RAX, VmxRead (GUEST_SS_SELECTOR));
  CmGeneratePushReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RAX);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RAX, NewRsp);
  CmGeneratePushReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RAX);
#endif
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RAX, VmxRead (GUEST_RFLAGS));
  CmGeneratePushReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RAX);
  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RAX, VmxRead (GUEST_CS_SELECTOR));
  CmGeneratePushReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RAX);

  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RAX,
                    VmxRead (GUEST_RIP) + VmxRead (VM_EXIT_INSTRUCTION_LEN));

  CmGeneratePushReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RAX);

  CmGenerateMovReg (&Trampoline[uTrampolineSize], &uTrampolineSize, REG_RAX, GuestRegs->rax);

#ifdef _X86_
  CmGenerateIretd (&Trampoline[uTrampolineSize], &uTrampolineSize);
#else
  CmGenerateIretq (&Trampoline[uTrampolineSize], &uTrampolineSize);
#endif

  // restore old GDTR
  CmReloadGdtr ((PVOID) VmxRead (GUEST_GDTR_BASE), (ULONG) VmxRead (GUEST_GDTR_LIMIT));

  MsrWrite (MSR_GS_BASE, VmxRead (GUEST_GS_BASE));
  MsrWrite (MSR_FS_BASE, VmxRead (GUEST_FS_BASE));

  // FIXME???
  // restore ds, es
//      CmSetDS((USHORT)VmxRead(GUEST_DS_SELECTOR));
//      CmSetES((USHORT)VmxRead(GUEST_ES_SELECTOR));

  // cs and ss must be the same with the guest OS in this implementation

  // restore old IDTR
  CmReloadIdtr ((PVOID) VmxRead (GUEST_IDTR_BASE), (ULONG) VmxRead (GUEST_IDTR_LIMIT));

  return;
}

static NTSTATUS NTAPI VmxShutdown (
  PCPU Cpu,
  PGUEST_REGS GuestRegs
)
{
  UCHAR Trampoline[0x600];

  _KdPrint (("VmxShutdown(): CPU#%d\n", Cpu->ProcessorNumber));

#if DEBUG_LEVEL>2
  VmxDumpVmcs ();
#endif
  InterlockedDecrement (&g_uSubvertedCPUs);

  // The code should be updated to build an approproate trampoline to exit to any guest mode.
  VmxGenerateTrampolineToGuest (Cpu, GuestRegs, Trampoline);

  _KdPrint (("VmxShutdown(): Trampoline generated\n", Cpu->ProcessorNumber));
  VmxDisable ();
  ((VOID (*)()) & Trampoline) ();

  // never returns
  return STATUS_SUCCESS;
}

static NTSTATUS NTAPI VmxVirtualize (
  PCPU Cpu
)
{
  ULONG64 rsp;
  if (!Cpu)
    return STATUS_INVALID_PARAMETER;

  _KdPrint (("VmxVirtualize(): VmxRead: 0x%X \n", VmxRead (VM_INSTRUCTION_ERROR)));
  _KdPrint (("VmxVirtualize(): RFlags before vmxLaunch: 0x%x \n", RegGetRflags ()));
  _KdPrint (("VmxVirtualize(): PCPU: 0x%p \n", Cpu));
  rsp = RegGetRsp ();
  _KdPrint (("VmxVirtualize(): Rsp: 0x%x \n", rsp));

#ifdef _X86_
  *((PULONG64) (g_HostStackBaseAddress + 0x0C00)) = (ULONG64) Cpu;
#endif

  __vmx_vmlaunch();
  //
  // VMLAUNCH后操作系统变为Guest，不会返回

  return STATUS_UNSUCCESSFUL;
}

static BOOLEAN NTAPI VmxIsTrapVaild (
  ULONG TrappedVmExit
)
{
  if (TrappedVmExit > VMX_MAX_GUEST_VMEXIT)
    return FALSE;
  return TRUE;
}
