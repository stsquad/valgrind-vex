
/*---------------------------------------------------------------*/
/*---                                                         ---*/
/*--- This file (host_arm_isel.c) is                          ---*/
/*--- Copyright (C) OpenWorks LLP.  All rights reserved.      ---*/
/*---                                                         ---*/
/*---------------------------------------------------------------*/

/*
   This file is part of LibVEX, a library for dynamic binary
   instrumentation and translation.

   Copyright (C) 2004-2009 OpenWorks LLP.  All rights reserved.

   This library is made available under a dual licensing scheme.

   If you link LibVEX against other code all of which is itself
   licensed under the GNU General Public License, version 2 dated June
   1991 ("GPL v2"), then you may use LibVEX under the terms of the GPL
   v2, as appearing in the file LICENSE.GPL.  If the file LICENSE.GPL
   is missing, you can obtain a copy of the GPL v2 from the Free
   Software Foundation Inc., 51 Franklin St, Fifth Floor, Boston, MA
   02110-1301, USA.

   For any other uses of LibVEX, you must first obtain a commercial
   license from OpenWorks LLP.  Please contact info@open-works.co.uk
   for information about commercial licensing.

   This software is provided by OpenWorks LLP "as is" and any express
   or implied warranties, including, but not limited to, the implied
   warranties of merchantability and fitness for a particular purpose
   are disclaimed.  In no event shall OpenWorks LLP be liable for any
   direct, indirect, incidental, special, exemplary, or consequential
   damages (including, but not limited to, procurement of substitute
   goods or services; loss of use, data, or profits; or business
   interruption) however caused and on any theory of liability,
   whether in contract, strict liability, or tort (including
   negligence or otherwise) arising in any way out of the use of this
   software, even if advised of the possibility of such damage.
*/

#include "libvex_basictypes.h"
#include "libvex_ir.h"
#include "libvex.h"

#include "main_util.h"
#include "main_globals.h"
#include "host_generic_regs.h"
#include "host_arm_defs.h"


/*---------------------------------------------------------*/
/*--- ARMvfp control word stuff                         ---*/
/*---------------------------------------------------------*/

/* Vex-generated code expects to run with the FPU set as follows: all
   exceptions masked, round-to-nearest, non-vector mode, with the NZCV
   flags cleared, and FZ (flush to zero) disabled.  Curiously enough,
   this corresponds to a FPSCR value of zero.

   fpscr should therefore be zero on entry to Vex-generated code, and
   should be unchanged at exit.  (Or at least the bottom 28 bits
   should be zero).
*/

#define DEFAULT_FPSCR 0


/*---------------------------------------------------------*/
/*--- ISelEnv                                           ---*/
/*---------------------------------------------------------*/

/* This carries around:

   - A mapping from IRTemp to IRType, giving the type of any IRTemp we
     might encounter.  This is computed before insn selection starts,
     and does not change.

   - A mapping from IRTemp to HReg.  This tells the insn selector
     which virtual register(s) are associated with each IRTemp
     temporary.  This is computed before insn selection starts, and
     does not change.  We expect this mapping to map precisely the
     same set of IRTemps as the type mapping does.

        - vregmap   holds the primary register for the IRTemp.
        - vregmapHI is only used for 64-bit integer-typed
             IRTemps.  It holds the identity of a second
             32-bit virtual HReg, which holds the high half
             of the value.

   - The name of the vreg in which we stash a copy of the link reg, so
     helper functions don't kill it.

   - The code array, that is, the insns selected so far.

   - A counter, for generating new virtual registers.

   - The host hardware capabilities word.  This is set at the start
     and does not change.

   Note, this is all host-independent.  */

typedef
   struct {
      IRTypeEnv*   type_env;

      HReg*        vregmap;
      HReg*        vregmapHI;
      Int          n_vregmap;

      HReg         savedLR;

      HInstrArray* code;

      Int          vreg_ctr;

      UInt         hwcaps;
   }
   ISelEnv;

static HReg lookupIRTemp ( ISelEnv* env, IRTemp tmp )
{
   vassert(tmp >= 0);
   vassert(tmp < env->n_vregmap);
   return env->vregmap[tmp];
}

static void lookupIRTemp64 ( HReg* vrHI, HReg* vrLO, ISelEnv* env, IRTemp tmp )
{
   vassert(tmp >= 0);
   vassert(tmp < env->n_vregmap);
   vassert(env->vregmapHI[tmp] != INVALID_HREG);
   *vrLO = env->vregmap[tmp];
   *vrHI = env->vregmapHI[tmp];
}

static void addInstr ( ISelEnv* env, ARMInstr* instr )
{
   addHInstr(env->code, instr);
   if (vex_traceflags & VEX_TRACE_VCODE) {
      ppARMInstr(instr);
      vex_printf("\n");
   }
}

static HReg newVRegI ( ISelEnv* env )
{
   HReg reg = mkHReg(env->vreg_ctr, HRcInt32, True/*virtual reg*/);
   env->vreg_ctr++;
   return reg;
}

static HReg newVRegD ( ISelEnv* env )
{
   HReg reg = mkHReg(env->vreg_ctr, HRcFlt64, True/*virtual reg*/);
   env->vreg_ctr++;
   return reg;
}

static HReg newVRegF ( ISelEnv* env )
{
   HReg reg = mkHReg(env->vreg_ctr, HRcFlt32, True/*virtual reg*/);
   env->vreg_ctr++;
   return reg;
}


/*---------------------------------------------------------*/
/*--- ISEL: Forward declarations                        ---*/
/*---------------------------------------------------------*/

/* These are organised as iselXXX and iselXXX_wrk pairs.  The
   iselXXX_wrk do the real work, but are not to be called directly.
   For each XXX, iselXXX calls its iselXXX_wrk counterpart, then
   checks that all returned registers are virtual.  You should not
   call the _wrk version directly.
*/
static ARMAMode1*  iselIntExpr_AMode1_wrk ( ISelEnv* env, IRExpr* e );
static ARMAMode1*  iselIntExpr_AMode1     ( ISelEnv* env, IRExpr* e );

static ARMAMode2*  iselIntExpr_AMode2_wrk ( ISelEnv* env, IRExpr* e );
static ARMAMode2*  iselIntExpr_AMode2     ( ISelEnv* env, IRExpr* e );

static ARMAModeV*  iselIntExpr_AModeV_wrk ( ISelEnv* env, IRExpr* e );
static ARMAModeV*  iselIntExpr_AModeV     ( ISelEnv* env, IRExpr* e );

static ARMRI84*    iselIntExpr_RI84_wrk
        ( /*OUT*/Bool* didInv, Bool mayInv, ISelEnv* env, IRExpr* e );
static ARMRI84*    iselIntExpr_RI84
        ( /*OUT*/Bool* didInv, Bool mayInv, ISelEnv* env, IRExpr* e );

static ARMRI5*     iselIntExpr_RI5_wrk    ( ISelEnv* env, IRExpr* e );
static ARMRI5*     iselIntExpr_RI5        ( ISelEnv* env, IRExpr* e );

static ARMCondCode iselCondCode_wrk       ( ISelEnv* env, IRExpr* e );
static ARMCondCode iselCondCode           ( ISelEnv* env, IRExpr* e );

static HReg        iselIntExpr_R_wrk      ( ISelEnv* env, IRExpr* e );
static HReg        iselIntExpr_R          ( ISelEnv* env, IRExpr* e );

static void        iselInt64Expr_wrk      ( HReg* rHi, HReg* rLo, 
                                            ISelEnv* env, IRExpr* e );
static void        iselInt64Expr          ( HReg* rHi, HReg* rLo, 
                                            ISelEnv* env, IRExpr* e );

static HReg        iselDblExpr_wrk        ( ISelEnv* env, IRExpr* e );
static HReg        iselDblExpr            ( ISelEnv* env, IRExpr* e );

static HReg        iselFltExpr_wrk        ( ISelEnv* env, IRExpr* e );
static HReg        iselFltExpr            ( ISelEnv* env, IRExpr* e );


/*---------------------------------------------------------*/
/*--- ISEL: Misc helpers                                ---*/
/*---------------------------------------------------------*/

static UInt ROR32 ( UInt x, UInt sh ) {
   vassert(sh >= 0 && sh < 32);
   if (sh == 0)
      return x;
   else
      return (x << (32-sh)) | (x >> sh);
}

/* Figure out if 'u' fits in the special shifter-operand 8x4 immediate
   form, and if so return the components. */
static Bool fitsIn8x4 ( /*OUT*/UInt* u8, /*OUT*/UInt* u4, UInt u )
{
   UInt i;
   for (i = 0; i < 16; i++) {
      if (0 == (u & 0xFFFFFF00)) {
         *u8 = u;
         *u4 = i;
         return True;
      }
      u = ROR32(u, 30);
   }
   vassert(i == 16);
   return False;
}

/* Make a int reg-reg move. */
static ARMInstr* mk_iMOVds_RR ( HReg dst, HReg src )
{
   vassert(hregClass(src) == HRcInt32);
   vassert(hregClass(dst) == HRcInt32);
   return ARMInstr_Mov(dst, ARMRI84_R(src));
}

/* Set the VFP unit's rounding mode to default (round to nearest). */
static void set_VFP_rounding_default ( ISelEnv* env )
{
   /* mov rTmp, #DEFAULT_FPSCR
      fmxr fpscr, rTmp
   */
   HReg rTmp = newVRegI(env);
   addInstr(env, ARMInstr_Imm32(rTmp, DEFAULT_FPSCR));
   addInstr(env, ARMInstr_FPSCR(True/*toFPSCR*/, rTmp));
}

/* Mess with the VFP unit's rounding mode: 'mode' is an I32-typed
   expression denoting a value in the range 0 .. 3, indicating a round
   mode encoded as per type IRRoundingMode.  Set FPSCR to have the
   same rounding.
*/
static
void set_VFP_rounding_mode ( ISelEnv* env, IRExpr* mode )
{
   /* This isn't simple, because 'mode' carries an IR rounding
      encoding, and we need to translate that to an ARMvfp one:
      The IR encoding:
         00  to nearest (the default)
         10  to +infinity
         01  to -infinity
         11  to zero
      The ARMvfp encoding:
         00  to nearest
         01  to +infinity
         10  to -infinity
         11  to zero
      Easy enough to do; just swap the two bits.
   */
   HReg irrm = iselIntExpr_R(env, mode);
   HReg tL   = newVRegI(env);
   HReg tR   = newVRegI(env);
   HReg t3   = newVRegI(env);
   /* tL = irrm << 1;
      tR = irrm >> 1;  if we're lucky, these will issue together
      tL &= 2;
      tR &= 1;         ditto
      t3 = tL | tR;
      t3 <<= 22;
      fmxr fpscr, t3
   */
   addInstr(env, ARMInstr_Shift(ARMsh_SHL, tL, irrm, ARMRI5_I5(1)));
   addInstr(env, ARMInstr_Shift(ARMsh_SHR, tR, irrm, ARMRI5_I5(1)));
   addInstr(env, ARMInstr_Alu(ARMalu_AND, tL, tL, ARMRI84_I84(2,0)));
   addInstr(env, ARMInstr_Alu(ARMalu_AND, tR, tR, ARMRI84_I84(1,0)));
   addInstr(env, ARMInstr_Alu(ARMalu_OR, t3, tL, ARMRI84_R(tR)));
   addInstr(env, ARMInstr_Shift(ARMsh_SHL, t3, t3, ARMRI5_I5(22)));
   addInstr(env, ARMInstr_FPSCR(True/*toFPSCR*/, t3));
}


/*---------------------------------------------------------*/
/*--- ISEL: Function call helpers                       ---*/
/*---------------------------------------------------------*/

/* Used only in doHelperCall.  See big comment in doHelperCall re
   handling of register-parameter args.  This function figures out
   whether evaluation of an expression might require use of a fixed
   register.  If in doubt return True (safe but suboptimal).
*/
static
Bool mightRequireFixedRegs ( IRExpr* e )
{
   switch (e->tag) {
   case Iex_RdTmp: case Iex_Const: case Iex_Get:
      return False;
   default:
      return True;
   }
}


/* Do a complete function call.  guard is a Ity_Bit expression
   indicating whether or not the call happens.  If guard==NULL, the
   call is unconditional.  Returns True iff it managed to handle this
   combination of arg/return types, else returns False. */

static
Bool doHelperCall ( ISelEnv* env,
                    Bool passBBP,
                    IRExpr* guard, IRCallee* cee, IRExpr** args )
{
   ARMCondCode cc;
   HReg        argregs[ARM_N_ARGREGS];
   HReg        tmpregs[ARM_N_ARGREGS];
   Bool        go_fast;
   Int         n_args, i, nextArgReg;
   ULong       target;

   vassert(ARM_N_ARGREGS == 4);

   /* Marshal args for a call and do the call.

      If passBBP is True, r8 (the baseblock pointer) is to be passed
      as the first arg.

      This function only deals with a tiny set of possibilities, which
      cover all helpers in practice.  The restrictions are that only
      arguments in registers are supported, hence only ARM_N_REGPARMS
      x 32 integer bits in total can be passed.  In fact the only
      supported arg types are I32 and I64.

      Generating code which is both efficient and correct when
      parameters are to be passed in registers is difficult, for the
      reasons elaborated in detail in comments attached to
      doHelperCall() in priv/host-x86/isel.c.  Here, we use a variant
      of the method described in those comments.

      The problem is split into two cases: the fast scheme and the
      slow scheme.  In the fast scheme, arguments are computed
      directly into the target (real) registers.  This is only safe
      when we can be sure that computation of each argument will not
      trash any real registers set by computation of any other
      argument.

      In the slow scheme, all args are first computed into vregs, and
      once they are all done, they are moved to the relevant real
      regs.  This always gives correct code, but it also gives a bunch
      of vreg-to-rreg moves which are usually redundant but are hard
      for the register allocator to get rid of.

      To decide which scheme to use, all argument expressions are
      first examined.  If they are all so simple that it is clear they
      will be evaluated without use of any fixed registers, use the
      fast scheme, else use the slow scheme.  Note also that only
      unconditional calls may use the fast scheme, since having to
      compute a condition expression could itself trash real
      registers.

      Note this requires being able to examine an expression and
      determine whether or not evaluation of it might use a fixed
      register.  That requires knowledge of how the rest of this insn
      selector works.  Currently just the following 3 are regarded as
      safe -- hopefully they cover the majority of arguments in
      practice: IRExpr_Tmp IRExpr_Const IRExpr_Get.
   */

   /* Note that the cee->regparms field is meaningless on ARM hosts
      (since there is only one calling convention) and so we always
      ignore it. */

   n_args = 0;
   for (i = 0; args[i]; i++)
      n_args++;

   argregs[0] = hregARM_R0();
   argregs[1] = hregARM_R1();
   argregs[2] = hregARM_R2();
   argregs[3] = hregARM_R3();

   tmpregs[0] = tmpregs[1] = tmpregs[2] =
   tmpregs[3] = INVALID_HREG;

   /* First decide which scheme (slow or fast) is to be used.  First
      assume the fast scheme, and select slow if any contraindications
      (wow) appear. */

   go_fast = True;

   if (guard) {
      if (guard->tag == Iex_Const
          && guard->Iex.Const.con->tag == Ico_U1
          && guard->Iex.Const.con->Ico.U1 == True) {
         /* unconditional */
      } else {
         /* Not manifestly unconditional -- be conservative. */
         go_fast = False;
      }
   }

   if (go_fast) {
      for (i = 0; i < n_args; i++) {
         if (mightRequireFixedRegs(args[i])) {
            go_fast = False;
            break;
         }
      }
   }
   /* At this point the scheme to use has been established.  Generate
      code to get the arg values into the argument rregs.  If we run
      out of arg regs, give up. */

   if (go_fast) {

      /* FAST SCHEME */
      nextArgReg = 0;
      if (passBBP) {
         addInstr(env, mk_iMOVds_RR( argregs[nextArgReg],
                                     hregARM_R8() ));
         nextArgReg++;
      }

      for (i = 0; i < n_args; i++) {
         IRType aTy = typeOfIRExpr(env->type_env, args[i]);
         if (nextArgReg >= ARM_N_ARGREGS)
            return False; /* out of argregs */
         if (aTy == Ity_I32) {
            addInstr(env, mk_iMOVds_RR( argregs[nextArgReg],
                                        iselIntExpr_R(env, args[i]) ));
            nextArgReg++;
         }
         else if (aTy == Ity_I64) {
            /* 64-bit args must be passed in an a reg-pair of the form
               n:n+1, where n is even.  Hence either r0:r1 or r2:r3.
               On a little-endian host, the less significant word is
               passed in the lower-numbered register. */
            if (nextArgReg & 1) {
               if (nextArgReg >= ARM_N_ARGREGS)
                  return False; /* out of argregs */
               addInstr(env, ARMInstr_Imm32( argregs[nextArgReg], 0xAA ));
               nextArgReg++;
            }
            if (nextArgReg >= ARM_N_ARGREGS)
               return False; /* out of argregs */
            HReg raHi, raLo;
            iselInt64Expr(&raHi, &raLo, env, args[i]);
            addInstr(env, mk_iMOVds_RR( argregs[nextArgReg], raLo ));
            nextArgReg++;
            addInstr(env, mk_iMOVds_RR( argregs[nextArgReg], raHi ));
            nextArgReg++;
         }
         else
            return False; /* unhandled arg type */
      }

      /* Fast scheme only applies for unconditional calls.  Hence: */
      cc = ARMcc_AL;

   } else {

      /* SLOW SCHEME; move via temporaries */
      nextArgReg = 0;

      if (passBBP) {
         /* This is pretty stupid; better to move directly to r0
            after the rest of the args are done. */
         tmpregs[nextArgReg] = newVRegI(env);
         addInstr(env, mk_iMOVds_RR( tmpregs[nextArgReg],
                                     hregARM_R8() ));
         nextArgReg++;
      }

      for (i = 0; i < n_args; i++) {
         IRType aTy = typeOfIRExpr(env->type_env, args[i]);
         if (nextArgReg >= ARM_N_ARGREGS)
            return False; /* out of argregs */
         if (aTy == Ity_I32) {
            tmpregs[nextArgReg] = iselIntExpr_R(env, args[i]);
            nextArgReg++;
         }
         else if (aTy == Ity_I64) {
            /* Same comment applies as in the Fast-scheme case. */
            if (nextArgReg & 1)
               nextArgReg++;
            if (nextArgReg + 1 >= ARM_N_ARGREGS)
               return False; /* out of argregs */
            HReg raHi, raLo;
            iselInt64Expr(&raHi, &raLo, env, args[i]);
            tmpregs[nextArgReg] = raLo;
            nextArgReg++;
            tmpregs[nextArgReg] = raHi;
            nextArgReg++;
         }
      }

      /* Now we can compute the condition.  We can't do it earlier
         because the argument computations could trash the condition
         codes.  Be a bit clever to handle the common case where the
         guard is 1:Bit. */
      cc = ARMcc_AL;
      if (guard) {
         if (guard->tag == Iex_Const
             && guard->Iex.Const.con->tag == Ico_U1
             && guard->Iex.Const.con->Ico.U1 == True) {
            /* unconditional -- do nothing */
         } else {
            cc = iselCondCode( env, guard );
         }
      }

      /* Move the args to their final destinations. */
      for (i = 0; i < nextArgReg; i++) {
         if (tmpregs[i] == INVALID_HREG) { // Skip invalid regs
            addInstr(env, ARMInstr_Imm32( argregs[i], 0xAA ));
            continue;
         }
         /* None of these insns, including any spill code that might
            be generated, may alter the condition codes. */
         addInstr( env, mk_iMOVds_RR( argregs[i], tmpregs[i] ) );
      }

   }

   /* Should be assured by checks above */
   vassert(nextArgReg <= ARM_N_ARGREGS);

   target = (HWord)Ptr_to_ULong(cee->addr);

   /* nextArgReg doles out argument registers.  Since these are
      assigned in the order r0, r1, r2, r3, its numeric value at this
      point, which must be between 0 and 4 inclusive, is going to be
      equal to the number of arg regs in use for the call.  Hence bake
      that number into the call (we'll need to know it when doing
      register allocation, to know what regs the call reads.)

      There is a bit of a twist -- harmless but worth recording.
      Suppose the arg types are (Ity_I32, Ity_I64).  Then we will have
      the first arg in r0 and the second in r3:r2, but r1 isn't used.
      We nevertheless have nextArgReg==4 and bake that into the call
      instruction.  This will mean the register allocator wil believe
      this insn reads r1 when in fact it doesn't.  But that's
      harmless; it just artificially extends the live range of r1
      unnecessarily.  The best fix would be to put into the
      instruction, a bitmask indicating which of r0/1/2/3 carry live
      values.  But that's too much hassle. */

   /* Finally, the call itself. */
   addInstr(env, ARMInstr_Call( cc, target, nextArgReg ));

   return True; /* success */
}


/*---------------------------------------------------------*/
/*--- ISEL: Integer expressions (32/16/8 bit)           ---*/
/*---------------------------------------------------------*/

/* Select insns for an integer-typed expression, and add them to the
   code list.  Return a reg holding the result.  This reg will be a
   virtual register.  THE RETURNED REG MUST NOT BE MODIFIED.  If you
   want to modify it, ask for a new vreg, copy it in there, and modify
   the copy.  The register allocator will do its best to map both
   vregs to the same real register, so the copies will often disappear
   later in the game.

   This should handle expressions of 32, 16 and 8-bit type.  All
   results are returned in a 32-bit register.  For 16- and 8-bit
   expressions, the upper 16/24 bits are arbitrary, so you should mask
   or sign extend partial values if necessary.
*/

/* --------------------- AMode1 --------------------- */

/* Return an AMode1 which computes the value of the specified
   expression, possibly also adding insns to the code list as a
   result.  The expression may only be a 32-bit one.
*/

static Bool sane_AMode1 ( ARMAMode1* am )
{
   switch (am->tag) {
      case ARMam1_RI:
         return
            toBool( hregClass(am->ARMam1.RI.reg) == HRcInt32
                    && (hregIsVirtual(am->ARMam1.RI.reg)
                        || am->ARMam1.RI.reg == hregARM_R8())
                    && am->ARMam1.RI.simm13 >= -4095
                    && am->ARMam1.RI.simm13 <= 4095 );
      case ARMam1_RRS:
         return
            toBool( hregClass(am->ARMam1.RRS.base) == HRcInt32
                    && hregIsVirtual(am->ARMam1.RRS.base)
                    && hregClass(am->ARMam1.RRS.index) == HRcInt32
                    && hregIsVirtual(am->ARMam1.RRS.index)
                    && am->ARMam1.RRS.shift >= 0
                    && am->ARMam1.RRS.shift <= 3 );
      default:
         vpanic("sane_AMode: unknown ARM AMode1 tag");
   }
}

static ARMAMode1* iselIntExpr_AMode1 ( ISelEnv* env, IRExpr* e )
{
   ARMAMode1* am = iselIntExpr_AMode1_wrk(env, e);
   vassert(sane_AMode1(am));
   return am;
}

static ARMAMode1* iselIntExpr_AMode1_wrk ( ISelEnv* env, IRExpr* e )
{
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_I32);

   /* FIXME: add RRS matching */

   /* {Add32,Sub32}(expr,simm13) */
   if (e->tag == Iex_Binop
       && (e->Iex.Binop.op == Iop_Add32 || e->Iex.Binop.op == Iop_Sub32)
       && e->Iex.Binop.arg2->tag == Iex_Const
       && e->Iex.Binop.arg2->Iex.Const.con->tag == Ico_U32) {
      Int simm = (Int)e->Iex.Binop.arg2->Iex.Const.con->Ico.U32;
      if (simm >= -4095 && simm <= 4095) {
         HReg reg;
         if (e->Iex.Binop.op == Iop_Sub32)
            simm = -simm;
         reg = iselIntExpr_R(env, e->Iex.Binop.arg1);
         return ARMAMode1_RI(reg, simm);
      }
   }

   /* Doesn't match anything in particular.  Generate it into
      a register and use that. */
   {
      HReg reg = iselIntExpr_R(env, e);
      return ARMAMode1_RI(reg, 0);
   }

}


/* --------------------- AMode2 --------------------- */

/* Return an AMode2 which computes the value of the specified
   expression, possibly also adding insns to the code list as a
   result.  The expression may only be a 32-bit one.
*/

static Bool sane_AMode2 ( ARMAMode2* am )
{
   switch (am->tag) {
      case ARMam2_RI:
         return
            toBool( hregClass(am->ARMam2.RI.reg) == HRcInt32
                    && hregIsVirtual(am->ARMam2.RI.reg)
                    && am->ARMam2.RI.simm9 >= -255
                    && am->ARMam2.RI.simm9 <= 255 );
      case ARMam2_RR:
         return
            toBool( hregClass(am->ARMam2.RR.base) == HRcInt32
                    && hregIsVirtual(am->ARMam2.RR.base)
                    && hregClass(am->ARMam2.RR.index) == HRcInt32
                    && hregIsVirtual(am->ARMam2.RR.index) );
      default:
         vpanic("sane_AMode: unknown ARM AMode2 tag");
   }
}

static ARMAMode2* iselIntExpr_AMode2 ( ISelEnv* env, IRExpr* e )
{
   ARMAMode2* am = iselIntExpr_AMode2_wrk(env, e);
   vassert(sane_AMode2(am));
   return am;
}

static ARMAMode2* iselIntExpr_AMode2_wrk ( ISelEnv* env, IRExpr* e )
{
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_I32);

   /* FIXME: add RR matching */

   /* {Add32,Sub32}(expr,simm8) */
   if (e->tag == Iex_Binop
       && (e->Iex.Binop.op == Iop_Add32 || e->Iex.Binop.op == Iop_Sub32)
       && e->Iex.Binop.arg2->tag == Iex_Const
       && e->Iex.Binop.arg2->Iex.Const.con->tag == Ico_U32) {
      Int simm = (Int)e->Iex.Binop.arg2->Iex.Const.con->Ico.U32;
      if (simm >= -255 && simm <= 255) {
         HReg reg;
         if (e->Iex.Binop.op == Iop_Sub32)
            simm = -simm;
         reg = iselIntExpr_R(env, e->Iex.Binop.arg1);
         return ARMAMode2_RI(reg, simm);
      }
   }

   /* Doesn't match anything in particular.  Generate it into
      a register and use that. */
   {
      HReg reg = iselIntExpr_R(env, e);
      return ARMAMode2_RI(reg, 0);
   }

}


/* --------------------- AModeV --------------------- */

/* Return an AModeV which computes the value of the specified
   expression, possibly also adding insns to the code list as a
   result.  The expression may only be a 32-bit one.
*/

static Bool sane_AModeV ( ARMAModeV* am )
{
  return toBool( hregClass(am->reg) == HRcInt32
                 && hregIsVirtual(am->reg)
                 && am->simm11 >= -1020 && am->simm11 <= 1020
                 && 0 == (am->simm11 & 3) );
}

static ARMAModeV* iselIntExpr_AModeV ( ISelEnv* env, IRExpr* e )
{
   ARMAModeV* am = iselIntExpr_AModeV_wrk(env, e);
   vassert(sane_AModeV(am));
   return am;
}

static ARMAModeV* iselIntExpr_AModeV_wrk ( ISelEnv* env, IRExpr* e )
{
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_I32);

   /* {Add32,Sub32}(expr, simm8 << 2) */
   if (e->tag == Iex_Binop
       && (e->Iex.Binop.op == Iop_Add32 || e->Iex.Binop.op == Iop_Sub32)
       && e->Iex.Binop.arg2->tag == Iex_Const
       && e->Iex.Binop.arg2->Iex.Const.con->tag == Ico_U32) {
      Int simm = (Int)e->Iex.Binop.arg2->Iex.Const.con->Ico.U32;
      if (simm >= -1020 && simm <= 1020 && 0 == (simm & 3)) {
         HReg reg;
         if (e->Iex.Binop.op == Iop_Sub32)
            simm = -simm;
         reg = iselIntExpr_R(env, e->Iex.Binop.arg1);
         return mkARMAModeV(reg, simm);
      }
   }

   /* Doesn't match anything in particular.  Generate it into
      a register and use that. */
   {
      HReg reg = iselIntExpr_R(env, e);
      return mkARMAModeV(reg, 0);
   }

}


/* --------------------- RI84 --------------------- */

/* Select instructions to generate 'e' into a RI84.  If mayInv is
   true, then the caller will also accept an I84 form that denotes
   'not e'.  In this case didInv may not be NULL, and *didInv is set
   to True.  This complication is so as to allow generation of an RI84
   which is suitable for use in either an AND or BIC instruction,
   without knowing (before this call) which one.
*/
static ARMRI84* iselIntExpr_RI84 ( /*OUT*/Bool* didInv, Bool mayInv,
                                   ISelEnv* env, IRExpr* e )
{
   ARMRI84* ri;
   if (mayInv)
      vassert(didInv != NULL);
   ri = iselIntExpr_RI84_wrk(didInv, mayInv, env, e);
   /* sanity checks ... */
   switch (ri->tag) {
      case ARMri84_I84:
         return ri;
      case ARMri84_R:
         vassert(hregClass(ri->ARMri84.R.reg) == HRcInt32);
         vassert(hregIsVirtual(ri->ARMri84.R.reg));
         return ri;
      default:
         vpanic("iselIntExpr_RI84: unknown arm RI84 tag");
   }
}

/* DO NOT CALL THIS DIRECTLY ! */
static ARMRI84* iselIntExpr_RI84_wrk ( /*OUT*/Bool* didInv, Bool mayInv,
                                       ISelEnv* env, IRExpr* e )
{
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_I32 || ty == Ity_I16 || ty == Ity_I8);

   if (didInv) *didInv = False;

   /* special case: immediate */
   if (e->tag == Iex_Const) {
      UInt u, u8 = 0x100, u4 = 0x10; /* both invalid */
      switch (e->Iex.Const.con->tag) {
         case Ico_U32: u = e->Iex.Const.con->Ico.U32; break;
         case Ico_U16: u = 0xFFFF & (e->Iex.Const.con->Ico.U16); break;
         case Ico_U8:  u = 0xFF   & (e->Iex.Const.con->Ico.U8); break;
         default: vpanic("iselIntExpr_RI84.Iex_Const(armh)");
      }
      if (fitsIn8x4(&u8, &u4, u)) {
         return ARMRI84_I84( (UShort)u8, (UShort)u4 );
      }
      if (mayInv && fitsIn8x4(&u8, &u4, ~u)) {
         vassert(didInv);
         *didInv = True;
         return ARMRI84_I84( (UShort)u8, (UShort)u4 );
      }
      /* else fail, fall through to default case */
   }

   /* default case: calculate into a register and return that */
   {
      HReg r = iselIntExpr_R ( env, e );
      return ARMRI84_R(r);
   }
}


/* --------------------- RI5 --------------------- */

/* Select instructions to generate 'e' into a RI5. */

static ARMRI5* iselIntExpr_RI5 ( ISelEnv* env, IRExpr* e )
{
   ARMRI5* ri = iselIntExpr_RI5_wrk(env, e);
   /* sanity checks ... */
   switch (ri->tag) {
      case ARMri5_I5:
         return ri;
      case ARMri5_R:
         vassert(hregClass(ri->ARMri5.R.reg) == HRcInt32);
         vassert(hregIsVirtual(ri->ARMri5.R.reg));
         return ri;
      default:
         vpanic("iselIntExpr_RI5: unknown arm RI5 tag");
   }
}

/* DO NOT CALL THIS DIRECTLY ! */
static ARMRI5* iselIntExpr_RI5_wrk ( ISelEnv* env, IRExpr* e )
{
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_I32 || ty == Ity_I8);

   /* special case: immediate */
   if (e->tag == Iex_Const) {
      UInt u; /* both invalid */
      switch (e->Iex.Const.con->tag) {
         case Ico_U32: u = e->Iex.Const.con->Ico.U32; break;
         case Ico_U16: u = 0xFFFF & (e->Iex.Const.con->Ico.U16); break;
         case Ico_U8:  u = 0xFF   & (e->Iex.Const.con->Ico.U8); break;
         default: vpanic("iselIntExpr_RI5.Iex_Const(armh)");
      }
      if (u >= 1 && u <= 31) {
         return ARMRI5_I5(u);
      }
      /* else fail, fall through to default case */
   }

   /* default case: calculate into a register and return that */
   {
      HReg r = iselIntExpr_R ( env, e );
      return ARMRI5_R(r);
   }
}


/* ------------------- CondCode ------------------- */

/* Generate code to evaluated a bit-typed expression, returning the
   condition code which would correspond when the expression would
   notionally have returned 1. */

static ARMCondCode iselCondCode ( ISelEnv* env, IRExpr* e )
{
   /* Uh, there's nothing we can sanity check here, unfortunately. */
   return iselCondCode_wrk(env,e);
}

static ARMCondCode iselCondCode_wrk ( ISelEnv* env, IRExpr* e )
{
   vassert(e);
   vassert(typeOfIRExpr(env->type_env,e) == Ity_I1);

   /* var */
   if (e->tag == Iex_RdTmp) {
      HReg rTmp = lookupIRTemp(env, e->Iex.RdTmp.tmp);
      /* CmpOrTst doesn't modify rTmp; so this is OK. */
      ARMRI84* one  = ARMRI84_I84(1,0);
      addInstr(env, ARMInstr_CmpOrTst(False/*test*/, rTmp, one));
      return ARMcc_NE;
   }

   /* Not1(e) */
   if (e->tag == Iex_Unop && e->Iex.Unop.op == Iop_Not1) {
      /* Generate code for the arg, and negate the test condition */
      return 1 ^ iselCondCode(env, e->Iex.Unop.arg);
   }

   /* --- patterns rooted at: 32to1 --- */

   if (e->tag == Iex_Unop
       && e->Iex.Unop.op == Iop_32to1) {
      HReg     rTmp = iselIntExpr_R(env, e->Iex.Unop.arg);
      ARMRI84* one  = ARMRI84_I84(1,0);
      addInstr(env, ARMInstr_CmpOrTst(False/*test*/, rTmp, one));
      return ARMcc_NE;
   }

   /* --- patterns rooted at: CmpNEZ8 --- */

   if (e->tag == Iex_Unop
       && e->Iex.Unop.op == Iop_CmpNEZ8) {
      HReg     r1   = iselIntExpr_R(env, e->Iex.Unop.arg);
      ARMRI84* xFF  = ARMRI84_I84(0xFF,0);
      addInstr(env, ARMInstr_CmpOrTst(False/*!isCmp*/, r1, xFF));
      return ARMcc_NE;
   }

   /* --- patterns rooted at: CmpNEZ32 --- */

   if (e->tag == Iex_Unop
       && e->Iex.Unop.op == Iop_CmpNEZ32) {
      HReg     r1   = iselIntExpr_R(env, e->Iex.Unop.arg);
      ARMRI84* zero = ARMRI84_I84(0,0);
      addInstr(env, ARMInstr_CmpOrTst(True/*isCmp*/, r1, zero));
      return ARMcc_NE;
   }

   /* --- patterns rooted at: CmpNEZ64 --- */

   if (e->tag == Iex_Unop
       && e->Iex.Unop.op == Iop_CmpNEZ64) {
      HReg     tHi, tLo;
      HReg     tmp  = newVRegI(env);
      ARMRI84* zero = ARMRI84_I84(0,0);
      iselInt64Expr(&tHi, &tLo, env, e->Iex.Unop.arg);
      addInstr(env, ARMInstr_Alu(ARMalu_OR, tmp, tHi, ARMRI84_R(tLo)));
      addInstr(env, ARMInstr_CmpOrTst(True/*isCmp*/, tmp, zero));
      return ARMcc_NE;
   }

   /* --- Cmp*32*(x,y) --- */
   if (e->tag == Iex_Binop
       && (e->Iex.Binop.op == Iop_CmpEQ32
           || e->Iex.Binop.op == Iop_CmpNE32
           || e->Iex.Binop.op == Iop_CmpLT32S
           || e->Iex.Binop.op == Iop_CmpLT32U
           || e->Iex.Binop.op == Iop_CmpLE32S
           || e->Iex.Binop.op == Iop_CmpLE32U)) {
      HReg     argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
      ARMRI84* argR = iselIntExpr_RI84(NULL,False, 
                                       env, e->Iex.Binop.arg2);
      addInstr(env, ARMInstr_CmpOrTst(True/*isCmp*/, argL, argR));
      switch (e->Iex.Binop.op) {
         case Iop_CmpEQ32:  return ARMcc_EQ;
         case Iop_CmpNE32:  return ARMcc_NE;
         case Iop_CmpLT32S: return ARMcc_LT;
         case Iop_CmpLT32U: return ARMcc_LO;
         case Iop_CmpLE32S: return ARMcc_LE;
         case Iop_CmpLE32U: return ARMcc_LS;
         default: vpanic("iselCondCode(arm): CmpXX32");
      }
   }

   ppIRExpr(e);
   vpanic("iselCondCode");
}


/* --------------------- Reg --------------------- */

static HReg iselIntExpr_R ( ISelEnv* env, IRExpr* e )
{
   HReg r = iselIntExpr_R_wrk(env, e);
   /* sanity checks ... */
#  if 0
   vex_printf("\n"); ppIRExpr(e); vex_printf("\n");
#  endif
   vassert(hregClass(r) == HRcInt32);
   vassert(hregIsVirtual(r));
   return r;
}

/* DO NOT CALL THIS DIRECTLY ! */
static HReg iselIntExpr_R_wrk ( ISelEnv* env, IRExpr* e )
{
//zz   MatchInfo mi;

   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(ty == Ity_I32 || ty == Ity_I16 || ty == Ity_I8);

   switch (e->tag) {

   /* --------- TEMP --------- */
   case Iex_RdTmp: {
      return lookupIRTemp(env, e->Iex.RdTmp.tmp);
   }

   /* --------- LOAD --------- */
   case Iex_Load: {
      HReg dst  = newVRegI(env);
      Bool isLL = e->Iex.Load.isLL;

      if (e->Iex.Load.end != Iend_LE)
         goto irreducible;

      /* Normal (non-Load-Linked) cases */
      if (ty == Ity_I32 && !isLL) {
         ARMAMode1* amode = iselIntExpr_AMode1 ( env, e->Iex.Load.addr );
         addInstr(env, ARMInstr_LdSt32(True/*isLoad*/, dst, amode));
         return dst;
      }
      if (ty == Ity_I16 && !isLL) {
         ARMAMode2* amode = iselIntExpr_AMode2 ( env, e->Iex.Load.addr );
         addInstr(env, ARMInstr_LdSt16(True/*isLoad*/, False/*!signedLoad*/,
                                       dst, amode));
         return dst;
      }
      if (ty == Ity_I8 && !isLL) {
         ARMAMode1* amode = iselIntExpr_AMode1 ( env, e->Iex.Load.addr );
         addInstr(env, ARMInstr_LdSt8U(True/*isLoad*/, dst, amode));
         return dst;
      }

      /* Load-Linked cases */
      if (isLL && (ty == Ity_I32 || ty == Ity_I8)) {
         Int  szB   = 0;
         HReg raddr = iselIntExpr_R ( env, e->Iex.Load.addr );
         switch (ty) {
            case Ity_I8:  szB = 1; break;
            case Ity_I32: szB = 4; break;
            default:      vassert(0);
         }
         addInstr(env, mk_iMOVds_RR(hregARM_R1(), raddr));
         addInstr(env, ARMInstr_LdrEX(szB));
         addInstr(env, mk_iMOVds_RR(dst, hregARM_R0()));
         return dst;
      }

//zz      if (ty == Ity_I16) {
//zz         addInstr(env, X86Instr_LoadEX(2,False,amode,dst));
//zz         return dst;
//zz      }
//zz      if (ty == Ity_I8) {
//zz         addInstr(env, X86Instr_LoadEX(1,False,amode,dst));
//zz         return dst;
//zz      }
      break;
   }

//zz   /* --------- TERNARY OP --------- */
//zz   case Iex_Triop: {
//zz      /* C3210 flags following FPU partial remainder (fprem), both
//zz         IEEE compliant (PREM1) and non-IEEE compliant (PREM). */
//zz      if (e->Iex.Triop.op == Iop_PRemC3210F64
//zz          || e->Iex.Triop.op == Iop_PRem1C3210F64) {
//zz         HReg junk = newVRegF(env);
//zz         HReg dst  = newVRegI(env);
//zz         HReg srcL = iselDblExpr(env, e->Iex.Triop.arg2);
//zz         HReg srcR = iselDblExpr(env, e->Iex.Triop.arg3);
//zz         /* XXXROUNDINGFIXME */
//zz         /* set roundingmode here */
//zz         addInstr(env, X86Instr_FpBinary(
//zz                           e->Iex.Binop.op==Iop_PRemC3210F64 
//zz                              ? Xfp_PREM : Xfp_PREM1,
//zz                           srcL,srcR,junk
//zz                 ));
//zz         /* The previous pseudo-insn will have left the FPU's C3210
//zz            flags set correctly.  So bag them. */
//zz         addInstr(env, X86Instr_FpStSW_AX());
//zz         addInstr(env, mk_iMOVsd_RR(hregX86_EAX(), dst));
//zz         addInstr(env, X86Instr_Alu32R(Xalu_AND, X86RMI_Imm(0x4700), dst));
//zz         return dst;
//zz      }
//zz
//zz      break;
//zz   }

   /* --------- BINARY OP --------- */
   case Iex_Binop: {

      ARMAluOp   aop = 0; /* invalid */
      ARMShiftOp sop = 0; /* invalid */

      /* ADD/SUB/AND/OR/XOR */
      switch (e->Iex.Binop.op) {
         case Iop_And32: {
            Bool     didInv = False;
            HReg     dst    = newVRegI(env);
            HReg     argL   = iselIntExpr_R(env, e->Iex.Binop.arg1);
            ARMRI84* argR   = iselIntExpr_RI84(&didInv, True/*mayInv*/,
                                               env, e->Iex.Binop.arg2);
            addInstr(env, ARMInstr_Alu(didInv ? ARMalu_BIC : ARMalu_AND,
                                       dst, argL, argR));
            return dst;
         }
         case Iop_Or32:  aop = ARMalu_OR;  goto std_binop;
         case Iop_Xor32: aop = ARMalu_XOR; goto std_binop;
         case Iop_Sub32: aop = ARMalu_SUB; goto std_binop;
         case Iop_Add32: aop = ARMalu_ADD; goto std_binop;
         std_binop: {
            HReg     dst  = newVRegI(env);
            HReg     argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
            ARMRI84* argR = iselIntExpr_RI84(NULL, False/*mayInv*/,
                                             env, e->Iex.Binop.arg2);
            addInstr(env, ARMInstr_Alu(aop, dst, argL, argR));
            return dst;
         }
         default: break;
      }

      /* SHL/SHR/SAR */
      switch (e->Iex.Binop.op) {
         case Iop_Shl32: sop = ARMsh_SHL; goto sh_binop;
         case Iop_Shr32: sop = ARMsh_SHR; goto sh_binop;
         case Iop_Sar32: sop = ARMsh_SAR; goto sh_binop;
         sh_binop: {
            HReg    dst  = newVRegI(env);
            HReg    argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
            ARMRI5* argR = iselIntExpr_RI5(env, e->Iex.Binop.arg2);
            addInstr(env, ARMInstr_Shift(sop, dst, argL, argR));
            vassert(ty == Ity_I32); /* else the IR is ill-typed */
            return dst;
         }
         default: break;
      }

      /* MUL */
      if (e->Iex.Binop.op == Iop_Mul32) {
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         HReg dst  = newVRegI(env);
         addInstr(env, mk_iMOVds_RR(hregARM_R2(), argL));
         addInstr(env, mk_iMOVds_RR(hregARM_R3(), argR));
         addInstr(env, ARMInstr_Mul(ARMmul_PLAIN));
         addInstr(env, mk_iMOVds_RR(dst, hregARM_R0()));
         return dst;
      }

      /* Handle misc other ops. */

      if (e->Iex.Binop.op == Iop_Max32U) {
         HReg argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
         HReg argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
         HReg dst  = newVRegI(env);
         addInstr(env, ARMInstr_CmpOrTst(True/*isCmp*/, argL, ARMRI84_R(argR)));
         addInstr(env, mk_iMOVds_RR(dst, argL));
         addInstr(env, ARMInstr_CMov(ARMcc_LO, dst, ARMRI84_R(argR)));
         return dst;
      }

      if (e->Iex.Binop.op == Iop_CmpF64) {
         HReg dL = iselDblExpr(env, e->Iex.Binop.arg1);
         HReg dR = iselDblExpr(env, e->Iex.Binop.arg2);
         HReg dst = newVRegI(env);
         /* Do the compare (FCMPD) and set NZCV in FPSCR.  Then also do
            FMSTAT, so we can examine the results directly. */
         addInstr(env, ARMInstr_VCmpD(dL, dR));
         /* Create in dst, the IRCmpF64Result encoded result. */
         addInstr(env, ARMInstr_Imm32(dst, 0));
         addInstr(env, ARMInstr_CMov(ARMcc_EQ, dst, ARMRI84_I84(0x40,0))); //EQ
         addInstr(env, ARMInstr_CMov(ARMcc_MI, dst, ARMRI84_I84(0x01,0))); //LT
         addInstr(env, ARMInstr_CMov(ARMcc_GT, dst, ARMRI84_I84(0x00,0))); //GT
         addInstr(env, ARMInstr_CMov(ARMcc_VS, dst, ARMRI84_I84(0x45,0))); //UN
         return dst;
      }

      if (e->Iex.Binop.op == Iop_F64toI32S
          || e->Iex.Binop.op == Iop_F64toI32U) {
         /* Wretched uglyness all round, due to having to deal
            with rounding modes.  Oh well. */
         /* FIXME: if arg1 is a constant indicating round-to-zero,
            then we could skip all this arsing around with FPSCR and
            simply emit FTO{S,U}IZD. */
         Bool syned = e->Iex.Binop.op == Iop_F64toI32S;
         HReg valD  = iselDblExpr(env, e->Iex.Binop.arg2);
         set_VFP_rounding_mode(env, e->Iex.Binop.arg1);
         /* FTO{S,U}ID valF, valD */
         HReg valF = newVRegF(env);
         addInstr(env, ARMInstr_VCvtID(False/*!iToD*/, syned,
                                       valF, valD));
         set_VFP_rounding_default(env);
         /* VMOV dst, valF */
         HReg dst = newVRegI(env);
         addInstr(env, ARMInstr_VXferS(False/*!toS*/, valF, dst));
         return dst;
      }

      break;
   }

   /* --------- UNARY OP --------- */
   case Iex_Unop: {

//zz      /* 1Uto8(32to1(expr32)) */
//zz      if (e->Iex.Unop.op == Iop_1Uto8) { 
//zz         DECLARE_PATTERN(p_32to1_then_1Uto8);
//zz         DEFINE_PATTERN(p_32to1_then_1Uto8,
//zz                        unop(Iop_1Uto8,unop(Iop_32to1,bind(0))));
//zz         if (matchIRExpr(&mi,p_32to1_then_1Uto8,e)) {
//zz            IRExpr* expr32 = mi.bindee[0];
//zz            HReg dst = newVRegI(env);
//zz            HReg src = iselIntExpr_R(env, expr32);
//zz            addInstr(env, mk_iMOVsd_RR(src,dst) );
//zz            addInstr(env, X86Instr_Alu32R(Xalu_AND,
//zz                                          X86RMI_Imm(1), dst));
//zz            return dst;
//zz         }
//zz      }
//zz
//zz      /* 8Uto32(LDle(expr32)) */
//zz      if (e->Iex.Unop.op == Iop_8Uto32) {
//zz         DECLARE_PATTERN(p_LDle8_then_8Uto32);
//zz         DEFINE_PATTERN(p_LDle8_then_8Uto32,
//zz                        unop(Iop_8Uto32,
//zz                             IRExpr_Load(Iend_LE,Ity_I8,bind(0))) );
//zz         if (matchIRExpr(&mi,p_LDle8_then_8Uto32,e)) {
//zz            HReg dst = newVRegI(env);
//zz            X86AMode* amode = iselIntExpr_AMode ( env, mi.bindee[0] );
//zz            addInstr(env, X86Instr_LoadEX(1,False,amode,dst));
//zz            return dst;
//zz         }
//zz      }
//zz
//zz      /* 8Sto32(LDle(expr32)) */
//zz      if (e->Iex.Unop.op == Iop_8Sto32) {
//zz         DECLARE_PATTERN(p_LDle8_then_8Sto32);
//zz         DEFINE_PATTERN(p_LDle8_then_8Sto32,
//zz                        unop(Iop_8Sto32,
//zz                             IRExpr_Load(Iend_LE,Ity_I8,bind(0))) );
//zz         if (matchIRExpr(&mi,p_LDle8_then_8Sto32,e)) {
//zz            HReg dst = newVRegI(env);
//zz            X86AMode* amode = iselIntExpr_AMode ( env, mi.bindee[0] );
//zz            addInstr(env, X86Instr_LoadEX(1,True,amode,dst));
//zz            return dst;
//zz         }
//zz      }
//zz
//zz      /* 16Uto32(LDle(expr32)) */
//zz      if (e->Iex.Unop.op == Iop_16Uto32) {
//zz         DECLARE_PATTERN(p_LDle16_then_16Uto32);
//zz         DEFINE_PATTERN(p_LDle16_then_16Uto32,
//zz                        unop(Iop_16Uto32,
//zz                             IRExpr_Load(Iend_LE,Ity_I16,bind(0))) );
//zz         if (matchIRExpr(&mi,p_LDle16_then_16Uto32,e)) {
//zz            HReg dst = newVRegI(env);
//zz            X86AMode* amode = iselIntExpr_AMode ( env, mi.bindee[0] );
//zz            addInstr(env, X86Instr_LoadEX(2,False,amode,dst));
//zz            return dst;
//zz         }
//zz      }
//zz
//zz      /* 8Uto32(GET:I8) */
//zz      if (e->Iex.Unop.op == Iop_8Uto32) {
//zz         if (e->Iex.Unop.arg->tag == Iex_Get) {
//zz            HReg      dst;
//zz            X86AMode* amode;
//zz            vassert(e->Iex.Unop.arg->Iex.Get.ty == Ity_I8);
//zz            dst = newVRegI(env);
//zz            amode = X86AMode_IR(e->Iex.Unop.arg->Iex.Get.offset,
//zz                                hregX86_EBP());
//zz            addInstr(env, X86Instr_LoadEX(1,False,amode,dst));
//zz            return dst;
//zz         }
//zz      }
//zz
//zz      /* 16to32(GET:I16) */
//zz      if (e->Iex.Unop.op == Iop_16Uto32) {
//zz         if (e->Iex.Unop.arg->tag == Iex_Get) {
//zz            HReg      dst;
//zz            X86AMode* amode;
//zz            vassert(e->Iex.Unop.arg->Iex.Get.ty == Ity_I16);
//zz            dst = newVRegI(env);
//zz            amode = X86AMode_IR(e->Iex.Unop.arg->Iex.Get.offset,
//zz                                hregX86_EBP());
//zz            addInstr(env, X86Instr_LoadEX(2,False,amode,dst));
//zz            return dst;
//zz         }
//zz      }

      switch (e->Iex.Unop.op) {
         case Iop_8Uto32: {
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            addInstr(env, ARMInstr_Alu(ARMalu_AND,
                                       dst, src, ARMRI84_I84(0xFF,0)));
            return dst;
         }
//zz         case Iop_8Uto16:
//zz         case Iop_8Uto32:
//zz         case Iop_16Uto32: {
//zz            HReg dst = newVRegI(env);
//zz            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
//zz            UInt mask = e->Iex.Unop.op==Iop_16Uto32 ? 0xFFFF : 0xFF;
//zz            addInstr(env, mk_iMOVsd_RR(src,dst) );
//zz            addInstr(env, X86Instr_Alu32R(Xalu_AND,
//zz                                          X86RMI_Imm(mask), dst));
//zz            return dst;
//zz         }
//zz         case Iop_8Sto16:
//zz         case Iop_8Sto32:
         case Iop_16Uto32: {
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            ARMRI5* amt = ARMRI5_I5(16);
            addInstr(env, ARMInstr_Shift(ARMsh_SHL, dst, src, amt));
            addInstr(env, ARMInstr_Shift(ARMsh_SHR, dst, dst, amt));
            return dst;
         }
         case Iop_8Sto32:
         case Iop_16Sto32: {
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            ARMRI5* amt = ARMRI5_I5(e->Iex.Unop.op==Iop_16Sto32 ? 16 : 24);
            addInstr(env, ARMInstr_Shift(ARMsh_SHL, dst, src, amt));
            addInstr(env, ARMInstr_Shift(ARMsh_SAR, dst, dst, amt));
            return dst;
         }
//zz         case Iop_Not8:
//zz         case Iop_Not16:
         case Iop_Not32: {
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            addInstr(env, ARMInstr_Unary(ARMun_NOT, dst, src));
            return dst;
         }
         case Iop_64HIto32: {
            HReg rHi, rLo;
            iselInt64Expr(&rHi,&rLo, env, e->Iex.Unop.arg);
            return rHi; /* and abandon rLo .. poor wee thing :-) */
         }
         case Iop_64to32: {
            HReg rHi, rLo;
            iselInt64Expr(&rHi,&rLo, env, e->Iex.Unop.arg);
            return rLo; /* similar stupid comment to the above ... */
         }
//zz         case Iop_16HIto8:
//zz         case Iop_32HIto16: {
//zz            HReg dst  = newVRegI(env);
//zz            HReg src  = iselIntExpr_R(env, e->Iex.Unop.arg);
//zz            Int shift = e->Iex.Unop.op == Iop_16HIto8 ? 8 : 16;
//zz            addInstr(env, mk_iMOVsd_RR(src,dst) );
//zz            addInstr(env, X86Instr_Sh32(Xsh_SHR, shift, dst));
//zz            return dst;
//zz         }
         case Iop_1Uto32:
         case Iop_1Uto8: {
            HReg        dst  = newVRegI(env);
            ARMCondCode cond = iselCondCode(env, e->Iex.Unop.arg);
            addInstr(env, ARMInstr_Mov(dst, ARMRI84_I84(0,0)));
            addInstr(env, ARMInstr_CMov(cond, dst, ARMRI84_I84(1,0)));
            return dst;
         }

         case Iop_1Sto32: {
            HReg        dst  = newVRegI(env);
            ARMCondCode cond = iselCondCode(env, e->Iex.Unop.arg);
            ARMRI5*     amt  = ARMRI5_I5(31);
            /* This is really rough.  We could do much better here;
               perhaps mvn{cond} dst, #0 as the second insn?
               (same applies to 1Sto64) */
            addInstr(env, ARMInstr_Mov(dst, ARMRI84_I84(0,0)));
            addInstr(env, ARMInstr_CMov(cond, dst, ARMRI84_I84(1,0)));
            addInstr(env, ARMInstr_Shift(ARMsh_SHL, dst, dst, amt));
            addInstr(env, ARMInstr_Shift(ARMsh_SAR, dst, dst, amt));
            return dst;
         }


//zz         case Iop_1Sto8:
//zz         case Iop_1Sto16:
//zz         case Iop_1Sto32: {
//zz            /* could do better than this, but for now ... */
//zz            HReg dst         = newVRegI(env);
//zz            X86CondCode cond = iselCondCode(env, e->Iex.Unop.arg);
//zz            addInstr(env, X86Instr_Set32(cond,dst));
//zz            addInstr(env, X86Instr_Sh32(Xsh_SHL, 31, dst));
//zz            addInstr(env, X86Instr_Sh32(Xsh_SAR, 31, dst));
//zz            return dst;
//zz         }
//zz         case Iop_Ctz32: {
//zz            /* Count trailing zeroes, implemented by x86 'bsfl' */
//zz            HReg dst = newVRegI(env);
//zz            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
//zz            addInstr(env, X86Instr_Bsfr32(True,src,dst));
//zz            return dst;
//zz         }
         case Iop_Clz32: {
            /* Count leading zeroes; easy on ARM. */
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            addInstr(env, ARMInstr_Unary(ARMun_CLZ, dst, src));
            return dst;
         }

         case Iop_CmpwNEZ32: {
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            addInstr(env, ARMInstr_Unary(ARMun_NEG, dst, src));
            addInstr(env, ARMInstr_Alu(ARMalu_OR, dst, dst, ARMRI84_R(src)));
            addInstr(env, ARMInstr_Shift(ARMsh_SAR, dst, dst, ARMRI5_I5(31)));
            return dst;
         }

         case Iop_Left32: {
            HReg dst = newVRegI(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            addInstr(env, ARMInstr_Unary(ARMun_NEG, dst, src));
            addInstr(env, ARMInstr_Alu(ARMalu_OR, dst, dst, ARMRI84_R(src)));
            return dst;
         }

//zz         case Iop_V128to32: {
//zz            HReg      dst  = newVRegI(env);
//zz            HReg      vec  = iselVecExpr(env, e->Iex.Unop.arg);
//zz            X86AMode* esp0 = X86AMode_IR(0, hregX86_ESP());
//zz            sub_from_esp(env, 16);
//zz            addInstr(env, X86Instr_SseLdSt(False/*store*/, vec, esp0));
//zz            addInstr(env, X86Instr_Alu32R( Xalu_MOV, X86RMI_Mem(esp0), dst ));
//zz            add_to_esp(env, 16);
//zz            return dst;
//zz         }
//zz
         case Iop_ReinterpF32asI32: {
            HReg dst = newVRegI(env);
            HReg src = iselFltExpr(env, e->Iex.Unop.arg);
            addInstr(env, ARMInstr_VXferS(False/*!toS*/, src, dst));
            return dst;
         }

//zz
//zz         case Iop_16to8:
         case Iop_32to8:
         case Iop_32to16:
            /* These are no-ops. */
            return iselIntExpr_R(env, e->Iex.Unop.arg);

         default: 
            break;
      }
      break;
   }

   /* --------- GET --------- */
   case Iex_Get: {
      if (ty == Ity_I32 
          && 0 == (e->Iex.Get.offset & 3)
          && e->Iex.Get.offset < 4096-4) {
         HReg dst = newVRegI(env);
         addInstr(env, ARMInstr_LdSt32(
                          True/*isLoad*/,
                          dst,
                          ARMAMode1_RI(hregARM_R8(), e->Iex.Get.offset)));
         return dst;
      }
//zz      if (ty == Ity_I8 || ty == Ity_I16) {
//zz         HReg dst = newVRegI(env);
//zz         addInstr(env, X86Instr_LoadEX(
//zz                          toUChar(ty==Ity_I8 ? 1 : 2),
//zz                          False,
//zz                          X86AMode_IR(e->Iex.Get.offset,hregX86_EBP()),
//zz                          dst));
//zz         return dst;
//zz      }
      break;
   }

//zz   case Iex_GetI: {
//zz      X86AMode* am 
//zz         = genGuestArrayOffset(
//zz              env, e->Iex.GetI.descr, 
//zz                   e->Iex.GetI.ix, e->Iex.GetI.bias );
//zz      HReg dst = newVRegI(env);
//zz      if (ty == Ity_I8) {
//zz         addInstr(env, X86Instr_LoadEX( 1, False, am, dst ));
//zz         return dst;
//zz      }
//zz      if (ty == Ity_I32) {
//zz         addInstr(env, X86Instr_Alu32R(Xalu_MOV, X86RMI_Mem(am), dst));
//zz         return dst;
//zz      }
//zz      break;
//zz   }

   /* --------- CCALL --------- */
   case Iex_CCall: {
      HReg    dst = newVRegI(env);
      vassert(ty == e->Iex.CCall.retty);

      /* be very restrictive for now.  Only 32/64-bit ints allowed
         for args, and 32 bits for return type. */
      if (e->Iex.CCall.retty != Ity_I32)
         goto irreducible;

      /* Marshal args, do the call, clear stack. */
      Bool ok = doHelperCall( env, False,
                              NULL, e->Iex.CCall.cee, e->Iex.CCall.args );
      if (ok) {
         addInstr(env, mk_iMOVds_RR(dst, hregARM_R0()));
         return dst;
      }
      /* else fall through; will hit the irreducible: label */
   }

   /* --------- LITERAL --------- */
   /* 32 literals */
   case Iex_Const: {
      UInt u   = 0;
      HReg dst = newVRegI(env);
      switch (e->Iex.Const.con->tag) {
         case Ico_U32: u = e->Iex.Const.con->Ico.U32; break;
         case Ico_U16: u = 0xFFFF & (e->Iex.Const.con->Ico.U16); break;
         case Ico_U8:  u = 0xFF   & (e->Iex.Const.con->Ico.U8); break;
         default: vpanic("iselIntExpr_R.Iex_Const(arm)");
      }
      addInstr(env, ARMInstr_Imm32(dst, u));
      return dst;
   }

   /* --------- MULTIPLEX --------- */
   case Iex_Mux0X: {
      IRExpr* cond = e->Iex.Mux0X.cond;

      /* Mux0X( 32to8(1Uto32(ccexpr)), expr0, exprX ) */
      if (ty == Ity_I32
          && cond->tag == Iex_Unop
          && cond->Iex.Unop.op == Iop_32to8
          && cond->Iex.Unop.arg->tag == Iex_Unop
          && cond->Iex.Unop.arg->Iex.Unop.op == Iop_1Uto32) {
         ARMCondCode cc;
         HReg     rX  = iselIntExpr_R(env, e->Iex.Mux0X.exprX);
         ARMRI84* r0  = iselIntExpr_RI84(NULL, False, env, e->Iex.Mux0X.expr0);
         HReg     dst = newVRegI(env);
         addInstr(env, mk_iMOVds_RR(dst, rX));
         cc = iselCondCode(env, cond->Iex.Unop.arg->Iex.Unop.arg);
         addInstr(env, ARMInstr_CMov(cc ^ 1, dst, r0));
         return dst;
      }

      /* Mux0X(cond, expr0, exprX) (general case) */
      if (ty == Ity_I32) {
         HReg     r8;
         HReg     rX  = iselIntExpr_R(env, e->Iex.Mux0X.exprX);
         ARMRI84* r0  = iselIntExpr_RI84(NULL, False, env, e->Iex.Mux0X.expr0);
         HReg     dst = newVRegI(env);
         addInstr(env, mk_iMOVds_RR(dst, rX));
         r8 = iselIntExpr_R(env, cond);
         addInstr(env, ARMInstr_CmpOrTst(False/*!isCmp*/, r8,
                                         ARMRI84_I84(0xFF,0)));
         addInstr(env, ARMInstr_CMov(ARMcc_EQ, dst, r0));
         return dst;
      }
      break;
   }

   default: 
   break;
   } /* switch (e->tag) */

   /* We get here if no pattern matched. */
  irreducible:
   ppIRExpr(e);
   vpanic("iselIntExpr_R: cannot reduce tree");
}


/* -------------------- 64-bit -------------------- */

/* Compute a 64-bit value into a register pair, which is returned as
   the first two parameters.  As with iselIntExpr_R, these may be
   either real or virtual regs; in any case they must not be changed
   by subsequent code emitted by the caller.  */

static void iselInt64Expr ( HReg* rHi, HReg* rLo, ISelEnv* env, IRExpr* e )
{
   iselInt64Expr_wrk(rHi, rLo, env, e);
#  if 0
   vex_printf("\n"); ppIRExpr(e); vex_printf("\n");
#  endif
   vassert(hregClass(*rHi) == HRcInt32);
   vassert(hregIsVirtual(*rHi));
   vassert(hregClass(*rLo) == HRcInt32);
   vassert(hregIsVirtual(*rLo));
}

/* DO NOT CALL THIS DIRECTLY ! */
static void iselInt64Expr_wrk ( HReg* rHi, HReg* rLo, ISelEnv* env, IRExpr* e )
{
   vassert(e);
   vassert(typeOfIRExpr(env->type_env,e) == Ity_I64);

   /* 64-bit literal */
   if (e->tag == Iex_Const) {
      ULong   w64 = e->Iex.Const.con->Ico.U64;
      UInt    wHi = toUInt(w64 >> 32);
      UInt    wLo = toUInt(w64);
      HReg    tHi = newVRegI(env);
      HReg    tLo = newVRegI(env);
      vassert(e->Iex.Const.con->tag == Ico_U64);
      addInstr(env, ARMInstr_Imm32(tHi, wHi));
      addInstr(env, ARMInstr_Imm32(tLo, wLo));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* read 64-bit IRTemp */
   if (e->tag == Iex_RdTmp) {
      lookupIRTemp64( rHi, rLo, env, e->Iex.RdTmp.tmp);
      return;
   }

   /* 64-bit load */
   if (e->tag == Iex_Load && e->Iex.Load.end == Iend_LE && !e->Iex.Load.isLL) {
      HReg      tLo, tHi, rA;
      vassert(e->Iex.Load.ty == Ity_I64);
      rA  = iselIntExpr_R(env, e->Iex.Load.addr);
      tHi = newVRegI(env);
      tLo = newVRegI(env);
      addInstr(env, ARMInstr_LdSt32(True/*isLoad*/, tHi, ARMAMode1_RI(rA, 4)));
      addInstr(env, ARMInstr_LdSt32(True/*isLoad*/, tLo, ARMAMode1_RI(rA, 0)));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* 64-bit GET */
   if (e->tag == Iex_Get) {
      ARMAMode1* am0 = ARMAMode1_RI(hregARM_R8(), e->Iex.Get.offset + 0);
      ARMAMode1* am4 = ARMAMode1_RI(hregARM_R8(), e->Iex.Get.offset + 4);
      HReg tHi = newVRegI(env);
      HReg tLo = newVRegI(env);
      addInstr(env, ARMInstr_LdSt32(True/*isLoad*/, tHi, am4));
      addInstr(env, ARMInstr_LdSt32(True/*isLoad*/, tLo, am0));
      *rHi = tHi;
      *rLo = tLo;
      return;
   }

   /* --------- BINARY ops --------- */
   if (e->tag == Iex_Binop) {
      switch (e->Iex.Binop.op) {

         /* 32 x 32 -> 64 multiply */
         case Iop_MullS32:
         case Iop_MullU32: {
            HReg     argL = iselIntExpr_R(env, e->Iex.Binop.arg1);
            HReg     argR = iselIntExpr_R(env, e->Iex.Binop.arg2);
            HReg     tHi  = newVRegI(env);
            HReg     tLo  = newVRegI(env);
            ARMMulOp mop  = e->Iex.Binop.op == Iop_MullS32
                               ? ARMmul_SX : ARMmul_ZX;
            addInstr(env, mk_iMOVds_RR(hregARM_R2(), argL));
            addInstr(env, mk_iMOVds_RR(hregARM_R3(), argR));
            addInstr(env, ARMInstr_Mul(mop));
            addInstr(env, mk_iMOVds_RR(tHi, hregARM_R1()));
            addInstr(env, mk_iMOVds_RR(tLo, hregARM_R0()));
            *rHi = tHi;
            *rLo = tLo;
            return;
         }

         case Iop_Or64: {
            HReg xLo, xHi, yLo, yHi;
            HReg tHi = newVRegI(env);
            HReg tLo = newVRegI(env);
            iselInt64Expr(&xHi, &xLo, env, e->Iex.Binop.arg1);
            iselInt64Expr(&yHi, &yLo, env, e->Iex.Binop.arg2);
            addInstr(env, ARMInstr_Alu(ARMalu_OR, tHi, xHi, ARMRI84_R(yHi)));
            addInstr(env, ARMInstr_Alu(ARMalu_OR, tLo, xLo, ARMRI84_R(yLo)));
            *rHi = tHi;
            *rLo = tLo;
            return;
         }

         case Iop_Add64: {
            HReg xLo, xHi, yLo, yHi;
            HReg tHi = newVRegI(env);
            HReg tLo = newVRegI(env);
            iselInt64Expr(&xHi, &xLo, env, e->Iex.Binop.arg1);
            iselInt64Expr(&yHi, &yLo, env, e->Iex.Binop.arg2);
            addInstr(env, ARMInstr_Alu(ARMalu_ADDS, tLo, xLo, ARMRI84_R(yLo)));
            addInstr(env, ARMInstr_Alu(ARMalu_ADC,  tHi, xHi, ARMRI84_R(yHi)));
            *rHi = tHi;
            *rLo = tLo;
            return;
         }

         /* 32HLto64(e1,e2) */
         case Iop_32HLto64: {
            *rHi = iselIntExpr_R(env, e->Iex.Binop.arg1);
            *rLo = iselIntExpr_R(env, e->Iex.Binop.arg2);
            return;
         }

         default:
            break;
      }
   }

   /* --------- UNARY ops --------- */
   if (e->tag == Iex_Unop) {
      switch (e->Iex.Unop.op) {

         /* ReinterpF64asI64 */
         case Iop_ReinterpF64asI64: {
            HReg dstHi = newVRegI(env);
            HReg dstLo = newVRegI(env);
            HReg src   = iselDblExpr(env, e->Iex.Unop.arg);
            addInstr(env, ARMInstr_VXferD(False/*!toD*/, src, dstHi, dstLo));
            *rHi = dstHi;
            *rLo = dstLo;
            return;
         }

         /* Left64(e) */
         case Iop_Left64: {
            HReg yLo, yHi;
            HReg tHi  = newVRegI(env);
            HReg tLo  = newVRegI(env);
            HReg zero = newVRegI(env);
            /* yHi:yLo = arg */
            iselInt64Expr(&yHi, &yLo, env, e->Iex.Unop.arg);
            /* zero = 0 */
            addInstr(env, ARMInstr_Imm32(zero, 0));
            /* tLo = 0 - yLo, and set carry */
            addInstr(env, ARMInstr_Alu(ARMalu_SUBS, tLo, zero, ARMRI84_R(yLo)));
            /* tHi = 0 - yHi - carry */
            addInstr(env, ARMInstr_Alu(ARMalu_SBC,  tHi, zero, ARMRI84_R(yHi)));
            /* So now we have tHi:tLo = -arg.  To finish off, or 'arg'
               back in, so as to give the final result 
               tHi:tLo = arg | -arg. */
            addInstr(env, ARMInstr_Alu(ARMalu_OR, tHi, tHi, ARMRI84_R(yHi)));
            addInstr(env, ARMInstr_Alu(ARMalu_OR, tLo, tLo, ARMRI84_R(yLo)));
            *rHi = tHi;
            *rLo = tLo;
            return;
         }

         /* CmpwNEZ64(e) */
         case Iop_CmpwNEZ64: {
            HReg srcLo, srcHi;
            HReg tmp1 = newVRegI(env);
            HReg tmp2 = newVRegI(env);
            /* srcHi:srcLo = arg */
            iselInt64Expr(&srcHi, &srcLo, env, e->Iex.Unop.arg);
            /* tmp1 = srcHi | srcLo */
            addInstr(env, ARMInstr_Alu(ARMalu_OR,
                                       tmp1, srcHi, ARMRI84_R(srcLo)));
            /* tmp2 = (tmp1 | -tmp1) >>s 31 */
            addInstr(env, ARMInstr_Unary(ARMun_NEG, tmp2, tmp1));
            addInstr(env, ARMInstr_Alu(ARMalu_OR,
                                       tmp2, tmp2, ARMRI84_R(tmp1)));
            addInstr(env, ARMInstr_Shift(ARMsh_SAR,
                                         tmp2, tmp2, ARMRI5_I5(31)));
            *rHi = tmp2;
            *rLo = tmp2;
            return;
         }

         case Iop_1Sto64: {
            HReg        dst  = newVRegI(env);
            ARMCondCode cond = iselCondCode(env, e->Iex.Unop.arg);
            ARMRI5*     amt  = ARMRI5_I5(31);
            /* This is really rough.  We could do much better here;
               perhaps mvn{cond} dst, #0 as the second insn?
               (same applies to 1Sto32) */
            addInstr(env, ARMInstr_Mov(dst, ARMRI84_I84(0,0)));
            addInstr(env, ARMInstr_CMov(cond, dst, ARMRI84_I84(1,0)));
            addInstr(env, ARMInstr_Shift(ARMsh_SHL, dst, dst, amt));
            addInstr(env, ARMInstr_Shift(ARMsh_SAR, dst, dst, amt));
            *rHi = dst;
            *rLo = dst;
            return;
         }

         default: 
            break;
      }
   } /* if (e->tag == Iex_Unop) */

   /* --------- MULTIPLEX --------- */
   if (e->tag == Iex_Mux0X) {
      IRType ty8;
      HReg   r8, rXhi, rXlo, r0hi, r0lo, dstHi, dstLo;
      ty8 = typeOfIRExpr(env->type_env,e->Iex.Mux0X.cond);
      vassert(ty8 == Ity_I8);
      iselInt64Expr(&rXhi, &rXlo, env, e->Iex.Mux0X.exprX);
      iselInt64Expr(&r0hi, &r0lo, env, e->Iex.Mux0X.expr0);
      dstHi = newVRegI(env);
      dstLo = newVRegI(env);
      addInstr(env, mk_iMOVds_RR(dstHi, rXhi));
      addInstr(env, mk_iMOVds_RR(dstLo, rXlo));
      r8 = iselIntExpr_R(env, e->Iex.Mux0X.cond);
      addInstr(env, ARMInstr_CmpOrTst(False/*!isCmp*/, r8,
                                      ARMRI84_I84(0xFF,0)));
      addInstr(env, ARMInstr_CMov(ARMcc_EQ, dstHi, ARMRI84_R(r0hi)));
      addInstr(env, ARMInstr_CMov(ARMcc_EQ, dstLo, ARMRI84_R(r0lo)));
      *rHi = dstHi;
      *rLo = dstLo;
      return;
   }

   ppIRExpr(e);
   vpanic("iselInt64Expr");
}


/*---------------------------------------------------------*/
/*--- ISEL: Floating point expressions (64 bit)         ---*/
/*---------------------------------------------------------*/

/* Compute a 64-bit floating point value into a register, the identity
   of which is returned.  As with iselIntExpr_R, the reg may be either
   real or virtual; in any case it must not be changed by subsequent
   code emitted by the caller.  */

static HReg iselDblExpr ( ISelEnv* env, IRExpr* e )
{
   HReg r = iselDblExpr_wrk( env, e );
#  if 0
   vex_printf("\n"); ppIRExpr(e); vex_printf("\n");
#  endif
   vassert(hregClass(r) == HRcFlt64);
   vassert(hregIsVirtual(r));
   return r;
}

/* DO NOT CALL THIS DIRECTLY */
static HReg iselDblExpr_wrk ( ISelEnv* env, IRExpr* e )
{
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(e);
   vassert(ty == Ity_F64);

   if (e->tag == Iex_RdTmp) {
      return lookupIRTemp(env, e->Iex.RdTmp.tmp);
   }

   if (e->tag == Iex_Const) {
      /* Just handle the zero case. */
      IRConst* con = e->Iex.Const.con;
      if (con->tag == Ico_F64i && con->Ico.F64i == 0ULL) {
         HReg z32 = newVRegI(env);
         HReg dst = newVRegD(env);
         addInstr(env, ARMInstr_Imm32(z32, 0));
         addInstr(env, ARMInstr_VXferD(True/*toD*/, dst, z32, z32));
         return dst;
      }
   }

   if (e->tag == Iex_Load && e->Iex.Load.end == Iend_LE && !e->Iex.Load.isLL) {
      ARMAModeV* am;
      HReg res = newVRegD(env);
      vassert(e->Iex.Load.ty == Ity_F64);
      am = iselIntExpr_AModeV(env, e->Iex.Load.addr);
      addInstr(env, ARMInstr_VLdStD(True/*isLoad*/, res, am));
      return res;
   }

   if (e->tag == Iex_Get) {
      // XXX This won't work if offset > 1020 or is not 0 % 4.
      // In which case we'll have to generate more longwinded code.
      ARMAModeV* am  = mkARMAModeV(hregARM_R8(), e->Iex.Get.offset);
      HReg       res = newVRegD(env);
      addInstr(env, ARMInstr_VLdStD(True/*isLoad*/, res, am));
      return res;
   }

   if (e->tag == Iex_Unop) {
      switch (e->Iex.Unop.op) {
         case Iop_ReinterpI64asF64: {
            HReg srcHi, srcLo;
            HReg dst = newVRegD(env);
            iselInt64Expr(&srcHi, &srcLo, env, e->Iex.Unop.arg);
            addInstr(env, ARMInstr_VXferD(True/*toD*/, dst, srcHi, srcLo));
            return dst;
         }
         case Iop_NegF64: {
            HReg src = iselDblExpr(env, e->Iex.Unop.arg);
            HReg dst = newVRegD(env);
            addInstr(env, ARMInstr_VUnaryD(ARMvfpu_NEG, dst, src));
            return dst;
         }
         case Iop_AbsF64: {
            HReg src = iselDblExpr(env, e->Iex.Unop.arg);
            HReg dst = newVRegD(env);
            addInstr(env, ARMInstr_VUnaryD(ARMvfpu_ABS, dst, src));
            return dst;
         }
         case Iop_F32toF64: {
            HReg src = iselFltExpr(env, e->Iex.Unop.arg);
            HReg dst = newVRegD(env);
            addInstr(env, ARMInstr_VCvtSD(True/*sToD*/, dst, src));
            return dst;
         }
         case Iop_I32UtoF64:
         case Iop_I32StoF64: {
            HReg src   = iselIntExpr_R(env, e->Iex.Unop.arg);
            HReg f32   = newVRegF(env);
            HReg dst   = newVRegD(env);
            Bool syned = e->Iex.Unop.op == Iop_I32StoF64;
            /* VMOV f32, src */
            addInstr(env, ARMInstr_VXferS(True/*toS*/, f32, src));
            /* FSITOD dst, f32 */
            addInstr(env, ARMInstr_VCvtID(True/*iToD*/, syned,
                                          dst, f32));
            return dst;
         }
         default:
            break;
      }
   }

   if (e->tag == Iex_Binop) {
      switch (e->Iex.Binop.op) {
         case Iop_SqrtF64: {
            /* first arg is rounding mode; we ignore it. */
            HReg src = iselDblExpr(env, e->Iex.Binop.arg2);
            HReg dst = newVRegD(env);
            addInstr(env, ARMInstr_VUnaryD(ARMvfpu_SQRT, dst, src));
            return dst;
         }
         default:
            break;
      }
   }

   if (e->tag == Iex_Triop) {
      switch (e->Iex.Triop.op) {
         case Iop_DivF64:
         case Iop_MulF64:
         case Iop_AddF64:
         case Iop_SubF64: {
            ARMVfpOp op = 0; /*INVALID*/
            HReg argL = iselDblExpr(env, e->Iex.Triop.arg2);
            HReg argR = iselDblExpr(env, e->Iex.Triop.arg3);
            HReg dst  = newVRegD(env);
            switch (e->Iex.Triop.op) {
               case Iop_DivF64: op = ARMvfp_DIV; break;
               case Iop_MulF64: op = ARMvfp_MUL; break;
               case Iop_AddF64: op = ARMvfp_ADD; break;
               case Iop_SubF64: op = ARMvfp_SUB; break;
               default: vassert(0);
            }
            addInstr(env, ARMInstr_VAluD(op, dst, argL, argR));
            return dst;
         }
         default:
            break;
      }
   }

   if (e->tag == Iex_Mux0X) {
      if (ty == Ity_F64
          && typeOfIRExpr(env->type_env,e->Iex.Mux0X.cond) == Ity_I8) {
         HReg r8;
         HReg rX  = iselDblExpr(env, e->Iex.Mux0X.exprX);
         HReg r0  = iselDblExpr(env, e->Iex.Mux0X.expr0);
         HReg dst = newVRegD(env);
         addInstr(env, ARMInstr_VUnaryD(ARMvfpu_COPY, dst, rX));
         r8 = iselIntExpr_R(env, e->Iex.Mux0X.cond);
         addInstr(env, ARMInstr_CmpOrTst(False/*!isCmp*/, r8,
                                         ARMRI84_I84(0xFF,0)));
         addInstr(env, ARMInstr_VCMovD(ARMcc_EQ, dst, r0));
         return dst;
      }
   }

   ppIRExpr(e);
   vpanic("iselDblExpr_wrk");
}


/*---------------------------------------------------------*/
/*--- ISEL: Floating point expressions (32 bit)         ---*/
/*---------------------------------------------------------*/

/* Compute a 64-bit floating point value into a register, the identity
   of which is returned.  As with iselIntExpr_R, the reg may be either
   real or virtual; in any case it must not be changed by subsequent
   code emitted by the caller.  */

static HReg iselFltExpr ( ISelEnv* env, IRExpr* e )
{
   HReg r = iselFltExpr_wrk( env, e );
#  if 0
   vex_printf("\n"); ppIRExpr(e); vex_printf("\n");
#  endif
   vassert(hregClass(r) == HRcFlt32);
   vassert(hregIsVirtual(r));
   return r;
}

/* DO NOT CALL THIS DIRECTLY */
static HReg iselFltExpr_wrk ( ISelEnv* env, IRExpr* e )
{
   IRType ty = typeOfIRExpr(env->type_env,e);
   vassert(e);
   vassert(ty == Ity_F32);

   if (e->tag == Iex_RdTmp) {
      return lookupIRTemp(env, e->Iex.RdTmp.tmp);
   }

   if (e->tag == Iex_Load && e->Iex.Load.end == Iend_LE && !e->Iex.Load.isLL) {
      ARMAModeV* am;
      HReg res = newVRegF(env);
      vassert(e->Iex.Load.ty == Ity_F32);
      am = iselIntExpr_AModeV(env, e->Iex.Load.addr);
      addInstr(env, ARMInstr_VLdStS(True/*isLoad*/, res, am));
      return res;
   }

   if (e->tag == Iex_Get) {
      // XXX This won't work if offset > 1020 or is not 0 % 4.
      // In which case we'll have to generate more longwinded code.
      ARMAModeV* am  = mkARMAModeV(hregARM_R8(), e->Iex.Get.offset);
      HReg       res = newVRegF(env);
      addInstr(env, ARMInstr_VLdStS(True/*isLoad*/, res, am));
      return res;
   }

   if (e->tag == Iex_Unop) {
      switch (e->Iex.Unop.op) {
         case Iop_ReinterpI32asF32: {
            HReg dst = newVRegF(env);
            HReg src = iselIntExpr_R(env, e->Iex.Unop.arg);
            addInstr(env, ARMInstr_VXferS(True/*toS*/, dst, src));
            return dst;
         }
         case Iop_NegF32: {
            HReg src = iselFltExpr(env, e->Iex.Unop.arg);
            HReg dst = newVRegF(env);
            addInstr(env, ARMInstr_VUnaryS(ARMvfpu_NEG, dst, src));
            return dst;
         }
         case Iop_AbsF32: {
            HReg src = iselFltExpr(env, e->Iex.Unop.arg);
            HReg dst = newVRegF(env);
            addInstr(env, ARMInstr_VUnaryS(ARMvfpu_ABS, dst, src));
            return dst;
         }
         default:
            break;
      }
   }

   if (e->tag == Iex_Binop) {
      switch (e->Iex.Binop.op) {
         case Iop_SqrtF32: {
            /* first arg is rounding mode; we ignore it. */
            HReg src = iselFltExpr(env, e->Iex.Binop.arg2);
            HReg dst = newVRegF(env);
            addInstr(env, ARMInstr_VUnaryS(ARMvfpu_SQRT, dst, src));
            return dst;
         }
         case Iop_F64toF32: {
            HReg valD = iselDblExpr(env, e->Iex.Binop.arg2);
            set_VFP_rounding_mode(env, e->Iex.Binop.arg1);
            HReg valS = newVRegF(env);
            /* FCVTSD valS, valD */
            addInstr(env, ARMInstr_VCvtSD(False/*!sToD*/, valS, valD));
            set_VFP_rounding_default(env);
            return valS;
         }
         default:
            break;
      }
   }

   if (e->tag == Iex_Triop) {
      switch (e->Iex.Triop.op) {
         case Iop_DivF32:
         case Iop_MulF32:
         case Iop_AddF32:
         case Iop_SubF32: {
            ARMVfpOp op = 0; /*INVALID*/
            HReg argL = iselFltExpr(env, e->Iex.Triop.arg2);
            HReg argR = iselFltExpr(env, e->Iex.Triop.arg3);
            HReg dst  = newVRegF(env);
            switch (e->Iex.Triop.op) {
               case Iop_DivF32: op = ARMvfp_DIV; break;
               case Iop_MulF32: op = ARMvfp_MUL; break;
               case Iop_AddF32: op = ARMvfp_ADD; break;
               case Iop_SubF32: op = ARMvfp_SUB; break;
               default: vassert(0);
            }
            addInstr(env, ARMInstr_VAluS(op, dst, argL, argR));
            return dst;
         }
         default:
            break;
      }
   }

   if (e->tag == Iex_Mux0X) {
      if (ty == Ity_F32
          && typeOfIRExpr(env->type_env,e->Iex.Mux0X.cond) == Ity_I8) {
         HReg r8;
         HReg rX  = iselFltExpr(env, e->Iex.Mux0X.exprX);
         HReg r0  = iselFltExpr(env, e->Iex.Mux0X.expr0);
         HReg dst = newVRegF(env);
         addInstr(env, ARMInstr_VUnaryS(ARMvfpu_COPY, dst, rX));
         r8 = iselIntExpr_R(env, e->Iex.Mux0X.cond);
         addInstr(env, ARMInstr_CmpOrTst(False/*!isCmp*/, r8,
                                         ARMRI84_I84(0xFF,0)));
         addInstr(env, ARMInstr_VCMovS(ARMcc_EQ, dst, r0));
         return dst;
      }
   }

   ppIRExpr(e);
   vpanic("iselFltExpr_wrk");
}


/*---------------------------------------------------------*/
/*--- ISEL: Statements                                  ---*/
/*---------------------------------------------------------*/

static void iselStmt ( ISelEnv* env, IRStmt* stmt )
{
   if (vex_traceflags & VEX_TRACE_VCODE) {
      vex_printf("\n-- ");
      ppIRStmt(stmt);
      vex_printf("\n");
   }
   switch (stmt->tag) {

   /* --------- STORE --------- */
   /* little-endian write to memory */
   case Ist_Store: {
      IRType    tya  = typeOfIRExpr(env->type_env, stmt->Ist.Store.addr);
      IRType    tyd  = typeOfIRExpr(env->type_env, stmt->Ist.Store.data);
      IREndness end  = stmt->Ist.Store.end;
      Bool      isSC = stmt->Ist.Store.resSC != IRTemp_INVALID;

      if (tya != Ity_I32 || end != Iend_LE) 
         goto stmt_fail;

      /* normal (non-Store-Conditional) cases */
      if (tyd == Ity_I32 && !isSC) {
         HReg       rD = iselIntExpr_R(env, stmt->Ist.Store.data);
         ARMAMode1* am = iselIntExpr_AMode1(env, stmt->Ist.Store.addr);
         addInstr(env, ARMInstr_LdSt32(False/*!isLoad*/, rD, am));
         return;
      }
      if (tyd == Ity_I16 && !isSC) {
         HReg       rD = iselIntExpr_R(env, stmt->Ist.Store.data);
         ARMAMode2* am = iselIntExpr_AMode2(env, stmt->Ist.Store.addr);
         addInstr(env, ARMInstr_LdSt16(False/*!isLoad*/,
                                       False/*!isSignedLoad*/, rD, am));
         return;
      }
      if (tyd == Ity_I8 && !isSC) {
         HReg       rD = iselIntExpr_R(env, stmt->Ist.Store.data);
         ARMAMode1* am = iselIntExpr_AMode1(env, stmt->Ist.Store.addr);
         addInstr(env, ARMInstr_LdSt8U(False/*!isLoad*/, rD, am));
         return;
      }
      if (tyd == Ity_I64 && !isSC) {
         HReg rDhi, rDlo, rA;
         iselInt64Expr(&rDhi, &rDlo, env, stmt->Ist.Store.data);
         rA = iselIntExpr_R(env, stmt->Ist.Store.addr);
         addInstr(env, ARMInstr_LdSt32(False/*!load*/, rDhi,
                                       ARMAMode1_RI(rA,4)));
         addInstr(env, ARMInstr_LdSt32(False/*!load*/, rDlo,
                                       ARMAMode1_RI(rA,0)));
         return;
      }
      if (tyd == Ity_F64 && !isSC) {
         HReg       dD = iselDblExpr(env, stmt->Ist.Store.data);
         ARMAModeV* am = iselIntExpr_AModeV(env, stmt->Ist.Store.addr);
         addInstr(env, ARMInstr_VLdStD(False/*!isLoad*/, dD, am));
         return;
      }
      if (tyd == Ity_F32 && !isSC) {
         HReg       fD = iselFltExpr(env, stmt->Ist.Store.data);
         ARMAModeV* am = iselIntExpr_AModeV(env, stmt->Ist.Store.addr);
         addInstr(env, ARMInstr_VLdStS(False/*!isLoad*/, fD, am));
         return;
      }

      /* Store-Conditional cases */
      if (isSC && (tyd == Ity_I32 || tyd == Ity_I8)) {
         Int  szB     = 0;
         HReg r_res   = lookupIRTemp(env, stmt->Ist.Store.resSC);
         HReg rD      = iselIntExpr_R(env, stmt->Ist.Store.data);
         HReg rA      = iselIntExpr_R(env, stmt->Ist.Store.addr);
         ARMRI84* one = ARMRI84_I84(1,0);
         switch (tyd) {
            case Ity_I8:  szB = 1; break;
            case Ity_I32: szB = 4; break;
            default:      vassert(0);
         }
         addInstr(env, mk_iMOVds_RR(hregARM_R1(), rD));
         addInstr(env, mk_iMOVds_RR(hregARM_R2(), rA));
         addInstr(env, ARMInstr_StrEX(szB));
         /* now r0 is 1 if failed, 0 if success.  Change to IR
            conventions (0 is fail, 1 is success).  Also transfer
            result to r_res. */
         addInstr(env, ARMInstr_Alu(ARMalu_XOR, r_res, hregARM_R0(), one));
         /* And be conservative -- mask off all but the lowest bit */
         addInstr(env, ARMInstr_Alu(ARMalu_AND, r_res, r_res, one));
         return;
      }

      break;
   }

   /* --------- PUT --------- */
   /* write guest state, fixed offset */
   case Ist_Put: {
       IRType tyd = typeOfIRExpr(env->type_env, stmt->Ist.Put.data);

       if (tyd == Ity_I32) {
           HReg       rD = iselIntExpr_R(env, stmt->Ist.Put.data);
           ARMAMode1* am = ARMAMode1_RI(hregARM_R8(), stmt->Ist.Put.offset);
           addInstr(env, ARMInstr_LdSt32(False/*!isLoad*/, rD, am));
           return;
       }
       if (tyd == Ity_I64) {
          HReg rDhi, rDlo;
          ARMAMode1* am0 = ARMAMode1_RI(hregARM_R8(), stmt->Ist.Put.offset + 0);
          ARMAMode1* am4 = ARMAMode1_RI(hregARM_R8(), stmt->Ist.Put.offset + 4);
          iselInt64Expr(&rDhi, &rDlo, env, stmt->Ist.Put.data);
          addInstr(env, ARMInstr_LdSt32(False/*!isLoad*/, rDhi, am4));
          addInstr(env, ARMInstr_LdSt32(False/*!isLoad*/, rDlo, am0));
          return;
       }
       if (tyd == Ity_F64) {
          // XXX This won't work if offset > 1020 or is not 0 % 4.
          // In which case we'll have to generate more longwinded code.
          ARMAModeV* am = mkARMAModeV(hregARM_R8(), stmt->Ist.Put.offset);
          HReg       rD = iselDblExpr(env, stmt->Ist.Put.data);
          addInstr(env, ARMInstr_VLdStD(False/*!isLoad*/, rD, am));
          return;
       }
       if (tyd == Ity_F32) {
          // XXX This won't work if offset > 1020 or is not 0 % 4.
          // In which case we'll have to generate more longwinded code.
          ARMAModeV* am = mkARMAModeV(hregARM_R8(), stmt->Ist.Put.offset);
          HReg       rD = iselFltExpr(env, stmt->Ist.Put.data);
          addInstr(env, ARMInstr_VLdStS(False/*!isLoad*/, rD, am));
          return;
       }
       break;
   }

//zz   /* --------- Indexed PUT --------- */
//zz   /* write guest state, run-time offset */
//zz   case Ist_PutI: {
//zz      ARMAMode2* am2
//zz           = genGuestArrayOffset(
//zz               env, stmt->Ist.PutI.descr, 
//zz               stmt->Ist.PutI.ix, stmt->Ist.PutI.bias );
//zz       
//zz       IRType tyd = typeOfIRExpr(env->type_env, stmt->Ist.PutI.data);
//zz       
//zz       if (tyd == Ity_I8) {
//zz           HReg reg = iselIntExpr_R(env, stmt->Ist.PutI.data);
//zz           addInstr(env, ARMInstr_StoreB(reg, am2));
//zz           return;
//zz       }
//zz// CAB: Ity_I32, Ity_I16 ?
//zz       break;
//zz   }

   /* --------- TMP --------- */
   /* assign value to temporary */
   case Ist_WrTmp: {
      IRTemp tmp = stmt->Ist.WrTmp.tmp;
      IRType ty = typeOfIRTemp(env->type_env, tmp);

      if (ty == Ity_I32 || ty == Ity_I16 || ty == Ity_I8) {
         ARMRI84* ri84 = iselIntExpr_RI84(NULL, False,
                                          env, stmt->Ist.WrTmp.data);
         HReg     dst  = lookupIRTemp(env, tmp);
         addInstr(env, ARMInstr_Mov(dst,ri84));
         return;
      }
      if (ty == Ity_I1) {
         HReg        dst  = lookupIRTemp(env, tmp);
         ARMCondCode cond = iselCondCode(env, stmt->Ist.WrTmp.data);
         addInstr(env, ARMInstr_Mov(dst, ARMRI84_I84(0,0)));
         addInstr(env, ARMInstr_CMov(cond, dst, ARMRI84_I84(1,0)));
         return;
      }
      if (ty == Ity_I64) {
         HReg rHi, rLo, dstHi, dstLo;
         iselInt64Expr(&rHi,&rLo, env, stmt->Ist.WrTmp.data);
         lookupIRTemp64( &dstHi, &dstLo, env, tmp);
         addInstr(env, mk_iMOVds_RR(dstHi, rHi) );
         addInstr(env, mk_iMOVds_RR(dstLo, rLo) );
         return;
      }
      if (ty == Ity_F64) {
         HReg src = iselDblExpr(env, stmt->Ist.WrTmp.data);
         HReg dst = lookupIRTemp(env, tmp);
         addInstr(env, ARMInstr_VUnaryD(ARMvfpu_COPY, dst, src));
         return;
      }
      if (ty == Ity_F32) {
         HReg src = iselFltExpr(env, stmt->Ist.WrTmp.data);
         HReg dst = lookupIRTemp(env, tmp);
         addInstr(env, ARMInstr_VUnaryS(ARMvfpu_COPY, dst, src));
         return;
      }
      break;
   }

   /* --------- Call to DIRTY helper --------- */
   /* call complex ("dirty") helper function */
   case Ist_Dirty: {
      IRType   retty;
      IRDirty* d = stmt->Ist.Dirty.details;
      Bool     passBBP = False;

      if (d->nFxState == 0)
         vassert(!d->needsBBP);

      passBBP = toBool(d->nFxState > 0 && d->needsBBP);

      /* Marshal args, do the call, clear stack. */
      Bool ok = doHelperCall( env, passBBP, d->guard, d->cee, d->args );
      if (!ok)
         break; /* will go to stmt_fail: */

      /* Now figure out what to do with the returned value, if any. */
      if (d->tmp == IRTemp_INVALID)
         /* No return value.  Nothing to do. */
         return;

      retty = typeOfIRTemp(env->type_env, d->tmp);

      if (retty == Ity_I64) {
         HReg dstHi, dstLo;
         /* The returned value is in r1:r0.  Park it in the
            register-pair associated with tmp. */
         lookupIRTemp64( &dstHi, &dstLo, env, d->tmp);
         addInstr(env, mk_iMOVds_RR(dstHi, hregARM_R1()) );
         addInstr(env, mk_iMOVds_RR(dstLo, hregARM_R0()) );
         return;
      }
      if (retty == Ity_I32 || retty == Ity_I16 || retty == Ity_I8) {
         /* The returned value is in r0.  Park it in the register
            associated with tmp. */
         HReg dst = lookupIRTemp(env, d->tmp);
         addInstr(env, mk_iMOVds_RR(dst, hregARM_R0()) );
         return;
      }

      break;
   }

   /* --------- INSTR MARK --------- */
   /* Doesn't generate any executable code ... */
   case Ist_IMark:
       return;

   /* --------- NO-OP --------- */
   case Ist_NoOp:
       return;

   /* --------- EXIT --------- */
   case Ist_Exit: {
      HReg        gnext;
      ARMCondCode cc;
      if (stmt->Ist.Exit.dst->tag != Ico_U32)
         vpanic("isel_arm: Ist_Exit: dst is not a 32-bit value");
      gnext = iselIntExpr_R(env, IRExpr_Const(stmt->Ist.Exit.dst));
      cc    = iselCondCode(env, stmt->Ist.Exit.guard);
      addInstr(env, mk_iMOVds_RR(hregARM_R14(), env->savedLR));
      addInstr(env, ARMInstr_Goto(stmt->Ist.Exit.jk, cc, gnext));
      return;
   }

   default: break;
   }
  stmt_fail:
   ppIRStmt(stmt);
   vpanic("iselStmt");
}


/*---------------------------------------------------------*/
/*--- ISEL: Basic block terminators (Nexts)             ---*/
/*---------------------------------------------------------*/

static void iselNext ( ISelEnv* env, IRExpr* next, IRJumpKind jk )
{
   HReg rDst;
   if (vex_traceflags & VEX_TRACE_VCODE) {
      vex_printf("\n-- goto {");
      ppIRJumpKind(jk);
      vex_printf("} ");
      ppIRExpr(next);
      vex_printf("\n");
   }
   rDst = iselIntExpr_R(env, next);
   addInstr(env, mk_iMOVds_RR(hregARM_R14(), env->savedLR));
   addInstr(env, ARMInstr_Goto(jk, ARMcc_AL, rDst));
}


/*---------------------------------------------------------*/
/*--- Insn selector top-level                           ---*/
/*---------------------------------------------------------*/

/* Translate an entire SB to arm code. */

HInstrArray* iselSB_ARM ( IRSB* bb, VexArch      arch_host,
                                    VexArchInfo* archinfo_host,
                                    VexAbiInfo*  vbi/*UNUSED*/ )
{
   Int      i, j;
   HReg     hreg, hregHI;
   ISelEnv* env;
   UInt     hwcaps_host = archinfo_host->hwcaps;

   /* sanity ... */
   vassert(arch_host == VexArchARM);
   vassert(0 == hwcaps_host);

   /* Make up an initial environment to use. */
   env = LibVEX_Alloc(sizeof(ISelEnv));
   env->vreg_ctr = 0;

   /* Set up output code array. */
   env->code = newHInstrArray();
    
   /* Copy BB's type env. */
   env->type_env = bb->tyenv;

   /* Make up an IRTemp -> virtual HReg mapping.  This doesn't
      change as we go along. */
   env->n_vregmap = bb->tyenv->types_used;
   env->vregmap   = LibVEX_Alloc(env->n_vregmap * sizeof(HReg));
   env->vregmapHI = LibVEX_Alloc(env->n_vregmap * sizeof(HReg));

   /* For each IR temporary, allocate a suitably-kinded virtual
      register. */
   j = 0;
   for (i = 0; i < env->n_vregmap; i++) {
      hregHI = hreg = INVALID_HREG;
      switch (bb->tyenv->types[i]) {
         case Ity_I1:
         case Ity_I8:
         case Ity_I16:
         case Ity_I32:  hreg   = mkHReg(j++, HRcInt32, True); break;
         case Ity_I64:  hregHI = mkHReg(j++, HRcInt32, True);
                        hreg   = mkHReg(j++, HRcInt32, True); break;
         case Ity_F32:  hreg   = mkHReg(j++, HRcFlt32, True); break;
         case Ity_F64:  hreg   = mkHReg(j++, HRcFlt64, True); break;
         //case Ity_V128: hreg   = mkHReg(j++, HRcVec128, True); break;
         default: ppIRType(bb->tyenv->types[i]);
                  vpanic("iselBB: IRTemp type");
      }
      env->vregmap[i]   = hreg;
      env->vregmapHI[i] = hregHI;
   }
   env->vreg_ctr = j;

   /* Keep a copy of the link reg, since any call to a helper function
      will trash it, and we can't get back to the dispatcher once that
      happens. */
   env->savedLR = newVRegI(env);
   addInstr(env, mk_iMOVds_RR(env->savedLR, hregARM_R14()));

   /* Ok, finally we can iterate over the statements. */
   for (i = 0; i < bb->stmts_used; i++)
      iselStmt(env,bb->stmts[i]);

   iselNext(env,bb->next,bb->jumpkind);

   /* record the number of vregs we used. */
   env->code->n_vregs = env->vreg_ctr;
   return env->code;
}


/*---------------------------------------------------------------*/
/*--- end                                     host_arm_isel.c ---*/
/*---------------------------------------------------------------*/
