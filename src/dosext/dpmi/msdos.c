/*
 * (C) Copyright 1992, ..., 2004 the "DOSEMU-Development-Team".
 *
 * for details see file COPYING in the DOSEMU distribution
 */

/* 	MS-DOS API translator for DOSEMU\'s DPMI Server
 *
 * DANG_BEGIN_MODULE msdos.c
 *
 * REMARK
 * MS-DOS API translator allows DPMI programs to call DOS service directly
 * in protected mode.
 *
 * /REMARK
 * DANG_END_MODULE
 *
 * First Attempted by Dong Liu,  dliu@rice.njit.edu
 *
 */

#include <stdlib.h>
#include <string.h>

#include "emu.h"
#include "emu-ldt.h"
#include "int.h"
#include "bios.h"
#include "emm.h"
#include "dpmi.h"
#include "msdos.h"

enum { ES_INDEX = 0, CS_INDEX = 1, SS_INDEX = 2,  DS_INDEX = 3,
       FS_INDEX = 4, GS_INDEX = 5 };

#ifdef __linux__
static struct sigcontext SAVED_REGS;
static struct sigcontext MOUSE_SAVED_REGS;
static struct sigcontext VIDEO_SAVED_REGS;
static struct sigcontext INT15_SAVED_REGS;
static struct sigcontext INT2f_SAVED_REGS;
#define S_REG(reg) (SAVED_REGS.reg)
#endif

#define TRANS_BUFFER_SEG EMM_SEGMENT

/* these are used like:  S_LO(ax) = 2 (sets al to 2) */
#define S_LO(reg)  (*(unsigned char *)&S_REG(e##reg))
#define S_HI(reg)  (*((unsigned char *)&S_REG(e##reg) + 1))

/* these are used like: LWORD(eax) = 65535 (sets ax to 65535) */
#define S_LWORD(reg)	(*((unsigned short *)&S_REG(reg)))
#define S_HWORD(reg)	(*((unsigned short *)&S_REG(reg) + 1))

#define DTA_over_1MB (void*)(GetSegmentBaseAddress(DPMI_CLIENT.USER_DTA_SEL) + DPMI_CLIENT.USER_DTA_OFF)
#define DTA_under_1MB (void*)((DPMI_CLIENT.private_data_segment + DTA_Para_ADD) << 4)

#define MAX_DOS_PATH 260

/* We use static varialbes because DOS in non-reentrant, but maybe a */
/* better way? */
static unsigned short DS_MAPPED;
static unsigned short ES_MAPPED;
static int in_dos_21 = 0;
static int last_dos_21 = 0;

static void prepare_ems_frame(void)
{
    if (DPMI_CLIENT.ems_frame_mapped)
	return;
    DPMI_CLIENT.ems_frame_mapped = 1;
    emm_get_map_registers(DPMI_CLIENT.ems_map_buffer);
    emm_unmap_all();
}

static void restore_ems_frame(void)
{
    if (!DPMI_CLIENT.ems_frame_mapped)
	return;
    emm_set_map_registers(DPMI_CLIENT.ems_map_buffer);
    DPMI_CLIENT.ems_frame_mapped = 0;
}

/* DOS selector is a selector whose base address is less than 0xffff0 */
/* and para. aligned.                                                 */
static int in_dos_space(unsigned short sel, unsigned long off)
{
    unsigned long base = Segments[sel >> 3].base_addr;

    if (base + off > 0x10ffef) {	/* ffff:ffff for DOS high */
      D_printf("DPMI: base address %#lx of sel %#x > DOS limit\n", base, sel);
      return 0;
    } else
    if (base & 0xf) {
      D_printf("DPMI: base address %#lx of sel %#x not para. aligned.\n", base, sel);
      return 0;
    } else
      return 1;
}

static void old_dos_terminate(struct sigcontext_struct *scp, int i)
{
    unsigned short psp_seg_sel;
    unsigned char *ptr;

    D_printf("DPMI: old_dos_terminate, int=%#x\n", i);

    REG(cs)  = DPMI_CLIENT.CURRENT_PSP;
    REG(eip) = 0x100;

#if 0
    _eip = *(unsigned short *)((char *)(DPMI_CLIENT.CURRENT_PSP<<4) + 0xa);
    _cs = ConvertSegmentToCodeDescriptor(
      *(unsigned short *)((char *)(DPMI_CLIENT.CURRENT_PSP<<4) + 0xa+2));
#endif

    /* put our return address there */
    *(unsigned short *)((char *)(DPMI_CLIENT.CURRENT_PSP<<4) + 0xa) =
	     DPMI_OFF + HLT_OFF(DPMI_return_from_dosint) + i;
    *(unsigned short *)((char *)(DPMI_CLIENT.CURRENT_PSP<<4) + 0xa+2) = DPMI_SEG;

    psp_seg_sel = *(unsigned short *)((char *)(DPMI_CLIENT.CURRENT_PSP<<4) + 0x16);
    ptr = (char *)SEG2LINEAR(psp_seg_sel);
    if (ptr[0] != 0xCD || ptr[1] != 0x20) {
	unsigned short psp_seg = DPMI_CLIENT.CURRENT_PSP;
	D_printf("DPMI: Trying PSP sel=%#x, V=%i, d=%i, l=%#lx\n",
	    psp_seg_sel, ValidAndUsedSelector(psp_seg_sel),
	    in_dos_space(psp_seg_sel, 0), GetSegmentLimit(psp_seg_sel));
	if (ValidAndUsedSelector(psp_seg_sel) && in_dos_space(psp_seg_sel, 0) &&
		GetSegmentLimit(psp_seg_sel) >= 0xff) {
	    ptr = (char *)GetSegmentBaseAddress(psp_seg_sel);
	    D_printf("DPMI: Trying PSP sel=%#x, addr=%p\n", psp_seg_sel, ptr);
	    if ((!(((int)ptr) & 0x0f)) && ptr[0] == 0xCD && ptr[1] == 0x20) {
		psp_seg = ((int)ptr) >> 4;
	        D_printf("DPMI: parent PSP sel=%#x, seg=%#x\n",
		    psp_seg_sel, psp_seg);
	    }
	} else {
	    D_printf("DPMI: using current PSP as parent!\n");
	}
	*(unsigned short *)((char *)(DPMI_CLIENT.CURRENT_PSP<<4) + 0x16) = psp_seg;
    } else {
	D_printf("DPMI: parent PSP seg=%#x\n", psp_seg_sel);
    }

    /* And update our PSP pointer */
    DPMI_CLIENT.CURRENT_PSP =
	*(unsigned short *)((char *)(DPMI_CLIENT.CURRENT_PSP<<4) + 0x16);
}

/*
 * DANG_BEGIN_FUNCTION msdos_pre_extender
 *
 * This function is called before a protected mode client goes to real
 * mode for DOS service. All protected mode selector is changed to
 * real mode segment register. And if client\'s data buffer is above 1MB,
 * necessary buffer copying is performed. This function returns 1 if
 * it does not need to go to real mode, otherwise returns 0.
 *
 * DANG_END_FUNCTION
 */

int msdos_pre_extender(struct sigcontext_struct *scp, int intr)
{
    D_printf("DPMI: pre_extender: int 0x%x, ax=0x%x\n", intr, _LWORD(eax));
    DS_MAPPED = 0;
    ES_MAPPED = 0;
    if (DPMI_CLIENT.USER_DTA_SEL && intr == 0x21) {
	switch (_HI(ax)) {	/* functions use DTA */
	case 0x11: case 0x12:	/* find first/next using FCB */
	case 0x4e: case 0x4f:	/* find first/next */
	    MEMCPY_DOS2DOS(DTA_under_1MB, DTA_over_1MB, 0x80);
	    break;
	}
    }

    /* only consider DOS and some BIOS services */
    switch (intr) {
    case 0x2f:
	INT2f_SAVED_REGS = *scp;
	if (_LWORD(eax) == 0x1684) {
	    D_printf("DPMI: Get VxD entry point BX = 0x%04x\n",
		     _LWORD(ebx));
#if 1
	    switch (_LWORD(ebx)) {
		case 0x01:
		    D_printf("DPMI: VMM VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_VMM);
		    break;
		case 0x05:
		    D_printf("DPMI: VTD VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_VTD);
		    break;
		case 0x09:
		    D_printf("DPMI: Reboot VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_Reboot);
		    break;
		case 0x0a:
		    D_printf("DPMI: VDD VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_VDD);
		    break;
		case 0x0c:
		    D_printf("DPMI: VMD VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_VMD);
		    break;
		case 0x0e:
		    D_printf("DPMI: VCD VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_VCD);
		    break;
		case 0x17:
		    D_printf("DPMI: SHELL VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_SHELL);
		    break;
		case 0x21:
		    D_printf("DPMI: PageFile VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_PageFile);
		    break;
		case 0x26:
		    D_printf("DPMI: APM VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_APM);
		    break;
#if 0
		case 0x27:
		    D_printf("DPMI: VXDLDR VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_VXDLDR);
		    break;
#endif
		case 0x33:
		    D_printf("DPMI: CONFIGMG VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_CONFIGMG);
		    break;
		case 0x37:
		    D_printf("DPMI: ENABLE VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_ENABLE);
		    break;
		case 0x442:
		    D_printf("DPMI: VTDAPI VxD entry point requested\n");
		    _es = DPMI_CLIENT.DPMI_SEL;
		    _edi = DPMI_OFF + HLT_OFF(DPMI_VXD_VTDAPI);
		    break;
		default:
		    D_printf("DPMI: ERROR: Unsupported VxD\n");
		    /* no entry point */
		    _es = _edi = 0;
	    }
#else
	    D_printf("DPMI: ERROR: Unsupported VxD\n");
	    /* no entry point */
	    _es = _edi = 0;
#endif
	    return MSDOS_DONE;
	}
	return 0;
    case 0x41:			/* win debug */
	return MSDOS_DONE;

    case 0x10:			/* video */
	VIDEO_SAVED_REGS = *scp;
	switch (_HI(ax)) {
	case 0x10:		/* Set/Get Palette Registers (EGA/VGA) */
	    switch(_LO(ax)) {
	    case 0x2:		/* set all palette registers and border */
	    case 0x09:		/* ead palette registers and border (PS/2) */
	    case 0x12:		/* set block of DAC color registers */
	    case 0x17:		/* read block of DAC color registers */
		ES_MAPPED = 1;
		break;
	    default:
		return 0;
	    }
	    break;
	case 0x11:		/* Character Generator Routine (EGA/VGA) */
	    switch (_LO(ax)) {
	    case 0x0:		/* user character load */
	    case 0x10:		/* user specified character definition table */
	    case 0x20: case 0x21:
		ES_MAPPED = 1;
		break;
	    default:
		return 0;
	    }
	    break;
	case 0x13:		/* Write String */
	case 0x15:		/*  Return Physical Display Parms */
	case 0x1b:
	    ES_MAPPED = 1;
	    break;
	case 0x1c:
	    if (_LO(ax) == 1 || _LO(ax) == 2)
		ES_MAPPED = 1;
	    else
		return 0;
	    break;
	default:
	    return 0;
	}
	break;
    case 0x15:			/* misc */
	INT15_SAVED_REGS = *scp;
	switch (_HI(ax)) {
	  case 0xc2:
	    D_printf("DPMI: PS2MOUSE function 0x%x\n", _LO(ax));
	    switch (_LO(ax)) {
	      case 0x07:		/* set handler addr */
		if ( _es && D_16_32(_ebx) ) {
		  D_printf("DPMI: PS2MOUSE: set handler addr 0x%x:0x%lx\n",
		    _es, D_16_32(_ebx));
		  PS2mouseCallBack.selector = _es;
		  PS2mouseCallBack.offset = D_16_32(_ebx); 
		  REG(es) = DPMI_SEG;
		  REG(ebx) = DPMI_OFF + HLT_OFF(DPMI_PS2_mouse_callback);
		} else {
		  D_printf("DPMI: PS2MOUSE: reset handler addr\n");
		  REG(es) = 0;
		  REG(ebx) = 0;
		}
		return 0;
	      default:
		return 0;
	    }
	    break;
	  default:
	    return 0;
	}
    case 0x20:			/* DOS terminate */
	old_dos_terminate(scp, intr);
	return 0;
    case 0x21:
	if (in_dos_21) 
	dbug_printf("DPMI: int21 AX=%#04x called recursively "
		    "from inside %#04x, in_dos_21=%d\n",
		_LWORD(eax), last_dos_21, in_dos_21);
	last_dos_21 = _LWORD(eax);

	SAVED_REGS = *scp;
	switch (_HI(ax)) {
	    /* first see if we don\'t need to go to real mode */
	case 0x25:		/* set vector */
	    DPMI_CLIENT.Interrupt_Table[_LO(ax)].selector = _ds;
	    DPMI_CLIENT.Interrupt_Table[_LO(ax)].offset = D_16_32(_edx);
	    D_printf("DPMI: int 21,ax=0x%04x, ds=0x%04x. dx=0x%04x\n",
		     _LWORD(eax), _ds, _LWORD(edx));
	    return MSDOS_DONE;
	case 0x35:	/* Get Interrupt Vector */
	    _es = DPMI_CLIENT.Interrupt_Table[_LO(ax)].selector;
	    _ebx = DPMI_CLIENT.Interrupt_Table[_LO(ax)].offset;
	    D_printf("DPMI: int 21,ax=0x%04x, es=0x%04x. bx=0x%04x\n",
		     _LWORD(eax), _es, _LWORD(ebx));
	    return MSDOS_DONE;
	case 0x48:		/* allocate memory */
	    {
		dpmi_pm_block *bp = DPMImalloc(_LWORD(ebx)<<4);
		if (!bp) {
		    _eflags |= CF;
		    _LWORD(ebx) = dpmi_free_memory >> 4;
		    _LWORD(eax) = 0x08;
		} else {
		    unsigned short sel = AllocateDescriptors(1);
		    SetSegmentBaseAddress(sel, (unsigned long)bp->base);
		    SetSegmentLimit(sel, bp -> size - 1);
		    _LWORD(eax) = sel;
		    _eflags &= ~CF;
		}
		return MSDOS_DONE;
	    }
	case 0x49:		/* free memory */
	    {
		unsigned long h =
		    base2handle((void *)GetSegmentBaseAddress(_es));
		if (!h) 
		    _eflags |= CF;
		else {
		    _eflags &= ~CF;
		    DPMIfree(h);
		    FreeDescriptor(_es);
		    FreeSegRegs(scp, _es);
		}
		return MSDOS_DONE;
	    }
	case 0x4a:		/* reallocate memory */
	    {
		unsigned long h;
		dpmi_pm_block *bp;

		h = base2handle((void *)GetSegmentBaseAddress(_es));
		if (!h) {
		    _eflags |= CF;
		    return MSDOS_DONE;
		}
		bp = DPMIrealloc(h, _LWORD(ebx)<<4);
		if (!bp) {
		    _eflags |= CF;
		    _LWORD(ebx) = dpmi_free_memory >> 4;
		    _LWORD(eax) = 0x08;
		} else {
		    SetSegmentBaseAddress(_es, (unsigned long)bp->base);
		    SetSegmentLimit(_es, bp -> size - 1);
		    _eflags &= ~CF;
		}
		return MSDOS_DONE;
	    }
	case 0x01 ... 0x08:	/* These are dos functions which */
	case 0x0b ... 0x0e:	/* are not required memory copy, */
	case 0x19:		/* and segment register translation. */
	case 0x2a ... 0x2e:
	case 0x30 ... 0x34:
	case 0x36: case 0x37:
	case 0x3e:
	case 0x42:
	case 0x45: case 0x46:
	case 0x4d:
	case 0x4f:		/* find next */
	case 0x54:
	case 0x58: case 0x59:
	case 0x5c:		/* lock */
	case 0x66 ... 0x68:	
	case 0xF8:		/* OEM SET vector */
	    in_dos_21++;
	    return 0;
	case 0x00:		/* DOS terminate */
	    old_dos_terminate(scp, intr);
	    in_dos_21++;
	    return 0;
	case 0x09:		/* Print String */
	    {
		int i;
		char *s, *d;
		prepare_ems_frame();
		REG(ds) = TRANS_BUFFER_SEG;
		REG(edx) = 0;
		d = (char *)(REG(ds)<<4);
		s = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx);
		for(i=0; i<0xffff; i++, d++, s++) {
		    *d = *s;
		    if( *s == '$')
			break;
		}
	    }
	    in_dos_21++;
	    return 0;
	case 0x0a:		/* buffered keyboard input */
	case 0x38:
	case 0x5a:		/* mktemp */
	case 0x5d:		/* Critical Error Information  */
	case 0x69:
	    DS_MAPPED = 1;
	    break;
	case 0x1a:		/* set DTA */
	  {
	    unsigned long off = D_16_32(_edx);
	    if ( !in_dos_space(_ds, off)) {
		DPMI_CLIENT.USER_DTA_SEL = _ds;
		DPMI_CLIENT.USER_DTA_OFF = off;
		REG(ds) = DPMI_CLIENT.private_data_segment+DTA_Para_ADD;
		REG(edx)=0;
                MEMCPY_DOS2DOS(DTA_under_1MB, DTA_over_1MB, 0x80);
	    } else {
                REG(ds) = GetSegmentBaseAddress(_ds) >> 4;
                DPMI_CLIENT.USER_DTA_SEL = 0;
            }
	  }
          in_dos_21++;
	  return 0;
            
	/* FCB functions */	    
	case 0x0f: case 0x10:	/* These are not supported by */
	case 0x14: case 0x15:	/* dosx.exe, according to Ralf Brown */
	case 0x21 ... 0x24:
	case 0x27: case 0x28:
	    error("MS-DOS: Unsupported function 0x%x\n", _HI(ax));
	    _HI(ax) = 0xff;
	    return MSDOS_DONE;
	case 0x11: case 0x12:	/* find first/next using FCB */
	case 0x13:		/* Delete using FCB */
	case 0x16:		/* Create usring FCB */
	case 0x17:		/* rename using FCB */
	    prepare_ems_frame();
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(edx)=0;
	    MEMCPY_DOS2DOS(SEG_ADR((void *), ds, dx),
			(void *)GetSegmentBaseAddress(_ds) + D_16_32(_edx),
			0x50);
	    in_dos_21++;
	    return 0;
	case 0x29:		/* Parse a file name for FCB */
	    {
		unsigned short seg = TRANS_BUFFER_SEG;
		prepare_ems_frame();
		REG(ds) = seg;
		REG(esi) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), ds, si),
			    (void *)GetSegmentBaseAddress(_ds) + D_16_32(_esi),
			    0x100);
		seg += 0x10;
		REG(es) = seg;
		REG(edi) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), es, di),
			    (void *)GetSegmentBaseAddress(_es) + D_16_32(_edi),
			    0x50);
	    }
	    in_dos_21++;
	    return 0;
	case 0x44:		/* IOCTL */
	    switch (_LO(ax)) {
	    case 0x02 ... 0x05:
	    case 0x0c: case 0x0d:
		DS_MAPPED = 1;
		break;
	    default:
		in_dos_21++;
		return 0;
	    }
	    break;
	case 0x47:		/* GET CWD */
	    prepare_ems_frame();
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(esi) = 0;
	    in_dos_21++;
	    return 0;
	case 0x4b:		/* EXEC */
	    D_printf("BCC: call dos exec.\n");
	    REG(cs) = DPMI_SEG;
	    REG(eip) = DPMI_OFF + HLT_OFF(DPMI_return_from_dos_exec);
	    msdos_pre_exec(scp);
	    return MSDOS_ALT_RET | MSDOS_NEED_FORK;

	case 0x50:		/* set PSP */
	  {
	    unsigned short envp;
	    if ( !in_dos_space(_LWORD(ebx), 0)) {
		DPMI_CLIENT.USER_PSP_SEL = _LWORD(ebx);
		LWORD(ebx) = DPMI_CLIENT.CURRENT_PSP;
		MEMCPY_DOS2DOS((void *)SEG2LINEAR(LWORD(ebx)), 
		    (void *)GetSegmentBaseAddress(_LWORD(ebx)), 0x100);
		D_printf("DPMI: PSP moved from %p to %p\n",
		    (char *)GetSegmentBaseAddress(_LWORD(ebx)),
		    (void *)SEG2LINEAR(LWORD(ebx)));
	    } else {
		REG(ebx) = GetSegmentBaseAddress(_LWORD(ebx)) >> 4;
		DPMI_CLIENT.USER_PSP_SEL = 0;
	    }
	    DPMI_CLIENT.CURRENT_PSP = LWORD(ebx);
	    envp = *(unsigned short *)(((char *)(LWORD(ebx)<<4)) + 0x2c);
	    if ( !in_dos_space(envp, 0)) {
		/* DANG_FIXTHIS: Please implement the ENV translation! */
		error("FIXME: ENV translation is not implemented\n");
		DPMI_CLIENT.CURRENT_ENV_SEL = 0;
	    } else {
		DPMI_CLIENT.CURRENT_ENV_SEL = envp;
	    }
	  }
	  in_dos_21++;
	  return 0;

	case 0x26:		/* create PSP */
	    prepare_ems_frame();
	    REG(edx) = TRANS_BUFFER_SEG;
	    in_dos_21++;
	    return 0;

	case 0x55:		/* create & set PSP */
	    if ( !in_dos_space(_LWORD(edx), 0)) {
		DPMI_CLIENT.USER_PSP_SEL = _LWORD(edx);
		LWORD(edx) = DPMI_CLIENT.CURRENT_PSP;
	    } else {
		REG(edx) = GetSegmentBaseAddress(_LWORD(edx)) >> 4;
		DPMI_CLIENT.CURRENT_PSP = LWORD(edx);
		DPMI_CLIENT.USER_PSP_SEL = 0;
	    }
	    in_dos_21++;
	    return 0;

	case 0x39:		/* mkdir */
	case 0x3a:		/* rmdir */
	case 0x3b:		/* chdir */
	case 0x3c:		/* creat */
	case 0x3d:		/* Dos OPEN */
	case 0x41:		/* unlink */
	case 0x43:		/* change attr */
	case 0x4e:		/* find first */
	case 0x5b:		/* Create */
	    if ((_HI(ax) == 0x4e) && (_ecx & 0x8))
		D_printf("DPMI: MS-DOS try to find volume label\n");
	    {
		char *src, *dst;
		prepare_ems_frame();
		REG(ds) = TRANS_BUFFER_SEG;
		REG(edx) = 0;
		src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx);
		dst = SEG_ADR((char *), ds, dx);
		D_printf("DPMI: passing ASCIIZ > 1MB to dos %#x\n", (int)dst); 
		D_printf("%#x: '%s'\n", (int)src, src);
                snprintf(dst, MAX_DOS_PATH, "%s", src);
	    }
	    in_dos_21++;
	    return 0;
	case 0x3f:		/* dos read */
	    set_io_buffer((char*)GetSegmentBaseAddress(_ds) + D_16_32(_edx),
		D_16_32(_ecx));
	    prepare_ems_frame();
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(edx) = 0;
	    REG(ecx) = D_16_32(_ecx);
	    in_dos_21++;
	    fake_int_to(DOS_LONG_READ_SEG, DOS_LONG_READ_OFF);
	    return MSDOS_ALT_ENT;
	case 0x40:		/* DOS Write */
	    set_io_buffer((char*)GetSegmentBaseAddress(_ds) + D_16_32(_edx),
		D_16_32(_ecx));
	    prepare_ems_frame();
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(edx) = 0;
	    REG(ecx) = D_16_32(_ecx);
	    in_dos_21++;
	    fake_int_to(DOS_LONG_WRITE_SEG, DOS_LONG_WRITE_OFF);
	    return MSDOS_ALT_ENT;
	case 0x53:		/* Generate Drive Parameter Table  */
	    {
		unsigned short seg = TRANS_BUFFER_SEG;
		prepare_ems_frame();
		REG(ds) = seg;
		REG(esi) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), ds, si),
			    (void *)GetSegmentBaseAddress(_ds) + D_16_32(_esi),
			    0x30);
		seg += 30;

		REG(es) = seg;
		REG(ebp) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), es, bp),
			    (void *)GetSegmentBaseAddress(_es) + D_16_32(_ebp),
			    0x60);
	    }
	    in_dos_21++;
	    return 0;
	case 0x56:		/* rename file */
	    {
		unsigned short seg = TRANS_BUFFER_SEG;
		prepare_ems_frame();
		REG(ds) = seg;
		REG(edx) = 0;
		snprintf((char *)(REG(ds)<<4), MAX_DOS_PATH, "%s",
			     (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx));
		seg += 0x20;

		REG(es) = seg;
		REG(edi) = 0;
		snprintf((char *)(REG(es)<<4), MAX_DOS_PATH, "%s",
			     (char *)GetSegmentBaseAddress(_es) + D_16_32(_edi));
	    }
	    in_dos_21++;
	    return 0;
	case 0x57:		/* Get/Set File Date and Time Using Handle */
	    if ((_LO(ax) == 0) || (_LO(ax) == 1)) {
		in_dos_21++;
		return 0;
	    }
	    ES_MAPPED = 1;
	    break;
	case 0x5e:
	    if (_LO(ax) == 0x03)
		ES_MAPPED = 1;
	    else
		DS_MAPPED = 1;
	    break;
	case 0x5f:		/* redirection */
	    switch (_LO(ax)) {
	    case 0: case 1:
		in_dos_21++;
		return 0;
	    case 2 ... 6:
		prepare_ems_frame();
		REG(ds) = TRANS_BUFFER_SEG;
		REG(esi) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), ds, si),
			(void *)GetSegmentBaseAddress(_ds) + D_16_32(_esi),
			0x100);
		REG(es) = TRANS_BUFFER_SEG + 0x10;
		REG(edi) = 0;
		MEMCPY_DOS2DOS(SEG_ADR((void *), es, di),
			(void *)GetSegmentBaseAddress(_es) + D_16_32(_edi),
			0x100);
		in_dos_21++;
		return 0;
	    }
	case 0x60:		/* Get Fully Qualified File Name */
	    prepare_ems_frame();
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(esi) = 0;
	    MEMCPY_DOS2DOS(SEG_ADR((void *), ds, si),
		    (void *)GetSegmentBaseAddress(_ds) + D_16_32(_esi),
		    0x100);
	    REG(es) = TRANS_BUFFER_SEG + 0x10;
	    REG(edi) = 0;
	    in_dos_21++;
	    return 0;
	case 0x6c:		/*  Extended Open/Create */
	    {
		char *src, *dst;
		prepare_ems_frame();
		REG(ds) = TRANS_BUFFER_SEG;
		REG(esi) = 0;
		src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_esi);
		dst = SEG_ADR((char *), ds, si);
		D_printf("DPMI: passing ASCIIZ > 1MB to dos %#x\n", (int)dst); 
		D_printf("%#x: '%s'\n", (int)src, src);
		snprintf(dst, MAX_DOS_PATH, "%s", src);
	    }
	    in_dos_21++;
	    return 0;
	case 0x65:		/* internationalization */
    	    switch (_LO(ax)) {
		case 0:
		    prepare_ems_frame();
		    REG(es) = TRANS_BUFFER_SEG;
		    REG(edi) = 0;
		    MEMCPY_DOS2DOS(SEG_ADR((void *), es, di),
			(void *)GetSegmentBaseAddress(_es) + D_16_32(_edi),
			_LWORD(ecx));
		    break;
		case 1 ... 7:
		    prepare_ems_frame();
		    REG(es) = TRANS_BUFFER_SEG;
		    REG(edi) = 0;
		    break;
		case 0x21:
		case 0xa1:
		    prepare_ems_frame();
		    REG(ds) = TRANS_BUFFER_SEG;
		    REG(edx) = 0;
		    MEMCPY_DOS2DOS(SEG_ADR((void *), ds, dx),
			(void *)GetSegmentBaseAddress(_ds) + D_16_32(_edx),
			_LWORD(ecx));
		    break;
		case 0x22:
		case 0xa2:
		    prepare_ems_frame();
		    REG(ds) = TRANS_BUFFER_SEG;
		    REG(edx) = 0;
		    strcpy(SEG_ADR((void *), ds, dx),
			(void *)GetSegmentBaseAddress(_ds) + D_16_32(_edx));
		    break;
	    }
            in_dos_21++;
            return 0;
    case 0x71:     /* LFN functions */
        {
        char *src, *dst;
        switch (_LO(ax)) {
        case 0x3B: /* change dir */
        case 0x41: /* delete file */
            REG(ds) = TRANS_BUFFER_SEG;
            REG(edx) = 0;
            src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx);
            dst = SEG_ADR((char *), ds, dx);
            snprintf(dst, MAX_DOS_PATH, "%s", src);
            in_dos_21++;
            return 0;
        case 0x4E: /* find first file */
            REG(ds) = TRANS_BUFFER_SEG;
            REG(edx) = 0;
            REG(es) = TRANS_BUFFER_SEG;
            REG(edi) = MAX_DOS_PATH;
            src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx);
            dst = SEG_ADR((char *), ds, dx);
            snprintf(dst, MAX_DOS_PATH, "%s", src);
            in_dos_21++;
            return 0;
        case 0x4F: /* find next file */
            REG(es) = TRANS_BUFFER_SEG;
            REG(edi) = 0;
            src = (char *)GetSegmentBaseAddress(_es) + D_16_32(_edi);
            dst = SEG_ADR((char *), es, di);
            MEMCPY_DOS2DOS(dst, src, 0x13e);
            in_dos_21++;
            return 0;
        case 0x47: /* get cur dir */
            REG(ds) = TRANS_BUFFER_SEG;
            REG(esi) = 0;
            in_dos_21++;
            return 0;
	case 0x60: /* canonicalize filename */
	    REG(ds) = TRANS_BUFFER_SEG;
	    REG(esi) = 0;
	    REG(es) = TRANS_BUFFER_SEG;
	    REG(edi) = MAX_DOS_PATH;
	    src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_esi);	
	    dst = SEG_ADR((char *), ds, si);
	    snprintf(dst, MAX_DOS_PATH, "%s", src);
	    in_dos_21++;
	    return 0;
        case 0x6c: /* extended open/create */
            REG(ds) = TRANS_BUFFER_SEG;
            REG(esi) = 0;
            src = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_esi);
            dst = SEG_ADR((char *), ds, si);
            snprintf(dst, MAX_DOS_PATH, "%s", src);
            in_dos_21++;
            return 0;
        case 0xA1: /* close find */
            in_dos_21++;
            return 0;
        default: /* all other subfuntions currently not supported */
            _eflags |= CF;
            _eax = _eax & 0xFFFFFF00;
            return 1;
        }
        }
	default:
	    break;
	}
	break;
    case 0x25:			/* Absolute Disk Read */
    case 0x26:			/* Absolute Disk write */
	DS_MAPPED = 1;
	D_printf("DPMI: msdos Absolute Disk Read/Write called.\n");
	break;
    case 0x33:			/* mouse */
	MOUSE_SAVED_REGS = *scp;
	switch (_LWORD(eax)) {
	case 0x09:		/* Set Mouse Graphics Cursor */
	    prepare_ems_frame();
	    REG(es) = TRANS_BUFFER_SEG;
	    REG(edx) = 0;
	    MEMCPY_DOS2DOS(SEG_ADR((void *), es, dx),
		    (void *)GetSegmentBaseAddress(_es) + D_16_32(_edx),
		    16);
	    return 0;
	case 0x0c:		/* set call back */
	case 0x14:		/* swap call back */
	    if ( _es && D_16_32(_edx) ) {
		D_printf("DPMI: set mouse callback\n");
		mouseCallBack.selector = _es;
		mouseCallBack.offset = D_16_32(_edx); 
		REG(es) = DPMI_SEG;
		REG(edx) = DPMI_OFF + HLT_OFF(DPMI_mouse_callback);
	    } else {
		D_printf("DPMI: reset mouse callback\n");
		REG(es) = 0;
		REG(edx) = 0;
	    }
	    return 0;
	case 0x16:		/* save state */
	case 0x17:		/* restore */
	    ES_MAPPED = 1;
	    break;
	default:
	    return 0;
	}
    default:
	return 0;
    }

    if (DS_MAPPED) {
	char *src, *dst;
	int len;
	prepare_ems_frame();
	DS_MAPPED = _ds;
	REG(ds) = TRANS_BUFFER_SEG;
	src = (char *)GetSegmentBaseAddress(_ds);
	dst = (char *)(REG(ds)<<4);
	len = ((Segments[_ds >> 3].limit > 0xffff) ||
	    	Segments[_ds >> 3].is_big) ?
		0xffff : Segments[_ds >> 3].limit;
	D_printf("DPMI: whole segment of DS at %#x copy to DOS at %#x for %#x\n",
		(int)src, (int)dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    }

    if (ES_MAPPED) {
	char *src, *dst;
	int len;
	prepare_ems_frame();
	ES_MAPPED = _es;
	REG(es) = TRANS_BUFFER_SEG;
	src = (char *)GetSegmentBaseAddress(_es);
	dst = (char *)(REG(es)<<4);
	len = ((Segments[_es >> 3].limit > 0xffff) ||
	    	Segments[_es >> 3].is_big) ?
		0xffff : Segments[_es >> 3].limit;
	D_printf("DPMI: whole segment of ES at %#x copy to DOS at %#x for %#x\n",
		(int)src, (int)dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    }
    if (intr==0x21) in_dos_21++;
    return 0;
}

void msdos_pre_exec(struct sigcontext_struct *scp)
{
    /* we must copy all data from higher 1MB to lower 1MB */
    unsigned short segment = TRANS_BUFFER_SEG;
    char *p;
    unsigned short sel,off;

    /* must copy command line */
    prepare_ems_frame();
    REG(ds) = segment;
    REG(edx) = 0;
    p = (char *)GetSegmentBaseAddress(_ds) + D_16_32(_edx);
    snprintf((char *)SEG2LINEAR(REG(ds)), MAX_DOS_PATH, "%s", p);
    segment += (MAX_DOS_PATH + 0x0f) >> 4;

    /* must copy parameter block */
    REG(es) = segment;
    REG(ebx) = 0;
    MEMCPY_DOS2DOS(SEG_ADR((void *), es, bx),
       (void *)GetSegmentBaseAddress(_es) + D_16_32(_ebx), 0x20);
    segment += 2;
#if 0
    /* now the envrionment segment */
    sel = READ_WORD(SEG_ADR((unsigned short *), es, bx));
    WRITE_WORD(SEG_ADR((unsigned short *), es, bx), segment);
    MEMCPY_DOS2DOS((void *)SEG2LINEAR(segment),           /* 4K envr. */
	(void *)GetSegmentBaseAddress(sel),
	0x1000);
    segment += 0x100;
#else
    WRITE_WORD(SEG_ADR((unsigned short *), es, bx), 0);
#endif
    /* now the tail of the command line */
    off = READ_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+2));
    sel = READ_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+4));
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+4), segment);
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+2), 0);
    MEMCPY_DOS2DOS((void *)SEG2LINEAR(segment),
	   (void *)GetSegmentBaseAddress(sel) + off,
	   0x80);
    segment += 8;

    /* set the FCB pointers to something reasonable */
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+6), 0);
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+8), segment);
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+0xA), 0);
    WRITE_WORD(SEGOFF2LINEAR(REG(es), LWORD(ebx)+0xC), segment);
    memset((void *)SEG2LINEAR(segment), 0, 0x30);
    segment += 3;

    /* then the enviroment seg */
    if (DPMI_CLIENT.CURRENT_ENV_SEL)
	WRITE_WORD(SEGOFF2LINEAR(DPMI_CLIENT.CURRENT_PSP, 0x2c),
	    GetSegmentBaseAddress(DPMI_CLIENT.CURRENT_ENV_SEL) >> 4);
}

void msdos_post_exec(void)
{
    DPMI_CLIENT.stack_frame.eflags = 0x0202 | (0x0dd5 & REG(eflags));
    DPMI_CLIENT.stack_frame.eax = REG(eax);
    if (!(LWORD(eflags) & CF)) {
	DPMI_CLIENT.stack_frame.ebx = REG(ebx);
        DPMI_CLIENT.stack_frame.edx = REG(edx);
     }

    if (DPMI_CLIENT.CURRENT_ENV_SEL)
	*(unsigned short *)((char *)(DPMI_CLIENT.CURRENT_PSP<<4) + 0x2c) =
	                     DPMI_CLIENT.CURRENT_ENV_SEL;

    restore_ems_frame();
}

/*
 * DANG_BEGIN_FUNCTION msdos_post_extender
 *
 * This function is called after return from real mode DOS service
 * All real mode segment registers are changed to protected mode selectors
 * And if client\'s data buffer is above 1MB, necessary buffer copying
 * is performed.
 *
 * DANG_END_FUNCTION
 */

void msdos_post_extender(int intr)
{
    D_printf("DPMI: post_extender: int 0x%x\n", intr);

    if (DPMI_CLIENT.USER_DTA_SEL && intr == 0x21 ) {
	switch (S_HI(ax)) {	/* functions use DTA */
	case 0x11: case 0x12:	/* find first/next using FCB */
	case 0x4e: case 0x4f:	/* find first/next */
	    MEMCPY_DOS2DOS(DTA_over_1MB, DTA_under_1MB, 0x80);
	    break;
	}
    }

    if (DS_MAPPED) {
	unsigned short my_ds;
	char *src, *dst;
	int len;
	my_ds = TRANS_BUFFER_SEG;
	src = (char *)(my_ds<<4);
	dst = (char *)GetSegmentBaseAddress(DS_MAPPED);
	len = ((Segments[DS_MAPPED >> 3].limit > 0xffff) ||
	    	Segments[DS_MAPPED >> 3].is_big) ?
		0xffff : Segments[DS_MAPPED >> 3].limit;
	D_printf("DPMI: DS_MAPPED seg at %#x copy back at %#x for %#x\n",
		(int)src, (int)dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    } 

    if (ES_MAPPED) {
	unsigned short my_es;
	char *src, *dst;
	int len;
	my_es = TRANS_BUFFER_SEG;
	src = (char *)(my_es<<4);
	dst = (char *)GetSegmentBaseAddress(ES_MAPPED);
	len = ((Segments[ES_MAPPED >> 3].limit > 0xffff) ||
	    	Segments[ES_MAPPED >> 3].is_big) ?
		0xffff : Segments[ES_MAPPED >> 3].limit;
	D_printf("DPMI: ES_MAPPED seg at %#x copy back at %#x for %#x\n",
		(int)src, (int)dst, len);
	MEMCPY_DOS2DOS(dst, src, len);
    } 

    switch (intr) {
    case 0x10:			/* video */
	if ((VIDEO_SAVED_REGS.eax & 0xffff) == 0x1130) {
	    /* get current character generator infor */
	    DPMI_CLIENT.stack_frame.es =
		ConvertSegmentToDescriptor(REG(es));
	    return;
	} else
	    break;
    case 0x15:
	/* we need to save regs at int15 because AH has the return value */
	if ((INT15_SAVED_REGS.eax & 0xff00) == 0xc000) { /* Get Configuration */
                if (REG(eflags)&CF)
                        return;
                if (!(DPMI_CLIENT.stack_frame.es =
                         ConvertSegmentToDescriptor(REG(es)))) return;
                break;
      }
        else
                return;
    case 0x2f:
	switch (LO_WORD(INT2f_SAVED_REGS.eax)) {
	    case 0x4310:
                XMS_call = MK_FARt(REG(es), LWORD(ebx));
                DPMI_CLIENT.stack_frame.es = DPMI_CLIENT.DPMI_SEL;
                DPMI_CLIENT.stack_frame.ebx = DPMI_OFF + HLT_OFF(DPMI_XMS_call);
		break;
	}
	return;

    case 0x21:
	in_dos_21--;
	switch (S_HI(ax)) {
	case 0x09:		/* print String */
	case 0x1a:		/* set DTA */
	    DPMI_CLIENT.stack_frame.edx = S_REG(edx);
	    break;
	case 0x11: case 0x12:	/* findfirst/next using FCB */
 	case 0x13:		/* Delete using FCB */
 	case 0x16:		/* Create usring FCB */
 	case 0x17:		/* rename using FCB */
	    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(S_REG(ds)) + D_16_32(S_REG(edx)),
			SEG_ADR((void *), ds, dx), 0x50);
 	    DPMI_CLIENT.stack_frame.edx = S_REG(edx);
	    break ;

	case 0x29:		/* Parse a file name for FCB */
	    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(S_REG(ds)) + D_16_32(S_REG(esi)),
		/* Warning: SI altered, assume old value = 0, don't touch. */
			    (void *)(REG(ds)<<4), 0x100);
	    DPMI_CLIENT.stack_frame.esi = S_REG(esi) + LWORD(esi); 

	    MEMCPY_DOS2DOS((void *)(GetSegmentBaseAddress(S_REG(es)) + D_16_32(S_REG(edi))),
			    SEG_ADR((void *), es, di),  0x50);
	    DPMI_CLIENT.stack_frame.edi = S_REG(edi);
	    break;

	case 0x2f:		/* GET DTA */
	    if (SEG_ADR((void*), es, bx) == DTA_under_1MB) {
		if (!DPMI_CLIENT.USER_DTA_SEL)
		    error("Selector is not set for the translated DTA\n");
		DPMI_CLIENT.stack_frame.es = DPMI_CLIENT.USER_DTA_SEL;
		DPMI_CLIENT.stack_frame.ebx = DPMI_CLIENT.USER_DTA_OFF;
	    } else {
		DPMI_CLIENT.stack_frame.es = ConvertSegmentToDescriptor(REG(es));
		HI_WORD(DPMI_CLIENT.stack_frame.ebx) = 0;
	    }
	    break;

	case 0x34:		/* Get Address of InDOS Flag */
	case 0x35:		/* GET Vector */
	case 0x52:		/* Get List of List */
	    if (!ES_MAPPED)
	      DPMI_CLIENT.stack_frame.es =
	               ConvertSegmentToDescriptor(REG(es));
	    break;

	case 0x39:		/* mkdir */
	case 0x3a:		/* rmdir */
	case 0x3b:		/* chdir */
	case 0x3c:		/* creat */
	case 0x3d:		/* Dos OPEN */
	case 0x41:		/* unlink */
	case 0x43:		/* change attr */
	case 0x4e:		/* find first */
	case 0x5b:		/* Create */
	    DPMI_CLIENT.stack_frame.edx = S_REG(edx);
	    break;

	case 0x50:		/* Set PSP */
	    DPMI_CLIENT.stack_frame.ebx = S_REG(ebx);
	    break;

	case 0x6c:		/*  Extended Open/Create */
	    DPMI_CLIENT.stack_frame.esi = S_REG(esi);
	    break;
	    
	case 0x55:		/* create & set PSP */
	  {
	    unsigned short envp;
	    envp = *(unsigned short *)(((char *)(LWORD(edx)<<4)) + 0x2c);
	    DPMI_CLIENT.CURRENT_ENV_SEL = ConvertSegmentToDescriptor(envp);
	    if ( !in_dos_space(S_LWORD(edx), 0)) {
		MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(S_LWORD(edx)),
		    SEG2LINEAR(LWORD(edx)), 0x100);
	    }
	    DPMI_CLIENT.stack_frame.edx = S_REG(edx);
	  }
	  break;

	case 0x26:		/* create PSP */
	    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(S_LWORD(edx)),
		(void *)SEG2LINEAR(LWORD(edx)), 0x100);
	    DPMI_CLIENT.stack_frame.edx = S_REG(edx);
	  break;

        case 0x59:		/* Get EXTENDED ERROR INFORMATION */
	    if(LWORD(eax) == 0x22 && !ES_MAPPED) { /* only this code has a pointer */
		DPMI_CLIENT.stack_frame.es =
			ConvertSegmentToDescriptor(REG(es));
	    }
	    break;
	case 0x38:
	    if (S_LO(ax) == 0x00 && !DS_MAPPED) { /* get country info */
		DPMI_CLIENT.stack_frame.ds =
	               ConvertSegmentToDescriptor(REG(ds));
	    }
	    break;
	case 0x47:		/* get CWD */
	    DPMI_CLIENT.stack_frame.esi = S_REG(esi);
	    if (LWORD(eflags) & CF)
		break;
	    snprintf((char *)(GetSegmentBaseAddress(S_REG(ds)) +
			D_16_32(S_REG(esi))), 0x40, "%s", 
		        SEG_ADR((char *), ds, si));
	    D_printf("DPMI: CWD: %s\n",(char *)(GetSegmentBaseAddress(S_REG(ds)) +
			D_16_32(S_REG(esi))));
	    break;
#if 0	    
	case 0x48:		/* allocate memory */
	    if (LWORD(eflags) & CF)
		break;
	    DPMI_CLIENT.stack_frame.eax =
		ConvertSegmentToDescriptor(LWORD(eax));
	    break;
#endif	    
	case 0x51:		/* get PSP */
	case 0x62:
	    {/* convert environment pointer to a descriptor*/
		unsigned short 
#if 0
		envp,
#endif
		psp;
		psp = LWORD(ebx);
#if 0
		envp = *(unsigned short *)(((char *)(psp<<4))+0x2c);
		envp = ConvertSegmentToDescriptor(envp);
		*(unsigned short *)(((char *)(psp<<4))+0x2c) = envp;
#endif
		if (psp == DPMI_CLIENT.CURRENT_PSP && DPMI_CLIENT.USER_PSP_SEL) {
		    DPMI_CLIENT.stack_frame.ebx = DPMI_CLIENT.USER_PSP_SEL;
		} else {
		    DPMI_CLIENT.stack_frame.ebx = ConvertSegmentToDescriptor(psp);
		}
	    }
	    break;
	case 0x53:		/* Generate Drive Parameter Table  */
	    DPMI_CLIENT.stack_frame.esi = S_REG(esi);
	    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(S_REG(es)) + D_16_32(S_REG(ebp)),
			    SEG_ADR((void *), es, bp),
			    0x60);
	    DPMI_CLIENT.stack_frame.ebp = S_REG(ebp);
	    break ;
	case 0x56:		/* rename */
	    DPMI_CLIENT.stack_frame.edx = S_REG(edx);
	    DPMI_CLIENT.stack_frame.edi = S_REG(edi);
	    break ;
	case 0x5d:
	    if (S_LO(ax) == 0x06) /* get address of DOS swappable area */
				/*        -> DS:SI                     */
		DPMI_CLIENT.stack_frame.ds = ConvertSegmentToDescriptor(REG(ds));
	    break;
	case 0x3f:
	    unset_io_buffer();
	    DPMI_CLIENT.stack_frame.edx = S_REG(edx);
	    DPMI_CLIENT.stack_frame.ecx = S_REG(ecx);
	    break;
	case 0x40:
	    unset_io_buffer();
	    DPMI_CLIENT.stack_frame.edx = S_REG(edx);
	    DPMI_CLIENT.stack_frame.ecx = S_REG(ecx);
	    break;
	case 0x5f:		/* redirection */
	    switch (S_LO(ax)) {
	    case 0: case 1:
		break ;
	    case 2 ... 6:
		DPMI_CLIENT.stack_frame.esi = S_REG(esi);
		MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(S_REG(ds))
			+ D_16_32(S_REG(esi)),
			SEG_ADR((void *), ds, si),
			0x100);
		DPMI_CLIENT.stack_frame.edi = S_REG(edi);
		MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(S_REG(es))
			+ D_16_32(S_REG(edi)),
			SEG_ADR((void *), es, di),
			0x100);
	    }
	    break;
	case 0x60:		/* Canonicalize file name */
	    DPMI_CLIENT.stack_frame.esi = S_REG(esi);
	    DPMI_CLIENT.stack_frame.edi = S_REG(edi);
	    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(S_REG(es))
			+ D_16_32(S_REG(edi)),
			SEG_ADR((void *), es, di),
			0x80);
	    break;
	case 0x65:		/* internationalization */
	    DPMI_CLIENT.stack_frame.edi = S_REG(edi);
	    DPMI_CLIENT.stack_frame.edx = S_REG(edx);
	    if (LWORD(eflags) & CF)
		break;
    	    switch (S_LO(ax)) {
		case 1 ... 7:
		    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(S_REG(es))
			+ D_16_32(S_REG(edi)),
			SEG_ADR((void *), es, di),
			S_LWORD(ecx));
		    break;
		case 0x21:
		case 0xa1:
		    MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(S_REG(ds))
			+ D_16_32(S_REG(edx)),
			SEG_ADR((void *), ds, dx),
			S_LWORD(ecx));
		    break;
		case 0x22:
		case 0xa2:
		    strcpy((void *)GetSegmentBaseAddress(S_REG(ds))
			+ D_16_32(S_REG(edx)),
			SEG_ADR((void *), ds, dx));
		    break;
	    }
	    break;
	case 0x71:		/* LFN functions */
        switch (S_LO(ax)) {
        case 0x3B:
        case 0x41:
            DPMI_CLIENT.stack_frame.edx = S_REG(edx);
            break;
        case 0x4E:
            DPMI_CLIENT.stack_frame.edx = S_REG(edx);
            /* fall thru */
        case 0x4F:
            DPMI_CLIENT.stack_frame.edi = S_REG(edi);
            if (LWORD(eflags) & CF)
                break;
            MEMCPY_DOS2DOS((void *)GetSegmentBaseAddress(S_REG(es))
                     + D_16_32(S_REG(edi)),
                     SEG_ADR((void *), es, di),
                        0x13E);
            break;
        case 0x47:
            DPMI_CLIENT.stack_frame.esi = S_REG(esi);
            if (LWORD(eflags) & CF)
                break;
	    snprintf((char *)(GetSegmentBaseAddress(S_REG(ds)) +
			D_16_32(S_REG(esi))), MAX_DOS_PATH, "%s", 
		        SEG_ADR((char *), ds, si));
            break;
	case 0x60:
	    DPMI_CLIENT.stack_frame.esi = S_REG(esi);
	    DPMI_CLIENT.stack_frame.edi = S_REG(edi);
	    if (LWORD(eflags) & CF)
		break;
	    snprintf((void *)GetSegmentBaseAddress(S_REG(es)) + 
		D_16_32(S_REG(edi)), MAX_DOS_PATH, "%s",
		SEG_ADR((char *), es, di));
	    break;
        case 0x6c:
            DPMI_CLIENT.stack_frame.esi = S_REG(esi);
            break;
        };

	default:
	    break;
	}
	break;
    case 0x25:			/* Absolute Disk Read */
    case 0x26:			/* Absolute Disk Write */
	/* the flags should be pushed to stack */
	if (DPMI_CLIENT.is_32) {
	    DPMI_CLIENT.stack_frame.esp -= 4;
	    *(unsigned long *)(GetSegmentBaseAddress(S_REG(ss)) + S_REG(esp) - 4) =
	      REG(eflags);
	} else {
	    DPMI_CLIENT.stack_frame.esp -= 2;
	    *(unsigned short *)(GetSegmentBaseAddress(S_REG(ss)) +
	      S_LWORD(esp) - 2) = LWORD(eflags);
	}
	break;
    case 0x33:			/* mouse */
	switch (MOUSE_SAVED_REGS.eax & 0xffff) {
	case 0x09:		/* Set Mouse Graphics Cursor */
	    DPMI_CLIENT.stack_frame.edx = MOUSE_SAVED_REGS.edx;
	    break;
	case 0x14:		/* swap call back */
	    DPMI_CLIENT.stack_frame.es =
                  	    ConvertSegmentToDescriptor(REG(es)); 
	    break;
	case 0x19:		/* Get User Alternate Interrupt Address */
	    DPMI_CLIENT.stack_frame.ebx =
                  	    ConvertSegmentToDescriptor(LWORD(ebx)); 
	    break;
	default:
	    break;
	}
    default:
	break;
    }
    DS_MAPPED = 0;
    ES_MAPPED = 0;
    restore_ems_frame();
}

static char decode_use_16bit;
static char use_prefix;
static  unsigned char *
decode_8e_index(struct sigcontext_struct *scp, unsigned char *prefix,
		int rm)
{
    switch (rm) {
    case 0:
	if (use_prefix)
	    return prefix + _LWORD(ebx) + _LWORD(esi);
	else
	    return (unsigned char *)(GetSegmentBaseAddress(_ds)+
				     _LWORD(ebx) + _LWORD(esi));
    case 1:
	if (use_prefix)
	    return prefix + _LWORD(ebx) + _LWORD(edi);
	else
	    return (unsigned char *)(GetSegmentBaseAddress(_ds)+
				     _LWORD(ebx) + _LWORD(edi));
    case 2:
	if (use_prefix)
	    return prefix + _LWORD(ebp) + _LWORD(esi);
	else
	    return (unsigned char *)(GetSegmentBaseAddress(_ss)+
				     _LWORD(ebp) + _LWORD(esi));
    case 3:
	if (use_prefix)
	    return prefix + _LWORD(ebp) + _LWORD(edi);
	else
	    return (unsigned char *)(GetSegmentBaseAddress(_ss)+
				     _LWORD(ebp) + _LWORD(edi));
    case 4:
	if (use_prefix)
	    return prefix + _LWORD(esi);
	else
	    return (unsigned char *)(GetSegmentBaseAddress(_ds)+
				     _LWORD(esi));
    case 5:
	if (use_prefix)
	    return prefix + _LWORD(edi);
	else
	    return (unsigned char *)(GetSegmentBaseAddress(_ds)+
				     _LWORD(edi));
    case 6:
	if (use_prefix)
	    return prefix + _LWORD(ebp);
	else
	    return (unsigned char *)(GetSegmentBaseAddress(_ss)+
				     _LWORD(ebp));
    case 7:
	if (use_prefix)
	    return prefix + _LWORD(ebx);
	else
	    return (unsigned char *)(GetSegmentBaseAddress(_ds)+
				     _LWORD(ebx));
    }
    D_printf("DPMI: decode_8e_index returns with NULL\n");
    return(NULL);
}

static unsigned char *
check_prefix (struct sigcontext_struct *scp)
{
    unsigned char *prefix, *csp;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);

    prefix = NULL;
    use_prefix = 0;
    switch (*csp) {
    case 0x2e:
	prefix = (unsigned char *)GetSegmentBaseAddress(_cs);
	use_prefix = 1;
	break;
    case 0x36:
	prefix = (unsigned char *)GetSegmentBaseAddress(_ss);
	use_prefix = 1;
	break;
    case 0x3e:
	prefix = (unsigned char *)GetSegmentBaseAddress(_ds);
	use_prefix = 1;
	break;
    case 0x26:
	prefix = (unsigned char *)GetSegmentBaseAddress(_es);
	use_prefix = 1;
	break;
    case 0x64:
	prefix = (unsigned char *)GetSegmentBaseAddress(_fs);
	use_prefix = 1;
	break;
    case 0x65:
	prefix = (unsigned char *)GetSegmentBaseAddress(_gs);
	use_prefix = 1;
	break;
    default:
	break;
    }
    if (use_prefix) {
    	D_printf("DPMI: check_prefix covered for *csp=%x\n", *csp);
    }
    return prefix;
}
/*
 * this function tries to decode opcode 0x8e (mov Sreg,m/r16), returns
 * the length of the instruction.
 */

static int
decode_8e(struct sigcontext_struct *scp, unsigned short *src,
	  unsigned  char * sreg)
{
    unsigned char *prefix, *csp;
    unsigned char mod, rm, reg;
    int len = 0;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);

    prefix = check_prefix(scp);
    if (use_prefix) {
	csp++;
	len++;
    }

    if (*csp != 0x8e)
	return 0;

    csp++;
    len += 2;
    mod = (*csp>>6) & 3;
    reg = (*csp>>3) & 7;
    rm = *csp & 0x7;

    switch (mod) {
    case 0:
	if (rm == 6) {		/* disp16 */
	    if(use_prefix)
		*src = *(unsigned short *)(prefix +
					   (int)(*(short *)(csp+1)));
	    else
		*src =  *(unsigned short *)(GetSegmentBaseAddress(_ds) +
					    (int)(*(short *)(csp+1)));
	    len += 2;
	} else
	    *src = *(unsigned short *)decode_8e_index(scp, prefix, rm);
	break;
    case 1:			/* disp8 */
	*src = *(unsigned short *)(decode_8e_index(scp, prefix, rm) +
				   (int)(*(char *)(csp+1)));
	len++;
	break;
    case 2:			/* disp16 */
	*src = *(unsigned short *)(decode_8e_index(scp, prefix, rm) +
				   (int)(*(short *)(csp+1)));
	len += 2;
	break;
    case 3:			/* register */
	switch (rm) {
	case 0:
	    *src = (unsigned short)_LWORD(eax);
	    break;
	case 1:
	    *src = (unsigned short)_LWORD(ecx);
	    break;
	case 2:
	    *src = (unsigned short)_LWORD(edx);
	    break;
	case 3:
	    *src = (unsigned short)_LWORD(ebx);
	    break;
	case 4:
	    *src = (unsigned short)_LWORD(esp);
	    break;
	case 5:
	    *src = (unsigned short)_LWORD(ebp);
	    break;
	case 6:
	    *src = (unsigned short)_LWORD(esi);
	    break;
	case 7:
	    *src = (unsigned short)_LWORD(edi);
	    break;
	}
    }

    *sreg = reg;
    return len;
}

static int
decode_load_descriptor(struct sigcontext_struct *scp, unsigned short
		       *segment, unsigned char * sreg)
{
    unsigned char *prefix, *csp;
    unsigned char mod, rm, reg;
    unsigned long *lp=NULL;
    unsigned offset;
    int len = 0;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);

    prefix = check_prefix(scp);
    if (use_prefix) {
	csp++;
	len++;
    }

    switch (*csp) {
    case 0xc5:
	*sreg = DS_INDEX;		/* LDS */
	break;
    case 0xc4:
	*sreg = ES_INDEX;		/* LES */
	break;
    default:
	return 0;
    }

    csp++;
    len += 2;
    mod = (*csp>>6) & 3;
    reg = (*csp>>3) & 7;
    rm = *csp & 0x7;

    switch (mod) {
    case 0:
	if (rm == 6) {		/* disp16 */
	    if(use_prefix)
		lp = (unsigned long *)(prefix +
					   (int)(*(short *)(csp+1)));
	    else
		lp =  (unsigned long *)(GetSegmentBaseAddress(_ds) +
					    (int)(*(short *)(csp+1)));
	    len += 2;
	} else
	    lp = (unsigned long *)decode_8e_index(scp, prefix, rm);
	break;
    case 1:			/* disp8 */
	lp = (unsigned long *)(decode_8e_index(scp, prefix, rm) +
				   (int)(*(char *)(csp+1)));
	len++;
	break;
    case 2:			/* disp16 */
	lp = (unsigned long *)(decode_8e_index(scp, prefix, rm) +
				   (int)(*(short *)(csp+1)));
	len += 2;
	break;
    case 3:			/* register */
				/* must be memory address */
	return 0;
    }

    offset = *lp & 0xffff;
    *segment = (*lp >> 16) & 0xffff;
    switch (reg) {
	case 0:
	    _LWORD(eax) = offset;
	    break;
	case 1:
	    _LWORD(ecx) = offset;
	    break;
	case 2:
	    _LWORD(edx) = offset;
	    break;
	case 3:
	    _LWORD(ebx) = offset;
	    break;
	case 4:
	    _LWORD(esp) = offset;
	    break;
	case 5:
	    _LWORD(ebp) = offset;
	    break;
	case 6:
	    _LWORD(esi) = offset;
	    break;
	case 7:
	    _LWORD(edi) = offset;
	    break;
	}
    return len;
}

static int
decode_pop_segreg(struct sigcontext_struct *scp, unsigned short
		       *segment, unsigned char * sreg)
{
    unsigned short *ssp;
    unsigned char *csp;
    int len;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);
    ssp = (unsigned short *) SEL_ADR(_ss, _esp);
    len = 0;
    switch (*csp) {
    case 0x1f:			/* pop ds */
	len = 1;
	*segment = *ssp;
	*sreg = DS_INDEX;
	break;
    case 0x07:			/* pop es */
	len = 1;
	*segment = *ssp;
	*sreg = ES_INDEX;
	break;
    case 0x17:			/* pop ss */
	len = 1;
	*segment = *ssp;
	*sreg = SS_INDEX;
	break;
    case 0x0f:		/* two byte opcode */
	csp++;
	switch (*csp) {
	case 0xa1:		/* pop fs */
	    len = 2;
	    *segment = *ssp;
	    *sreg = FS_INDEX;
	    break;
	case 0xa9:		/* pop gs */
	    len = 2;
	    *segment = *ssp;
	    *sreg = GS_INDEX;
	break;
	}
    }
    return len;
}

static int
decode_retf_iret(struct sigcontext_struct *scp, unsigned short *segment,
    unsigned char * sreg, int decode_use_16bit)
{
    unsigned short *ssp;
    unsigned char *csp;
    int len;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);
    ssp = (unsigned short *) SEL_ADR(_ss, _esp);
    len = 0;
    switch (*csp) {
	case 0xca:			/* retf imm16 */
	case 0xcb:			/* retf */
	case 0xcf:			/* iret */
	    len = 1;
	    _eip = decode_use_16bit ? ssp[0] : ((unsigned int *) ssp)[0];
	    *segment = decode_use_16bit ? ssp[1] : ((unsigned int *) ssp)[1];
	    *sreg = CS_INDEX;
	    _esp += decode_use_16bit ? 4 : 8;
	    break;
    }
    if (!len)
	return 0;

    switch (*csp) {
	case 0xca:			/* retf imm16 */
	    len += 2;
	    _esp += ((unsigned short *) (csp + 1))[0];
	    break;
	case 0xcf:			/* iret */
	    _eflags = decode_use_16bit ? ssp[2] : ((unsigned int *) ssp)[2];
	    _esp += decode_use_16bit ? 2 : 4;
	    break;
    }
    if (len)
	D_printf("DPMI: retf decoded, seg=0x%x\n", *segment);
    return len;
}

static int
decode_jmp_f(struct sigcontext_struct *scp, unsigned short *segment,
    unsigned char * sreg, int decode_use_16bit)
{
    unsigned short *ssp;
    unsigned char *csp;
    int len;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);
    ssp = (unsigned short *) SEL_ADR(_ss, _esp);
    len = 0;
    switch (*csp) {
	case 0xea:			/* jmp seg:off16/off32 */
	    len = decode_use_16bit ? 5 : 7;
	    _eip = decode_use_16bit ? ((unsigned short *)(csp + 1))[0] :
		((unsigned int *)(csp + 1))[0];
	    *segment = decode_use_16bit ? ((unsigned short *)(csp + 3))[0] :
		((unsigned short *)(csp + 5))[0];
	    *sreg = CS_INDEX;
	    break;
    }
    if (len)
	D_printf("DPMI: jmpf decoded, seg=0x%x\n", *segment);
    return len;
}

/*
 * decode_modify_segreg_insn tries to decode instructions which would modify a
 * segment register, returns the length of the insn.
 */
static  int
decode_modify_segreg_insn(struct sigcontext_struct *scp, unsigned
			  short *segment, unsigned char *sreg)
{
    unsigned char *csp;
    int len, size_prfix;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);
    size_prfix = 0;
    decode_use_16bit = !Segments[_cs>>3].is_32;
    if (*csp == 0x66) { /* Operand-Size prefix */
	csp++;
	_eip++;
	decode_use_16bit ^= 1;
	size_prfix++;
    }
	
    /* first try mov sreg, .. (equal for 16/32 bit operand size) */
    if ((len = decode_8e(scp, segment, sreg))) {
      _eip += len;
      return len + size_prfix;
    }
 
    if (decode_use_16bit) {	/*  32bit decode not implemented yet */
      /* then try lds, les ... */
      if ((len = decode_load_descriptor(scp, segment, sreg))) {
        _eip += len;
	return len+size_prfix;
      }
    }

    /* now try pop sreg */
    if ((len = decode_pop_segreg(scp, segment, sreg))) {
      _esp += decode_use_16bit ? 2 : 4;
      _eip += len;
      return len+size_prfix;
    }

    /* try retf, iret */
    if ((len = decode_retf_iret(scp, segment, sreg, decode_use_16bit))) {
      /* eip, esp and eflags are modified! */
      return len+size_prfix;
    }

    /* try far jmp */
    if ((len = decode_jmp_f(scp, segment, sreg, decode_use_16bit))) {
      /* eip is modified! */
      return len+size_prfix;
    }

    return 0;
}
	
    
#if 0 /* Not USED!  JES 96/01/2x */
static  int msdos_fix_cs_prefix (struct sigcontext_struct *scp)
{
    unsigned char *csp;

    csp = (unsigned char *) SEL_ADR(_cs, _eip);
    if (*csp != 0x2e)		/* not cs prefix */
	return 0;

    /* bcc try to something like mov cs:[xx],ax here, we cheat it by */
    /* using mov gs:[xx],ax instead, hope bcc will never use gs :=( */

    if ((Segments[_cs>>3].type & MODIFY_LDT_CONTENTS_CODE) &&
	(Segments[(_cs>>3)+1].base_addr == Segments[_cs>>3].base_addr)
	&&((Segments[(_cs>>3)+1].type & MODIFY_LDT_CONTENTS_CODE)==0)) {
	    _gs = _cs + 8;
	    *csp = 0x65;	/* gs prefix */
	    return 1;
    }
    return 0;
}
#endif


int msdos_fault(struct sigcontext_struct *scp)
{
    struct sigcontext_struct new_sct;
    unsigned char reg;
    unsigned short segment, desc;
    unsigned long len;

    D_printf("DPMI: msdos_fault, err=%#lx\n",_err);
    if ((_err & 0xffff) == 0) {	/*  not a selector error */
    /* Why should we "fix" the NULL dereferences? */
    /* Because the unmodified Win3.1 kernel (not WinOS2) needs this */
    /* Yes, but only when LDT is read-only, and then it doesn't work anyway.
     * So lets disable it again and see if someone else needs this. */
#if 0
	char fixed = 0;
	unsigned char * csp;

	csp = (unsigned char *) SEL_ADR(_cs, _eip);

	/* see if client wants to access control registers */
	if (*csp == 0x0f) {
	  if (cpu_trap_0f(csp, scp)) return 1;	/* 1=handled */
	}
	
	switch (*csp) {
	case 0x2e:		/* cs: */
	    break;		/* do nothing */
	case 0x36:		/* ss: */
	    break;		/* do nothing */
	case 0x26:		/* es: */
	    if (_es == 0) {
		D_printf("DPMI: client tries to use use gdt 0 as es\n");
		_es = ConvertSegmentToDescriptor(0);
		fixed = 1;
	    }
	    break;
	case 0x64:		/* fs: */
	    if (_fs == 0) {
		D_printf("DPMI: client tries to use use gdt 0 as fs\n");
		_fs = ConvertSegmentToDescriptor(0);
		fixed = 1;
	    }
	    break;
	case 0x65:		/* gs: */
	    if (_gs == 0) {
		D_printf("DPMI: client tries to use use gdt 0 as es\n");
		_gs = ConvertSegmentToDescriptor(0);
		fixed = 1;
	    }
	    break;
	case 0xf2:		/* REPNE prefix */
	case 0xf3:		/* REP, REPE */
	    /* this might be a string insn */
	    switch (*(csp+1)) {
	    case 0xaa: case 0xab:		/* stos */
	    case 0xae: case 0xaf:	        /* scas */
		/* only use es */
		if (_es == 0) {
		    D_printf("DPMI: client tries to use use gdt 0 as es\n");
		    _es = ConvertSegmentToDescriptor(0);
		    fixed = 1;
		}
		break;
	    case 0xa4: case 0xa5:		/* movs */
	    case 0xa6: case 0xa7:         /* cmps */
		/* use both ds and es */
		if (_es == 0) {
		    D_printf("DPMI: client tries to use use gdt 0 as es\n");
		    _es = ConvertSegmentToDescriptor(0);
		    fixed = 1;
		}
		if (_ds == 0) {
		    D_printf("DPMI: client tries to use use gdt 0 as ds\n");
		    _ds = ConvertSegmentToDescriptor(0);
		    fixed = 1;
		}
		break;
	    }
	    break;
	case 0x3e:		/* ds: */
	default:		/* assume default is using ds, but if the */
				/* client sets ss to 0, it is totally broken */
	    if (_ds == 0) {
		D_printf("DPMI: client tries to use use gdt 0 as ds\n");
		_ds = ConvertSegmentToDescriptor(0);
		fixed = 1;
	    }
	    break;
	}
	return fixed;
#else
	return 0;
#endif
    }
    
    /* now it is a invalid selector error, try to fix it if it is */
    /* caused by an instruction mov Sreg,m/r16                    */

    new_sct = *scp;
    len = decode_modify_segreg_insn(&new_sct, &segment, &reg);
    if (len == 0) 
	return 0;
    if (ValidAndUsedSelector(segment)) {
	/*
	 * The selector itself is OK, but the descriptor (type) is not.
	 * We cannot fix this! So just give up immediately and dont
	 * screw up the context.
	 */
	D_printf("DPMI: msdos_fault: Illegal use of selector %#x\n", segment);
	return 0;
    }

    D_printf("DPMI: try mov to a invalid selector 0x%04x\n", segment);

#if 0
    /* only allow using some special GTD\'s */
    if ((segment != 0x0040) && (segment != 0xa000) &&
	(segment != 0xb000) && (segment != 0xb800) &&
	(segment != 0xc000) && (segment != 0xe000) &&
	(segment != 0xf000) && (segment != 0xbf8) &&
	(segment != 0xf800) && (segment != 0xff00))
	return 0;
#endif    

    if (!(desc = (reg != CS_INDEX ? ConvertSegmentToDescriptor(segment) :
	ConvertSegmentToCodeDescriptor(segment))))
	return 0;

    /* OKay, all the sanity checks passed. Now we go and fix the selector */
    switch (reg) {
    case ES_INDEX:
	new_sct.es = desc;
	break;
    case CS_INDEX:
	new_sct.cs = desc;
	break;
    case SS_INDEX:
	new_sct.ss = desc;
	break;
    case DS_INDEX:
	new_sct.ds = desc;
	break;
    case FS_INDEX:
	new_sct.fs = desc;
	break;
    case GS_INDEX:
	new_sct.gs = desc;
	break;
    default :
	/* Cannot be here */
	error("DPMI: Invalid segreg %#x\n", reg);
	return 0;
    }

    /* lets hope we fixed the thing, apply the "fix" to context and return */
    *scp = new_sct;
    return 1;
}
