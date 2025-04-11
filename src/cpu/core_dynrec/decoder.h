/*
 *  Copyright (C) 2002-2015  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */



#include "decoder_basic.h"
#include "operators.h"
#include "decoder_opcodes.h"

#include "dyn_fpu.h"

/*
	The function CreateCacheBlock translates the instruction stream
	until either an unhandled instruction is found, the maximum
	number of translated instructions is reached or some critical
	instruction is encountered.
*/

static CacheBlockDynRec * CreateCacheBlock(CodePageHandlerDynRec * codepage, PhysPt start, Bitu max_opcodes) {
    // Initialize variables
    decode.code_start = start;
    decode.code = start;
    decode.page.code = codepage;
    decode.page.index = start & 4095;
    decode.page.wmap = codepage->write_map;
    decode.page.invmap = codepage->invalidation_map;
    decode.page.first = start >> 12;
    decode.active_block = decode.block = cache_openblock();
    decode.block->page.start = (Bit16u)decode.page.index;
    codepage->AddCacheBlock(decode.block);

    InitFlagsOptimization();

    // Set cache.block.running
    gen_mov_direct_ptr(&cache.block.running, (DRC_PTR_SIZE_IM)decode.block);

    // Cycle check
    gen_mov_word_to_reg(FC_RETOP, &CPU_Cycles, true);
    save_info_dynrec[used_save_info_dynrec].branch_pos = gen_create_branch_long_leqzero(FC_RETOP);
    save_info_dynrec[used_save_info_dynrec].type = cycle_check;
    used_save_info_dynrec++;

    decode.cycles = 0;
    Bitu opcode_count = 0; // Track opcodes processed
    while (opcode_count < max_opcodes) {
        // Init prefixes
        decode.big_addr = cpu.code.big;
        decode.big_op = cpu.code.big;
        decode.seg_prefix = 0;
        decode.seg_prefix_used = false;
        decode.rep = REP_NONE;
        decode.cycles++;
        decode.op_start = decode.code;

        // Fast path for fetching opcode
        Bitu opcode = decode_fetchb();
        if (decode.page.invmap && decode.page.index <= 4095) {
            if (GCC_UNLIKELY(decode.page.invmap[decode.page.index - 1] >= 3)) { // Lowered SMC threshold
                goto illegalopcode;
            }
        }

        // Use a lookup table for common opcodes
        static const void* opcode_table[256] = {
            [0x00] = &&op_00, [0x01] = &&op_01, [0x02] = &&op_02, [0x03] = &&op_03,
            [0x04] = &&op_04, [0x05] = &&op_05, [0x06] = &&op_06, [0x07] = &&op_07,
            [0x08] = &&op_08, [0x09] = &&op_09, [0x0a] = &&op_0a, [0x0b] = &&op_0b,
            [0x0c] = &&op_0c, [0x0d] = &&op_0d, [0x0e] = &&op_0e, [0x0f] = &&op_0f,
            [0x10] = &&op_10, [0x11] = &&op_11, [0x12] = &&op_12, [0x13] = &&op_13,
            [0x14] = &&op_14, [0x15] = &&op_15, [0x16] = &&op_16, [0x17] = &&op_17,
            [0x18] = &&op_18, [0x19] = &&op_19, [0x1a] = &&op_1a, [0x1b] = &&op_1b,
            [0x1c] = &&op_1c, [0x1d] = &&op_1d, [0x1e] = &&op_1e, [0x1f] = &&op_1f,
            [0x20] = &&op_20, [0x21] = &&op_21, [0x22] = &&op_22, [0x23] = &&op_23,
            [0x24] = &&op_24, [0x25] = &&op_25, [0x26] = &&op_26, [0x28] = &&op_28,
            [0x29] = &&op_29, [0x2a] = &&op_2a, [0x2b] = &&op_2b, [0x2c] = &&op_2c,
            [0x2d] = &&op_2d, [0x2e] = &&op_2e, [0x30] = &&op_30, [0x31] = &&op_31,
            [0x32] = &&op_32, [0x33] = &&op_33, [0x34] = &&op_34, [0x35] = &&op_35,
            [0x36] = &&op_36, [0x38] = &&op_38, [0x39] = &&op_39, [0x3a] = &&op_3a,
            [0x3b] = &&op_3b, [0x3c] = &&op_3c, [0x3d] = &&op_3d, [0x3e] = &&op_3e,
            [0x40] = &&op_40_47, [0x41] = &&op_40_47, [0x42] = &&op_40_47, [0x43] = &&op_40_47,
            [0x44] = &&op_40_47, [0x45] = &&op_40_47, [0x46] = &&op_40_47, [0x47] = &&op_40_47,
            [0x48] = &&op_48_4f, [0x49] = &&op_48_4f, [0x4a] = &&op_48_4f, [0x4b] = &&op_48_4f,
            [0x4c] = &&op_48_4f, [0x4d] = &&op_48_4f, [0x4e] = &&op_48_4f, [0x4f] = &&op_48_4f,
            [0x50] = &&op_50_57, [0x51] = &&op_50_57, [0x52] = &&op_50_57, [0x53] = &&op_50_57,
            [0x54] = &&op_50_57, [0x55] = &&op_50_57, [0x56] = &&op_50_57, [0x57] = &&op_50_57,
            [0x58] = &&op_58_5f, [0x59] = &&op_58_5f, [0x5a] = &&op_58_5f, [0x5b] = &&op_58_5f,
            [0x5c] = &&op_58_5f, [0x5d] = &&op_58_5f, [0x5e] = &&op_58_5f, [0x5f] = &&op_58_5f,
            [0x60] = &&op_60, [0x61] = &&op_61, [0x64] = &&op_64, [0x65] = &&op_65,
            [0x66] = &&op_66, [0x67] = &&op_67, [0x68] = &&op_68, [0x6a] = &&op_6a,
            [0x69] = &&op_69, [0x6b] = &&op_6b, [0x70] = &&op_70_7f, [0x71] = &&op_70_7f,
            [0x72] = &&op_70_7f, [0x73] = &&op_70_7f, [0x74] = &&op_70_7f, [0x75] = &&op_70_7f,
            [0x76] = &&op_70_7f, [0x77] = &&op_70_7f, [0x78] = &&op_70_7f, [0x79] = &&op_70_7f,
            [0x7a] = &&op_70_7f, [0x7b] = &&op_70_7f, [0x7c] = &&op_70_7f, [0x7d] = &&op_70_7f,
            [0x7e] = &&op_70_7f, [0x7f] = &&op_70_7f, [0x80] = &&op_80, [0x81] = &&op_81,
            [0x82] = &&op_80, [0x83] = &&op_83, [0x84] = &&op_84, [0x85] = &&op_85,
            [0x86] = &&op_86, [0x87] = &&op_87, [0x88] = &&op_88, [0x89] = &&op_89,
            [0x8a] = &&op_8a, [0x8b] = &&op_8b, [0x8c] = &&op_8c, [0x8d] = &&op_8d,
            [0x8e] = &&op_8e, [0x8f] = &&op_8f, [0x90] = &&op_90, [0x91] = &&op_91_97,
            [0x92] = &&op_91_97, [0x93] = &&op_91_97, [0x94] = &&op_91_97, [0x95] = &&op_91_97,
            [0x96] = &&op_91_97, [0x97] = &&op_91_97, [0x98] = &&op_98, [0x99] = &&op_99,
            [0x9a] = &&op_9a, [0x9b] = &&op_90, [0x9c] = &&op_9c, [0x9d] = &&op_9d,
            [0x9e] = &&op_9e, [0xa0] = &&op_a0, [0xa1] = &&op_a1, [0xa2] = &&op_a2,
            [0xa3] = &&op_a3, [0xa4] = &&op_a4, [0xa5] = &&op_a5, [0xa8] = &&op_a8,
            [0xa9] = &&op_a9, [0xaa] = &&op_aa, [0xab] = &&op_ab, [0xac] = &&op_ac,
            [0xad] = &&op_ad, [0xb0] = &&op_b0_b7, [0xb1] = &&op_b0_b7, [0xb2] = &&op_b0_b7,
            [0xb3] = &&op_b0_b7, [0xb4] = &&op_b0_b7, [0xb5] = &&op_b0_b7, [0xb6] = &&op_b0_b7,
            [0xb7] = &&op_b0_b7, [0xb8] = &&op_b8_bf, [0xb9] = &&op_b8_bf, [0xba] = &&op_b8_bf,
            [0xbb] = &&op_b8_bf, [0xbc] = &&op_b8_bf, [0xbd] = &&op_b8_bf, [0xbe] = &&op_b8_bf,
            [0xbf] = &&op_b8_bf, [0xc0] = &&op_c0, [0xc1] = &&op_c1, [0xc2] = &&op_c2,
            [0xc3] = &&op_c3, [0xc4] = &&op_c4, [0xc5] = &&op_c5, [0xc6] = &&op_c6,
            [0xc7] = &&op_c7, [0xc8] = &&op_c8, [0xc9] = &&op_c9, [0xca] = &&op_ca,
            [0xcb] = &&op_cb, [0xcd] = &&op_cd, [0xcf] = &&op_cf,
            [0xd8] = &&op_d8, [0xd9] = &&op_d9, [0xda] = &&op_da, [0xdb] = &&op_db,
            [0xdc] = &&op_dc, [0xdd] = &&op_dd, [0xde] = &&op_de, [0xdf] = &&op_df,
            [0xe0] = &&op_e0, [0xe1] = &&op_e1, [0xe2] = &&op_e2, [0xe3] = &&op_e3,
            [0xe4] = &&op_e4, [0xe5] = &&op_e5, [0xe6] = &&op_e6, [0xe7] = &&op_e7,
            [0xe8] = &&op_e8, [0xe9] = &&op_e9, [0xea] = &&op_ea, [0xeb] = &&op_eb,
            [0xec] = &&op_ec, [0xed] = &&op_ed, [0xee] = &&op_ee, [0xef] = &&op_ef,
            [0xf0] = &&op_90, [0xf2] = &&op_f2, [0xf3] = &&op_f3, [0xf5] = &&op_f5,
            [0xf6] = &&op_f6, [0xf7] = &&op_f7, [0xf8] = &&op_f8, [0xf9] = &&op_f9,
            [0xfa] = &&op_fa, [0xfb] = &&op_fb, [0xfc] = &&op_fc, [0xfd] = &&op_fd,
            [0xfe] = &&op_fe, [0xff] = &&op_ff
        };

        if (opcode_table[opcode]) {
            goto *opcode_table[opcode];
        } else {
            goto illegalopcode;
        }

    op_00: dyn_dop_ebgb(DOP_ADD); goto next_opcode;
    op_01: dyn_dop_evgv(DOP_ADD); goto next_opcode;
    op_02: dyn_dop_gbeb(DOP_ADD); goto next_opcode;
    op_03: dyn_dop_gvev(DOP_ADD); goto next_opcode;
    op_04: dyn_dop_byte_imm(DOP_ADD, DRC_REG_EAX, 0); goto next_opcode;
    op_05: dyn_dop_word_imm(DOP_ADD, DRC_REG_EAX); goto next_opcode;
    op_06: dyn_push_seg(DRC_SEG_ES); goto next_opcode;
    op_07: dyn_pop_seg(DRC_SEG_ES); goto next_opcode;
    op_08: dyn_dop_ebgb(DOP_OR); goto next_opcode;
    op_09: dyn_dop_evgv(DOP_OR); goto next_opcode;
    op_0a: dyn_dop_gbeb(DOP_OR); goto next_opcode;
    op_0b: dyn_dop_gvev(DOP_OR); goto next_opcode;
    op_0c: dyn_dop_byte_imm(DOP_OR, DRC_REG_EAX, 0); goto next_opcode;
    op_0d: dyn_dop_word_imm(DOP_OR, DRC_REG_EAX); goto next_opcode;
    op_0e: dyn_push_seg(DRC_SEG_CS); goto next_opcode;
    op_0f: {
        Bitu dual_code = decode_fetchb();
        static const void* dual_table[256] = {
            [0x00] = &&dual_00, [0x01] = &&dual_01, [0x20] = &&dual_20, [0x22] = &&dual_22,
            [0x80] = &&dual_80_8f, [0x81] = &&dual_80_8f, [0x82] = &&dual_80_8f, [0x83] = &&dual_80_8f,
            [0x84] = &&dual_80_8f, [0x85] = &&dual_80_8f, [0x86] = &&dual_80_8f, [0x87] = &&dual_80_8f,
            [0x88] = &&dual_80_8f, [0x89] = &&dual_80_8f, [0x8a] = &&dual_80_8f, [0x8b] = &&dual_80_8f,
            [0x8c] = &&dual_80_8f, [0x8d] = &&dual_80_8f, [0x8e] = &&dual_80_8f, [0x8f] = &&dual_80_8f,
            [0xa0] = &&dual_a0, [0xa1] = &&dual_a1, [0xa4] = &&dual_a4, [0xa5] = &&dual_a5,
            [0xa8] = &&dual_a8, [0xa9] = &&dual_a9, [0xac] = &&dual_ac, [0xad] = &&dual_ad,
            [0xaf] = &&dual_af, [0xb4] = &&dual_b4, [0xb5] = &&dual_b5, [0xb6] = &&dual_b6,
            [0xb7] = &&dual_b7, [0xbe] = &&dual_be, [0xbf] = &&dual_bf
        };
        if (dual_table[dual_code]) {
            goto *dual_table[dual_code];
        } else {
            goto illegalopcode;
        }

    dual_00:
        if ((reg_flags & FLAG_VM) || (!cpu.pmode)) goto illegalopcode;
        dyn_grp6();
        goto next_opcode;
    dual_01:
        if (dyn_grp7()) goto finish_block;
        goto next_opcode;
    dual_20: dyn_mov_from_crx(); goto next_opcode;
    dual_22: dyn_mov_to_crx(); goto finish_block;
    dual_80_8f:
        dyn_branched_exit((BranchTypes)(dual_code & 0xf),
                         decode.big_op ? (Bit32s)decode_fetchd() : (Bit16s)decode_fetchw());
        goto finish_block;
    dual_a0: dyn_push_seg(DRC_SEG_FS); goto next_opcode;
    dual_a1: dyn_pop_seg(DRC_SEG_FS); goto next_opcode;
    dual_a4: dyn_dshift_ev_gv(true, true); goto next_opcode;
    dual_a5: dyn_dshift_ev_gv(true, false); goto next_opcode;
    dual_a8: dyn_push_seg(DRC_SEG_GS); goto next_opcode;
    dual_a9: dyn_pop_seg(DRC_SEG_GS); goto next_opcode;
    dual_ac: dyn_dshift_ev_gv(false, true); goto next_opcode;
    dual_ad: dyn_dshift_ev_gv(false, false); goto next_opcode;
    dual_af: dyn_imul_gvev(0); goto next_opcode;
    dual_b4:
        dyn_get_modrm();
        if (GCC_UNLIKELY(decode.modrm.mod == 3)) goto illegalopcode;
        dyn_load_seg_off_ea(DRC_SEG_FS);
        goto next_opcode;
    dual_b5:
        dyn_get_modrm();
        if (GCC_UNLIKELY(decode.modrm.mod == 3)) goto illegalopcode;
        dyn_load_seg_off_ea(DRC_SEG_GS);
        goto next_opcode;
    dual_b6: dyn_movx_ev_gb(false); goto next_opcode;
    dual_b7: dyn_movx_ev_gw(false); goto next_opcode;
    dual_be: dyn_movx_ev_gb(true); goto next_opcode;
    dual_bf: dyn_movx_ev_gw(true); goto next_opcode;
    }
    op_10: dyn_dop_ebgb(DOP_ADC); goto next_opcode;
    op_11: dyn_dop_evgv(DOP_ADC); goto next_opcode;
    op_12: dyn_dop_gbeb(DOP_ADC); goto next_opcode;
    op_13: dyn_dop_gvev(DOP_ADC); goto next_opcode;
    op_14: dyn_dop_byte_imm(DOP_ADC, DRC_REG_EAX, 0); goto next_opcode;
    op_15: dyn_dop_word_imm(DOP_ADC, DRC_REG_EAX); goto next_opcode;
    op_16: dyn_push_seg(DRC_SEG_SS); goto next_opcode;
    op_17: dyn_pop_seg(DRC_SEG_SS); goto next_opcode;
    op_18: dyn_dop_ebgb(DOP_SBB); goto next_opcode;
    op_19: dyn_dop_evgv(DOP_SBB); goto next_opcode;
    op_1a: dyn_dop_gbeb(DOP_SBB); goto next_opcode;
    op_1b: dyn_dop_gvev(DOP_SBB); goto next_opcode;
    op_1c: dyn_dop_byte_imm(DOP_SBB, DRC_REG_EAX, 0); goto next_opcode;
    op_1d: dyn_dop_word_imm(DOP_SBB, DRC_REG_EAX); goto next_opcode;
    op_1e: dyn_push_seg(DRC_SEG_DS); goto next_opcode;
    op_1f: dyn_pop_seg(DRC_SEG_DS); goto next_opcode;
    op_20: dyn_dop_ebgb(DOP_AND); goto next_opcode;
    op_21: dyn_dop_evgv(DOP_AND); goto next_opcode;
    op_22: dyn_dop_gbeb(DOP_AND); goto next_opcode;
    op_23: dyn_dop_gvev(DOP_AND); goto next_opcode;
    op_24: dyn_dop_byte_imm(DOP_AND, DRC_REG_EAX, 0); goto next_opcode;
    op_25: dyn_dop_word_imm(DOP_AND, DRC_REG_EAX); goto next_opcode;
    op_26: dyn_segprefix(DRC_SEG_ES); continue;
    op_28: dyn_dop_ebgb(DOP_SUB); goto next_opcode;
    op_29: dyn_dop_evgv(DOP_SUB); goto next_opcode;
    op_2a: dyn_dop_gbeb(DOP_SUB); goto next_opcode;
    op_2b: dyn_dop_gvev(DOP_SUB); goto next_opcode;
    op_2c: dyn_dop_byte_imm(DOP_SUB, DRC_REG_EAX, 0); goto next_opcode;
    op_2d: dyn_dop_word_imm(DOP_SUB, DRC_REG_EAX); goto next_opcode;
    op_2e: dyn_segprefix(DRC_SEG_CS); continue;
    op_30: dyn_dop_ebgb(DOP_XOR); goto next_opcode;
    op_31: dyn_dop_evgv(DOP_XOR); goto next_opcode;
    op_32: dyn_dop_gbeb(DOP_XOR); goto next_opcode;
    op_33: dyn_dop_gvev(DOP_XOR); goto next_opcode;
    op_34: dyn_dop_byte_imm(DOP_XOR, DRC_REG_EAX, 0); goto next_opcode;
    op_35: dyn_dop_word_imm(DOP_XOR, DRC_REG_EAX); goto next_opcode;
    op_36: dyn_segprefix(DRC_SEG_SS); continue;
    op_38: dyn_dop_ebgb(DOP_CMP); goto next_opcode;
    op_39: dyn_dop_evgv(DOP_CMP); goto next_opcode;
    op_3a: dyn_dop_gbeb(DOP_CMP); goto next_opcode;
    op_3b: dyn_dop_gvev(DOP_CMP); goto next_opcode;
    op_3c: dyn_dop_byte_imm(DOP_CMP, DRC_REG_EAX, 0); goto next_opcode;
    op_3d: dyn_dop_word_imm(DOP_CMP, DRC_REG_EAX); goto next_opcode;
    op_3e: dyn_segprefix(DRC_SEG_DS); continue;
    op_40_47: dyn_sop_word(SOP_INC, opcode & 7); goto next_opcode;
    op_48_4f: dyn_sop_word(SOP_DEC, opcode & 7); goto next_opcode;
    op_50_57: dyn_push_reg(opcode & 7); goto next_opcode;
    op_58_5f: dyn_pop_reg(opcode & 7); goto next_opcode;
    op_60:
        gen_call_function_raw(decode.big_op ? (void*)&dynrec_pusha_dword : (void*)&dynrec_pusha_word);
        goto next_opcode;
    op_61:
        gen_call_function_raw(decode.big_op ? (void*)&dynrec_popa_dword : (void*)&dynrec_popa_word);
        goto next_opcode;
    op_64: dyn_segprefix(DRC_SEG_FS); continue;
    op_65: dyn_segprefix(DRC_SEG_GS); continue;
    op_66: decode.big_op = !cpu.code.big; continue;
    op_67: decode.big_addr = !cpu.code.big; continue;
    op_68: dyn_push_word_imm(decode.big_op ? decode_fetchd() : decode_fetchw()); goto next_opcode;
    op_6a: dyn_push_byte_imm((Bit8s)decode_fetchb()); goto next_opcode;
    op_69: dyn_imul_gvev(decode.big_op ? 4 : 2); goto next_opcode;
    op_6b: dyn_imul_gvev(1); goto next_opcode;
    op_70_7f:
        dyn_branched_exit((BranchTypes)(opcode & 0xf), (Bit8s)decode_fetchb());
        goto finish_block;
    op_80: dyn_grp1_eb_ib(); goto next_opcode;
    op_81: dyn_grp1_ev_iv(false); goto next_opcode;
    op_82: dyn_grp1_eb_ib(); goto next_opcode;
    op_83: dyn_grp1_ev_iv(true); goto next_opcode;
    op_84: dyn_dop_gbeb(DOP_TEST); goto next_opcode;
    op_85: dyn_dop_gvev(DOP_TEST); goto next_opcode;
    op_86: dyn_dop_ebgb_xchg(); goto next_opcode;
    op_87: dyn_dop_evgv_xchg(); goto next_opcode;
    op_88: dyn_dop_ebgb_mov(); goto next_opcode;
    op_89: dyn_dop_evgv_mov(); goto next_opcode;
    op_8a: dyn_dop_gbeb_mov(); goto next_opcode;
    op_8b: dyn_dop_gvev_mov(); goto next_opcode;
    op_8c: dyn_mov_ev_seg(); goto next_opcode;
    op_8d: dyn_lea(); goto next_opcode;
    op_8e: dyn_mov_seg_ev(); goto next_opcode;
    op_8f: dyn_pop_ev(); goto next_opcode;
    op_90: goto next_opcode; // nop, lock, wait
    op_91_97: dyn_xchg_ax(opcode & 7); goto next_opcode;
    op_98: dyn_cbw(); goto next_opcode;
    op_99: dyn_cwd(); goto next_opcode;
    op_9a: dyn_call_far_imm(); goto finish_block;
    op_9c:
        AcquireFlags(FMASK_TEST);
        gen_call_function_I((void*)&CPU_PUSHF, decode.big_op);
        dyn_check_exception(FC_RETOP);
        goto next_opcode;
    op_9d:
        gen_call_function_I((void*)&CPU_POPF, decode.big_op);
        dyn_check_exception(FC_RETOP);
        InvalidateFlags();
        goto next_opcode;
    op_9e: dyn_sahf(); goto next_opcode;
    op_a0: dyn_mov_byte_al_direct(decode.big_addr ? decode_fetchd() : decode_fetchw()); goto next_opcode;
    op_a1: dyn_mov_byte_ax_direct(decode.big_addr ? decode_fetchd() : decode_fetchw()); goto next_opcode;
    op_a2: dyn_mov_byte_direct_al(); goto next_opcode;
    op_a3: dyn_mov_byte_direct_ax(decode.big_addr ? decode_fetchd() : decode_fetchw()); goto next_opcode;
    op_a4:
        if (decode.rep != REP_NONE) {
            gen_call_function_raw(decode.big_op ? (void*)&rep_movsd : (void*)&rep_movsb);
        } else {
            dyn_string(STR_MOVSB);
        }
        goto next_opcode;
    op_a5:
        if (decode.rep != REP_NONE) {
            gen_call_function_raw(decode.big_op ? (void*)&rep_movsd : (void*)&rep_movsw);
        } else {
            dyn_string(decode.big_op ? STR_MOVSD : STR_MOVSW);
        }
        goto next_opcode;
    op_a8: dyn_dop_byte_imm(DOP_TEST, DRC_REG_EAX, 0); goto next_opcode;
    op_a9: dyn_dop_word_imm(DOP_TEST, DRC_REG_EAX); goto next_opcode;
    op_aa:
        if (decode.rep != REP_NONE) {
            gen_call_function_raw(decode.big_op ? (void*)&rep_stosd : (void*)&rep_stosb);
        } else {
            dyn_string(STR_STOSB);
        }
        goto next_opcode;
    op_ab:
        if (decode.rep != REP_NONE) {
            gen_call_function_raw(decode.big_op ? (void*)&rep_stosd : (void*)&rep_stosw);
        } else {
            dyn_string(decode.big_op ? STR_STOSD : STR_STOSW);
        }
        goto next_opcode;
    op_ac:
        if (decode.rep != REP_NONE) {
            gen_call_function_raw(decode.big_op ? (void*)&rep_lodsd : (void*)&rep_lodsb);
        } else {
            dyn_string(STR_LODSB);
        }
        goto next_opcode;
    op_ad:
        if (decode.rep != REP_NONE) {
            gen_call_function_raw(decode.big_op ? (void*)&rep_lodsd : (void*)&rep_lodsw);
        } else {
            dyn_string(decode.big_op ? STR_LODSD : STR_LODSW);
        }
        goto next_opcode;
    op_b0_b7: dyn_mov_byte_imm(opcode & 3, (opcode >> 2) & 1, decode_fetchb()); goto next_opcode;
    op_b8_bf: dyn_mov_word_imm(opcode & 7); goto next_opcode;
    op_c0: dyn_grp2_eb(grp2_imm); goto next_opcode;
    op_c1: dyn_grp2_ev(grp2_imm); goto next_opcode;
    op_c2: dyn_ret_near(decode_fetchw()); goto finish_block;
    op_c3: dyn_ret_near(0); goto finish_block;
    op_c4:
        dyn_get_modrm();
        if (GCC_UNLIKELY(decode.modrm.mod == 3)) goto illegalopcode;
        dyn_load_seg_off_ea(DRC_SEG_ES);
        goto next_opcode;
    op_c5:
        dyn_get_modrm();
        if (GCC_UNLIKELY(decode.modrm.mod == 3)) goto illegalopcode;
        dyn_load_seg_off_ea(DRC_SEG_DS);
        goto next_opcode;
    op_c6: dyn_dop_ebib_mov(); goto next_opcode;
    op_c7: dyn_dop_eviv_mov(); goto next_opcode;
    op_c8: dyn_enter(); goto next_opcode;
    op_c9: dyn_leave(); goto next_opcode;
    op_ca: dyn_ret_far(decode_fetchw()); goto finish_block;
    op_cb: dyn_ret_far(0); goto finish_block;
    op_cd: dyn_interrupt(decode_fetchb()); goto finish_block;
    op_cf: dyn_iret(); goto finish_block;
#ifdef CPU_FPU
    op_d8: dyn_fpu_esc0(); goto next_opcode;
    op_d9: dyn_fpu_esc1(); goto next_opcode;
    op_da: dyn_fpu_esc2(); goto next_opcode;
    op_db: dyn_fpu_esc3(); goto next_opcode;
    op_dc: dyn_fpu_esc4(); goto next_opcode;
    op_dd: dyn_fpu_esc5(); goto next_opcode;
    op_de: dyn_fpu_esc6(); goto next_opcode;
    op_df: dyn_fpu_esc7(); goto next_opcode;
#endif
    op_e0: dyn_loop(LOOP_NE); goto finish_block;
    op_e1: dyn_loop(LOOP_E); goto finish_block;
    op_e2: dyn_loop(LOOP_NONE); goto finish_block;
    op_e3: dyn_loop(LOOP_JCXZ); goto finish_block;
    op_e4: dyn_read_port_byte_direct(decode_fetchb()); goto next_opcode;
    op_e5: dyn_read_port_word_direct(decode_fetchb()); goto next_opcode;
    op_e6: dyn_write_port_byte_direct(decode_fetchb()); goto next_opcode;
    op_e7: dyn_write_port_word_direct(decode_fetchb()); goto next_opcode;
    op_e8: dyn_call_near_imm(); goto finish_block;
    op_e9: dyn_exit_link(decode.big_op ? (Bit32s)decode_fetchd() : (Bit16s)decode_fetchw()); goto finish_block;
    op_ea: dyn_jmp_far_imm(); goto finish_block;
    op_eb: dyn_exit_link((Bit8s)decode_fetchb()); goto finish_block;
    op_ec: dyn_read_port_byte(); goto next_opcode;
    op_ed: dyn_read_port_word(); goto next_opcode;
    op_ee: dyn_write_port_byte(); goto next_opcode;
    op_ef: dyn_write_port_word(); goto next_opcode;
    op_f2: decode.rep = REP_NZ; continue;
    op_f3: decode.rep = REP_Z; continue;
    op_f5: gen_call_function_raw((void*)dynrec_cmc); goto next_opcode;
    op_f6: dyn_grp3_eb(); goto next_opcode;
    op_f7: dyn_grp3_ev(); goto next_opcode;
    op_f8: gen_call_function_raw((void*)dynrec_clc); goto next_opcode;
    op_f9: gen_call_function_raw((void*)dynrec_stc); goto next_opcode;
    op_fa:
        gen_call_function_raw((void*)&CPU_CLI);
        dyn_check_exception(FC_RETOP);
        goto next_opcode;
    op_fb:
        gen_call_function_raw((void*)&CPU_STI);
        dyn_check_exception(FC_RETOP);
        if (opcode_count >= max_opcodes - 1) max_opcodes++; // Extend for STI
        goto next_opcode;
    op_fc: gen_call_function_raw((void*)dynrec_cld); goto next_opcode;
    op_fd: gen_call_function_raw((void*)dynrec_std); goto next_opcode;
    op_fe:
        if (dyn_grp4_eb()) goto finish_block;
        goto next_opcode;
    op_ff:
        switch (dyn_grp4_ev()) {
            case 0: goto next_opcode;
            case 1: goto core_close_block;
            case 2: goto illegalopcode;
            default: goto next_opcode;
        }

    next_opcode:
        opcode_count++;
        continue;
    }

    // Max opcodes reached
    dyn_set_eip_end();
    dyn_reduce_cycles();
    gen_jmp_ptr(&decode.block->link[0].to, offsetof(CacheBlockDynRec, cache.start));
    dyn_closeblock();
    goto finish_block;

core_close_block:
    dyn_reduce_cycles();
    dyn_return(BR_Normal);
    dyn_closeblock();
    goto finish_block;

illegalopcode:
    dyn_set_eip_last();
    dyn_reduce_cycles();
    dyn_return(BR_Opcode);
    dyn_closeblock();
    goto finish_block;

finish_block:
    decode.page.index--;
    decode.active_block->page.end = (Bit16u)decode.page.index;
    return decode.block;
}