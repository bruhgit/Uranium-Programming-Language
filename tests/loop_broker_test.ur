import assert as assert
import path as path
import process as process

fn repoRoot() {
    return path.parent(path.parent(path.parent(process.executable())))
}

class main() {
    let root = repoRoot()
    let fixturesRoot = path.join4(root, "tests", "fixtures", "loop_broker")
    let script = path.join(fixturesRoot, "infinite.ur")
    let uranium = process.executable()
    let result = process.run(uranium + " " + script)

    assert.ok(!result.ok, "loop broker should stop runaway scripts")
    assert.ok(contains(result.output, "Loop broker"), "loop broker failure should be visible in command output")
    print("loop-broker-ok")
}
