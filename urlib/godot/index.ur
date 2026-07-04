import process as process

const MODE_EDITOR = "editor"
const MODE_PROJECT = "project"
const MODE_SCENE = "scene"
const LANG_GDSCRIPT = "gdscript"
const LANG_CSHARP = "csharp"

fn findEditor(hint) {
return godotFindEditor(hint)
}

fn createProject(rootPath, spec) {
return godotCreateProject(rootPath, spec)
}

fn createScene(scenePath, spec) {
return godotCreateScene(scenePath, spec)
}

fn createScript(scriptPath, spec) {
return godotCreateScript(scriptPath, spec)
}

fn createPlugin(projectRoot, spec) {
return godotCreatePlugin(projectRoot, spec)
}

fn createGDExtension(projectRoot, spec) {
return godotCreateGDExtension(projectRoot, spec)
}

fn command(projectRoot, spec) {
return godotBuildCommand(projectRoot, spec)
}

fn editorCommand(projectRoot, executable) {
return command(projectRoot, [
"mode": MODE_EDITOR,
"executable": executable
])
}

fn runCommand(projectRoot, executable) {
return command(projectRoot, [
"mode": MODE_PROJECT,
"executable": executable
])
}

fn sceneCommand(projectRoot, scenePath, executable) {
return command(projectRoot, [
"mode": MODE_SCENE,
"scene": scenePath,
"executable": executable
])
}

fn edit(projectRoot, executable) {
return process.run(editorCommand(projectRoot, executable))
}

fn run(projectRoot, executable) {
return process.run(runCommand(projectRoot, executable))
}

fn runScene(projectRoot, scenePath, executable) {
return process.run(sceneCommand(projectRoot, scenePath, executable))
}

fn quickStart(rootPath, projectName) {
let project = createProject(rootPath, [
"name": projectName,
"mainScene": "res://scenes/main.tscn"
])

let script = createScript(project.scriptsDir + "/main.gd", [
"language": LANG_GDSCRIPT,
"extends": "Node2D",
"className": "MainController",
"body": [
"func _ready():",
"    pass"
]
])

let scene = createScene(project.scenesDir + "/main.tscn", [
"rootType": "Node2D",
"rootName": "Main",
"script": script.path,
"children": [
[
"name": "Camera2D",
"type": "Camera2D",
"parent": ".",
"properties": [
"enabled": true
]
]
]
])

return [
"project": project,
"script": script,
"scene": scene
]
}
