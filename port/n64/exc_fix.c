// O codigo original (MSVC/x86) assume dois comportamentos que a CPU/FPU do N64 nao tem:
//  1) acessos desalinhados (ex.: *(u32*)ponteiro_de_byte)  -> excecoes de endereco (codigos 4 e 5);
//  2) conversao float->int fora da faixa: o x86 devolve 0x80000000 (0x8000000000000000 em 64 bits),
//     a FPU do N64 levanta excecao "unimplemented" (codigo 15), que nao pode ser mascarada.
// Este tratador emula os dois casos e retoma a execucao, em vez de trocar cada ocorrencia no codigo.
// Se a instrucao que falhou esta num delay slot (BD), o desvio tambem e emulado.
#include <libdragon.h>
#include <stdint.h>
#include <string.h>

static exception_handler_t gOld;
static uint32_t gFailed; /* tentativas de correcao que falharam */
uint32_t gta2_n64_excfix_n[2]; /* emulated so far: [0] unaligned accesses, [1] float->int out of range (each one costs a full exception) */

static uint64_t rd_be(uint32_t a, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
    {
        v = (v << 8) | *(volatile uint8_t *)(a + (uint32_t)i);
    }
    return v;
}

static void wr_be(uint32_t a, uint64_t v, int n)
{
    for (int i = n - 1; i >= 0; i--)
    {
        *(volatile uint8_t *)(a + (uint32_t)i) = (uint8_t)v;
        v >>= 8;
    }
}

// Aplica o efeito da instrucao 'insn' (que esta em 'pc') como o hardware faria sem a excecao.
// Retorna 1 se tratou.
static int fix_insn(const exception_t *ex, reg_block_t *r, uint32_t insn, uint32_t pc)
{
    const uint32_t op = insn >> 26, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;

    if (ex->code == 4 || ex->code == 5) // AdEL / AdES
    {
        ++gta2_n64_excfix_n[0];
        const uint32_t addr = (uint32_t)r->gpr[rs] + (uint32_t)(int32_t)(int16_t)insn;
        if (addr == pc)
        {
            return 0;
        }
        uint8_t *fpr = (uint8_t *)&r->fpr[rt];
        switch (op)
        {
            case 0x23: r->gpr[rt] = (uint64_t)(int64_t)(int32_t)rd_be(addr, 4); return 1;            // lw
            case 0x27: r->gpr[rt] = rd_be(addr, 4); return 1;                                       // lwu
            case 0x21: r->gpr[rt] = (uint64_t)(int64_t)(int16_t)rd_be(addr, 2); return 1;            // lh
            case 0x25: r->gpr[rt] = rd_be(addr, 2); return 1;                                       // lhu
            case 0x37: r->gpr[rt] = rd_be(addr, 8); return 1;                                       // ld
            case 0x2B: wr_be(addr, r->gpr[rt], 4); return 1;                                        // sw
            case 0x29: wr_be(addr, r->gpr[rt], 2); return 1;                                        // sh
            case 0x3F: wr_be(addr, r->gpr[rt], 8); return 1;                                        // sd
            case 0x31: { uint32_t v = (uint32_t)rd_be(addr, 4); memcpy(fpr + 4, &v, 4); } return 1; // lwc1
            case 0x39: { uint32_t v; memcpy(&v, fpr + 4, 4); wr_be(addr, v, 4); } return 1;         // swc1
            case 0x35: { uint64_t v = rd_be(addr, 8); memcpy(fpr, &v, 8); } return 1;               // ldc1
            case 0x3D: { uint64_t v; memcpy(&v, fpr, 8); wr_be(addr, v, 8); } return 1;             // sdc1
            default: return 0;
        }
    }
    if (ex->code == 15 && op == 0x11) // FPE em instrucao COP1
    {
        const uint32_t fmt = rs, funct = insn & 63, fd = (insn >> 6) & 31;
        if ((fmt == 0x10 || fmt == 0x11) && ((funct >= 0x08 && funct <= 0x0F) || funct == 0x24 || funct == 0x25))
        {
            uint8_t *dst = (uint8_t *)&r->fpr[fd];
            if (funct == 0x24 || (funct >= 0x0C && funct <= 0x0F)) // *.w.* : resultado de 32 bits
            {
                uint32_t v = 0x80000000u;
                memcpy(dst + 4, &v, 4);
            }
            else // *.l.* : 64 bits
            {
                uint64_t v = 0x8000000000000000ull;
                memcpy(dst, &v, 8);
            }
            r->fc31 &= ~0x0003F000u; // limpa os bits de causa
            ++gta2_n64_excfix_n[1];
            return 1;
        }
    }
    return 0;
}

// Destino do desvio 'insn' em 'bpc', com os registradores antes do delay slot.
// *link = registrador a receber bpc+8 (0 = nenhum). Retorna 0 se nao e um desvio conhecido.
static int branch_target(const reg_block_t *r, uint32_t insn, uint32_t bpc, uint32_t *np, uint32_t *link)
{
    const uint32_t op = insn >> 26, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
    const uint32_t tgt = bpc + 4 + (uint32_t)((int32_t)(int16_t)insn << 2);
    const int64_t a = (int64_t)r->gpr[rs], b = (int64_t)r->gpr[rt];
    int taken;
    *link = 0;
    switch (op)
    {
        case 0x00: // SPECIAL
            if ((insn & 63) == 8) { *np = (uint32_t)r->gpr[rs]; return 1; }                                   // jr
            if ((insn & 63) == 9) { *np = (uint32_t)r->gpr[rs]; *link = (insn >> 11) & 31; return 1; }        // jalr
            return 0;
        case 0x02: *np = ((bpc + 4) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2); return 1;                  // j
        case 0x03: *np = ((bpc + 4) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2); *link = 31; return 1;       // jal
        case 0x04: case 0x14: taken = (a == b); break;                                                        // beq / beql
        case 0x05: case 0x15: taken = (a != b); break;                                                        // bne / bnel
        case 0x06: case 0x16: taken = (a <= 0); break;                                                        // blez / blezl
        case 0x07: case 0x17: taken = (a > 0); break;                                                         // bgtz / bgtzl
        case 0x01: // REGIMM
            switch (rt & 0x13)
            {
                case 0x00: taken = (a < 0); break;                                                            // bltz(l)
                case 0x01: taken = (a >= 0); break;                                                           // bgez(l)
                case 0x10: taken = (a < 0); *link = 31; break;                                                // bltzal(l)
                default:   taken = (a >= 0); *link = 31; break;                                               // bgezal(l)
            }
            break;
        case 0x11: // COP1: bc1f/bc1t(l)
            if (rs != 8) return 0;
            taken = (((r->fc31 >> 23) & 1) == (rt & 1));
            break;
        default: return 0;
    }
    *np = taken ? tgt : bpc + 8;
    if (!taken) *link = (op == 0x01 && (rt & 0x10)) ? 31 : 0; // bltzal/bgezal ligam sempre
    return 1;
}

static void handler(exception_t *ex)
{
    reg_block_t *r = ex->regs;
    const uint32_t epc = r->epc;
    if (!(r->cr & 0x80000000u))
    {
        if (fix_insn(ex, r, *(volatile uint32_t *)epc, epc))
        {
            r->epc = epc + 4;
            return;
        }
    }
    else // a instrucao esta no delay slot de um desvio em 'epc'
    {
        uint32_t np, link;
        if (branch_target(r, *(volatile uint32_t *)epc, epc, &np, &link) &&
            fix_insn(ex, r, *(volatile uint32_t *)(epc + 4), epc + 4))
        {
            if (link) r->gpr[link] = epc + 8;
            r->epc = np;
            r->cr &= ~0x80000000u;
            return;
        }
    }
    gFailed++;
    if (gOld)
    {
        gOld(ex);
    }
}

void gta2_install_exception_fixups(void)
{
    gOld = register_exception_handler(handler);
}
