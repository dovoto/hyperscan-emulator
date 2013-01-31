#include <cstdio>
#include <iostream>

#include "hyperscan/cpu.h"

uint8_t* RAM;
uint8_t* NOR;

void dumpRegisters(const hyperscan::CPU &cpu) {
	printf("PC = 0x%08X                N[%c] Z[%c] C[%c] V[%c] T[%c]\n",
		cpu.pc,
		cpu.N ? 'x' : ' ',
		cpu.Z ? 'x' : ' ',
		cpu.C ? 'x' : ' ',
		cpu.V ? 'x' : ' ',
		cpu.T ? 'x' : ' '
	);

	for(int i=0; i<32; i+=4) {
		printf("%sr%d[%08X] %sr%d[%08X] %sr%d[%08X] %sr%d[%08X]\n",
			((i + 0) < 10) ? " " : "", i + 0, cpu.r[i + 0],
			((i + 1) < 10) ? " " : "", i + 1, cpu.r[i + 1],
			((i + 2) < 10) ? " " : "", i + 2, cpu.r[i + 2],
			((i + 3) < 10) ? " " : "", i + 3, cpu.r[i + 3]
		);
	}
}

void dumpMemory() {
	FILE* out = fopen("MEMDUMP", "wb");
	fwrite(RAM, 0x1000000, 1, out);
	fclose(out);
}

// mmio
// TODO: move out

uint8_t cpuReadByte(uint32_t addr) {
	switch(addr >> 24) {
		// NOR:
		case 0x9E:
		case 0x9F:
				return NOR[addr & 0x000FFFFF];
			break;
		// RAM
		case 0x80:
		case 0xA0:
				return RAM[addr & 0x00FFFFFF];
			break;
		// Registers
		case 0x88:
			// XXX P_MIU1_SDRAM_SETTING: SDRAM self refresh
			if(addr == 0x8807006C)
				return 0;

			return -1;
	}

	return -1;
}

uint16_t cpuReadHword(uint32_t addr) {
	return cpuReadByte(addr + 0) << 0 |
		   cpuReadByte(addr + 1) << 8;
}

uint32_t cpuReadWord(uint32_t addr) {
	return cpuReadHword(addr + 0) <<  0 |
		   cpuReadHword(addr + 2) << 16;
}

void cpuWriteByte(uint32_t addr, uint8_t value) {
	switch(addr >> 24) {
		// RAM
		case 0x80:
		case 0xA0:
				RAM[addr & 0x00FFFFFF] = value;
			break;
	}
}

void cpuWriteHword(uint32_t addr, uint16_t value) {
	cpuWriteByte(addr + 0, (value >> 0) & 0xFF);
	cpuWriteByte(addr + 1, (value >> 8) & 0xFF);
}

void cpuWriteWord(uint32_t addr, uint32_t value) {
	cpuWriteHword(addr + 0, (value >>  0) & 0xFFFF);
	cpuWriteHword(addr + 2, (value >> 16) & 0xFFFF);
}

// --- utils
// -- TODO: move to cpu.cpp

// Sign extends x to the size of b bits
int32_t sign_extend(uint32_t x, uint8_t b) {
	uint32_t m = 1UL << (b - 1);

	x = x & ((1UL << b) - 1);
	return (x ^ m) - m;
}

// Retrieve bits s -> (start + size) as 0 -> size
uint32_t bit_range(uint32_t x, uint8_t start, uint8_t size) {
	return (x >> start) & ((1 << size) - 1);
}

// -- cpu emulation
// -- TODO: move to cpu.cpp

void spg290_insn16(hyperscan::CPU &cpu, uint16_t insn) {
	switch(insn >> 12) {
		case 0x00: {
				uint8_t rA = bit_range(insn, 4, 4);
				uint8_t rD = bit_range(insn, 8, 4);
				switch(bit_range(insn, 0, 4)) {
					case 0x00: /* nop */ break;
					case 0x01: cpu.g0[rD] = cpu.g1[rA]; break;
					case 0x02: cpu.g1[rD] = cpu.g0[rA]; break;
					case 0x03: cpu.g0[rD] = cpu.g0[rA]; break;
					case 0x04: if(cpu.conditional(rD)) cpu.pc = cpu.g0[rA] - 2; break;
					case 0x05: if(rA == 0) cpu.T = cpu.conditional(rD); break;
					default:
						fprintf(stderr, "unimplemented 16bit op0, %d\n", bit_range(insn, 0, 4));
						dumpRegisters(cpu); dumpMemory();
						exit(1);
				}
			} break;
		case 0x02: {
				uint32_t &rA = cpu.g0[bit_range(insn, 4, 4)];
				uint32_t &rD = cpu.g0[bit_range(insn, 8, 4)];
				switch(bit_range(insn, 0, 4)) {
					case 0x03: cpu.cmp(rD, rA, true); break;
					case 0x08: rD = cpuReadWord(rA); break;
					case 0x0C: cpuWriteWord(rA, rD); break;
					// XXX: H-bit is not correclty behaving
					case 0x0A: rD = cpuReadWord(rA); rA += 4; break;
					case 0x0E: cpuWriteWord(rA -= 4, rD); break;
					case 0x0F: cpuWriteByte(rA, rD); break;
					default:
						fprintf(stderr, "unimplemented 16bit op2, %d\n", bit_range(insn, 0, 4));
						dumpRegisters(cpu); dumpMemory();
						exit(1);
				}
			} break;
		case 0x03:
				if(insn & 1)
					cpu.r3 = cpu.pc + 4;

				cpu.pc &= 0xFFFFF000;
				cpu.pc |= (bit_range(insn, 1, 11) << 1) - 2;
			break;
		case 0x04:
				if(cpu.conditional(bit_range(insn, 8, 4)))
					cpu.pc += (sign_extend(bit_range(insn, 0, 8), 8) << 1) - 2;
			break;
		case 0x05:
				cpu.g0[bit_range(insn, 8, 4)] = bit_range(insn, 0, 8);
			break;
		case 0x06: {
				uint32_t &rD = cpu.g0[bit_range(insn, 8, 4)];
				uint8_t imm5 = bit_range(insn, 3, 5);
				switch(bit_range(insn, 0, 3)) {
					case 0x04: rD = cpu.bitclr(rD, imm5, true); break;
					case 0x05: rD = cpu.bitset(rD, imm5, true); break;
					case 0x06: cpu.bittst(rD, imm5, true); break;
					default:
						fprintf(stderr, "unimplemented 16bit op6, func%d\n", bit_range(insn, 0, 3));
						dumpRegisters(cpu); dumpMemory();
						exit(1);
				}
			} break;
		case 0x07: {
				uint32_t &rD = cpu.g0[bit_range(insn, 8, 4)];
				uint8_t imm5 = bit_range(insn, 3, 5);
				switch(bit_range(insn, 0, 3)) {
					case 0x00: rD = cpuReadWord(cpu.r2 + (imm5 << 2)); break;
					case 0x04: cpuWriteWord(cpu.r2 + (imm5 << 2), rD); break;
					default:
						fprintf(stderr, "unimplemented 16bit op7, func%d\n", bit_range(insn, 0, 3));
						dumpRegisters(cpu); dumpMemory();
						exit(1);
				}
			} break;
		default:
			fprintf(stderr, "unimplemented (16bit): op=%04X (%d)\n", insn, insn >> 12);
			dumpRegisters(cpu); dumpMemory();
			exit(1);
	}

}

void spg290_insn32(hyperscan::CPU &cpu, uint32_t insn) {
	uint8_t op = insn >> 25;
	switch(op) {
		case 0x00: {
				bool cu = insn & 1;
				uint32_t rDv = bit_range(insn, 20, 5);
				uint32_t rAv = bit_range(insn, 15, 5);
				uint32_t rBv = bit_range(insn, 10, 5);

				uint32_t &rD = cpu.r[rDv];
				uint32_t &rA = cpu.r[rAv];
				uint32_t &rB = cpu.r[rBv];
				switch(bit_range(insn, 1, 6)) {
					case 0x00: /* nop */ break;
					case 0x04: if(cpu.conditional(rBv)) cpu.pc = rA - 4; break;
					case 0x08: rD = cpu.add(rA, rB, cu); break;
					case 0x09: rD = cpu.addc(rA, rB, cu); break;
					case 0x0A: rD = cpu.sub(rA, rB, cu); break;
					case 0x0C: cpu.cmp(rA, rB, bit_range(insn, 20, 2), cu); break;
					case 0x10: rD = cpu.bit_and(rA, rB, cu); break;
					case 0x11: rD = cpu.bit_or(rA, rB, cu); break;
					case 0x15: rD = cpu.bitset(rA, rBv, cu); break;
					case 0x16: cpu.bittst(rA, rBv, cu); break;
					case 0x2B: if(cpu.conditional(rBv)) rD = rA; break;
					case 0x2C: rD = sign_extend(rA,  8); if(cu) cpu.basic_flags(rD); break;
					case 0x2D: rD = sign_extend(rA, 16); if(cu) cpu.basic_flags(rD); break;
					case 0x2E: rD = rA & 0x000000FF; if(cu) cpu.basic_flags(rD); break;
					case 0x2F: rD = rA & 0x0000FFFF; if(cu) cpu.basic_flags(rD); break;
					case 0x38: rD = cpu.shift_left(rA, rBv, cu); break;
					case 0x3A: rD = cpu.shift_right(rA, rBv, cu); break;
					default:
						fprintf(stderr, "unimplemented (0x00): op=%02X\n", bit_range(insn, 1, 6));
						dumpRegisters(cpu); dumpMemory();
						exit(1);
				}
			} break;
		case 0x01:
		case 0x05: {
				bool cu = insn & 1;
				bool shifted = op & 0x04;
				uint32_t &rD = cpu.r[bit_range(insn, 20, 5)];
				uint32_t imm16 = bit_range(insn, 1, 16) << (shifted * 16);
				switch(bit_range(insn, 17, 3)) {
					case 0x00: rD = cpu.add(rD, sign_extend(imm16, 16 * (shifted + 1)), cu); break;
					case 0x02: cpu.cmp(rD, imm16, 3, cu); break;
					case 0x04: rD = cpu.bit_and(rD, imm16, cu); break;
					case 0x05: rD = cpu.bit_or(rD, imm16, cu); break;
					case 0x06: rD = sign_extend(imm16, 16 * (shifted + 1)); break;
				}
			} break;
		case 0x02: {
				// Link
				if(insn & 1)
					cpu.r3 = cpu.pc + 4;

				// Update PC
				cpu.pc &= 0xFC000000;
				cpu.pc |= (bit_range(insn, 1, 24) << 1) - 4;
			} break;
		case 0x03:
		case 0x07: {
				uint32_t &rD = cpu.r[bit_range(insn, 20, 5)];
				uint32_t &rA = cpu.r[bit_range(insn, 15, 5)];
				uint32_t imm12 = sign_extend(bit_range(insn, 3, 12), 12);

				// Pre-increment
				if(op == 0x03)
					rA += imm12;

				switch(insn & 0x07) {
					case 0x00: rD = cpuReadWord(rA); break;
					case 0x01: rD = sign_extend(cpuReadHword(rA), 16); break;
					case 0x02: rD = cpuReadHword(rA); break;
					case 0x03: rD = sign_extend(cpuReadByte(rA), 8); break;
					case 0x04: cpuWriteWord(rA, rD); break;
					case 0x05: cpuWriteHword(rA, rD); break;
					case 0x06: rD = cpuReadByte(rA); break;
					case 0x07: cpuWriteByte(rA, rD); break;
				}

				// Post-increment
				if(op == 0x07)
					rA += imm12;
			} break;
		case 0x04: {
				// TODO: I don't know why this works and not what the docs say
				// Taken from score7 binutils
				if(cpu.conditional(bit_range(insn, 10, 4))) {
					// XXX: docs say this should happen if test fails as well
					if(insn & 1)
						cpu.r3 = cpu.pc + 4;

                    cpu.pc += sign_extend(((insn & 0x01FF8000) >> 5) | (insn & 0x3FE), 20) - 4;
				}
			} break;
		case 0x06:
				// Bullshit.
			break;
		case 0x0C: {
				uint32_t &rD = cpu.r[bit_range(insn, 20, 5)];
				uint32_t &rA = cpu.r[bit_range(insn, 15, 5)];
				uint32_t imm14 = bit_range(insn, 1, 14);

				rD = cpu.bit_and(rA, imm14, insn & 1);
			} break;
		case 0x10: {
				uint32_t &rD = cpu.r[bit_range(insn, 20, 5)];
				uint32_t &rA = cpu.r[bit_range(insn, 15, 5)];
				uint32_t imm15 = bit_range(insn, 0, 15);

				rD = cpuReadWord(rA + sign_extend(imm15, 15));
			} break;
		case 0x12: {
				uint32_t &rD = cpu.r[bit_range(insn, 20, 5)];
				uint32_t &rA = cpu.r[bit_range(insn, 15, 5)];
				uint32_t imm15 = bit_range(insn, 0, 15);

				rD = cpuReadHword(rA + sign_extend(imm15, 15));
			} break;
		case 0x14: {
				uint32_t &rD = cpu.r[bit_range(insn, 20, 5)];
				uint32_t &rA = cpu.r[bit_range(insn, 15, 5)];
				uint32_t imm15 = bit_range(insn, 0, 15);

				cpuWriteWord(rA + sign_extend(imm15, 15), rD);
			} break;
		case 0x18:
				// cache
			break;
		default:
				fprintf(stderr, "unimplemented: op=%02X (0x%08X)\n", op, cpuReadWord(cpu.pc));
				dumpRegisters(cpu); dumpMemory();
				exit(1);
			break;
	}
}

// decodes and executes instruction, returns instruction size
size_t spg290_insn(hyperscan::CPU &cpu) {
	int instructionSize = 2;
	uint32_t instruction = cpuReadHword(cpu.pc);

	if(instruction & 0x8000) {
		instruction &= 0x7FFF;
		instruction |= cpuReadHword(cpu.pc + instructionSize) << 15;
		instruction &= 0x3FFFFFFF;

		instructionSize += 2;

		spg290_insn32(cpu, instruction);
	} else {
		spg290_insn16(cpu, instruction);
	}

	return instructionSize;
}

void loadFileInto(const char* fileName, uint8_t* result) {
	FILE* f = fopen(fileName, "rb");
	if(!f) {
		fprintf(stderr, "bad file: %s", fileName);
		exit(1);
	}

	fseek(f, 0, SEEK_END);
	size_t fileSize = ftell(f);
	fseek(f, 0, SEEK_SET);

	fread(result, fileSize, 1, f);
	fclose(f);
}

int main() {
	hyperscan::CPU cpu;

	RAM = new uint8_t[0x01000000];
	std::fill(RAM, RAM + 0x01000000, 0);

	NOR = new uint8_t[0x00100000];
	std::fill(NOR, NOR + 0x00100000, 0);

	loadFileInto("roms/hsfirmware.bin", NOR);
//	loadFileInto("roms/mini_colors.bin", RAM + 0x000901FC);

	cpu.pc = 0x9F000000;
//	cpu.pc = 0xA0091000;
	while(1)
		cpu.pc += spg290_insn(cpu);

	free(RAM);

	return 0;
}
