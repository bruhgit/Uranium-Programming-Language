import assert as assert
import process as process
import std as std

class main() {
    let features = process.features()

    assert.equal(features.platform, process.platform(),
                 "capability platform should match process platform")
    assert.ok(hasKey(features, "gui"), "runtime capabilities should expose gui support")
    assert.ok(hasKey(features, "http"), "runtime capabilities should expose http support")
    assert.ok(hasKey(features, "cryptoSha256"),
              "runtime capabilities should expose crypto hash support")
    assert.ok(hasKey(features, "nativeJit"),
              "runtime capabilities should expose JIT support")
    assert.ok(hasKey(features, "sqlite"),
              "runtime capabilities should expose sqlite support")
    assert.ok(hasKey(features, "threads"),
              "runtime capabilities should expose thread support")

    assert.ok(features.packageManager, "package manager capability should be available")
    assert.ok(features.formatter, "formatter capability should be available")
    assert.ok(features.linter, "linter capability should be available")
    assert.ok(features.debugger, "debugger capability should be available")
    assert.ok(features.lsp, "LSP capability should be available")
    assert.ok(features.asyncScheduler, "async scheduler should be available")
    assert.ok(features.workerPool, "worker pool should be available")
    assert.ok(features.sqlite, "sqlite support should be available")
    assert.ok(features.typeCheckerStatic, "static type checker flag should be visible")
    assert.ok(features.genericSpecialization,
              "generic specialization flag should be visible")
    assert.ok(features.gcYoungGeneration, "young generation GC flag should be visible")
    assert.ok(features.gcMoving == false, "moving GC is not implemented yet")
    assert.ok(features.gcConcurrent == false, "concurrent GC is not implemented yet")

    assert.equal(process.feature("platform"), features.platform,
                 "single feature lookup should match platform entry")
    assert.equal(process.supports("threads"), features.threads,
                 "supports() should mirror boolean capability values")
    assert.equal(std.runtimeFeatures().platform, features.platform,
                 "std runtime feature view should match process view")
    assert.equal(std.processFeatures().platform, features.platform,
                 "std process feature view should match process view")
    assert.equal(std.envFeatures().platform, features.platform,
                 "std env feature view should match process view")
    assert.equal(std.runtimeSupports("threads"), true,
                 "std runtime supports should surface boolean capabilities")

    if (features.platform == "windows") {
        assert.ok(features.http, "windows builds should expose HTTP support")
        assert.equal(features.httpBackend, "winhttp",
                     "windows HTTP backend should be winhttp")
        assert.ok(features.cryptoSha256, "windows builds should expose SHA-256 support")
        assert.ok(features.cryptoAes, "windows builds should expose AES support")
        assert.equal(features.cryptoBackend, "cryptoapi",
                     "windows crypto backend should be cryptoapi")
        assert.ok(features.gui, "windows builds should expose GUI support")
        assert.equal(features.guiBackend, "win32",
                     "windows GUI backend should be win32")
        assert.ok(features.aotCompile, "windows builds should expose AOT packaging")
    } else {
        assert.equal(features.gui, false,
                     "non-windows builds should report GUI stubs today")
        assert.equal(features.guiBackend, "stub",
                     "non-windows builds should report GUI stub backend today")
    }

    if (features.nativeJit) {
        assert.equal(features.jitBackend, "native",
                     "native JIT capable builds should report native backend")
    } else {
        assert.equal(features.jitBackend, "fastpath",
                     "fallback builds should report fastpath JIT backend")
    }

    print("runtime-capabilities-ok")
}
