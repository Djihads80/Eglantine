# Ampere texture operand layout (TEX / TLD4)

From Mesa NAK `sm70_encode.rs` (`OpTld4`) and `nak_nir_lower_tex.c`.

## TLD4 fields (opcode 0xb63, cbuf handle; 0xf63 bindless uniform handle)

| Bits | Field |
|---|---|
| 16-23 | Rd (components 0-1 when split) |
| 24-31 | Ra: first source list |
| 32-39 | Rb: second source list |
| 40-53 | cbuf handle offset; 54-58 binding |
| 60 | scalar (sources rebalanced) |
| 61-63 | dim: 0 1D, 4 1D array, 1 2D, 5 2D array, 2 3D, 3 cube, 7 cube array |
| 64-71 | second Rd (components 2-3) |
| 72-75 | channel mask |
| 76-77 | offset mode: 1 AOFFI (one packed word), 2 per-pixel/PTP (two words), 3 invalid |
| 78 | depth compare |
| 79 | .F16 results (Eglantine handles it before the case) |
| 81-83 | fault predicate (7 = none) |
| 87-88 | gather component; 90 nodep |

## Source lists

- Ra list: `[array index (u16 int)?, x, y]`.
- Rb list: `[offset word | two PTP words]?` then `[depth reference]?`.
- When the Rb list is empty, the coordinates split evenly: n = 3 gives Ra = (s0, s1), Rb = s2;
  n = 2 gives Ra = s0, Rb = s1.
- RZ (255) as a base reads zero for every element.

## Maxwell TLD4 target (`texture_gather.cpp`)

Coordinate regs from bits 8-15 read `[array?, x, y]` (array via 16-bit u->f). Meta regs from bits
20-27 read the handle first (bindless only), then AOFFI (1) or PTP (2) words, then the reference.
Type 28-30 (2 = 2D, 3 = 2D array), DC bit 50, offset type 54-55, component 56-57. Eglantine's
lowering stages coordinates in R250+ and meta in R246+.

SPIR-V: PTP offsets must fold to constants (`ConstOffsets`). Otherwise the emitter logs "Not all
arguments in PTP are immediate, ignoring" and drops them.
