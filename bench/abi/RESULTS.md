# ABI v1 measurement (x86-64 Linux)

**Question**: which LLVM calling convention should C7-to-C7 calls use? (`kC7CallConv` in `compiler/codegen/lower.cpp`)

**Method** (`python3 bench/abi/run.py [runs] [program ...|report]`): each C7 program is compiled once to LLVM IR; only the
calling-convention keyword is rewritten (`fastcc` -> C / `swiftcc` / `preserve_mostcc`) for *all* C7 functions, then
`llc -O2` + `gcc` link. Every variant must print the same output (checked) and the outputs were verified independently
in Python. CPU time (user+sys) of 5 runs, min and median. Two pipelines: `raw` = exactly what `c7c` emits today (llc only),
`opt` = `opt -passes=function(mem2reg,sroa,instcombine,simplifycfg)` first (no inlining, so calls remain).

**Environment**: Intel Xeon @ 2.30GHz, **1 core** (shared sandbox), LLVM 18, gcc 13.3. Differences below ~3% are noise here.

**Programs**: `scalar` fib(40) with i32/i64; `pair` 16-byte struct by value (arg+return); `triple` 24-byte; `big` 32-byte;
`result` `Result<i64,E>` + `try`; `strres` `Result<Str,E>` (24 bytes) + `try`.

**Not measured**: the register-assignment draft of spec section 13.6 (the spec is not in the repository and the draft was
never implemented). `fastcc`, C, `swiftcc`, `preserve_most` are the LLVM conventions available as stand-ins.

## Results

CPU seconds, min / median of 5 runs (lower is better); (ratio) = min relative to fastcc

### pipeline: raw
| program | fastcc | c | swiftcc | preserve_most |
|---|---|---|---|---|
| scalar | 0.428 / 0.441 (1.00x) | 0.418 / 0.438 (0.98x) | 0.434 / 0.445 (1.01x) | 0.434 / 0.441 (1.01x) |
| pair | 0.498 / 0.503 (1.00x) | 0.498 / 0.501 (1.00x) | 0.493 / 0.539 (0.99x) | 0.620 / 0.634 (1.25x) |
| triple | 0.346 / 0.347 (1.00x) | 0.347 / 0.348 (1.00x) | 0.348 / 0.355 (1.01x) | 0.433 / 0.437 (1.25x) |
| big | 0.391 / 0.395 (1.00x) | 0.394 / 0.398 (1.01x) | 0.257 / 0.257 (0.66x) | 0.573 / 0.578 (1.47x) |
| result | 0.519 / 0.539 (1.00x) | 0.516 / 0.531 (0.99x) | 0.522 / 0.543 (1.01x) | 0.618 / 0.637 (1.19x) |
| strres | 0.427 / 0.434 (1.00x) | 0.426 / 0.441 (1.00x) | 0.426 / 0.427 (1.00x) | 0.421 / 0.444 (0.99x) |

### pipeline: opt
| program | fastcc | c | swiftcc | preserve_most |
|---|---|---|---|---|
| scalar | 0.430 / 0.434 (1.00x) | 0.441 / 0.452 (1.03x) | 0.433 / 0.436 (1.01x) | 0.434 / 0.439 (1.01x) |
| pair | 0.578 / 0.589 (1.00x) | 0.585 / 0.588 (1.01x) | 0.586 / 0.596 (1.01x) | 0.580 / 0.590 (1.00x) |
| triple | 0.357 / 0.366 (1.00x) | 0.355 / 0.355 (0.99x) | 0.354 / 0.354 (0.99x) | 0.376 / 0.377 (1.05x) |
| big | 0.328 / 0.341 (1.00x) | 0.326 / 0.329 (1.00x) | 0.220 / 0.228 (0.67x) | 0.395 / 0.408 (1.21x) |
| result | 0.659 / 0.661 (1.00x) | 0.663 / 0.678 (1.01x) | 0.664 / 0.678 (1.01x) | 0.752 / 0.759 (1.14x) |
| strres | 0.372 / 0.374 (1.00x) | 0.368 / 0.371 (0.99x) | 0.373 / 0.373 (1.00x) | 0.371 / 0.372 (1.00x) |

### .text bytes (opt pipeline)
| program | fastcc | c | swiftcc | preserve_most |
|---|---|---|---|---|
| scalar | 3802 | 3802 | 3802 | 4362 |
| pair | 3898 | 3898 | 3898 | 4458 |
| triple | 3914 | 3914 | 3914 | 4490 |
| big | 4090 | 4090 | 3962 | 4634 |
| result | 3866 | 3866 | 3866 | 4426 |
| strres | 3898 | 3898 | 3898 | 4458 |

## Reading the numbers

- `fastcc` and the C convention are indistinguishable (all within 3%).
- `preserve_most` is slower on call-heavy code (up to 1.47x): callee-saved register pressure outweighs cheaper callers.
- `swiftcc` wins only for the 32-byte aggregate (`big`: 0.66x, i.e. 1.5x faster) and is equal elsewhere, with the same code size.
  Disassembly shows why: `fastcc` returns a 4-word aggregate through hidden-pointer memory and passes part of the arguments on
  the stack; `swiftcc` returns it in rax/rdx/rcx/r8 and passes arguments in registers. A 3-word aggregate (`triple`,
  `Result<Str,E>`) is already returned in registers by `fastcc`, so there is nothing to win.
- The `opt` pipeline is not uniformly faster than `raw` (e.g. `result` 0.52s -> 0.66s, `big` 0.39s -> 0.33s); it is not evidence
  for or against adding optimisation passes — that is stage 9 work.
- Full test suite (67 tests) passes under `fastcc`, `swiftcc` and `preserve_mostcc`, so switching is a one-line change.

## Decision (provisional, x86-64 Linux only)

**ABI v1 = `fastcc`** (unchanged). No convention shows a benefit that justifies leaving it, except `swiftcc` on 4-word
returns, which the current standard library does not produce. Reconsider `swiftcc` if 4-word-or-larger by-value returns become
common, or when the 13.6 register draft can be compared. Re-measure on every new target (AArch64, x86-32, Windows).
