/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
#include "vm_local.h"

/* vm_interpreted_sf.c - interpretador da QVM para o SF-100 (substitui qcommon/vm_interpreted.c do ioq3; ver docs/PORT.md). */

static int loadWord(const void *addr) {
	int word;
	memcpy(&word, addr, 4);
	return LittleLong(word);
}

#define SF_MAX_VMS 8
static vm_t *sf_threaded[SF_MAX_VMS];
static int sf_is_threaded(vm_t *vm) { int i; for (i = 0; i < SF_MAX_VMS; i++) if (sf_threaded[i] == vm) return 1; return 0; }
static void sf_set_threaded(vm_t *vm, int on) {
	int i;
	for (i = 0; i < SF_MAX_VMS; i++) if (sf_threaded[i] == vm) { sf_threaded[i] = on ? vm : NULL; return; }
	if (on) for (i = 0; i < SF_MAX_VMS; i++) if (!sf_threaded[i]) { sf_threaded[i] = vm; return; }
}

char *VM_Indent( vm_t *vm ) {
	static char	*string = "                                        ";
	if ( vm->callLevel > 20 ) {
		return string;
	}
	return string + 2 * ( 20 - vm->callLevel );
}

void VM_StackTrace( vm_t *vm, int programCounter, int programStack ) {
	int		count;

	count = 0;
	do {
		Com_Printf( "%s\n", VM_ValueToSymbol( vm, programCounter ) );
		programStack =  *(int *)&vm->dataBase[programStack+4];
		programCounter = *(int *)&vm->dataBase[programStack];
	} while ( programCounter != -1 && ++count < 32 );

}


/*
====================
VM_PrepareInterpreter
====================
*/
void VM_PrepareInterpreter( vm_t *vm, vmHeader_t *header ) {
	int		op;
	int		byte_pc;
	int		int_pc;
	byte	*code;
	int		instruction;
	int		*codeBase;

	sf_set_threaded(vm, 0);							// o código é reexpandido: precisa ser threaded de novo
	vm->codeBase = Hunk_Alloc( vm->codeLength*4, h_high );			// we're now int aligned
//	memcpy( vm->codeBase, (byte *)header + header->codeOffset, vm->codeLength );

	// we don't need to translate the instructions, but we still need
	// to find each instructions starting point for jumps
	int_pc = byte_pc = 0;
	instruction = 0;
	code = (byte *)header + header->codeOffset;
	codeBase = (int *)vm->codeBase;

	// Copy and expand instructions to words while building instruction table
	while ( instruction < header->instructionCount ) {
		vm->instructionPointers[ instruction ] = int_pc;
		instruction++;

		op = (int)code[ byte_pc ];
		codeBase[int_pc] = op;
		if(byte_pc > header->codeLength)
			Com_Error(ERR_DROP, "VM_PrepareInterpreter: pc > header->codeLength");

		byte_pc++;
		int_pc++;

		// these are the only opcodes that aren't a single byte
		switch ( op ) {
		case OP_ENTER:
		case OP_CONST:
		case OP_LOCAL:
		case OP_LEAVE:
		case OP_EQ:
		case OP_NE:
		case OP_LTI:
		case OP_LEI:
		case OP_GTI:
		case OP_GEI:
		case OP_LTU:
		case OP_LEU:
		case OP_GTU:
		case OP_GEU:
		case OP_EQF:
		case OP_NEF:
		case OP_LTF:
		case OP_LEF:
		case OP_GTF:
		case OP_GEF:
		case OP_BLOCK_COPY:
			codeBase[int_pc] = loadWord(&code[byte_pc]);
			byte_pc += 4;
			int_pc++;
			break;
		case OP_ARG:
			codeBase[int_pc] = (int)code[byte_pc];
			byte_pc++;
			int_pc++;
			break;
		default:
			break;
		}

	}
	int_pc = 0;
	instruction = 0;
	
	// Now that the code has been expanded to int-sized opcodes, we'll translate instruction index
	//into an index into codeBase[], which contains opcodes and operands.
	while ( instruction < header->instructionCount ) {
		op = codeBase[ int_pc ];
		instruction++;
		int_pc++;
		
		switch ( op ) {
		// These ops need to translate addresses in jumps from instruction index to int index
		case OP_EQ:
		case OP_NE:
		case OP_LTI:
		case OP_LEI:
		case OP_GTI:
		case OP_GEI:
		case OP_LTU:
		case OP_LEU:
		case OP_GTU:
		case OP_GEU:
		case OP_EQF:
		case OP_NEF:
		case OP_LTF:
		case OP_LEF:
		case OP_GTF:
		case OP_GEF:
			if(codeBase[int_pc] < 0 || codeBase[int_pc] > vm->instructionCount)
				Com_Error(ERR_DROP, "VM_PrepareInterpreter: Jump to invalid instruction number");

			// codeBase[pc] is the instruction index. Convert that into an offset into
			//the int-aligned codeBase[] by the lookup table.
			codeBase[int_pc] = vm->instructionPointers[codeBase[int_pc]];
			int_pc++;
			break;

		// These opcodes have an operand that isn't an instruction index
		case OP_ENTER:
		case OP_CONST:
		case OP_LOCAL:
		case OP_LEAVE:
		case OP_BLOCK_COPY:
		case OP_ARG:
			int_pc++;
			break;

		default:
			break;
		}

	}
}


/*
==============
VM_CallInterpreted (versão SF-100)

Interpretador "threaded" (goto computado): VM_PrepareInterpreter expande o código em palavras de 32 bits e, na primeira chamada de cada VM,
vm_thread() troca cada opcode pelo endereço do seu tratador, então cada instrução termina buscando e saltando direto para a próxima (sem o
switch com checagem de faixa e sem o relê de r0/r1 do original). Pilha de operandos por ponteiro. Superinstruções (mesma extensão em
palavras, então os alvos de salto não mudam): LOCAL+LOAD4, CONST+LOAD4, CONST+ADD, CONST+ARG, LOCAL+ARG, LOCAL+LOAD4+ARG.
==============
*/
enum { OPX_LLOAD4 = OP_CVFI + 1, OPX_CLOAD4, OPX_CADD, OPX_CARG, OPX_LARG, OPX_LLOAD4ARG, OPX_COUNT };

static int vm_op_words(int op) {                       /* palavras do tratador + operando */
	switch (op) {
	case OP_ENTER: case OP_CONST: case OP_LOCAL: case OP_LEAVE: case OP_EQ: case OP_NE: case OP_LTI: case OP_LEI: case OP_GTI: case OP_GEI:
	case OP_LTU: case OP_LEU: case OP_GTU: case OP_GEU: case OP_EQF: case OP_NEF: case OP_LTF: case OP_LEF: case OP_GTF: case OP_GEF:
	case OP_BLOCK_COPY: case OP_ARG:
		return 2;
	default: return 1;
	}
}

static void vm_thread(vm_t *vm, const void *const *disp) {
	int *code = (int *)vm->codeBase; int pc, n = vm->instructionCount, i, total = 0;
	byte *target;
	/* alvos de desvio conhecidos (operando das comparações): nunca fundir uma instrução que é alvo */
	for (pc = 0, i = 0; i < n; i++) { int op = code[pc]; pc += vm_op_words(op); }
	total = pc;
	target = Hunk_AllocateTempMemory(total + 1); Com_Memset(target, 0, total + 1);
	for (pc = 0, i = 0; i < n; i++) {
		int op = code[pc];
		if (op >= OP_EQ && op <= OP_GEF && code[pc + 1] >= 0 && code[pc + 1] <= total) target[code[pc + 1]] = 1;
		pc += vm_op_words(op);
	}
	for (pc = 0, i = 0; i < n; i++) {
		int op = code[pc], w = vm_op_words(op), pc1 = pc + w, newop = op;
		if (pc1 < total && !target[pc1]) {                     /* a próxima instrução existe e não é alvo de desvio */
			int op1 = code[pc1], pc2 = pc1 + vm_op_words(op1);
			if (op == OP_LOCAL && op1 == OP_LOAD4) {
				newop = OPX_LLOAD4;
				if (pc2 < total && !target[pc2] && code[pc2] == OP_ARG) newop = OPX_LLOAD4ARG;
			}
			else if (op == OP_CONST && op1 == OP_LOAD4) newop = OPX_CLOAD4;
			else if (op == OP_CONST && op1 == OP_ADD) newop = OPX_CADD;
			else if (op == OP_CONST && op1 == OP_ARG) newop = OPX_CARG;
			else if (op == OP_LOCAL && op1 == OP_ARG) newop = OPX_LARG;
		}
		code[pc] = (int)(intptr_t)disp[newop];
		pc += w;
	}
	Hunk_FreeTempMemory(target);
}

int	VM_CallInterpreted( vm_t *vm, int *args ) {
	static const void *const disp[OPX_COUNT] = {
		&&L_NOP, &&L_NOP, &&L_BREAK, &&L_ENTER, &&L_LEAVE, &&L_CALL, &&L_PUSH, &&L_POP, &&L_CONST, &&L_LOCAL, &&L_JUMP,
		&&L_EQ, &&L_NE, &&L_LTI, &&L_LEI, &&L_GTI, &&L_GEI, &&L_LTU, &&L_LEU, &&L_GTU, &&L_GEU, &&L_EQF, &&L_NEF, &&L_LTF, &&L_LEF, &&L_GTF, &&L_GEF,
		&&L_LOAD1, &&L_LOAD2, &&L_LOAD4, &&L_STORE1, &&L_STORE2, &&L_STORE4, &&L_ARG, &&L_BLOCK_COPY, &&L_SEX8, &&L_SEX16,
		&&L_NEGI, &&L_ADD, &&L_SUB, &&L_DIVI, &&L_DIVU, &&L_MODI, &&L_MODU, &&L_MULI, &&L_MULU, &&L_BAND, &&L_BOR, &&L_BXOR, &&L_BCOM,
		&&L_LSH, &&L_RSHI, &&L_RSHU, &&L_NEGF, &&L_ADDF, &&L_SUBF, &&L_DIVF, &&L_MULF, &&L_CVIF, &&L_CVFI,
		&&L_LLOAD4, &&L_CLOAD4, &&L_CADD, &&L_CARG, &&L_LARG, &&L_LLOAD4ARG
	};
	int		stack[OPSTACK_SIZE / 4 + 8];
	int		*base, *sp, *pc, *codeImage;
	int		ps, stackOnEntry, arg, dataMask;
	byte	*image;

	if (!sf_is_threaded(vm)) { vm_thread(vm, disp); sf_set_threaded(vm, 1); }

	vm->currentlyInterpreting = qtrue;
	ps = stackOnEntry = vm->programStack;
	image = vm->dataBase; codeImage = (int *)vm->codeBase; dataMask = vm->dataMask;
	ps -= ( 8 + 4 * MAX_VMMAIN_ARGS );
	for ( arg = 0; arg < MAX_VMMAIN_ARGS; arg++ ) *(int *)&image[ ps + 8 + arg * 4 ] = args[ arg ];
	*(int *)&image[ ps + 4 ] = 0;	// return stack
	*(int *)&image[ ps ] = -1;	// will terminate the loop on return
	base = (int *)(((uintptr_t)stack + 15) & ~(uintptr_t)15);
	*base = 0xDEADBEEF; sp = base; pc = codeImage;

#define NEXT() do { const void *t_ = (const void *)(intptr_t)*pc++; goto *t_; } while (0)
#define IMG(a) (*(int *)&image[(a) & dataMask])
#define FP(i) (((float *)sp)[i])
#define BRANCH(cond) do { sp -= 2; if (cond) pc = codeImage + pc[0]; else pc++; NEXT(); } while (0)
	NEXT();

L_NOP:	NEXT();
L_BREAK:	vm->breakCount++; NEXT();
L_CONST:	sp++; *sp = *pc++; NEXT();
L_LOCAL:	sp++; *sp = *pc++ + ps; NEXT();
L_LOAD4:	*sp = IMG(*sp); NEXT();
L_LOAD2:	*sp = *(unsigned short *)&image[*sp & dataMask]; NEXT();
L_LOAD1:	*sp = image[*sp & dataMask]; NEXT();
L_STORE4:	IMG(sp[-1]) = sp[0]; sp -= 2; NEXT();
L_STORE2:	*(short *)&image[sp[-1] & dataMask] = (short)sp[0]; sp -= 2; NEXT();
L_STORE1:	image[sp[-1] & dataMask] = (byte)sp[0]; sp -= 2; NEXT();
L_ARG:	IMG(*pc + ps) = *sp; sp--; pc++; NEXT();
L_BLOCK_COPY:	VM_BlockCopy(sp[-1], sp[0], *pc); pc++; sp -= 2; NEXT();
L_PUSH:	sp++; NEXT();
L_POP:	sp--; NEXT();
L_ENTER:	ps -= *pc++; NEXT();

L_CALL: {
	int target = *sp;
	*(int *)&image[ ps ] = (int)(pc - codeImage);
	sp--;
	if ( target < 0 ) {
		intptr_t r;
		vm->programStack = ps - 4;
		*(int *)&image[ ps + 4 ] = -1 - target;
		r = vm->systemCall( (intptr_t *)&image[ ps + 4 ] );
		sp++; *sp = (int)r;
		pc = codeImage + *(int *)&image[ ps ];
	} else if ( (unsigned)target >= (unsigned)vm->instructionCount ) {
		Com_Error( ERR_DROP, "VM program counter out of range in OP_CALL" );
		return 0;
	} else {
		pc = codeImage + vm->instructionPointers[ target ];
	}
	NEXT();
}
L_LEAVE: {
	int saved;
	ps += *pc;
	saved = *(int *)&image[ ps ];
	if ( saved == -1 ) goto done;
	if ( (unsigned)saved >= (unsigned)vm->codeLength ) { Com_Error( ERR_DROP, "VM program counter out of range in OP_LEAVE" ); return 0; }
	pc = codeImage + saved;
	NEXT();
}
L_JUMP:
	if ( (unsigned)*sp >= (unsigned)vm->instructionCount ) { Com_Error( ERR_DROP, "VM program counter out of range in OP_JUMP" ); return 0; }
	pc = codeImage + vm->instructionPointers[ *sp ]; sp--; NEXT();

L_EQ:	BRANCH(sp[1] == sp[2]);
L_NE:	BRANCH(sp[1] != sp[2]);
L_LTI:	BRANCH(sp[1] < sp[2]);
L_LEI:	BRANCH(sp[1] <= sp[2]);
L_GTI:	BRANCH(sp[1] > sp[2]);
L_GEI:	BRANCH(sp[1] >= sp[2]);
L_LTU:	BRANCH((unsigned)sp[1] < (unsigned)sp[2]);
L_LEU:	BRANCH((unsigned)sp[1] <= (unsigned)sp[2]);
L_GTU:	BRANCH((unsigned)sp[1] > (unsigned)sp[2]);
L_GEU:	BRANCH((unsigned)sp[1] >= (unsigned)sp[2]);
L_EQF:	BRANCH(FP(1) == FP(2));
L_NEF:	BRANCH(FP(1) != FP(2));
L_LTF:	BRANCH(FP(1) < FP(2));
L_LEF:	BRANCH(FP(1) <= FP(2));
L_GTF:	BRANCH(FP(1) > FP(2));
L_GEF:	BRANCH(FP(1) >= FP(2));

L_NEGI:	*sp = -*sp; NEXT();
L_ADD:	sp--; sp[0] = sp[0] + sp[1]; NEXT();
L_SUB:	sp--; sp[0] = sp[0] - sp[1]; NEXT();
L_DIVI:	sp--; sp[0] = sp[0] / sp[1]; NEXT();
L_DIVU:	sp--; sp[0] = (int)((unsigned)sp[0] / (unsigned)sp[1]); NEXT();
L_MODI:	sp--; sp[0] = sp[0] % sp[1]; NEXT();
L_MODU:	sp--; sp[0] = (int)((unsigned)sp[0] % (unsigned)sp[1]); NEXT();
L_MULI:	sp--; sp[0] = sp[0] * sp[1]; NEXT();
L_MULU:	sp--; sp[0] = (int)((unsigned)sp[0] * (unsigned)sp[1]); NEXT();
L_BAND:	sp--; sp[0] = sp[0] & sp[1]; NEXT();
L_BOR:	sp--; sp[0] = sp[0] | sp[1]; NEXT();
L_BXOR:	sp--; sp[0] = sp[0] ^ sp[1]; NEXT();
L_BCOM:	*sp = ~*sp; NEXT();
L_LSH:	sp--; sp[0] = sp[0] << sp[1]; NEXT();
L_RSHI:	sp--; sp[0] = sp[0] >> sp[1]; NEXT();
L_RSHU:	sp--; sp[0] = (int)((unsigned)sp[0] >> sp[1]); NEXT();
L_NEGF:	FP(0) = -FP(0); NEXT();
L_ADDF:	sp--; FP(0) = FP(0) + FP(1); NEXT();
L_SUBF:	sp--; FP(0) = FP(0) - FP(1); NEXT();
L_DIVF:	sp--; FP(0) = FP(0) / FP(1); NEXT();
L_MULF:	sp--; FP(0) = FP(0) * FP(1); NEXT();
L_CVIF:	FP(0) = (float)sp[0]; NEXT();
L_CVFI:	*sp = Q_ftol(FP(0)); NEXT();
L_SEX8:	*sp = (signed char)*sp; NEXT();
L_SEX16:	*sp = (short)*sp; NEXT();

/* superinstruções: mesma extensão em palavras da sequência original (o resto da sequência é pulado) */
L_LLOAD4:	sp++; *sp = IMG(pc[0] + ps); pc += 2; NEXT();                   /* LOCAL x; LOAD4 */
L_CLOAD4:	sp++; *sp = IMG(pc[0]); pc += 2; NEXT();                        /* CONST x; LOAD4 */
L_CADD:	*sp += pc[0]; pc += 2; NEXT();                                  /* CONST x; ADD */
L_CARG:	IMG(pc[2] + ps) = pc[0]; pc += 3; NEXT();                       /* CONST v; ARG k */
L_LARG:	IMG(pc[2] + ps) = pc[0] + ps; pc += 3; NEXT();                  /* LOCAL x; ARG k */
L_LLOAD4ARG:	IMG(pc[3] + ps) = IMG(pc[0] + ps); pc += 4; NEXT();             /* LOCAL x; LOAD4; ARG k */

done:
	vm->currentlyInterpreting = qfalse;
	if (sp != base + 1 || *base != (int)0xDEADBEEF)
		Com_Error(ERR_DROP, "Interpreter error: opStack[0] = %X, opStackOfs = %d", base[0], (int)(sp - base));
	vm->programStack = stackOnEntry;
	return *sp;
}
