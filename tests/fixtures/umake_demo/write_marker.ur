import fs as fs
import path as path
import process as process

class main() {
    let root = path.parent(process.entry())
    let outputDir = path.join(root, "out")
    fs.createDirs(outputDir)
    fs.writeText(path.join(outputDir, "result.txt"), "umake-ok\n")
}
