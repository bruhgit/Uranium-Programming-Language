@vm AUTO_MAIN false
@vm PLATFORM_ERRORS true

@os WIN32
    print("Executing Windows!\n")
@os MACOS
    // Syntax error!
    let a = ;
@os END
