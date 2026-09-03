import assert as assert
import "../bolt/index.ur" as "bolt"

class main() {
    let surface = bolt.surface(6, 4, 0)
    bolt.paintButton(surface, 1, 1, 4, 2, false)
    assert.equal(bolt.getPixel(surface, 1, 1), 1, "bolt button frame should draw border")

    let app = bolt.createHeadlessApp("test", 6, 4)
    bolt.addLabel(app, "title", "Bolt", 0, 0, 10, 1)
    let widget = bolt.findWidgetById(app, "title")
    assert.equal(widget.text, "Bolt", "bolt widget registry should preserve label text")

    print("bolt-library-ok")
}
