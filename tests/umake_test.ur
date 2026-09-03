import assert as assert
import fs as fs
import path as path
import process as process

class main() {
    let repoRoot = path.parent(path.parent(path.parent(process.executable())))
    let fixtureRoot = path.join4(repoRoot, "tests", "fixtures", "umake_demo")
    let outputFile = path.join3(fixtureRoot, "out", "result.txt")
    if (fs.exists(outputFile)) {
        fs.remove(outputFile)
    }

    let uranium = process.executable()

    let listResult = process.run(uranium + " --make-list " + fixtureRoot)
    assert.ok(listResult.ok, "umake list should succeed")
    assert.ok(contains(listResult.output, "default"), "umake list should expose the default target")
    assert.ok(contains(listResult.output, "build"), "umake list should expose the build target")
    assert.ok(contains(listResult.output, "SCRIPT=write_marker.ur"), "umake list should expose parsed variables")

    let runResult = process.run(uranium + " --make-file " + fixtureRoot)
    assert.ok(runResult.ok, "umake default target should run successfully")
    assert.ok(fs.exists(outputFile), "umake should create the expected output file")
    assert.equal(fs.readText(outputFile), "umake-ok\n", "umake should run dependent Uranium tasks")

    print("umake-ok")
}
