<!-- SPDX-License-Identifier: GPL-2.0-only -->
# IOKit test stubs (syntax gate only)

These headers are **not** IOKit. They are a minimal shape of the handful of
types and methods `xnu/` uses, so the IOKit glue can be compiled on any host:

```sh
tests/run.sh
```

`tests/run.sh` compiles `xnu/NativeMaliGpuService.cpp` and
`xnu/NativeMaliXnuSupport.cpp` against this directory with
`-fsyntax-only -Wall -Wextra -Werror`.

What the gate proves:

- the glue parses, its declarations match its definitions, and it does not
  accumulate warnings;
- the pure interfaces (`NativeMaliRegisterTarget`, `NativeMaliPageSource`,
  `NativeMaliMemorySource`, `NativeMaliCacheMaintainer`) are still the ones the
  glue implements.

What it does **not** prove:

- that these signatures exist in XNU, or that they have these semantics;
- anything about `IOBufferMemoryDescriptor`, `IODeviceMemory` or `IOLock`
  behaviour;
- anything about the target kernel ABI.

It is a rot detector, not a build. The real compile is `build-arm-driver.py` on
the UniXNU kernel tree; see `docs/KEXT_BUILD.md`. When a real header mismatch is
found there, fix this stub to match the header — never the other way round.

`-Wno-overloaded-virtual` is passed because `IOService` and `OSObject` both
declare `init()` in the real headers too; the stub reproduces that shape, and
XNU's own build disables that warning.
