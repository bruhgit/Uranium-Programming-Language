// UCPAPI (Uranium C Programming API) Integration Test
import ucpapi

class main() {
    print("=== UCPAPI (Uranium C Programming API) Test ===")
    
    // 1. Load the compiled C library
    let libPath = "build/Debug/test_lib.dll"
    print("Loading C library from: " + libPath)
    let lib = ucpapi.load(libPath)
    
    // 2. Create C-compatible variables
    let c_a = ucpapi.int("a", 15)
    let c_b = ucpapi.int("b", 27)
    let c_msg = ucpapi.string("msg", "Hello from Uranium to C!")
    
    let c_dbl1 = ucpapi.double("x", 5.5)
    let c_dbl2 = ucpapi.double("y", 4.0)

    // 3. Run print_message(const char* msg)
    print("\n--- Testing print_message ---")
    let retCode = lib.run("print_message", [c_msg])
    print("print_message returned: " + str(retCode)) // Should be 42

    // 4. Run add_numbers(int a, int b)
    print("\n--- Testing add_numbers ---")
    let sum = lib.run("add_numbers", [c_a, c_b])
    print("add_numbers returned: " + str(sum)) // Should be 42 (15 + 27)

    // 5. Run multiply_doubles(double a, double b)
    print("\n--- Testing multiply_doubles ---")
    let prod = lib.run("multiply_doubles", [c_dbl1, c_dbl2])
    print("multiply_doubles returned: " + str(prod)) // Should be 22 (5.5 * 4 = 22.0 -> cast to int)

    // 6. Unload library
    print("\nClosing library...")
    let closed = lib.close()
    print("Library closed: " + str(closed))
    
    print("\nUCPAPI Integration Test Completed Successfully!")
}
