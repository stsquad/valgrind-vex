
/*---------------------------------------------------------------*/
/*--- begin                               guest_arm_helpers.c ---*/
/*---------------------------------------------------------------*/

/*
   This file is part of Valgrind, a dynamic binary instrumentation
   framework.

   Copyright (C) 2004-2011 OpenWorks LLP
      info@open-works.net

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#include "libvex_basictypes.h"
#include "libvex_emwarn.h"
#include "libvex_guest_arm.h"
#include "libvex_ir.h"
#include "libvex.h"

#include "main_util.h"
#include "guest_generic_bb_to_IR.h"
#include "guest_arm_defs.h"


/* This file contains helper functions for arm guest code.  Calls to
   these functions are generated by the back end.  These calls are of
   course in the host machine code and this file will be compiled to
   host machine code, so that all makes sense.

   Only change the signatures of these helper functions very
   carefully.  If you change the signature here, you'll have to change
   the parameters passed to it in the IR calls constructed by
   guest-arm/toIR.c.
*/


/* Set to 1 to get detailed profiling info about individual N, Z, C
   and V flag evaluation. */
#define PROFILE_NZCV_FLAGS 0

#if PROFILE_NZCV_FLAGS

static UInt tab_n_eval[ARMG_CC_OP_NUMBER];
static UInt tab_z_eval[ARMG_CC_OP_NUMBER];
static UInt tab_c_eval[ARMG_CC_OP_NUMBER];
static UInt tab_v_eval[ARMG_CC_OP_NUMBER];
static UInt initted = 0;
static UInt tot_evals = 0;

static void initCounts ( void )
{
   UInt i;
   for (i = 0; i < ARMG_CC_OP_NUMBER; i++) {
      tab_n_eval[i] = tab_z_eval[i] = tab_c_eval[i] = tab_v_eval[i] = 0;
   }
   initted = 1;
}

static void showCounts ( void )
{
   UInt i;
   vex_printf("\n                 N          Z          C          V\n");
   vex_printf(  "---------------------------------------------------\n");
   for (i = 0; i < ARMG_CC_OP_NUMBER; i++) {
      vex_printf("CC_OP=%d  %9d  %9d  %9d  %9d\n",
                 i,
                 tab_n_eval[i], tab_z_eval[i],
                 tab_c_eval[i], tab_v_eval[i] );
    }
}

#define NOTE_N_EVAL(_cc_op) NOTE_EVAL(_cc_op, tab_n_eval)
#define NOTE_Z_EVAL(_cc_op) NOTE_EVAL(_cc_op, tab_z_eval)
#define NOTE_C_EVAL(_cc_op) NOTE_EVAL(_cc_op, tab_c_eval)
#define NOTE_V_EVAL(_cc_op) NOTE_EVAL(_cc_op, tab_v_eval)

#define NOTE_EVAL(_cc_op, _tab) \
   do { \
      if (!initted) initCounts(); \
      vassert( ((UInt)(_cc_op)) < ARMG_CC_OP_NUMBER); \
      _tab[(UInt)(_cc_op)]++; \
      tot_evals++; \
      if (0 == (tot_evals & 0xFFFFF)) \
        showCounts(); \
   } while (0)

#endif /* PROFILE_NZCV_FLAGS */


/* Calculate the N flag from the supplied thunk components, in the
   least significant bit of the word.  Returned bits 31:1 are zero. */
static
UInt armg_calculate_flag_n ( UInt cc_op, UInt cc_dep1,
                             UInt cc_dep2, UInt cc_dep3 )
{
#  if PROFILE_NZCV_FLAGS
   NOTE_N_EVAL(cc_op);
#  endif

   switch (cc_op) {
      case ARMG_CC_OP_COPY: {
         /* (nzcv:28x0, unused, unused) */
         UInt nf   = (cc_dep1 >> ARMG_CC_SHIFT_N) & 1;
         return nf;
      }
      case ARMG_CC_OP_ADD: {
         /* (argL, argR, unused) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt res  = argL + argR;
         UInt nf   = res >> 31;
         return nf;
      }
      case ARMG_CC_OP_SUB: {
         /* (argL, argR, unused) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt res  = argL - argR;
         UInt nf   = res >> 31;
         return nf;
      }
      case ARMG_CC_OP_ADC: {
         /* (argL, argR, oldC) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt oldC = cc_dep3;
         vassert((oldC & ~1) == 0);
         UInt res  = argL + argR + oldC;
         UInt nf   = res >> 31;
         return nf;
      }
      case ARMG_CC_OP_SBB: {
         /* (argL, argR, oldC) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt oldC = cc_dep3;
         vassert((oldC & ~1) == 0);
         UInt res  = argL - argR - (oldC ^ 1);
         UInt nf   = res >> 31;
         return nf;
      }
      case ARMG_CC_OP_LOGIC: {
         /* (res, shco, oldV) */
         UInt res  = cc_dep1;
         UInt nf   = res >> 31;
         return nf;
      }
      case ARMG_CC_OP_MUL: {
         /* (res, unused, oldC:oldV) */
         UInt res  = cc_dep1;
         UInt nf   = res >> 31;
         return nf;
      }
      case ARMG_CC_OP_MULL: {
         /* (resLo32, resHi32, oldC:oldV) */
         UInt resHi32 = cc_dep2;
         UInt nf      = resHi32 >> 31;
         return nf;
      }
      default:
         /* shouldn't really make these calls from generated code */
         vex_printf("armg_calculate_flag_n"
                    "( op=%u, dep1=0x%x, dep2=0x%x, dep3=0x%x )\n",
                    cc_op, cc_dep1, cc_dep2, cc_dep3 );
         vpanic("armg_calculate_flags_n");
   }
}


/* Calculate the Z flag from the supplied thunk components, in the
   least significant bit of the word.  Returned bits 31:1 are zero. */
static
UInt armg_calculate_flag_z ( UInt cc_op, UInt cc_dep1,
                             UInt cc_dep2, UInt cc_dep3 )
{
#  if PROFILE_NZCV_FLAGS
   NOTE_Z_EVAL(cc_op);
#  endif

   switch (cc_op) {
      case ARMG_CC_OP_COPY: {
         /* (nzcv:28x0, unused, unused) */
         UInt zf   = (cc_dep1 >> ARMG_CC_SHIFT_Z) & 1;
         return zf;
      }
      case ARMG_CC_OP_ADD: {
         /* (argL, argR, unused) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt res  = argL + argR;
         UInt zf   = res == 0;
         return zf;
      }
      case ARMG_CC_OP_SUB: {
         /* (argL, argR, unused) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt res  = argL - argR;
         UInt zf   = res == 0;
         return zf;
      }
      case ARMG_CC_OP_ADC: {
         /* (argL, argR, oldC) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt oldC = cc_dep3;
         vassert((oldC & ~1) == 0);
         UInt res  = argL + argR + oldC;
         UInt zf   = res == 0;
         return zf;
      }
      case ARMG_CC_OP_SBB: {
         /* (argL, argR, oldC) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt oldC = cc_dep3;
         vassert((oldC & ~1) == 0);
         UInt res  = argL - argR - (oldC ^ 1);
         UInt zf   = res == 0;
         return zf;
      }
      case ARMG_CC_OP_LOGIC: {
         /* (res, shco, oldV) */
         UInt res  = cc_dep1;
         UInt zf   = res == 0;
         return zf;
      }
      case ARMG_CC_OP_MUL: {
         /* (res, unused, oldC:oldV) */
         UInt res  = cc_dep1;
         UInt zf   = res == 0;
         return zf;
      }
      case ARMG_CC_OP_MULL: {
         /* (resLo32, resHi32, oldC:oldV) */
         UInt resLo32 = cc_dep1;
         UInt resHi32 = cc_dep2;
         UInt zf      = (resHi32|resLo32) == 0;
         return zf;
      }
      default:
         /* shouldn't really make these calls from generated code */
         vex_printf("armg_calculate_flags_z"
                    "( op=%u, dep1=0x%x, dep2=0x%x, dep3=0x%x )\n",
                    cc_op, cc_dep1, cc_dep2, cc_dep3 );
         vpanic("armg_calculate_flags_z");
   }
}


/* CALLED FROM GENERATED CODE: CLEAN HELPER */
/* Calculate the C flag from the supplied thunk components, in the
   least significant bit of the word.  Returned bits 31:1 are zero. */
UInt armg_calculate_flag_c ( UInt cc_op, UInt cc_dep1,
                             UInt cc_dep2, UInt cc_dep3 )
{
#  if PROFILE_NZCV_FLAGS
   NOTE_C_EVAL(cc_op);
#  endif

   switch (cc_op) {
      case ARMG_CC_OP_COPY: {
         /* (nzcv:28x0, unused, unused) */
         UInt cf   = (cc_dep1 >> ARMG_CC_SHIFT_C) & 1;
         return cf;
      }
      case ARMG_CC_OP_ADD: {
         /* (argL, argR, unused) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt res  = argL + argR;
         UInt cf   = res < argL;
         return cf;
      }
      case ARMG_CC_OP_SUB: {
         /* (argL, argR, unused) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt cf   = argL >= argR;
         return cf;
      }
      case ARMG_CC_OP_ADC: {
         /* (argL, argR, oldC) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt oldC = cc_dep3;
         vassert((oldC & ~1) == 0);
         UInt res  = argL + argR + oldC;
         UInt cf   = oldC ? (res <= argL) : (res < argL);
         return cf;
      }
      case ARMG_CC_OP_SBB: {
         /* (argL, argR, oldC) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt oldC = cc_dep3;
         vassert((oldC & ~1) == 0);
         UInt cf   = oldC ? (argL >= argR) : (argL > argR);
         return cf;
      }
      case ARMG_CC_OP_LOGIC: {
         /* (res, shco, oldV) */
         UInt shco = cc_dep2;
         vassert((shco & ~1) == 0);
         UInt cf   = shco;
         return cf;
      }
      case ARMG_CC_OP_MUL: {
         /* (res, unused, oldC:oldV) */
         UInt oldC = (cc_dep3 >> 1) & 1;
         vassert((cc_dep3 & ~3) == 0);
         UInt cf   = oldC;
         return cf;
      }
      case ARMG_CC_OP_MULL: {
         /* (resLo32, resHi32, oldC:oldV) */
         UInt oldC    = (cc_dep3 >> 1) & 1;
         vassert((cc_dep3 & ~3) == 0);
         UInt cf      = oldC;
         return cf;
      }
      default:
         /* shouldn't really make these calls from generated code */
         vex_printf("armg_calculate_flag_c"
                    "( op=%u, dep1=0x%x, dep2=0x%x, dep3=0x%x )\n",
                    cc_op, cc_dep1, cc_dep2, cc_dep3 );
         vpanic("armg_calculate_flag_c");
   }
}


/* CALLED FROM GENERATED CODE: CLEAN HELPER */
/* Calculate the V flag from the supplied thunk components, in the
   least significant bit of the word.  Returned bits 31:1 are zero. */
UInt armg_calculate_flag_v ( UInt cc_op, UInt cc_dep1,
                             UInt cc_dep2, UInt cc_dep3 )
{
#  if PROFILE_NZCV_FLAGS
   NOTE_V_EVAL(cc_op);
#  endif

   switch (cc_op) {
      case ARMG_CC_OP_COPY: {
         /* (nzcv:28x0, unused, unused) */
         UInt vf   = (cc_dep1 >> ARMG_CC_SHIFT_V) & 1;
         return vf;
      }
      case ARMG_CC_OP_ADD: {
         /* (argL, argR, unused) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt res  = argL + argR;
         UInt vf   = ((res ^ argL) & (res ^ argR)) >> 31;
         return vf;
      }
      case ARMG_CC_OP_SUB: {
         /* (argL, argR, unused) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt res  = argL - argR;
         UInt vf   = ((argL ^ argR) & (argL ^ res)) >> 31;
         return vf;
      }
      case ARMG_CC_OP_ADC: {
         /* (argL, argR, oldC) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt oldC = cc_dep3;
         vassert((oldC & ~1) == 0);
         UInt res  = argL + argR + oldC;
         UInt vf   = ((res ^ argL) & (res ^ argR)) >> 31;
         return vf;
      }
      case ARMG_CC_OP_SBB: {
         /* (argL, argR, oldC) */
         UInt argL = cc_dep1;
         UInt argR = cc_dep2;
         UInt oldC = cc_dep3;
         vassert((oldC & ~1) == 0);
         UInt res  = argL - argR - (oldC ^ 1);
         UInt vf   = ((argL ^ argR) & (argL ^ res)) >> 31;
         return vf;
      }
      case ARMG_CC_OP_LOGIC: {
         /* (res, shco, oldV) */
         UInt oldV = cc_dep3;
         vassert((oldV & ~1) == 0);
         UInt vf   = oldV;
         return vf;
      }
      case ARMG_CC_OP_MUL: {
         /* (res, unused, oldC:oldV) */
         UInt oldV = (cc_dep3 >> 0) & 1;
         vassert((cc_dep3 & ~3) == 0);
         UInt vf   = oldV;
         return vf;
      }
      case ARMG_CC_OP_MULL: {
         /* (resLo32, resHi32, oldC:oldV) */
         UInt oldV    = (cc_dep3 >> 0) & 1;
         vassert((cc_dep3 & ~3) == 0);
         UInt vf      = oldV;
         return vf;
      }
      default:
         /* shouldn't really make these calls from generated code */
         vex_printf("armg_calculate_flag_v"
                    "( op=%u, dep1=0x%x, dep2=0x%x, dep3=0x%x )\n",
                    cc_op, cc_dep1, cc_dep2, cc_dep3 );
         vpanic("armg_calculate_flag_v");
   }
}


/* CALLED FROM GENERATED CODE: CLEAN HELPER */
/* Calculate NZCV from the supplied thunk components, in the positions
   they appear in the CPSR, viz bits 31:28 for N Z C V respectively.
   Returned bits 27:0 are zero. */
UInt armg_calculate_flags_nzcv ( UInt cc_op, UInt cc_dep1,
                                 UInt cc_dep2, UInt cc_dep3 )
{
   UInt f;
   UInt res = 0;
   f = armg_calculate_flag_n(cc_op, cc_dep1, cc_dep2, cc_dep3);
   res |= (f << ARMG_CC_SHIFT_N);
   f = armg_calculate_flag_z(cc_op, cc_dep1, cc_dep2, cc_dep3);
   res |= (f << ARMG_CC_SHIFT_Z);
   f = armg_calculate_flag_c(cc_op, cc_dep1, cc_dep2, cc_dep3);
   res |= (f << ARMG_CC_SHIFT_C);
   f = armg_calculate_flag_v(cc_op, cc_dep1, cc_dep2, cc_dep3);
   res |= (f << ARMG_CC_SHIFT_V);
   return res;
}


/* CALLED FROM GENERATED CODE: CLEAN HELPER */
/* Calculate the QC flag from the arguments, in the lowest bit
   of the word (bit 0).  Urr, having this out of line is bizarre.
   Push back inline. */
UInt armg_calculate_flag_qc ( UInt resL1, UInt resL2,
                              UInt resR1, UInt resR2 )
{
   if (resL1 != resR1 || resL2 != resR2)
      return 1;
   else
      return 0;
}

/* CALLED FROM GENERATED CODE: CLEAN HELPER */
/* Calculate the specified condition from the thunk components, in the
   lowest bit of the word (bit 0).  Returned bits 31:1 are zero. */
UInt armg_calculate_condition ( UInt cond_n_op /* (ARMCondcode << 4) | cc_op */,
                                UInt cc_dep1,
                                UInt cc_dep2, UInt cc_dep3 )
{
   UInt cond  = cond_n_op >> 4;
   UInt cc_op = cond_n_op & 0xF;
   UInt nf, zf, vf, cf, inv;
   //   vex_printf("XXXXXXXX %x %x %x %x\n", 
   //              cond_n_op, cc_dep1, cc_dep2, cc_dep3);

   // skip flags computation in this case
   if (cond == ARMCondAL) return 1;

   inv  = cond & 1;

   switch (cond) {
      case ARMCondEQ:    // Z=1         => z
      case ARMCondNE:    // Z=0
         zf = armg_calculate_flag_z(cc_op, cc_dep1, cc_dep2, cc_dep3);
         return inv ^ zf;

      case ARMCondHS:    // C=1         => c
      case ARMCondLO:    // C=0
         cf = armg_calculate_flag_c(cc_op, cc_dep1, cc_dep2, cc_dep3);
         return inv ^ cf;

      case ARMCondMI:    // N=1         => n
      case ARMCondPL:    // N=0
         nf = armg_calculate_flag_n(cc_op, cc_dep1, cc_dep2, cc_dep3);
         return inv ^ nf;

      case ARMCondVS:    // V=1         => v
      case ARMCondVC:    // V=0
         vf = armg_calculate_flag_v(cc_op, cc_dep1, cc_dep2, cc_dep3);
         return inv ^ vf;

      case ARMCondHI:    // C=1 && Z=0   => c & ~z
      case ARMCondLS:    // C=0 || Z=1
         cf = armg_calculate_flag_c(cc_op, cc_dep1, cc_dep2, cc_dep3);
         zf = armg_calculate_flag_z(cc_op, cc_dep1, cc_dep2, cc_dep3);
         return inv ^ (cf & ~zf);

      case ARMCondGE:    // N=V          => ~(n^v)
      case ARMCondLT:    // N!=V
         nf = armg_calculate_flag_n(cc_op, cc_dep1, cc_dep2, cc_dep3);
         vf = armg_calculate_flag_v(cc_op, cc_dep1, cc_dep2, cc_dep3);
         return inv ^ (1 & ~(nf ^ vf));

      case ARMCondGT:    // Z=0 && N=V   => ~z & ~(n^v)  =>  ~(z | (n^v))
      case ARMCondLE:    // Z=1 || N!=V
         nf = armg_calculate_flag_n(cc_op, cc_dep1, cc_dep2, cc_dep3);
         vf = armg_calculate_flag_v(cc_op, cc_dep1, cc_dep2, cc_dep3);
         zf = armg_calculate_flag_z(cc_op, cc_dep1, cc_dep2, cc_dep3);
         return inv ^ (1 & ~(zf | (nf ^ vf)));

      case ARMCondAL: // handled above
      case ARMCondNV: // should never get here: Illegal instr
      default:
         /* shouldn't really make these calls from generated code */
         vex_printf("armg_calculate_condition(ARM)"
                    "( %u, %u, 0x%x, 0x%x, 0x%x )\n",
                    cond, cc_op, cc_dep1, cc_dep2, cc_dep3 );
         vpanic("armg_calculate_condition(ARM)");
   }
}


/*---------------------------------------------------------------*/
/*--- Flag-helpers translation-time function specialisers.    ---*/
/*--- These help iropt specialise calls the above run-time    ---*/
/*--- flags functions.                                        ---*/
/*---------------------------------------------------------------*/

/* Used by the optimiser to try specialisations.  Returns an
   equivalent expression, or NULL if none. */

static Bool isU32 ( IRExpr* e, UInt n )
{
   return
      toBool( e->tag == Iex_Const
              && e->Iex.Const.con->tag == Ico_U32
              && e->Iex.Const.con->Ico.U32 == n );
}

IRExpr* guest_arm_spechelper ( HChar*   function_name,
                               IRExpr** args,
                               IRStmt** precedingStmts,
                               Int      n_precedingStmts )
{
#  define unop(_op,_a1) IRExpr_Unop((_op),(_a1))
#  define binop(_op,_a1,_a2) IRExpr_Binop((_op),(_a1),(_a2))
#  define mkU32(_n) IRExpr_Const(IRConst_U32(_n))
#  define mkU8(_n)  IRExpr_Const(IRConst_U8(_n))

   Int i, arity = 0;
   for (i = 0; args[i]; i++)
      arity++;
#  if 0
   vex_printf("spec request:\n");
   vex_printf("   %s  ", function_name);
   for (i = 0; i < arity; i++) {
      vex_printf("  ");
      ppIRExpr(args[i]);
   }
   vex_printf("\n");
#  endif

   /* --------- specialising "armg_calculate_condition" --------- */

   if (vex_streq(function_name, "armg_calculate_condition")) {

      /* specialise calls to the "armg_calculate_condition" function.
         Not sure whether this is strictly necessary, but: the
         replacement IR must produce only the values 0 or 1.  Bits
         31:1 are required to be zero. */
      IRExpr *cond_n_op, *cc_dep1, *cc_dep2, *cc_ndep;
      vassert(arity == 4);
      cond_n_op = args[0]; /* (ARMCondcode << 4)  |  ARMG_CC_OP_* */
      cc_dep1   = args[1];
      cc_dep2   = args[2];
      cc_ndep   = args[3];

      /*---------------- SUB ----------------*/

      if (isU32(cond_n_op, (ARMCondEQ << 4) | ARMG_CC_OP_SUB)) {
         /* EQ after SUB --> test argL == argR */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpEQ32, cc_dep1, cc_dep2));
      }
      if (isU32(cond_n_op, (ARMCondNE << 4) | ARMG_CC_OP_SUB)) {
         /* NE after SUB --> test argL != argR */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpNE32, cc_dep1, cc_dep2));
      }

      if (isU32(cond_n_op, (ARMCondGT << 4) | ARMG_CC_OP_SUB)) {
         /* GT after SUB --> test argL >s argR
                         --> test argR <s argL */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpLT32S, cc_dep2, cc_dep1));
      }
      if (isU32(cond_n_op, (ARMCondLE << 4) | ARMG_CC_OP_SUB)) {
         /* LE after SUB --> test argL <=s argR */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpLE32S, cc_dep1, cc_dep2));
      }

      if (isU32(cond_n_op, (ARMCondLT << 4) | ARMG_CC_OP_SUB)) {
         /* LT after SUB --> test argL <s argR */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpLT32S, cc_dep1, cc_dep2));
      }

      if (isU32(cond_n_op, (ARMCondGE << 4) | ARMG_CC_OP_SUB)) {
         /* GE after SUB --> test argL >=s argR
                         --> test argR <=s argL */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpLE32S, cc_dep2, cc_dep1));
      }

      if (isU32(cond_n_op, (ARMCondHS << 4) | ARMG_CC_OP_SUB)) {
         /* HS after SUB --> test argL >=u argR
                         --> test argR <=u argL */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpLE32U, cc_dep2, cc_dep1));
      }
      if (isU32(cond_n_op, (ARMCondLO << 4) | ARMG_CC_OP_SUB)) {
         /* LO after SUB --> test argL <u argR */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpLT32U, cc_dep1, cc_dep2));
      }

      if (isU32(cond_n_op, (ARMCondLS << 4) | ARMG_CC_OP_SUB)) {
         /* LS after SUB --> test argL <=u argR */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpLE32U, cc_dep1, cc_dep2));
      }
      if (isU32(cond_n_op, (ARMCondHI << 4) | ARMG_CC_OP_SUB)) {
         /* HI after SUB --> test argL >u argR
                         --> test argR <u argL */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpLT32U, cc_dep2, cc_dep1));
      }

      /*---------------- SBB ----------------*/

      if (isU32(cond_n_op, (ARMCondHS << 4) | ARMG_CC_OP_SBB)) {
         /* This seems to happen a lot in softfloat code, eg __divdf3+140 */
         /* thunk is: (dep1=argL, dep2=argR, ndep=oldC) */
         /* HS after SBB (same as C after SBB below)
            --> oldC ? (argL >=u argR) : (argL >u argR)
            --> oldC ? (argR <=u argL) : (argR <u argL)
         */
         return
            IRExpr_Mux0X(
               unop(Iop_32to8, cc_ndep),
               /* case oldC == 0 */
               unop(Iop_1Uto32, binop(Iop_CmpLT32U, cc_dep2, cc_dep1)),
               /* case oldC != 0 */
               unop(Iop_1Uto32, binop(Iop_CmpLE32U, cc_dep2, cc_dep1))
            );
      }

      /*---------------- LOGIC ----------------*/

      if (isU32(cond_n_op, (ARMCondEQ << 4) | ARMG_CC_OP_LOGIC)) {
         /* EQ after LOGIC --> test res == 0 */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpEQ32, cc_dep1, mkU32(0)));
      }
      if (isU32(cond_n_op, (ARMCondNE << 4) | ARMG_CC_OP_LOGIC)) {
         /* NE after LOGIC --> test res != 0 */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpNE32, cc_dep1, mkU32(0)));
      }

      if (isU32(cond_n_op, (ARMCondPL << 4) | ARMG_CC_OP_LOGIC)) {
         /* PL after LOGIC --> test (res >> 31) == 0 */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpEQ32,
                           binop(Iop_Shr32, cc_dep1, mkU8(31)),
                           mkU32(0)));
      }
      if (isU32(cond_n_op, (ARMCondMI << 4) | ARMG_CC_OP_LOGIC)) {
         /* MI after LOGIC --> test (res >> 31) == 1 */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpEQ32,
                           binop(Iop_Shr32, cc_dep1, mkU8(31)),
                           mkU32(1)));
      }

      /*----------------- AL -----------------*/

      /* A critically important case for Thumb code.

         What we're trying to spot is the case where cond_n_op is an
         expression of the form Or32(..., 0xE0) since that means the
         caller is asking for CondAL and we can simply return 1
         without caring what the ... part is.  This is a potentially
         dodgy kludge in that it assumes that the ... part has zeroes
         in bits 7:4, so that the result of the Or32 is guaranteed to
         be 0xE in bits 7:4.  Given that the places where this first
         arg are constructed (in guest_arm_toIR.c) are very
         constrained, we can get away with this.  To make this
         guaranteed safe would require to have a new primop, Slice44
         or some such, thusly

         Slice44(arg1, arg2) = 0--(24)--0 arg1[7:4] arg2[3:0]

         and we would then look for Slice44(0xE0, ...)
         which would give the required safety property.

         It would be infeasibly expensive to scan backwards through
         the entire block looking for an assignment to the temp, so
         just look at the previous 16 statements.  That should find it
         if it is an interesting case, as a result of how the
         boilerplate guff at the start of each Thumb insn translation
         is made.
      */
      if (cond_n_op->tag == Iex_RdTmp) {
         Int    j;
         IRTemp look_for = cond_n_op->Iex.RdTmp.tmp;
         Int    limit    = n_precedingStmts - 16;
         if (limit < 0) limit = 0;
         if (0) vex_printf("scanning %d .. %d\n", n_precedingStmts-1, limit);
         for (j = n_precedingStmts - 1; j >= limit; j--) {
            IRStmt* st = precedingStmts[j];
            if (st->tag == Ist_WrTmp
                && st->Ist.WrTmp.tmp == look_for
                && st->Ist.WrTmp.data->tag == Iex_Binop
                && st->Ist.WrTmp.data->Iex.Binop.op == Iop_Or32
                && isU32(st->Ist.WrTmp.data->Iex.Binop.arg2, (ARMCondAL << 4)))
               return mkU32(1);
         }
         /* Didn't find any useful binding to the first arg
            in the previous 16 stmts. */
      }
   }

   /* --------- specialising "armg_calculate_flag_c" --------- */

   else
   if (vex_streq(function_name, "armg_calculate_flag_c")) {

      /* specialise calls to the "armg_calculate_flag_c" function.
         Note that the returned value must be either 0 or 1; nonzero
         bits 31:1 are not allowed.  In turn, incoming oldV and oldC
         values (from the thunk) are assumed to have bits 31:1
         clear. */
      IRExpr *cc_op, *cc_dep1, *cc_dep2, *cc_ndep;
      vassert(arity == 4);
      cc_op   = args[0]; /* ARMG_CC_OP_* */
      cc_dep1 = args[1];
      cc_dep2 = args[2];
      cc_ndep = args[3];

      if (isU32(cc_op, ARMG_CC_OP_LOGIC)) {
         /* Thunk args are (result, shco, oldV) */
         /* C after LOGIC --> shco */
         return cc_dep2;
      }

      if (isU32(cc_op, ARMG_CC_OP_SUB)) {
         /* Thunk args are (argL, argR, unused) */
         /* C after SUB --> argL >=u argR
                        --> argR <=u argL */
         return unop(Iop_1Uto32,
                     binop(Iop_CmpLE32U, cc_dep2, cc_dep1));
      }

      if (isU32(cc_op, ARMG_CC_OP_SBB)) {
         /* This happens occasionally in softfloat code, eg __divdf3+140 */
         /* thunk is: (dep1=argL, dep2=argR, ndep=oldC) */
         /* C after SBB (same as HS after SBB above)
            --> oldC ? (argL >=u argR) : (argL >u argR)
            --> oldC ? (argR <=u argL) : (argR <u argL)
         */
         return
            IRExpr_Mux0X(
               unop(Iop_32to8, cc_ndep),
               /* case oldC == 0 */
               unop(Iop_1Uto32, binop(Iop_CmpLT32U, cc_dep2, cc_dep1)),
               /* case oldC != 0 */
               unop(Iop_1Uto32, binop(Iop_CmpLE32U, cc_dep2, cc_dep1))
            );
      }

   }

   /* --------- specialising "armg_calculate_flag_v" --------- */

   else
   if (vex_streq(function_name, "armg_calculate_flag_v")) {

      /* specialise calls to the "armg_calculate_flag_v" function.
         Note that the returned value must be either 0 or 1; nonzero
         bits 31:1 are not allowed.  In turn, incoming oldV and oldC
         values (from the thunk) are assumed to have bits 31:1
         clear. */
      IRExpr *cc_op, *cc_dep1, *cc_dep2, *cc_ndep;
      vassert(arity == 4);
      cc_op   = args[0]; /* ARMG_CC_OP_* */
      cc_dep1 = args[1];
      cc_dep2 = args[2];
      cc_ndep = args[3];

      if (isU32(cc_op, ARMG_CC_OP_LOGIC)) {
         /* Thunk args are (result, shco, oldV) */
         /* V after LOGIC --> oldV */
         return cc_ndep;
      }

      if (isU32(cc_op, ARMG_CC_OP_SUB)) {
         /* Thunk args are (argL, argR, unused) */
         /* V after SUB 
            --> let res = argL - argR
                in ((argL ^ argR) & (argL ^ res)) >> 31
            --> ((argL ^ argR) & (argL ^ (argL - argR))) >> 31
         */
         IRExpr* argL = cc_dep1;
         IRExpr* argR = cc_dep2;
         return
            binop(Iop_Shr32,
                  binop(Iop_And32,
                        binop(Iop_Xor32, argL, argR),
                        binop(Iop_Xor32, argL, binop(Iop_Sub32, argL, argR))
                  ),
                  mkU8(31)
            );
      }

      if (isU32(cc_op, ARMG_CC_OP_SBB)) {
         /* This happens occasionally in softfloat code, eg __divdf3+140 */
         /* thunk is: (dep1=argL, dep2=argR, ndep=oldC) */
         /* V after SBB
            --> let res = argL - argR - (oldC ^ 1)
                in  (argL ^ argR) & (argL ^ res) & 1
         */
         return
            binop(
               Iop_And32,
               binop(
                  Iop_And32,
                  // argL ^ argR
                  binop(Iop_Xor32, cc_dep1, cc_dep2),
                  // argL ^ (argL - argR - (oldC ^ 1))
                  binop(Iop_Xor32,
                        cc_dep1,
                        binop(Iop_Sub32,
                              binop(Iop_Sub32, cc_dep1, cc_dep2),
                              binop(Iop_Xor32, cc_ndep, mkU32(1)))
                  )
               ),
               mkU32(1)
            );
      }

   }

#  undef unop
#  undef binop
#  undef mkU32
#  undef mkU8

   return NULL;
}


/*----------------------------------------------*/
/*--- The exported fns ..                    ---*/
/*----------------------------------------------*/

/* VISIBLE TO LIBVEX CLIENT */
#if 0
void LibVEX_GuestARM_put_flags ( UInt flags_native,
                                 /*OUT*/VexGuestARMState* vex_state )
{
   vassert(0); // FIXME

   /* Mask out everything except N Z V C. */
   flags_native
      &= (ARMG_CC_MASK_N | ARMG_CC_MASK_Z | ARMG_CC_MASK_V | ARMG_CC_MASK_C);
   
   vex_state->guest_CC_OP   = ARMG_CC_OP_COPY;
   vex_state->guest_CC_DEP1 = flags_native;
   vex_state->guest_CC_DEP2 = 0;
   vex_state->guest_CC_NDEP = 0;
}
#endif

/* VISIBLE TO LIBVEX CLIENT */
UInt LibVEX_GuestARM_get_cpsr ( /*IN*/VexGuestARMState* vex_state )
{
   UInt cpsr = 0;
   // NZCV
   cpsr |= armg_calculate_flags_nzcv(
               vex_state->guest_CC_OP,
               vex_state->guest_CC_DEP1,
               vex_state->guest_CC_DEP2,
               vex_state->guest_CC_NDEP
            );
   vassert(0 == (cpsr & 0x0FFFFFFF));
   // Q
   if (vex_state->guest_QFLAG32 > 0)
      cpsr |= (1 << 27);
   // GE
   if (vex_state->guest_GEFLAG0 > 0)
      cpsr |= (1 << 16);
   if (vex_state->guest_GEFLAG1 > 0)
      cpsr |= (1 << 17);
   if (vex_state->guest_GEFLAG2 > 0)
      cpsr |= (1 << 18);
   if (vex_state->guest_GEFLAG3 > 0)
      cpsr |= (1 << 19);
   // M
   cpsr |= (1 << 4); // 0b10000 means user-mode
   // J,T   J (bit 24) is zero by initialisation above
   // T  we copy from R15T[0]
   if (vex_state->guest_R15T & 1)
      cpsr |= (1 << 5);
   // ITSTATE we punt on for the time being.  Could compute it
   // if needed though.
   // E, endianness, 0 (littleendian) from initialisation above
   // A,I,F disable some async exceptions.  Not sure about these.
   // Leave as zero for the time being.
   return cpsr;
}

/* VISIBLE TO LIBVEX CLIENT */
void LibVEX_GuestARM_initialise ( /*OUT*/VexGuestARMState* vex_state )
{
   vex_state->host_EvC_FAILADDR = 0;
   vex_state->host_EvC_COUNTER = 0;

   vex_state->guest_R0  = 0;
   vex_state->guest_R1  = 0;
   vex_state->guest_R2  = 0;
   vex_state->guest_R3  = 0;
   vex_state->guest_R4  = 0;
   vex_state->guest_R5  = 0;
   vex_state->guest_R6  = 0;
   vex_state->guest_R7  = 0;
   vex_state->guest_R8  = 0;
   vex_state->guest_R9  = 0;
   vex_state->guest_R10 = 0;
   vex_state->guest_R11 = 0;
   vex_state->guest_R12 = 0;
   vex_state->guest_R13 = 0;
   vex_state->guest_R14 = 0;
   vex_state->guest_R15T = 0;  /* NB: implies ARM mode */

   vex_state->guest_CC_OP   = ARMG_CC_OP_COPY;
   vex_state->guest_CC_DEP1 = 0;
   vex_state->guest_CC_DEP2 = 0;
   vex_state->guest_CC_NDEP = 0;
   vex_state->guest_QFLAG32 = 0;
   vex_state->guest_GEFLAG0 = 0;
   vex_state->guest_GEFLAG1 = 0;
   vex_state->guest_GEFLAG2 = 0;
   vex_state->guest_GEFLAG3 = 0;

   vex_state->guest_EMWARN  = 0;
   vex_state->guest_TISTART = 0;
   vex_state->guest_TILEN   = 0;
   vex_state->guest_NRADDR  = 0;
   vex_state->guest_IP_AT_SYSCALL = 0;

   vex_state->guest_D0  = 0;
   vex_state->guest_D1  = 0;
   vex_state->guest_D2  = 0;
   vex_state->guest_D3  = 0;
   vex_state->guest_D4  = 0;
   vex_state->guest_D5  = 0;
   vex_state->guest_D6  = 0;
   vex_state->guest_D7  = 0;
   vex_state->guest_D8  = 0;
   vex_state->guest_D9  = 0;
   vex_state->guest_D10 = 0;
   vex_state->guest_D11 = 0;
   vex_state->guest_D12 = 0;
   vex_state->guest_D13 = 0;
   vex_state->guest_D14 = 0;
   vex_state->guest_D15 = 0;
   vex_state->guest_D16 = 0;
   vex_state->guest_D17 = 0;
   vex_state->guest_D18 = 0;
   vex_state->guest_D19 = 0;
   vex_state->guest_D20 = 0;
   vex_state->guest_D21 = 0;
   vex_state->guest_D22 = 0;
   vex_state->guest_D23 = 0;
   vex_state->guest_D24 = 0;
   vex_state->guest_D25 = 0;
   vex_state->guest_D26 = 0;
   vex_state->guest_D27 = 0;
   vex_state->guest_D28 = 0;
   vex_state->guest_D29 = 0;
   vex_state->guest_D30 = 0;
   vex_state->guest_D31 = 0;

   /* ARM encoded; zero is the default as it happens (result flags
      (NZCV) cleared, FZ disabled, round to nearest, non-vector mode,
      all exns masked, all exn sticky bits cleared). */
   vex_state->guest_FPSCR = 0;

   vex_state->guest_TPIDRURO = 0;

   /* Not in a Thumb IT block. */
   vex_state->guest_ITSTATE = 0;

   vex_state->padding1 = 0;
}


/*-----------------------------------------------------------*/
/*--- Describing the arm guest state, for the benefit     ---*/
/*--- of iropt and instrumenters.                         ---*/
/*-----------------------------------------------------------*/

/* Figure out if any part of the guest state contained in minoff
   .. maxoff requires precise memory exceptions.  If in doubt return
   True (but this is generates significantly slower code).  

   We enforce precise exns for guest R13(sp), R15T(pc).
*/
Bool guest_arm_state_requires_precise_mem_exns ( Int minoff, 
                                                 Int maxoff)
{
   Int sp_min = offsetof(VexGuestARMState, guest_R13);
   Int sp_max = sp_min + 4 - 1;
   Int pc_min = offsetof(VexGuestARMState, guest_R15T);
   Int pc_max = pc_min + 4 - 1;

   if (maxoff < sp_min || minoff > sp_max) {
      /* no overlap with sp */
   } else {
      return True;
   }

   if (maxoff < pc_min || minoff > pc_max) {
      /* no overlap with pc */
   } else {
      return True;
   }

   /* We appear to need precise updates of R11 in order to get proper
      stacktraces from non-optimised code. */
   Int r11_min = offsetof(VexGuestARMState, guest_R11);
   Int r11_max = r11_min + 4 - 1;

   if (maxoff < r11_min || minoff > r11_max) {
      /* no overlap with r11 */
   } else {
      return True;
   }

   /* Ditto R7, particularly needed for proper stacktraces in Thumb
      code. */
   Int r7_min = offsetof(VexGuestARMState, guest_R7);
   Int r7_max = r7_min + 4 - 1;

   if (maxoff < r7_min || minoff > r7_max) {
      /* no overlap with r7 */
   } else {
      return True;
   }

   return False;
}



#define ALWAYSDEFD(field)                           \
    { offsetof(VexGuestARMState, field),            \
      (sizeof ((VexGuestARMState*)0)->field) }

VexGuestLayout
   armGuest_layout 
      = { 
          /* Total size of the guest state, in bytes. */
          .total_sizeB = sizeof(VexGuestARMState),

          /* Describe the stack pointer. */
          .offset_SP = offsetof(VexGuestARMState,guest_R13),
          .sizeof_SP = 4,

          /* Describe the instruction pointer. */
          .offset_IP = offsetof(VexGuestARMState,guest_R15T),
          .sizeof_IP = 4,

          /* Describe any sections to be regarded by Memcheck as
             'always-defined'. */
          .n_alwaysDefd = 10,

          /* flags thunk: OP is always defd, whereas DEP1 and DEP2
             have to be tracked.  See detailed comment in gdefs.h on
             meaning of thunk fields. */
          .alwaysDefd
             = { /* 0 */ ALWAYSDEFD(guest_R15T),
                 /* 1 */ ALWAYSDEFD(guest_CC_OP),
                 /* 2 */ ALWAYSDEFD(guest_CC_NDEP),
                 /* 3 */ ALWAYSDEFD(guest_EMWARN),
                 /* 4 */ ALWAYSDEFD(guest_TISTART),
                 /* 5 */ ALWAYSDEFD(guest_TILEN),
                 /* 6 */ ALWAYSDEFD(guest_NRADDR),
                 /* 7 */ ALWAYSDEFD(guest_IP_AT_SYSCALL),
                 /* 8 */ ALWAYSDEFD(guest_TPIDRURO),
                 /* 9 */ ALWAYSDEFD(guest_ITSTATE)
               }
        };


/*---------------------------------------------------------------*/
/*--- end                                 guest_arm_helpers.c ---*/
/*---------------------------------------------------------------*/
