# Uranium Progress Log

Last updated: 2026-04-05

## Current State

Uranium artik calisan ve genislemis bir programlama dili/runtime durumunda.
Cekirdek compiler + VM + bytecode hattinin ustune package manager, stdlib, async runtime,
GUI, GC ve JIT katmanlari eklendi.
Son turda Godot entegrasyonu da geldi: native scaffold uretimi, command builder ve stdlib modulu.

## Completed Milestones

### Language Core

- bytecode compiler + VM
- `.urc` compiled output
- `.ura` runnable archive packaging
- `fn`, `let`, `const`, `return`
- `if`, `elif`, `else`, `while`, `for`
- `switch/case/default`
- `match/case/default` with literal/path/array/object patterns
- `break`, `continue`
- `throw`, `try/catch/finally`
- closures / nested functions / upvalues
- `async fn` + `await`
- arrays and maps
- destructuring declarations for arrays and maps
- classes, instances, methods, `this`
- inheritance + `super`
- `enum`
- `interface` / `trait` contracts
- type annotations
- generic parameter syntax
- named arguments
- default parameter values
- imports, `from ... import ...`, aliases
- module visibility: `export` / `private`

### Runtime

- Uranium-owned heap
- mark/sweep GC
- tiered young/full collection cycle with generation accounting
- dynamic VM stack/frame/handler growth
- task scheduler for async execution
- runtime exception handling
- native function bridge

### Performance

- peephole optimizer
- bytecode fast-path JIT tier
- native machine-code JIT for a narrow hot numeric subset
- native machine-code JIT now also handles numeric locals and terminal numeric equality

### Standard Library

Implemented module groups include:

- `core`
- `math`
- `text`
- `geometry`
- `stats`
- `random`
- `time`
- `units`
- `physics`
- `finance`
- `fs`
- `process`
- `json`
- `http`
- `timer`
- `thread`
- `async`
- `path`
- `env`
- `assert`
- `gc`
- `regex`
- `encoding`
- `log`
- `gui`
- `godot`
- `net` (Sockets & TCP Servers)
- `crypto` (Hashing & Base64)
- `std`

### Packaging And Distribution

- `uranium.pkg` manifest support
- dependency field support in manifests
- local filesystem registry
- semver dependency ranges: exact / `^` / `~` / `*`
- publish flow
- lockfile generation via `uranium.lock`
- lockfile integrity metadata
- dependency installation into `.uranium/packages/...`
- lockfile-first deterministic install
- dependency update/remove lifecycle commands
- dependency-aware import resolution through lockfile

### Tooling

- formatter CLI: `--fmt`, `--fmt-check`
- linter CLI: `--lint`
- debugger tooling: `--debug`, `--debug-run`
- basic stdio LSP server: `--lsp`
- UMake task runner: `--make`, `--make-file`, `--make-list`
- UMake variables + include support
- LSP currently supports:
  - initialize / shutdown / exit
  - didOpen / didChange / didSave diagnostics
  - document formatting

Supported package commands:

- `uranium --registry-init <dir>`
- `uranium --publish <pkg> [registry]`
- `uranium --lock <pkg> [registry]`
- `uranium --install <pkg> [registry]`
- `uranium --update <pkg> [registry]`
- `uranium --remove <pkg> <dependency> [registry]`
- `uranium --pack <pkg> [output.ura]`
- `uranium --init-package <dir>`
- `uranium --make [target]`
- `uranium --make-file <path> [target]`
- `uranium --make-list [path]`

## Recent Validation

### Core Test Suite

Validated successfully:

- `cmake --build builddbg --config Debug`
- `cmake --build buildcheck --config Debug`
- `builddbg\\Debug\\uranium.exe --test tests`

Latest observed result:

- `8 passed, 0 failed`

### Tooling Validation

Validated successfully:

- `uranium --help`
- `uranium --fmt-check tests/runtime_smoke_test.ur`
- `uranium --lint test_lib.ur`
- `uranium --debug tests/runtime_smoke_test.ur`
- `uranium --lsp` with initialize + didOpen + formatting + shutdown flow

### Package Manager Flow

Validated with demo packages under `package_registry_demo/`:

- registry initialization
- package publish for `color_tools` and `greetings`
- lockfile generation for `demo_app`
- dependency installation into `.uranium/packages`
- running `demo_app` from source through installed dependencies
- packing and running `demo_app.ura`

Observed runtime output:

- `greet:[HELLO URANIUM] / ready`

Additional semver/integrity flow validated with demo packages under `package_registry_semver_demo/`:

- semver lock resolution selected `number_tools@1.2.0` for `^1.0.0`
- lockfile wrote integrity fields for each resolved package
- `--install` stayed pinned to the existing lock after publishing `1.2.5`
- `--update` refreshed the lock and moved the app to `number_tools@1.2.5`
- `--remove` deleted a dependency from the manifest and rewrote an empty lock/install tree

## Important Demo Assets

- `package_registry_demo/color_tools`
- `package_registry_demo/greetings`
- `package_registry_demo/app`
- `runtime_upgrade_demo.ur`
- `async_demo.ur`
- `optimizer_jit_demo.ur`
- `gui_demo.ur`
- `godot_demo.ur`
- `system_demo.ur`
- `language_final_demo.ur`
- `package_registry_semver_demo/app`
- `package_registry_semver_demo/wrapper`
- `package_registry_semver_demo/number_tools_v100`
- `package_registry_semver_demo/number_tools_v120`

## Latest Runtime & Compiler Upgrades (Enterprise Phase)

- **Aşırı Sıkı Tip Denetleyici (Strict Static Typing):** Compile-time hata yakalama, değişken türünü kaybetmeyen AST doğrulaması (örneğin `let x: Number = "str"` durdurulur).
- **Control-Flow JIT Compiler:** Native x86_64 JIT derleyicisi devasa bir atımla sadece matematik işlemlerini değil, tüm IF koşullarını ve Loops/While iterasyonlarını `jmp/jz` makine yamalarıyla tamamen compile eder hale getirildi. 
- **Anti-Fragmentation Object Pool GC:** Mark & Sweep GC, bellek parçalanmasını sıfırlayacak `Placement New` kullanan yüksek performanslı nesne havuzlama (Object Pooling) mimarisine kavuştu.
- heap now tracks `youngObjects`, `oldObjects`, `youngBytes`, `oldBytes`
- GC stats expose `minorCollections`, `fullCollections` and last collection mode
- VM now triggers young collections first and escalates to full collections over time

## Latest Language Surface Upgrade

- `enum Name { A, B = 7, C }` now compiles to map-backed enum values
- `interface` and `trait` declarations now work as compile-time contracts
- classes can use `implements`
- function/class declarations now accept generic parameter syntax like `fn id<T>(value: T)`
- variable, parameter and return type annotations are parsed and preserved as metadata
- default parameter values and named arguments now work in source and `.urc`
- destructuring declarations support array and object/map patterns
- `match` now supports literal, enum-path, array and object patterns

## Latest Package Manager Upgrade

- manifests now accept exact, caret, tilde and wildcard dependency requests
- lockfiles now store resolved exact versions plus per-package integrity fields
- installs are now lockfile-first and deterministic instead of silently re-resolving
- publishing writes per-package registry metadata for integrity lookup
- CLI now supports `--update` and `--remove`
- semver demo assets were added under `package_registry_semver_demo/`

## Latest Standard Library Upgrade

- **Networking**: `netTcpListen`, `netTcpAccept`, `netTcpReceive`, `netTcpSend`, `netTcpClose` added as Native C++ Sockets. You can now build TCP connections and Servers.
- **Cryptography**: `cryptoHashSha256`, `cryptoBase64Encode`, `cryptoBase64Decode` added using Windows Cryptography API.

## Known Remaining Gaps

- type system artik compile-time mismatch ve generic binding yapiyor, ama tam constraint solver / derin inference degil
- native JIT branch-aware hale geldi, ancak hala tum property/call/object agirlikli fonksiyonlari native backend'e almiyor
- GC young/full + remembered-set seviyesinde guclu, ama incremental/compacting collector degil
- concurrency buyudu, ama scheduler/worker/thread modeli hala daha fazla birlesebilir
- tooling ve package ekosistemi guclu, ama remote registry/auth ve daha zengin debugger/LSP hala sonraki adimlar

## Recommended Next Priorities

1. Gelişmiş GUI Framework mimarisi (Cross-Platform)
2. IDE ve Debugger'ın profesyonel seviyeye çıkartılması
3. Remote Package Manager HTTP güvenlik mekanizmaları

## Notes

## Latest Runtime Safety Upgrade

- quoted external import alias form now works: `import "./lib.ur" as "lib"`
- standard/urlib imports stay unquoted: `import math as math`
- VM now has a loop broker that aborts probable infinite loops / non-yielding hot paths

## Latest Type/JIT/GC/Concurrency Upgrade

- shared type system helpers were added in `src/type_system.*`
- compiler now performs real static mismatch checks for typed variables, assignments, returns, operators and known function calls
- generic calls now infer concrete type bindings both statically and at runtime
- VM now validates typed function arguments and return values during execution
- generic functions now create cached runtime specializations keyed by concrete type bindings
- GC now tracks a remembered set and uses a write barrier to support old-to-young references safely
- native JIT widened beyond pure arithmetic to cover bool constants/results, `!`, typed equality and typed conditional jumps
- thread stdlib now supports transferable array/map payloads over channels instead of string-only messages
- mutex primitives were added: create, lock, tryLock, unlock
- worker pools and async I/O jobs were added for file read/write and HTTP GET
- regression coverage was added in `tests/advanced_runtime_test.ur`
- negative compile-time coverage was added in `tests/fixtures/type_errors/static_type_mismatch.ur`

## Latest JIT / Fast-Path Stabilization

- fast-path planner artik lineer degil, kontrol-akisi farkinda stack-depth analizi yapiyor
- birden fazla `return` iceren `if/else` fonksiyonlari artik plan uretebiliyor
- fast-path metadata now stores per-instruction entry stack depth
- native JIT candidate analizi artik branch/merge durumlarini izliyor
- native emitter artik instruction-local stack depth ile kod uretiyor
- VM fast-path executor artik `jump`, `jump_if_false` ve `loop` calistirabiliyor
- `tests/advanced_runtime_test.ur` tekrar yesile dondu
- su an test paketi sonucu: `4 passed, 0 failed`

## Latest AOT / Native Binary Compile Upgrade

- Uranium artik `uranium app.ur --compile` ve `uranium --compile app.ur` akisini destekliyor
- `.ur`, `.urc` ve `.ura` girdileri self-contained `.exe` olarak paketlenebiliyor
- uretilen binary kendi icinde Uranium runtime + gomulu `.ura` payload'i tasiyor
- runtime acilisinda kendi executable tail'ini okuyup embedded payload varsa dogrudan onu calistiriyor
- kaynak derleme sirasinda mevcut davranis korunuyor: `.urc` de yine `compiled/` altina yaziliyor
- smoke fixture eklendi: `tests/fixtures/aot_demo/hello.ur`
- dogrulama: `hello.ur -> hello.exe`, sonra `hello.exe foo bar` ciktilari `aot-ok` ve `args=2`

## Latest Cross-Platform Foundation Upgrade

- `CMakeLists.txt` artik `resources.rc` ve Windows linklerini kosullu bagliyor
- build now links `Threads::Threads` genel olarak, `CURL::libcurl` uygun Unix/macOS buildlerinde, `OpenSSL::Crypto` uygun Linux buildlerinde
- `net_native.cpp` artik Winsock'a ek olarak POSIX/BSD socket katmani da iceriyor
- `http_native.cpp` Windows'ta WinHTTP, diger uygun buildlerde `libcurl` kullanabiliyor
- `crypto_native.cpp` Windows'ta CryptoAPI, macOS'ta CommonCrypto, Linux'ta OpenSSL tabanli SHA-256/AES yollarina sahip
- portability regresyonu eklendi: `tests/crypto_portability_test.ur`
- su an Windows dogrulama sonucu: `5 passed, 0 failed`
- hala Windows-agir kalan katmanlar acik: native GUI, Godot native integration ve native machine-code JIT backend

## Latest Runtime Capability Surface Upgrade

- `runtimeCapabilities()` native API eklendi; `urlib/process` bunu `process.features()` ve `process.supports(name)` olarak aciyor
- `urlib/env` ve `urlib/std` uzerinde de ayni capability bilgisi tek yerden alinabiliyor: `std.runtimeFeatures()`
- capability haritasi artik `platform`, `gui`, `http`, `crypto`, `sqlite`, `threads`, `workerPool`, `asyncScheduler`, `nativeJit`, `gcYoungGeneration`, `gcMoving`, `gcConcurrent`, `typeCheckerStatic`, `genericSpecialization`, `aotCompile` gibi alanlari acikca bildiriyor
- boylece cross-platform davranis gizli varsayim yerine runtime capability kontrolu ile yazilabilir
- yeni regresyon: `tests/runtime_capabilities_test.ur`

## Latest Structured Concurrency Upgrade

- `urlib/async` uzerinde cancellation token, `throwIfCancelled` ve `checkpoint` yardimcilari eklendi
- `urlib/thread` uzerinde `broadcast`, `drain`, `receiveWithTimeout` ve `select` yardimcilari eklendi
- bu katman channel ve worker akislari icin daha kullanisli bir structured-concurrency yuzeyi veriyor
- regresyon: `tests/structured_concurrency_test.ur`

## Latest Parser / Import Rewrite / Bolt Upgrade

- parser artik stray `}` token'inda ayni hatayi sonsuz tekrar etmiyor; `src/compiler.cpp` icinde token ilerletilip tek seferlik anlamli hata veriliyor
- dogrulama: `tmp_bad_brace.ur` artik tek satir hata ile duruyor: `Unexpected '}' without a matching block.`
- import rewriter bug'i duzeltildi: property access uyeleri (`app.surface`, `theme.on` gibi) namespace-rewrite'e artik girmiyor
- bu duzeltme `src/source_loader.cpp` icinde yapildi ve harici modul property zincirlerinde `nil` bozulmasini kapatti
- Bolt harici kutuphanesinin `setTheme()` akisi artik stabil; package smoke testi gecti
- Bolt package yolu: `bolt/index.ur`, smoke testi: `bolt/tests/smoke_test.ur`
- guncel dogrulama: Bolt package `1 passed, 0 failed`, ana test paketi `8 passed, 0 failed`

## Latest High-Level Networking Upgrade

- `urlib/net/index.ur` eklendi; TCP socket helper'lari standard kutuphane seviyesine tasindi
- `urlib/httpserver/index.ur` eklendi; request parse, response builder, router ve `serve/serveApp` yuzeyi geldi
- `urlib/thread/index.ur` artik `spawn(path)` ve `join(id)` sarmallarini aciyor
- loopback fixture eklendi: `tests/fixtures/http_server_fixture/server_once.ur`
- regresyon eklendi: `tests/http_server_test.ur`
- ornek API sunucusu eklendi: `APIs/api.ur`

## Latest LSP / Tooling Upgrade

- `src/tooling.cpp` icindeki LSP server artik sembol indeksli calisiyor
- yeni LSP capability'leri: `textDocument/definition`, `textDocument/hover`, `textDocument/completion`, `textDocument/documentSymbol`
- completion artik local semboller, import alias'lari ve `module.member` erisimlerinde hedef modullerin exportlarini listeliyor
- definition artik local declaration'lara ve `from ... import ...` ile module-alias member referanslarina gidebiliyor
- hover artik declaration detaylarini donduruyor
- smoke fixture eklendi: `tests/fixtures/lsp_demo_symbols.ur`
- stdio LSP smoke dogrulama sonucu: `lsp-rich-features-ok`

This file is intended to be the running project memory for Uranium.
Future major milestones should be appended here as the language evolves.
