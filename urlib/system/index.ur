import process

fn cmd(command) {
    let result = process.run(command)
    if (result.output != "") {
        print(result.output)
    }
    return result
}
