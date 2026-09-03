import assert as assert
import "./fixtures/external_tools.ur" as "tools"

class main() {
    assert.equal(tools.double(21), 42, "quoted alias imports should work for external modules")
    assert.equal(tools.label(), "external-tools", "external module namespace should stay usable")
    print("external-import-ok")
}
