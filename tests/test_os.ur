@vm AUTO_MAIN false

@os WIN32
    print("This is Windows!\n")
@os MACOS
    print("This is macOS!\n")
@os LINUX
    print("This is Linux!\n")
    // This should be ignored on Windows
    let x = ;
@os END
